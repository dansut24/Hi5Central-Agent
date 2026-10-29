#pragma once

#include "vp8_encoder.h"
#include "vp9_mf_encoder.h"
#include "encoding/sustained_encoder_health.h"
#include "h264_mf_encoder.h"
#include "input_injector.h"

#include <rtc/rtc.hpp>
#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DisplayInfo;
class DesktopFrameSource;

class WebRtcSender {
public:
    using SignalSendFn = std::function<void(const std::string&)>;
    using InputEventFn = std::function<void(const nlohmann::json&)>;
    using DirectMouseMoveFn = std::function<bool(double xNorm, double yNorm, uint64_t seq, double clientTsMs)>;
    using ConnectionClosedFn = std::function<void(const std::string& reason)>;
    using EncoderOverloadFn = std::function<void()>;

    enum class Mode {
        DirectCapture,
        ExternalFeed
    };

    enum class VideoCodec {
        VP8,
        H264,
        VP9
    };

    WebRtcSender(
        std::string sessionId,
        std::vector<std::string> iceServers,
        SignalSendFn sendFn,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        Mode mode = Mode::DirectCapture,
        std::string codecMode = "auto"
    );

    ~WebRtcSender();

    void start();
    void stop();

    void handleSignalingMessage(const std::string& jsonText);

    bool switchMonitor(int index);
    nlohmann::json buildMonitorInfoMessage() const;

    bool isExternalFeedMode() const { return m_mode == Mode::ExternalFeed; }

    // SDP-selected codec, not an environment preference or capture default.
    // The selected codec stays fixed during an established WebRTC session.
    const char* negotiatedVideoCodecName() const {
        switch (m_videoCodec) {
        case VideoCodec::VP9: return "VP9";
        case VideoCodec::H264: return "H264";
        default: return "VP8";
        }
    }

    void setInputEventHandler(InputEventFn fn);
    void setDirectMouseMoveHandler(DirectMouseMoveFn fn);
    void setConnectionClosedHandler(ConnectionClosedFn fn);
    void setEncoderOverloadHandler(EncoderOverloadFn fn);

    bool sendControlMessage(const nlohmann::json& msg);
    bool sendControlMessageText(const std::string& text);

    // Called by the service when the capture helper publishes low-CPU stream stats.
    // This lets the VP8 encoder follow the real stream mode instead of staying
    // at the original session max FPS/bitrate forever.
    void setExternalStreamHint(int streamMode, int targetFps, bool backstageMode, bool secureDesktop);

    void sendExternalEncodedVp8(const uint8_t* data,
        size_t size,
        uint32_t rtpTimestamp,
        bool keyframe);

    void sendExternalRawI420(const I420Frame& frame,
        bool forceKeyframe = false);

private:
    uint32_t randomU32();

    void createPeerConnection();
    void createInputDataChannel();
    std::shared_ptr<rtc::DataChannel> createNamedInputDataChannel(const std::string& label);
    void attachInputDataChannelHandlers(const std::shared_ptr<rtc::DataChannel>& dc, const std::string& label);
    bool handleBinaryMousePacket(const rtc::binary& data, const std::string& label);
    void signalLocalOfferIfReady();

    void ensureDirectCaptureInitialized();
    void startStreamingThread();
    void streamingLoop();

    std::vector<std::vector<uint8_t>> packetizeVp8(const EncodedFrame& frame);
    void sendRtpVp8Frame(const EncodedFrame& frame);

    void sendVp8PayloadPackets(const std::vector<std::vector<uint8_t>>& vp8Payloads,
        uint32_t rtpTimestamp);

    std::vector<std::vector<uint8_t>> packetizeVp9(const Vp9EncodedFrame& frame);
    void sendRtpVp9Frame(const Vp9EncodedFrame& frame);
    void sendVp9PayloadPackets(const std::vector<std::vector<uint8_t>>& vp9Payloads,
        uint32_t rtpTimestamp);

    bool shouldUseH264Experiment() const;
    std::vector<std::vector<uint8_t>> splitH264NalUnits(const std::vector<uint8_t>& frame);
    void sendRtpH264Frame(const H264EncodedFrame& frame);
    void sendH264NalPayloads(const std::vector<std::vector<uint8_t>>& nalUnits, uint32_t rtpTimestamp);

private:
    std::string m_sessionId;
    std::vector<std::string> m_iceServers;
    SignalSendFn m_signalSend;
    InputEventFn m_inputEventFn;
    DirectMouseMoveFn m_directMouseMoveFn;
    ConnectionClosedFn m_connectionClosedFn;
    EncoderOverloadFn m_encoderOverloadFn;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    int m_bitrateKbps = 6000;

