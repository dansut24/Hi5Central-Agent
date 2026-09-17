#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "session_launcher.h"

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

#include "../util/log.h"

#include <string>
#include <vector>

namespace hi5 {

    static std::wstring ToWide(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    static std::string Narrow(const std::wstring& ws) {
        if (ws.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    static std::wstring DirOf(const std::wstring& path) {
        const auto pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return L".";
        return path.substr(0, pos);
    }

    static bool EnablePrivilege(const wchar_t* privName) {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            LogWarn("[launcher] OpenProcessToken failed err=" + std::to_string(GetLastError()));
            return false;
        }

        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, privName, &luid)) {
            LogWarn("[launcher] LookupPrivilegeValue failed for " + Narrow(privName) +
                " err=" + std::to_string(GetLastError()));
            CloseHandle(hToken);
            return false;
        }

        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
            LogWarn("[launcher] AdjustTokenPrivileges failed for " + Narrow(privName) +
                " err=" + std::to_string(GetLastError()));
            CloseHandle(hToken);
            return false;
        }

        const DWORD lastErr = GetLastError();
        CloseHandle(hToken);

        if (lastErr != ERROR_SUCCESS) {
            LogWarn("[launcher] privilege not fully enabled for " + Narrow(privName) +
                " err=" + std::to_string(lastErr));
            return false;
        }

