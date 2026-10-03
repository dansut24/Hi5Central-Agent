#include "portable_live_tools.h"

#include "execution_context.h"
#include "platform_info.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <util.h>
#else
#include <pty.h>
#endif

namespace hi5 {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::size_t kMaxTransferBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kDownloadChunkBytes = 48ULL * 1024ULL;

std::string messageSessionId(const json& message) {
    return message.value("sessionId", message.value("session_id", std::string()));
}

std::string messageTransferId(const json& message) {
    return message.value("transferId", message.value("transfer_id", std::string()));
}

std::string messageRunAs(const json& message) {
    return message.value("runAs", message.value("run_as", std::string("root")));
}

std::string executablePath() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        buffer.resize(std::strlen(buffer.c_str()));
        std::error_code ec;
        const auto resolved = fs::weakly_canonical(buffer, ec);
        return ec ? buffer : resolved.string();
    }
    return {};
#else
    std::array<char, 4096> buffer{};
    const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(count));
#endif
}

std::string base64Encode(const unsigned char* data, std::size_t length) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((length + 2) / 3) * 4);

    for (std::size_t index = 0; index < length; index += 3) {
        const unsigned int a = data[index];
        const unsigned int b = index + 1 < length ? data[index + 1] : 0;
        const unsigned int c = index + 2 < length ? data[index + 2] : 0;
        const unsigned int triple = (a << 16) | (b << 8) | c;

        output.push_back(table[(triple >> 18) & 0x3f]);
        output.push_back(table[(triple >> 12) & 0x3f]);
        output.push_back(index + 1 < length ? table[(triple >> 6) & 0x3f] : '=');
        output.push_back(index + 2 < length ? table[triple & 0x3f] : '=');
    }

    return output;
}

std::string base64Encode(const std::string& value) {
    return base64Encode(
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size());
}

