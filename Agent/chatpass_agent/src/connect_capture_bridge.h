#pragma once

#ifdef _WIN32

#include "ipc/input_pipe.h"
#include "ipc/shmem_ring.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class WebRtcSender;

namespace hi5 {

class ConnectCaptureBridge {
public:
    using StateCallback = std::function<void(const std::string&)>;

    ConnectCaptureBridge() = default;
    ~ConnectCaptureBridge();

    bool Start(const std::string& sessionId, int fps, int displayIndex = 0,
        const std::string& connectTicket = {});
    bool AttachExisting(
        const std::string& sessionId,
        const std::string& serviceName,
        const std::string& normalShmem,
        const std::string& secureShmem,
        const std::string& normalInput,
        const std::string& secureInput,
        const std::string& normalStop,
        const std::string& secureStop,
        const std::string& brokerStop,
        const std::string& loginDesktop,
        const std::string& cadRequest,
        const std::string& cadSuccess,
        const std::string& cadFailure);
    void Stop(bool stopBroker = true);

    bool StartPump(WebRtcSender* sender, StateCallback stateCallback);
    void StopPump();

    bool HandleInputEvent(const nlohmann::json& msg);
    bool RequestSecureAttention();
    bool HandleFastMouse(double xNorm, double yNorm, uint64_t seq, double clientTsMs);
    bool SwitchMonitor(int index);
    nlohmann::json BuildMonitorInfoMessage(const std::string& sessionId) const;

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

private:
    bool CreateSharedObjects(const std::string& prefix);
    bool OpenSharedObjects();
    bool InstallAndStartBroker();
    void RemoveBrokerService();
    void PumpLoop();
    InputPipeWriter& ActiveInputPipe();
    const InputPipeWriter& ActiveInputPipe() const;
    bool WriteText(InputPipeWriter& pipe, const std::string& text, InputCmdType type);
    bool WriteKey(InputPipeWriter& pipe, const std::string& code, bool down);
    bool WriteShortcut(InputPipeWriter& pipe, ShortcutAction action);
    bool WriteMouseMove(InputPipeWriter& pipe, double xNorm, double yNorm);
    bool WriteMouseButton(InputPipeWriter& pipe, uint8_t button, bool down);

private:
    std::string sessionId_;
    std::string serviceName_;
    std::string normalShmemName_;
    std::string secureShmemName_;
    std::string normalInputName_;
    std::string secureInputName_;
    std::string normalStopName_;
    std::string secureStopName_;
    std::string brokerStopName_;
    std::string loginDesktopName_;
    std::string cadRequestName_;
    std::string cadSuccessName_;
    std::string cadFailureName_;
    std::string connectTicket_;
    bool ownsBroker_{ false };

    ShmemRing normalShmem_;
    ShmemRing secureShmem_;
    InputPipeWriter normalInput_;
    InputPipeWriter secureInput_;

    HANDLE normalStopEvent_{ nullptr };
    HANDLE secureStopEvent_{ nullptr };
    HANDLE brokerStopEvent_{ nullptr };
    HANDLE loginDesktopEvent_{ nullptr };
    HANDLE cadRequestEvent_{ nullptr };
    HANDLE cadSuccessEvent_{ nullptr };
    HANDLE cadFailureEvent_{ nullptr };

    std::atomic<bool> running_{ false };
    std::atomic<bool> pumpRunning_{ false };
    std::atomic<bool> secureDesktopActive_{ false };
    std::atomic<bool> secureFallbackReady_{ false };
    std::atomic<int> displayIndex_{ 0 };
    int fps_{ 30 };

    std::thread pumpThread_;
    WebRtcSender* sender_{ nullptr };
    StateCallback stateCallback_;
    mutable std::mutex callbackMu_;

    I420Frame cachedVisibleFrame_;
    uint64_t cachedVisibleTimestampNs_{ 0 };
    mutable std::mutex frameCacheMu_;
};

int RunConnectCaptureBrokerService(int argc, char** argv);

} // namespace hi5

#endif // _WIN32