        return true;
    }

    static void EnableLaunchPrivileges() {
        EnablePrivilege(L"SeAssignPrimaryTokenPrivilege");
        EnablePrivilege(L"SeIncreaseQuotaPrivilege");
        EnablePrivilege(L"SeTcbPrivilege");
        EnablePrivilege(L"SeDebugPrivilege");
    }

    static bool IsInteractiveGuiCommand(const std::string& cmdLine) {
        return cmdLine.find("--mode chat-overlay") != std::string::npos ||
            cmdLine.find("--mode=chat-overlay") != std::string::npos ||
            cmdLine.find("--mode banner") != std::string::npos ||
            cmdLine.find("--mode=banner") != std::string::npos;
    }


    static bool IsRunningAsLocalSystem() {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            LogWarn("[launcher] OpenProcessToken(query) failed err=" + std::to_string(GetLastError()));
            return false;
        }

        DWORD needed = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
        if (needed == 0) {
            CloseHandle(hToken);
            return false;
        }

        std::vector<BYTE> buffer(needed);
        if (!GetTokenInformation(hToken, TokenUser, buffer.data(), needed, &needed)) {
            LogWarn("[launcher] GetTokenInformation(TokenUser) failed err=" + std::to_string(GetLastError()));
            CloseHandle(hToken);
            return false;
        }

        auto* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
        PSID localSystemSid = nullptr;
        if (!AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &localSystemSid)) {
            CloseHandle(hToken);
            return false;
        }

        BOOL matches = EqualSid(tokenUser->User.Sid, localSystemSid);

        FreeSid(localSystemSid);
        CloseHandle(hToken);
        return matches == TRUE;
    }

    static HANDLE LaunchWithCurrentProcessToken(
        const std::string& exePath,
        const std::string& cmdLine,
        const wchar_t* desktopName,
        const char* logPrefix) {
        const std::wstring wExePath = ToWide(exePath);
        const std::wstring wCmd = L"\"" + wExePath + L"\" " + ToWide(cmdLine);
        const std::wstring wDir = DirOf(wExePath);

        std::vector<wchar_t> cmdBuf(wCmd.begin(), wCmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.lpDesktop = const_cast<LPWSTR>(desktopName);

        PROCESS_INFORMATION pi{};

        LogInfo(std::string(logPrefix) +
            " CreateProcessW current-user fallback desktop=" + Narrow(desktopName) +
            " exe=" + Narrow(wExePath) +
            " dir=" + Narrow(wDir));

        const BOOL ok = CreateProcessW(
            wExePath.c_str(),
            cmdBuf.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            wDir.c_str(),
            &si,
            &pi);

        const DWORD lastErr = GetLastError();
        if (!ok) {
            LogError(std::string(logPrefix) + " CreateProcessW current-user fallback failed err=" + std::to_string(lastErr));
            return nullptr;
        }

        LogInfo(std::string(logPrefix) + " current-user fallback pid=" + std::to_string(pi.dwProcessId));
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    static bool DuplicateCurrentProcessPrimaryTokenForSession(DWORD sessionId, HANDLE& hPrimaryOut) {
        hPrimaryOut = nullptr;

        HANDLE hProcToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY |
            TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            &hProcToken)) {
            LogError("[launcher] OpenProcessToken(current process) failed err=" +
                std::to_string(GetLastError()));
            return false;
        }

        HANDLE hDup = nullptr;
        if (!DuplicateTokenEx(hProcToken,
            MAXIMUM_ALLOWED,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &hDup)) {
            LogError("[launcher] DuplicateTokenEx(current process token) failed err=" +
                std::to_string(GetLastError()));
            CloseHandle(hProcToken);
            return false;
        }

        CloseHandle(hProcToken);

        if (!SetTokenInformation(hDup, TokenSessionId, &sessionId, sizeof(sessionId))) {
            LogError("[launcher] SetTokenInformation(TokenSessionId=" +
                std::to_string(sessionId) + ") failed err=" +
                std::to_string(GetLastError()));
            CloseHandle(hDup);
            return false;
        }

        hPrimaryOut = hDup;
        return true;
    }

    static LPVOID BuildEnvironmentForToken(HANDLE hToken) {
        LPVOID envBlock = nullptr;
        if (!CreateEnvironmentBlock(&envBlock, hToken, FALSE)) {
            LogWarn("[launcher] CreateEnvironmentBlock failed err=" +
                std::to_string(GetLastError()) + ", using null environment");
            return nullptr;
        }
        return envBlock;
    }

    static HANDLE LaunchWithToken(HANDLE hPrimary,
        const std::wstring& wExePath,
        const std::wstring& wCmd,
        const std::wstring& wDir,
        const wchar_t* desktopName,
        const std::string& tokenDesc,
        const char* logPrefix,
        bool allowGui = false) {
        LPVOID envBlock = BuildEnvironmentForToken(hPrimary);

        std::vector<wchar_t> cmdBuf(wCmd.begin(), wCmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(desktopName);
        if (!allowGui) {
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
        }

        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NO_WINDOW;
        if (envBlock) flags |= CREATE_UNICODE_ENVIRONMENT;

        const char* effectivePrefix = allowGui ? "[launcher][interactive-gui]" : logPrefix;
        LogInfo(std::string(effectivePrefix) +
            " CreateProcessAsUserW desktop=" + Narrow(desktopName) +
            " exe=" + Narrow(wExePath) +
            " dir=" + Narrow(wDir) +
            " token=" + tokenDesc +
            " allow_gui=" + std::to_string(allowGui ? 1 : 0));

        const BOOL ok = CreateProcessAsUserW(
            hPrimary,
            wExePath.c_str(),
            cmdBuf.data(),
            nullptr,
            nullptr,
            FALSE,
            flags,
            envBlock,
            wDir.c_str(),
            &si,
            &pi);

        const DWORD lastErr = GetLastError();

        if (envBlock) {
            DestroyEnvironmentBlock(envBlock);
        }

        if (!ok) {
            LogError(std::string(logPrefix) + " CreateProcessAsUserW failed err=" +
                std::to_string(lastErr));
            return nullptr;
        }

        LogInfo(std::string(effectivePrefix) + " pid=" + std::to_string(pi.dwProcessId));
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    HANDLE LaunchInInteractiveSessionOnDesktop(const std::string& exePath,
        const std::string& cmdLine, const std::wstring& desktopName) {
        EnableLaunchPrivileges();

        const DWORD sessionId = WTSGetActiveConsoleSessionId();
        if (sessionId == 0xFFFFFFFF || desktopName.empty()) {
            LogError("[launcher][private-desktop] invalid console session or desktop");
            return nullptr;
        }

        HANDLE hUserToken = nullptr;
        if (!WTSQueryUserToken(sessionId, &hUserToken)) {
            LogError("[launcher][private-desktop] WTSQueryUserToken failed err=" +
                std::to_string(GetLastError()));
            return nullptr;
        }

        HANDLE hPrimary = nullptr;
        if (!DuplicateTokenEx(hUserToken, MAXIMUM_ALLOWED, nullptr,
            SecurityImpersonation, TokenPrimary, &hPrimary)) {
            LogError("[launcher][private-desktop] DuplicateTokenEx failed err=" +
                std::to_string(GetLastError()));
            CloseHandle(hUserToken);
            return nullptr;
        }
        CloseHandle(hUserToken);

        const std::wstring wExePath = ToWide(exePath);
        const std::wstring wCmd = L"\"" + wExePath + L"\" " + ToWide(cmdLine);
        const std::wstring wDir = DirOf(wExePath);
        HANDLE hProc = LaunchWithToken(hPrimary, wExePath, wCmd, wDir,
            desktopName.c_str(), "InteractiveUser(private-desktop)",
            "[launcher][private-desktop]", true);
        CloseHandle(hPrimary);
        return hProc;
    }

    HANDLE LaunchInElevatedSessionOnDesktop(const std::string& exePath,
        const std::string& cmdLine, const std::wstring& desktopName) {
        if (desktopName.empty()) {
            LogError("[launcher][private-desktop-elevated] desktop name is empty");
            return nullptr;
        }
        if (!IsRunningAsLocalSystem()) {
            LogError("[launcher][private-desktop-elevated] LocalSystem token required");
            return nullptr;
        }

        EnableLaunchPrivileges();
        DWORD sessionId = 0xFFFFFFFF;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) || sessionId == 0xFFFFFFFF) {
            LogError("[launcher][private-desktop-elevated] unable to resolve process session err=" +
                std::to_string(GetLastError()));
            return nullptr;
        }

        HANDLE hPrimary = nullptr;
        if (!DuplicateCurrentProcessPrimaryTokenForSession(sessionId, hPrimary)) {
            LogError("[launcher][private-desktop-elevated] failed to duplicate session-bound LocalSystem token");
            return nullptr;
        }

        const std::wstring wExePath = ToWide(exePath);
        const std::wstring wCmd = L"\"" + wExePath + L"\" " + ToWide(cmdLine);
        const std::wstring wDir = DirOf(wExePath);
        HANDLE hProc = LaunchWithToken(hPrimary, wExePath, wCmd, wDir,
            desktopName.c_str(), "LocalSystem(private-desktop)",
            "[launcher][private-desktop-elevated]", true);
        CloseHandle(hPrimary);
        return hProc;
    }

    HANDLE LaunchInInteractiveSession(const std::string& exePath,
        const std::string& cmdLine) {
        EnableLaunchPrivileges();

        const DWORD sessionId = WTSGetActiveConsoleSessionId();
        if (sessionId == 0xFFFFFFFF) {
            LogError("[launcher] no active console session");
            return nullptr;
        }

        LogInfo("[launcher] active console session=" + std::to_string(sessionId));

        HANDLE hUserToken = nullptr;
        if (!WTSQueryUserToken(sessionId, &hUserToken)) {
            LogError("[launcher] WTSQueryUserToken failed err=" +
                std::to_string(GetLastError()));
            return nullptr;
        }

        HANDLE hPrimary = nullptr;
        if (!DuplicateTokenEx(hUserToken,
            MAXIMUM_ALLOWED,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &hPrimary)) {
            LogError("[launcher] DuplicateTokenEx failed err=" +
                std::to_string(GetLastError()));
            CloseHandle(hUserToken);
            return nullptr;
        }

        CloseHandle(hUserToken);

        const std::wstring wExePath = ToWide(exePath);
        const std::wstring wCmd = L"\"" + wExePath + L"\" " + ToWide(cmdLine);
        const std::wstring wDir = DirOf(wExePath);

        const bool allowGui = IsInteractiveGuiCommand(cmdLine);
        if (allowGui) {
            LogInfo("[launcher][interactive-gui] detected interactive GUI helper command; launching GUI-visible helper");
        }

        HANDLE hProc = LaunchWithToken(
            hPrimary,
            wExePath,
            wCmd,
            wDir,
            L"winsta0\\default",
            "InteractiveUser",
            allowGui ? "[launcher][interactive-gui]" : "[launcher]",
            allowGui);

        CloseHandle(hPrimary);
        return hProc;
    }

    HANDLE LaunchInElevatedDefaultSessionForSession(const std::string& exePath,
        const std::string& cmdLine, DWORD sessionId) {
        if (!IsRunningAsLocalSystem()) {
            LogWarn("[launcher] process is not LocalSystem/SCM; using current-user CreateProcessW fallback for default desktop");
            return LaunchWithCurrentProcessToken(
                exePath, cmdLine, L"winsta0\\default", "[launcher][default-console]");
        }
        if (sessionId == 0xFFFFFFFF) {
            LogError("[launcher] no target console session for elevated default desktop");
            return nullptr;
        }
        EnableLaunchPrivileges();
        LogInfo("[launcher] launching elevated on Default Desktop, session=" + std::to_string(sessionId));
        HANDLE hPrimary = nullptr;
        if (!DuplicateCurrentProcessPrimaryTokenForSession(sessionId, hPrimary)) {
            LogError("[launcher] failed to build LocalSystem primary token for default desktop");
            return nullptr;
        }
        const std::wstring wExePath = ToWide(exePath);
        const std::wstring wCmd = L"\"" + wExePath + L"\" " + ToWide(cmdLine);
        const std::wstring wDir = DirOf(wExePath);
        HANDLE hProc = LaunchWithToken(hPrimary, wExePath, wCmd, wDir,
            L"winsta0\\default", "LocalSystem(session-bound)", "[launcher][default-elevated]");
        CloseHandle(hPrimary);
        return hProc;
    }

    HANDLE LaunchInElevatedDefaultSession(const std::string& exePath,
        const std::string& cmdLine) {
        return LaunchInElevatedDefaultSessionForSession(exePath, cmdLine, WTSGetActiveConsoleSessionId());
    }

    HANDLE LaunchOnSecureDesktopForSession(const std::string& exePath,
        const std::string& cmdLine, DWORD sessionId) {
        if (sessionId == 0xFFFFFFFF) {
            LogError("[launcher] no target console session for Secure Desktop");
            return nullptr;
        }
        EnableLaunchPrivileges();
        LogInfo("[launcher] launching on Secure Desktop, session=" + std::to_string(sessionId));
        HANDLE hPrimary = nullptr;
        if (!DuplicateCurrentProcessPrimaryTokenForSession(sessionId, hPrimary)) {
            LogError("[launcher] failed to build LocalSystem primary token for secure desktop");
            return nullptr;
        }
        const std::wstring wExePath = ToWide(exePath);
        const std::wstring wCmd = L"\"" + wExePath + L"\" " + ToWide(cmdLine);
        const std::wstring wDir = DirOf(wExePath);
        HANDLE hProc = LaunchWithToken(hPrimary, wExePath, wCmd, wDir,
            L"winsta0\\Winlogon", "LocalSystem(session-bound)", "[launcher][secure]");
        CloseHandle(hPrimary);
        return hProc;
    }

    HANDLE LaunchOnSecureDesktop(const std::string& exePath,
        const std::string& cmdLine) {
        return LaunchOnSecureDesktopForSession(exePath, cmdLine, WTSGetActiveConsoleSessionId());
    }
} // namespace hi5