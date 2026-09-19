#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr const char* kPatchHostVersion = "0.1.0";

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, output.data(), size);
    while (!output.empty() && output.back() == L'\0') output.pop_back();
    return output;
}
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
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

std::string ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return "";
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
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

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
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
};

CommandResult RunHidden(const std::wstring& command, const std::filesystem::path& outputPath) {
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
        return { static_cast<int>(GetLastError()), "CreateProcessW failed" };
    }

    WaitForSingleObject(process.hProcess, 10 * 60 * 1000);

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) exitCode = 1;
    if (exitCode == STILL_ACTIVE) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        exitCode = ERROR_TIMEOUT;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    return { static_cast<int>(exitCode), ReadFileUtf8(outputPath) };
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

WingetListRow FindWingetListRow(const std::string& output, const std::string& packageId) {
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
                const auto tail = Tokens(line.substr(end));
                if (!tail.empty()) row.installedVersion = tail[0];
                if (tail.size() >= 2) row.source = tail.back();
                if (tail.size() >= 3) row.availableVersion = tail[1];
                return row;
            }
            pos = lowerLine.find(needle, pos + 1);
        }
    }

    return {};
}
json Capabilities() {
    return {
        { "patchHostVersion", kPatchHostVersion },
        { "protocolVersion", 1 },
        { "softwareDiscovery", true },
        { "softwareInstall", false },
        { "windowsUpdateDiscovery", false },
        { "windowsUpdateInstall", false },
        { "providers", json::array({ "winget" }) },
        { "manifestMode", "job_scoped_only" },
        { "catalogueOnDevice", false }
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
            { "detail", exported.output },
            { "capabilities", Capabilities() }
        };
    }
    const std::string exportJson = ReadFileUtf8(exportPath);
    const json rootJson = json::parse(exportJson, nullptr, false);
    if (rootJson.is_discarded()) {
        return {
            { "success", false },
            { "error", "winget_export_invalid_json" },
            { "capabilities", Capabilities() }
        };
    }

    const std::wstring listCommand =
        Quote(winget) +
        L" list --source winget --accept-source-agreements --disable-interactivity --nowarn";
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

            const std::string exportedVersion = package.value("Version", "");
            const WingetListRow row = FindWingetListRow(listed.output, packageId);
            packages.push_back({
                { "packageId", packageId },
                { "name", row.name.empty() ? packageId : row.name },
                { "publisher", "" },
                { "installedVersion", exportedVersion.empty() ? row.installedVersion : exportedVersion },
                { "availableVersion", row.availableVersion },
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
        return Emit({
            { "success", true },
            { "capabilities", Capabilities() }
        }, output);
    }

    if (HasArg(argc, argv, L"--discover-software")) {
        return Emit(DiscoverWinget(), output);
    }

    return Emit({
        { "success", false },
        { "error", "invalid_arguments" },
        { "usage", "Hi5CentralPatchHost.exe --capabilities|--discover-software [--output path]" },
        { "capabilities", Capabilities() }
    }, output);
}
