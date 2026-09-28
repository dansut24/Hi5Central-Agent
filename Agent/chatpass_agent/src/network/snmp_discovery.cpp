#include "network/snmp_discovery.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <winhttp.h>
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
#include <unordered_set>
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


std::string TrimAscii(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string HeaderValue(const std::string& response, const std::string& wantedKey) {
    const std::string wanted = LowerAscii(wantedKey);
    std::istringstream stream(response);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = LowerAscii(TrimAscii(line.substr(0, colon)));
        if (key == wanted) return TrimAscii(line.substr(colon + 1));
    }
    return {};
}

bool IsPrivateIpv4Address(const std::string& ipAddress) {
    IN_ADDR address{};
    if (InetPtonA(AF_INET, ipAddress.c_str(), &address) != 1) return false;
    const std::uint32_t host = ntohl(address.S_un.S_addr);
    const std::uint8_t first = static_cast<std::uint8_t>((host >> 24) & 0xff);
    const std::uint8_t second = static_cast<std::uint8_t>((host >> 16) & 0xff);
    return first == 10
        || (first == 172 && second >= 16 && second <= 31)
        || (first == 192 && second == 168);
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            count) != count) {
        return {};
    }
    return output;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            count,
            nullptr,
            nullptr) != count) {
        return {};
    }
    return output;
}

struct SsdpRecord {
    std::string ipAddress;
    std::string location;
    std::string server;
    std::string searchTarget;
    std::string usn;
};

std::unordered_map<std::string, SsdpRecord> DiscoverSsdp(
    const std::unordered_set<std::string>& targets) {
    std::unordered_map<std::string, SsdpRecord> records;
#ifdef _WIN32
    if (targets.empty()) return records;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return records;

    DWORD timeoutMs = 250;
    setsockopt(
        sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMs),
        sizeof(timeoutMs));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        closesocket(sock);
        return records;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(1900);
    InetPtonA(AF_INET, "239.255.255.250", &destination.sin_addr);

    static constexpr char kSearch[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";
    sendto(
        sock,
        kSearch,
        static_cast<int>(sizeof(kSearch) - 1),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
    std::vector<char> buffer(64 * 1024);

    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        const int received = recvfrom(
            sock,
            buffer.data(),
            static_cast<int>(buffer.size() - 1),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        if (received <= 0) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;
            break;
        }

        char ipBuffer[INET_ADDRSTRLEN]{};
        if (!InetNtopA(AF_INET, &source.sin_addr, ipBuffer, sizeof(ipBuffer))) continue;
        const std::string ipAddress = ipBuffer;
        if (targets.find(ipAddress) == targets.end()) continue;

        const std::string response(buffer.data(), static_cast<std::size_t>(received));
        const std::string statusLine = LowerAscii(response.substr(0, std::min<std::size_t>(64, response.size())));
        if (statusLine.find("200 ok") == std::string::npos) continue;

        SsdpRecord candidate;
        candidate.ipAddress = ipAddress;
        candidate.location = HeaderValue(response, "location");
        candidate.server = HeaderValue(response, "server");
        candidate.searchTarget = HeaderValue(response, "st");
        candidate.usn = HeaderValue(response, "usn");

        auto it = records.find(ipAddress);
        if (it == records.end()) {
            records.emplace(ipAddress, std::move(candidate));
        } else {
            if (it->second.location.empty() && !candidate.location.empty()) it->second.location = candidate.location;
            if (it->second.server.empty() && !candidate.server.empty()) it->second.server = candidate.server;
            if (it->second.searchTarget.empty() && !candidate.searchTarget.empty()) it->second.searchTarget = candidate.searchTarget;
            if (it->second.usn.empty() && !candidate.usn.empty()) it->second.usn = candidate.usn;
        }
    }

    closesocket(sock);
#else
    (void)targets;
#endif
    return records;
}

