#include "remote_webrtc.h"
#include "remote_session_ui.h"

#if defined(__APPLE__)

#import <Foundation/Foundation.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

using json = nlohmann::json;

std::string argValue(int argc, char** argv, const std::string& name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && name == argv[i]) return argv[i + 1] ? argv[i + 1] : "";
    }
    return {};
}

bool sendLine(int fd, std::mutex& mutex, const json& message) {
    const std::string payload = message.dump() + "\n";
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t written =
            ::write(fd, payload.data() + offset, payload.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

int connectSocket(const std::string& path) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        int noSigPipe = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            ::close(fd);
            return -1;
        }
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

        if (::connect(
                fd,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

bool terminalMessage(const std::string& type) {
    return type == "viewer_disconnected" ||
           type == "viewer_closed" ||
           type == "viewer_left" ||
           type == "end_session" ||
           type == "stop_webrtc";
}

int runHelper(int argc, char** argv) {
    const std::string socketPath = argValue(argc, argv, "--socket");
    if (socketPath.empty()) return 2;

    const int fd = connectSocket(socketPath);
    if (fd < 0) return 3;

    std::mutex writeMutex;
    auto send = [&](const json& message) {
        return sendLine(fd, writeMutex, message);
    };

    hi5::RemoteSessionUi sessionUi(send);
    hi5::RemoteDesktopManager manager(send);
    std::atomic<bool> readerDone{false};

    std::thread reader([&]() {
        std::string buffered;
        char chunk[8192];

        while (true) {
            const ssize_t count = ::read(fd, chunk, sizeof(chunk));
            if (count > 0) {
                buffered.append(chunk, static_cast<std::size_t>(count));
                for (;;) {
                    const auto newline = buffered.find('\n');
                    if (newline == std::string::npos) break;

                    std::string line = buffered.substr(0, newline);
                    buffered.erase(0, newline + 1);
                    if (line.empty()) continue;

                    auto message = json::parse(line, nullptr, false);
                    if (message.is_discarded() || !message.is_object()) continue;

                    const std::string type = message.value("type", "");
                    if (type == "start_webrtc") {
                        sessionUi.handleStart(message);
                        manager.handleMessage(message);
                    } else if (type == "chat_message") {
                        sessionUi.handleChatMessage(message);
                    } else if (type == "chat_close") {
                        sessionUi.handleChatClose();
                    } else {
                        manager.handleMessage(message);
                        if (terminalMessage(type)) {
                            sessionUi.handleSessionEnd();
                        }
                    }
                }
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }

        manager.stopAll();
        sessionUi.handleSessionEnd();
        readerDone.store(true);
        sessionUi.quit();
    });

    sessionUi.run();

    if (!readerDone.load()) {
        ::shutdown(fd, SHUT_RDWR);
    }
    if (reader.joinable()) reader.join();

    ::close(fd);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        return runHelper(argc, argv);
    }
}

#endif
