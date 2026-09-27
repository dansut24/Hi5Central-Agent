#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hi5 {

class ConnectFileBrowser {
public:
    using SendCallback = std::function<void(const nlohmann::json&)>;

    explicit ConnectFileBrowser(SendCallback send);

    bool Handle(const std::string& sessionId, const nlohmann::json& message);
    void CancelAll();

private:
    struct IncomingUpload {
        std::filesystem::path target;
        std::unique_ptr<std::ofstream> stream;
        std::uint64_t expected{ 0 };
        std::uint64_t received{ 0 };
    };

    void List(const std::string& sessionId, const std::string& rawPath);
    void Download(const std::string& sessionId, const nlohmann::json& message);
    void UploadInline(const std::string& sessionId, const nlohmann::json& message);
    void UploadStart(const std::string& sessionId, const nlohmann::json& message);
    void UploadChunk(const std::string& sessionId, const nlohmann::json& message);
    void UploadComplete(const std::string& sessionId, const nlohmann::json& message);
    void UploadCancel(const std::string& sessionId, const nlohmann::json& message);
    void DeletePath(const std::string& sessionId, const nlohmann::json& message);
    void MakeDirectory(const std::string& sessionId, const nlohmann::json& message);
    void RenamePath(const std::string& sessionId, const nlohmann::json& message);

    void Send(nlohmann::json payload);
    void Error(const std::string& sessionId, const std::string& requestType,
        const std::string& path, const std::string& error);

    static std::filesystem::path ResolvePath(const std::string& rawPath);
    static bool IsProtectedPath(const std::filesystem::path& target);
    static std::string Base64Encode(const std::vector<unsigned char>& data);
    static std::vector<unsigned char> Base64Decode(const std::string& input);
    static std::string UploadKey(const std::string& sessionId, const std::string& transferId);
    static std::string ComputerName();

    SendCallback send_;
    std::mutex uploadsMu_;
    std::unordered_map<std::string, std::unique_ptr<IncomingUpload>> uploads_;
};

} // namespace hi5
