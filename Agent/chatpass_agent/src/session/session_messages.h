#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hi5 {

enum class SessionEventType {
    Unknown = 0,
    ViewerConnected,
    ViewerDisconnected,
    SecureDesktopEntering,
    SecureDesktopReady,
    SecureDesktopExited,
    DesktopHandoffEntering,
    DesktopHandoffReady,
    ChatMessageReceived,
    ChatMessageSent,
    ChatOpenRequested,
    ChatCloseRequested,
    AgentBannerRequested,
    AgentBannerDismissed,
    FileListRequested,
    FileListReceived,
    FileUploadRequested,
    FileDownloadRequested
};

struct ChatMessage {
    std::string sessionId;
    std::string sender;      // "tech" | "user" | "system"
    std::string displayName;
    std::string body;
    std::int64_t unixMs = 0;
};

struct FileBrowserEntry {
    std::string name;
    std::string path;
    bool isDir = false;
    std::uint64_t size = 0;
    std::string fileId;
    std::string fileToken;
};

struct AgentBannerState {
    std::string sessionId;
    std::string technicianName;
    bool connected = false;
    bool chatAvailable = false;
};

} // namespace hi5
