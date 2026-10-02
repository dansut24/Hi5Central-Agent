#include "job_executor.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hi5 {
namespace {

void appendOutput(
    std::string& output,
    const char* buffer,
    std::size_t length,
    std::size_t maxOutputBytes,
    bool& truncated) {

    if (length == 0) return;
    if (output.size() >= maxOutputBytes) {
        truncated = true;
        return;
    }

    const std::size_t available = maxOutputBytes - output.size();
    const std::size_t toAppend = std::min(available, length);
    output.append(buffer, toAppend);
    if (toAppend < length) truncated = true;
}

void drainPipe(
    int fd,
    std::string& output,
    std::size_t maxOutputBytes,
    bool& truncated) {

    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            appendOutput(
                output,
                buffer,
                static_cast<std::size_t>(count),
                maxOutputBytes,
                truncated);
            continue;
        }

        if (count == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
        break;
    }
}

} // namespace

CommandResult runShellCommand(
    const std::string& command,
    int timeoutSeconds,
    std::size_t maxOutputBytes) {

    CommandResult result;
    if (command.empty()) {
        result.error = "Command is empty";
        return result;
    }

    timeoutSeconds = std::clamp(timeoutSeconds, 5, 3600);
    maxOutputBytes = std::clamp<std::size_t>(maxOutputBytes, 4096, 1024 * 1024);

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        result.error = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    if (child < 0) {
        result.error = std::string("fork failed: ") + std::strerror(errno);
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return result;
    }

    if (child == 0) {
        ::setpgid(0, 0);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(pipeFds[1]);
    ::setpgid(child, child);

    const int originalFlags = ::fcntl(pipeFds[0], F_GETFL, 0);
    if (originalFlags >= 0) {
        ::fcntl(pipeFds[0], F_SETFL, originalFlags | O_NONBLOCK);
    }

    int waitStatus = 0;
    bool childExited = false;

    while (!childExited) {
        struct pollfd descriptor {};
        descriptor.fd = pipeFds[0];
        descriptor.events = POLLIN | POLLHUP;

        const int pollResult = ::poll(&descriptor, 1, 100);
        if (pollResult > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
            drainPipe(
                pipeFds[0],
                result.output,
                maxOutputBytes,
                result.truncated);
        }

        const pid_t waited = ::waitpid(child, &waitStatus, WNOHANG);
        if (waited == child) {
            childExited = true;
            break;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started);

        if (elapsed.count() >= timeoutSeconds) {
            result.timedOut = true;
            result.error = "Command timed out";
            ::kill(-child, SIGKILL);
            ::kill(child, SIGKILL);
            ::waitpid(child, &waitStatus, 0);
            childExited = true;
            break;
        }
    }

    drainPipe(
        pipeFds[0],
        result.output,
        maxOutputBytes,
        result.truncated);
    ::close(pipeFds[0]);

    if (result.timedOut) {
        result.exitCode = 124;
    } else if (WIFEXITED(waitStatus)) {
        result.exitCode = WEXITSTATUS(waitStatus);
    } else if (WIFSIGNALED(waitStatus)) {
        result.exitCode = 128 + WTERMSIG(waitStatus);
    } else {
        result.exitCode = 1;
    }

    result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    return result;
}

nlohmann::json buildCommandResultJson(
    const std::string& command,
    const CommandResult& result) {

    using json = nlohmann::json;

    json payload = {
        {"exit_code", result.exitCode},
        {"duration_ms", result.durationMs},
        {"output", result.output},
        {"timed_out", result.timedOut},
        {"truncated", result.truncated}
    };

    if (!command.empty()) payload["command"] = command;
    if (!result.error.empty()) payload["error"] = result.error;

    auto parsed = json::parse(result.output, nullptr, false);
    if (!parsed.is_discarded()) {
        payload["parsed"] = parsed;
        if (parsed.is_object()) {
            for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                payload[it.key()] = it.value();
            }
        }
    }

    return payload;
}

} // namespace hi5
