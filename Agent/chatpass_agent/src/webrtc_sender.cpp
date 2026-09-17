#include "webrtc_sender.h"
#include "frame_source.h"
#include "codec_capabilities.h"
#include "util/log.h"
#ifdef _WIN32
#include "platform/windows/wasapi_loopback.h"
#include <opus/opus.h>
#endif

#include <rtc/rtc.hpp>
#include <rtc/av1rtppacketizer.hpp>
#include <rtc/h265rtppacketizer.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using json = nlohmann::json;

namespace {
    static std::string makeDisplaySignature(const std::vector<DisplayInfo>& displays) {
        std::ostringstream oss;
        for (const auto& d : displays) {
            oss << d.index << '|'
                << d.name << '|'
                << d.x << '|'
                << d.y << '|'
                << d.width << '|'
                << d.height << '|'
                << (d.primary ? 1 : 0)
                << ';';
        }
        return oss.str();
    }

    static bool looksLikeCaptureResetError(const std::string& s) {
        return s.find("DuplicateOutput failed") != std::string::npos
            || s.find("AcquireNextFrame failed") != std::string::npos
            || s.find("DXGI_ERROR_ACCESS_LOST") != std::string::npos
            || s.find("DXGI_ERROR_INVALID_CALL") != std::string::npos
            || s.find("DXGI_ERROR_NOT_CURRENTLY_AVAILABLE") != std::string::npos;
    }

