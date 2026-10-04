#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#define IDR_AGENT_SETUP 101

#pragma pack(push, 1)
struct Hi5EmbeddedDeploymentConfig {
    char magic[24];
    char deploymentId[64];
    char deploymentSecret[128];
    char apiBase[256];
    char reserved[40];
};
#pragma pack(pop)

static_assert(sizeof(Hi5EmbeddedDeploymentConfig) == 512, "Deployment config block must remain exactly 512 bytes.");

#ifdef _MSC_VER
#pragma section(".h5cfg", read)
#define HI5_CFG_SECTION __declspec(allocate(".h5cfg"))
#else
#define HI5_CFG_SECTION __attribute__((section(".h5cfg"), used))
#endif

extern "C" HI5_CFG_SECTION const volatile Hi5EmbeddedDeploymentConfig gHi5DeploymentConfig = {
    { 'H','5','C','0','F','9','A','1','7','D','4','2','B','6','E','3' },
    "__TEMPLATE__",
    "__TEMPLATE__",
    "https://api.hi5central.com",
    ""
};

namespace {
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring CommandLineOption(const std::wstring& name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};

    std::wstring value;
    const std::wstring prefix = name + L"=";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i] ? argv[i] : L"";
        if (_wcsicmp(arg.c_str(), name.c_str()) == 0 && i + 1 < argc) {
            value = argv[++i] ? argv[i] : L"";
            break;
        }
        if (arg.size() > prefix.size() &&
            _wcsnicmp(arg.c_str(), prefix.c_str(), prefix.size()) == 0) {
            value = arg.substr(prefix.size());
            break;
        }
    }

    LocalFree(argv);
    return Trim(value);
}

std::string FixedField(const volatile char* value, size_t capacity) {
    size_t length = 0;
    while (length < capacity && value[length] != '\0') ++length;
    return std::string(const_cast<const char*>(value), length);
}

bool IsQuiet(const std::wstring& commandLine) {
    std::wstring lower = commandLine;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower.find(L"--quiet") != std::wstring::npos ||
           lower.find(L"/quiet") != std::wstring::npos ||
           lower.find(L"/qn") != std::wstring::npos ||
           lower.find(L"/silent") != std::wstring::npos;
}

std::wstring Quote(const std::wstring& value) {
    std::wstring escaped = value;
    size_t pos = 0;
    while ((pos = escaped.find(L'"', pos)) != std::wstring::npos) {
        escaped.insert(pos, 1, L'\\');
        pos += 2;
    }
    return L"\"" + escaped + L"\"";
}

bool WriteEmbeddedSetup(const std::filesystem::path& outputPath, std::wstring& error) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_AGENT_SETUP), RT_RCDATA);
    if (!resource) {
        error = L"Embedded Agent setup resource was not found.";
        return false;
    }

    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) {
        error = L"Embedded Agent setup resource could not be loaded.";
        return false;
    }

    const DWORD size = SizeofResource(nullptr, resource);
    const void* bytes = LockResource(loaded);
    if (!bytes || size < 1024 * 1024) {
        error = L"Embedded Agent setup resource is invalid.";
        return false;
    }

    HANDLE file = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Unable to create the temporary Agent setup.";
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes, size, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != size) {
        DeleteFileW(outputPath.c_str());
        error = L"Unable to write the temporary Agent setup.";
        return false;
    }
    return true;
}

bool ConfigureServiceRecovery() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE service = OpenServiceW(
        scm,
        L"Hi5CentralAgent",
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }

    SC_ACTION actions[3]{};
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 60 * 1000;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 5 * 60 * 1000;
    actions[2].Type = SC_ACTION_RESTART;
    actions[2].Delay = 15 * 60 * 1000;

    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 60 * 60;
    failure.cActions = 3;
    failure.lpsaActions = actions;

    BOOL configured = ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    SERVICE_FAILURE_ACTIONS_FLAG flag{};
    flag.fFailureActionsOnNonCrashFailures = TRUE;
    configured = ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag) && configured;

    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    if (QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytesNeeded) &&
        status.dwCurrentState == SERVICE_STOPPED) {
        StartServiceW(service, 0, nullptr);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return configured == TRUE;
}

bool RunAgentSetup(const std::filesystem::path& setupPath,
                   const std::string& deploymentId,
                   const std::string& deploymentSecret,
                   const std::string& apiBase,
                   const std::string& installSource,
                   DWORD& exitCode,
                   std::wstring& error) {
    std::wstring args =
        Quote(setupPath.wstring()) +
        L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-" +
        L" /DEPLOYMENT_ID=" + Quote(Utf8ToWide(deploymentId)) +
        L" /DEPLOYMENT_SECRET=" + Quote(Utf8ToWide(deploymentSecret)) +
        L" /PACKAGE_ID=" + Quote(Utf8ToWide(deploymentId)) +
        L" /API_BASE_URL=" + Quote(Utf8ToWide(apiBase)) +
        L" /INSTALL_SOURCE=" + Quote(Utf8ToWide(installSource));

    std::vector<wchar_t> command(args.begin(), args.end());
    command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = L"Unable to start the embedded Hi5Central Agent setup.";
        return false;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) exitCode = ERROR_GEN_FAILURE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != 0) {
        error = L"Hi5Central Agent setup returned exit code " + std::to_wstring(exitCode) + L".";
        return false;
    }
    return true;
}

