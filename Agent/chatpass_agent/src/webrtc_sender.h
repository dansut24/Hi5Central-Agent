#pragma once

#include "vp8_encoder.h"
#include "vp9_mf_encoder.h"
#include "vp9_vpx_encoder.h"
#include "h264_mf_encoder.h"
#include "h265_mf_encoder.h"
#include "av1_mf_encoder.h"
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

#ifdef _WIN32
struct OpusEncoder;
namespace hi5 { class WasapiLoopbackCapture; }
#endif

struct DisplayInfo;
class DesktopFrameSource;

class WebRtcSender {
public:
    using SignalSendFn = std::function<void(const std::string&)>;
    using InputEventFn = std::function<void(const nlohmann::json&)>;
    using DirectMouseMoveFn = std::function<bool(double xNorm, double yNorm, uint64_t seq, double clientTsMs)>;
    using ConnectionClosedFn = std::function<void(const std::string& reason)>;

    enum class Mode {
        DirectCapture,
        ExternalFeed
    };

    enum class VideoCodec {
        VP8,
        VP9,
        AV1,
        H264,
        H265
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
        std::string codecMode = "auto",
        bool enableAudio = false
    );

    ~WebRtcSender();

    void start();
    void stop();

    void handleSignalingMessage(const std::string& jsonText);

    bool switchMonitor(int index);
    nlohmann::json buildMonitorInfoMessage() const;

    bool isExternalFeedMode() const { return m_mode == Mode::ExternalFeed; }

    void setInputEventHandler(InputEventFn fn);
    void setDirectMouseMoveHandler(DirectMouseMoveFn fn);
    void setConnectionClosedHandler(ConnectionClosedFn fn);

    bool sendControlMessage(const nlohmann::json& msg);
    bool sendControlMessageText(const std::string& text);
    bool hasPendingDevCodecSwitch();

    // Called by the service when the capture helper publishes low-CPU stream stats.
    // VP8 uses these hints for runtime tuning; adaptive codec health also uses
    // them so VP9/H.264 are judged against the real live capture cadence.
    void setExternalStreamHint(int streamMode, int targetFps, bool backstageMode, bool secureDesktop);

    void sendExternalEncodedVp8(const uint8_t* data,
        size_t size,
        uint32_t rtpTimestamp,
        bool keyframe);

    void sendExternalRawI420(const I420Frame& frame,
        uint64_t captureTimestampNs,
        bool forceKeyframe = false);

    // Returns true when the shared D3D11 frame was consumed by the GPU H.264
    // path. False asks MediaHost to use the existing I420 fallback for the frame.
    bool trySendExternalGpuH264(const SharedGpuFrame& frame,
        uint64_t captureTimestampNs,
        bool forceKeyframe = false);

private:
    uint32_t randomU32();

    void createPeerConnection();
    void createInputDataChannel();
    std::shared_ptr<rtc::DataChannel> createNamedInputDataChannel(const std::string& label);
    void attachInputDataChannelHandlers(const std::shared_ptr<rtc::DataChannel>& dc, const std::string& label);
    bool handleBinaryMousePacket(const rtc::binary& data, const std::string& label);
    void signalLocalOfferIfReady();
    uint32_t externalRtpTimestamp(uint64_t captureTimestampNs);
    bool selectAutoCodecFromAnswer(const std::string& sdp);
    bool selectBestAutoCodec(const std::string& reasonPrefix);
    bool applyDevCodecSwitch(const std::string& requested, std::string& activeCodec, std::string& detail);
    std::string activeVideoCodecName() const;
    bool switchVideoCodec(VideoCodec codec, const std::string& reason, int negotiatedPayloadType = -1);
    void configureVideoMediaHandler(VideoCodec codec);
    void observeCodecHealth(double encodeAvgMs, double encodeMaxMs, double sendAvgMs);
#ifdef _WIN32
    void startAudioLoopback();
    void stopAudioLoopback();
    void onAudioPcm(const int16_t* samples, size_t frames);
#endif

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
#ifdef _WIN32
    bool m_enableAudio = false;
    uint32_t m_audioSsrc = 0;
    std::shared_ptr<rtc::Track> m_audioTrack;
    std::shared_ptr<rtc::RtpPacketizationConfig> m_audioRtpConfig;
    std::unique_ptr<hi5::WasapiLoopbackCapture> m_audioCapture;
    OpusEncoder* m_opusEncoder = nullptr;
    std::vector<int16_t> m_audioPcm;
    std::mutex m_audioMu;
#endif
    std::shared_ptr<rtc::DataChannel> m_inputDc;
    std::shared_ptr<rtc::DataChannel> m_inputMoveDc;
    std::shared_ptr<rtc::DataChannel> m_inputControlDc;

