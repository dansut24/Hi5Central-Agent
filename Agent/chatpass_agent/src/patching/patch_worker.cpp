#include "patch_worker.h"
#include "../util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winhttp.h>
#include <urlmon.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")

using json = nlohmann::json;

namespace hi5 {
namespace {

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::filesystem::path ProgramDataRoot() {
    return std::filesystem::path(LR"(C:\ProgramData\Hi5Central\Agent)");
}

std::wstring PatchCacheDir() {
    std::filesystem::path dir = ProgramDataRoot() / L"PatchCache";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::filesystem::path PatchActionsDir() {
    std::filesystem::path dir = ProgramDataRoot() / L"PatchActions";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path LogsDir() {
    std::filesystem::path dir = ProgramDataRoot() / L"Logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path PatchStatusPath() {
    return LogsDir() / L"PatchStatus.txt";
}

std::string NowLocalText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64]{};
    sprintf_s(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void WritePatchStatus(const std::string& line) {
    try {
        std::ofstream out(PatchStatusPath(), std::ios::app | std::ios::binary);
        out << NowLocalText() << " " << line << "\r\n";
    } catch (...) {
        // Do not let diagnostics break patching.
    }
}

std::string TruncateForReport(const std::string& value, size_t maxLen = 6000) {
    if (value.size() <= maxLen) return value;
    return value.substr(0, maxLen) + "\n...[truncated]";
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ContainsInsensitive(const std::string& haystack, const std::string& needle) {
    return Lower(haystack).find(Lower(needle)) != std::string::npos;
}

bool EndsWithInsensitive(const std::string& value, const std::string& suffix) {
    const std::string v = Lower(value);
    const std::string s = Lower(suffix);
    return v.size() >= s.size() && v.compare(v.size() - s.size(), s.size(), s) == 0;
}

std::string StripAnsiAndProgressNoise(const std::string& input) {
    std::string out;
    out.reserve(input.size());

    bool inEsc = false;
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (inEsc) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) inEsc = false;
            continue;
        }
        if (c == 0x1b) {
            inEsc = true;
            continue;
        }
        if (c == '\r') {
            out.push_back('\n');
            continue;
        }
        if (c == 0 || c == '\b') continue;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::vector<int> ParseVersionParts(const std::string& version) {
    std::vector<int> parts;
    std::string current;
    for (char ch : version) {
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

int CompareVersionsText(const std::string& installed, const std::string& target) {
    const auto a = ParseVersionParts(installed);
    const auto b = ParseVersionParts(target);
    if (a.empty() || b.empty()) return 0;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

std::string ExtractWingetListedVersion(const std::string& output, const std::string& wingetId) {
    const std::string clean = StripAnsiAndProgressNoise(output);
    std::istringstream in(clean);
    std::string line;
    const std::string idLower = Lower(wingetId);

    while (std::getline(in, line)) {
        if (!ContainsInsensitive(line, wingetId)) continue;
        if (ContainsInsensitive(line, "Command=") || ContainsInsensitive(line, "--id")) continue;

        std::vector<std::string> tokens;
        std::istringstream ls(line);
        std::string token;
        while (ls >> token) tokens.push_back(token);

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (Lower(tokens[i]) == idLower && i + 1 < tokens.size()) {
                return tokens[i + 1];
            }
        }
    }
    return "";
}

bool OutputMeansNoUpgrade(const std::string& output) {
    return ContainsInsensitive(output, "No available upgrade found") ||
           ContainsInsensitive(output, "No newer package versions are available") ||
           ContainsInsensitive(output, "No applicable update found");
}

bool VersionMeetsTarget(const std::string& installed, const std::string& target) {
    if (target.empty() || installed.empty()) return false;
    return CompareVersionsText(installed, target) >= 0;
}

std::string ReplaceAll(std::string input, const std::string& from, const std::string& to) {
    if (from.empty()) return input;
    size_t pos = 0;
    while ((pos = input.find(from, pos)) != std::string::npos) {
        input.replace(pos, from.size(), to);
        pos += to.size();
    }
    return input;
}

std::string QuoteForCmd(const std::string& value) {
    std::string escaped = value;
    escaped = ReplaceAll(escaped, "\"", "\\\"");
    return "\"" + escaped + "\"";
}

std::wstring SanitizeFilePart(const std::string& input) {
    std::wstring wide = Utf8ToWide(input.empty() ? "task" : input);
    for (wchar_t& c : wide) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'\"' || c == L'<' || c == L'>' || c == L'|' || c < 32) {
            c = L'_';
        }
    }
    return wide;
}

std::string ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string FileSizeText(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return "unknown";
    return std::to_string(static_cast<unsigned long long>(size)) + " bytes";
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            char hex[4];
            sprintf_s(hex, "%%%02X", c);
            out << hex;
        }
    }
    return out.str();
}

struct ParsedUrl {
    bool https = true;
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
};

ParsedUrl ParseUrl(const std::string& url) {
    std::wstring wide = Utf8ToWide(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[512]{};
    wchar_t path[4096]{};

    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    uc.dwSchemeLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &uc)) {
        throw std::runtime_error("WinHttpCrackUrl failed");
    }

