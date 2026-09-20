#include "agent_identity.h"
#include "platform/platform_identity.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace {
    using json = nlohmann::json;

    constexpr const char* kDefaultApiBase = "https://api.hi5central.com";
    constexpr const char* kDefaultAgentWsBase = "wss://rmm.hi5central.com/agent/ws";
#ifndef HI5_AGENT_VERSION
#define HI5_AGENT_VERSION "1.0.0"
#endif
    constexpr const char* kAgentVersion = HI5_AGENT_VERSION;
    constexpr DWORD kDpapiFlags = CRYPTPROTECT_LOCAL_MACHINE;

    std::string trim(std::string s) {
        auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    std::wstring utf8ToWide(const std::string& s) {
        if (s.empty()) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
        return out;
    }

    std::string wideToUtf8(const std::wstring& s) {
        if (s.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::string getenvString(const char* name) {
        DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
        if (needed == 0) return {};
        std::string out(needed, '\0');
        DWORD written = GetEnvironmentVariableA(name, out.data(), needed);
        if (written == 0 || written >= needed) return {};
        out.resize(written);
        return trim(out);
    }

    std::unordered_map<std::string, std::string> parseIniIfExists(const std::filesystem::path& path) {
        std::unordered_map<std::string, std::string> kv;
        std::ifstream in(path);
        if (!in) return kv;

        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') continue;

            auto pos = line.find('=');
            if (pos == std::string::npos) continue;

            kv[lower(trim(line.substr(0, pos)))] = trim(line.substr(pos + 1));
        }

        return kv;
    }

    std::string getFirst(
        const std::unordered_map<std::string, std::string>& kv,
        std::initializer_list<const char*> keys
    ) {
        for (const char* key : keys) {
            auto it = kv.find(lower(key));
            if (it != kv.end() && !trim(it->second).empty()) return trim(it->second);
        }
        return {};
    }

    std::vector<uint8_t> readAllBytesIfExists(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};

        in.seekg(0, std::ios::end);
        std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        if (size <= 0) return {};

        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!in.read(reinterpret_cast<char*>(data.data()), size)) return {};
        return data;
    }

    void writeAllBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("failed to create " + path.filename().string());
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("failed to write " + path.filename().string());
    }

    void moveAsideIfExists(const std::filesystem::path& path, const char* suffix) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;

        std::filesystem::path target = path;
        target += suffix;

        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(path, target, ec);
        if (ec) {
            // If rename fails, leave the file in place rather than risking data loss.
            // The new loader ignores legacy files once state.dat exists.
        }
    }

    std::string decryptDpapiBlobToUtf8(const std::vector<uint8_t>& blob) {
        if (blob.empty()) return {};

        DATA_BLOB inBlob{};
        inBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(blob.data()));
        inBlob.cbData = static_cast<DWORD>(blob.size());

        DATA_BLOB outBlob{};
        if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, kDpapiFlags, &outBlob)) {
            return {};
        }

        std::string out(
            reinterpret_cast<const char*>(outBlob.pbData),
            reinterpret_cast<const char*>(outBlob.pbData) + outBlob.cbData
        );
        LocalFree(outBlob.pbData);
        return trim(out);
    }

    std::vector<uint8_t> encryptUtf8ToDpapiBlob(const std::string& plain, const wchar_t* description) {
        DATA_BLOB inBlob{};
        inBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.data()));
        inBlob.cbData = static_cast<DWORD>(plain.size());

        DATA_BLOB outBlob{};
        if (!CryptProtectData(&inBlob, description, nullptr, nullptr, nullptr, kDpapiFlags, &outBlob)) {
            throw std::runtime_error("CryptProtectData failed");
        }

        std::vector<uint8_t> out(outBlob.pbData, outBlob.pbData + outBlob.cbData);
        LocalFree(outBlob.pbData);
        return out;
    }

    std::string nowIsoUtc() {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        char buf[64]{};
        sprintf_s(
            buf,
            "%04u-%02u-%02uT%02u:%02u:%02uZ",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond
        );
        return buf;
    }

    
    std::string getMachineFingerprint() {
    return hi5::GetPlatformMachineId();
    }

    struct AgentState {
        std::string deviceId;
        std::string deviceKey;
        std::string apiBaseUrl;
        std::string agentWsBaseUrl;
        std::string tenantId;
        std::string groupId;
        std::string enrollmentPackageId;
        std::string fingerprint;
    };

    bool isValidState(const AgentState& s) {
        return !s.deviceId.empty() && !s.deviceKey.empty() && !s.agentWsBaseUrl.empty();
    }

    AgentState parseStateJson(const std::string& plain) {
        AgentState s;
        const auto j = json::parse(plain, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return s;

                s.deviceId = trim(j.value("device_id", std::string()));
        if (s.deviceId.empty()) s.deviceId = trim(j.value("deviceId", std::string()));
                s.deviceKey = trim(j.value("device_key", std::string()));
        if (s.deviceKey.empty()) s.deviceKey = trim(j.value("deviceKey", std::string()));
        if (s.deviceKey.empty()) s.deviceKey = trim(j.value("agent_secret", std::string()));
        if (s.deviceKey.empty()) s.deviceKey = trim(j.value("agentSecret", std::string()));
                s.apiBaseUrl = trim(j.value("api_base_url", std::string()));
        if (s.apiBaseUrl.empty()) s.apiBaseUrl = trim(j.value("apiBaseUrl", std::string()));
                s.agentWsBaseUrl = trim(j.value("agent_ws_base_url", std::string()));
        if (s.agentWsBaseUrl.empty()) s.agentWsBaseUrl = trim(j.value("agentWsBaseUrl", std::string()));
                s.tenantId = trim(j.value("tenant_id", std::string()));
        if (s.tenantId.empty()) s.tenantId = trim(j.value("tenantId", std::string()));
                s.groupId = trim(j.value("group_id", std::string()));
        if (s.groupId.empty()) s.groupId = trim(j.value("groupId", std::string()));
                s.enrollmentPackageId = trim(j.value("enrollment_package_id", std::string()));
        if (s.enrollmentPackageId.empty()) s.enrollmentPackageId = trim(j.value("enrollmentPackageId", std::string()));
        s.fingerprint = trim(j.value("fingerprint", std::string()));
        return s;
    }

    AgentState readStateIfExists(const std::filesystem::path& statePath) {
        const auto bytes = readAllBytesIfExists(statePath);
        if (bytes.empty()) return {};
        return parseStateJson(decryptDpapiBlobToUtf8(bytes));
    }

    void writeState(const std::filesystem::path& statePath, const AgentState& state) {
        json j = {
            {"version", 1},
            {"device_id", state.deviceId},
            {"device_key", state.deviceKey},
            {"api_base_url", state.apiBaseUrl},
            {"agent_ws_base_url", state.agentWsBaseUrl},
            {"tenant_id", state.tenantId},
            {"group_id", state.groupId},
            {"enrollment_package_id", state.enrollmentPackageId},
            {"fingerprint", state.fingerprint},
            {"updated_at", nowIsoUtc()}
        };

        writeAllBytes(statePath, encryptUtf8ToDpapiBlob(j.dump(), L"Hi5Central Agent State"));
    }

    struct ParsedUrl {
        std::wstring scheme;
        std::wstring host;
        INTERNET_PORT port = 0;
        std::wstring path;
        bool https = true;
    };

    ParsedUrl parseUrl(const std::string& urlUtf8) {
        const std::wstring url = utf8ToWide(urlUtf8);

        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);

        wchar_t scheme[16]{};
        wchar_t host[256]{};
        wchar_t path[4096]{};

        parts.lpszScheme = scheme;
        parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));

        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
            throw std::runtime_error("invalid URL: " + urlUtf8);
        }

        ParsedUrl out;
        out.scheme.assign(parts.lpszScheme, parts.dwSchemeLength);
        out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
        out.port = parts.nPort;
        out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        out.https = (parts.nScheme == INTERNET_SCHEME_HTTPS);
        if (out.path.empty()) out.path = L"/";
        return out;
    }

    std::string httpPostJson(const std::string& url, const std::string& body) {
        ParsedUrl parsed = parseUrl(url);

        HINTERNET session = WinHttpOpen(
            L"Hi5CentralAgent/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (!session) throw std::runtime_error("WinHttpOpen failed");

        HINTERNET connect = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
        if (!connect) {
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpConnect failed");
        }

        DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(
            connect,
            L"POST",
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags
        );
        if (!request) {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpOpenRequest failed");
        }

        DWORD timeoutMs = 15000;
        WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

        const wchar_t* headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";

        BOOL ok = WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1L),
            const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0
        );

        if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
            DWORD err = GetLastError();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHTTP request failed err=" + std::to_string(err));
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX
        );

        std::string response;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;

            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;

            chunk.resize(read);
            response += chunk;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);

        if (status < 200 || status >= 300) {
            throw std::runtime_error("HTTP " + std::to_string(status) + ": " + response);
        }

        return response;
    }

    struct EnrollResult {
        std::string deviceId;
        std::string deviceKey;
        std::string tenantId;
        std::string groupId;
        std::string enrollmentPackageId;
    };

    EnrollResult enrollWindowsDevice(
        const std::string& apiBase,
        const std::string& enrollmentToken,
        const std::string& hostname,
        const std::string& fingerprint,
        const std::string& existingDeviceId,
        const std::string& existingDeviceKey,
        const std::unordered_map<std::string, std::string>& legacyConfig
    ) {
        const std::string url = apiBase + "/api/v1/agent/enroll";

        json body = {
            {"enrollmentToken", enrollmentToken},
            {"hostname", hostname},
            {"platform", "windows"},
            {"architecture", "x64"},
            {"agentVersion", kAgentVersion},
            {"fingerprint", fingerprint},
            {"device_fingerprint", fingerprint}
        };

        if (!existingDeviceId.empty()) body["deviceId"] = existingDeviceId;
        if (!existingDeviceKey.empty()) body["agentSecret"] = existingDeviceKey;

        const std::string tenantId = getFirst(legacyConfig, { "tenant_id", "tenantid" });
        const std::string groupId = getFirst(legacyConfig, { "group_id", "groupid" });
        const std::string packageId = getFirst(legacyConfig, { "package_id", "packageid", "enrollment_package_id" });
        const std::string provisionBlob = getFirst(legacyConfig, { "provision_blob", "provisionblob" });

        if (!tenantId.empty()) body["tenant_id"] = tenantId;
        if (!groupId.empty()) body["group_id"] = groupId;
        if (!packageId.empty()) body["package_id"] = packageId;
        if (!provisionBlob.empty()) body["provision_blob"] = provisionBlob;

        const auto res = json::parse(httpPostJson(url, body.dump()), nullptr, false);
        if (res.is_discarded()) throw std::runtime_error("enrollment returned invalid JSON");
        if (res.contains("success") && !res.value("success", false)) {
            throw std::runtime_error("enrollment failed: " + res.dump());
        }

        EnrollResult out;
                out.deviceId = trim(res.value("device_id", std::string()));
        if (out.deviceId.empty() && res.contains("device") && res["device"].is_object()) {
            out.deviceId = trim(res["device"].value("id", std::string()));
        }
        if (out.deviceId.empty() && res.contains("credentials") && res["credentials"].is_object()) {
            out.deviceId = trim(res["credentials"].value("deviceId", std::string()));
        }
                out.deviceKey = trim(res.value("device_key", std::string()));
        if (out.deviceKey.empty() && res.contains("credentials") && res["credentials"].is_object()) {
            out.deviceKey = trim(res["credentials"].value("agentSecret", std::string()));
        }
                out.tenantId = trim(res.value("tenant_id", std::string()));
        if (out.tenantId.empty() && res.contains("tenant") && res["tenant"].is_object()) {
            out.tenantId = trim(res["tenant"].value("id", std::string()));
        }
        out.groupId = trim(res.value("group_id", std::string()));
        out.enrollmentPackageId = trim(res.value("enrollment_package_id", std::string()));

        if (out.deviceId.empty()) throw std::runtime_error("enrollment response missing device id: " + res.dump());
        if (out.deviceKey.empty()) out.deviceKey = existingDeviceKey;
        if (out.deviceKey.empty()) throw std::runtime_error("enrollment response missing agent secret: " + res.dump());

        return out;
    }

    std::string normalizedUrlNoTrailingSlash(std::string value) {
        value = trim(value);
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value;
    }
}