    std::unique_ptr<DesktopFrameSource> m_source;
    std::unique_ptr<Vp8Encoder> m_encoder;
    std::unique_ptr<H264MfEncoder> m_h264Encoder;
    std::unique_ptr<H264MfEncoder> m_h264GpuEncoder;
    std::unique_ptr<Vp9MfEncoder> m_vp9Encoder;
    std::unique_ptr<Vp9VpxEncoder> m_vp9VpxEncoder;
    std::unique_ptr<Av1MfEncoder> m_av1Encoder;
    std::unique_ptr<H265MfEncoder> m_h265Encoder;
    I420Frame m_vp8ScaleScratch;
    I420Frame m_vp9ScaleScratch;
    I420Frame m_h264ScaleScratch;
    std::shared_ptr<rtc::RtpPacketizationConfig> m_nativeVideoRtpConfig;
    bool m_vp9Failed = false;
    bool m_av1Failed = false;
    bool m_h265Failed = false;
    bool m_autoCodec = false;
    bool m_peerAcceptsVp8 = false;
    bool m_peerAcceptsVp9 = false;
    bool m_peerAcceptsH264 = false;
    bool m_peerAcceptsAv1 = false;
    bool m_peerAcceptsH265 = false;
    // Once the SDP answer has been applied, the selected codec/payload is fixed
    // for the lifetime of this PeerConnection. Changing codec requires a new
    // offer/answer exchange; sending a different payload on the existing track
    // can leave browsers connected with a permanently black video element.
    bool m_negotiationLocked = false;
    VideoCodec m_negotiatedCodec = VideoCodec::VP8;
    int m_negotiatedPayloadType = -1;
    std::string m_negotiatedH264Fmtp;
    std::string m_negotiatedH264ProfileLevelId = "42e01f";
    int m_negotiatedH264PacketizationMode = 1;
    bool m_negotiatedH264LevelAsymmetryAllowed = false;
    int m_negotiatedH264MaxWidth = 1280;
    int m_negotiatedH264MaxHeight = 720;
    bool m_hwAv1Available = false;
    bool m_hwAv1ProbeDone = false;
    bool m_swAv1Available = false;
    bool m_hwVp9Available = false;
    bool m_hwVp9ProbeDone = false;
    bool m_hwH265Available = false;
    bool m_hwH265ProbeDone = false;
    bool m_swH265Available = false;
    bool m_hwH264Available = false;
    bool m_hwH264ProbeDone = false;
    bool m_swVp9Allowed = false;
    int m_codecUnhealthyWindows = 0;
    int m_av1EmptyOutputFrames = 0;
    std::chrono::steady_clock::time_point m_lastCodecSwitchAt{};
    VideoCodec m_videoCodec = VideoCodec::VP8;
    bool m_h264Attempted = false;
    bool m_h264Failed = false;
    bool m_h264GpuFailed = false;
    std::string m_codecMode = "vp8";
    std::mutex m_codecSwitchMu;
    std::string m_pendingDevCodecSwitch;
    InputInjector m_injector;

    std::thread m_streamThread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_canSend{ false };
    std::atomic<bool> m_offerSent{ false };
    std::atomic<bool> m_offerSignalSent{ false };
    std::atomic<bool> m_answerHandled{ false };
    std::atomic<bool> m_forceKeyframe{ false };

    int m_externalFrameCounter = 0;
    uint64_t m_externalRtpBaseCaptureNs = 0;
    uint32_t m_externalRtpBaseTimestamp = 0;
    uint32_t m_externalLastRtpTimestamp = 0;
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
    std::chrono::steady_clock::time_point m_externalLastEncodeAt{};
    std::chrono::steady_clock::time_point m_externalWakeUntil{};
    std::chrono::steady_clock::time_point m_externalLastNonIdleHintAt{};
    int m_externalEffectiveMode = 0; // 0=idle, 1=active/wake, 2=motion, 3=wake
    bool m_externalWasIdle = true;

    std::atomic<int> m_externalHintMode{ 0 };       // 0=idle, 1=active, 2=motion
    std::atomic<int> m_externalHintFps{ 0 };
    std::atomic<int> m_externalLastNonIdleMode{ 2 };
    std::atomic<int64_t> m_externalInputWakeUntilMs{ 0 };
    std::atomic<bool> m_externalHintBackstage{ false };
    std::atomic<bool> m_externalHintSecure{ false };
    std::atomic<double> m_viewerRttMs{ 0.0 };
    std::atomic<double> m_viewerJitterMs{ 0.0 };
    std::atomic<double> m_viewerJitterBufferMs{ 0.0 };
    std::atomic<double> m_viewerBitrateKbps{ 0.0 };
    std::atomic<int> m_viewerMaxWidth{ 0 };
    std::atomic<int> m_viewerMaxHeight{ 0 };
    std::atomic<int> m_viewerTargetFps{ 0 };

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