    static int readEnvInt(const char* name, int fallbackValue, int minValue, int maxValue) {
        char buf[32]{};
        DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf)) return fallbackValue;
        try {
            int v = std::stoi(std::string(buf, buf + n));
            return std::max(minValue, std::min(maxValue, v));
        }
        catch (...) {
            return fallbackValue;
        }
    }

    static std::string readEnvString(const char* name, const char* fallbackValue = "") {
        char buf[128]{};
        DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        std::string value = (n > 0 && n < sizeof(buf)) ? std::string(buf, buf + n) : std::string(fallbackValue);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    static std::string imageQualityMode() {
        std::string value = readEnvString("HI5_IMAGE_QUALITY", "balanced");
        if (value == "near-lossless" || value == "near_lossless") value = "near_lossless";
        if (value != "balanced" && value != "text" && value != "near_lossless" && value != "lossless")
            value = "balanced";
        return value;
    }


    static int makeEvenAtLeast2(int v) {
        v = std::max(2, v);
        if (v & 1) --v;
        return std::max(2, v);
    }

    static void scalePlaneNearest(const std::vector<uint8_t>& src,
        int srcW,
        int srcH,
        std::vector<uint8_t>& dst,
        int dstW,
        int dstH) {
        dst.resize(static_cast<size_t>(dstW) * static_cast<size_t>(dstH));
        if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || src.empty()) {
            std::fill(dst.begin(), dst.end(), 128);
            return;
        }
        for (int y = 0; y < dstH; ++y) {
            const int sy = std::min(srcH - 1, (y * srcH) / dstH);
            for (int x = 0; x < dstW; ++x) {
                const int sx = std::min(srcW - 1, (x * srcW) / dstW);
                const size_t si = static_cast<size_t>(sy) * static_cast<size_t>(srcW) + static_cast<size_t>(sx);
                const size_t di = static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x);
                dst[di] = si < src.size() ? src[si] : 128;
            }
        }
    }

    static const I420Frame& scaleI420ForWebRtc(const I420Frame& src,
        I420Frame& scratch, int maxW, int maxH, bool* scaledOut = nullptr) {
        if (scaledOut) *scaledOut = false;
        if (src.width <= 0 || src.height <= 0) return src;
        if (maxW <= 0 || maxH <= 0 || (src.width <= maxW && src.height <= maxH)) return src;

        const double sx = static_cast<double>(maxW) / static_cast<double>(src.width);
        const double sy = static_cast<double>(maxH) / static_cast<double>(src.height);
        const double scale = std::min(sx, sy);
        const int dstW = std::min(makeEvenAtLeast2(static_cast<int>(src.width * scale)), makeEvenAtLeast2(maxW));
        const int dstH = std::min(makeEvenAtLeast2(static_cast<int>(src.height * scale)), makeEvenAtLeast2(maxH));

        scratch.width = dstW;
        scratch.height = dstH;
        scalePlaneNearest(src.y, src.width, src.height, scratch.y, dstW, dstH);
        const int srcUw = (src.width + 1) / 2;
        const int srcUh = (src.height + 1) / 2;
        const int dstUw = (dstW + 1) / 2;
        const int dstUh = (dstH + 1) / 2;
        scalePlaneNearest(src.u, srcUw, srcUh, scratch.u, dstUw, dstUh);
        scalePlaneNearest(src.v, srcUw, srcUh, scratch.v, dstUw, dstUh);
        if (scaledOut) *scaledOut = true;
        return scratch;
    }

    struct Vp8RuntimeProfile {
        const char* name = "software-vp8-lowcpu-idle";
        int fps = 2;
        int bitrateKbps = 1200;
        int cpuUsed = 12;
        int minQuantizer = 4;
        int maxQuantizer = 38;
        int keyframeSeconds = 12;
    };

    static Vp8RuntimeProfile buildVp8RuntimeProfile(int streamMode,
        int hintedFps,
        bool backstage,
        bool secureDesktop,
        int originalMaxFps,
        int originalMaxBitrateKbps) {
        const int mode = std::max(0, std::min(3, streamMode)); // 3 = wake-from-idle profile
        const int maxFps = std::max(1, originalMaxFps);
        const std::string quality = imageQualityMode();
        const int qualityCeilingKbps = quality == "lossless" ? 20000 :
            (quality == "near_lossless" ? 16000 : (quality == "text" ? 10000 : originalMaxBitrateKbps));
        const int maxKbps = std::max(300, std::max(originalMaxBitrateKbps, qualityCeilingKbps));

        Vp8RuntimeProfile p{};

        if (backstage) {
            if (mode <= 0) {
                p.name = "software-vp8-backstage-idle";
                p.fps = readEnvInt("HI5_VP8_BACKSTAGE_IDLE_FPS", 2, 1, 10);
                p.bitrateKbps = readEnvInt("HI5_VP8_BACKSTAGE_IDLE_KBPS", 900, 250, 8000);
                p.cpuUsed = readEnvInt("HI5_VP8_BACKSTAGE_IDLE_CPUUSED", 13, 4, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_BACKSTAGE_IDLE_MAX_Q", 42, 24, 56);
                p.keyframeSeconds = readEnvInt("HI5_VP8_BACKSTAGE_IDLE_KEYFRAME_SECONDS", 15, 3, 60);
            }
            else if (mode == 3) {
                p.name = "software-vp8-backstage-wake";
                p.fps = readEnvInt("HI5_VP8_BACKSTAGE_WAKE_FPS", 8, 1, 24);
                p.bitrateKbps = readEnvInt("HI5_VP8_BACKSTAGE_WAKE_KBPS", 1800, 400, 10000);
                p.cpuUsed = readEnvInt("HI5_VP8_BACKSTAGE_WAKE_CPUUSED", 12, 3, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_BACKSTAGE_WAKE_MAX_Q", 40, 24, 54);
                p.keyframeSeconds = readEnvInt("HI5_VP8_BACKSTAGE_WAKE_KEYFRAME_SECONDS", 12, 2, 60);
            }
            else if (mode == 1) {
                p.name = "software-vp8-backstage-active";
                p.fps = readEnvInt("HI5_VP8_BACKSTAGE_ACTIVE_FPS", 10, 1, 30);
                p.bitrateKbps = readEnvInt("HI5_VP8_BACKSTAGE_ACTIVE_KBPS", 1800, 500, 12000);
                p.cpuUsed = readEnvInt("HI5_VP8_BACKSTAGE_ACTIVE_CPUUSED", 12, 3, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_BACKSTAGE_ACTIVE_MAX_Q", 42, 24, 54);
                p.keyframeSeconds = readEnvInt("HI5_VP8_BACKSTAGE_ACTIVE_KEYFRAME_SECONDS", 10, 2, 30);
            }
            else {
                p.name = "software-vp8-backstage-motion";
                p.fps = readEnvInt("HI5_VP8_BACKSTAGE_MOTION_FPS", 12, 1, 30);
                p.bitrateKbps = readEnvInt("HI5_VP8_BACKSTAGE_MOTION_KBPS", 3500, 1000, 16000);
                p.cpuUsed = readEnvInt("HI5_VP8_BACKSTAGE_MOTION_CPUUSED", 10, 2, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_BACKSTAGE_MOTION_MAX_Q", 40, 20, 52);
                p.keyframeSeconds = readEnvInt("HI5_VP8_BACKSTAGE_MOTION_KEYFRAME_SECONDS", 10, 1, 30);
            }
        }
        else {
            if (mode <= 0) {
                p.name = secureDesktop ? "software-vp8-secure-idle" : "software-vp8-idle";
                p.fps = readEnvInt("HI5_VP8_IDLE_FPS", 2, 1, 10);
                p.bitrateKbps = readEnvInt("HI5_VP8_IDLE_KBPS", 1200, 250, 10000);
                p.cpuUsed = readEnvInt("HI5_VP8_IDLE_CPUUSED", 12, 4, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_IDLE_MAX_Q", 38, 20, 54);
                p.keyframeSeconds = readEnvInt("HI5_VP8_IDLE_KEYFRAME_SECONDS", 12, 3, 60);
            }
            else if (mode == 3) {
                p.name = secureDesktop ? "software-vp8-secure-wake" : "software-vp8-wake";
                p.fps = readEnvInt("HI5_VP8_WAKE_FPS", 8, 1, 30);
                p.bitrateKbps = readEnvInt("HI5_VP8_WAKE_KBPS", 2000, 500, 12000);
                p.cpuUsed = readEnvInt("HI5_VP8_WAKE_CPUUSED", 13, 3, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_WAKE_MAX_Q", 42, 22, 54);
                p.keyframeSeconds = readEnvInt("HI5_VP8_WAKE_KEYFRAME_SECONDS", 10, 2, 60);
            }
            else if (mode == 1) {
                p.name = secureDesktop ? "software-vp8-secure-active" : "software-vp8-active";
                p.fps = readEnvInt("HI5_VP8_ACTIVE_FPS", 15, 1, 30);
                p.bitrateKbps = readEnvInt("HI5_VP8_ACTIVE_KBPS", 3500, 500, 16000);
                p.cpuUsed = readEnvInt("HI5_VP8_ACTIVE_CPUUSED", 11, 3, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_ACTIVE_MAX_Q", 38, 20, 50);
                p.keyframeSeconds = readEnvInt("HI5_VP8_ACTIVE_KEYFRAME_SECONDS", 8, 2, 30);
            }
            else {
                p.name = secureDesktop ? "software-vp8-secure-motion" : "software-vp8-motion";
                p.fps = readEnvInt("HI5_VP8_MOTION_FPS", 20, 1, 60);
                p.bitrateKbps = readEnvInt("HI5_VP8_MOTION_KBPS", 5500, 1000, 20000);
                p.cpuUsed = readEnvInt("HI5_VP8_MOTION_CPUUSED", 10, 2, 16);
                p.maxQuantizer = readEnvInt("HI5_VP8_MOTION_MAX_Q", 38, 18, 50);
                p.keyframeSeconds = readEnvInt("HI5_VP8_MOTION_KEYFRAME_SECONDS", 8, 1, 30);
            }
        }

        // Default mode favours low CPU. Quality mode restores a higher motion
        // ceiling without changing code when you want to compare against the old
        // 24-30 FPS / 8 Mbps profile.
        const bool qualityMode = readEnvInt("HI5_VP8_QUALITY_MODE", 0, 0, 1) == 1;
        if (qualityMode && mode >= 2 && !backstage) {
            p.name = secureDesktop ? "software-vp8-secure-quality" : "software-vp8-quality";
            p.fps = readEnvInt("HI5_VP8_QUALITY_FPS", 24, 1, 60);
            p.bitrateKbps = readEnvInt("HI5_VP8_QUALITY_KBPS", 8000, 1000, 20000);
            p.cpuUsed = readEnvInt("HI5_VP8_QUALITY_CPUUSED", 7, 2, 16);
            p.maxQuantizer = readEnvInt("HI5_VP8_QUALITY_MAX_Q", 32, 18, 46);
            p.keyframeSeconds = readEnvInt("HI5_VP8_QUALITY_KEYFRAME_SECONDS", 5, 1, 20);
        }

        if (hintedFps > 0) {
            p.fps = std::min(p.fps, std::max(1, hintedFps));
        }

        if (quality == "text") {
            p.bitrateKbps = std::min(maxKbps, std::max(p.bitrateKbps, (p.bitrateKbps * 3) / 2));
            p.maxQuantizer = std::min(p.maxQuantizer, 22);
        }
        else if (quality == "near_lossless") {
            p.bitrateKbps = std::min(maxKbps, std::max(p.bitrateKbps, p.bitrateKbps * 2));
            p.maxQuantizer = std::min(p.maxQuantizer, 10);
        }
        else if (quality == "lossless") {
            // VP8 does not expose the true VP9 lossless control used by Hi5Central.
            // If VP8 is explicitly forced, keep it visually near-lossless instead.
            p.bitrateKbps = maxKbps;
            p.maxQuantizer = std::min(p.maxQuantizer, 6);
        }

        p.fps = std::max(1, std::min(maxFps, p.fps));
        p.bitrateKbps = std::max(250, std::min(maxKbps, p.bitrateKbps));
        p.minQuantizer = quality == "near_lossless" ? 0 : 4;
        return p;
    }
}

WebRtcSender::WebRtcSender(std::string sessionId,
    std::vector<std::string> iceServers,
    WebRtcSender::SignalSendFn sendFn,
    int width,
    int height,
    int fps,
    int bitrateKbps,
    Mode mode,
    std::string codecMode,
    bool enableAudio)
    : m_sessionId(std::move(sessionId)),
    m_iceServers(std::move(iceServers)),
    m_signalSend(std::move(sendFn)),
    m_width(width),
    m_height(height),
    m_fps(fps),
    m_bitrateKbps(bitrateKbps),
    m_ssrc(randomU32()),
#ifdef _WIN32
    m_enableAudio(enableAudio),
    m_audioSsrc(randomU32()),
#endif
    m_mode(mode),
    m_source(nullptr),
    m_encoder(nullptr) {
    m_codecMode = codecMode.empty() ? "auto" : std::move(codecMode);

    std::transform(m_codecMode.begin(), m_codecMode.end(), m_codecMode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });

    LogInfo("[codec] WebRtcSender codec mode=" + m_codecMode);

    // Cache real endpoint encoder capability once per session. Auto only promotes
    // expensive codecs when the endpoint has a hardware encoder, with libvpx VP9
    // permitted on sufficiently capable CPUs as a measured software fallback.
    try {
        const auto localCaps = hi5::ProbeCodecCapabilitiesAndSelect("auto");
        for (const auto& c : localCaps.capabilities) {
            if (c.codec == "av1") { m_hwAv1Available = c.hardwareEncodeAvailable; m_swAv1Available = c.softwareEncodeAvailable; }
            else if (c.codec == "vp9") m_hwVp9Available = c.hardwareEncodeAvailable;
            else if (c.codec == "h265") { m_hwH265Available = c.hardwareEncodeAvailable; m_swH265Available = c.softwareEncodeAvailable; }
            else if (c.codec == "h264") m_hwH264Available = c.hardwareEncodeAvailable;
        }
    } catch (...) {
    }
#ifdef _WIN32
    m_swVp9Allowed = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) >= 8;
#else
    m_swVp9Allowed = true;
#endif
    LogInfo("[codec] endpoint capabilities session=" + m_sessionId +
        " av1_hw=" + std::string(m_hwAv1Available ? "1" : "0") +
        " av1_sw=" + std::string(m_swAv1Available ? "1" : "0") +
        " vp9_hw=" + std::string(m_hwVp9Available ? "1" : "0") +
        " h265_hw=" + std::string(m_hwH265Available ? "1" : "0") +
        " h265_sw=" + std::string(m_swH265Available ? "1" : "0") +
        " h264_hw=" + std::string(m_hwH264Available ? "1" : "0") +
        " vp9_sw_allowed=" + std::string(m_swVp9Allowed ? "1" : "0"));

    m_autoCodec = (m_codecMode == "auto");
    if (m_autoCodec) {
        // Remote-support Auto favours predictable endpoint CPU/RAM over maximum
        // compression efficiency. Advertise all mature codecs, but begin with
        // the proven low-latency VP8 baseline unless an operator explicitly
        // selects VP9/H.264 (or a future hardware policy promotes them).
        m_videoCodec = VideoCodec::VP8;
        m_payloadType = 96;
        LogInfo("[codec] adaptive auto mode enabled, VP8 low-CPU baseline preferred session=" + m_sessionId);
    }
    else if (m_codecMode == "av1_hw" || m_codecMode == "av1" || m_codecMode == "av1_sw") {
        m_videoCodec = VideoCodec::AV1;
        m_payloadType = 100;
        LogInfo("[codec] WebRtcSender AV1 requested mode=" + m_codecMode +
            " session=" + m_sessionId);
    }
    else if (m_codecMode == "vp9_hw" || m_codecMode == "vp9" || m_codecMode == "vp9_sw") {
        m_videoCodec = VideoCodec::VP9;
        m_payloadType = 98;
        LogInfo("[codec] WebRtcSender VP9 requested mode=" + m_codecMode +
            " session=" + m_sessionId);
    }
    else if (m_codecMode == "h265_hw" || m_codecMode == "h265" || m_codecMode == "h265_sw") {
        m_videoCodec = VideoCodec::H265;
        m_payloadType = 104;
        LogInfo("[codec] WebRtcSender H.265 requested mode=" + m_codecMode +
            " session=" + m_sessionId);
    }
    else if (m_codecMode == "h264_hw" || m_codecMode == "h264" || m_codecMode == "h264_sw") {
        m_videoCodec = VideoCodec::H264;
        m_payloadType = 102;
        LogInfo("[codec] WebRtcSender H.264 requested mode=" + m_codecMode +
            " session=" + m_sessionId);
    }
    else {
        m_videoCodec = VideoCodec::VP8;
        m_payloadType = 96;
    }

    if (m_mode == Mode::DirectCapture) {
        ensureDirectCaptureInitialized();
    }
}

WebRtcSender::~WebRtcSender() {
    stop();
}

bool WebRtcSender::hasPendingDevCodecSwitch() {
    std::lock_guard<std::mutex> lock(m_codecSwitchMu);
    return !m_pendingDevCodecSwitch.empty();
}

void WebRtcSender::setInputEventHandler(InputEventFn fn) {
    m_inputEventFn = std::move(fn);
}

void WebRtcSender::setDirectMouseMoveHandler(DirectMouseMoveFn fn) {
    m_directMouseMoveFn = std::move(fn);
}

void WebRtcSender::setConnectionClosedHandler(ConnectionClosedFn fn) {
    m_connectionClosedFn = std::move(fn);
}

uint32_t WebRtcSender::randomU32() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return dist(gen);
}

uint32_t WebRtcSender::externalRtpTimestamp(uint64_t captureTimestampNs) {
    // External capture publishes a monotonic timestamp in nanoseconds. Drive RTP
    // from that real capture clock rather than from an encoder frame counter: the
    // remote desktop intentionally changes cadence between idle/active/motion, and
    // a synthetic fixed-FPS clock causes WebRTC playout latency to accumulate.
    if (captureTimestampNs == 0) {
        if (m_externalLastRtpTimestamp == 0) {
            m_externalLastRtpTimestamp = randomU32();
        }
        const uint32_t step = static_cast<uint32_t>(90000u / static_cast<uint32_t>(std::max(1, m_fps)));
        m_externalLastRtpTimestamp += std::max<uint32_t>(1u, step);
        return m_externalLastRtpTimestamp;
    }

    if (m_externalRtpBaseCaptureNs == 0) {
        m_externalRtpBaseCaptureNs = captureTimestampNs;
        m_externalRtpBaseTimestamp = randomU32();
        m_externalLastRtpTimestamp = m_externalRtpBaseTimestamp;
        return m_externalLastRtpTimestamp;
    }

    const uint64_t deltaNs = captureTimestampNs >= m_externalRtpBaseCaptureNs
        ? (captureTimestampNs - m_externalRtpBaseCaptureNs)
        : 0;
    const uint64_t delta90k = (deltaNs * 90000ull) / 1000000000ull;
    uint32_t timestamp = m_externalRtpBaseTimestamp + static_cast<uint32_t>(delta90k);

    // Multiple publications can theoretically land within the same 90 kHz tick.
    // Keep timestamps strictly advancing for decoders while preserving wall-clock
    // spacing for normal frames.
    if (timestamp == m_externalLastRtpTimestamp && deltaNs != 0) {
        ++timestamp;
    }
    m_externalLastRtpTimestamp = timestamp;
    return timestamp;
}

void WebRtcSender::selectAutoCodecFromAnswer(const std::string& sdp) {
    std::string upper = sdp;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
        });

    m_peerAcceptsAv1 = upper.find("A=RTPMAP:100 AV1/90000") != std::string::npos;
    m_peerAcceptsVp9 = upper.find("A=RTPMAP:98 VP9/90000") != std::string::npos;
    m_peerAcceptsH265 = upper.find("A=RTPMAP:104 H265/90000") != std::string::npos;
    m_peerAcceptsH264 = upper.find("A=RTPMAP:102 H264/90000") != std::string::npos;
    m_peerAcceptsVp8 = upper.find("A=RTPMAP:96 VP8/90000") != std::string::npos;

    LogInfo("[codec] viewer answer capabilities session=" + m_sessionId +
        " av1=" + std::string(m_peerAcceptsAv1 ? "1" : "0") +
        " vp9=" + std::string(m_peerAcceptsVp9 ? "1" : "0") +
        " h265=" + std::string(m_peerAcceptsH265 ? "1" : "0") +
        " h264=" + std::string(m_peerAcceptsH264 ? "1" : "0") +
        " vp8=" + std::string(m_peerAcceptsVp8 ? "1" : "0"));

    if (!m_autoCodec) return;

    const std::string quality = imageQualityMode();
    if (quality == "lossless" && m_peerAcceptsVp9) {
        // True lossless is implemented by the bundled libvpx VP9 path. Hardware
        // MFT encoders are not assumed lossless because their rate-control APIs vary.
        m_codecMode = "vp9_sw";
        switchVideoCodec(VideoCodec::VP9, "auto: true lossless mode requires libvpx VP9");
    }
    else if (m_peerAcceptsAv1 && m_hwAv1Available) {
        switchVideoCodec(VideoCodec::AV1, "auto: hardware AV1 available on endpoint and Viewer");
    }
    else if (m_peerAcceptsVp9 && m_hwVp9Available) {
        switchVideoCodec(VideoCodec::VP9, "auto: hardware VP9 available on endpoint and Viewer");
    }
    else if (m_peerAcceptsH265 && m_hwH265Available) {
        switchVideoCodec(VideoCodec::H265, "auto: hardware H.265 available on endpoint and Viewer");
    }
    else if (m_peerAcceptsH264 && m_hwH264Available) {
        switchVideoCodec(VideoCodec::H264, "auto: hardware H.264 available on endpoint and Viewer");
    }
    else if (m_peerAcceptsVp9 && m_swVp9Allowed) {
        m_codecMode = "vp9_sw";
        switchVideoCodec(VideoCodec::VP9, "auto: software VP9 allowed; live health fallback to VP8 enabled");
    }
    else if (m_peerAcceptsVp8) {
        switchVideoCodec(VideoCodec::VP8, "auto: low-CPU VP8 fallback");
    }
    else if (m_peerAcceptsVp9) {
        m_codecMode = "vp9_sw";
        switchVideoCodec(VideoCodec::VP9, "auto: Viewer has no VP8; software VP9 required");
    }
    else {
        LogInfo("[codec] adaptive offer answer exposed no recognised usable video payload; keeping current codec session=" + m_sessionId);
    }
}

std::string WebRtcSender::activeVideoCodecName() const {
    if (m_videoCodec == VideoCodec::AV1) return "AV1";
    if (m_videoCodec == VideoCodec::VP9) return "VP9";
    if (m_videoCodec == VideoCodec::H265) return "H.265";
    if (m_videoCodec == VideoCodec::H264) return "H.264";
    return "VP8";
}

bool WebRtcSender::applyDevCodecSwitch(const std::string& requestedRaw, std::string& activeCodec, std::string& detail) {
    std::string requested = requestedRaw;
    std::transform(requested.begin(), requested.end(), requested.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
    requested.erase(std::remove(requested.begin(), requested.end(), '.'), requested.end());
    if (requested == "hevc") requested = "h265";

    auto reject = [&](const std::string& why) {
        activeCodec = activeVideoCodecName();
        detail = why;
        return false;
        };
    auto choose = [&](VideoCodec codec, const std::string& mode, const std::string& why) {
        m_codecMode = mode;
        const bool ok = switchVideoCodec(codec, why);
        activeCodec = activeVideoCodecName();
        detail = ok ? "Switch accepted; waiting for the first encoded frame" : "Codec switch was rejected";
        return ok;
        };

    if (requested == "auto") {
        m_autoCodec = true;
        const std::string quality = imageQualityMode();
        if (quality == "lossless" && m_peerAcceptsVp9 && !m_vp9Failed)
            return choose(VideoCodec::VP9, "vp9_sw", "dev selector: Auto lossless -> VP9 software");
        if (m_peerAcceptsAv1 && m_hwAv1Available && !m_av1Failed)
            return choose(VideoCodec::AV1, "auto", "dev selector: Auto -> hardware AV1");
        if (m_peerAcceptsVp9 && m_hwVp9Available && !m_vp9Failed)
            return choose(VideoCodec::VP9, "auto", "dev selector: Auto -> hardware VP9");
        if (m_peerAcceptsH265 && m_hwH265Available && !m_h265Failed)
            return choose(VideoCodec::H265, "auto", "dev selector: Auto -> hardware H.265");
        if (m_peerAcceptsH264 && m_hwH264Available && !m_h264Failed)
            return choose(VideoCodec::H264, "auto", "dev selector: Auto -> hardware H.264");
        if (m_peerAcceptsVp9 && m_swVp9Allowed && !m_vp9Failed)
            return choose(VideoCodec::VP9, "vp9_sw", "dev selector: Auto -> software VP9");
        if (m_peerAcceptsVp8)
            return choose(VideoCodec::VP8, "vp8", "dev selector: Auto -> VP8 fallback");
        return reject("No mutually negotiated Auto codec is currently healthy");
    }

    m_autoCodec = false;
    if (requested == "vp8") {
        if (!m_peerAcceptsVp8) return reject("Viewer did not negotiate VP8 for this session");
        return choose(VideoCodec::VP8, "vp8", "dev selector forced VP8");
    }
    if (requested == "vp9") {
        if (!m_peerAcceptsVp9) return reject("Viewer did not negotiate VP9 for this session");
        m_vp9Failed = false;
        return choose(VideoCodec::VP9, "vp9", "dev selector forced VP9");
    }
    if (requested == "av1") {
        if (!m_peerAcceptsAv1) return reject("Viewer did not negotiate AV1 for this session");
        if (!m_hwAv1Available && !m_swAv1Available) return reject("Endpoint has no usable AV1 encoder");
        m_av1Failed = false;
        m_av1EmptyOutputFrames = 0;
        return choose(VideoCodec::AV1, "av1", "dev selector forced AV1");
    }
    if (requested == "h264") {
        if (!m_peerAcceptsH264) return reject("Viewer did not negotiate H.264 for this session");
        m_h264Failed = false;
        m_h264Attempted = false;
        return choose(VideoCodec::H264, "h264", "dev selector forced H.264");
    }
    if (requested == "h265") {
        if (!m_peerAcceptsH265) return reject("Viewer did not negotiate H.265/HEVC for this session");
        if (!m_hwH265Available && !m_swH265Available) return reject("Endpoint has no usable H.265/HEVC encoder");
        m_h265Failed = false;
        return choose(VideoCodec::H265, "h265", "dev selector forced H.265");
    }
    return reject("Unknown codec request: " + requestedRaw);
}

bool WebRtcSender::switchVideoCodec(VideoCodec codec, const std::string& reason) {
    int payload = 96;
    const char* name = "VP8";
    if (codec == VideoCodec::VP9) { payload = 98; name = "VP9"; }
    else if (codec == VideoCodec::AV1) { payload = 100; name = "AV1"; }
    else if (codec == VideoCodec::H264) { payload = 102; name = "H.264"; }
    else if (codec == VideoCodec::H265) { payload = 104; name = "H.265"; }

    if (codec == VideoCodec::AV1 && m_autoCodec && !m_peerAcceptsAv1) return false;
    if (codec == VideoCodec::VP9 && m_autoCodec && !m_peerAcceptsVp9) return false;
    if (codec == VideoCodec::H265 && m_autoCodec && !m_peerAcceptsH265) return false;
    if (codec == VideoCodec::H264 && m_autoCodec && !m_peerAcceptsH264) return false;
    if (codec == VideoCodec::VP8 && m_autoCodec && !m_peerAcceptsVp8) return false;

    const bool changed = codec != m_videoCodec || payload != m_payloadType;
    m_videoCodec = codec;
    m_payloadType = payload;
    if (!changed) {
        configureVideoMediaHandler(codec);
        return true;
    }

    m_encoder.reset();
    m_vp9Encoder.reset();
    m_vp9VpxEncoder.reset();
    m_av1Encoder.reset();
    m_h264Encoder.reset();
    m_h265Encoder.reset();
    m_externalEncoderWidth = 0;
    m_externalEncoderHeight = 0;
    m_externalConfiguredFps = 0;
    m_externalConfiguredBitrateKbps = 0;
    m_externalFrameCounter = 0;
    m_codecUnhealthyWindows = 0;
    m_av1EmptyOutputFrames = 0;
    m_forceKeyframe = true;
    m_lastCodecSwitchAt = std::chrono::steady_clock::now();
    configureVideoMediaHandler(codec);

    LogInfo("[codec] live switch session=" + m_sessionId + " codec=" + name +
        " payload=" + std::to_string(payload) + " reason=" + reason);
    LogSupportEvent(std::string("Codec switched to ") + name + " - " + reason);
    return true;
}

void WebRtcSender::configureVideoMediaHandler(VideoCodec codec) {
    if (!m_track) return;

    if (m_nativeVideoRtpConfig) {
        m_sequence = m_nativeVideoRtpConfig->sequenceNumber;
        m_externalLastRtpTimestamp = m_nativeVideoRtpConfig->timestamp;
        m_nativeVideoRtpConfig.reset();
    }

    // VP8/VP9/H.264 use Hi5Central's existing manual RTP packetization.
    if (codec != VideoCodec::AV1 && codec != VideoCodec::H265) {
        m_track->setMediaHandler(nullptr);
        return;
    }

    auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(
        m_ssrc, "video-stream", static_cast<uint8_t>(m_payloadType), 90000);
    cfg->sequenceNumber = m_sequence;
    cfg->timestamp = m_externalLastRtpTimestamp != 0 ? m_externalLastRtpTimestamp : randomU32();

    std::shared_ptr<rtc::MediaHandler> packetizer;
    if (codec == VideoCodec::AV1) {
        packetizer = std::make_shared<rtc::AV1RtpPacketizer>(
            rtc::AV1RtpPacketizer::Packetization::TemporalUnit, cfg);
    }
    else {
        packetizer = std::make_shared<rtc::H265RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, cfg);
    }
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(cfg));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    m_track->setMediaHandler(packetizer);
    m_nativeVideoRtpConfig = cfg;
}

