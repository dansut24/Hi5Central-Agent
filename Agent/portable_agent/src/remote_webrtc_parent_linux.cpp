#include "remote_webrtc.h"
#include "platform_info.h"

#if defined(__linux__)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <mutex>
#include <pwd.h>
#include <signal.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace hi5 {
namespace {

using json = nlohmann::json;

struct UserIdentity {
    std::string name;
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    std::string runtimeDir;
};

UserIdentity activeUserIdentity() {
    UserIdentity out;
    out.name = activeUser();
    if (out.name.empty()) return out;

    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size < 1024) size = 16384;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    struct passwd pwd {};
    struct passwd* result = nullptr;
    if (getpwnam_r(out.name.c_str(), &pwd, buffer.data(), buffer.size(), &result) == 0 && result) {
        out.uid = pwd.pw_uid;
        out.gid = pwd.pw_gid;
        out.runtimeDir = "/run/user/" + std::to_string(out.uid);
    }
    return out;
}

bool sendLine(int fd, std::mutex& mutex, const json& message) {
    const std::string payload = message.dump() + "\n";
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t written = ::send(
            fd,
            payload.data() + offset,
            payload.size() - offset,
            MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

std::string safeSessionToken(std::string value) {
    for (auto& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
    }
    if (value.size() > 72) value.resize(72);
    return value.empty() ? "session" : value;
}

std::string helperPath() {
    static const char* candidates[] = {
        "/opt/hi5central/agent/Hi5CentralRemoteHelper",
        "/usr/local/libexec/hi5central/Hi5CentralRemoteHelper"
    };
    for (const auto* candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate;
    }
    return {};
}

} // namespace

class RemoteWebRtcSession {
public:
    using SendFn = RemoteDesktopManager::SendFn;

    RemoteWebRtcSession(std::string sessionId, SendFn send)
        : sessionId_(std::move(sessionId)), send_(std::move(send)) {}

    ~RemoteWebRtcSession() { stop(); }

    bool start(const json& initialMessage, std::string& error) {
        const auto user = activeUserIdentity();
        if (user.name.empty() || user.uid == static_cast<uid_t>(-1)) {
            error = "No active graphical Linux user is available for remote desktop.";
            return false;
        }
        if (user.runtimeDir.empty() || !std::filesystem::exists(user.runtimeDir)) {
            error = "The active Linux desktop does not expose an XDG runtime directory.";
            return false;
        }

        const auto helper = helperPath();
        if (helper.empty()) {
            error = "Hi5CentralRemoteHelper is not installed. Upgrade the Linux Agent.";
            return false;
        }

        const std::string socketDir = "/run/hi5central";
        ::mkdir(socketDir.c_str(), 0755);
        socketPath_ = socketDir + "/remote-" + safeSessionToken(sessionId_) + ".sock";
        ::unlink(socketPath_.c_str());

        listenerFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listenerFd_ < 0) {
            error = "Unable to create Linux remote helper socket: " + std::string(std::strerror(errno));
            return false;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (socketPath_.size() >= sizeof(address.sun_path)) {
            error = "Linux remote helper socket path is too long.";
            stop();
            return false;
        }
        std::strncpy(address.sun_path, socketPath_.c_str(), sizeof(address.sun_path) - 1);

        if (::bind(listenerFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = "Unable to bind Linux remote helper socket: " + std::string(std::strerror(errno));
            stop();
            return false;
        }
        ::chmod(socketPath_.c_str(), 0600);
        ::chown(socketPath_.c_str(), user.uid, user.gid);

        if (::listen(listenerFd_, 1) != 0) {
            error = "Unable to listen for the Linux remote helper.";
            stop();
            return false;
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            error = "Unable to launch the Linux remote helper.";
            stop();
            return false;
        }
        if (pid == 0) {
            ::close(listenerFd_);

            // The Agent is a long-lived multi-socket service. Do not leak its
            // HTTPS/WebSocket/PipeWire/event descriptors into the desktop
            // helper: libdatachannel must create its own clean ICE sockets.
            const long maxFd = std::min<long>(sysconf(_SC_OPEN_MAX), 4096);
            for (int fd = 3; fd < maxFd; ++fd) ::close(fd);

            const std::string runtime = "XDG_RUNTIME_DIR=" + user.runtimeDir;
            const std::string bus = "DBUS_SESSION_BUS_ADDRESS=unix:path=" + user.runtimeDir + "/bus";
            const char* runuser = "/usr/sbin/runuser";
            const char* env = "/usr/bin/env";

            execl(
                runuser,
                "runuser",
                "-u",
                user.name.c_str(),
                "--",
                env,
                runtime.c_str(),
                bus.c_str(),
                helper.c_str(),
                "--remote-helper",
                "--socket",
                socketPath_.c_str(),
                static_cast<char*>(nullptr));
            _exit(122);
        }

        launcherPid_ = pid;

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenerFd_, &readSet);
        timeval timeout {};
        timeout.tv_sec = 15;
        const int ready = ::select(listenerFd_ + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            error = ready == 0
                ? "Timed out waiting for the Linux desktop helper."
                : "Failed while waiting for the Linux desktop helper.";
            stop();
            return false;
        }

        helperFd_ = ::accept4(listenerFd_, nullptr, nullptr, SOCK_CLOEXEC);
        ::close(listenerFd_);
        listenerFd_ = -1;
        ::unlink(socketPath_.c_str());

        if (helperFd_ < 0) {
            error = "The Linux remote helper could not connect.";
            stop();
            return false;
        }

        running_.store(true);
        readerThread_ = std::thread([this]() { readerLoop(); });

        if (!sendLine(helperFd_, writeMutex_, initialMessage)) {
            error = "Unable to send the remote session to the Linux helper.";
            stop();
            return false;
        }
        return true;
    }

    bool send(const json& message) {
        if (!running_.load() || helperFd_ < 0) return false;
        return sendLine(helperFd_, writeMutex_, message);
    }

    bool running() const {
        return running_.load() && helperFd_ >= 0;
    }

    void stop() {
        const bool wasRunning = running_.exchange(false);
        if (wasRunning && helperFd_ >= 0) {
            sendLine(helperFd_, writeMutex_, {
                {"type", "stop_webrtc"},
                {"session_id", sessionId_}
            });
        }

        if (helperFd_ >= 0) {
            ::shutdown(helperFd_, SHUT_RDWR);
            ::close(helperFd_);
            helperFd_ = -1;
        }
        if (listenerFd_ >= 0) {
            ::close(listenerFd_);
            listenerFd_ = -1;
        }
        if (!socketPath_.empty()) ::unlink(socketPath_.c_str());

        if (readerThread_.joinable() &&
            readerThread_.get_id() != std::this_thread::get_id()) {
            readerThread_.join();
        }

        if (launcherPid_ > 0) {
            int status = 0;
            (void)::waitpid(launcherPid_, &status, WNOHANG);
            launcherPid_ = -1;
        }
    }

private:
    void readerLoop() {
        std::string buffered;
        char chunk[8192];
        while (running_.load() && helperFd_ >= 0) {
            const ssize_t count = ::read(helperFd_, chunk, sizeof(chunk));
            if (count > 0) {
                buffered.append(chunk, static_cast<std::size_t>(count));
                for (;;) {
                    const auto newline = buffered.find('\n');
                    if (newline == std::string::npos) break;
                    std::string line = buffered.substr(0, newline);
                    buffered.erase(0, newline + 1);
                    if (line.empty()) continue;
                    auto message = json::parse(line, nullptr, false);
                    if (!message.is_discarded() && message.is_object()) send_(message);
                }
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
        running_.store(false);
    }

    std::string sessionId_;
    SendFn send_;
    std::string socketPath_;
    int listenerFd_ = -1;
    int helperFd_ = -1;
    pid_t launcherPid_ = -1;
    std::atomic<bool> running_{false};
    std::thread readerThread_;
    std::mutex writeMutex_;
};

RemoteDesktopManager::RemoteDesktopManager(SendFn send) : send_(std::move(send)) {}
RemoteDesktopManager::~RemoteDesktopManager() { stopAll(); }

bool RemoteDesktopManager::handleMessage(const json& message) {
    if (!message.is_object()) return false;
    const std::string type = message.value("type", "");
    const std::string sessionId = message.value("session_id", message.value("sessionId", ""));
    if (sessionId.empty()) return false;

    if (type == "start_webrtc") {
        const std::string mode = message.value("mode", "console");
        if (mode != "console") {
            send_({
                {"type", "remote_error"},
                {"session_id", sessionId},
                {"code", "unsupported_mode"},
                {"message", "Linux desktop remote access supports console sessions only."}
            });
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto existing = sessions_.find(sessionId);
            if (existing != sessions_.end()) {
                if (existing->second && existing->second->running()) {
                    // The viewer may retry start_webrtc while a Wayland portal
                    // request or WebRTC negotiation is still in progress. Keep
                    // the existing user-session helper instead of creating a
                    // second GNOME portal/PipeWire session for the same ID.
                    return true;
                }
                if (existing->second) existing->second->stop();
                sessions_.erase(existing);
            }
        }

        auto session = std::make_unique<RemoteWebRtcSession>(sessionId, send_);
        std::string error;
        if (!session->start(message, error)) {
            send_({
                {"type", "remote_error"},
                {"session_id", sessionId},
                {"code", "linux_helper_start_failed"},
                {"message", error.empty() ? "Linux remote helper could not start." : error}
            });
            return true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[sessionId] = std::move(session);
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return false;

    const bool terminal =
        type == "viewer_disconnected" || type == "viewer_closed" ||
        type == "viewer_left" || type == "end_session" || type == "stop_webrtc";

    const bool sent = it->second->send(message);
    if (terminal) {
        it->second->stop();
        sessions_.erase(it);
        return true;
    }
    return sent;
}

void RemoteDesktopManager::stopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, session] : sessions_) {
        if (session) session->stop();
    }
    sessions_.clear();
}

} // namespace hi5

#endif
