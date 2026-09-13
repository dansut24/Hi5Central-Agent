#include "named_pipe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace hi5 {
namespace {
    static HANDLE AsHandle(void* p) { return reinterpret_cast<HANDLE>(p); }
}

NamedPipeServer::NamedPipeServer() = default;
NamedPipeServer::~NamedPipeServer() { Stop(); }

bool NamedPipeServer::Start(const std::string& pipeName, MessageCallback cb) {
    Stop();
    pipeName_ = pipeName;
    callback_ = std::move(cb);
    running_.store(true);
    thread_ = std::thread([this]() { ThreadMain(); });
    return true;
}

void NamedPipeServer::Stop() {
    running_.store(false);
    if (pipe_) {
        CancelIoEx(AsHandle(pipe_), nullptr);
        DisconnectNamedPipe(AsHandle(pipe_));
        CloseHandle(AsHandle(pipe_));
        pipe_ = nullptr;
    }
    if (thread_.joinable()) thread_.join();
}

void NamedPipeServer::ThreadMain() {
    const std::string fullName = pipeName_.rfind("\\\\.\\pipe\\", 0) == 0 ? pipeName_ : ("\\\\.\\pipe\\" + pipeName_);
    while (running_.load()) {
        HANDLE hPipe = CreateNamedPipeA(
            fullName.c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            return;
        }
        pipe_ = hPipe;
        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(hPipe);
            pipe_ = nullptr;
            Sleep(200);
            continue;
        }

        char buffer[4096];
        while (running_.load()) {
            DWORD read = 0;
            BOOL ok = ReadFile(hPipe, buffer, sizeof(buffer) - 1, &read, nullptr);
            if (!ok || read == 0) break;
            buffer[read] = '\0';
            if (callback_) callback_(std::string(buffer, read));
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        pipe_ = nullptr;
    }
}

bool NamedPipeClient::Connect(const std::string& pipeName, int retries, int retryDelayMs) {
    Close();
    const std::string fullName = pipeName.rfind("\\\\.\\pipe\\", 0) == 0 ? pipeName : ("\\\\.\\pipe\\" + pipeName);
    for (int i = 0; i < retries; ++i) {
        HANDLE h = CreateFileA(fullName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            pipe_ = h;
            return true;
        }
        Sleep(retryDelayMs);
    }
    return false;
}

void NamedPipeClient::Close() {
    if (pipe_) {
        CloseHandle(AsHandle(pipe_));
        pipe_ = nullptr;
    }
}

bool NamedPipeClient::SendLine(const std::string& line) {
    if (!pipe_) return false;
    DWORD written = 0;
    return WriteFile(AsHandle(pipe_), line.data(), (DWORD)line.size(), &written, nullptr) && written == line.size();
}

} // namespace hi5
