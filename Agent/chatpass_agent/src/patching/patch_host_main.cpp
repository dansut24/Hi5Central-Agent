#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <Softpub.h>
#include <bcrypt.h>
#include <urlmon.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr const char* kPatchHostVersion = "0.2.3";
constexpr DWORD kDpapiFlags = CRYPTPROTECT_UI_FORBIDDEN;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, output.data(), size);
    while (!output.empty() && output.back() == L'\0') output.pop_back();
    return output;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), size, nullptr, nullptr);
    while (!output.empty() && output.back() == '\0') output.pop_back();
    return output;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ContainsInsensitive(const std::string& haystack, const std::string& needle) {
    return needle.empty() || Lower(haystack).find(Lower(needle)) != std::string::npos;
}

bool EndsWithInsensitive(const std::string& value, const std::string& suffix) {
    const std::string left = Lower(value);
    const std::string right = Lower(suffix);
    return left.size() >= right.size() && left.compare(left.size() - right.size(), right.size(), right) == 0;
}

std::string Truncate(const std::string& value, size_t maxLen = 6000) {
    if (value.size() <= maxLen) return value;
    return value.substr(0, maxLen) + "\n...[truncated]";
}

std::string ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return "";
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool WriteFileUtf8(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return stream.good();
}

std::filesystem::path PatchHostRoot() {
    std::filesystem::path root = LR"(C:\ProgramData\Hi5Central\Agent\PatchHost)";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

std::wstring SafePathSegment(const std::string& value) {
    std::wstring output;
    for (const wchar_t ch : Utf8ToWide(value)) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')
            || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_') {
            output.push_back(ch);
        } else {
            output.push_back(L'_');
        }
    }
    return output.empty() ? L"unknown-job" : output;
}

std::filesystem::path PatchJobRoot(const json& manifest) {
    const auto root = PatchHostRoot() / L"jobs" / SafePathSegment(manifest.value("jobId", std::string()));
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

struct SoftwareInstallMutex {
    HANDLE handle = nullptr;
    bool locked = false;

    ~SoftwareInstallMutex() {
        if (locked && handle) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
};

bool AcquireSoftwareInstallMutex(SoftwareInstallMutex& mutex, unsigned long long& waitedMs) {
    const ULONGLONG started = GetTickCount64();
    mutex.handle = CreateMutexW(nullptr, FALSE, L"Global\\Hi5CentralPatchHostSoftwareInstall");
    if (!mutex.handle) return false;
    const DWORD wait = WaitForSingleObject(mutex.handle, 2 * 60 * 60 * 1000);
    waitedMs = static_cast<unsigned long long>(GetTickCount64() - started);
    mutex.locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    return mutex.locked;
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::string QuoteCmd(const std::string& value) {
    std::string escaped = value;
    size_t pos = 0;
    while ((pos = escaped.find('"', pos)) != std::string::npos) {
        escaped.insert(pos, "\\");
        pos += 2;
    }
    return "\"" + escaped + "\"";
}
std::wstring ResolveWinget() {
    wchar_t programFiles[MAX_PATH]{};
    const DWORD count = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    if (count > 0 && count < MAX_PATH) {
        const std::filesystem::path windowsApps = std::filesystem::path(programFiles) / L"WindowsApps";
        std::error_code ec;
        std::vector<std::filesystem::path> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(windowsApps, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"Microsoft.DesktopAppInstaller_", 0) != 0) continue;
            if (name.find(L"_x64__8wekyb3d8bbwe") == std::wstring::npos) continue;
            const auto winget = entry.path() / L"winget.exe";
            if (std::filesystem::exists(winget, ec)) candidates.push_back(winget);
        }
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end());
            return candidates.back().wstring();
        }
    }
    return L"winget.exe";
}

struct CommandResult {
    int exitCode = -1;
    std::string output;
    bool timedOut = false;
};