AgentIdentity loadAgentIdentityFromDir(
    const std::wstring& dir,
    const std::string& defaultAgentWsBaseUrl
) {
    const std::filesystem::path root(dir);
        const std::filesystem::path statePath = root / L"state.dat";
    const std::filesystem::path agentJsonPath = root / L"agent.json";
    const std::filesystem::path legacyConfigPath = root / L"config.ini";
    const std::filesystem::path legacySecretsPath = root / L"secrets.dat";

    std::filesystem::create_directories(root);

    const std::string envApiBase = normalizedUrlNoTrailingSlash(
        !getenvString("HI5_API_BASE").empty() ? getenvString("HI5_API_BASE") : getenvString("HI5_API_BASE_URL")
    );
    const std::string envWsBase = trim(
        !getenvString("HI5_WSS_URL").empty() ? getenvString("HI5_WSS_URL") : getenvString("HI5_AGENT_WS_BASE_URL")
    );

    AgentState state = readStateIfExists(statePath);

    // A fresh enrollment package supplied by the installer must be able to replace
    // stale development/legacy identity state. Do this transactionally: the old
    // state.dat is left untouched unless enrollment succeeds and the replacement
    // state has been DPAPI-protected successfully.
    const auto bootstrapConfig = parseIniIfExists(legacyConfigPath);
    std::string bootstrapToken = getenvString("HI5_ENROLLMENT_TOKEN");
    if (bootstrapToken.empty()) {
        bootstrapToken = getFirst(bootstrapConfig, { "enrollment_token", "provision_token", "token" });
    }

    std::string forceEnrollmentValue = getenvString("HI5_FORCE_ENROLLMENT");
    if (forceEnrollmentValue.empty()) {
        forceEnrollmentValue = getFirst(bootstrapConfig, { "force_enrollment", "forceenrollment" });
    }
    forceEnrollmentValue = lower(trim(forceEnrollmentValue));
    const bool forceEnrollment = !bootstrapToken.empty() &&
        (forceEnrollmentValue == "1" || forceEnrollmentValue == "true" ||
         forceEnrollmentValue == "yes" || forceEnrollmentValue == "on");

    const std::string bootstrapPackageId = getFirst(bootstrapConfig, {
        "package_id", "packageid", "enrollment_package_id"
    });
    const bool packageAlreadyApplied = isValidState(state) && !bootstrapPackageId.empty() &&
        state.enrollmentPackageId == bootstrapPackageId;

    if (forceEnrollment && !packageAlreadyApplied) {
        std::string apiBase = envApiBase;
        if (apiBase.empty()) {
            apiBase = normalizedUrlNoTrailingSlash(getFirst(bootstrapConfig, {
                "api_base_url", "api_base", "apibase", "api_url"
            }));
        }
        if (apiBase.empty()) apiBase = kDefaultApiBase;

        std::string agentWsBase = envWsBase;
        if (agentWsBase.empty()) {
            agentWsBase = getFirst(bootstrapConfig, {
                "agent_ws_base_url", "agent_ws_base", "wss_url", "ws_url", "control_url"
            });
        }
        if (agentWsBase.empty()) {
            agentWsBase = defaultAgentWsBaseUrl.empty() ? kDefaultAgentWsBase : defaultAgentWsBaseUrl;
        }

        std::string hostname = getFirst(bootstrapConfig, { "hostname", "host_name", "computer_name" });
        if (hostname.empty()) hostname = hi5::GetPlatformHostname();
        std::string fingerprint = getFirst(bootstrapConfig, { "fingerprint", "device_fingerprint" });
        if (fingerprint.empty()) fingerprint = getMachineFingerprint();

        const EnrollResult enrolled = enrollWindowsDevice(
            apiBase, bootstrapToken, hostname, fingerprint, "", "", bootstrapConfig
        );

        AgentState replacementState;
        replacementState.deviceId = enrolled.deviceId;
        replacementState.deviceKey = enrolled.deviceKey;
        replacementState.apiBaseUrl = apiBase;
        replacementState.agentWsBaseUrl = agentWsBase;
        replacementState.tenantId = enrolled.tenantId.empty()
            ? getFirst(bootstrapConfig, { "tenant_id", "tenantid" }) : enrolled.tenantId;
        replacementState.groupId = enrolled.groupId.empty()
            ? getFirst(bootstrapConfig, { "group_id", "groupid" }) : enrolled.groupId;
        replacementState.enrollmentPackageId = enrolled.enrollmentPackageId.empty()
            ? bootstrapPackageId : enrolled.enrollmentPackageId;
        replacementState.fingerprint = fingerprint;

        writeState(statePath, replacementState);
        moveAsideIfExists(legacyConfigPath, ".migrated");
        moveAsideIfExists(legacySecretsPath, ".migrated");

        AgentIdentity ident;
        ident.deviceId = replacementState.deviceId;
        ident.deviceKey = replacementState.deviceKey;
        ident.agentWsBaseUrl = replacementState.agentWsBaseUrl;
        return ident;
    }

    if (isValidState(state)) {
        if (!envApiBase.empty()) state.apiBaseUrl = envApiBase;
        if (!envWsBase.empty()) state.agentWsBaseUrl = envWsBase;
        if (state.apiBaseUrl.empty()) state.apiBaseUrl = kDefaultApiBase;
        if (state.agentWsBaseUrl.empty()) state.agentWsBaseUrl = defaultAgentWsBaseUrl.empty() ? kDefaultAgentWsBase : defaultAgentWsBaseUrl;

        writeState(statePath, state);

        AgentIdentity ident;
        ident.deviceId = state.deviceId;
        ident.deviceKey = state.deviceKey;
        ident.agentWsBaseUrl = state.agentWsBaseUrl;
        return ident;
    }

    const auto agentJsonBytes = readAllBytesIfExists(agentJsonPath);
    if (!agentJsonBytes.empty()) {
        AgentState jsonState = parseStateJson(std::string(agentJsonBytes.begin(), agentJsonBytes.end()));
        if (isValidState(jsonState)) {
            if (!envApiBase.empty()) jsonState.apiBaseUrl = envApiBase;
            if (!envWsBase.empty()) jsonState.agentWsBaseUrl = envWsBase;
            if (jsonState.apiBaseUrl.empty()) jsonState.apiBaseUrl = kDefaultApiBase;
            if (jsonState.agentWsBaseUrl.empty()) {
                jsonState.agentWsBaseUrl = defaultAgentWsBaseUrl.empty() ? kDefaultAgentWsBase : defaultAgentWsBaseUrl;
            }

            writeState(statePath, jsonState);

            AgentIdentity ident;
            ident.deviceId = jsonState.deviceId;
            ident.deviceKey = jsonState.deviceKey;
            ident.agentWsBaseUrl = jsonState.agentWsBaseUrl;
            return ident;
        }
    }

    const auto legacyConfig = parseIniIfExists(legacyConfigPath);

    std::string apiBase = envApiBase;
    if (apiBase.empty()) apiBase = normalizedUrlNoTrailingSlash(getFirst(legacyConfig, { "api_base_url", "api_base", "apibase", "api_url" }));
    if (apiBase.empty()) apiBase = kDefaultApiBase;

    std::string agentWsBase = envWsBase;
    if (agentWsBase.empty()) agentWsBase = getFirst(legacyConfig, { "agent_ws_base_url", "agent_ws_base", "wss_url", "ws_url", "control_url" });
    if (agentWsBase.empty()) agentWsBase = defaultAgentWsBaseUrl.empty() ? kDefaultAgentWsBase : defaultAgentWsBaseUrl;

    std::string enrollmentToken = getenvString("HI5_ENROLLMENT_TOKEN");
    if (enrollmentToken.empty()) enrollmentToken = getFirst(legacyConfig, { "enrollment_token", "provision_token", "token" });

    std::string hostname = getFirst(legacyConfig, { "hostname", "host_name", "computer_name" });
    if (hostname.empty()) hostname = hi5::GetPlatformHostname();

    std::string fingerprint = getFirst(legacyConfig, { "fingerprint", "device_fingerprint" });
    if (fingerprint.empty()) fingerprint = getMachineFingerprint();

    std::string existingDeviceId = getFirst(legacyConfig, { "device_id", "deviceid", "id" });
    std::string existingDeviceKey = getFirst(legacyConfig, {
        "device_key",
        "devicekey",
        "agent_secret",
        "agentsecret",
        "secret"
    });

    const auto legacySecretBytes = readAllBytesIfExists(legacySecretsPath);
    if (existingDeviceKey.empty() && !legacySecretBytes.empty()) {
        existingDeviceKey = decryptDpapiBlobToUtf8(legacySecretBytes);
    }

    if (!existingDeviceId.empty() && !existingDeviceKey.empty()) {
        AgentState directState;
        directState.deviceId = existingDeviceId;
        directState.deviceKey = existingDeviceKey;
        directState.apiBaseUrl = apiBase;
        directState.agentWsBaseUrl = agentWsBase;
        directState.tenantId = getFirst(legacyConfig, { "tenant_id", "tenantid" });
        directState.groupId = getFirst(legacyConfig, { "group_id", "groupid" });
        directState.enrollmentPackageId = getFirst(legacyConfig, { "package_id", "packageid", "enrollment_package_id" });
        directState.fingerprint = fingerprint;

        writeState(statePath, directState);

        AgentIdentity ident;
        ident.deviceId = directState.deviceId;
        ident.deviceKey = directState.deviceKey;
        ident.agentWsBaseUrl = directState.agentWsBaseUrl;
        return ident;
    }
    EnrollResult enrolled = enrollWindowsDevice(
        apiBase,
        enrollmentToken,
        hostname,
        fingerprint,
        existingDeviceId,
        existingDeviceKey,
        legacyConfig
    );

    AgentState newState;
    newState.deviceId = enrolled.deviceId;
    newState.deviceKey = enrolled.deviceKey;
    newState.apiBaseUrl = apiBase;
    newState.agentWsBaseUrl = agentWsBase;
    newState.tenantId = enrolled.tenantId.empty() ? getFirst(legacyConfig, { "tenant_id", "tenantid" }) : enrolled.tenantId;
    newState.groupId = enrolled.groupId.empty() ? getFirst(legacyConfig, { "group_id", "groupid" }) : enrolled.groupId;
    newState.enrollmentPackageId = enrolled.enrollmentPackageId.empty() ? getFirst(legacyConfig, { "package_id", "packageid", "enrollment_package_id" }) : enrolled.enrollmentPackageId;
    newState.fingerprint = fingerprint;

    writeState(statePath, newState);

    // Legacy migration: the new active identity lives in state.dat.
    // Move the old files out of active use so enrollment tokens are not kept live on disk.
    moveAsideIfExists(legacyConfigPath, ".migrated");
    moveAsideIfExists(legacySecretsPath, ".migrated");

    AgentIdentity ident;
    ident.deviceId = newState.deviceId;
    ident.deviceKey = newState.deviceKey;
    ident.agentWsBaseUrl = newState.agentWsBaseUrl;
    return ident;
}

