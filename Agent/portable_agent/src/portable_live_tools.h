#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hi5 {

class PortableLiveTools {
public:
    using SendFunction = std::function<bool(const nlohmann::json&)>;

    explicit PortableLiveTools(SendFunction send);
    ~PortableLiveTools();

    PortableLiveTools(const PortableLiveTools&) = delete;
    PortableLiveTools& operator=(const PortableLiveTools&) = delete;

    bool handleMessage(const nlohmann::json& message);
    void stopAll();

    static int runInternalHelper(int argc, char* argv[]);

private:
    struct TerminalSession;
    struct UploadState {
        std::string sessionId;
        std::string transferId;
        std::string directory;
        std::string filename;
        std::string runAs;
        std::uint64_t expected = 0;
        std::vector<unsigned char> bytes;
    };

    void handleTerminalMessage(const nlohmann::json& message);
    void startTerminal(const nlohmann::json& message);
    void terminalInput(const nlohmann::json& message);
    void terminalResize(const nlohmann::json& message);
    void stopTerminal(const std::string& sessionId, bool notify = true);

    void handleFilesMessage(const nlohmann::json& message);
    void filesList(const nlohmann::json& message);
    void filesAction(const nlohmann::json& message);
    void filesDownload(const nlohmann::json& message);
    void filesUpload(const nlohmann::json& message);

    void sendTerminal(
        const std::string& type,
        const std::string& sessionId,
        const std::string& text = {});

    void sendFilesError(
        const std::string& sessionId,
        const std::string& path,
        const std::string& error);

    SendFunction send_;
    std::mutex terminalsMutex_;
    std::map<std::string, std::shared_ptr<TerminalSession>> terminals_;
    std::mutex uploadsMutex_;
    std::unordered_map<std::string, UploadState> uploads_;
};

} // namespace hi5
