#include "webrtc_sender.h"
#include "ipc/named_pipe.h"
#include "ipc/shmem_ring.h"
#include "util/log.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace hi5 {
namespace {

std::string ArgValue(int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return std::string(argv[i + 1]);
    }
    return fallback;
}
std::vector<std::string> ArgValues(int argc, char** argv, const std::string& name) {
    std::vector<std::string> out;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) out.emplace_back(argv[i + 1]);
    }
    return out;
}

int ArgInt(int argc, char** argv, const std::string& name, int fallback) {
    const std::string value = ArgValue(argc, argv, name);
    if (value.empty()) return fallback;
    try { return std::stoi(value); }
    catch (...) { return fallback; }
}

bool OpenConsumerWithRetry(ShmemRing& ring, const std::string& name) {
    for (int i = 0; i < 80; ++i) {
        if (ring.OpenConsumer(name)) return true;
        Sleep(125);
    }
    return false;
}

void LogMediaMemory(const std::string& sessionId, const char* stage) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return;
    }
    constexpr double kMb = 1024.0 * 1024.0;
    LogInfo("[media-memory] session=" + sessionId +
        " stage=" + std::string(stage) +
        " working_set_mb=" + std::to_string(static_cast<double>(pmc.WorkingSetSize) / kMb) +
        " private_mb=" + std::to_string(static_cast<double>(pmc.PrivateUsage) / kMb) +
        " peak_ws_mb=" + std::to_string(static_cast<double>(pmc.PeakWorkingSetSize) / kMb));
}

} // namespace