std::string XmlDecode(std::string value) {
    const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
    };
    for (const auto& [encoded, decoded] : entities) {
        std::size_t position = 0;
        while ((position = value.find(encoded, position)) != std::string::npos) {
            value.replace(position, std::strlen(encoded), decoded);
            position += std::strlen(decoded);
        }
    }
    return TrimAscii(value);
}

std::string ExtractXmlTag(const std::string& xml, const std::string& tag) {
    if (xml.empty() || tag.empty()) return {};
    const std::string lower = LowerAscii(xml);
    const std::string openNeedle = "<" + LowerAscii(tag);
    const std::string closeNeedle = "</" + LowerAscii(tag) + ">";

    const auto open = lower.find(openNeedle);
    if (open == std::string::npos) return {};
    const auto valueStart = lower.find('>', open + openNeedle.size());
    if (valueStart == std::string::npos) return {};
    const auto close = lower.find(closeNeedle, valueStart + 1);
    if (close == std::string::npos || close <= valueStart + 1) return {};
    return XmlDecode(xml.substr(valueStart + 1, close - valueStart - 1));
}

std::string InferSsdpDeviceType(
    const std::string& friendlyName,
    const std::string& manufacturer,
    const std::string& modelName,
    const std::string& deviceType,
    const std::string& server,
    const std::string& searchTarget) {
    const std::string value = LowerAscii(
        friendlyName + " " + manufacturer + " " + modelName + " "
        + deviceType + " " + server + " " + searchTarget);
    if (value.find("internetgatewaydevice") != std::string::npos
        || value.find("wanconnectiondevice") != std::string::npos
        || value.find("router") != std::string::npos
        || value.find("gateway") != std::string::npos) return "router";
    if (value.find("mediarenderer") != std::string::npos
        || value.find("mediaserver") != std::string::npos
        || value.find("smart tv") != std::string::npos
        || value.find("fire tv") != std::string::npos
        || value.find("roku") != std::string::npos) return "media_device";
    if (value.find("printer") != std::string::npos) return "printer";
    if (value.find("camera") != std::string::npos
        || value.find("doorbell") != std::string::npos
        || value.find("ring") != std::string::npos) return "camera";
    if (value.find("nas") != std::string::npos
        || value.find("storage") != std::string::npos) return "storage";
    return "network_device";
}

json BuildSsdpEnrichment(const SsdpRecord& record) {
    json item = {
        {"ipAddress", record.ipAddress},
        {"hostname", ""},
        {"vendor", ""},
        {"model", ""},
        {"deviceType", InferSsdpDeviceType(
            "", "", "", "", record.server, record.searchTarget)},
        {"discoveryMethods", json::array({"ssdp"})},
        {"metadata", {
            {"ssdp", {
                {"server", record.server},
                {"st", record.searchTarget},
                {"usn", record.usn},
                {"location", record.location}
            }}
        }}
    };

#ifdef _WIN32
    if (record.location.empty()) return item;

    const std::wstring wideUrl = Utf8ToWide(record.location);
    if (wideUrl.empty()) return item;

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) return item;
    if (parts.nScheme != INTERNET_SCHEME_HTTP) return item;

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::string hostUtf8 = WideToUtf8(host);
    if (hostUtf8 != record.ipAddress || !IsPrivateIpv4Address(hostUtf8)) return item;

    std::wstring path;
    if (parts.lpszUrlPath && parts.dwUrlPathLength) {
        path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    } else {
        path = L"/";
    }
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(
        L"Hi5Central-NetworkDiscovery/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) return item;
    WinHttpSetTimeouts(session, 1000, 1000, 1000, 1500);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return item;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return item;
    }

    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

    std::string xml;
    bool ok = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0) != FALSE;
    if (ok) ok = WinHttpReceiveResponse(request, nullptr) != FALSE;

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (ok) {
        ok = WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) != FALSE
            && statusCode >= 200 && statusCode < 300;
    }

    static constexpr std::size_t kMaxXmlBytes = 256 * 1024;
    while (ok && xml.size() < kMaxXmlBytes) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        const DWORD allowed = static_cast<DWORD>(
            std::min<std::size_t>(available, kMaxXmlBytes - xml.size()));
        if (allowed == 0) break;
        std::vector<char> chunk(allowed);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), allowed, &read) || read == 0) break;
        xml.append(chunk.data(), static_cast<std::size_t>(read));
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!ok || xml.empty()) return item;

    const std::string friendlyName = ExtractXmlTag(xml, "friendlyName");
    const std::string manufacturer = ExtractXmlTag(xml, "manufacturer");
    const std::string modelName = ExtractXmlTag(xml, "modelName");
    const std::string deviceType = ExtractXmlTag(xml, "deviceType");

    if (!friendlyName.empty()) item["hostname"] = friendlyName;
    if (!manufacturer.empty()) item["vendor"] = manufacturer;
    if (!modelName.empty()) item["model"] = modelName;
    item["deviceType"] = InferSsdpDeviceType(
        friendlyName,
        manufacturer,
        modelName,
        deviceType,
        record.server,
        record.searchTarget);
    item["metadata"]["ssdp"]["deviceType"] = deviceType;
    item["metadata"]["ssdp"]["friendlyName"] = friendlyName;
    item["metadata"]["ssdp"]["manufacturer"] = manufacturer;
    item["metadata"]["ssdp"]["modelName"] = modelName;