    ParsedUrl result;
    result.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    result.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    result.port = uc.nPort;
    result.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) {
        result.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    if (result.path.empty()) result.path = L"/";
    return result;
}

std::string HttpRequest(const std::string& method, const std::string& url, const std::string& body, const std::string& apiKey) {
    ParsedUrl parsed = ParseUrl(url);

    HINTERNET session = WinHttpOpen(
        L"Hi5Central-Agent-PatchWorker/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");

    HINTERNET connect = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpConnect failed");
    }

    DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect,
        Utf8ToWide(method).c_str(),
        parsed.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!apiKey.empty()) headers += L"x-api-key: " + Utf8ToWide(apiKey) + L"\r\n";

    BOOL sent = WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttp request failed");
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

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

    if (status < 200 || status >= 300) throw std::runtime_error("HTTP " + std::to_string(status) + ": " + response);
    return response;
}

std::wstring ExpandEnv(const std::wstring& value) {
    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) return value;
    std::wstring out(needed, L'\0');
    ExpandEnvironmentStringsW(value.c_str(), out.data(), needed);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring FindWingetExe() {
    const std::vector<std::wstring> directCandidates = {
        ExpandEnv(LR"(%LOCALAPPDATA%\Microsoft\WindowsApps\winget.exe)"),
        ExpandEnv(LR"(%ProgramFiles%\WindowsApps\Microsoft.DesktopAppInstaller_8wekyb3d8bbwe\winget.exe)"),
        L"winget.exe"
    };

    for (const auto& candidate : directCandidates) {
        if (candidate == L"winget.exe") continue;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }

    wchar_t programFiles[MAX_PATH]{};
    DWORD got = GetEnvironmentVariableW(L"ProgramFiles", programFiles, static_cast<DWORD>(std::size(programFiles)));
    if (got > 0) {
        std::filesystem::path apps = std::filesystem::path(programFiles) / L"WindowsApps";
        WIN32_FIND_DATAW fd{};
        std::wstring pattern = (apps / L"Microsoft.DesktopAppInstaller_*_x64__8wekyb3d8bbwe").wstring();
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        std::wstring best;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    std::filesystem::path p = apps / fd.cFileName / L"winget.exe";
                    std::error_code ec;
                    if (std::filesystem::exists(p, ec)) {
                        const std::wstring s = p.wstring();
                        if (s > best) best = s;
                    }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!best.empty()) return best;
    }

    return L"winget.exe";
}

std::string BuildWingetUpgradeCommand(const std::string& wingetId) {
    const std::string winget = QuoteForCmd(WideToUtf8(FindWingetExe()));
    return winget + " upgrade --id " + QuoteForCmd(wingetId) + " --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity";
}

std::string BuildWingetListCommand(const std::string& wingetId) {
    const std::string winget = QuoteForCmd(WideToUtf8(FindWingetExe()));
    return winget + " list --id " + QuoteForCmd(wingetId) + " --exact --accept-source-agreements --disable-interactivity";
}