void WebRtcSender::observeCodecHealth(double encodeAvgMs, double encodeMaxMs, double sendAvgMs) {
    if (!m_autoCodec) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_lastCodecSwitchAt.time_since_epoch().count() != 0 &&
        now - m_lastCodecSwitchAt < std::chrono::seconds(10)) {
        return;
    }

    // Judge encoder health against the capture rate the streamer is actually
    // asking for now, not the FPS the current encoder happened to be created
    // with. VP9 stays interaction-ready while idle, so its configured FPS can
    // intentionally be higher than the 2 FPS idle capture cadence.
    const int hintedFps = m_externalHintFps.load();
    const int healthFps = std::max(1, hintedFps > 0 ? hintedFps : m_externalConfiguredFps);
    const double frameBudgetMs = 1000.0 / static_cast<double>(healthFps);
    const double viewerRttMs = m_viewerRttMs.load();
    const double viewerJitterMs = m_viewerJitterMs.load();
    const double viewerJitterBufferMs = m_viewerJitterBufferMs.load();
    const bool encoderUnhealthy = encodeAvgMs > std::max(35.0, frameBudgetMs * 0.80) ||
        encodeMaxMs > std::max(120.0, frameBudgetMs * 2.5);
    const bool networkPressured = viewerRttMs > 250.0 || viewerJitterBufferMs > 250.0 || viewerJitterMs > 80.0;

    if (!encoderUnhealthy) {
        if (networkPressured) {
            LogInfo("[codec] network pressure without encoder pressure session=" + m_sessionId +
                " rtt_ms=" + std::to_string(viewerRttMs) +
                " jitter_ms=" + std::to_string(viewerJitterMs) +
                " jitter_buffer_ms=" + std::to_string(viewerJitterBufferMs) +
                " action=hold_codec");
        }
        m_codecUnhealthyWindows = 0;
        return;
    }

    ++m_codecUnhealthyWindows;
    LogInfo("[codec] unhealthy window session=" + m_sessionId +
        " count=" + std::to_string(m_codecUnhealthyWindows) +
        " encode_avg_ms=" + std::to_string(encodeAvgMs) +
        " encode_max_ms=" + std::to_string(encodeMaxMs) +
        " send_avg_ms=" + std::to_string(sendAvgMs) +
        " viewer_rtt_ms=" + std::to_string(viewerRttMs) +
        " viewer_jitter_buffer_ms=" + std::to_string(viewerJitterBufferMs));

    // Require two consecutive 5-second health windows before a codec change.
    if (m_codecUnhealthyWindows < 2) return;

    if (m_videoCodec == VideoCodec::AV1) {
        if (m_peerAcceptsVp9 && !m_vp9Failed)
            switchVideoCodec(VideoCodec::VP9, "AV1 encode/send latency remained high");
        else if (m_peerAcceptsH265 && !m_h265Failed)
            switchVideoCodec(VideoCodec::H265, "AV1 latency remained high and VP9 was unavailable");
        else if (m_peerAcceptsH264 && !m_h264Failed)
            switchVideoCodec(VideoCodec::H264, "AV1 latency remained high; using H.264 fallback");
        else if (m_peerAcceptsVp8)
            switchVideoCodec(VideoCodec::VP8, "AV1 latency remained high; using VP8 baseline");
    }
    else if (m_videoCodec == VideoCodec::H265) {
        if (m_peerAcceptsH264 && !m_h264Failed)
            switchVideoCodec(VideoCodec::H264, "H.265 encode/send latency remained high");
        else if (m_peerAcceptsVp9 && !m_vp9Failed && m_swVp9Allowed) {
            m_codecMode = "vp9_sw";
            switchVideoCodec(VideoCodec::VP9, "H.265 latency remained high; trying software VP9");
        }
        else if (m_peerAcceptsVp8)
            switchVideoCodec(VideoCodec::VP8, "H.265 latency remained high; using VP8 baseline");
    }
    else if (m_videoCodec == VideoCodec::H264 && m_peerAcceptsVp9 && !m_vp9Failed && m_swVp9Allowed) {
        m_codecMode = "vp9_sw";
        switchVideoCodec(VideoCodec::VP9, "H.264 encode/send latency remained high");
    }
    else if (m_videoCodec == VideoCodec::VP9 && m_peerAcceptsVp8) {
        switchVideoCodec(VideoCodec::VP8, "VP9 encode/send latency remained high");
    }
    else if (m_videoCodec == VideoCodec::H264 && m_peerAcceptsVp8) {
        switchVideoCodec(VideoCodec::VP8, "H.264 latency remained high and VP9 was unavailable");
    }
}

void WebRtcSender::ensureDirectCaptureInitialized() {
    if (m_mode != Mode::DirectCapture) {
        return;
    }

    if (!m_source) {
        m_source = std::make_unique<DesktopFrameSource>();
        const auto d = m_source->currentDisplayInfo();
        m_injector.setTargetDisplayRect(d.x, d.y, d.width, d.height);
    }
}

void WebRtcSender::start() {
    m_offerSignalSent = false;
    createPeerConnection();

    if (!m_offerSent.exchange(true)) {
        m_pc->setLocalDescription();
    }
}

void WebRtcSender::stop() {
    m_running = false;
    m_canSend = false;
#ifdef _WIN32
    stopAudioLoopback();
#endif

    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }

    m_inputDc.reset();
    m_inputMoveDc.reset();
    m_inputControlDc.reset();
#ifdef _WIN32
    m_audioTrack.reset();
    m_audioRtpConfig.reset();
#endif
    m_track.reset();
    m_pc.reset();
}

#ifdef _WIN32
void WebRtcSender::startAudioLoopback() {
    std::lock_guard<std::mutex> lock(m_audioMu);
    if (!m_enableAudio || m_audioCapture || !m_audioTrack || !m_audioTrack->isOpen()) return;

    int opusError = OPUS_OK;
    m_opusEncoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &opusError);
    if (!m_opusEncoder || opusError != OPUS_OK) {
        LogWarn("[audio] Opus encoder creation failed error=" + std::to_string(opusError));
        if (m_opusEncoder) { opus_encoder_destroy(m_opusEncoder); m_opusEncoder = nullptr; }
        return;
    }
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(m_opusEncoder, OPUS_SET_VBR(1));
    m_audioPcm.clear();
    m_audioCapture = std::make_unique<hi5::WasapiLoopbackCapture>();
    auto* capture = m_audioCapture.get();
    const bool started = capture->Start([this](const int16_t* samples, size_t frames) { onAudioPcm(samples, frames); });
    if (!started) {
        m_audioCapture.reset();
        opus_encoder_destroy(m_opusEncoder);
        m_opusEncoder = nullptr;
        LogWarn("[audio] loopback capture could not start session=" + m_sessionId);
        return;
    }
    LogInfo("[audio] remote audio enabled session=" + m_sessionId);
}

void WebRtcSender::stopAudioLoopback() {
    std::unique_ptr<hi5::WasapiLoopbackCapture> capture;
    {
        std::lock_guard<std::mutex> lock(m_audioMu);
        capture = std::move(m_audioCapture);
    }
    if (capture) capture->Stop();
    std::lock_guard<std::mutex> lock(m_audioMu);
    m_audioPcm.clear();
    if (m_opusEncoder) { opus_encoder_destroy(m_opusEncoder); m_opusEncoder = nullptr; }
}

void WebRtcSender::onAudioPcm(const int16_t* samples, size_t frames) {
    if (!samples || frames == 0) return;
    std::lock_guard<std::mutex> lock(m_audioMu);
    if (!m_opusEncoder) return;
    constexpr size_t kChannels = 2;
    constexpr int kFrameSamples = 960; // 20 ms at 48 kHz
    constexpr size_t kFrameValues = static_cast<size_t>(kFrameSamples) * kChannels;
    m_audioPcm.insert(m_audioPcm.end(), samples, samples + (frames * kChannels));

    while (m_audioPcm.size() >= kFrameValues) {
        unsigned char encoded[4000]{};
        const int bytes = opus_encode(m_opusEncoder, m_audioPcm.data(), kFrameSamples, encoded, static_cast<opus_int32>(sizeof(encoded)));
        m_audioPcm.erase(m_audioPcm.begin(), m_audioPcm.begin() + static_cast<std::ptrdiff_t>(kFrameValues));
        if (bytes <= 0 || !m_audioTrack || !m_audioTrack->isOpen() || !m_audioRtpConfig) continue;
        rtc::binary payload;
        payload.reserve(static_cast<size_t>(bytes));
        for (int i = 0; i < bytes; ++i) payload.push_back(static_cast<std::byte>(encoded[i]));
        try {
            m_audioTrack->send(payload);
            m_audioRtpConfig->timestamp += kFrameSamples;
        } catch (const std::exception& ex) {
            LogWarn(std::string("[audio] send failed session=") + m_sessionId + " error=" + ex.what());
        }
    }
}
#endif

bool WebRtcSender::switchMonitor(int index) {
    if (m_mode != Mode::DirectCapture) {
        return false;
    }

    ensureDirectCaptureInitialized();

    const bool ok = m_source->setDisplayIndex(index);
    if (ok) {
        m_encoder.reset();
        m_forceKeyframe = true;

        const auto d = m_source->currentDisplayInfo();
        m_injector.setTargetDisplayRect(d.x, d.y, d.width, d.height);

        std::cout << "[monitor] switched session=" << m_sessionId
            << " -> display " << index << "\n";
    }
    return ok;
}

json WebRtcSender::buildMonitorInfoMessage() const {
    json msg;
    msg["type"] = "monitor_info";
    msg["session_id"] = m_sessionId;
    msg["current"] = 0;
    msg["monitors"] = json::array();

    if (m_mode == Mode::DirectCapture && m_source) {
        msg["current"] = m_source->currentDisplayIndex();
        for (const auto& d : m_source->listDisplays()) {
            msg["monitors"].push_back({
                {"index", d.index},
                {"name", d.name},
                {"x", d.x},
                {"y", d.y},
                {"w", d.width},
                {"h", d.height},
                {"primary", d.primary}
                });
        }
    }

    return msg;
}

void WebRtcSender::attachInputDataChannelHandlers(const std::shared_ptr<rtc::DataChannel>& dc, const std::string& label) {
    if (!dc) return;

    dc->onOpen([this, label]() {
        std::cout << "[dc] " << label << " open session=" << m_sessionId << "\n";
        });

    dc->onClosed([this, label]() {
        std::cout << "[dc] " << label << " closed session=" << m_sessionId << "\n";
        });

    const std::weak_ptr<rtc::DataChannel> weakDc = dc;
    dc->onMessage([this, label, weakDc](rtc::message_variant data) {
        try {
            if (const auto* b = std::get_if<rtc::binary>(&data)) {
                if (handleBinaryMousePacket(*b, label)) {
                    return;
                }
            }

            if (const auto* s = std::get_if<std::string>(&data)) {
                const auto msg = json::parse(*s, nullptr, false);
                if (!msg.is_discarded()) {
                    const std::string kind = msg.value("kind", msg.value("type", ""));

                    if (kind == "viewer_diagnostics") {
                        m_viewerRttMs = msg.value("rtt_ms", 0.0);
                        m_viewerJitterMs = msg.value("jitter_ms", 0.0);
                        m_viewerJitterBufferMs = msg.value("jitter_buffer_ms", 0.0);
                        m_viewerBitrateKbps = msg.value("bitrate_kbps", 0.0);
                        return;
                    }

                    if (kind == "dev_codec_switch") {
                        const std::string requested = msg.value("codec", std::string());
                        {
                            std::lock_guard<std::mutex> lock(m_codecSwitchMu);
                            m_pendingDevCodecSwitch = requested;
                        }
                        if (auto replyDc = weakDc.lock()) {
                            try {
                                replyDc->send(json{
                                    {"type", "dev_codec_switch_result"},
                                    {"status", "queued"},
                                    {"requested", requested},
                                    {"active", activeVideoCodecName()},
                                    {"detail", "Codec switch queued for the next video frame boundary"}
                                }.dump());
                            }
                            catch (...) {
                            }
                        }
                        LogInfo("[codec] dev switch queued session=" + m_sessionId + " requested=" + requested);
                        return;
                    }

                    // Mouse movement is high frequency and best-effort. Keep the
                    // native cursor path responsive, but do not force the expensive
                    // full-motion VP8 profile for every single mouse packet. Clicks,
                    // wheel and keyboard still boost to motion immediately.
                    if (kind != "mouse_move") {
                        m_externalHintMode = std::max(m_externalHintMode.load(), 2);
                    }
                    if (m_mode == Mode::ExternalFeed && m_inputEventFn) {
                        m_inputEventFn(msg);
                    }
                    else {
                        m_injector.handleMessage(msg);
                    }
                }
            }
        }
        catch (const std::exception& ex) {
            std::cerr << "[dc] " << label << " input parse/dispatch failed session="
                << m_sessionId << ": " << ex.what() << "\n";
        }
        catch (...) {
        }
        });
}

namespace {
    static uint8_t hi5ByteAt(const rtc::binary& data, size_t i) {
        return std::to_integer<uint8_t>(data[i]);
    }

    static uint32_t hi5ReadU32Le(const rtc::binary& data, size_t offset) {
        return static_cast<uint32_t>(hi5ByteAt(data, offset + 0)) |
            (static_cast<uint32_t>(hi5ByteAt(data, offset + 1)) << 8) |
            (static_cast<uint32_t>(hi5ByteAt(data, offset + 2)) << 16) |
            (static_cast<uint32_t>(hi5ByteAt(data, offset + 3)) << 24);
    }