int RunMediaHostMain(int argc, char** argv) {
    const std::string sessionId = ArgValue(argc, argv, "--session");
    const std::string shmemName = ArgValue(argc, argv, "--shmem");
    const std::string controlPipeName = ArgValue(argc, argv, "--control-pipe");
    const std::string eventPipeName = ArgValue(argc, argv, "--event-pipe");
    const std::string stopEventName = ArgValue(argc, argv, "--stop-event");
    const std::string codecMode = ArgValue(argc, argv, "--codec", "auto");
    const int width = ArgInt(argc, argv, "--width", 1920);
    const int height = ArgInt(argc, argv, "--height", 1080);
    const int fps = ArgInt(argc, argv, "--fps", 30);
    const int bitrateKbps = ArgInt(argc, argv, "--bitrate", 8000);
    const bool enableAudio = ArgInt(argc, argv, "--audio", 1) != 0;
    const auto iceServers = ArgValues(argc, argv, "--ice-server");

    if (sessionId.empty() || shmemName.empty() || controlPipeName.empty() ||
        eventPipeName.empty() || stopEventName.empty()) {
        LogError("[media-host] missing required arguments");
        return 2;
    }

    LogMediaMemory(sessionId, "process-start");
    rtc::InitLogger(rtc::LogLevel::Info);

    ShmemRing frameRing;
    if (!OpenConsumerWithRetry(frameRing, shmemName)) {
        LogError("[media-host] failed to open frame ring session=" + sessionId + " name=" + shmemName);
        return 3;
    }

    HANDLE stopEvent = OpenEventA(SYNCHRONIZE, FALSE, stopEventName.c_str());
    if (!stopEvent) {
        LogError("[media-host] failed to open stop event session=" + sessionId +
            " err=" + std::to_string(GetLastError()));
        return 4;
    }

    NamedPipeClient eventPipe;
    std::mutex eventMu;
    auto sendEvent = [&](const json& message) -> bool {
        std::lock_guard<std::mutex> lock(eventMu);
        return eventPipe.SendLine(message.dump());
    };

    NamedPipeServer controlServer;
    std::atomic<WebRtcSender*> senderPtr{ nullptr };
    std::atomic<bool> startRequested{ false };
    std::atomic<bool> closed{ false };

    controlServer.Start(controlPipeName, [&](const std::string& raw) {
        const auto message = json::parse(raw, nullptr, false);
        if (message.is_discarded()) return;
        WebRtcSender* sender = senderPtr.load(std::memory_order_acquire);
        if (!sender) return;

        const std::string type = message.value("type", std::string());
        if (type == "start") {
            startRequested.store(true, std::memory_order_release);
            return;
        }
        if (type == "signal") {
            const std::string payload = message.value("payload", std::string());
            if (!payload.empty()) sender->handleSignalingMessage(payload);
            return;
        }
        if (type == "stream_hint") {
            sender->setExternalStreamHint(
                message.value("stream_mode", 0),
                message.value("target_fps", fps),
                message.value("backstage", false),
                message.value("secure", false));
        }
    });

    if (!eventPipe.Connect(eventPipeName, 80, 125)) {
        LogError("[media-host] failed to connect event pipe session=" + sessionId);
        controlServer.Stop();
        CloseHandle(stopEvent);
        return 5;
    }

    LogMediaMemory(sessionId, "ipc-ready");

    WebRtcSender sender(
        sessionId,
        iceServers,
        [&](const std::string& payload) {
            sendEvent(json{ {"type", "signal"}, {"payload", payload} });
        },
        width,
        height,
        fps,
        bitrateKbps,
        WebRtcSender::Mode::ExternalFeed,
        codecMode,
        enableAudio);

    LogMediaMemory(sessionId, "sender-constructed");

    sender.setInputEventHandler([&](const json& input) {
        sendEvent(json{ {"type", "input_event"}, {"payload", input} });
    });

    sender.setDirectMouseMoveHandler([&](double xNorm, double yNorm, uint64_t seq, double clientTsMs) {
        return sendEvent(json{
            {"type", "mouse_move"},
            {"x_norm", xNorm},
            {"y_norm", yNorm},
            {"seq", seq},
            {"client_ts", clientTsMs}
        });
    });
    sender.setConnectionClosedHandler([&](const std::string& reason) {
        sendEvent(json{ {"type", "closed"}, {"reason", reason} });
        closed.store(true, std::memory_order_release);
    });

    senderPtr.store(&sender, std::memory_order_release);
    sendEvent(json{ {"type", "ready"}, {"session_id", sessionId} });

    LogInfo("[media-host] ready session=" + sessionId +
        " codec=" + codecMode +
        " audio=" + std::string(enableAudio ? "1" : "0"));

    bool senderStarted = false;
    bool firstFrameLogged = false;
    auto nextMemoryLog = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    I420Frame frame;
    uint64_t frameTsNs = 0;
    while (!closed.load(std::memory_order_acquire) &&
        WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        if (!senderStarted && startRequested.exchange(false, std::memory_order_acq_rel)) {
            sender.start();
            senderStarted = true;
            LogInfo("[media-host] WebRTC started session=" + sessionId);
            LogMediaMemory(sessionId, "sender-started");
        }
        if (!senderStarted) {
            Sleep(2);
            continue;
        }

        bool gotFrame = false;
        bool forceKeyframe = false;
        bool thisForceKeyframe = false;

        while (frameRing.ReadRawI420Frame(frame, frameTsNs, &thisForceKeyframe)) {
            gotFrame = true;
            forceKeyframe = forceKeyframe || thisForceKeyframe;
            if (!frameRing.HasFrame()) break;
        }

        if (gotFrame) {
            sender.sendExternalRawI420(frame, frameTsNs, forceKeyframe);
            if (!firstFrameLogged) {
                firstFrameLogged = true;
                LogMediaMemory(sessionId, "first-frame");
            }
        }
        else {
            Sleep(2);
        }

        const auto memoryNow = std::chrono::steady_clock::now();
        if (memoryNow >= nextMemoryLog) {
            LogMediaMemory(sessionId, "steady");
            nextMemoryLog = memoryNow + std::chrono::seconds(5);
        }
    }
    senderPtr.store(nullptr, std::memory_order_release);
    if (senderStarted) sender.stop();
    controlServer.Stop();
    eventPipe.Close();
    frameRing.Close();
    CloseHandle(stopEvent);

    LogInfo("[media-host] stopped session=" + sessionId);
    return 0;
}

} // namespace hi5
