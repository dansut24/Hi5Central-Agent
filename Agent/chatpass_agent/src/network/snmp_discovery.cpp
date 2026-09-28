#include "network/snmp_discovery.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hi5::network {
namespace {

using json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;

struct SnmpValue {
    std::uint8_t tag = 0;
    std::string text;
    std::uint64_t number = 0;
    bool hasNumber = false;
};

void AppendLength(Bytes& out, std::size_t length) {
    if (length < 0x80) {
        out.push_back(static_cast<std::uint8_t>(length));
        return;
    }
    Bytes encoded;
    while (length > 0) {
        encoded.push_back(static_cast<std::uint8_t>(length & 0xff));
        length >>= 8;
    }
    out.push_back(static_cast<std::uint8_t>(0x80 | encoded.size()));
    for (auto it = encoded.rbegin(); it != encoded.rend(); ++it) out.push_back(*it);
}

Bytes Wrap(std::uint8_t tag, const Bytes& body) {
    Bytes out;
    out.reserve(body.size() + 6);
    out.push_back(tag);
    AppendLength(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void Append(Bytes& target, const Bytes& value) {
    target.insert(target.end(), value.begin(), value.end());
}

Bytes EncodeInteger(std::uint64_t value) {
    Bytes body;
    do {
        body.push_back(static_cast<std::uint8_t>(value & 0xff));
        value >>= 8;
    } while (value > 0);
    std::reverse(body.begin(), body.end());
    if (!body.empty() && (body.front() & 0x80) != 0) body.insert(body.begin(), 0);
    return Wrap(0x02, body);
}

Bytes EncodeOctetString(const std::string& value) {
    return Wrap(0x04, Bytes(value.begin(), value.end()));
}

bool ParseOidComponents(const std::string& oid, std::vector<std::uint64_t>& parts) {
    parts.clear();
    std::stringstream stream(oid);
    std::string token;
    while (std::getline(stream, token, '.')) {
        if (token.empty()) continue;
        try {
            const auto value = std::stoull(token);
            parts.push_back(value);
        } catch (...) {
            return false;
        }
    }
    return parts.size() >= 2 && parts[0] <= 2 && parts[1] <= 39;
}

Bytes EncodeOid(const std::string& oid) {
    std::vector<std::uint64_t> parts;
    if (!ParseOidComponents(oid, parts)) return {};
    Bytes body;
    body.push_back(static_cast<std::uint8_t>(parts[0] * 40 + parts[1]));
    for (std::size_t i = 2; i < parts.size(); ++i) {
        std::uint64_t value = parts[i];
        Bytes segment;
        segment.push_back(static_cast<std::uint8_t>(value & 0x7f));
        value >>= 7;
        while (value > 0) {
            segment.push_back(static_cast<std::uint8_t>(0x80 | (value & 0x7f)));
            value >>= 7;
        }
        for (auto it = segment.rbegin(); it != segment.rend(); ++it) body.push_back(*it);
    }
    return Wrap(0x06, body);
}

Bytes BuildGetRequest(int version, const std::string& community, std::uint32_t requestId,
                      const std::vector<std::string>& oids) {
    Bytes varBindList;
    for (const auto& oid : oids) {
        Bytes varBind;
        Append(varBind, EncodeOid(oid));
        Append(varBind, Wrap(0x05, {}));
        Append(varBindList, Wrap(0x30, varBind));
    }

    Bytes pduBody;
    Append(pduBody, EncodeInteger(requestId));
    Append(pduBody, EncodeInteger(0));
    Append(pduBody, EncodeInteger(0));
    Append(pduBody, Wrap(0x30, varBindList));

    Bytes message;
    Append(message, EncodeInteger(static_cast<std::uint64_t>(version)));
    Append(message, EncodeOctetString(community));
    Append(message, Wrap(0xA0, pduBody));
    return Wrap(0x30, message);
}

class BerReader {
public:
    BerReader(const std::uint8_t* data, std::size_t length) : data_(data), length_(length) {}

    bool Read(std::uint8_t& tag, const std::uint8_t*& content, std::size_t& contentLength) {
        if (position_ >= length_) return false;
        tag = data_[position_++];
        if (position_ >= length_) return false;

        std::uint8_t lengthByte = data_[position_++];
        std::size_t valueLength = 0;
        if ((lengthByte & 0x80) == 0) {
            valueLength = lengthByte;
        } else {
            const std::size_t count = lengthByte & 0x7f;
            if (count == 0 || count > sizeof(std::size_t) || position_ + count > length_) return false;
            for (std::size_t i = 0; i < count; ++i) {
                valueLength = (valueLength << 8) | data_[position_++];
            }
        }

        if (position_ + valueLength > length_) return false;
        content = data_ + position_;
        contentLength = valueLength;
        position_ += valueLength;
        return true;
    }

    bool Empty() const { return position_ >= length_; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t length_ = 0;
    std::size_t position_ = 0;
};

std::uint64_t DecodeUnsigned(const std::uint8_t* data, std::size_t length) {
    std::uint64_t value = 0;
    if (length > 0 && data[0] == 0 && length > 1) {
        ++data;
        --length;
    }
    const std::size_t start = length > 8 ? length - 8 : 0;
    for (std::size_t i = start; i < length; ++i) value = (value << 8) | data[i];
    return value;
}

std::string DecodeOid(const std::uint8_t* data, std::size_t length) {
    if (!data || length == 0) return {};
    std::vector<std::uint64_t> parts;
    const std::uint8_t first = data[0];
    parts.push_back(std::min<std::uint64_t>(2, first / 40));
    parts.push_back(first - (parts[0] * 40));

    std::uint64_t value = 0;
    for (std::size_t i = 1; i < length; ++i) {
        value = (value << 7) | (data[i] & 0x7f);
        if ((data[i] & 0x80) == 0) {
            parts.push_back(value);
            value = 0;
        }
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out << '.';
        out << parts[i];
    }
    return out.str();
}

std::string DecodeText(const std::uint8_t* data, std::size_t length) {
    std::string value(reinterpret_cast<const char*>(data), length);
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0 || (c < 0x20 && c != '\t' && c != '\r' && c != '\n')) ch = ' ';
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

bool ReadInteger(BerReader& reader, std::uint64_t& value) {
    std::uint8_t tag = 0;
    const std::uint8_t* content = nullptr;
    std::size_t length = 0;
    if (!reader.Read(tag, content, length) || tag != 0x02) return false;
    value = DecodeUnsigned(content, length);
    return true;
}

bool ParseResponse(const std::uint8_t* packet, std::size_t packetLength, std::uint32_t requestId,
                   std::unordered_map<std::string, SnmpValue>& values, std::string& error) {
    values.clear();
    BerReader outer(packet, packetLength);
    std::uint8_t tag = 0;
    const std::uint8_t* messageContent = nullptr;
    std::size_t messageLength = 0;
    if (!outer.Read(tag, messageContent, messageLength) || tag != 0x30) {
        error = "SNMP response did not contain a valid message sequence.";
        return false;
    }

    BerReader message(messageContent, messageLength);
    std::uint64_t version = 0;
    if (!ReadInteger(message, version)) {
        error = "SNMP response version was invalid.";
        return false;
    }

    const std::uint8_t* communityContent = nullptr;
    std::size_t communityLength = 0;
    if (!message.Read(tag, communityContent, communityLength) || tag != 0x04) {
        error = "SNMP response community was invalid.";
        return false;
    }

    const std::uint8_t* pduContent = nullptr;
    std::size_t pduLength = 0;
    if (!message.Read(tag, pduContent, pduLength) || tag != 0xA2) {
        error = "SNMP response did not contain a GetResponse PDU.";
        return false;
    }

    BerReader pdu(pduContent, pduLength);
    std::uint64_t responseId = 0;
    std::uint64_t errorStatus = 0;
    std::uint64_t errorIndex = 0;
    if (!ReadInteger(pdu, responseId) || !ReadInteger(pdu, errorStatus) || !ReadInteger(pdu, errorIndex)) {
        error = "SNMP response PDU header was invalid.";
        return false;
    }
    if (static_cast<std::uint32_t>(responseId) != requestId) {
        error = "SNMP response request id did not match.";
        return false;
    }
    if (errorStatus != 0) {
        error = "SNMP agent returned error status " + std::to_string(errorStatus) +
                " at index " + std::to_string(errorIndex) + ".";
        return false;
    }

    const std::uint8_t* listContent = nullptr;
    std::size_t listLength = 0;
    if (!pdu.Read(tag, listContent, listLength) || tag != 0x30) {
        error = "SNMP response varbind list was invalid.";
        return false;
    }

    BerReader list(listContent, listLength);
    while (!list.Empty()) {
        const std::uint8_t* varBindContent = nullptr;
        std::size_t varBindLength = 0;
        if (!list.Read(tag, varBindContent, varBindLength) || tag != 0x30) break;

        BerReader varBind(varBindContent, varBindLength);
        const std::uint8_t* oidContent = nullptr;
        std::size_t oidLength = 0;
        if (!varBind.Read(tag, oidContent, oidLength) || tag != 0x06) continue;
        const std::string oid = DecodeOid(oidContent, oidLength);
        if (oid.empty()) continue;

        const std::uint8_t* valueContent = nullptr;
        std::size_t valueLength = 0;
        std::uint8_t valueTag = 0;
        if (!varBind.Read(valueTag, valueContent, valueLength)) continue;

        SnmpValue value;
        value.tag = valueTag;
        if (valueTag == 0x04) {
            value.text = DecodeText(valueContent, valueLength);
        } else if (valueTag == 0x06) {
            value.text = DecodeOid(valueContent, valueLength);
        } else if (valueTag == 0x02 || valueTag == 0x41 || valueTag == 0x42 ||
                   valueTag == 0x43 || valueTag == 0x46) {
            value.number = DecodeUnsigned(valueContent, valueLength);
            value.hasNumber = true;
            value.text = std::to_string(value.number);
        } else if (valueTag == 0x40 && valueLength == 4) {
            value.text = std::to_string(valueContent[0]) + "." +
                         std::to_string(valueContent[1]) + "." +
                         std::to_string(valueContent[2]) + "." +
                         std::to_string(valueContent[3]);
        }
        values[oid] = std::move(value);
    }
    return true;
}

bool EnsureWinsock(std::string& error) {
#ifdef _WIN32
    static std::once_flag once;
    static int startupResult = WSASYSNOTREADY;
    std::call_once(once, []() {
        WSADATA data{};
        startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startupResult != 0) {
        error = "WSAStartup failed: " + std::to_string(startupResult);
        return false;
    }
    return true;
#else
    error = "SNMP network discovery is not implemented for this platform yet.";
    return false;
#endif
}

bool ParseIpv4Cidr(const std::string& cidr, std::uint32_t& firstAddress, std::uint32_t& addressCount,
                   std::string& error) {
#ifdef _WIN32
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        error = "CIDR prefix is required.";
        return false;
    }
    const std::string addressText = cidr.substr(0, slash);
    int prefix = -1;
    try {
        prefix = std::stoi(cidr.substr(slash + 1));
    } catch (...) {
        error = "CIDR prefix is invalid.";
        return false;
    }
    if (prefix < 20 || prefix > 32) {
        error = "CIDR must be between /20 and /32.";
        return false;
    }

    IN_ADDR address{};
    if (InetPtonA(AF_INET, addressText.c_str(), &address) != 1) {
        error = "CIDR IPv4 address is invalid.";
        return false;
    }

    const std::uint32_t host = ntohl(address.S_un.S_addr);
    const std::uint8_t firstOctet = static_cast<std::uint8_t>((host >> 24) & 0xff);
    const std::uint8_t secondOctet = static_cast<std::uint8_t>((host >> 16) & 0xff);
    const bool privateAddress =
        firstOctet == 10
        || (firstOctet == 172 && secondOctet >= 16 && secondOctet <= 31)
        || (firstOctet == 192 && secondOctet == 168);
    if (!privateAddress) {
        error = "Network discovery is limited to private RFC1918 IPv4 ranges.";
        return false;
    }

    const std::uint32_t hostBits = static_cast<std::uint32_t>(32 - prefix);
    const std::uint32_t size = hostBits == 32 ? 0 : (1u << hostBits);
    const std::uint32_t mask = prefix == 32 ? 0xffffffffu : (0xffffffffu << hostBits);
    const std::uint32_t network = host & mask;

    if (prefix <= 30) {
        firstAddress = network + 1;
        addressCount = size >= 2 ? size - 2 : 0;
    } else {
        firstAddress = network;
        addressCount = size;
    }
    if (addressCount == 0 || addressCount > 4096) {
        error = "CIDR contains an unsupported number of host addresses.";
        return false;
    }
    return true;
#else
    (void)cidr;
    (void)firstAddress;
    (void)addressCount;
    error = "SNMP network discovery is not implemented for this platform yet.";
    return false;
#endif
}

std::string Ipv4ToString(std::uint32_t hostAddress) {
#ifdef _WIN32
    IN_ADDR address{};
    address.S_un.S_addr = htonl(hostAddress);
    char buffer[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &address, buffer, static_cast<DWORD>(sizeof(buffer)))) return {};
    return buffer;
#else
    return {};
#endif
}

std::string FormatMacAddress(const unsigned char* bytes, std::size_t length) {
    if (!bytes || length != 6) return {};

    bool allZero = true;
    bool allBroadcast = true;
    for (std::size_t i = 0; i < length; ++i) {
        allZero = allZero && bytes[i] == 0x00;
        allBroadcast = allBroadcast && bytes[i] == 0xff;
    }
    if (allZero || allBroadcast) return {};

    char buffer[4]{};
    std::string output;
    for (std::size_t i = 0; i < length; ++i) {
        std::snprintf(buffer, sizeof(buffer), "%02X", bytes[i]);
        if (!output.empty()) output.push_back(':');
        output += buffer;
    }
    return output;
}

std::unordered_map<std::uint32_t, std::string> SnapshotIpv4Neighbours() {
    std::unordered_map<std::uint32_t, std::string> neighbours;
#ifdef _WIN32
    ULONG size = 0;
    if (GetIpNetTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return neighbours;
    }
    std::vector<std::uint8_t> storage(size);
    auto* table = reinterpret_cast<MIB_IPNETTABLE*>(storage.data());
    if (GetIpNetTable(table, &size, FALSE) != NO_ERROR) return neighbours;

    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& row = table->table[index];
        if (row.dwType == MIB_IPNET_TYPE_INVALID) continue;
        if (row.dwPhysAddrLen == 0 || row.dwPhysAddrLen > sizeof(row.bPhysAddr)) continue;
        const std::string mac = FormatMacAddress(row.bPhysAddr, row.dwPhysAddrLen);
        if (mac.empty()) continue;
        neighbours[ntohl(row.dwAddr)] = mac;
    }
#endif
    return neighbours;
}

bool PingIpv4(const std::string& ipAddress, int timeoutMs, int& latencyMs) {
    latencyMs = -1;
#ifdef _WIN32
    const IPAddr destination = inet_addr(ipAddress.c_str());
    if (destination == INADDR_NONE) return false;

    HANDLE handle = IcmpCreateFile();
    if (handle == INVALID_HANDLE_VALUE) return false;

    static constexpr char kPayload[] = "hi5central";
    const DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(kPayload) + 32;
    std::vector<std::uint8_t> replyBuffer(replySize);
    const DWORD replies = IcmpSendEcho(
        handle,
        destination,
        const_cast<char*>(kPayload),
        static_cast<WORD>(sizeof(kPayload) - 1),
        nullptr,
        replyBuffer.data(),
        replySize,
        static_cast<DWORD>(timeoutMs));
    IcmpCloseHandle(handle);

    if (replies == 0) return false;
    const auto* reply = reinterpret_cast<const ICMP_ECHO_REPLY*>(replyBuffer.data());
    if (reply->Status != IP_SUCCESS) return false;
    latencyMs = static_cast<int>(reply->RoundTripTime);
    return true;
#else
    (void)ipAddress;
    (void)timeoutMs;
    return false;
#endif
}

std::string ReverseDnsName(const std::string& ipAddress) {
#ifdef _WIN32
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (InetPtonA(AF_INET, ipAddress.c_str(), &address.sin_addr) != 1) return {};

    char host[NI_MAXHOST]{};
    const int result = getnameinfo(
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address),
        host,
        static_cast<DWORD>(sizeof(host)),
        nullptr,
        0,
        NI_NAMEREQD);
    if (result != 0) return {};
    return DecodeText(reinterpret_cast<const std::uint8_t*>(host), std::strlen(host));
#else
    (void)ipAddress;
    return {};
#endif
}

std::string ResolveMacAddress(const std::string& ipAddress);

json ProbePresence(
    std::uint32_t hostAddress,
    int timeoutMs,
    const std::unordered_map<std::uint32_t, std::string>& neighbourSnapshot,
    bool& present) {
    present = false;
    const std::string ipAddress = Ipv4ToString(hostAddress);
    if (ipAddress.empty()) return json();

    std::string macAddress;
    const auto known = neighbourSnapshot.find(hostAddress);
    if (known != neighbourSnapshot.end()) macAddress = known->second;

    int latencyMs = -1;
    const bool icmpReachable = PingIpv4(ipAddress, timeoutMs, latencyMs);
    if (icmpReachable && macAddress.empty()) macAddress = ResolveMacAddress(ipAddress);

    present = icmpReachable || !macAddress.empty();
    if (!present) return json();

    json methods = json::array();
    if (!macAddress.empty()) methods.push_back("arp");
    if (icmpReachable) methods.push_back("icmp");

    json device = {
        {"ipAddress", ipAddress},
        {"macAddress", macAddress},
        {"hostname", ""},
        {"icmpReachable", icmpReachable},
        {"discoveryMethods", methods},
        {"interfaces", json::array()},
        {"metadata", {
            {"scanner", "hi5central-native-presence"},
            {"nameEnrichmentDeferred", true}
        }}
    };
    if (latencyMs >= 0) device["latencyMs"] = latencyMs;
    return device;
}

std::string ResolveMacAddress(const std::string& ipAddress) {
#ifdef _WIN32
    IPAddr destination = inet_addr(ipAddress.c_str());
    if (destination == INADDR_NONE) return {};
    ULONG macWords[2]{};
    ULONG macLength = 6;
    const DWORD result = SendARP(destination, 0, macWords, &macLength);
    if (result != NO_ERROR || macLength == 0 || macLength > 8) return {};

    const auto* bytes = reinterpret_cast<const unsigned char*>(macWords);
    return FormatMacAddress(bytes, macLength);
#else
    (void)ipAddress;
    return {};
#endif
}

bool QueryTarget(const std::string& ipAddress, int port, int timeoutMs, int retries,
                 int snmpVersion, const std::string& community, std::uint32_t requestId,
                 const std::vector<std::string>& oids,
                 std::unordered_map<std::string, SnmpValue>& values, std::string& error) {
#ifdef _WIN32
    const Bytes request = BuildGetRequest(snmpVersion, community, requestId, oids);
    if (request.empty()) {
        error = "Unable to build SNMP request.";
        return false;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<u_short>(port));
    if (InetPtonA(AF_INET, ipAddress.c_str(), &target.sin_addr) != 1) {
        error = "Target address is invalid.";
        return false;
    }

    for (int attempt = 0; attempt <= retries; ++attempt) {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            error = "Unable to create SNMP UDP socket.";
            return false;
        }

        DWORD timeout = static_cast<DWORD>(timeoutMs);
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        const int sent = sendto(
            sock,
            reinterpret_cast<const char*>(request.data()),
            static_cast<int>(request.size()),
            0,
            reinterpret_cast<const sockaddr*>(&target),
            sizeof(target));
        if (sent == SOCKET_ERROR) {
            closesocket(sock);
            if (attempt == retries) error = "SNMP request send failed.";
            continue;
        }

        std::uint8_t response[8192]{};
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        const int received = recvfrom(
            sock,
            reinterpret_cast<char*>(response),
            static_cast<int>(sizeof(response)),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        closesocket(sock);

        if (received <= 0) {
            if (attempt == retries) error = "timeout";
            continue;
        }

        char sourceIp[INET_ADDRSTRLEN]{};
        if (!InetNtopA(AF_INET, &source.sin_addr, sourceIp, static_cast<DWORD>(sizeof(sourceIp))) ||
            ipAddress != sourceIp) {
            if (attempt == retries) error = "SNMP response source did not match target.";
            continue;
        }

        std::string parseError;
        if (ParseResponse(response, static_cast<std::size_t>(received), requestId, values, parseError)) {
            error.clear();
            return true;
        }
        if (attempt == retries) error = parseError;
    }
    return false;
#else
    (void)ipAddress;
    (void)port;
    (void)timeoutMs;
    (void)retries;
    (void)snmpVersion;
    (void)community;
    (void)requestId;
    (void)oids;
    (void)values;
    error = "SNMP network discovery is not implemented for this platform yet.";
    return false;
#endif
}

const SnmpValue* FindValue(const std::unordered_map<std::string, SnmpValue>& values,
                           const std::string& oid) {
    const auto it = values.find(oid);
    return it == values.end() ? nullptr : &it->second;
}

std::string ValueText(const std::unordered_map<std::string, SnmpValue>& values,
                      const std::string& oid) {
    const auto* value = FindValue(values, oid);
    return value ? value->text : std::string();
}

std::uint64_t ValueNumber(const std::unordered_map<std::string, SnmpValue>& values,
                          const std::string& oid, bool& present) {
    const auto* value = FindValue(values, oid);
    present = value && value->hasNumber;
    return present ? value->number : 0;
}

json ProbeDevice(std::uint32_t hostAddress, int port, int timeoutMs, int retries,
                 int snmpVersion, const std::string& versionLabel, const std::string& community,
                 std::uint32_t requestId, bool& responded) {
    responded = false;
    const std::string ipAddress = Ipv4ToString(hostAddress);
    if (ipAddress.empty()) return json();

    static const std::vector<std::string> kOids = {
        "1.3.6.1.2.1.1.1.0",  // sysDescr
        "1.3.6.1.2.1.1.2.0",  // sysObjectID
        "1.3.6.1.2.1.1.3.0",  // sysUpTime
        "1.3.6.1.2.1.1.4.0",  // sysContact
        "1.3.6.1.2.1.1.5.0",  // sysName
        "1.3.6.1.2.1.1.6.0",  // sysLocation
        "1.3.6.1.2.1.2.1.0",  // ifNumber
    };

    std::unordered_map<std::string, SnmpValue> values;
    std::string queryError;
    if (!QueryTarget(ipAddress, port, timeoutMs, retries, snmpVersion, community,
                     requestId, kOids, values, queryError)) {
        return json();
    }
    responded = true;

    bool hasUptime = false;
    bool hasInterfaceCount = false;
    const auto uptime = ValueNumber(values, "1.3.6.1.2.1.1.3.0", hasUptime);
    const auto interfaceCount = ValueNumber(values, "1.3.6.1.2.1.2.1.0", hasInterfaceCount);
    const std::string sysName = ValueText(values, "1.3.6.1.2.1.1.5.0");

    json device = {
        {"ipAddress", ipAddress},
        {"macAddress", ResolveMacAddress(ipAddress)},
        {"hostname", sysName},
        {"snmpVersion", versionLabel},
        {"sysName", sysName},
        {"sysDescr", ValueText(values, "1.3.6.1.2.1.1.1.0")},
        {"sysObjectId", ValueText(values, "1.3.6.1.2.1.1.2.0")},
        {"sysContact", ValueText(values, "1.3.6.1.2.1.1.4.0")},
        {"sysLocation", ValueText(values, "1.3.6.1.2.1.1.6.0")},
        {"interfaces", json::array()},
        {"metadata", {
            {"scanner", "hi5central-native-snmp"},
            {"standardOids", static_cast<int>(kOids.size())}
        }}
    };
    if (hasUptime) device["uptimeTicks"] = uptime;
    if (hasInterfaceCount) device["interfaceCount"] = interfaceCount;
    return device;
}

}  // namespace