CommandResult RunHidden(const std::wstring& command, const std::filesystem::path& outputPath, DWORD timeoutMs = 10 * 60 * 1000) {
    std::wstring shell = L"cmd.exe /d /c \"" + command + L" > " + Quote(outputPath.wstring()) + L" 2>&1\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = shell;
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    if (!created) {
        return { static_cast<int>(GetLastError()), "CreateProcessW failed", false };
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
    DWORD exitCode = 1;
    bool timedOut = false;
    if (wait == WAIT_TIMEOUT) {
        timedOut = true;
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        exitCode = ERROR_TIMEOUT;
    } else if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        exitCode = GetLastError();
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return { static_cast<int>(exitCode), ReadFileUtf8(outputPath), timedOut };
}

std::vector<std::string> Tokens(const std::string& value) {
    std::istringstream stream(value);
    std::vector<std::string> output;
    std::string token;
    while (stream >> token) output.push_back(token);
    return output;
}

struct WingetListRow {
    std::string name;
    std::string installedVersion;
    std::string availableVersion;
    std::string source;
};

std::vector<WingetListRow> FindWingetListRows(const std::string& output, const std::string& packageId) {
    std::vector<WingetListRow> rows;
    std::istringstream lines(output);
    std::string line;
    const std::string needle = Lower(packageId);
    while (std::getline(lines, line)) {
        const std::string lowerLine = Lower(line);
        size_t pos = lowerLine.find(needle);
        while (pos != std::string::npos) {
            const bool leftOk = pos == 0 || std::isspace(static_cast<unsigned char>(line[pos - 1]));
            const size_t end = pos + packageId.size();
            const bool rightOk = end >= line.size() || std::isspace(static_cast<unsigned char>(line[end]));
            if (leftOk && rightOk) {
                WingetListRow row;
                row.name = Trim(line.substr(0, pos));
                auto tail = Tokens(line.substr(end));

                if (!tail.empty()) {
                    const std::string last = Lower(tail.back());
                    if (last == "winget" || last == "msstore") {
                        row.source = tail.back();
                        tail.pop_back();
                    }
                }

                if (!tail.empty()) {
                    if ((tail[0] == "<" || tail[0] == ">" || tail[0] == "<=" || tail[0] == ">=")
                        && tail.size() >= 2) {
                        row.installedVersion = tail[0] + " " + tail[1];
                        if (tail.size() >= 3) row.availableVersion = tail[2];
                    } else {
                        row.installedVersion = tail[0];
                        if (tail.size() >= 2) row.availableVersion = tail[1];
                    }
                }

                rows.push_back(std::move(row));
                break;
            }
            pos = lowerLine.find(needle, pos + 1);
        }
    }
    return rows;
}

std::vector<int> VersionParts(const std::string& value) {
    std::vector<int> parts;
    std::string current;
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
        } else if (!current.empty()) {
            try { parts.push_back(std::stoi(current)); } catch (...) { parts.push_back(0); }
            current.clear();
        }
    }
    if (!current.empty()) {
        try { parts.push_back(std::stoi(current)); } catch (...) { parts.push_back(0); }
    }
    return parts;
}

int CompareVersions(const std::string& left, const std::string& right) {
    const std::string leftText = Trim(left);
    const auto a = VersionParts(leftText);
    const auto b = VersionParts(right);
    if (a.empty() || b.empty()) return 0;
    const size_t size = std::max(a.size(), b.size());
    for (size_t index = 0; index < size; ++index) {
        const int av = index < a.size() ? a[index] : 0;
        const int bv = index < b.size() ? b[index] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }

    // WinGet can report an installed version as "< X" when it only knows
    // that the installed package is older than X. Never flatten that to an
    // exact X during post-install verification, or a failed upgrade could be
    // reported as successful.
    if (leftText.rfind("<", 0) == 0 && leftText.rfind("<=", 0) != 0) return -1;
    if (leftText.rfind(">", 0) == 0 && leftText.rfind(">=", 0) != 0) return 1;

    return 0;
}

bool VersionMeetsTarget(const std::string& installed, const std::string& target) {
    return !installed.empty() && !target.empty() && CompareVersions(installed, target) >= 0;
}

WingetListRow LowestWingetListRow(const std::vector<WingetListRow>& rows) {
    WingetListRow selected;
    bool hasSelected = false;
    for (const auto& row : rows) {
        if (row.installedVersion.empty()) continue;
        if (!hasSelected || CompareVersions(row.installedVersion, selected.installedVersion) < 0) {
            selected = row;
            hasSelected = true;
        }
    }
    return selected;
}

std::string DecryptDpapiFile(const std::filesystem::path& path) {
    const auto encrypted = ReadFileBytes(path);
    if (encrypted.empty()) return "";

    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(encrypted.data()));
    input.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, kDpapiFlags, &output)) return "";

    std::string plain(reinterpret_cast<const char*>(output.pbData), reinterpret_cast<const char*>(output.pbData) + output.cbData);
    LocalFree(output.pbData);
    return plain;
}