std::filesystem::path MakeTempSetupPath() {
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        return std::filesystem::temp_directory_path() / L"Hi5CentralAgentSetup.exe";
    }

    wchar_t tempFile[MAX_PATH]{};
    if (!GetTempFileNameW(tempPath, L"H5A", 0, tempFile)) {
        return std::filesystem::path(tempPath) / L"Hi5CentralAgentSetup.exe";
    }

    DeleteFileW(tempFile);
    std::filesystem::path result(tempFile);
    result.replace_extension(L".exe");
    return result;
}

void ShowError(const std::wstring& message, bool quiet) {
    if (!quiet) {
        MessageBoxW(nullptr, message.c_str(), L"Hi5Central Agent", MB_OK | MB_ICONERROR);
    }
}

void ShowSuccess(bool quiet) {
    if (!quiet) {
        MessageBoxW(
            nullptr,
            L"Hi5Central Agent has been installed.\n\n"
            L"If this device is offline, it will enrol automatically when connectivity to Hi5Central becomes available.",
            L"Hi5Central Agent",
            MB_OK | MB_ICONINFORMATION);
    }
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool quiet = IsQuiet(GetCommandLineW());

    const bool validMagic =
        gHi5DeploymentConfig.magic[0] == 'H' &&
        gHi5DeploymentConfig.magic[1] == '5' &&
        gHi5DeploymentConfig.magic[2] == 'C' &&
        gHi5DeploymentConfig.magic[3] == '0' &&
        gHi5DeploymentConfig.magic[4] == 'F' &&
        gHi5DeploymentConfig.magic[5] == '9' &&
        gHi5DeploymentConfig.magic[6] == 'A' &&
        gHi5DeploymentConfig.magic[7] == '1' &&
        gHi5DeploymentConfig.magic[8] == '7' &&
        gHi5DeploymentConfig.magic[9] == 'D' &&
        gHi5DeploymentConfig.magic[10] == '4' &&
        gHi5DeploymentConfig.magic[11] == '2' &&
        gHi5DeploymentConfig.magic[12] == 'B' &&
        gHi5DeploymentConfig.magic[13] == '6' &&
        gHi5DeploymentConfig.magic[14] == 'E' &&
        gHi5DeploymentConfig.magic[15] == '3';

    const std::wstring deploymentIdOverride = CommandLineOption(L"--deployment-id");
    const std::wstring deploymentSecretOverride = CommandLineOption(L"--deployment-secret");
    const std::wstring apiBaseOverride = CommandLineOption(L"--api-base");
    const std::wstring installSourceOverride = CommandLineOption(L"--install-source");

    const std::string deploymentId = deploymentIdOverride.empty()
        ? FixedField(gHi5DeploymentConfig.deploymentId, sizeof(gHi5DeploymentConfig.deploymentId))
        : WideToUtf8(deploymentIdOverride);
    const std::string deploymentSecret = deploymentSecretOverride.empty()
        ? FixedField(gHi5DeploymentConfig.deploymentSecret, sizeof(gHi5DeploymentConfig.deploymentSecret))
        : WideToUtf8(deploymentSecretOverride);
    const std::string apiBase = apiBaseOverride.empty()
        ? FixedField(gHi5DeploymentConfig.apiBase, sizeof(gHi5DeploymentConfig.apiBase))
        : WideToUtf8(apiBaseOverride);
    const std::string installSource = installSourceOverride.empty()
        ? "tenant-installer"
        : WideToUtf8(installSourceOverride);

    const bool templateDeploymentId =
        deploymentId == "__TEMPLATE__" || deploymentId.rfind("H5MSI_ID_", 0) == 0;
    const bool templateDeploymentSecret =
        deploymentSecret == "__TEMPLATE__" || deploymentSecret.rfind("H5MSI_SECRET_", 0) == 0;
    const bool templateApiBase = apiBase.rfind("H5MSI_API_", 0) == 0;

    if (!validMagic ||
        deploymentId.empty() || templateDeploymentId ||
        deploymentSecret.empty() || templateDeploymentSecret ||
        templateApiBase || apiBase.rfind("https://", 0) != 0) {
        ShowError(
            L"This Hi5Central Agent installer has not been assigned to a tenant. "
            L"Download a new installer from the Hi5Central portal.",
            quiet);
        return 2;
    }

    const auto setupPath = MakeTempSetupPath();
    std::wstring error;
    if (!WriteEmbeddedSetup(setupPath, error)) {
        ShowError(error, quiet);
        return 3;
    }

    DWORD setupExitCode = ERROR_GEN_FAILURE;
    const bool installed = RunAgentSetup(
        setupPath, deploymentId, deploymentSecret, apiBase, installSource, setupExitCode, error);

    DeleteFileW(setupPath.c_str());

    if (!installed) {
        ShowError(error, quiet);
        return static_cast<int>(setupExitCode ? setupExitCode : 4);
    }

    ConfigureServiceRecovery();
    ShowSuccess(quiet);
    return 0;
}