std::string NormalizeWingetCommand(std::string command) {
    std::string trimmed = command;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());

    if (ContainsInsensitive(trimmed, "winget ")) {
        const std::string winget = QuoteForCmd(WideToUtf8(FindWingetExe()));
        const std::string lower = Lower(trimmed);
        size_t pos = lower.find("winget");
        if (pos != std::string::npos) trimmed.replace(pos, 6, winget);

        if (!ContainsInsensitive(trimmed, "--exact")) trimmed += " --exact";
        if (!ContainsInsensitive(trimmed, "--accept-package-agreements")) trimmed += " --accept-package-agreements";
        if (!ContainsInsensitive(trimmed, "--accept-source-agreements")) trimmed += " --accept-source-agreements";
        if (!ContainsInsensitive(trimmed, "--disable-interactivity")) trimmed += " --disable-interactivity";
    }
    return trimmed;
}

std::string BuildInstallerCommand(
    const std::string& taskId,
    const std::string& softwareName,
    const std::string& localFileName,
    const std::filesystem::path& localPath,
    const std::string& requestedCommand) {

    const std::string localPathText = WideToUtf8(localPath.wstring());
    const std::string quotedLocalPath = QuoteForCmd(localPathText);
    const std::string lowerName = Lower(softwareName + " " + localFileName + " " + requestedCommand);

    if (EndsWithInsensitive(localPathText, ".msi")) {
        const std::filesystem::path msiLog = PatchActionsDir() / (SanitizeFilePart(taskId) + L"-msi.log");
        return "msiexec.exe /i " + quotedLocalPath + " /qn /norestart /L*v " + QuoteForCmd(WideToUtf8(msiLog.wstring()));
    }

    if (EndsWithInsensitive(localPathText, ".msu")) {
        return "wusa.exe " + quotedLocalPath + " /quiet /norestart";
    }

    if (EndsWithInsensitive(localPathText, ".exe")) {
        if (ContainsInsensitive(lowerName, "chrome")) return quotedLocalPath + " /silent /install";
        if (ContainsInsensitive(lowerName, "edge")) return quotedLocalPath + " /silent /install";
        if (ContainsInsensitive(requestedCommand, "{file}")) return ReplaceAll(requestedCommand, "{file}", quotedLocalPath);
        if (!requestedCommand.empty() && ContainsInsensitive(requestedCommand, localFileName)) {
            std::string cmd = requestedCommand;
            cmd = ReplaceAll(cmd, "\"" + localFileName + "\"", quotedLocalPath);
            cmd = ReplaceAll(cmd, localFileName, quotedLocalPath);
            return NormalizeWingetCommand(cmd);
        }
        return quotedLocalPath + " /quiet /norestart";
    }

    if (ContainsInsensitive(requestedCommand, "{file}")) return ReplaceAll(requestedCommand, "{file}", quotedLocalPath);
    if (!requestedCommand.empty() && ContainsInsensitive(requestedCommand, localFileName)) {
        std::string cmd = requestedCommand;
        cmd = ReplaceAll(cmd, "\"" + localFileName + "\"", quotedLocalPath);
        cmd = ReplaceAll(cmd, localFileName, quotedLocalPath);
        return NormalizeWingetCommand(cmd);
    }

    return quotedLocalPath;
}

} // namespace

PatchWorker::PatchWorker() = default;

PatchWorker::~PatchWorker() {
    Stop();
}

void PatchWorker::Start(PatchWorkerConfig config) {
    if (!config.enabled) {
        LogInfo("[patch] disabled");
        return;
    }
    if (config.deviceId.empty()) {
        LogWarn("[patch] not started because deviceId is empty");
        return;
    }
    if (thread_.joinable()) return;

    config_ = std::move(config);
    stop_.store(false);
    thread_ = std::thread([this]() { Run(); });

    LogInfo("[patch] worker started device_id=" + config_.deviceId);
    WritePatchStatus("[worker] started device_id=" + config_.deviceId + " apiBaseUrl=" + config_.apiBaseUrl);
}

void PatchWorker::Stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    LogInfo("[patch] worker stopped");
    WritePatchStatus("[worker] stopped");
}