std::vector<unsigned char> base64DecodeBytes(const std::string& input) {
    std::array<int, 256> table{};
    table.fill(-1);
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }

    std::vector<unsigned char> output;
    int value = 0;
    int bits = -8;
    for (const unsigned char ch : input) {
        if (ch == '=') break;
        const int decoded = table[ch];
        if (decoded < 0) continue;
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<unsigned char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::string base64DecodeString(const std::string& input) {
    const auto bytes = base64DecodeBytes(input);
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}

std::string fileTimeIso(const fs::file_time_type& value) {
    try {
        const auto systemValue = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            value - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const auto time = std::chrono::system_clock::to_time_t(systemValue);
        std::tm tm {};
        gmtime_r(&time, &tm);
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    } catch (...) {
        return {};
    }
}

bool validLeafName(const std::string& name) {
    return !name.empty() &&
        name != "." &&
        name != ".." &&
        name.find('/') == std::string::npos &&
        name.find('\0') == std::string::npos;
}

std::string normalizedPathString(const fs::path& path) {
    if (path.empty()) return "/";
    return path.lexically_normal().string();
}

json driveInfo(const std::string& name, const fs::path& root) {
    std::error_code ec;
    const auto space = fs::space(root, ec);
    json result = {
        {"name", name},
        {"root", normalizedPathString(root)}
    };
    if (!ec) {
        result["total_bytes"] = space.capacity;
        result["free_bytes"] = space.available;
    }
    return result;
}

json listDirectory(const json& payload) {
    fs::path target = payload.value("path", std::string("/"));
    if (target.empty()) target = "/";

    std::error_code ec;
    if (target.is_relative()) target = fs::absolute(target, ec);
    if (ec) throw std::runtime_error("Could not resolve path: " + ec.message());

    target = target.lexically_normal();
    if (!fs::exists(target, ec) || ec) {
        throw std::runtime_error("Path does not exist.");
    }
    if (!fs::is_directory(target, ec) || ec) {
        throw std::runtime_error("Path is not a folder.");
    }

    json entries = json::array();
    fs::directory_iterator iterator(target, fs::directory_options::skip_permission_denied, ec);
    if (ec) throw std::runtime_error("Could not open folder: " + ec.message());

    for (const auto& entry : iterator) {
        try {
            const auto entryPath = entry.path();
            const auto status = entry.symlink_status(ec);
            if (ec) {
                ec.clear();
                continue;
            }

            std::string type = "file";
            if (fs::is_directory(status)) type = "folder";
            else if (fs::is_symlink(status)) type = "symlink";
            else if (!fs::is_regular_file(status)) type = "other";

            std::uint64_t size = 0;
            if (type == "file") {
                size = entry.file_size(ec);
                if (ec) {
                    size = 0;
                    ec.clear();
                }
            }

            const auto modified = entry.last_write_time(ec);
            const std::string modifiedAt = ec ? std::string() : fileTimeIso(modified);
            ec.clear();

            entries.push_back({
                {"name", entryPath.filename().string()},
                {"path", normalizedPathString(entryPath)},
                {"full_path", normalizedPathString(entryPath)},
                {"type", type},
                {"size_bytes", size},
                {"modified_at", modifiedAt}
            });
        } catch (...) {
            // Skip filesystem names that cannot be represented safely in JSON.
        }
    }

    std::sort(entries.begin(), entries.end(), [](const json& left, const json& right) {
        const bool leftFolder = left.value("type", "") == "folder";
        const bool rightFolder = right.value("type", "") == "folder";
        if (leftFolder != rightFolder) return leftFolder > rightFolder;
        return left.value("name", "") < right.value("name", "");
    });

    fs::path parent;
    if (target != target.root_path()) parent = target.parent_path();

    json drives = json::array();
    drives.push_back(driveInfo("Root", "/"));
    const std::string home = payload.value("home", std::string());
    if (!home.empty() && fs::path(home) != fs::path("/")) {
        drives.push_back(driveInfo("Home", home));
    }

    return {
        {"ok", true},
        {"path", normalizedPathString(target)},
        {"parent", parent.empty() ? "" : normalizedPathString(parent)},
        {"entries", entries},
        {"drives", drives},
        {"count", entries.size()}
    };
}

json performFileAction(const std::string& operation, const json& payload) {
    if (operation == "mkdir") {
        const std::string name = payload.value("name", "");
        if (!validLeafName(name)) throw std::runtime_error("Invalid folder name.");
        fs::path base = payload.value("current_path", payload.value("path", std::string("/")));
        fs::path target = base / name;
        std::error_code ec;
        if (!fs::create_directory(target, ec)) {
            if (ec) throw std::runtime_error("Could not create folder: " + ec.message());
        }
        return {
            {"ok", true},
            {"message", "Folder created"},
            {"refresh_path", normalizedPathString(base)}
        };
    }

    if (operation == "rename") {
        const std::string name = payload.value("name", "");
        if (!validLeafName(name)) throw std::runtime_error("Invalid target name.");
        fs::path source = payload.value("path", "");
        if (source.empty()) throw std::runtime_error("Source path is required.");
        fs::path target = source.parent_path() / name;
        std::error_code ec;
        fs::rename(source, target, ec);
        if (ec) throw std::runtime_error("Rename failed: " + ec.message());
        return {
            {"ok", true},
            {"message", "Renamed"},
            {"refresh_path", normalizedPathString(source.parent_path())}
        };
    }

    if (operation == "delete") {
        fs::path target = payload.value("path", "");
        if (target.empty() || target == target.root_path()) {
            throw std::runtime_error("Refusing to delete an empty or root path.");
        }
        const fs::path parent = target.parent_path();
        std::error_code ec;
        const auto removed = fs::remove_all(target, ec);
        if (ec) throw std::runtime_error("Delete failed: " + ec.message());
        if (removed == 0) throw std::runtime_error("Path does not exist.");
        return {
            {"ok", true},
            {"message", "Deleted"},
            {"refresh_path", normalizedPathString(parent)}
        };
    }

    throw std::runtime_error("Unsupported file operation.");
}

int writeJsonResult(const json& result) {
    std::cout << result.dump();
    std::cout.flush();
    return 0;
}

struct HelperResult {
    int exitCode = 1;
    std::vector<unsigned char> output;
    std::string error;
    bool truncated = false;
};

HelperResult runHelper(
    const ExecutionContext& context,
    const std::string& operation,
    const json& payload,
    const std::vector<unsigned char>* input = nullptr,
    std::size_t maxOutputBytes = 8 * 1024 * 1024) {

    HelperResult result;
    const std::string binary = executablePath();
    if (binary.empty()) {
        result.error = "Could not resolve Agent executable path.";
        return result;
    }

    const std::string encodedPayload = base64Encode(payload.dump());

    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    int stdinPipe[2] = {-1, -1};

    if (::pipe(stdoutPipe) != 0 || ::pipe(stderrPipe) != 0 || (input && ::pipe(stdinPipe) != 0)) {
        result.error = "Could not create helper pipes.";
        for (int fd : stdoutPipe) if (fd >= 0) ::close(fd);
        for (int fd : stderrPipe) if (fd >= 0) ::close(fd);
        for (int fd : stdinPipe) if (fd >= 0) ::close(fd);
        return result;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        result.error = "Could not start file helper.";
        for (int fd : stdoutPipe) if (fd >= 0) ::close(fd);
        for (int fd : stderrPipe) if (fd >= 0) ::close(fd);
        for (int fd : stdinPipe) if (fd >= 0) ::close(fd);
        return result;
    }

    if (child == 0) {
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);
        if (input) ::dup2(stdinPipe[0], STDIN_FILENO);

        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        if (input) {
            ::close(stdinPipe[0]);
            ::close(stdinPipe[1]);
        }

        std::string contextMode = context.mode.empty() ? "root" : context.mode;
        std::array<char*, 6> args {
            const_cast<char*>(binary.c_str()),
            const_cast<char*>("--hi5-file-helper"),
            const_cast<char*>(operation.c_str()),
            const_cast<char*>(encodedPayload.c_str()),
            const_cast<char*>(contextMode.c_str()),
            nullptr
        };
        ::execv(binary.c_str(), args.data());

        const std::string message = std::string("execv failed: ") + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
    }

    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);
    if (input) ::close(stdinPipe[0]);

    if (input) {
        std::size_t offset = 0;
        while (offset < input->size()) {
            const ssize_t written = ::write(
                stdinPipe[1],
                input->data() + offset,
                input->size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            break;
        }
        ::close(stdinPipe[1]);
    }

    std::array<unsigned char, 65536> buffer{};
    for (;;) {
        const ssize_t count = ::read(stdoutPipe[0], buffer.data(), buffer.size());
        if (count > 0) {
            const std::size_t bytes = static_cast<std::size_t>(count);
            if (result.output.size() < maxOutputBytes) {
                const std::size_t available = maxOutputBytes - result.output.size();
                const std::size_t keep = std::min(available, bytes);
                result.output.insert(
                    result.output.end(),
                    buffer.begin(),
                    buffer.begin() + static_cast<std::ptrdiff_t>(keep));
                if (keep < bytes) result.truncated = true;
            } else {
                result.truncated = true;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    ::close(stdoutPipe[0]);

    std::array<char, 4096> errorBuffer{};
    for (;;) {
        const ssize_t count = ::read(stderrPipe[0], errorBuffer.data(), errorBuffer.size());
        if (count > 0) {
            if (result.error.size() < 65536) {
                result.error.append(errorBuffer.data(), static_cast<std::size_t>(count));
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    ::close(stderrPipe[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    else result.exitCode = 1;

    return result;
}

json helperJson(const HelperResult& helper) {
    if (helper.exitCode != 0) {
        throw std::runtime_error(
            helper.error.empty() ? "File operation failed." : helper.error);
    }
    const std::string text(
        reinterpret_cast<const char*>(helper.output.data()),
        helper.output.size());
    auto result = json::parse(text, nullptr, false);
    if (!result.is_object()) throw std::runtime_error("File helper returned invalid data.");
    if (!result.value("ok", false)) {
        throw std::runtime_error(result.value("error", "File operation failed."));
    }
    return result;
}

std::string shellForRequest(
    const ExecutionContext& context,
    const std::string& request) {

    if (request == "bash" && fs::exists("/bin/bash")) return "/bin/bash";
    if (request == "zsh" && fs::exists("/bin/zsh")) return "/bin/zsh";
    if (request == "sh" && fs::exists("/bin/sh")) return "/bin/sh";
    if (!context.shell.empty() && fs::exists(context.shell)) return context.shell;
    return defaultRootShell();
}

} // namespace

struct PortableLiveTools::TerminalSession {
    std::string sessionId;
    std::string runAs;
    std::string username;
    pid_t pid = -1;
    int masterFd = -1;
    std::atomic<bool> running{false};
    std::atomic<bool> closedSent{false};
    std::thread reader;
};

PortableLiveTools::PortableLiveTools(SendFunction send)
    : send_(std::move(send)) {}

PortableLiveTools::~PortableLiveTools() {
    stopAll();
}

void PortableLiveTools::sendTerminal(
    const std::string& type,
    const std::string& sessionId,
    const std::string& text) {

    if (!send_) return;
    json message = {
        {"type", type},
        {"sessionId", sessionId},
        {"session_id", sessionId}
    };
    if (type == "terminal_output") message["data"] = text;
    if (type == "terminal_error") message["error"] = text;
    send_(message);
}

void PortableLiveTools::sendFilesError(
    const std::string& sessionId,
    const std::string& path,
    const std::string& error) {

    if (!send_) return;
    send_({
        {"type", "files_error"},
        {"sessionId", sessionId},
        {"session_id", sessionId},
        {"path", path},
        {"error", error}
    });
}

bool PortableLiveTools::handleMessage(const json& message) {
    const std::string type = message.value("type", "");
    if (type.rfind("terminal_", 0) == 0) {
        handleTerminalMessage(message);
        return true;
    }
    if (type.rfind("files_", 0) == 0) {
        handleFilesMessage(message);
        return true;
    }
    return false;
}

void PortableLiveTools::handleTerminalMessage(const json& message) {
    const std::string type = message.value("type", "");
    if (type == "terminal_start") startTerminal(message);
    else if (type == "terminal_input") terminalInput(message);
    else if (type == "terminal_resize") terminalResize(message);
    else if (type == "terminal_stop") stopTerminal(messageSessionId(message));
}

void PortableLiveTools::startTerminal(const json& message) {
    const std::string sessionId = messageSessionId(message);
    if (sessionId.empty()) return;

    const std::string runAs = messageRunAs(message);
    const ExecutionContext context = resolveExecutionContext(runAs);
    if (!context.valid) {
        sendTerminal("terminal_error", sessionId, context.error);
        return;
    }

    stopTerminal(sessionId, false);

    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(std::clamp(message.value("cols", 120), 20, 300));
    size.ws_row = static_cast<unsigned short>(std::clamp(message.value("rows", 32), 5, 100));

    int masterFd = -1;
    const pid_t child = ::forkpty(&masterFd, nullptr, nullptr, &size);
    if (child < 0) {
        sendTerminal(
            "terminal_error",
            sessionId,
            std::string("Could not create terminal: ") + std::strerror(errno));
        return;
    }

    if (child == 0) {
        std::string contextError;
        if (!applyExecutionContext(context, &contextError)) {
            const std::string messageText = contextError + "\r\n";
            ::write(STDERR_FILENO, messageText.data(), messageText.size());
            _exit(126);
        }

        const std::string shell = shellForRequest(
            context,
            message.value("shell", std::string("shell")));

        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("HOME", context.home.c_str(), 1);
        ::setenv("USER", context.username.c_str(), 1);
        ::setenv("LOGNAME", context.username.c_str(), 1);
        ::setenv("SHELL", shell.c_str(), 1);

        ::execl(shell.c_str(), shell.c_str(), "-i", static_cast<char*>(nullptr));
        const std::string error =
            std::string("Could not start shell: ") + std::strerror(errno) + "\r\n";
        ::write(STDERR_FILENO, error.data(), error.size());
        _exit(127);
    }

    auto session = std::make_shared<TerminalSession>();
    session->sessionId = sessionId;
    session->runAs = context.mode;
    session->username = context.username;
    session->pid = child;
    session->masterFd = masterFd;
    session->running.store(true);

    {
        std::lock_guard<std::mutex> lock(terminalsMutex_);
        terminals_[sessionId] = session;
    }

    session->reader = std::thread([this, session]() {
        std::array<char, 8192> buffer{};
        while (session->running.load()) {
            const ssize_t count = ::read(
                session->masterFd,
                buffer.data(),
                buffer.size());

            if (count > 0) {
                sendTerminal(
                    "terminal_output",
                    session->sessionId,
                    std::string(buffer.data(), static_cast<std::size_t>(count)));
                continue;
            }

            if (count < 0 && errno == EINTR) continue;
            break;
        }

        session->running.store(false);
        if (!session->closedSent.exchange(true)) {
            sendTerminal("terminal_closed", session->sessionId);
        }
    });
}

void PortableLiveTools::terminalInput(const json& message) {
    const std::string sessionId = messageSessionId(message);
    const std::string data = message.value("data", "");
    if (sessionId.empty() || data.empty()) return;

    std::shared_ptr<TerminalSession> session;
    {
        std::lock_guard<std::mutex> lock(terminalsMutex_);
        const auto iterator = terminals_.find(sessionId);
        if (iterator == terminals_.end()) {
            sendTerminal("terminal_error", sessionId, "Terminal session not found.");
            return;
        }
        session = iterator->second;
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(
            session->masterFd,
            data.data() + offset,
            data.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        sendTerminal(
            "terminal_error",
            sessionId,
            std::string("Terminal input failed: ") + std::strerror(errno));
        return;
    }
}

void PortableLiveTools::terminalResize(const json& message) {
    const std::string sessionId = messageSessionId(message);
    std::shared_ptr<TerminalSession> session;
    {
        std::lock_guard<std::mutex> lock(terminalsMutex_);
        const auto iterator = terminals_.find(sessionId);
        if (iterator == terminals_.end()) return;
        session = iterator->second;
    }

    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(std::clamp(message.value("cols", 120), 20, 300));
    size.ws_row = static_cast<unsigned short>(std::clamp(message.value("rows", 32), 5, 100));
    (void)::ioctl(session->masterFd, TIOCSWINSZ, &size);
}

void PortableLiveTools::stopTerminal(const std::string& sessionId, bool notify) {
    if (sessionId.empty()) return;

    std::shared_ptr<TerminalSession> session;
    {
        std::lock_guard<std::mutex> lock(terminalsMutex_);
        const auto iterator = terminals_.find(sessionId);
        if (iterator == terminals_.end()) return;
        session = iterator->second;
        terminals_.erase(iterator);
    }

    session->running.store(false);

    if (session->pid > 0) {
        ::kill(-session->pid, SIGHUP);
        ::kill(session->pid, SIGHUP);

        int status = 0;
        for (int attempt = 0; attempt < 10; ++attempt) {
            const pid_t waited = ::waitpid(session->pid, &status, WNOHANG);
            if (waited == session->pid) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (::waitpid(session->pid, &status, WNOHANG) == 0) {
            ::kill(-session->pid, SIGKILL);
            ::kill(session->pid, SIGKILL);
            while (::waitpid(session->pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }

    if (session->masterFd >= 0) {
        ::close(session->masterFd);
        session->masterFd = -1;
    }

    if (session->reader.joinable()) session->reader.join();

    if (notify && !session->closedSent.exchange(true)) {
        sendTerminal("terminal_closed", sessionId);
    }
}

void PortableLiveTools::stopAll() {
    std::vector<std::string> sessions;
    {
        std::lock_guard<std::mutex> lock(terminalsMutex_);
        for (const auto& [id, value] : terminals_) sessions.push_back(id);
    }
    for (const auto& id : sessions) stopTerminal(id, false);

    std::lock_guard<std::mutex> uploadLock(uploadsMutex_);
    uploads_.clear();
}

void PortableLiveTools::handleFilesMessage(const json& message) {
    const std::string type = message.value("type", "");
    if (type == "files_list") filesList(message);
    else if (type == "files_mkdir_request" ||
             type == "files_rename_request" ||
             type == "files_delete_request") filesAction(message);
    else if (type == "files_download_request") filesDownload(message);
    else if (type == "files_upload_start" ||
             type == "files_upload_chunk" ||
             type == "files_upload_complete" ||
             type == "files_upload_cancel") filesUpload(message);
    else if (type == "files_cancel" || type == "files_download_cancel") {
        // Downloads are bounded and short-lived. Browser cancellation simply
        // stops consuming future chunks; no persistent remote state is kept.
    }
}

void PortableLiveTools::filesList(const json& message) {
    const std::string sessionId = messageSessionId(message);
    const std::string path = message.value("path", std::string("/"));
    if (sessionId.empty()) return;

    const ExecutionContext context = resolveExecutionContext(messageRunAs(message));
    if (!context.valid) {
        sendFilesError(sessionId, path, context.error);
        return;
    }

    std::thread([this, sessionId, path, context]() {
        try {
            const auto helper = runHelper(
                context,
                "list",
                {{"path", path}, {"home", context.home}});
            const auto result = helperJson(helper);

            send_({
                {"type", "files_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"path", result.value("path", path)},
                {"parent", result.value("parent", "")},
                {"entries", result.value("entries", json::array())},
                {"drives", result.value("drives", json::array())},
                {"count", result.value("count", 0)},
                {"result", result}
            });
        } catch (const std::exception& error) {
            sendFilesError(sessionId, path, error.what());
        }
    }).detach();
}

void PortableLiveTools::filesAction(const json& message) {
    const std::string sessionId = messageSessionId(message);
    const std::string type = message.value("type", "");
    if (sessionId.empty()) return;

    const ExecutionContext context = resolveExecutionContext(messageRunAs(message));
    if (!context.valid) {
        sendFilesError(sessionId, message.value("path", ""), context.error);
        return;
    }

    std::string operation;
    if (type == "files_mkdir_request") operation = "mkdir";
    else if (type == "files_rename_request") operation = "rename";
    else if (type == "files_delete_request") operation = "delete";
    else return;

    json payload = {
        {"path", message.value("path", "")},
        {"current_path", message.value(
            "currentPath",
            message.value("current_path", std::string("/")))},
        {"name", message.value("name", "")}
    };

    std::thread([this, sessionId, type, operation, payload, context]() {
        try {
            const auto result = helperJson(runHelper(context, operation, payload));
            const std::string refreshPath = result.value("refresh_path", "/");
            send_({
                {"type", "files_action_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"action", type},
                {"success", true},
                {"refreshPath", refreshPath},
                {"refresh_path", refreshPath},
                {"message", result.value("message", "Completed")}
            });
        } catch (const std::exception& error) {
            send_({
                {"type", "files_action_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"action", type},
                {"success", false},
                {"refreshPath", payload.value("current_path", "/")},
                {"refresh_path", payload.value("current_path", "/")},
                {"error", error.what()}
            });
        }
    }).detach();
}

void PortableLiveTools::filesDownload(const json& message) {
    const std::string sessionId = messageSessionId(message);
    const std::string transferId = messageTransferId(message);
    const std::string path = message.value("path", "");
    if (sessionId.empty() || transferId.empty() || path.empty()) return;

    const ExecutionContext context = resolveExecutionContext(messageRunAs(message));
    if (!context.valid) {
        sendFilesError(sessionId, path, context.error);
        return;
    }

    std::thread([this, sessionId, transferId, path, context]() {
        try {
            const auto helper = runHelper(
                context,
                "read",
                {{"path", path}, {"max_bytes", kMaxTransferBytes}},
                nullptr,
                kMaxTransferBytes + 1);

            if (helper.exitCode != 0) {
                throw std::runtime_error(
                    helper.error.empty() ? "Could not read file." : helper.error);
            }
            if (helper.truncated || helper.output.size() > kMaxTransferBytes) {
                throw std::runtime_error("Browser download is limited to 256 MB.");
            }

            const std::string filename = fs::path(path).filename().string();
            send_({
                {"type", "files_download_start"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"transferId", transferId},
                {"transfer_id", transferId},
                {"path", path},
                {"filename", filename.empty() ? "download.bin" : filename},
                {"size", helper.output.size()},
                {"size_bytes", helper.output.size()}
            });

            std::size_t offset = 0;
            int index = 0;
            while (offset < helper.output.size()) {
                const std::size_t count = std::min(
                    kDownloadChunkBytes,
                    helper.output.size() - offset);
                const std::string encoded = base64Encode(
                    helper.output.data() + offset,
                    count);

                send_({
                    {"type", "files_download_chunk"},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"transferId", transferId},
                    {"transfer_id", transferId},
                    {"index", index++},
                    {"data", encoded}
                });
                offset += count;
            }

            send_({
                {"type", "files_download_complete"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"transferId", transferId},
                {"transfer_id", transferId},
                {"path", path},
                {"filename", filename.empty() ? "download.bin" : filename},
                {"size", helper.output.size()},
                {"size_bytes", helper.output.size()}
            });
        } catch (const std::exception& error) {
            sendFilesError(sessionId, path, error.what());
        }
    }).detach();
}

void PortableLiveTools::filesUpload(const json& message) {
    const std::string type = message.value("type", "");
    const std::string sessionId = messageSessionId(message);
    const std::string transferId = messageTransferId(message);
    if (sessionId.empty() || transferId.empty()) return;

    if (type == "files_upload_start") {
        const std::string filename = message.value("filename", "");
        if (!validLeafName(filename)) {
            send_({
                {"type", "files_upload_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"transferId", transferId},
                {"transfer_id", transferId},
                {"success", false},
                {"error", "Invalid filename."}
            });
            return;
        }

        const auto expected = message.value(
            "size_bytes",
            message.value("size", static_cast<std::uint64_t>(0)));
        if (expected > kMaxTransferBytes) {
            send_({
                {"type", "files_upload_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"transferId", transferId},
                {"transfer_id", transferId},
                {"success", false},
                {"error", "Browser upload is limited to 256 MB."}
            });
            return;
        }

        const ExecutionContext context = resolveExecutionContext(messageRunAs(message));
        if (!context.valid) {
            send_({
                {"type", "files_upload_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"transferId", transferId},
                {"transfer_id", transferId},
                {"success", false},
                {"error", context.error}
            });
            return;
        }

        UploadState state;
        state.sessionId = sessionId;
        state.transferId = transferId;
        state.directory = message.value("directory", message.value("path", std::string("/")));
        state.filename = filename;
        state.runAs = context.mode;
        state.expected = expected;
        if (expected > 0) state.bytes.reserve(static_cast<std::size_t>(expected));

        {
            std::lock_guard<std::mutex> lock(uploadsMutex_);
            uploads_[transferId] = std::move(state);
        }

        send_({
            {"type", "files_upload_progress"},
            {"sessionId", sessionId},
            {"session_id", sessionId},
            {"transferId", transferId},
            {"transfer_id", transferId},
            {"filename", filename},
            {"receivedBytes", 0},
            {"received_bytes", 0},
            {"size", expected},
            {"size_bytes", expected}
        });
        return;
    }

    if (type == "files_upload_cancel") {
        std::lock_guard<std::mutex> lock(uploadsMutex_);
        uploads_.erase(transferId);
        return;
    }

    if (type == "files_upload_chunk") {
        const auto decoded = base64DecodeBytes(message.value("data", ""));
        std::lock_guard<std::mutex> lock(uploadsMutex_);
        const auto iterator = uploads_.find(transferId);
        if (iterator == uploads_.end()) return;

        if (iterator->second.bytes.size() + decoded.size() > kMaxTransferBytes) {
            uploads_.erase(iterator);
            send_({
                {"type", "files_upload_result"},
                {"sessionId", sessionId},
                {"session_id", sessionId},
                {"transferId", transferId},
                {"transfer_id", transferId},
                {"success", false},
                {"error", "Upload exceeded the 256 MB browser limit."}
            });
            return;
        }

        iterator->second.bytes.insert(
            iterator->second.bytes.end(),
            decoded.begin(),
            decoded.end());

        send_({
            {"type", "files_upload_progress"},
            {"sessionId", sessionId},
            {"session_id", sessionId},
            {"transferId", transferId},
            {"transfer_id", transferId},
            {"filename", iterator->second.filename},
            {"receivedBytes", iterator->second.bytes.size()},
            {"received_bytes", iterator->second.bytes.size()},
            {"size", iterator->second.expected},
            {"size_bytes", iterator->second.expected}
        });
        return;
    }

    if (type == "files_upload_complete") {
        UploadState state;
        {
            std::lock_guard<std::mutex> lock(uploadsMutex_);
            const auto iterator = uploads_.find(transferId);
            if (iterator == uploads_.end()) return;
            state = std::move(iterator->second);
            uploads_.erase(iterator);
        }

        std::thread([this, state = std::move(state)]() mutable {
            try {
                if (state.expected > 0 && state.bytes.size() != state.expected) {
                    throw std::runtime_error(
                        "Upload incomplete. Received " +
                        std::to_string(state.bytes.size()) +
                        " of " +
                        std::to_string(state.expected) +
                        " bytes.");
                }

                const ExecutionContext context = resolveExecutionContext(state.runAs);
                if (!context.valid) throw std::runtime_error(context.error);

                const auto helper = runHelper(
                    context,
                    "write",
                    {
                        {"directory", state.directory},
                        {"filename", state.filename},
                        {"transfer_id", state.transferId},
                        {"expected", state.bytes.size()}
                    },
                    &state.bytes,
                    1024);

                if (helper.exitCode != 0) {
                    throw std::runtime_error(
                        helper.error.empty() ? "Could not write uploaded file." : helper.error);
                }

                send_({
                    {"type", "files_upload_result"},
                    {"sessionId", state.sessionId},
                    {"session_id", state.sessionId},
                    {"transferId", state.transferId},
                    {"transfer_id", state.transferId},
                    {"filename", state.filename},
                    {"success", true},
                    {"refreshPath", state.directory},
                    {"refresh_path", state.directory},
                    {"message", "Upload complete"}
                });
            } catch (const std::exception& error) {
                send_({
                    {"type", "files_upload_result"},
                    {"sessionId", state.sessionId},
                    {"session_id", state.sessionId},
                    {"transferId", state.transferId},
                    {"transfer_id", state.transferId},
                    {"filename", state.filename},
                    {"success", false},
                    {"refreshPath", state.directory},
                    {"refresh_path", state.directory},
                    {"error", error.what()}
                });
            }
        }).detach();
    }
}

int PortableLiveTools::runInternalHelper(int argc, char* argv[]) {
    if (argc < 2 || !argv || !argv[1] || std::string(argv[1]) != "--hi5-file-helper") {
        return -1;
    }

    if (argc < 5 || !argv[2] || !argv[3] || !argv[4]) {
        std::cerr << "Missing file helper operation, payload or execution context." << std::endl;
        return 2;
    }

    const std::string operation = argv[2];
    const std::string payloadText = base64DecodeString(argv[3]);
    const std::string runAs = argv[4];

    const ExecutionContext context = resolveExecutionContext(runAs);
    if (!context.valid) {
        std::cerr << (context.error.empty() ? "Invalid execution context." : context.error) << std::endl;
        return 126;
    }

    std::string contextError;
    if (!applyExecutionContext(context, &contextError)) {
        std::cerr << contextError << std::endl;
        return 126;
    }
    const auto payload = json::parse(payloadText, nullptr, false);
    if (!payload.is_object()) {
        std::cerr << "Invalid file helper payload." << std::endl;
        return 2;
    }

    try {
        if (operation == "list") {
            return writeJsonResult(listDirectory(payload));
        }

        if (operation == "mkdir" || operation == "rename" || operation == "delete") {
            return writeJsonResult(performFileAction(operation, payload));
        }

        if (operation == "read") {
            fs::path path = payload.value("path", "");
            if (path.empty()) throw std::runtime_error("File path is required.");

            std::error_code ec;
            if (!fs::exists(path, ec) || ec) throw std::runtime_error("File does not exist.");
            if (!fs::is_regular_file(path, ec) || ec) throw std::runtime_error("Path is not a regular file.");

            const auto size = fs::file_size(path, ec);
            if (ec) throw std::runtime_error("Could not inspect file size: " + ec.message());
            const auto maxBytes = payload.value(
                "max_bytes",
                static_cast<std::uint64_t>(kMaxTransferBytes));
            if (size > maxBytes) throw std::runtime_error("Browser download is limited to 256 MB.");

            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Could not open file for reading.");

            std::array<char, 65536> buffer{};
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0) {
                    std::cout.write(buffer.data(), count);
                    if (!std::cout) throw std::runtime_error("Could not stream file data.");
                }
            }
            std::cout.flush();
            return 0;
        }

        if (operation == "write") {
            const std::string filename = payload.value("filename", "");
            if (!validLeafName(filename)) throw std::runtime_error("Invalid filename.");

            fs::path directory = payload.value("directory", std::string("/"));
            fs::path finalPath = directory / filename;
            const std::string transfer = payload.value("transfer_id", "upload");
            fs::path tempPath = directory / ("." + filename + ".hi5upload-" + transfer + ".tmp");

            std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Could not create upload temporary file.");

            std::uint64_t received = 0;
            std::array<char, 65536> buffer{};
            while (std::cin) {
                std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = std::cin.gcount();
                if (count > 0) {
                    output.write(buffer.data(), count);
                    if (!output) throw std::runtime_error("Could not write uploaded data.");
                    received += static_cast<std::uint64_t>(count);
                    if (received > kMaxTransferBytes) {
                        throw std::runtime_error("Upload exceeded the 256 MB browser limit.");
                    }
                }
            }
            output.flush();
            output.close();

            const auto expected = payload.value("expected", received);
            if (received != expected) {
                std::error_code removeError;
                fs::remove(tempPath, removeError);
                throw std::runtime_error("Uploaded byte count did not match expected size.");
            }

            std::error_code ec;
            fs::rename(tempPath, finalPath, ec);
            if (ec) {
                std::error_code removeError;
                fs::remove(tempPath, removeError);
                throw std::runtime_error("Could not finalise uploaded file: " + ec.message());
            }
            return 0;
        }

        throw std::runtime_error("Unknown file helper operation.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

} // namespace hi5
