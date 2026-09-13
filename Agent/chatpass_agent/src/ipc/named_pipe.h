#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace hi5 {

class NamedPipeServer {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    NamedPipeServer();
    ~NamedPipeServer();

    bool Start(const std::string& pipeName, MessageCallback cb);
    void Stop();

private:
    void ThreadMain();

    std::string pipeName_;
    MessageCallback callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    void* pipe_ = nullptr;
};

class NamedPipeClient {
public:
    bool Connect(const std::string& pipeName, int retries = 40, int retryDelayMs = 250);
    void Close();
    bool SendLine(const std::string& line);

private:
    void* pipe_ = nullptr;
};

} // namespace hi5