    static float hi5ReadF32Le(const rtc::binary& data, size_t offset) {
        uint32_t raw = hi5ReadU32Le(data, offset);
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    static double hi5ReadF64Le(const rtc::binary& data, size_t offset) {
        uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw |= (static_cast<uint64_t>(hi5ByteAt(data, offset + static_cast<size_t>(i))) << (8 * i));
        }
        double value = 0.0;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
}

bool WebRtcSender::handleBinaryMousePacket(const rtc::binary& data, const std::string& label) {
    // Binary mouse packet v1:
    // byte 0     : packet type 1 = absolute mouse move
    // bytes 1-4  : uint32 little-endian sequence
    // bytes 5-8  : float32 little-endian x_norm
    // bytes 9-12 : float32 little-endian y_norm
    // bytes 13-20: optional float64 little-endian viewer timestamp in ms
    if (label != "viewer-mouse-move" && label != "input-move") {
        return false;
    }
    if (data.size() < 13) {
        return false;
    }

    const uint8_t packetType = hi5ByteAt(data, 0);
    if (packetType != 1) {
        return false;
    }

    const uint32_t seq32 = hi5ReadU32Le(data, 1);
    const uint64_t seq = static_cast<uint64_t>(seq32);
    const double xNorm = std::max(0.0, std::min(1.0, static_cast<double>(hi5ReadF32Le(data, 5))));
    const double yNorm = std::max(0.0, std::min(1.0, static_cast<double>(hi5ReadF32Le(data, 9))));
    const double clientTsMs = data.size() >= 21 ? hi5ReadF64Le(data, 13) : 0.0;

    if (seq > 0) {
        uint64_t prev = m_lastBinaryMouseSeq.load(std::memory_order_relaxed);
        while (seq > prev) {
            if (m_lastBinaryMouseSeq.compare_exchange_weak(prev, seq,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        }
        if (seq <= prev) {
            return true;
        }
    }

    // Pointer movement is transported independently from video. Do not wake
    // the encoder profile for cursor-only motion.

    const auto receivedAt = std::chrono::steady_clock::now();
    bool injected = false;
    if (m_directMouseMoveFn) {
        injected = m_directMouseMoveFn(xNorm, yNorm, seq, clientTsMs);
    }

    if (!injected) {
        json msg;
        msg["kind"] = "mouse_move";
        msg["x_norm"] = xNorm;
        msg["y_norm"] = yNorm;
        msg["seq"] = seq;
        if (clientTsMs > 0.0) msg["client_ts"] = clientTsMs;
        if (m_mode == Mode::ExternalFeed && m_inputEventFn) {
            m_inputEventFn(msg);
        }
        else {
            m_injector.handleMessage(msg);
        }
    }

    const uint64_t count = m_binaryMousePackets.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now = std::chrono::steady_clock::now();
    if (m_nextBinaryMouseLog.time_since_epoch().count() == 0 || now >= m_nextBinaryMouseLog) {
        const auto rxToReturnUs = std::chrono::duration_cast<std::chrono::microseconds>(now - receivedAt).count();
        std::cout << "[mouse] binary fast path session=" << m_sessionId
            << " label=" << label
            << " seq=" << seq
            << " x=" << std::fixed << std::setprecision(4) << xNorm
            << " y=" << std::fixed << std::setprecision(4) << yNorm
            << " injected=" << (injected ? 1 : 0)
            << " rx_to_return_us=" << rxToReturnUs
            << " packets=" << count
            << "\n";
        m_nextBinaryMouseLog = now + std::chrono::seconds(2);
    }

    return true;
}

std::shared_ptr<rtc::DataChannel> WebRtcSender::createNamedInputDataChannel(const std::string& label) {
    auto dc = m_pc->createDataChannel(label);
    attachInputDataChannelHandlers(dc, label);
    return dc;
}


bool WebRtcSender::sendControlMessage(const json& msg) {
    return sendControlMessageText(msg.dump());
}

bool WebRtcSender::sendControlMessageText(const std::string& text) {
    auto sendOn = [&](const std::shared_ptr<rtc::DataChannel>& dc) -> bool {
        if (!dc || !dc->isOpen()) return false;
        try {
            dc->send(text);
            return true;
        }
        catch (...) {
            return false;
        }
        };

    if (sendOn(m_inputControlDc)) return true;
    if (sendOn(m_inputDc)) return true;
    return false;
}

void WebRtcSender::setExternalStreamHint(int streamMode, int targetFps, bool backstageMode, bool secureDesktop) {
    const int requestedMode = std::max(0, std::min(2, streamMode));
    const int requestedFps = std::max(0, std::min(120, targetFps));
    const int idleDelayMs = readEnvInt("HI5_VP8_IDLE_DELAY_MS", 1000, 0, 30000);
    const auto now = std::chrono::steady_clock::now();

    // Mouse/input activity should move the encoder back to active/motion
    // immediately, but dropping back to the very cheap idle VP8 profile too
    // quickly makes the first frame after every small pause feel heavy. Hold
    // the last non-idle profile for a short grace window after motion stops.
    if (requestedMode > 0) {
        m_externalLastNonIdleHintAt = now;
        m_externalLastNonIdleMode = requestedMode;
        m_externalHintMode = requestedMode;
        m_externalHintFps = requestedFps;
    }
    else {
        bool holdNonIdle = false;
        if (idleDelayMs > 0 && m_externalLastNonIdleHintAt.time_since_epoch().count() != 0) {
            const auto idleForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_externalLastNonIdleHintAt).count();
            holdNonIdle = idleForMs < idleDelayMs;
        }

        if (holdNonIdle) {
            const int heldMode = std::max(1, std::min(2, m_externalLastNonIdleMode.load()));
            m_externalHintMode = heldMode;
            // Keep the encoder cadence warm during the hold window. The frame
            // source can still skip unchanged frames, so this does not force
            // unnecessary encodes when nothing is being published.
            if (requestedFps > 0) {
                m_externalHintFps = requestedFps;
            }
        }
        else {
            m_externalHintMode = 0;
            m_externalHintFps = requestedFps;
        }
    }

    m_externalHintBackstage = backstageMode;
    m_externalHintSecure = secureDesktop;
}

void WebRtcSender::createInputDataChannel() {
    // Keep the legacy reliable ordered input channel for backwards compatibility,
    // but add separate SCTP streams so high-rate mouse movement cannot sit in
    // front of clicks/keyboard. Mouse movement is sent on input-move, while
    // buttons/keys/wheel use input-control.
    m_inputDc = createNamedInputDataChannel("input");
    m_inputMoveDc = createNamedInputDataChannel("input-move");
    m_inputControlDc = createNamedInputDataChannel("input-control");
}


static void replaceAllInPlace(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string normalizeOutgoingH264SdpForDesktop(std::string sdp, int bitrateKbps) {
    // Keep the SDP at Constrained Baseline level 3.1 (42e01f) and cap the
    // encoded H.264 resolution to 720p in the sender. Chromium/WebView was
    // answering with 42e01f even when the offer advertised a higher level, so
    // sending oversized desktop frames produced a connected black video track.
    replaceAllInPlace(sdp, "profile-level-id=42e034", "profile-level-id=42e01f");
    replaceAllInPlace(sdp, "profile-level-id=420034", "profile-level-id=42e01f");
    replaceAllInPlace(sdp, "profile-level-id=42C034", "profile-level-id=42e01f");
    replaceAllInPlace(sdp, "profile-level-id=42c034", "profile-level-id=42e01f");

    // If a previous pass accidentally produced an enormous AS value, normalise
    // it to the configured kbps. b=AS is in kbps in SDP.
    const std::string desired = "b=AS:" + std::to_string(std::max(500, bitrateKbps));
    const size_t mVideo = sdp.find("m=video");
    if (mVideo != std::string::npos) {
        const size_t nextM = sdp.find("\r\nm=", mVideo + 2);
        const size_t sectionEnd = (nextM == std::string::npos) ? sdp.size() : nextM;
        const size_t bPos = sdp.find("\r\nb=AS:", mVideo);
        if (bPos != std::string::npos && bPos < sectionEnd) {
            const size_t lineEnd = sdp.find("\r\n", bPos + 2);
            if (lineEnd != std::string::npos) {
                sdp.replace(bPos + 2, lineEnd - (bPos + 2), desired);
            }
        }
    }

    return sdp;
}

void WebRtcSender::signalLocalOfferIfReady() {
    if (m_offerSignalSent.exchange(true)) {
        return;
    }

    auto desc = m_pc->localDescription();
    if (!desc.has_value()) {
        m_offerSignalSent = false;
        return;
    }

    std::string sdp = std::string(desc.value());

    if (m_videoCodec == VideoCodec::H264 || m_autoCodec) {
        sdp = normalizeOutgoingH264SdpForDesktop(sdp, m_bitrateKbps);
        std::cout << "[codec] outgoing H.264 SDP normalized session=" << m_sessionId
            << " profile_level_id=42e01f"
            << " bitrate_kbps=" << m_bitrateKbps << "\n";
    }

    if (sdp.find("m=video") == std::string::npos) {
        std::cout << "[pc] local description not ready yet (no video m-line) session="
            << m_sessionId << "\n";
        m_offerSignalSent = false;
        return;
    }

    json msg;
    msg["type"] = "webrtc_offer";
    msg["session_id"] = m_sessionId;
    msg["sdp"] = sdp;
    msg["sdp_type"] = desc->typeString();

    m_signalSend(msg.dump());
    std::cout << "[pc] local offer sent session=" << m_sessionId << "\n";
}

void WebRtcSender::createPeerConnection() {
    rtc::Configuration config;
    config.forceMediaTransport = true;

    if (m_iceServers.empty()) {
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        config.iceServers.emplace_back("stun:stun1.l.google.com:19302");
    }
    else {
        for (const auto& s : m_iceServers) {
            config.iceServers.emplace_back(s);
        }
    }

    m_pc = std::make_shared<rtc::PeerConnection>(config);

    // Accept viewer-created low-latency input channels too. The viewer uses
    // an unordered/unreliable data channel for high-rate mouse movement, while
    // the reliable agent-created channels remain available for clicks, keys and
    // older viewer builds.
    m_pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        if (!dc) return;
        std::string label = "viewer-input";
        try {
            label = dc->label();
        }
        catch (...) {
        }
        std::cout << "[dc] remote channel accepted label=" << label
            << " session=" << m_sessionId << "\n";
        attachInputDataChannelHandlers(dc, label);
        });

    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    if (m_autoCodec) {
        // Keep every supported candidate negotiated on the same media section/SSRC.
        // Auto will only select codecs the Viewer answers and the endpoint can encode.
        media.addAV1Codec(100);
        media.addVP9Codec(98);
        media.addH265Codec(104);
        media.addH264Codec(102);
        media.addVP8Codec(96);
        LogInfo("[codec] SDP adaptive offer AV1=100 VP9=98 H265=104 H264=102 VP8=96 session=" + m_sessionId);
    }
    else if (m_videoCodec == VideoCodec::AV1) {
        try {
            media.addAV1Codec(m_payloadType);
            LogInfo("[codec] SDP offering AV1 payload=" + std::to_string(m_payloadType) +
                " session=" + m_sessionId);
        } catch (...) {
            LogInfo("[codec] addAV1Codec failed before offer; falling back to VP8 session=" + m_sessionId);
            m_videoCodec = VideoCodec::VP8;
            m_payloadType = 96;
            media.addVP8Codec(m_payloadType);
        }
    }
    else if (m_videoCodec == VideoCodec::H265) {
        try {
            media.addH265Codec(m_payloadType);
            LogInfo("[codec] SDP offering H.265 payload=" + std::to_string(m_payloadType) +
                " session=" + m_sessionId);
        } catch (...) {
            LogInfo("[codec] addH265Codec failed before offer; falling back to VP8 session=" + m_sessionId);
            m_videoCodec = VideoCodec::VP8;
            m_payloadType = 96;
            media.addVP8Codec(m_payloadType);
        }
    }
    else if (m_videoCodec == VideoCodec::H264) {
        try {
            media.addH264Codec(m_payloadType);
            std::cout << "[codec] SDP offering experimental H.264 payload=" << m_payloadType
                << " session=" << m_sessionId << "\n";
        }
        catch (...) {
            std::cout << "[codec] addH264Codec failed before offer; falling back to VP8 session="
                << m_sessionId << "\n";
            m_videoCodec = VideoCodec::VP8;
            m_payloadType = 96;
            media.addVP8Codec(m_payloadType);
        }
    }
    else if (m_videoCodec == VideoCodec::VP9) {
        try {
            media.addVP9Codec(m_payloadType);
            LogInfo("[codec] SDP offering VP9 payload=" + std::to_string(m_payloadType) +
                " session=" + m_sessionId);
        }
        catch (...) {
            LogInfo("[codec] addVP9Codec failed before offer; falling back to VP8 session=" +
                m_sessionId);
            m_videoCodec = VideoCodec::VP8;
            m_payloadType = 96;
            media.addVP8Codec(m_payloadType);
        }
    }
    else {
        media.addVP8Codec(m_payloadType);
    }

    const std::string selectedCodecName =
        m_videoCodec == VideoCodec::AV1 ? "AV1" :
        m_videoCodec == VideoCodec::H265 ? "H.265" :
        m_videoCodec == VideoCodec::H264 ? "H.264" :
        m_videoCodec == VideoCodec::VP9 ? "VP9" : "VP8";
    LogSupportEvent("Codec: " + selectedCodecName);

    media.addSSRC(m_ssrc, "video-stream");
    media.setBitrate(m_bitrateKbps * 1000);

    m_track = m_pc->addTrack(media);
    configureVideoMediaHandler(m_videoCodec);

#ifdef _WIN32
    if (m_enableAudio) {
        constexpr uint8_t kAudioPayloadType = 111;
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(kAudioPayloadType);
        audio.addSSRC(m_audioSsrc, "audio-stream");
        m_audioTrack = m_pc->addTrack(audio);
        m_audioRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            m_audioSsrc, "audio-stream", kAudioPayloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(m_audioRtpConfig);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(m_audioRtpConfig));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        m_audioTrack->setMediaHandler(packetizer);
        m_audioTrack->onOpen([this]() {
            LogInfo("[audio] WebRTC audio track open session=" + m_sessionId);
            startAudioLoopback();
        });
        m_audioTrack->onClosed([this]() {
            LogInfo("[audio] WebRTC audio track closed session=" + m_sessionId);
        });
    }
#endif

    createInputDataChannel();

    m_track->onOpen([this]() {
        std::cout << "[track] open session=" << m_sessionId << "\n";
        m_canSend = true;

        if (m_mode == Mode::DirectCapture) {
            startStreamingThread();
        }
        });

    m_track->onClosed([this]() {
        std::cout << "[track] closed session=" << m_sessionId << "\n";
        m_canSend = false;
        if (m_connectionClosedFn) {
            m_connectionClosedFn("track_closed");
        }
        });

    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        std::cout << "[pc] state=" << static_cast<int>(state)
            << " session=" << m_sessionId << "\n";
        if (state == rtc::PeerConnection::State::Connected) {
            LogSupportEvent("Connected - WebRTC");
        }
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            m_canSend = false;
            if (m_connectionClosedFn) {
                m_connectionClosedFn("peer_connection_closed_or_failed");
            }
        }
        });

    m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        std::cout << "[pc] gathering=" << static_cast<int>(state)
            << " session=" << m_sessionId << "\n";

        if (state == rtc::PeerConnection::GatheringState::InProgress ||
            state == rtc::PeerConnection::GatheringState::Complete) {
            signalLocalOfferIfReady();
        }
        });

    m_pc->onLocalDescription([this](rtc::Description) {
        signalLocalOfferIfReady();
        });

    m_pc->onLocalCandidate([this](rtc::Candidate cand) {
        json msg;
        msg["type"] = "ice_candidate";
        msg["session_id"] = m_sessionId;
        msg["candidate"] = std::string(cand);
        msg["sdpMid"] = cand.mid();
        msg["sdpMLineIndex"] = 0;
        m_signalSend(msg.dump());
        });
}