#else
    (void)record;
#endif
    return item;
}


void AppendDnsName(std::vector<std::uint8_t>& packet, const std::string& name) {
    std::size_t start = 0;
    while (start < name.size()) {
        const std::size_t dot = name.find('.', start);
        const std::size_t end = dot == std::string::npos ? name.size() : dot;
        const std::size_t length = end - start;
        if (length == 0 || length > 63) return;
        packet.push_back(static_cast<std::uint8_t>(length));
        packet.insert(packet.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                      name.begin() + static_cast<std::ptrdiff_t>(end));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    packet.push_back(0);
}

bool ReadDnsName(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t& offset,
    std::string& output) {
    output.clear();
    if (!data || offset >= size) return false;

    std::size_t cursor = offset;
    bool jumped = false;
    std::size_t jumps = 0;

    for (;;) {
        if (cursor >= size || jumps > 24) return false;
        const std::uint8_t length = data[cursor];

        if ((length & 0xc0) == 0xc0) {
            if (cursor + 1 >= size) return false;
            const std::size_t pointer =
                (static_cast<std::size_t>(length & 0x3f) << 8)
                | static_cast<std::size_t>(data[cursor + 1]);
            if (pointer >= size) return false;
            if (!jumped) offset = cursor + 2;
            cursor = pointer;
            jumped = true;
            ++jumps;
            continue;
        }

        if (length == 0) {
            if (!jumped) offset = cursor + 1;
            return true;
        }

        if ((length & 0xc0) != 0 || length > 63 || cursor + 1 + length > size) {
            return false;
        }

        if (!output.empty()) output.push_back('.');
        output.append(
            reinterpret_cast<const char*>(data + cursor + 1),
            static_cast<std::size_t>(length));
        cursor += 1 + length;
        if (!jumped) offset = cursor;
    }
}

std::uint16_t ReadDnsU16(const std::uint8_t* data, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[offset]) << 8)
        | static_cast<std::uint16_t>(data[offset + 1]));
}