std::string Sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "";
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }
    if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &returned, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }

    object.resize(objectLength);
    digest.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }

    std::ifstream stream(path, std::ios::binary);
    std::vector<char> buffer(1024 * 1024);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }
    }

    if (BCryptFinishHash(hash, digest.data(), hashLength, 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

bool VerifyAuthenticodeTrust(const std::filesystem::path& path, std::string& signerName) {
    signerName.clear();

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_SAFER_FLAG;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &trustData);

    if (status == ERROR_SUCCESS && trustData.hWVTStateData) {
        CRYPT_PROVIDER_DATA* providerData = WTHelperProvDataFromStateData(trustData.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = providerData ? WTHelperGetProvSignerFromChain(providerData, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        if (cert && cert->pCert) {
            wchar_t displayName[512]{};
            const DWORD length = CertGetNameStringW(
                cert->pCert,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0,
                nullptr,
                displayName,
                static_cast<DWORD>(sizeof(displayName) / sizeof(displayName[0])));
            if (length > 1) signerName = WideToUtf8(displayName);
        }
    }

    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trustData);
    return status == ERROR_SUCCESS;
}

bool DownloadHttps(const std::string& url, const std::filesystem::path& path) {
    if (Lower(url).rfind("https://", 0) != 0) return false;
    const HRESULT hr = URLDownloadToFileW(nullptr, Utf8ToWide(url).c_str(), path.c_str(), 0, nullptr);
    return SUCCEEDED(hr);
}
json Capabilities() {
    return {
        { "patchHostVersion", kPatchHostVersion },
        { "protocolVersion", 1 },
        { "softwareDiscovery", true },
        { "softwareInstall", true },
        { "windowsUpdateDiscovery", false },
        { "windowsUpdateInstall", false },
        { "providers", json::array({ "winget", "vendor_direct" }) },
        { "manifestMode", "dpapi_job_scoped" },
        { "catalogueOnDevice", false },
        { "vendorDirect", {
            { "httpsOnly", true },
            { "sha256Required", true },
            { "authenticodeRequired", true },
            { "installerTypes", json::array({ "msi", "exe" }) }
        } }
    };
}

json DiscoverWinget() {
    const auto root = PatchHostRoot();
    const auto exportPath = root / L"winget-export.json";
    const auto exportLog = root / L"winget-export.log";
    const auto listLog = root / L"winget-list.log";
    const std::wstring winget = ResolveWinget();

    std::error_code ec;
    std::filesystem::remove(exportPath, ec);

    const std::wstring exportCommand =
        Quote(winget) +
        L" export --output " + Quote(exportPath.wstring()) +
        L" --source winget --include-versions --accept-source-agreements --disable-interactivity --nowarn";

    const CommandResult exported = RunHidden(exportCommand, exportLog);
    if (exported.exitCode != 0 || !std::filesystem::exists(exportPath, ec)) {
        return {
            { "success", false },
            { "error", "winget_export_failed" },
            { "exitCode", exported.exitCode },
            { "detail", Truncate(exported.output) },
            { "capabilities", Capabilities() }
        };
    }

    const json rootJson = json::parse(ReadFileUtf8(exportPath), nullptr, false);
    if (rootJson.is_discarded()) {
        return { { "success", false }, { "error", "winget_export_invalid_json" }, { "capabilities", Capabilities() } };
    }

    const std::wstring listCommand =
        Quote(winget) + L" list --source winget --accept-source-agreements --disable-interactivity --nowarn";
    const CommandResult listed = RunHidden(listCommand, listLog);

    json packages = json::array();
    const auto sources = rootJson.value("Sources", json::array());
    for (const auto& source : sources) {
        std::string sourceName = "winget";
        if (source.contains("SourceDetails") && source["SourceDetails"].is_object()) {
            sourceName = source["SourceDetails"].value("Name", sourceName);
        }

        for (const auto& package : source.value("Packages", json::array())) {
            const std::string packageId = package.value("PackageIdentifier", "");
            if (packageId.empty()) continue;
            const auto rows = FindWingetListRows(listed.output, packageId);
            const WingetListRow row = LowestWingetListRow(rows);
            json installedInstances = json::array();
            std::string availableVersion;
            for (const auto& candidate : rows) {
                if (!candidate.installedVersion.empty()) installedInstances.push_back(candidate.installedVersion);
                if (availableVersion.empty() && !candidate.availableVersion.empty()) {
                    availableVersion = candidate.availableVersion;
                }
            }
            packages.push_back({
                { "packageId", packageId },
                { "name", row.name.empty() ? packageId : row.name },
                { "publisher", "" },
                { "installedVersion", row.installedVersion.empty() ? package.value("Version", "") : row.installedVersion },
                { "installedInstances", installedInstances },
                { "availableVersion", availableVersion },
                { "source", sourceName.empty() ? "winget" : sourceName }
            });
        }
    }

    return {
        { "success", true },
        { "capabilities", Capabilities() },
        { "packages", packages },
        { "winget", {
            { "path", WideToUtf8(winget) },
            { "exportExitCode", exported.exitCode },
            { "listExitCode", listed.exitCode }
        } }
    };
}
bool ManifestValid(const json& manifest, std::string& error) {
    if (!manifest.is_object()) { error = "manifest_not_object"; return false; }
    if (manifest.value("protocolVersion", 0) != 1) { error = "unsupported_protocol"; return false; }
    if (manifest.value("action", std::string()) != "software.install") { error = "unsupported_action"; return false; }
    if (manifest.value("jobId", std::string()).empty()) { error = "job_id_missing"; return false; }
    if (manifest.value("deviceId", std::string()).empty()) { error = "device_id_missing"; return false; }
    if (manifest.value("targetVersion", std::string()).empty()) { error = "target_version_missing"; return false; }

    const long long expires = manifest.value("expiresUnixMs", 0LL);
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (expires <= now) { error = "manifest_expired"; return false; }
    if (expires > now + 60LL * 60LL * 1000LL) { error = "manifest_expiry_too_far"; return false; }

    const std::string provider = manifest.value("provider", std::string());
    if (provider != "winget" && provider != "vendor_direct") { error = "unsupported_provider"; return false; }

    const std::string packageId = manifest.value("packageId", std::string());
    if (packageId.empty()) { error = "package_id_missing"; return false; }

    if (provider == "vendor_direct") {
        const std::string url = manifest.value("downloadUrl", std::string());
        const std::string sha = manifest.value("sha256", std::string());
        const std::string type = Lower(manifest.value("installerType", std::string()));
        if (Lower(url).rfind("https://", 0) != 0) { error = "vendor_url_not_https"; return false; }
        if (sha.size() != 64) { error = "vendor_sha256_missing"; return false; }
        if (type != "msi" && type != "exe") { error = "unsupported_installer_type"; return false; }
        if (manifest.value("expectedSigner", std::string()).empty()) { error = "expected_signer_missing"; return false; }
    }

    return true;
}

json VerifyInstalledVersion(const json& manifest, const std::filesystem::path& root) {
    const std::string packageId = manifest.value("packageId", std::string());
    const std::wstring winget = ResolveWinget();
    const auto log = root / L"verify.log";
    const std::wstring command =
        Quote(winget) + L" list --id " + Quote(Utf8ToWide(packageId)) +
        L" --exact --source winget --accept-source-agreements --disable-interactivity --nowarn";
    const CommandResult verify = RunHidden(command, log, 3 * 60 * 1000);
    const auto rows = FindWingetListRows(verify.output, packageId);
    const WingetListRow row = LowestWingetListRow(rows);
    const std::string target = manifest.value("targetVersion", std::string());

    bool meetsTarget = !rows.empty();
    size_t comparableInstances = 0;
    json installedVersions = json::array();
    for (const auto& candidate : rows) {
        if (candidate.installedVersion.empty()) continue;
        comparableInstances += 1;
        installedVersions.push_back(candidate.installedVersion);
        if (!VersionMeetsTarget(candidate.installedVersion, target)) meetsTarget = false;
    }
    if (comparableInstances == 0) meetsTarget = false;

    return {
        { "exitCode", verify.exitCode },
        { "installedVersion", row.installedVersion },
        { "installedVersions", installedVersions },
        { "matchingInstances", comparableInstances },
        { "availableVersion", row.availableVersion },
        { "meetsTarget", meetsTarget },
        { "output", Truncate(verify.output, 2500) }
    };
}

json VerifyInstalledVersionWithRetry(const json& manifest, const std::filesystem::path& root, bool retry) {
    const DWORD delaysMs[] = { 0, 1000, 2000, 4000, 8000, 15000 };
    json verification;
    int attempts = 0;
    for (const DWORD delay : delaysMs) {
        if (delay > 0) Sleep(delay);
        verification = VerifyInstalledVersion(manifest, root);
        attempts += 1;
        if (verification.value("meetsTarget", false) || !retry) break;
    }
    verification["attempts"] = attempts;
    return verification;
}

CommandResult RunWingetUpgrade(const json& manifest, const std::filesystem::path& root, const std::wstring& suffix = L"winget-install.log") {
    const std::string packageId = manifest.value("packageId", std::string());
    const std::wstring winget = ResolveWinget();
    const std::wstring command =
        Quote(winget) + L" upgrade --id " + Quote(Utf8ToWide(packageId)) +
        L" --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity --nowarn";
    return RunHidden(command, root / suffix, 30 * 60 * 1000);
}

json ExecuteManifest(const std::filesystem::path& encryptedManifestPath) {
    const std::string plain = DecryptDpapiFile(encryptedManifestPath);
    if (plain.empty()) return { { "success", false }, { "error", "manifest_decrypt_failed" }, { "capabilities", Capabilities() } };

    const json manifest = json::parse(plain, nullptr, false);
    std::string validationError;
    if (manifest.is_discarded() || !ManifestValid(manifest, validationError)) {
        return { { "success", false }, { "error", validationError.empty() ? "manifest_invalid_json" : validationError }, { "capabilities", Capabilities() } };
    }

    const std::filesystem::path root = PatchJobRoot(manifest);
    SoftwareInstallMutex installMutex;
    unsigned long long serializedWaitMs = 0;
    if (!AcquireSoftwareInstallMutex(installMutex, serializedWaitMs)) {
        return {
            { "success", false },
            { "error", "patch_execution_lock_timeout" },
            { "detail", "Timed out waiting for another software patch on this endpoint to finish." },
            { "serializedWaitMs", serializedWaitMs },
            { "capabilities", Capabilities() }
        };
    }

    const std::string provider = manifest.value("provider", std::string());
    const std::string applicationName = manifest.value("applicationName", std::string());
    const std::string installedVersion = manifest.value("installedVersion", std::string());
    const std::string targetVersion = manifest.value("targetVersion", std::string());
    const std::string packageId = manifest.value("packageId", std::string());

    json result = {
        { "success", false },
        { "applicationName", applicationName },
        { "packageId", packageId },
        { "provider", provider },
        { "installedVersion", installedVersion },
        { "targetVersion", targetVersion },
        { "rebootRequired", false },
        { "verificationPassed", false },
        { "fallbackUsed", false },
        { "sha256Verified", false },
        { "signatureVerified", false },
        { "serializedWaitMs", serializedWaitMs },
        { "jobWorkDir", WideToUtf8(root.wstring()) },
        { "capabilities", Capabilities() }
    };

    CommandResult install;
    bool directAttempted = false;

    if (provider == "vendor_direct") {
        directAttempted = true;
        const std::string installerType = Lower(manifest.value("installerType", std::string()));
        const std::filesystem::path installerPath = root / (std::wstring(L"installer.") + Utf8ToWide(installerType));
        std::error_code ec;
        std::filesystem::remove(installerPath, ec);

        if (!DownloadHttps(manifest.value("downloadUrl", std::string()), installerPath)) {
            result["error"] = "vendor_download_failed";
        } else {
            const std::string actualSha = Sha256File(installerPath);
            const std::string expectedSha = manifest.value("sha256", std::string());
            result["actualSha256"] = actualSha;
            result["sha256Verified"] = !actualSha.empty() && Lower(actualSha) == Lower(expectedSha);

            if (!result["sha256Verified"].get<bool>()) {
                result["error"] = "sha256_mismatch";
            } else {
                std::string signer;
                if (!VerifyAuthenticodeTrust(installerPath, signer)) {
                    result["error"] = "authenticode_invalid";
                } else {
                    result["signer"] = signer;
                    const std::string expectedSigner = manifest.value("expectedSigner", std::string());
                    result["signatureVerified"] = !signer.empty() && ContainsInsensitive(signer, expectedSigner);
                    if (!result["signatureVerified"].get<bool>()) {
                        result["error"] = "unexpected_signer";
                    } else {
                    std::wstring command;
                    if (installerType == "msi") {
                        command = L"msiexec.exe /i " + Quote(installerPath.wstring()) + L" /qn /norestart";
                    } else {
                        const std::string args = manifest.value("installArguments", std::string("/quiet /norestart"));
                        command = Quote(installerPath.wstring()) + L" " + Utf8ToWide(args);
                    }
                    install = RunHidden(command, root / L"vendor-install.log", 30 * 60 * 1000);
                    result["exitCode"] = install.exitCode;
                    result["installerOutput"] = Truncate(install.output, 4000);
                    result["rebootRequired"] = install.exitCode == 3010 || install.exitCode == 1641;
                    }
                }
            }
        }

        std::filesystem::remove(installerPath, ec);
    } else {
        install = RunWingetUpgrade(manifest, root);
        result["exitCode"] = install.exitCode;
        result["installerOutput"] = Truncate(install.output, 4000);
        result["rebootRequired"] = install.exitCode == 3010 || install.exitCode == 1641;
    }

    const bool installerSucceeded =
        install.exitCode == 0 || install.exitCode == 3010 || install.exitCode == 1641;
    json verification = VerifyInstalledVersionWithRetry(manifest, root, installerSucceeded);
    result["verifiedVersion"] = verification.value("installedVersion", std::string());
    result["verification"] = verification;
    result["verificationPassed"] = verification.value("meetsTarget", false);

    if ((!installerSucceeded || !result["verificationPassed"].get<bool>())
        && directAttempted
        && manifest.value("fallbackProvider", std::string()) == "winget") {
        result["fallbackUsed"] = true;
        const CommandResult fallback = RunWingetUpgrade(manifest, root, L"winget-fallback.log");
        result["fallbackExitCode"] = fallback.exitCode;
        result["fallbackOutput"] = Truncate(fallback.output, 3000);
        result["rebootRequired"] = result["rebootRequired"].get<bool>() || fallback.exitCode == 3010 || fallback.exitCode == 1641;
        const bool fallbackSucceeded = fallback.exitCode == 0 || fallback.exitCode == 3010 || fallback.exitCode == 1641;
        verification = VerifyInstalledVersionWithRetry(manifest, root, fallbackSucceeded);
        result["verifiedVersion"] = verification.value("installedVersion", std::string());
        result["verification"] = verification;
        result["verificationPassed"] = verification.value("meetsTarget", false);
        if (fallbackSucceeded) install = fallback;
    }

    const bool finalInstallerSucceeded =
        install.exitCode == 0 || install.exitCode == 3010 || install.exitCode == 1641;
    const bool success = finalInstallerSucceeded && result["verificationPassed"].get<bool>();
    result["success"] = success;

    if (!success) {
        result["verificationFailed"] = !result["verificationPassed"].get<bool>();
        if (!result.contains("error")) {
            result["error"] = finalInstallerSucceeded ? "target_version_not_verified" : "installer_failed";
        }
    }

    return result;
}
std::wstring ArgValue(int argc, wchar_t** argv, const std::wstring& name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) return argv[index + 1];
    }
    return L"";
}

