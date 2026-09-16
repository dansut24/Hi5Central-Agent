#include "ui/native_banner.h"
#include "ui/native_tray.h"
#include "util/log.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace hi5 {
    int RunNativeChatMain(int argc, char** argv);
}

#ifdef _WIN32
namespace {
using json = nlohmann::json;

std::string ModeFromArgs(const std::vector<std::string>& args) {
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--mode") return args[i + 1];
    }
    return {};
}

DWORD CurrentWindowsSessionId() {
    DWORD sid = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid)) return 0;
    return sid;
}

std::string HostPipeName(DWORD sid) {
    return "\\\\.\\pipe\\Hi5CentralUserHost_" + std::to_string(sid);
}

std::wstring HostMutexName(DWORD sid) {
    return L"Local\\Hi5CentralUserHost_" + std::to_wstring(sid);
}

int RunFeature(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    const std::string mode = ModeFromArgs(args);
    if (mode == "tray") return hi5::RunNativeTrayMain(static_cast<int>(argv.size()), argv.data());
    if (mode == "banner") return hi5::RunNativeBannerMain(static_cast<int>(argv.size()), argv.data());
    if (mode == "native-chat") return hi5::RunNativeChatMain(static_cast<int>(argv.size()), argv.data());
    LogError("[user-host] unsupported feature mode=" + mode);
    return 2;
}

bool SendToHost(DWORD sid, const std::vector<std::string>& args) {
    const std::string pipeName = HostPipeName(sid);
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE pipe = CreateFileA(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            json command = {{"type", "launch"}, {"argv", args}};
            const std::string line = command.dump() + "\n";
            DWORD written = 0;
            const bool ok = WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
            return ok;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY) break;
        Sleep(50);
    }
    return false;
}

HANDLE CreateHostPipe(const std::string& pipeName) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
        "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    HANDLE pipe = CreateNamedPipeA(
        pipeName.c_str(), PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        4, 65536, 65536, 0, sa.lpSecurityDescriptor ? &sa : nullptr);
    if (sd) LocalFree(sd);
    return pipe;
}

int RunHost(DWORD sid, std::vector<std::string> initialFeature) {
    const std::wstring mutexName = HostMutexName(sid);
    HANDLE singleton = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!singleton) return 3;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        if (!initialFeature.empty() && SendToHost(sid, initialFeature)) return 0;
        return 4;
    }

    if (!initialFeature.empty()) {
        std::thread([args = std::move(initialFeature)]() { RunFeature(args); }).detach();
    }

    const std::string pipeName = HostPipeName(sid);
    LogInfo("[user-host] resident host started windows_session=" + std::to_string(sid));
    for (;;) {
        HANDLE pipe = CreateHostPipe(pipeName);
        if (pipe == INVALID_HANDLE_VALUE) {
            LogError("[user-host] CreateNamedPipe failed err=" + std::to_string(GetLastError()));
            Sleep(500);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        std::string payload;
        char buffer[8192];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
            payload.append(buffer, buffer + got);
            if (payload.find('\n') != std::string::npos || payload.size() > 262144) break;
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);

        const auto nl = payload.find('\n');
        if (nl != std::string::npos) payload.resize(nl);
        auto command = json::parse(payload, nullptr, false);
        if (command.is_discarded() || !command.is_object()) continue;
        if (command.value("type", std::string()) == "shutdown") break;
        if (command.value("type", std::string()) != "launch" || !command.contains("argv") || !command["argv"].is_array()) continue;
        std::vector<std::string> args;
        for (const auto& item : command["argv"]) if (item.is_string()) args.push_back(item.get<std::string>());
        if (args.empty()) continue;
        std::thread([args = std::move(args)]() { RunFeature(args); }).detach();
    }

    ReleaseMutex(singleton);
    CloseHandle(singleton);
    LogInfo("[user-host] resident host stopped windows_session=" + std::to_string(sid));
    return 0;
}
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    const DWORD sid = CurrentWindowsSessionId();
    const std::string mode = ModeFromArgs(args);
    if (mode == "user-host") return RunHost(sid, {});
    if (mode == "tray" || mode == "banner" || mode == "native-chat") {
        if (SendToHost(sid, args)) return 0;
        return RunHost(sid, std::move(args));
    }
#endif
    LogError("[user-host] missing or unsupported --mode");
    return 2;
}