std::uint32_t ReadDnsU32(const std::uint8_t* data, std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24)
        | (static_cast<std::uint32_t>(data[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 8)
        | static_cast<std::uint32_t>(data[offset + 3]);
}

std::string MdnsInstanceName(const std::string& value) {
    const auto marker = value.find("._");
    if (marker == std::string::npos || marker == 0) return {};
    return TrimAscii(value.substr(0, marker));
}

std::string StripLocalSuffix(std::string value) {
    const std::string lower = LowerAscii(value);
    static constexpr char kSuffix[] = ".local";
    if (lower.size() > sizeof(kSuffix) - 1
        && lower.compare(
            lower.size() - (sizeof(kSuffix) - 1),
            sizeof(kSuffix) - 1,
            kSuffix) == 0) {
        value.resize(value.size() - (sizeof(kSuffix) - 1));
    }
    return TrimAscii(value);
}

struct MdnsRecord {
    std::string ipAddress;
    std::string friendlyName;
    std::string hostname;
    std::string vendor;
    std::string model;
    std::string deviceType;
    std::vector<std::string> services;
};

void AddUniqueService(std::vector<std::string>& services, const std::string& value) {
    if (value.empty() || value.size() > 256 || services.size() >= 32) return;
    if (std::find(services.begin(), services.end(), value) == services.end()) {
        services.push_back(value);
    }
}

std::string InferMdnsDeviceType(const MdnsRecord& record) {
    std::string value = LowerAscii(
        record.friendlyName + " " + record.hostname + " "
        + record.vendor + " " + record.model + " ");
    for (const auto& service : record.services) {
        value += LowerAscii(service);
        value.push_back(' ');
    }

    if (value.find("_ipp._tcp") != std::string::npos
        || value.find("_printer._tcp") != std::string::npos
        || value.find("_pdl-datastream._tcp") != std::string::npos) return "printer";
    if (value.find("_airplay._tcp") != std::string::npos
        || value.find("_raop._tcp") != std::string::npos
        || value.find("_googlecast._tcp") != std::string::npos
        || value.find("_spotify-connect._tcp") != std::string::npos) return "media_device";
    if (value.find("_hap._tcp") != std::string::npos
        || value.find("_homekit._tcp") != std::string::npos) return "smart_home";
    if (value.find("_workstation._tcp") != std::string::npos
        || value.find("_smb._tcp") != std::string::npos
        || value.find("_ssh._tcp") != std::string::npos) return "computer";
    if (value.find("camera") != std::string::npos
        || value.find("doorbell") != std::string::npos
        || value.find("ring") != std::string::npos) return "camera";
    return "network_device";
}

void ApplyMdnsTxt(
    MdnsRecord& record,
    const std::uint8_t* data,
    std::size_t offset,
    std::size_t length) {
    const std::size_t end = offset + length;
    while (offset < end) {
        const std::uint8_t partLength = data[offset++];
        if (partLength == 0 || offset + partLength > end) break;
        std::string part(
            reinterpret_cast<const char*>(data + offset),
            static_cast<std::size_t>(partLength));
        offset += partLength;

        const auto equals = part.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = LowerAscii(TrimAscii(part.substr(0, equals)));
        const std::string value = TrimAscii(part.substr(equals + 1));
        if (value.empty() || value.size() > 512) continue;

        if ((key == "fn" || key == "name") && record.friendlyName.empty()) {
            record.friendlyName = value;
        } else if ((key == "md" || key == "model" || key == "ty") && record.model.empty()) {
            record.model = value;
        } else if ((key == "manufacturer" || key == "mf" || key == "vendor")
                   && record.vendor.empty()) {
            record.vendor = value;
        }
    }
}

std::unordered_map<std::string, MdnsRecord> DiscoverMdns(
    const std::unordered_set<std::string>& targets) {
    std::unordered_map<std::string, MdnsRecord> records;
#ifdef _WIN32
    if (targets.empty()) return records;

    static const std::vector<std::string> kServiceTypes = {
        "_airplay._tcp.local",
        "_raop._tcp.local",
        "_googlecast._tcp.local",
        "_ipp._tcp.local",
        "_printer._tcp.local",
        "_pdl-datastream._tcp.local",
        "_workstation._tcp.local",
        "_smb._tcp.local",
        "_ssh._tcp.local",
        "_hap._tcp.local",
        "_homekit._tcp.local",
        "_companion-link._tcp.local",
        "_spotify-connect._tcp.local",
        "_http._tcp.local",
        "_https._tcp.local"
    };

    std::vector<std::uint8_t> query(12, 0);
    query[4] = static_cast<std::uint8_t>((kServiceTypes.size() >> 8) & 0xff);
    query[5] = static_cast<std::uint8_t>(kServiceTypes.size() & 0xff);
    for (const auto& service : kServiceTypes) {
        AppendDnsName(query, service);
        query.push_back(0);
        query.push_back(12);  // PTR
        query.push_back(0x80);
        query.push_back(0x01);  // IN + unicast-response preference
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return records;

    DWORD timeoutMs = 250;
    setsockopt(
        sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMs),
        sizeof(timeoutMs));
    unsigned char ttl = 1;
    setsockopt(
        sock,
        IPPROTO_IP,
        IP_MULTICAST_TTL,
        reinterpret_cast<const char*>(&ttl),
        sizeof(ttl));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        closesocket(sock);
        return records;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(5353);
    InetPtonA(AF_INET, "224.0.0.251", &destination.sin_addr);
    sendto(
        sock,
        reinterpret_cast<const char*>(query.data()),
        static_cast<int>(query.size()),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2400);
    std::vector<std::uint8_t> buffer(64 * 1024);

    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        const int received = recvfrom(
            sock,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        if (received <= 0) {
            const int socketError = WSAGetLastError();
            if (socketError == WSAETIMEDOUT || socketError == WSAEWOULDBLOCK) continue;
            break;
        }

        if (ntohs(source.sin_port) != 5353) continue;

        char ipBuffer[INET_ADDRSTRLEN]{};
        if (!InetNtopA(AF_INET, &source.sin_addr, ipBuffer, sizeof(ipBuffer))) continue;
        const std::string ipAddress = ipBuffer;
        if (targets.find(ipAddress) == targets.end()) continue;

        const std::size_t packetSize = static_cast<std::size_t>(received);
        if (packetSize < 12) continue;
        const std::uint16_t flags = ReadDnsU16(buffer.data(), 2);
        if ((flags & 0x8000u) == 0) continue;

        MdnsRecord& record = records[ipAddress];
        record.ipAddress = ipAddress;

        const std::uint16_t qdCount = ReadDnsU16(buffer.data(), 4);
        const std::uint16_t anCount = ReadDnsU16(buffer.data(), 6);
        const std::uint16_t nsCount = ReadDnsU16(buffer.data(), 8);
        const std::uint16_t arCount = ReadDnsU16(buffer.data(), 10);
        if (static_cast<std::uint32_t>(qdCount)
                + static_cast<std::uint32_t>(anCount)
                + static_cast<std::uint32_t>(nsCount)
                + static_cast<std::uint32_t>(arCount) > 512) {
            continue;
        }

        std::size_t offset = 12;
        bool valid = true;
        for (std::uint16_t index = 0; index < qdCount; ++index) {
            std::string ignored;
            if (!ReadDnsName(buffer.data(), packetSize, offset, ignored)
                || offset + 4 > packetSize) {
                valid = false;
                break;
            }
            offset += 4;
        }
        if (!valid) continue;

        const std::uint32_t recordCount =
            static_cast<std::uint32_t>(anCount)
            + static_cast<std::uint32_t>(nsCount)
            + static_cast<std::uint32_t>(arCount);

        for (std::uint32_t index = 0; index < recordCount; ++index) {
            std::string owner;
            if (!ReadDnsName(buffer.data(), packetSize, offset, owner)
                || offset + 10 > packetSize) break;

            const std::uint16_t type = ReadDnsU16(buffer.data(), offset);
            offset += 2;
            offset += 2;  // class
            (void)ReadDnsU32(buffer.data(), offset);
            offset += 4;
            const std::uint16_t rdLength = ReadDnsU16(buffer.data(), offset);
            offset += 2;
            if (offset + rdLength > packetSize) break;

            const std::size_t rdataOffset = offset;
            if (type == 12) {  // PTR
                std::size_t nameOffset = rdataOffset;
                std::string target;
                if (ReadDnsName(buffer.data(), packetSize, nameOffset, target)) {
                    AddUniqueService(record.services, owner);
                    const std::string instance = MdnsInstanceName(target);
                    if (record.friendlyName.empty() && !instance.empty()) {
                        record.friendlyName = instance;
                    }
                }
            } else if (type == 33 && rdLength >= 6) {  // SRV
                std::size_t targetOffset = rdataOffset + 6;
                std::string target;
                if (ReadDnsName(buffer.data(), packetSize, targetOffset, target)) {
                    const std::string hostname = StripLocalSuffix(target);
                    if (record.hostname.empty() && !hostname.empty()) {
                        record.hostname = hostname;
                    }
                }
                const std::string instance = MdnsInstanceName(owner);
                if (record.friendlyName.empty() && !instance.empty()) {
                    record.friendlyName = instance;
                }
            } else if (type == 16) {  // TXT
                ApplyMdnsTxt(record, buffer.data(), rdataOffset, rdLength);
            }

            offset = rdataOffset + rdLength;
        }
    }

    closesocket(sock);

    for (auto it = records.begin(); it != records.end();) {
        auto& record = it->second;
        const bool useful = !record.friendlyName.empty()
            || !record.hostname.empty()
            || !record.vendor.empty()
            || !record.model.empty()
            || !record.services.empty();
        if (!useful) {
            it = records.erase(it);
            continue;
        }
        record.deviceType = InferMdnsDeviceType(record);
        ++it;
    }
#else
    (void)targets;
#endif
    return records;
}

json BuildMdnsEnrichment(const MdnsRecord& record) {
    json services = json::array();
    for (const auto& service : record.services) services.push_back(service);

    const std::string name = !record.friendlyName.empty()
        ? record.friendlyName
        : record.hostname;

    return json{
        {"ipAddress", record.ipAddress},
        {"hostname", name},
        {"vendor", record.vendor},
        {"model", record.model},
        {"deviceType", record.deviceType},
        {"discoveryMethods", json::array({"mdns"})},
        {"metadata", {
            {"mdns", {
                {"hostname", record.hostname},
                {"friendlyName", record.friendlyName},
                {"services", services}
            }}
        }}
    };
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

nlohmann::json RunNetworkDiscoveryEnrichment(
    const nlohmann::json& payload,
    std::string& error) {
    error.clear();
    const auto startedAt = std::chrono::steady_clock::now();

    std::string winsockError;
    if (!EnsureWinsock(winsockError)) {
        error = winsockError;
        return json{{"status", "failed"}, {"error", error}};
    }

    const json rawTargets = payload.value("targets", json::array());
    if (!rawTargets.is_array()) {
        error = "Network discovery enrichment targets are invalid.";
        return json{{"status", "failed"}, {"error", error}};
    }

    std::vector<std::string> targets;
    std::unordered_set<std::string> seen;
    targets.reserve(std::min<std::size_t>(rawTargets.size(), 512));

    for (const auto& raw : rawTargets) {
        if (!raw.is_string()) continue;
        const std::string ipAddress = raw.get<std::string>();
        if (ipAddress.empty() || ipAddress.size() > 64) continue;

        IN_ADDR address{};
        if (InetPtonA(AF_INET, ipAddress.c_str(), &address) != 1) continue;
        const std::uint32_t host = ntohl(address.S_un.S_addr);
        const std::uint8_t firstOctet = static_cast<std::uint8_t>((host >> 24) & 0xff);
        const std::uint8_t secondOctet = static_cast<std::uint8_t>((host >> 16) & 0xff);
        const bool privateAddress =
            firstOctet == 10
            || (firstOctet == 172 && secondOctet >= 16 && secondOctet <= 31)
            || (firstOctet == 192 && secondOctet == 168);
        if (!privateAddress) continue;

        if (seen.insert(ipAddress).second) {
            targets.push_back(ipAddress);
            if (targets.size() >= 512) break;
        }
    }

    std::atomic<std::size_t> nextIndex{0};
    std::mutex resultMutex;
    std::vector<json> resolved;
    resolved.reserve(targets.size() * 2);

    std::unordered_set<std::string> targetSet(targets.begin(), targets.end());
    std::unordered_map<std::string, SsdpRecord> ssdpRecords;
    std::unordered_map<std::string, MdnsRecord> mdnsRecords;
    std::thread ssdpDiscovery([&]() {
        ssdpRecords = DiscoverSsdp(targetSet);
    });
    std::thread mdnsDiscovery([&]() {
        mdnsRecords = DiscoverMdns(targetSet);
    });

    const std::size_t workerCount =
        std::max<std::size_t>(1, std::min<std::size_t>(16, targets.size()));
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&]() {
            for (;;) {
                const std::size_t index = nextIndex.fetch_add(1);
                if (index >= targets.size()) break;

                const std::string& ipAddress = targets[index];
                const std::string hostname = ReverseDnsName(ipAddress);
                if (hostname.empty()) continue;

                json item = {
                    {"ipAddress", ipAddress},
                    {"hostname", hostname},
                    {"discoveryMethods", json::array({"reverse_dns"})}
                };
                std::lock_guard<std::mutex> lock(resultMutex);
                resolved.push_back(std::move(item));
            }
        });
    }
    for (auto& worker : workers) worker.join();
    if (ssdpDiscovery.joinable()) ssdpDiscovery.join();
    if (mdnsDiscovery.joinable()) mdnsDiscovery.join();

    for (const auto& [ipAddress, record] : mdnsRecords) {
        (void)ipAddress;
        resolved.push_back(BuildMdnsEnrichment(record));
    }

    std::vector<SsdpRecord> ssdpList;
    ssdpList.reserve(ssdpRecords.size());
    for (const auto& [ipAddress, record] : ssdpRecords) {
        (void)ipAddress;
        ssdpList.push_back(record);
    }

    std::atomic<std::size_t> ssdpIndex{0};
    const std::size_t ssdpWorkerCount =
        std::max<std::size_t>(1, std::min<std::size_t>(8, ssdpList.size()));
    std::vector<std::thread> ssdpWorkers;
    ssdpWorkers.reserve(ssdpWorkerCount);
    for (std::size_t worker = 0; worker < ssdpWorkerCount; ++worker) {
        ssdpWorkers.emplace_back([&]() {
            for (;;) {
                const std::size_t index = ssdpIndex.fetch_add(1);
                if (index >= ssdpList.size()) break;
                json item = BuildSsdpEnrichment(ssdpList[index]);
                std::lock_guard<std::mutex> lock(resultMutex);
                resolved.push_back(std::move(item));
            }
        });
    }
    for (auto& worker : ssdpWorkers) worker.join();

    std::sort(
        resolved.begin(),
        resolved.end(),
        [](const json& left, const json& right) {
            const std::string leftIp = left.value("ipAddress", std::string());
            const std::string rightIp = right.value("ipAddress", std::string());
            if (leftIp != rightIp) return leftIp < rightIp;
            return left.value("hostname", std::string()).size()
                > right.value("hostname", std::string()).size();
        });

    std::unordered_set<std::string> enrichedIps;
    std::unordered_set<std::string> namedIps;
    for (const auto& item : resolved) {
        const std::string ipAddress = item.value("ipAddress", std::string());
        if (!ipAddress.empty()) enrichedIps.insert(ipAddress);
        if (!item.value("hostname", std::string()).empty()) namedIps.insert(ipAddress);
    }

    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();

    return json{
        {"status", "ok"},
        {"protocolVersion", 3},
        {"targetsTotal", targets.size()},
        {"enrichedCount", enrichedIps.size()},
        {"resolvedCount", namedIps.size()},
        {"ssdpCount", ssdpRecords.size()},
        {"mdnsCount", mdnsRecords.size()},
        {"durationMs", durationMs},
        {"devices", resolved}
    };
}

}  // namespace hi5::network