bool HasArg(int argc, wchar_t** argv, const std::wstring& name) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == name) return true;
    }
    return false;
}

int Emit(const json& payload, const std::wstring& outputPath) {
    const std::string serialized = payload.dump(2);
    if (!outputPath.empty()) {
        if (!WriteFileUtf8(outputPath, serialized + "\n")) return 3;
    } else {
        std::cout << serialized << std::endl;
    }
    return payload.value("success", true) ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring output = ArgValue(argc, argv, L"--output");

    if (HasArg(argc, argv, L"--version")) {
        std::cout << kPatchHostVersion << std::endl;
        return 0;
    }

    if (HasArg(argc, argv, L"--capabilities")) {
        return Emit({ { "success", true }, { "capabilities", Capabilities() } }, output);
    }

    if (HasArg(argc, argv, L"--discover-software")) {
        return Emit(DiscoverWinget(), output);
    }

    const std::wstring manifest = ArgValue(argc, argv, L"--execute-manifest");
    if (!manifest.empty()) {
        return Emit(ExecuteManifest(manifest), output);
    }

    return Emit({
        { "success", false },
        { "error", "invalid_arguments" },
        { "usage", "Hi5CentralPatchHost.exe --capabilities|--discover-software|--execute-manifest <path> [--output <path>]" },
        { "capabilities", Capabilities() }
    }, output);
}