nlohmann::json RunSnmpDiscovery(const nlohmann::json& payload, std::string& error) {
    error.clear();
    const auto scanStartedAt = std::chrono::steady_clock::now();

    std::string winsockError;
    if (!EnsureWinsock(winsockError)) {
        error = winsockError;
        return json{{"status", "failed"}, {"error", error}};
    }

    const std::string cidr = payload.value("cidr", std::string());
    const bool presenceEnabled = payload.value("presenceEnabled", false);
    const bool snmpEnabled = payload.value("snmpEnabled", payload.contains("credential"));
    if (!presenceEnabled && !snmpEnabled) {
        error = "No network discovery methods are enabled.";
        return json{{"status", "failed"}, {"error", error}};
    }

    const int presenceTimeoutMs =
        std::max(50, std::min(5000, payload.value("presenceTimeoutMs", 350)));
    const int port = std::max(1, std::min(65535, payload.value("snmpPort", 161)));
    const int timeoutMs = std::max(100, std::min(10000, payload.value("timeoutMs", 800)));
    const int retries = std::max(0, std::min(5, payload.value("retries", 1)));
    const int concurrency = std::max(1, std::min(128, payload.value("concurrency", 32)));

    std::string versionLabel;
    std::string community;
    int snmpVersion = -1;
    if (snmpEnabled) {
        versionLabel = payload.value("snmpVersion", std::string("v2c"));
        const json credential = payload.value("credential", json::object());
        community = credential.value("community", std::string());

        if (versionLabel == "v1") snmpVersion = 0;
        else if (versionLabel == "v2c") snmpVersion = 1;
        else {
            error = "Only SNMPv1 and SNMPv2c are supported by this Agent build.";
            return json{{"status", "failed"}, {"error", error}};
        }
        if (community.empty() || community.size() > 2048) {
            error = "SNMP community is missing or invalid.";
            return json{{"status", "failed"}, {"error", error}};
        }
    }

    std::uint32_t firstAddress = 0;
    std::uint32_t addressCount = 0;
    if (!ParseIpv4Cidr(cidr, firstAddress, addressCount, error)) {
        return json{{"status", "failed"}, {"error", error}};
    }

    const auto initialNeighbours =
        presenceEnabled ? SnapshotIpv4Neighbours()
                        : std::unordered_map<std::uint32_t, std::string>{};

    std::atomic<std::uint32_t> nextIndex{0};
    std::atomic<std::uint32_t> presenceCount{0};
    std::atomic<std::uint32_t> snmpCount{0};
    std::mutex devicesMutex;
    std::vector<std::pair<std::uint32_t, json>> devices;
    devices.reserve(std::min<std::uint32_t>(addressCount, 512));

    const auto mergeSnmp = [](json& device, json snmpDevice) {
        if (device.is_null() || device.empty()) {
            device = std::move(snmpDevice);
        } else {
            static const std::vector<std::string> kSnmpFields = {
                "snmpVersion", "sysName", "sysDescr", "sysObjectId", "sysContact",
                "sysLocation", "uptimeTicks", "interfaceCount", "interfaces"
            };
            for (const auto& field : kSnmpFields) {
                if (snmpDevice.contains(field) && !snmpDevice[field].is_null()) {
                    device[field] = snmpDevice[field];
                }
            }
            const std::string snmpHostname = snmpDevice.value("hostname", std::string());
            if (!snmpHostname.empty()) device["hostname"] = snmpHostname;
            const std::string currentMac = device.value("macAddress", std::string());
            const std::string snmpMac = snmpDevice.value("macAddress", std::string());
            if (currentMac.empty() && !snmpMac.empty()) device["macAddress"] = snmpMac;
        }

        json methods = device.value("discoveryMethods", json::array());
        bool hasSnmp = false;
        if (methods.is_array()) {
            for (const auto& method : methods) {
                if (method.is_string() && method.get<std::string>() == "snmp") {
                    hasSnmp = true;
                    break;
                }
            }
        } else {
            methods = json::array();
        }
        if (!hasSnmp) methods.push_back("snmp");
        device["discoveryMethods"] = std::move(methods);

        json metadata = device.value("metadata", json::object());
        if (!metadata.is_object()) metadata = json::object();
        metadata["scanner"] = "hi5central-native-network-discovery";
        metadata["snmpEnriched"] = true;
        device["metadata"] = std::move(metadata);
    };

    const std::uint32_t workerCount =
        std::max<std::uint32_t>(1, std::min<std::uint32_t>(
            static_cast<std::uint32_t>(concurrency), addressCount));
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (std::uint32_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker]() {
            for (;;) {
                const std::uint32_t index = nextIndex.fetch_add(1);
                if (index >= addressCount) break;

                const std::uint32_t hostAddress = firstAddress + index;
                json device;
                bool presenceResponded = false;
                bool snmpResponded = false;

                if (presenceEnabled) {
                    device = ProbePresence(
                        hostAddress,
                        presenceTimeoutMs,
                        initialNeighbours,
                        presenceResponded);
                    if (presenceResponded) presenceCount.fetch_add(1);
                }

                if (snmpEnabled) {
                    const std::uint32_t requestId =
                        static_cast<std::uint32_t>(
                            (GetTickCount64() + hostAddress + (worker * 7919u)) & 0x7fffffffu);
                    json snmpDevice = ProbeDevice(
                        hostAddress,
                        port,
                        timeoutMs,
                        retries,
                        snmpVersion,
                        versionLabel,
                        community,
                        requestId,
                        snmpResponded);
                    if (snmpResponded && !snmpDevice.is_null() && !snmpDevice.empty()) {
                        snmpCount.fetch_add(1);
                        mergeSnmp(device, std::move(snmpDevice));
                    }
                }

                if (device.is_null() || device.empty()) continue;
                std::lock_guard<std::mutex> lock(devicesMutex);
                devices.emplace_back(hostAddress, std::move(device));
            }
        });
    }
    for (auto& worker : workers) worker.join();

    // An ICMP request to a local-L2 device can populate the Windows neighbour table
    // even when that device blocks echo replies. Capture that table after the active
    // probe so those hosts remain visible as ARP-discovered devices.
    if (presenceEnabled) {
        const auto finalNeighbours = SnapshotIpv4Neighbours();
        std::unordered_map<std::uint32_t, bool> seen;
        for (const auto& item : devices) seen[item.first] = true;

        const std::uint64_t rangeEnd =
            static_cast<std::uint64_t>(firstAddress) + static_cast<std::uint64_t>(addressCount);
        for (const auto& [hostAddress, macAddress] : finalNeighbours) {
            if (hostAddress < firstAddress || static_cast<std::uint64_t>(hostAddress) >= rangeEnd) {
                continue;
            }
            if (seen.find(hostAddress) != seen.end() || macAddress.empty()) continue;

            const std::string ipAddress = Ipv4ToString(hostAddress);
            if (ipAddress.empty()) continue;

            json methods = json::array({"arp"});
            json device = {
                {"ipAddress", ipAddress},
                {"macAddress", macAddress},
                {"hostname", ""},
                {"icmpReachable", false},
                {"discoveryMethods", methods},
                {"interfaces", json::array()},
                {"metadata", {
                    {"scanner", "hi5central-native-presence"},
                    {"arpAfterProbe", true},
                    {"nameEnrichmentDeferred", true}
                }}
            };
            devices.emplace_back(hostAddress, std::move(device));
            presenceCount.fetch_add(1);
        }
    }

    std::sort(devices.begin(), devices.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });

    json outputDevices = json::array();
    for (auto& item : devices) outputDevices.push_back(std::move(item.second));

    const auto scanDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStartedAt).count();

    return json{
        {"status", "ok"},
        {"protocolVersion", 2},
        {"cidr", cidr},
        {"presenceEnabled", presenceEnabled},
        {"snmpEnabled", snmpEnabled},
        {"snmpVersion", snmpEnabled ? versionLabel : std::string()},
        {"addressesTotal", addressCount},
        {"addressesResponded", outputDevices.size()},
        {"presenceDevices", presenceCount.load()},
        {"snmpEnrichedDevices", snmpCount.load()},
        {"scanDurationMs", scanDurationMs},
        {"nameEnrichmentDeferred", true},
        {"devices", std::move(outputDevices)}
    };
}

}  // namespace hi5::network