    uint32_t m_ssrc = 0;
    uint16_t m_sequence = 0;
    int m_payloadType = 96;

    Mode m_mode = Mode::DirectCapture;

    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::Track> m_track;
    std::shared_ptr<rtc::DataChannel> m_inputDc;
    std::shared_ptr<rtc::DataChannel> m_inputMoveDc;
    std::shared_ptr<rtc::DataChannel> m_inputControlDc;

    std::unique_ptr<DesktopFrameSource> m_source;
    std::unique_ptr<Vp8Encoder> m_encoder;
    std::unique_ptr<H264MfEncoder> m_h264Encoder;
    std::unique_ptr<Vp9MfEncoder> m_vp9Encoder;
    bool m_vp9Failed = false;
    bool m_vp9NativeFailed = false; // Only downgrade native resolution, never discard the stream.
    int m_vp9SlowNativeFrames = 0;
    int m_vp9RejectedBitrateKbps = 0;
    SustainedEncoderHealth m_vp9Health;
    VideoCodec m_videoCodec = VideoCodec::VP8;
    bool m_h264Attempted = false;
    bool m_h264Failed = false;
    std::string m_codecMode = "vp8";
    InputInjector m_injector;

    std::thread m_streamThread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_canSend{ false };
    std::atomic<bool> m_offerSent{ false };
    std::atomic<bool> m_offerSignalSent{ false };
    std::atomic<bool> m_answerHandled{ false };
    std::atomic<bool> m_forceKeyframe{ false };
    // Separate backoffs: frame recovery remains fast at true desktop changes
    // (handled by the frame pump) but suppresses redundant remote IDR storms.
    std::atomic<uint64_t> m_lastViewerKeyframeRequestMs{0};
    std::atomic<uint64_t> m_lastViewerNetworkLogMs{0};

    int m_externalFrameCounter = 0;
    int m_externalEncoderWidth = 0;
    int m_externalEncoderHeight = 0;


    uint64_t m_externalEncodedFrames = 0;
    uint64_t m_externalSentFrames = 0;
    double m_externalEncodeMsTotal = 0.0;
    double m_externalEncodeMsMax = 0.0;
    double m_externalSendMsTotal = 0.0;
    double m_externalSendMsMax = 0.0;
    int m_externalKeyframeEvery = 30;
    std::chrono::steady_clock::time_point m_externalNextStatsLog{};
    std::chrono::steady_clock::time_point m_externalLastProfileChange{};
    std::chrono::steady_clock::time_point m_externalLastFrameAt{};
    std::chrono::steady_clock::time_point m_externalWakeUntil{};
    std::chrono::steady_clock::time_point m_externalLastNonIdleHintAt{};
    int m_externalEffectiveMode = 0; // 0=idle, 1=active/wake, 2=motion, 3=wake
    bool m_externalWasIdle = true;

    std::atomic<int> m_externalHintMode{ 0 };       // 0=idle, 1=active, 2=motion
    std::atomic<int> m_externalHintFps{ 0 };
    std::atomic<int> m_externalLastNonIdleMode{ 2 };
    std::atomic<bool> m_externalHintBackstage{ false };
    std::atomic<bool> m_externalHintSecure{ false };

    // WebRTC viewer_stream_profile: -1=not supplied, 0=native, >0=bounded.
    // Atomic: SCTP data-channel callbacks run independently of capture/encoding.
    std::atomic<int> m_viewerMaxWidth{ -1 };
    std::atomic<int> m_viewerMaxHeight{ -1 };
    std::atomic<int> m_viewerTargetFps{ 0 };
    std::atomic<int> m_viewerTargetBitrateKbps{ 0 };

    int m_externalConfiguredFps = 0;
    int m_externalConfiguredBitrateKbps = 0;
    int m_externalConfiguredCpuUsed = 0;
    int m_externalConfiguredMaxQuantizer = 0;
    std::string m_externalProfileName = "software-vp8-lowcpu";

    std::mutex m_sendMu;
    std::mutex m_externalEncodeMu;

    std::atomic<uint64_t> m_lastBinaryMouseSeq{ 0 };
    std::atomic<uint64_t> m_binaryMousePackets{ 0 };
    std::chrono::steady_clock::time_point m_nextBinaryMouseLog{};
};