void PatchWorker::Run() {
    while (!stop_.load()) {
        try {
            PollOnce();
        } catch (const std::exception& ex) {
            LogWarn(std::string("[patch] poll failed: ") + ex.what());
            WritePatchStatus(std::string("[poll] failed: ") + ex.what());
        }

        const int sleepSeconds = config_.pollSeconds > 0 ? config_.pollSeconds : 30;
        for (int i = 0; i < sleepSeconds && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void PatchWorker::PollOnce() {
    const std::string url = config_.apiBaseUrl + "/api/device/tasks?externalDeviceId=" + UrlEncode(config_.deviceId) + "&markRunning=true";
    WritePatchStatus("[poll] GET " + url);
    const std::string response = HttpRequest("GET", url, "", config_.apiKey);
    WritePatchStatus("[poll] response bytes=" + std::to_string(response.size()));

    const json root = json::parse(response, nullptr, false);
    if (root.is_discarded() || !root.value("ok", false)) {
        LogWarn("[patch] invalid tasks response");
        WritePatchStatus("[poll] invalid tasks response=" + TruncateForReport(response, 2000));
        return;
    }

    const auto tasks = root.value("tasks", json::array());
    WritePatchStatus("[poll] tasks=" + std::to_string(tasks.size()));

    for (const auto& task : tasks) {
        const std::string status = task.value("status", "");
        const std::string step = task.value("current_step", "");
        if (status == "running" && (step == "picked_up" || step == "queued" || step == "running")) ExecuteTask(task);
    }
}

void PatchWorker::ExecuteTask(const json& task) {
    const std::string taskId = task.value("id", "");
    const std::string softwareName = task.value("software_name", "Unknown software");
    const std::string targetVersion = task.value("target_version", "");
    if (taskId.empty()) return;

    try {
        WritePatchStatus("[task=" + taskId + "] raw_task=" + TruncateForReport(task.dump(), 4000));
        LogInfo("[patch] executing task=" + taskId + " software=" + softwareName);
        WritePatchStatus("[task=" + taskId + "] executing software=" + softwareName);

        const json execution = task.value("execution", json::object());
        const std::string executionType = execution.value("executionType", "");
        const std::string downloadUrl = execution.value("downloadUrl", "");
        const std::string localFileName = execution.value("localFileName", "installer.bin");
        const std::string wingetId = task.value("winget_id", "");
        std::string installCommand = execution.value("installCommand", task.value("command", ""));

        WritePatchStatus("[task=" + taskId + "] executionType=" + executionType);
        WritePatchStatus("[task=" + taskId + "] downloadUrl=" + downloadUrl);
        WritePatchStatus("[task=" + taskId + "] localFileName=" + localFileName);
        WritePatchStatus("[task=" + taskId + "] wingetId=" + wingetId);
        WritePatchStatus("[task=" + taskId + "] initial installCommand=" + installCommand);

        const std::wstring cacheDir = PatchCacheDir();
        std::filesystem::path localPath;

        if (executionType == "download_and_install") {
            if (downloadUrl.empty()) throw std::runtime_error("downloadUrl is empty");

            localPath = std::filesystem::path(cacheDir) / Utf8ToWide(localFileName);
            Report(taskId, "running", "downloading", 10, "Downloading installer");
            WritePatchStatus("[task=" + taskId + "] download target=" + WideToUtf8(localPath.wstring()));

            if (!DownloadFile(downloadUrl, localPath.wstring())) throw std::runtime_error("download failed");

            WritePatchStatus("[task=" + taskId + "] download complete file_size=" + FileSizeText(localPath));
            Report(taskId, "running", "downloaded", 40, "Installer downloaded");

            installCommand = BuildInstallerCommand(taskId, softwareName, localFileName, localPath, installCommand);
        } else if (executionType == "winget") {
            if (!wingetId.empty()) installCommand = BuildWingetUpgradeCommand(wingetId);
            else installCommand = NormalizeWingetCommand(installCommand);
        } else {
            installCommand = NormalizeWingetCommand(installCommand);
        }

        if (installCommand.empty()) throw std::runtime_error("install command is empty");

        WritePatchStatus("[task=" + taskId + "] final installCommand=" + installCommand);
        LogInfo("[patch] install command task=" + taskId + " command=" + installCommand);
        Report(taskId, "running", "installing", 70, "Installing package");

        CommandResult result = RunCommand(taskId, installCommand, cacheDir);

        // If this is an MSI install, also pull the verbose MSI log into the
        // command output/status log. This is where Windows Installer explains
        // the real reason for failures such as exit code 1/1603/1618.
        if (executionType == "download_and_install" && EndsWithInsensitive(WideToUtf8(localPath.wstring()), ".msi")) {
            const std::filesystem::path msiLog = PatchActionsDir() / (SanitizeFilePart(taskId) + L"-msi.log");
            const std::string msiText = ReadFileUtf8(msiLog);
            WritePatchStatus("[task=" + taskId + "] msiLogPath=" + WideToUtf8(msiLog.wstring()));
            if (!msiText.empty()) {
                WritePatchStatus("[task=" + taskId + "] msiLogTail=" + TruncateForReport(msiText.substr(msiText.size() > 6000 ? msiText.size() - 6000 : 0)));
                result.output += "\n\n[MSI log tail]\n";
                result.output += msiText.substr(msiText.size() > 6000 ? msiText.size() - 6000 : 0);
            } else {
                WritePatchStatus("[task=" + taskId + "] msiLogTail=<empty-or-not-created>");
            }
        }

        WritePatchStatus("[task=" + taskId + "] installer exitCode=" + std::to_string(result.exitCode));
        WritePatchStatus("[task=" + taskId + "] installer output=" + TruncateForReport(result.output));

        bool usedWingetFallback = false;
        if (result.exitCode != 0 && result.exitCode != 3010 && result.exitCode != 1641 && executionType == "download_and_install" && !wingetId.empty()) {
            const std::string fallback = BuildWingetUpgradeCommand(wingetId);
            usedWingetFallback = true;
            WritePatchStatus("[task=" + taskId + "] direct installer failed, trying winget fallback=" + fallback);
            Report(taskId, "running", "winget_fallback", 85, "Direct installer failed. Trying WinGet fallback.", result.exitCode, false);
            result = RunCommand(taskId + "_winget", fallback, cacheDir);
            WritePatchStatus("[task=" + taskId + "] winget fallback exitCode=" + std::to_string(result.exitCode));
            WritePatchStatus("[task=" + taskId + "] winget fallback output=" + TruncateForReport(result.output));
        }

        std::string verifyOutput;
        int verifyExit = -1;
        if (!wingetId.empty()) {
            const std::string verifyCommand = BuildWingetListCommand(wingetId);
            CommandResult verify = RunCommand(taskId + "_verify", verifyCommand, cacheDir);
            verifyOutput = verify.output;
            verifyExit = verify.exitCode;
            WritePatchStatus("[task=" + taskId + "] post-install verify command=" + verifyCommand);
            WritePatchStatus("[task=" + taskId + "] post-install verify exitCode=" + std::to_string(verifyExit));
            WritePatchStatus("[task=" + taskId + "] post-install verify output=" + TruncateForReport(verifyOutput));
        }

        const std::string verifiedVersion = !wingetId.empty() ? ExtractWingetListedVersion(verifyOutput, wingetId) : "";
        if (!verifiedVersion.empty()) {
            WritePatchStatus("[task=" + taskId + "] verifiedVersion=" + verifiedVersion + " targetVersion=" + targetVersion);
        }

        bool installerSucceeded = (result.exitCode == 0 || result.exitCode == 3010 || result.exitCode == 1641);

        // Important: some vendor installers, especially evergreen browser MSIs, can return
        // success after repairing/reconfiguring the existing product without actually moving
        // the product to the catalogue target version. Treat that as not complete and try the
        // WinGet path if available.
        if (installerSucceeded && !targetVersion.empty() && !verifiedVersion.empty() && !VersionMeetsTarget(verifiedVersion, targetVersion) && executionType == "download_and_install" && !wingetId.empty() && !usedWingetFallback) {
            const std::string fallback = BuildWingetUpgradeCommand(wingetId);
            usedWingetFallback = true;
            WritePatchStatus("[task=" + taskId + "] direct installer exitCode=0 but verifiedVersion=" + verifiedVersion + " is below targetVersion=" + targetVersion + "; trying winget fallback=" + fallback);
            Report(taskId, "running", "winget_fallback", 85, "Installer completed but version did not change to target. Trying WinGet fallback.", result.exitCode, false);
            result = RunCommand(taskId + "_winget", fallback, cacheDir);
            WritePatchStatus("[task=" + taskId + "] winget fallback exitCode=" + std::to_string(result.exitCode));
            WritePatchStatus("[task=" + taskId + "] winget fallback output=" + TruncateForReport(result.output));

            if (!wingetId.empty()) {
                const std::string verifyCommand = BuildWingetListCommand(wingetId);
                CommandResult verify = RunCommand(taskId + "_verify_after_fallback", verifyCommand, cacheDir);
                verifyOutput = verify.output;
                verifyExit = verify.exitCode;
                WritePatchStatus("[task=" + taskId + "] post-fallback verify command=" + verifyCommand);
                WritePatchStatus("[task=" + taskId + "] post-fallback verify exitCode=" + std::to_string(verifyExit));
                WritePatchStatus("[task=" + taskId + "] post-fallback verify output=" + TruncateForReport(verifyOutput));
            }
        }

        const std::string finalVerifiedVersion = !wingetId.empty() ? ExtractWingetListedVersion(verifyOutput, wingetId) : "";
        const bool finalMeetsTarget = targetVersion.empty() || finalVerifiedVersion.empty() || VersionMeetsTarget(finalVerifiedVersion, targetVersion);

        if ((result.exitCode == 0 || result.exitCode == 3010 || result.exitCode == 1641) && finalMeetsTarget) {
            const bool rebootRequired = result.exitCode == 3010 || result.exitCode == 1641;
            std::string msg = rebootRequired ? "Patch installed successfully. Reboot required." : "Patch installed successfully.";
            if (usedWingetFallback) msg += " Direct installer path did not reach target; WinGet fallback succeeded.";
            if (!finalVerifiedVersion.empty()) msg += " Verified version: " + finalVerifiedVersion + ".";
            if (!verifyOutput.empty()) msg += "\n\nVerification:\n" + TruncateForReport(verifyOutput, 2000);
            Report(taskId, "completed", "completed", 100, msg, result.exitCode, rebootRequired);
            WritePatchStatus("[task=" + taskId + "] completed exitCode=" + std::to_string(result.exitCode));
        } else {
            std::string msg;
            if (!targetVersion.empty() && !finalVerifiedVersion.empty() && !VersionMeetsTarget(finalVerifiedVersion, targetVersion)) {
                msg = "Patch did not reach target version. Verified version is " + finalVerifiedVersion + ", target is " + targetVersion + ".";
                if (OutputMeansNoUpgrade(result.output)) {
                    msg += " WinGet reports no newer package from configured sources, so the catalogue/source target is ahead of what this endpoint can install.";
                }
            } else if (OutputMeansNoUpgrade(result.output)) {
                msg = "WinGet reports no available upgrade from configured sources.";
            } else {
                msg = "Patch installer failed with exit code " + std::to_string(result.exitCode);
            }
            if (!result.output.empty()) msg += "\n\nOutput:\n" + TruncateForReport(result.output, 2500);
            if (!verifyOutput.empty()) msg += "\n\nVerification:\n" + TruncateForReport(verifyOutput, 1500);
            Report(taskId, "failed", "failed", 100, msg, result.exitCode, false);
            WritePatchStatus("[task=" + taskId + "] failed exitCode=" + std::to_string(result.exitCode));
        }
    } catch (const std::exception& ex) {
        LogError(std::string("[patch] task failed: ") + ex.what());
        WritePatchStatus("[task=" + taskId + "] exception=" + std::string(ex.what()));
        Report(taskId, "failed", "failed", 100, ex.what(), -1, false);
    }
}

bool PatchWorker::DownloadFile(const std::string& url, const std::wstring& outputPath) {
    WritePatchStatus("[download] url=" + url + " output=" + WideToUtf8(outputPath));
    HRESULT hr = URLDownloadToFileW(nullptr, Utf8ToWide(url).c_str(), outputPath.c_str(), 0, nullptr);
    if (FAILED(hr)) WritePatchStatus("[download] failed hr=" + std::to_string(static_cast<long>(hr)));
    return SUCCEEDED(hr);
}

PatchWorker::CommandResult PatchWorker::RunCommand(const std::string& taskId, const std::string& command, const std::wstring& workingDir) {
    std::filesystem::path actions = PatchActionsDir();
    std::filesystem::path script = actions / (SanitizeFilePart(taskId) + L".cmd");
    std::filesystem::path output = actions / (SanitizeFilePart(taskId) + L".out.txt");

    {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        out << "@echo off\r\n";
        out << "setlocal EnableExtensions\r\n";
        out << "chcp 65001 >nul\r\n";
        out << "set PATH=%SystemRoot%\\System32;%SystemRoot%;%SystemRoot%\\System32\\WindowsPowerShell\\v1.0;%ProgramFiles%\\WindowsApps;%LOCALAPPDATA%\\Microsoft\\WindowsApps;%PATH%\r\n";
        out << "echo [Hi5CentralPatch] WorkingDir=%CD%\r\n";
        out << "echo [Hi5CentralPatch] Command=" << command << "\r\n";
        out << command << "\r\n";
        out << "set HI5_EXIT=%ERRORLEVEL%\r\n";
        out << "echo [Hi5CentralPatch] ExitCode=%HI5_EXIT%\r\n";
        out << "exit /b %HI5_EXIT%\r\n";
    }

    // Run through cmd.exe and redirect inside the /c payload. The previous
    // form placed the redirection outside the quoted script path, which could
    // fail very early and leave an empty .out.txt even though the command
    // returned exit code 1.
    const std::string scriptText = WideToUtf8(script.wstring());
    const std::string outputText = WideToUtf8(output.wstring());
    std::string shellCommand =
        "cmd.exe /d /c \"call " + QuoteForCmd(scriptText) +
        " > " + QuoteForCmd(outputText) + " 2>&1\"";
    std::wstring fullCommand = Utf8ToWide(shellCommand);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCommand = fullCommand;

    WritePatchStatus("[command task=" + taskId + "] script=" + WideToUtf8(script.wstring()));
    WritePatchStatus("[command task=" + taskId + "] output=" + WideToUtf8(output.wstring()));
    WritePatchStatus("[command task=" + taskId + "] command=" + command);
    WritePatchStatus("[command task=" + taskId + "] shell=" + shellCommand);

    BOOL ok = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si,
        &pi);

    if (!ok) {
        DWORD err = GetLastError();
        throw std::runtime_error("CreateProcessW failed: " + std::to_string(err));
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    CommandResult result;
    result.exitCode = static_cast<int>(exitCode);
    result.scriptPath = script.wstring();
    result.outputPath = output.wstring();
    result.output = ReadFileUtf8(output);
    if (result.output.empty()) {
        result.output =
            "[Hi5CentralPatch] Output file was empty.\r\n"
            "[Hi5CentralPatch] This usually means the process failed before stdout/stderr was attached, "
            "or the target executable could not start in the service/SYSTEM context.\r\n"
            "[Hi5CentralPatch] Script=" + WideToUtf8(script.wstring()) + "\r\n"
            "[Hi5CentralPatch] Shell=" + shellCommand + "\r\n"
            "[Hi5CentralPatch] Command=" + command + "\r\n";
    }
    return result;
}

void PatchWorker::Report(
    const std::string& taskId,
    const std::string& status,
    const std::string& currentStep,
    int progress,
    const std::string& message,
    int exitCode,
    bool rebootRequired) {

    json body = {
        {"taskId", taskId},
        {"status", status},
        {"currentStep", currentStep},
        {"progress", progress},
        {"resultMessage", message},
        {"rebootRequired", rebootRequired}
    };
    if (exitCode >= 0) body["exitCode"] = exitCode;

    WritePatchStatus("[task=" + taskId + "] report status=" + status + " step=" + currentStep + " progress=" + std::to_string(progress) + " message=" + TruncateForReport(message, 1000));

    try {
        const std::string response = HttpRequest("POST", config_.apiBaseUrl + "/api/device/tasks/report", body.dump(), config_.apiKey);
        WritePatchStatus("[task=" + taskId + "] report ok response=" + TruncateForReport(response, 2500));
    } catch (const std::exception& ex) {
        LogWarn(std::string("[patch] report failed: ") + ex.what());
        WritePatchStatus("[task=" + taskId + "] report failed: " + std::string(ex.what()));
    }
}

} // namespace hi5