void WebRtcSender::handleSignalingMessage(const std::string& jsonText) {
    const auto msg = json::parse(jsonText, nullptr, false);
    if (msg.is_discarded()) {
        std::cerr << "[signal] invalid json session=" << m_sessionId << "\n";
        return;
    }

    const std::string type = msg.value("type", "");
    std::cout << "[signal] inbound type=" << type << " session=" << m_sessionId << "\n";

    auto jsonString = [](const json& value, const std::string& fallback = "") -> std::string {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        return fallback;
        };

    auto normaliseCandidate = [](std::string candidate) -> std::string {
        while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == '\n' || candidate.back() == ' ' || candidate.back() == '\t')) {
            candidate.pop_back();
        }

        while (!candidate.empty() && (candidate.front() == ' ' || candidate.front() == '\t')) {
            candidate.erase(candidate.begin());
        }

        const std::string aPrefix = "a=candidate:";
        if (candidate.rfind(aPrefix, 0) == 0) {
            candidate = candidate.substr(2);
        }

        return candidate;
        };

    if (type == "webrtc_answer" || type == "answer" || type == "viewer_answer") {
        if (m_answerHandled.exchange(true)) {
            std::cout << "[signal] duplicate answer ignored session=" << m_sessionId << "\n";
            return;
        }

        std::string sdp;
        std::string sdpType = "answer";

        if (msg.contains("sdp") && msg["sdp"].is_string()) {
            sdp = msg["sdp"].get<std::string>();
        }
        else if (msg.contains("description") && msg["description"].is_object()) {
            sdp = jsonString(msg["description"].value("sdp", json()));
            sdpType = jsonString(msg["description"].value("type", json()), "answer");
        }
        else if (msg.contains("answer") && msg["answer"].is_object()) {
            sdp = jsonString(msg["answer"].value("sdp", json()));
            sdpType = jsonString(msg["answer"].value("type", json()), "answer");
        }

        if (msg.contains("sdp_type") && msg["sdp_type"].is_string()) {
            sdpType = msg["sdp_type"].get<std::string>();
        }

        if (sdp.empty()) {
            std::cerr << "[signal] answer missing sdp session=" << m_sessionId << " payload=" << jsonText.substr(0, 400) << "\n";
            return;
        }

        try {
            std::cout << "[signal] got answer session=" << m_sessionId
                << " sdp_len=" << sdp.size()
                << " type=" << sdpType << "\n";

            selectAutoCodecFromAnswer(sdp);
            m_pc->setRemoteDescription(rtc::Description(sdp, sdpType));

            std::cout << "[signal] set remote description ok session=" << m_sessionId << "\n";
        }
        catch (const std::exception& ex) {
            std::cerr << "[signal] set remote description failed session=" << m_sessionId
                << " error=" << ex.what() << "\n";
        }

        return;
    }

    if (type == "ice_candidate" || type == "candidate") {
        std::string cand;
        std::string mid = "0";

        if (msg.contains("candidate") && msg["candidate"].is_string()) {
            cand = msg["candidate"].get<std::string>();
        }
        else if (msg.contains("candidate") && msg["candidate"].is_object()) {
            const auto& c = msg["candidate"];
            cand = jsonString(c.value("candidate", json()));
            mid = jsonString(c.value("sdpMid", json()), mid);
        }

        if (msg.contains("sdpMid") && msg["sdpMid"].is_string()) {
            mid = msg["sdpMid"].get<std::string>();
        }

        cand = normaliseCandidate(cand);

        if (cand.empty()) {
            std::cerr << "[signal] candidate missing candidate string session=" << m_sessionId
                << " payload=" << jsonText.substr(0, 400) << "\n";
            return;
        }

        try {
            std::cout << "[signal] got candidate session=" << m_sessionId
                << " mid=" << mid
                << " len=" << cand.size()
                << " preview=" << cand.substr(0, 120) << "\n";

            m_pc->addRemoteCandidate(rtc::Candidate(cand, mid));

            std::cout << "[signal] add remote candidate ok session=" << m_sessionId << "\n";
        }
        catch (const std::exception& ex) {
            std::cerr << "[signal] add remote candidate failed session=" << m_sessionId
                << " error=" << ex.what()
                << " candidate=" << cand.substr(0, 200) << "\n";
        }

        return;
    }

    std::cout << "[signal] ignored type=" << type << " session=" << m_sessionId << "\n";
}

void WebRtcSender::startStreamingThread() {
    if (m_running.exchange(true)) {
        return;
    }

    m_streamThread = std::thread([this]() {
        streamingLoop();
        });
}

void WebRtcSender::streamingLoop() {
    ensureDirectCaptureInitialized();

    const auto frameInterval = std::chrono::milliseconds(1000 / m_fps);
    auto nextMonitorPoll = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    std::string lastDisplaySig = makeDisplaySignature(m_source->listDisplays());
    int lastCurrentIndex = m_source->currentDisplayIndex();

    while (m_running) {
        const auto start = std::chrono::steady_clock::now();

        if (start >= nextMonitorPoll) {
            try {
                const auto displays = m_source->listDisplays();
                const auto sig = makeDisplaySignature(displays);
                const int current = m_source->currentDisplayIndex();

                if (sig != lastDisplaySig || current != lastCurrentIndex) {
                    lastDisplaySig = sig;
                    lastCurrentIndex = current;

                    const auto d = m_source->currentDisplayInfo();
                    m_injector.setTargetDisplayRect(d.x, d.y, d.width, d.height);

                    m_encoder.reset();
                    m_forceKeyframe = true;

                    m_signalSend(buildMonitorInfoMessage().dump());
                    std::cout << "[monitor] updated session=" << m_sessionId
                        << " current=" << current << "\n";
                }
            }
            catch (...) {
            }

            nextMonitorPoll = start + std::chrono::seconds(2);
        }

        if (m_canSend && m_track && m_track->isOpen()) {
            try {
                I420Frame raw = m_source->nextFrame();

                if (!m_encoder || raw.width != m_width || raw.height != m_height) {
                    m_width = raw.width;
                    m_height = raw.height;
                    m_encoder = std::make_unique<Vp8Encoder>(m_width, m_height, m_fps, m_bitrateKbps);
                    m_forceKeyframe = true;

                    std::cout << "[stream] encoder resized session=" << m_sessionId
                        << " -> " << m_width << "x" << m_height << "\n";
                }

                const bool forceKf = m_forceKeyframe.exchange(false);
                EncodedFrame encoded = m_encoder->encode(raw, forceKf);
                if (!encoded.data.empty()) {
                    sendRtpVp8Frame(encoded);
                }
            }
            catch (const std::exception& ex) {
                const std::string err = ex.what();
                std::cerr << "[stream] encode/send error session=" << m_sessionId
                    << ": " << err << "\n";

                if (looksLikeCaptureResetError(err)) {
                    try {
                        const int wantedDisplay = m_source ? m_source->currentDisplayIndex() : 0;

                        std::cout << "[stream] capture reset session=" << m_sessionId
                            << " display=" << wantedDisplay << "\n";

                        auto newSource = std::make_unique<DesktopFrameSource>();
                        newSource->setDisplayIndex(wantedDisplay);

                        const auto d = newSource->currentDisplayInfo();
                        m_injector.setTargetDisplayRect(d.x, d.y, d.width, d.height);

                        m_source = std::move(newSource);
                        m_encoder.reset();
                        m_forceKeyframe = true;

                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    catch (const std::exception& rex) {
                        std::cerr << "[stream] capture reset failed session=" << m_sessionId
                            << ": " << rex.what() << "\n";
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }

        std::this_thread::sleep_until(start + frameInterval);
    }
}

std::vector<std::vector<uint8_t>> WebRtcSender::packetizeVp8(const EncodedFrame& frame) {
    constexpr size_t maxPayload = 1200;
    std::vector<std::vector<uint8_t>> packets;

    size_t offset = 0;
    bool first = true;

    while (offset < frame.data.size()) {
        const size_t remaining = frame.data.size() - offset;
        const size_t chunk = std::min(remaining, maxPayload - 1);

        std::vector<uint8_t> payload;
        payload.reserve(1 + chunk);

        const uint8_t descriptor = first ? 0x10 : 0x00;
        payload.push_back(descriptor);

        payload.insert(payload.end(),
            frame.data.begin() + static_cast<std::ptrdiff_t>(offset),
            frame.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));

        packets.push_back(std::move(payload));
        offset += chunk;
        first = false;
    }

    return packets;
}

void WebRtcSender::sendVp8PayloadPackets(const std::vector<std::vector<uint8_t>>& vp8Payloads,
    uint32_t rtpTimestamp) {
    std::lock_guard<std::mutex> lock(m_sendMu);

    if (!m_canSend || !m_track || !m_track->isOpen()) {
        return;
    }

    for (size_t i = 0; i < vp8Payloads.size(); ++i) {
        const bool marker = (i + 1 == vp8Payloads.size());

        const auto& payload = vp8Payloads[i];
        std::vector<uint8_t> packet;
        packet.resize(12 + payload.size());

        packet[0] = 0x80;
        packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (m_payloadType & 0x7F));
        packet[2] = static_cast<uint8_t>((m_sequence >> 8) & 0xFF);
        packet[3] = static_cast<uint8_t>(m_sequence & 0xFF);
        packet[4] = static_cast<uint8_t>((rtpTimestamp >> 24) & 0xFF);
        packet[5] = static_cast<uint8_t>((rtpTimestamp >> 16) & 0xFF);
        packet[6] = static_cast<uint8_t>((rtpTimestamp >> 8) & 0xFF);
        packet[7] = static_cast<uint8_t>(rtpTimestamp & 0xFF);
        packet[8] = static_cast<uint8_t>((m_ssrc >> 24) & 0xFF);
        packet[9] = static_cast<uint8_t>((m_ssrc >> 16) & 0xFF);
        packet[10] = static_cast<uint8_t>((m_ssrc >> 8) & 0xFF);
        packet[11] = static_cast<uint8_t>(m_ssrc & 0xFF);

        std::copy(payload.begin(), payload.end(), packet.begin() + 12);

        rtc::binary binaryPacket;
        binaryPacket.reserve(packet.size());
        for (uint8_t b : packet) {
            binaryPacket.push_back(static_cast<std::byte>(b));
        }

        m_track->send(binaryPacket);
        ++m_sequence;
    }
}



std::vector<std::vector<uint8_t>> WebRtcSender::packetizeVp9(const Vp9EncodedFrame& frame) {
    constexpr size_t maxPayload = 1200;
    std::vector<std::vector<uint8_t>> packets;

    if (frame.data.empty()) {
        return packets;
    }

    size_t offset = 0;
    bool first = true;

    while (offset < frame.data.size()) {
        const size_t remaining = frame.data.size() - offset;
        const size_t chunk = std::min(remaining, maxPayload - 1);

        std::vector<uint8_t> payload;
        payload.reserve(1 + chunk);

        /*
          Minimal VP9 RTP payload descriptor:
          - I=0, P=0, L=0, F=0, B/E set by packet boundary.
          - This is enough for a first WebRTC VP9 validation path with one spatial layer.
        */
        uint8_t descriptor = 0x00;
        if (first) {
            descriptor |= 0x08; // B: beginning of frame
        }
        if (chunk == remaining) {
            descriptor |= 0x04; // E: end of frame
        }

        payload.push_back(descriptor);
        payload.insert(payload.end(),
            frame.data.begin() + static_cast<std::ptrdiff_t>(offset),
            frame.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));

        packets.push_back(std::move(payload));
        offset += chunk;
        first = false;
    }

    return packets;
}

void WebRtcSender::sendVp9PayloadPackets(const std::vector<std::vector<uint8_t>>& vp9Payloads,
    uint32_t rtpTimestamp) {
    std::lock_guard<std::mutex> lock(m_sendMu);

    if (!m_canSend || !m_track || !m_track->isOpen()) {
        return;
    }

    for (size_t i = 0; i < vp9Payloads.size(); ++i) {
        const bool marker = (i + 1 == vp9Payloads.size());
        const auto& payload = vp9Payloads[i];

        std::vector<uint8_t> packet;
        packet.resize(12 + payload.size());

        packet[0] = 0x80;
        packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (m_payloadType & 0x7F));
        packet[2] = static_cast<uint8_t>((m_sequence >> 8) & 0xFF);
        packet[3] = static_cast<uint8_t>(m_sequence & 0xFF);
        packet[4] = static_cast<uint8_t>((rtpTimestamp >> 24) & 0xFF);
        packet[5] = static_cast<uint8_t>((rtpTimestamp >> 16) & 0xFF);
        packet[6] = static_cast<uint8_t>((rtpTimestamp >> 8) & 0xFF);
        packet[7] = static_cast<uint8_t>(rtpTimestamp & 0xFF);
        packet[8] = static_cast<uint8_t>((m_ssrc >> 24) & 0xFF);
        packet[9] = static_cast<uint8_t>((m_ssrc >> 16) & 0xFF);
        packet[10] = static_cast<uint8_t>((m_ssrc >> 8) & 0xFF);
        packet[11] = static_cast<uint8_t>(m_ssrc & 0xFF);

        std::copy(payload.begin(), payload.end(), packet.begin() + 12);

        rtc::binary binaryPacket;
        binaryPacket.reserve(packet.size());
        for (uint8_t b : packet) {
            binaryPacket.push_back(static_cast<std::byte>(b));
        }

        m_track->send(binaryPacket);
        ++m_sequence;
    }
}

void WebRtcSender::sendRtpVp9Frame(const Vp9EncodedFrame& frame) {
    const auto vp9Payloads = packetizeVp9(frame);

    static std::atomic<uint64_t> vp9FrameLogCounter{ 0 };
    const uint64_t c = ++vp9FrameLogCounter;

    if (c <= 5 || (c % 60) == 0) {
        LogInfo(
            "[vp9] rtp packetize frame=" + std::to_string(c) +
            " payload_count=" + std::to_string(vp9Payloads.size()) +
            " bytes=" + std::to_string(frame.data.size()) +
            " ts=" + std::to_string(frame.timestamp90k) +
            " keyframe=" + std::string(frame.keyframe ? "1" : "0")
        );
    }

    sendVp9PayloadPackets(vp9Payloads, frame.timestamp90k);
}



bool WebRtcSender::shouldUseH264Experiment() const {
    return m_videoCodec == VideoCodec::H264 && !m_h264Failed;
}

std::vector<std::vector<uint8_t>> WebRtcSender::splitH264NalUnits(const std::vector<uint8_t>& frame) {
    std::vector<std::vector<uint8_t>> nalUnits;
    if (frame.empty()) return nalUnits;

    auto isStart3 = [&](size_t i) {
        return i + 3 <= frame.size() && frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1;
        };
    auto isStart4 = [&](size_t i) {
        return i + 4 <= frame.size() && frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 0 && frame[i + 3] == 1;
        };

    bool hasAnnexB = false;
    for (size_t i = 0; i + 4 < frame.size(); ++i) {
        if (isStart3(i) || isStart4(i)) {
            hasAnnexB = true;
            break;
        }
    }

    if (hasAnnexB) {
        size_t i = 0;
        while (i < frame.size()) {
            while (i < frame.size() && !isStart3(i) && !isStart4(i)) ++i;
            if (i >= frame.size()) break;
            const size_t startCode = isStart4(i) ? 4 : 3;
            const size_t nalStart = i + startCode;
            i = nalStart;
            while (i < frame.size() && !isStart3(i) && !isStart4(i)) ++i;
            const size_t nalEnd = i;
            if (nalEnd > nalStart) {
                nalUnits.emplace_back(frame.begin() + static_cast<std::ptrdiff_t>(nalStart),
                    frame.begin() + static_cast<std::ptrdiff_t>(nalEnd));
            }
        }
        return nalUnits;
    }

    // AVCC / length-prefixed fallback using 4 byte lengths.
    size_t off = 0;
    while (off + 4 < frame.size()) {
        uint32_t len = (uint32_t(frame[off]) << 24) | (uint32_t(frame[off + 1]) << 16) |
            (uint32_t(frame[off + 2]) << 8) | uint32_t(frame[off + 3]);
        if (len == 0 || off + 4 + len > frame.size()) break;
        nalUnits.emplace_back(frame.begin() + static_cast<std::ptrdiff_t>(off + 4),
            frame.begin() + static_cast<std::ptrdiff_t>(off + 4 + len));
        off += 4 + len;
    }

    if (nalUnits.empty()) {
        nalUnits.push_back(frame);
    }

    return nalUnits;
}

void WebRtcSender::sendRtpH264Frame(const H264EncodedFrame& frame) {
    const auto nalUnits = splitH264NalUnits(frame.data);
    static std::atomic<uint64_t> h264FrameLogCounter{ 0 };
    const uint64_t c = ++h264FrameLogCounter;
    if (c <= 5 || (c % 60) == 0) {
        size_t total = 0;
        int sps = 0, pps = 0, idr = 0;
        for (const auto& n : nalUnits) {
            total += n.size();
            if (!n.empty()) {
                const int t = n[0] & 0x1F;
                if (t == 7) ++sps;
                if (t == 8) ++pps;
                if (t == 5) ++idr;
            }
        }
        LogInfo(
            "[h264] rtp packetize frame=" + std::to_string(c) +
            " nal_count=" + std::to_string(nalUnits.size()) +
            " total_nal_bytes=" + std::to_string(total) +
            " sps=" + std::to_string(sps) +
            " pps=" + std::to_string(pps) +
            " idr=" + std::to_string(idr) +
            " ts=" + std::to_string(frame.timestamp90k) +
            " keyframe=" + std::string(frame.keyframe ? "1" : "0")
        );
    }
    sendH264NalPayloads(nalUnits, frame.timestamp90k);
}

