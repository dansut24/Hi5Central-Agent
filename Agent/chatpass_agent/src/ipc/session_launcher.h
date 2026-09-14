#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <string>

namespace hi5 {

    // Launches exePath with cmdLine in the active interactive console session
    // using the active logged-on user's token on the normal desktop: winsta0\default.
    HANDLE LaunchInInteractiveSession(const std::string& exePath,
        const std::string& cmdLine);

    // Launches exePath with cmdLine in the active console session
    // using a session-bound LocalSystem token on the normal desktop: winsta0\default.
    // This is intended for high-integrity input/capture when elevated/admin windows
    // are present on the normal desktop.
    HANDLE LaunchInElevatedDefaultSession(const std::string& exePath,
        const std::string& cmdLine);
    HANDLE LaunchInElevatedDefaultSessionForSession(const std::string& exePath,
        const std::string& cmdLine, DWORD sessionId);

    // Launches exePath with cmdLine on the secure desktop: winsta0\Winlogon.
    // Intended for UAC prompts, lock screen, and logon screen capture.
    HANDLE LaunchOnSecureDesktop(const std::string& exePath,
        const std::string& cmdLine);
    HANDLE LaunchOnSecureDesktopForSession(const std::string& exePath,
        const std::string& cmdLine, DWORD sessionId);

} // namespace hi5  