void WebRtcSender::sendH264NalPayloads(const std::vector<std::vector<uint8_t>>& nalUnits, uint32_t rtpTimestamp) {
    constexpr size_t maxPayload = 1200;

    std::lock_guard<std::mutex> lock(m_sendMu);

    if (!m_canSend || !m_track || !m_track->isOpen()) {
        return;
    }

    for (size_t ni = 0; ni < nalUnits.size(); ++ni) {
        const auto& nal = nalUnits[ni];
        if (nal.empty()) continue;

        if (nal.size() <= maxPayload) {
            const bool marker = (ni + 1 == nalUnits.size());
            std::vector<uint8_t> packet;
            packet.resize(12 + nal.size());

            packet[0] = 0x80;
            packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (m_payloadType & 0x7F));
            packet[2] = static_cast<uint8_t>((m_sequence >> 8) & 0xFF);
            packet[3] = static_cast<uint8_t>(m_sequence & 0xFF);
            packet[4] = static_cast<uint8_t>((rtpTimestamp >> 24) & 0xFF);
            packet[5] = static_cast<uint8_t>((rtpTimestamp >> 16) & 0xFF);
            packet[6] = static_cast<uint8_t>((rtpTimestamp >> 8) & 0xFF);
            packet[7] = static_cast<uint8_t>(rtpTimestamp & 0xFF);
            packet[8] = static_cast<uint8_t>((m_ssrc >> 24) & 0xFF);
            packet[9] = static_cast<uint8_t>((m_ssrc >> 16) & 0xFF);
            packet[10] = static_cast<uint8_t>((m_ssrc >> 8) & 0xFF);
            packet[11] = static_cast<uint8_t>(m_ssrc & 0xFF);
            std::copy(nal.begin(), nal.end(), packet.begin() + 12);

            rtc::binary binaryPacket;
            binaryPacket.reserve(packet.size());
            for (uint8_t b : packet) binaryPacket.push_back(static_cast<std::byte>(b));
            m_track->send(binaryPacket);
            ++m_sequence;
            continue;
        }

        // FU-A fragmentation for large NAL units.
        const uint8_t nalHeader = nal[0];
        const uint8_t nalF = nalHeader & 0x80;
        const uint8_t nalNri = nalHeader & 0x60;
        const uint8_t nalType = nalHeader & 0x1F;
        const uint8_t fuIndicator = nalF | nalNri | 28;

        size_t offset = 1;
        bool start = true;
        while (offset < nal.size()) {
            const size_t chunk = std::min(maxPayload - 2, nal.size() - offset);
            const bool end = (offset + chunk >= nal.size());
            const bool marker = end && (ni + 1 == nalUnits.size());

            std::vector<uint8_t> payload;
            payload.reserve(2 + chunk);
            payload.push_back(fuIndicator);
            payload.push_back(static_cast<uint8_t>((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nalType));
            payload.insert(payload.end(),
                nal.begin() + static_cast<std::ptrdiff_t>(offset),
                nal.begin() + static_cast<std::ptrdiff_t>(offset + chunk));

            std::vector<uint8_t> packet;
            packet.resize(12 + payload.size());
            packet[0] = 0x80;
            packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (m_payloadType & 0x7F));
            packet[2] = static_cast<uint8_t>((m_sequence >> 8) & 0xFF);
            packet[3] = static_cast<uint8_t>(m_sequence & 0xFF);
            packet[4] = static_cast<uint8_t>((rtpTimestamp >> 24) & 0xFF);
            packet[5] = static_cast<uint8_t>((rtpTimestamp >> 16) & 0xFF);
            packet[6] = static_cast<uint8_t>((rtpTimestamp >> 8) & 0xFF);
            packet[7] = static_cast<uint8_t>(rtpTimestamp & 0xFF);
            packet[8] = static_cast<uint8_t>((m_ssrc >> 24) & 0xFF);
            packet[9] = static_cast<uint8_t>((m_ssrc >> 16) & 0xFF);
            packet[10] = static_cast<uint8_t>((m_ssrc >> 8) & 0xFF);
            packet[11] = static_cast<uint8_t>(m_ssrc & 0xFF);
            std::copy(payload.begin(), payload.end(), packet.begin() + 12);

            rtc::binary binaryPacket;
            binaryPacket.reserve(packet.size());
            for (uint8_t b : packet) binaryPacket.push_back(static_cast<std::byte>(b));
            m_track->send(binaryPacket);
            ++m_sequence;

            offset += chunk;
            start = false;
        }
    }
}

void WebRtcSender::sendExternalRawI420(const I420Frame& frame, uint64_t captureTimestampNs, bool forceKeyframe) {
    static std::atomic<uint64_t> rawEntryLogCounter{ 0 };
    const uint64_t rawEntryCount = ++rawEntryLogCounter;

    if (rawEntryCount <= 10 || rawEntryCount % 120 == 0) {
        LogInfo(
            "[external] raw entry session=" + m_sessionId +
            " codec=" + m_codecMode +
            " video_codec=" + std::string(
                m_videoCodec == VideoCodec::AV1 ? "av1" :
                m_videoCodec == VideoCodec::H265 ? "h265" :
                m_videoCodec == VideoCodec::H264 ? "h264" :
                m_videoCodec == VideoCodec::VP9 ? "vp9" : "vp8") +
            " quality=" + imageQualityMode() +
            " h264_failed=" + std::string(m_h264Failed ? "true" : "false") +
            " mode=" + std::string(m_mode == Mode::ExternalFeed ? "external" : "direct") +
            " can_send=" + std::string(m_canSend ? "true" : "false") +
            " track=" + std::string(m_track ? "yes" : "no") +
            " track_open=" + std::string((m_track && m_track->isOpen()) ? "true" : "false") +
            " frame=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
            " force_kf=" + std::string(forceKeyframe ? "true" : "false")
        );
    }

    if (m_mode != Mode::ExternalFeed || frame.width <= 0 || frame.height <= 0) {
        return;
    }
    if (!m_canSend || !m_track || !m_track->isOpen()) {
        return;
    }

    std::lock_guard<std::mutex> encodeLock(m_externalEncodeMu);

    std::string devCodecRequest;
    {
        std::lock_guard<std::mutex> lock(m_codecSwitchMu);
        devCodecRequest.swap(m_pendingDevCodecSwitch);
    }
    if (!devCodecRequest.empty()) {
        std::string activeCodec;
        std::string detail;
        const bool switched = applyDevCodecSwitch(devCodecRequest, activeCodec, detail);
        sendControlMessage(json{
            {"type", "dev_codec_switch_result"},
            {"status", switched ? "accepted" : "failed"},
            {"requested", devCodecRequest},
            {"active", activeCodec},
            {"detail", detail}
        });
        if (switched) forceKeyframe = true;
    }

    const uint32_t captureRtpTimestamp = externalRtpTimestamp(captureTimestampNs);
    const auto nowForProfile = std::chrono::steady_clock::now();
    if (m_externalLastFrameAt.time_since_epoch().count() != 0) {
        const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowForProfile - m_externalLastFrameAt).count();
        // If stream stats have not arrived yet, infer a safe profile from the frame cadence.
        if (m_externalHintFps.load() <= 0) {
            if (gapMs > 350) {
                m_externalHintMode = 0;
                m_externalHintFps = 2;
            }
            else if (gapMs > 90) {
                m_externalHintMode = 1;
                m_externalHintFps = 10;
            }
            else {
                m_externalHintMode = 2;
                m_externalHintFps = std::min(30, std::max(1, m_fps));
            }
        }
    }
    m_externalLastFrameAt = nowForProfile;

    const int requestedMode = std::max(0, std::min(2, m_externalHintMode.load()));
    const int previousEffectiveMode = m_externalEffectiveMode;
    const int wakeHoldMs = readEnvInt("HI5_VP8_WAKE_HOLD_MS", 250, 0, 5000);
    int effectiveMode = requestedMode;

    // Avoid the expensive first movement after idle by entering a short, cheaper
    // wake profile before the full motion profile. Also prevents immediate
    // idle->motion encoder thrash when a single dirty frame arrives.
    if (requestedMode <= 0) {
        m_externalWasIdle = true;
        m_externalWakeUntil = {};
    }
    else {
        if (m_externalWasIdle && wakeHoldMs > 0) {
            m_externalWakeUntil = nowForProfile + std::chrono::milliseconds(wakeHoldMs);
            m_externalWasIdle = false;
        }
        if (m_externalWakeUntil.time_since_epoch().count() != 0 && nowForProfile < m_externalWakeUntil) {
            effectiveMode = 3; // wake profile
        }
    }
    m_externalEffectiveMode = effectiveMode;

    if (previousEffectiveMode <= 0 && effectiveMode > 0 && readEnvInt("HI5_VP8_FORCE_KEYFRAME_ON_WAKE", 0, 0, 1) == 1) {
        forceKeyframe = true;
    }

    Vp8RuntimeProfile profile = buildVp8RuntimeProfile(
        effectiveMode,
        m_externalHintFps.load(),
        m_externalHintBackstage.load(),
        m_externalHintSecure.load(),
        std::max(1, m_fps),
        std::max(250, m_bitrateKbps));

    const bool sizeChanged = frame.width != m_externalEncoderWidth || frame.height != m_externalEncoderHeight;
    const bool profileChanged = profile.fps != m_externalConfiguredFps ||
        profile.bitrateKbps != m_externalConfiguredBitrateKbps ||
        profile.cpuUsed != m_externalConfiguredCpuUsed ||
        profile.maxQuantizer != m_externalConfiguredMaxQuantizer;

    if (m_videoCodec == VideoCodec::VP8 && (!m_encoder || sizeChanged)) {
        m_externalEncoderWidth = frame.width;
        m_externalEncoderHeight = frame.height;
        m_externalConfiguredFps = profile.fps;
        m_externalConfiguredBitrateKbps = profile.bitrateKbps;
        m_externalConfiguredCpuUsed = profile.cpuUsed;
        m_externalConfiguredMaxQuantizer = profile.maxQuantizer;
        m_externalProfileName = profile.name;
        m_encoder = std::make_unique<Vp8Encoder>(
            frame.width,
            frame.height,
            profile.fps,
            profile.bitrateKbps,
            profile.cpuUsed,
            profile.minQuantizer,
            profile.maxQuantizer,
            readEnvInt("HI5_VP8_THREADS", 2, 1, 8));
        m_externalFrameCounter = 0;
        forceKeyframe = true;
        m_externalLastProfileChange = nowForProfile;
        std::cout << "[external] service encoder created session=" << m_sessionId
            << " -> " << frame.width << "x" << frame.height
            << " profile=" << m_externalProfileName
            << " fps=" << profile.fps
            << " bitrate=" << profile.bitrateKbps
            << " cpuused=" << profile.cpuUsed
            << " qmax=" << profile.maxQuantizer
            << "\n";
    }
    else if (m_videoCodec == VideoCodec::VP8 && profileChanged) {
        const bool ok = m_encoder->reconfigure(
            profile.fps,
            profile.bitrateKbps,
            profile.cpuUsed,
            profile.minQuantizer,
            profile.maxQuantizer);
        if (ok) {
            m_externalConfiguredFps = profile.fps;
            m_externalConfiguredBitrateKbps = profile.bitrateKbps;
            m_externalConfiguredCpuUsed = profile.cpuUsed;
            m_externalConfiguredMaxQuantizer = profile.maxQuantizer;
            m_externalProfileName = profile.name;
            m_externalLastProfileChange = nowForProfile;
            // Do not force a keyframe for routine idle/active/motion tuning. That
            // was the main source of the visible CPU spike after waking from idle.
            std::cout << "[external] service encoder retuned session=" << m_sessionId
                << " profile=" << m_externalProfileName
                << " fps=" << profile.fps
                << " bitrate=" << profile.bitrateKbps
                << " cpuused=" << profile.cpuUsed
                << " qmax=" << profile.maxQuantizer
                << "\n";
        }
        else {
            m_encoder.reset();
            m_externalEncoderWidth = 0;
            m_externalEncoderHeight = 0;
            m_forceKeyframe = true;
            return;
        }
    }

    const auto encodeInterval = std::chrono::milliseconds(std::max(1, 1000 / std::max(1, profile.fps)));
    if (!forceKeyframe && !m_forceKeyframe.load() && m_externalLastEncodeAt.time_since_epoch().count() != 0 &&
        nowForProfile - m_externalLastEncodeAt < encodeInterval) {
        return;
    }
    m_externalLastEncodeAt = nowForProfile;

    ++m_externalFrameCounter;

    // Low-CPU policy: keyframes are event based. Viewer connect, monitor switch,
    // UAC/secure desktop and encoder resize still force a keyframe. Periodic
    // recovery keyframes are much less frequent while idle and profile-aware.
    m_externalKeyframeEvery = std::max(profile.fps, std::max(1, profile.fps) * profile.keyframeSeconds);
    const bool periodicKeyframe = (m_externalFrameCounter % m_externalKeyframeEvery) == 0;
    const bool keyframe = forceKeyframe || periodicKeyframe || m_forceKeyframe.exchange(false);

    auto sendNativePacketizedVideo = [&](const std::vector<uint8_t>& bytes) {
        if (bytes.empty() || !m_track || !m_track->isOpen() || !m_nativeVideoRtpConfig) return false;
        m_nativeVideoRtpConfig->timestamp = captureRtpTimestamp;
        rtc::binary payload;
        payload.reserve(bytes.size());
        for (uint8_t b : bytes) payload.push_back(static_cast<std::byte>(b));
        const auto sendStart = std::chrono::steady_clock::now();
        m_track->send(payload);
        const auto sendEnd = std::chrono::steady_clock::now();
        const double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
        m_externalSendMsTotal += sendMs;
        m_externalSendMsMax = std::max(m_externalSendMsMax, sendMs);
        ++m_externalSentFrames;
        m_sequence = m_nativeVideoRtpConfig->sequenceNumber;
        return true;
    };

    auto reportCodecHealth = [&](const char* codecName) {
        const auto healthNow = std::chrono::steady_clock::now();
        if (m_externalNextStatsLog.time_since_epoch().count() == 0)
            m_externalNextStatsLog = healthNow + std::chrono::seconds(5);
        if (healthNow < m_externalNextStatsLog) return;

        const double encodedCount = std::max<uint64_t>(1, m_externalEncodedFrames);
        const double sentCount = std::max<uint64_t>(1, m_externalSentFrames);
        const double encodeAvgMs = m_externalEncodeMsTotal / encodedCount;
        const double sendAvgMs = m_externalSendMsTotal / sentCount;
        LogInfo(std::string("[") + codecName + "] encode health session=" + m_sessionId +
            " encode_avg_ms=" + std::to_string(encodeAvgMs) +
            " encode_max_ms=" + std::to_string(m_externalEncodeMsMax) +
            " send_avg_ms=" + std::to_string(sendAvgMs) +
            " send_max_ms=" + std::to_string(m_externalSendMsMax));
        observeCodecHealth(encodeAvgMs, m_externalEncodeMsMax, sendAvgMs);
        m_externalEncodedFrames = 0; m_externalSentFrames = 0;
        m_externalEncodeMsTotal = 0.0; m_externalEncodeMsMax = 0.0;
        m_externalSendMsTotal = 0.0; m_externalSendMsMax = 0.0;
        m_externalNextStatsLog = healthNow + std::chrono::seconds(5);
    };

    auto selectNextAutoCodec = [&](VideoCodec failed, const std::string& why) {
        if (!m_autoCodec) return false;
        if (failed == VideoCodec::AV1) {
            if (m_peerAcceptsVp9 && !m_vp9Failed) return switchVideoCodec(VideoCodec::VP9, why + "; falling back from AV1");
            if (m_peerAcceptsH265 && !m_h265Failed) return switchVideoCodec(VideoCodec::H265, why + "; falling back from AV1");
            if (m_peerAcceptsH264 && !m_h264Failed) return switchVideoCodec(VideoCodec::H264, why + "; falling back from AV1");
        }
        if (failed == VideoCodec::H265) {
            if (m_peerAcceptsH264 && !m_h264Failed) return switchVideoCodec(VideoCodec::H264, why + "; falling back from H.265");
            if (m_peerAcceptsVp9 && !m_vp9Failed) return switchVideoCodec(VideoCodec::VP9, why + "; falling back from H.265");
        }
        if (failed == VideoCodec::H264 && m_peerAcceptsVp9 && !m_vp9Failed)
            return switchVideoCodec(VideoCodec::VP9, why + "; falling back from H.264");
        if ((failed == VideoCodec::VP9 || failed == VideoCodec::H264 || failed == VideoCodec::H265 || failed == VideoCodec::AV1) && m_peerAcceptsVp8)
            return switchVideoCodec(VideoCodec::VP8, why + "; using VP8 baseline");
        return false;
    };

    auto recoverForcedCodec = [&](VideoCodec failed, const std::string& requested, const std::string& why) {
        if (m_autoCodec) return selectNextAutoCodec(failed, why);

        bool recovered = false;
        if (failed != VideoCodec::VP9 && m_peerAcceptsVp9 && !m_vp9Failed && (m_hwVp9Available || m_swVp9Allowed)) {
            m_codecMode = m_hwVp9Available ? "vp9" : "vp9_sw";
            recovered = switchVideoCodec(VideoCodec::VP9, why + "; restoring VP9 after forced codec failure");
        }
        if (!recovered && m_peerAcceptsVp8) {
            m_codecMode = "vp8";
            recovered = switchVideoCodec(VideoCodec::VP8, why + "; restoring VP8 baseline after forced codec failure");
        }
        sendControlMessage(json{
            {"type", "dev_codec_switch_result"},
            {"status", "failed"},
            {"requested", requested},
            {"active", activeVideoCodecName()},
            {"detail", why + (recovered ? "; previous working video restored" : "; no fallback codec available")}
        });
        return recovered;
    };

    if (m_videoCodec == VideoCodec::AV1 && !m_av1Failed) {
        const int av1Fps = readEnvInt("HI5_AV1_ENCODER_FPS", std::min(30, std::max(1, m_fps)), 1, 60);
        const std::string av1Quality = imageQualityMode();
        const int av1DefaultKbps = av1Quality == "lossless" ? std::max(20000, m_bitrateKbps) :
            (av1Quality == "near_lossless" ? std::max(12000, m_bitrateKbps) :
            (av1Quality == "text" ? std::max(8000, m_bitrateKbps) : std::max(250, m_bitrateKbps)));
        const int av1Kbps = readEnvInt("HI5_AV1_ENCODER_KBPS", av1DefaultKbps, 500, 30000);
        const bool av1SizeChanged = frame.width != m_externalEncoderWidth || frame.height != m_externalEncoderHeight;
        if (!m_av1Encoder || av1SizeChanged) {
            m_av1Encoder.reset();
            m_externalEncoderWidth = frame.width; m_externalEncoderHeight = frame.height;
            m_externalConfiguredFps = av1Fps; m_externalConfiguredBitrateKbps = av1Kbps;
            auto enc = std::make_unique<Av1MfEncoder>();
            std::string err;
            const bool preferHardware = m_codecMode != "av1_sw";
            if (enc->init(frame.width, frame.height, av1Fps, av1Kbps, preferHardware, &err)) {
                m_av1Encoder = std::move(enc);
                m_externalProfileName = preferHardware ? "mediafoundation-av1-hardware-preferred" : "mediafoundation-av1-software";
                LogInfo("[av1] encoder active session=" + m_sessionId + " name=" + m_av1Encoder->encoderName());
            } else {
                LogInfo("[av1] init failed session=" + m_sessionId + " error=" + err);
                m_av1Failed = true;
                recoverForcedCodec(VideoCodec::AV1, "av1", "AV1 encoder unavailable");
                return;
            }
            forceKeyframe = true;
        }

        const auto encodeStart = std::chrono::steady_clock::now();
        Av1EncodedFrame encoded{};
        std::string err;
        bool ok = m_av1Encoder->encode(frame, keyframe || av1SizeChanged, encoded, &err);
        if (!ok && m_codecMode != "av1_sw") {
            LogInfo("[av1] hardware-preferred encode failed; same-frame software retry session=" + m_sessionId + " error=" + err);
            auto sw = std::make_unique<Av1MfEncoder>();
            std::string swErr;
            if (sw->init(frame.width, frame.height, av1Fps, av1Kbps, false, &swErr)) {
                Av1EncodedFrame retry{};
                if (sw->encode(frame, true, retry, &swErr)) {
                    m_av1Encoder = std::move(sw); m_codecMode = "av1_sw"; encoded = std::move(retry); ok = true;
                    m_externalProfileName = "mediafoundation-av1-software-fallback";
                }
            }
        }
        if (!ok) {
            LogInfo("[av1] encode failed session=" + m_sessionId + " error=" + err);
            m_av1Failed = true; m_av1Encoder.reset();
            recoverForcedCodec(VideoCodec::AV1, "av1", "AV1 encode failed");
            return;
        }
        const auto encodeEnd = std::chrono::steady_clock::now();
        const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
        m_externalEncodeMsTotal += encodeMs; m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs); ++m_externalEncodedFrames;
        if (!encoded.data.empty()) {
            m_av1EmptyOutputFrames = 0;
            sendNativePacketizedVideo(encoded.data);
        } else {
            ++m_av1EmptyOutputFrames;
            const int maxBufferedFrames = readEnvInt("HI5_AV1_MAX_EMPTY_OUTPUT_FRAMES", 10, 2, 60);
            if (m_av1EmptyOutputFrames >= maxBufferedFrames) {
                LogInfo("[av1] encoder produced no output for " + std::to_string(m_av1EmptyOutputFrames) +
                    " consecutive input frames; abandoning AV1 session=" + m_sessionId);
                m_av1Failed = true;
                m_av1Encoder.reset();
                m_av1EmptyOutputFrames = 0;
                recoverForcedCodec(VideoCodec::AV1, "av1", "AV1 encoder accepted input but produced no video");
                return;
            }
        }
        reportCodecHealth("av1");
        return;
    }

    if (m_videoCodec == VideoCodec::H265 && !m_h265Failed) {
        const int h265Fps = std::max(1, profile.fps);
        const int h265Kbps = std::max(500, profile.bitrateKbps);
        const bool h265SizeChanged = frame.width != m_externalEncoderWidth || frame.height != m_externalEncoderHeight;
        if (!m_h265Encoder || h265SizeChanged) {
            m_h265Encoder.reset();
            m_externalEncoderWidth = frame.width; m_externalEncoderHeight = frame.height;
            m_externalConfiguredFps = h265Fps; m_externalConfiguredBitrateKbps = h265Kbps;
            auto enc = std::make_unique<H265MfEncoder>();
            std::string err;
            const bool preferHardware = m_codecMode != "h265_sw";
            if (enc->init(frame.width, frame.height, h265Fps, h265Kbps, preferHardware, &err)) {
                m_h265Encoder = std::move(enc);
                m_externalProfileName = preferHardware ? "mediafoundation-h265-hardware-preferred" : "mediafoundation-h265-software";
                LogInfo("[h265] encoder active session=" + m_sessionId + " name=" + m_h265Encoder->encoderName());
            } else {
                LogInfo("[h265] init failed session=" + m_sessionId + " error=" + err);
                m_h265Failed = true;
                recoverForcedCodec(VideoCodec::H265, "h265", "H.265 encoder unavailable");
                return;
            }
            forceKeyframe = true;
        }

        const auto encodeStart = std::chrono::steady_clock::now();
        H265EncodedFrame encoded{}; std::string err;
        bool ok = m_h265Encoder->encode(frame, keyframe || h265SizeChanged, encoded, &err);
        if (!ok && m_codecMode != "h265_sw") {
            LogInfo("[h265] hardware-preferred encode failed; same-frame software retry session=" + m_sessionId + " error=" + err);
            auto sw = std::make_unique<H265MfEncoder>(); std::string swErr;
            if (sw->init(frame.width, frame.height, h265Fps, h265Kbps, false, &swErr)) {
                H265EncodedFrame retry{};
                if (sw->encode(frame, true, retry, &swErr)) {
                    m_h265Encoder = std::move(sw); m_codecMode = "h265_sw"; encoded = std::move(retry); ok = true;
                    m_externalProfileName = "mediafoundation-h265-software-fallback";
                }
            }
        }
        if (!ok) {
            LogInfo("[h265] encode failed session=" + m_sessionId + " error=" + err);
            m_h265Failed = true; m_h265Encoder.reset();
            recoverForcedCodec(VideoCodec::H265, "h265", "H.265 encode failed");
            return;
        }
        const auto encodeEnd = std::chrono::steady_clock::now();
        const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
        m_externalEncodeMsTotal += encodeMs; m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs); ++m_externalEncodedFrames;
        if (!encoded.data.empty()) sendNativePacketizedVideo(encoded.data);
        reportCodecHealth("h265");
        return;
    }

    if (m_videoCodec == VideoCodec::VP9 && !m_vp9Failed) {
        bool vp9Scaled = false;
        const int vp9MaxW = readEnvInt("HI5_VP9_MAX_WIDTH", 1920, 0, 7680);
        const int vp9MaxH = readEnvInt("HI5_VP9_MAX_HEIGHT", 1080, 0, 4320);
        const I420Frame& vp9Frame = scaleI420ForWebRtc(frame, m_vp9ScaleScratch, vp9MaxW, vp9MaxH, &vp9Scaled);
        const bool sizeChangedVp9 = vp9Frame.width != m_externalEncoderWidth || vp9Frame.height != m_externalEncoderHeight;
        const int vp9EncoderFps = readEnvInt("HI5_VP9_ENCODER_FPS", std::min(30, std::max(1, m_fps)), 1, 60);
        const std::string vp9Quality = imageQualityMode();
        const int vp9DefaultKbps = vp9Quality == "lossless" ? std::max(16000, m_bitrateKbps) :
            (vp9Quality == "near_lossless" ? std::max(12000, m_bitrateKbps) :
            (vp9Quality == "text" ? std::max(8000, m_bitrateKbps) : std::max(250, m_bitrateKbps)));
        const int vp9EncoderBitrateKbps = readEnvInt("HI5_VP9_ENCODER_KBPS", vp9DefaultKbps, 500, 30000);
        const int vp9DefaultMinQ = vp9Quality == "lossless" ? 0 : (vp9Quality == "near_lossless" ? 0 : (vp9Quality == "text" ? 2 : 4));
        const int vp9DefaultMaxQ = vp9Quality == "lossless" ? 0 : (vp9Quality == "near_lossless" ? 10 : (vp9Quality == "text" ? 22 : 38));

        auto createSoftwareVp9 = [&]() -> bool {
            try {
                m_vp9Encoder.reset();
                m_vp9VpxEncoder = std::make_unique<Vp9VpxEncoder>(
                    vp9Frame.width, vp9Frame.height, vp9EncoderFps, vp9EncoderBitrateKbps,
                    readEnvInt("HI5_VP9_CPUUSED", 8, 0, 9),
                    readEnvInt("HI5_VP9_MIN_Q", vp9DefaultMinQ, 0, 63),
                    readEnvInt("HI5_VP9_MAX_Q", vp9DefaultMaxQ, 0, 63),
                    readEnvInt("HI5_VP9_THREADS", vp9Frame.width >= 3840 ? 4 : 2, 1, 8));
                m_externalProfileName = "libvpx-vp9-interaction-ready";
                m_codecMode = "vp9_sw";
                m_vp9Failed = false;
                LogInfo("[vp9] libvpx software encoder active session=" + m_sessionId +
                    " quality=" + vp9Quality +
                    " q=" + std::to_string(vp9DefaultMinQ) + "-" + std::to_string(vp9DefaultMaxQ));
                return true;
            } catch (const std::exception& ex) {
                LogInfo("[vp9] libvpx software init failed session=" + m_sessionId + " error=" + ex.what());
                m_vp9VpxEncoder.reset();
                return false;
            }
        };

        if ((!m_vp9Encoder && !m_vp9VpxEncoder) || sizeChangedVp9) {
            m_vp9Encoder.reset();
            m_vp9VpxEncoder.reset();
            m_externalEncoderWidth = vp9Frame.width;
            m_externalEncoderHeight = vp9Frame.height;
            m_externalConfiguredFps = vp9EncoderFps;
            m_externalConfiguredBitrateKbps = vp9EncoderBitrateKbps;
            m_externalProfileName = "mediafoundation-vp9-interaction-ready";

            const bool forceSoftware = m_codecMode == "vp9_sw" || (m_autoCodec && !m_hwVp9Available);
            if (!forceSoftware) {
                auto enc = std::make_unique<Vp9MfEncoder>();
                std::string vp9Err;
                if (enc->init(vp9Frame.width, vp9Frame.height, vp9EncoderFps, vp9EncoderBitrateKbps, true, &vp9Err)) {
                    m_vp9Encoder = std::move(enc);
                    LogInfo("[vp9] Media Foundation encoder active session=" + m_sessionId +
                        " name=" + m_vp9Encoder->encoderName());
                } else {
                    LogInfo("[vp9] hardware init failed; falling back to libvpx session=" + m_sessionId +
                        " error=" + vp9Err);
                    createSoftwareVp9();
                }
            } else {
                createSoftwareVp9();
            }

            if (!m_vp9Encoder && !m_vp9VpxEncoder) {
                m_vp9Failed = true;
                if (m_autoCodec && m_peerAcceptsVp8) switchVideoCodec(VideoCodec::VP8, "VP9 encoder unavailable");
                return;
            }
            forceKeyframe = true;
        }

        try {
            const auto encodeStart = std::chrono::steady_clock::now();
            Vp9EncodedFrame encoded{};
            const bool vp9Keyframe = keyframe || sizeChangedVp9;
            bool ok = false;

            if (m_vp9VpxEncoder) {
                encoded = m_vp9VpxEncoder->encode(vp9Frame, vp9Keyframe);
                ok = true;
            } else if (m_vp9Encoder) {
                std::string vp9Err;
                ok = m_vp9Encoder->encode(vp9Frame, vp9Keyframe, encoded, &vp9Err);
                if (!ok) {
                    LogInfo("[vp9] hardware encode failed; same-frame libvpx fallback session=" + m_sessionId +
                        " error=" + vp9Err);
                    if (createSoftwareVp9()) {
                        encoded = m_vp9VpxEncoder->encode(vp9Frame, true);
                        ok = true;
                    }
                }
            }

            if (!ok) {
                m_vp9Failed = true;
                m_vp9Encoder.reset();
                m_vp9VpxEncoder.reset();
                recoverForcedCodec(VideoCodec::VP9, "vp9", "VP9 encode failed");
                return;
            }

            const auto encodeEnd = std::chrono::steady_clock::now();
            const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
            m_externalEncodeMsTotal += encodeMs;
            m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs);
            ++m_externalEncodedFrames;

            if (!encoded.data.empty()) {
                encoded.timestamp90k = captureRtpTimestamp;
                const auto sendStart = std::chrono::steady_clock::now();
                sendRtpVp9Frame(encoded);
                const auto sendEnd = std::chrono::steady_clock::now();
                const double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
                m_externalSendMsTotal += sendMs;
                m_externalSendMsMax = std::max(m_externalSendMsMax, sendMs);
                ++m_externalSentFrames;
            }

            const auto healthNow = std::chrono::steady_clock::now();
            if (m_externalNextStatsLog.time_since_epoch().count() == 0) m_externalNextStatsLog = healthNow + std::chrono::seconds(5);
            if (healthNow >= m_externalNextStatsLog) {
                const double encodedCount = std::max<uint64_t>(1, m_externalEncodedFrames);
                const double sentCount = std::max<uint64_t>(1, m_externalSentFrames);
                const double encodeAvgMs = m_externalEncodeMsTotal / encodedCount;
                const double sendAvgMs = m_externalSendMsTotal / sentCount;
                LogInfo("[vp9] encode health session=" + m_sessionId +
                    " engine=" + std::string(m_vp9VpxEncoder ? "libvpx" : "mediafoundation") +
                    " encode_avg_ms=" + std::to_string(encodeAvgMs) +
                    " encode_max_ms=" + std::to_string(m_externalEncodeMsMax) +
                    " send_avg_ms=" + std::to_string(sendAvgMs));
                observeCodecHealth(encodeAvgMs, m_externalEncodeMsMax, sendAvgMs);
                m_externalEncodedFrames = 0; m_externalSentFrames = 0;
                m_externalEncodeMsTotal = 0.0; m_externalEncodeMsMax = 0.0;
                m_externalSendMsTotal = 0.0; m_externalSendMsMax = 0.0;
                m_externalNextStatsLog = healthNow + std::chrono::seconds(5);
            }
            return;
        } catch (const std::exception& ex) {
            LogInfo("[vp9] exception session=" + m_sessionId + " error=" + ex.what());
            m_vp9Failed = true;
            m_vp9Encoder.reset();
            m_vp9VpxEncoder.reset();
            recoverForcedCodec(VideoCodec::VP9, "vp9", "VP9 exception");
            return;
        }
    }

    // Never fall through to another encoder while the negotiated payload still
    // belongs to a failed codec. Auto switches payload explicitly; forced modes
    // keep the session/input channels alive and drop video for diagnostics.
    if (m_videoCodec == VideoCodec::AV1 || m_videoCodec == VideoCodec::H265 ||
        m_videoCodec == VideoCodec::VP9) {
        return;
    }

    if (m_videoCodec == VideoCodec::H264 && !m_h264Failed) {
        bool h264Scaled = false;
        const int h264MaxW = readEnvInt("HI5_H264_MAX_WIDTH", 1920, 0, 7680);
        const int h264MaxH = readEnvInt("HI5_H264_MAX_HEIGHT", 1080, 0, 4320);
        const I420Frame& h264Frame = scaleI420ForWebRtc(frame, m_h264ScaleScratch, h264MaxW, h264MaxH, &h264Scaled);
        const int h264Fps = readEnvInt("HI5_H264_ENCODER_FPS", std::min(30, std::max(1, m_fps)), 1, 60);
        const int h264DesktopFloorKbps = h264Frame.width >= 1600 ? 8000 : (h264Frame.width >= 1200 ? 6000 : 4000);
        const int h264Kbps = readEnvInt("HI5_H264_ENCODER_KBPS", std::max(profile.bitrateKbps, h264DesktopFloorKbps), 1000, 24000);
        const bool sizeChangedH264 = h264Frame.width != m_externalEncoderWidth || h264Frame.height != m_externalEncoderHeight;
        const bool profileChangedH264 = h264Fps != m_externalConfiguredFps ||
            h264Kbps != m_externalConfiguredBitrateKbps;

        if (!m_h264Encoder || sizeChangedH264) {
            m_externalEncoderWidth = h264Frame.width;
            m_externalEncoderHeight = h264Frame.height;
            m_externalConfiguredFps = h264Fps;
            m_externalConfiguredBitrateKbps = h264Kbps;
            m_externalProfileName = std::string("hardware-h264-desktop-") + profile.name;

            auto enc = std::make_unique<H264MfEncoder>();
            std::string h264Err;
            const bool preferHardware = (m_codecMode != "h264_sw");
            if (!enc->init(h264Frame.width, h264Frame.height, h264Fps, h264Kbps, preferHardware, &h264Err)) {
                std::cerr << "[h264] init failed session=" << m_sessionId
                    << " error=" << h264Err
                    << " note=staying on negotiated track; set HI5_CODEC=vp8 to return to VP8\n";
                m_h264Failed = true;
                m_h264Encoder.reset();
                recoverForcedCodec(VideoCodec::H264, "h264", "H.264 encoder unavailable");
                return;
            }
            else {
                m_h264Encoder = std::move(enc);
                LogInfo(
                    "[h264] encoder created session=" + m_sessionId +
                    " name=" + m_h264Encoder->encoderName() +
                    " source=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                    " encoded_size=" + std::to_string(h264Frame.width) + "x" + std::to_string(h264Frame.height) +
                    " scaled=" + std::string(h264Scaled ? "1" : "0") +
                    " fps=" + std::to_string(h264Fps) +
                    " bitrate=" + std::to_string(h264Kbps) +
                    " hw_preferred=" + std::string(preferHardware ? "1" : "0")
                );
            }
            forceKeyframe = true;
        }

        if (m_h264Encoder && !m_h264Failed) {
            try {
                const auto encodeStart = std::chrono::steady_clock::now();
                H264EncodedFrame encoded{};
                std::string h264Err;
                const bool h264Keyframe = keyframe || sizeChangedH264 || profileChangedH264;
                if (!m_h264Encoder->encode(h264Frame, h264Keyframe, encoded, &h264Err)) {
                    LogInfo("[h264] encode failed session=" + m_sessionId +
                        " mode=" + m_codecMode + " error=" + h264Err);

                    const bool hardwarePreferredMode = m_codecMode != "h264_sw";
                    bool recoveredWithSoftware = false;
                    if (hardwarePreferredMode) {
                        LogInfo("[h264] hardware-preferred encode failed; retrying same frame with software H.264 session=" + m_sessionId);
                        auto swEnc = std::make_unique<H264MfEncoder>();
                        std::string swErr;
                        if (swEnc->init(h264Frame.width, h264Frame.height, h264Fps, h264Kbps, false, &swErr)) {
                            H264EncodedFrame swEncoded{};
                            if (swEnc->encode(h264Frame, true, swEncoded, &swErr)) {
                                const std::string swName = swEnc->encoderName();
                                m_h264Encoder = std::move(swEnc);
                                m_codecMode = "h264_sw";
                                m_externalProfileName = std::string("software-h264-fallback-") + profile.name;
                                m_h264Failed = false;
                                encoded = std::move(swEncoded);
                                recoveredWithSoftware = true;
                                LogInfo("[h264] software fallback active session=" + m_sessionId +
                                    " name=" + swName + " same_frame_retry=1");
                            } else {
                                LogInfo("[h264] software fallback encode failed session=" + m_sessionId + " error=" + swErr);
                            }
                        } else {
                            LogInfo("[h264] software fallback init failed session=" + m_sessionId + " error=" + swErr);
                        }
                    }

                    if (!recoveredWithSoftware) {
                        m_h264Failed = true;
                        m_h264Encoder.reset();
                        recoverForcedCodec(VideoCodec::H264, "h264", "H.264 encode failed");
                        return;
                    }
                }

                if (encoded.data.empty()) {
                    static std::atomic<uint64_t> h264EmptyCounter{ 0 };
                    const uint64_t emptyCount = ++h264EmptyCounter;

                    if (emptyCount <= 10 || emptyCount % 60 == 0) {
                        LogInfo(
                            "[h264] encode returned empty session=" + m_sessionId +
                            " count=" + std::to_string(emptyCount) +
                            " frame=" + std::to_string(h264Frame.width) +
                            "x" +
                            std::to_string(h264Frame.height) +
                            " keyframe=" +
                            std::string(h264Keyframe ? "true" : "false")
                        );
                    }
                }

                const auto encodeEnd = std::chrono::steady_clock::now();

                const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
                m_externalEncodeMsTotal += encodeMs;
                m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs);
                ++m_externalEncodedFrames;

                if (!encoded.data.empty()) {
                    encoded.timestamp90k = captureRtpTimestamp;
                    const auto sendStart = std::chrono::steady_clock::now();
                    sendRtpH264Frame(encoded);
                    const auto sendEnd = std::chrono::steady_clock::now();

                    const double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
                    m_externalSendMsTotal += sendMs;
                    m_externalSendMsMax = std::max(m_externalSendMsMax, sendMs);
                    ++m_externalSentFrames;
                }

                const auto now = std::chrono::steady_clock::now();
                if (m_externalNextStatsLog.time_since_epoch().count() == 0) {
                    m_externalNextStatsLog = now + std::chrono::seconds(5);
                }
                if (now >= m_externalNextStatsLog) {
                    const double encodedCount = std::max<uint64_t>(1, m_externalEncodedFrames);
                    const double sentCount = std::max<uint64_t>(1, m_externalSentFrames);
                    const double encodeAvgMs = m_externalEncodeMsTotal / encodedCount;
                    const double sendAvgMs = m_externalSendMsTotal / sentCount;
                    std::cout << "[h264] encode health session=" << m_sessionId
                        << " encoded=" << m_externalEncodedFrames
                        << " sent=" << m_externalSentFrames
                        << " encode_avg_ms=" << encodeAvgMs
                        << " encode_max_ms=" << m_externalEncodeMsMax
                        << " send_avg_ms=" << sendAvgMs
                        << " send_max_ms=" << m_externalSendMsMax
                        << " bitrate=" << m_externalConfiguredBitrateKbps
                        << " target_fps=" << m_externalConfiguredFps
                        << "\n";

                    observeCodecHealth(encodeAvgMs, m_externalEncodeMsMax, sendAvgMs);
                    m_externalEncodedFrames = 0;
                    m_externalSentFrames = 0;
                    m_externalEncodeMsTotal = 0.0;
                    m_externalEncodeMsMax = 0.0;
                    m_externalSendMsTotal = 0.0;
                    m_externalSendMsMax = 0.0;
                    m_externalNextStatsLog = now + std::chrono::seconds(5);
                }
                return;
            }
            catch (const std::exception& ex) {
                std::cerr << "[h264] exception session=" << m_sessionId
                    << " error=" << ex.what() << "\n";
                m_h264Failed = true;
                m_h264Encoder.reset();
                recoverForcedCodec(VideoCodec::H264, "h264", "H.264 exception");
                return;
            }
        }
    }

    // Important: if H.264 was negotiated in SDP, never send VP8 bytes on that
    // same payload type/track. If the Media Foundation encoder fails after the
    // offer is already negotiated, drop frames and keep the session alive long
    // enough for logs instead of poisoning the H.264 decoder with VP8 payloads.
    if (m_videoCodec == VideoCodec::H264) {
        return;
    }

    try {
        const auto encodeStart = std::chrono::steady_clock::now();
        EncodedFrame encoded = m_encoder->encode(frame, keyframe);
        const auto encodeEnd = std::chrono::steady_clock::now();

        const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
        m_externalEncodeMsTotal += encodeMs;
        m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs);
        ++m_externalEncodedFrames;

        if (!encoded.data.empty()) {
            encoded.rtpTimestamp = captureRtpTimestamp;
            const auto sendStart = std::chrono::steady_clock::now();
            sendRtpVp8Frame(encoded);
            const auto sendEnd = std::chrono::steady_clock::now();

            const double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
            m_externalSendMsTotal += sendMs;
            m_externalSendMsMax = std::max(m_externalSendMsMax, sendMs);
            ++m_externalSentFrames;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_externalNextStatsLog.time_since_epoch().count() == 0) {
            m_externalNextStatsLog = now + std::chrono::seconds(5);
        }
        if (now >= m_externalNextStatsLog) {
            const double encodedCount = std::max<uint64_t>(1, m_externalEncodedFrames);
            const double sentCount = std::max<uint64_t>(1, m_externalSentFrames);
            const double encodeAvgMs = m_externalEncodeMsTotal / encodedCount;
            const double sendAvgMs = m_externalSendMsTotal / sentCount;

            std::cout << "[external] encode health session=" << m_sessionId
                << " encoded=" << m_externalEncodedFrames
                << " sent=" << m_externalSentFrames
                << " encode_avg_ms=" << encodeAvgMs
                << " encode_max_ms=" << m_externalEncodeMsMax
                << " send_avg_ms=" << sendAvgMs
                << " send_max_ms=" << m_externalSendMsMax
                << " bitrate=" << m_externalConfiguredBitrateKbps
                << " target_fps=" << m_externalConfiguredFps
                << " keyframe_every=" << m_externalKeyframeEvery
                << "\n";

            try {
                json diag = {
                    {"type", "stream_encoder_diagnostics"},
                    {"session_id", m_sessionId},
                    {"codec", "VP8"},
                    {"profile", m_externalProfileName},
                    {"encoder_mode", m_externalProfileName},
                    {"stream_mode", m_externalHintMode.load()},
                    {"effective_stream_mode", m_externalEffectiveMode},
                    {"wake_profile", m_externalEffectiveMode == 3},
                    {"idle_delay_ms", readEnvInt("HI5_VP8_IDLE_DELAY_MS", 1000, 0, 30000)},
                    {"last_non_idle_mode", m_externalLastNonIdleMode.load()},
                    {"backstage", m_externalHintBackstage.load()},
                    {"secure_desktop", m_externalHintSecure.load()},
                    {"encoded_frames", m_externalEncodedFrames},
                    {"sent_frames", m_externalSentFrames},
                    {"encode_avg_ms", encodeAvgMs},
                    {"encode_max_ms", m_externalEncodeMsMax},
                    {"send_avg_ms", sendAvgMs},
                    {"send_max_ms", m_externalSendMsMax},
                    {"bitrate_kbps", m_externalConfiguredBitrateKbps},
                    {"target_fps", m_externalConfiguredFps},
                    {"cpu_used", m_externalConfiguredCpuUsed},
                    {"max_quantizer", m_externalConfiguredMaxQuantizer},
                    {"keyframe_every_frames", m_externalKeyframeEvery}
                };
                m_signalSend(diag.dump());
                sendControlMessage(diag);
            }
            catch (...) {
            }

            observeCodecHealth(encodeAvgMs, m_externalEncodeMsMax, sendAvgMs);
            m_externalEncodedFrames = 0;
            m_externalSentFrames = 0;
            m_externalEncodeMsTotal = 0.0;
            m_externalEncodeMsMax = 0.0;
            m_externalSendMsTotal = 0.0;
            m_externalSendMsMax = 0.0;
            m_externalNextStatsLog = now + std::chrono::seconds(5);
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "[external] encode failed session=" << m_sessionId
            << ": " << ex.what() << "\n";
        m_encoder.reset();
        m_externalEncoderWidth = 0;
        m_externalEncoderHeight = 0;
        m_forceKeyframe = true;
    }
}

void WebRtcSender::sendExternalEncodedVp8(const uint8_t* data,
    size_t size,
    uint32_t rtpTimestamp,
    bool /*keyframe*/) {
    if (m_mode != Mode::ExternalFeed || !data || size == 0) {
        return;
    }

    EncodedFrame frame;
    frame.data.assign(data, data + size);
    frame.rtpTimestamp = rtpTimestamp;

    const auto vp8Payloads = packetizeVp8(frame);
    sendVp8PayloadPackets(vp8Payloads, frame.rtpTimestamp);
}

void WebRtcSender::sendRtpVp8Frame(const EncodedFrame& frame) {
    const auto vp8Payloads = packetizeVp8(frame);
    sendVp8PayloadPackets(vp8Payloads, frame.rtpTimestamp);
}