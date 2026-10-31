#include "webrtc_sender.h"
#include "codec_auto_fps.h"
#include "frame_source.h"
#include "frame_scale_bilinear.h"
#include "h264_scaling_policy.h"
#include "util/log.h"

#include <rtc/rtc.hpp>
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


    // Exact p95 of bounded, actually measured encoder frame times.
    // A rare slow frame is recorded in max, but not mislabeled as p95.
    static double encodeP95Ms(const std::vector<double>& samples) {
        if (samples.empty()) return 0.0;
        auto ordered = samples;
        std::sort(ordered.begin(), ordered.end());
        const size_t index = (ordered.size() * 95 + 99) / 100 - 1;
        return ordered[std::min(index, ordered.size() - 1)];
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

    static I420Frame scaleI420ToWebRtcBounds(const I420Frame& src, int maxW, int maxH,
        bool* scaledOut = nullptr, bool filteredScaling = false) {
        if (scaledOut) *scaledOut = false;
        if (src.width <= 0 || src.height <= 0) return src;

        // Preserve aspect ratio with a per-codec ceiling; the 720p H.264
        // level-3.1 restriction must not be applied to VP9.
        if (maxW <= 0 || maxH <= 0) return src;
        if (src.width <= maxW && src.height <= maxH) return src;

        const double sx = static_cast<double>(maxW) / static_cast<double>(src.width);
        const double sy = static_cast<double>(maxH) / static_cast<double>(src.height);
        const double scale = std::min(sx, sy);
        int dstW = makeEvenAtLeast2(static_cast<int>(src.width * scale));
        int dstH = makeEvenAtLeast2(static_cast<int>(src.height * scale));
        dstW = std::min(dstW, makeEvenAtLeast2(maxW));
        dstH = std::min(dstH, makeEvenAtLeast2(maxH));

        I420Frame dst;
        dst.width = dstW;
        dst.height = dstH;

        if (filteredScaling) {
            hi5::quality::ScalePlaneBilinear(src.y, src.width, src.height, dst.y, dstW, dstH);
        } else {
            scalePlaneNearest(src.y, src.width, src.height, dst.y, dstW, dstH);
        }

        const int srcUw = (src.width + 1) / 2;
        const int srcUh = (src.height + 1) / 2;
        const int dstUw = (dstW + 1) / 2;
        const int dstUh = (dstH + 1) / 2;
        if (filteredScaling) {
            hi5::quality::ScalePlaneBilinear(src.u, srcUw, srcUh, dst.u, dstUw, dstUh);
            hi5::quality::ScalePlaneBilinear(src.v, srcUw, srcUh, dst.v, dstUw, dstUh);
        } else {
            scalePlaneNearest(src.u, srcUw, srcUh, dst.u, dstUw, dstUh);
            scalePlaneNearest(src.v, srcUw, srcUh, dst.v, dstUw, dstUh);
        }

        if (scaledOut) *scaledOut = true;
        return dst;
    }

    static bool h264SharpTextTrialEnabled() {
        // Opt-in only: generic H.264 WebRTC continues to advertise Level 3.1.
        // The higher resolution must be signalled with a matching SDP level.
        return readEnvInt("HI5_H264_SHARP_TEXT", 0, 0, 1) == 1;
    }

    static I420Frame scaleI420ForH264WebRtc(const I420Frame& src, bool* scaledOut = nullptr) {
        const bool sharp = h264SharpTextTrialEnabled();
        // Constrained Baseline L3.1 = 720p; opt-in L4.0 = up to 1080p.
        // Older deployments may have MACHINE-SCOPE HI5_H264_MAX_* = 1280/720
        // from historical L3.1 testing. Do not let that stale global setting
        // silently defeat an explicit service-scoped sharp-text trial.
        // Sharp mode has its OWN independent bounds, clamped to L4.0.
        const int maxW = sharp
            ? readEnvInt("HI5_H264_SHARP_MAX_WIDTH", 1920, 2, 1920)
            : readEnvInt("HI5_H264_MAX_WIDTH", 1280, 2, 1280);
        const int maxH = sharp
            ? readEnvInt("HI5_H264_SHARP_MAX_HEIGHT", 1080, 2, 1080)
            : readEnvInt("HI5_H264_MAX_HEIGHT", 720, 2, 720);
        // Opt-in filtered scaler only for sharp-text H.264. The generic
        // nearest-neighbour scaler remains unchanged for proven codecs.
        const bool filtered = hi5::quality::UseFilteredH264Scaling(sharp,
            readEnvInt("HI5_H264_SHARP_FILTER", 1, 0, 1) == 1,
            readEnvInt("HI5_H264_FILTERED_720P", 0, 0, 1) == 1);
        return scaleI420ToWebRtcBounds(src, maxW, maxH, scaledOut, filtered);
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
        const int maxKbps = std::max(300, originalMaxBitrateKbps);

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

        p.fps = std::max(1, std::min(maxFps, p.fps));
        p.bitrateKbps = std::max(250, std::min(maxKbps, p.bitrateKbps));
        p.minQuantizer = 4;
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
    std::string codecMode)
    : m_sessionId(std::move(sessionId)),
    m_iceServers(std::move(iceServers)),
    m_signalSend(std::move(sendFn)),
    m_width(width),
    m_height(height),
    m_fps(fps),
    m_bitrateKbps(bitrateKbps),
    m_ssrc(randomU32()),
    m_mode(mode),
    m_source(nullptr),
    m_encoder(nullptr) {
    m_codecMode = codecMode.empty() ? "auto" : std::move(codecMode);

    std::transform(m_codecMode.begin(), m_codecMode.end(), m_codecMode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });

    LogInfo("[codec] WebRtcSender codec mode=" + m_codecMode);

    if (m_codecMode == "h264_hw" || m_codecMode == "h264" || m_codecMode == "h264_sw") {
        m_videoCodec = VideoCodec::H264;
        m_payloadType = 102;
        LogInfo("[codec] WebRtcSender experimental H.264 requested mode=" + m_codecMode +
            " session=" + m_sessionId);
    }
    else if (m_codecMode == "vp9_hw" || m_codecMode == "vp9" || m_codecMode == "vp9_sw") {
        m_videoCodec = VideoCodec::VP9;
        m_payloadType = 98;
        LogInfo("[codec] WebRtcSender VP9 requested mode=" + m_codecMode +
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

void WebRtcSender::setInputEventHandler(InputEventFn fn) {
    m_inputEventFn = std::move(fn);
}

void WebRtcSender::setDirectMouseMoveHandler(DirectMouseMoveFn fn) {
    m_directMouseMoveFn = std::move(fn);
}

void WebRtcSender::setConnectionClosedHandler(ConnectionClosedFn fn) {
    m_connectionClosedFn = std::move(fn);
}

void WebRtcSender::setEncoderOverloadHandler(EncoderOverloadFn fn) {
    m_encoderOverloadFn = std::move(fn);
}

uint32_t WebRtcSender::randomU32() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return dist(gen);
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

    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }

    m_inputDc.reset();
    m_inputMoveDc.reset();
    m_inputControlDc.reset();
    m_track.reset();
    m_pc.reset();
}

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

    dc->onMessage([this, label](rtc::message_variant data) {
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
                    // Apply viewer resolution over WebRTC control; it was
                    // previously mistaken for a keyboard/mouse input event.
                    if (kind == "viewer_stream_profile" && msg.is_object()) {
                        const std::string pref = msg.value("preference", std::string("auto"));
                        int w = -1, h = -1; // no preference
                        if (pref == "native") { w = 0; h = 0; }
                        else if (pref == "1080p") { w = 1920; h = 1080; }
                        else if (pref == "720p") { w = 1280; h = 720; }
                        const int requestedFps = std::clamp(msg.value("target_fps", 0), 0, 60);
                        const int kbps = std::clamp(msg.value("target_bitrate_kbps", 0), 0, 20000);
                        const std::string fpsMode = msg.value("fps_mode", std::string("legacy"));
                        // Auto must be evidence-driven. At present no live
                        // capture+browser FPS qualifications are persisted, so
                        // keep the current 20 FPS ceiling even when a machine
                        // supports a faster encoder. The independent development
                        // override can request higher speeds within m_fps.
                        const auto automatic = hi5::universal_codec::selectAutomaticFps(
                            m_codecMode, {}, 0, std::max(1, m_fps), 120);
                        // Automatic stays evidence-based. Manual is an explicit
                        // viewer-requested experiment; it never asserts actual
                        // delivered FPS, and cannot exceed this sender's ceiling.
                        const bool manual = (fpsMode == "manual" || fpsMode == "development_override") &&
                            hi5::universal_codec::manualFpsAllowed(requestedFps);
                        const int fps = manual
                            ? std::min(requestedFps, m_fps)
                            : std::min(requestedFps > 0 ? requestedFps : 20,
                                       automatic.targetFps);
                        const int prevW = m_viewerMaxWidth.exchange(w);
                        const int prevH = m_viewerMaxHeight.exchange(h);
                        const int prevFps = m_viewerTargetFps.exchange(fps);
                        const int prevManual = m_viewerManualFps.exchange(
                            manual ? fps : 0);
                        // The authenticated Viewer control also updates actual
                        // capture-helper pacing. Otherwise a 60 FPS request
                        // changes only the encoder while GDI stays capped at 20.
                        if (m_mode == Mode::ExternalFeed && m_inputEventFn &&
                            (prevManual != (manual ? fps : 0))) {
                            m_inputEventFn(json{
                                {"kind", "viewer_frame_rate"},
                                {"target_fps", manual ? fps : 0}
                            });
                        }

                        m_viewerTargetBitrateKbps = kbps;
                        if (prevW != w || prevH != h) m_forceKeyframe = true;
                        // Authoritative reply on the already-authenticated
                        // control channel. The Viewer can promote Automatic only
                        // after the Agent exposes a truly qualified ceiling;
                        // older agents omit this message and remain at 20 FPS.
                        try {
                            json ack = {
                                {"type", "viewer_stream_profile_result"},
                                {"fps_mode", fpsMode},
                                {"requested_fps", requestedFps},
                                {"accepted_fps", std::min(fps > 0 ? fps : m_fps, m_fps)},
                                {"qualified_max_fps", std::min(automatic.targetFps, m_fps)},
                                {"agent_max_fps", m_fps},
                                {"fps_qualified", !manual && automatic.qualified},
                                {"manual_override", manual},
                                {"target_is_not_measured_fps", true}
                            };
                            sendControlMessageText(ack.dump());
                        } catch (...) {
                            // A diagnostic reply must never interrupt the media.
                        }
                        if (prevW != w || prevH != h || prevFps != fps || prevManual != (manual ? fps : 0)) {
                            LogInfo("[fps-policy] mode=" + fpsMode +
                                " requested=" + std::to_string(requestedFps) +
                                " accepted=" + std::to_string(fps) +
                                " agent_ceiling=" + std::to_string(m_fps) +
                                " automatic_qualified=" +
                                    (automatic.qualified ? "1" : "0") +
                                " reason=" + automatic.reason +
                                " session=" + m_sessionId);
                            LogInfo("[quality] viewer=" + pref +
                                " cap=" + std::to_string(w) + "x" + std::to_string(h) +
                                " target=" + std::to_string(fps) + "fps/" + std::to_string(kbps) +
                                "kbps session=" + m_sessionId);
                        }
                        return;
                    }
                    // The mobile and native Viewers ask for an IDR/keyframe
                    // when a desktop source changes or decoded video stalls.
                    // Handle it here, not as a keyboard/mouse command.
                    // The Windows service also receives the request to recover
                    // a capture helper when no raw frames are being produced.
                    if (kind == "request_keyframe") {
                        const uint64_t ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                        uint64_t prev = m_lastViewerKeyframeRequestMs.load();
                        if (prev != 0 && ms >= prev && ms - prev < 4000) return;
                        if (!m_lastViewerKeyframeRequestMs.compare_exchange_strong(prev, ms)) return;
                        m_forceKeyframe = true;
                        m_externalHintMode = 2;
                        LogInfo("[video-recovery] viewer keyframe accepted session=" + m_sessionId);
                        if (m_mode == Mode::ExternalFeed && m_inputEventFn) {
                            m_inputEventFn(msg);
                        }
                        return;
                    }
                    // RTT/jitter/packet loss are receiver-side network/decode
                    // measurements, not encoder timings. Sample at most once
                    // per 10 seconds, separate from capture and VP8/VP9 logs.
                    if (kind == "viewer_diagnostics") {
                        const uint64_t ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                        uint64_t prev = m_lastViewerNetworkLogMs.load();
                        if (prev != 0 && ms >= prev && ms - prev < 10000) return;
                        if (!m_lastViewerNetworkLogMs.compare_exchange_strong(prev, ms)) return;
                        const auto bounded = [&](const char* key, double limit) {
                            const double n = msg.value(key, 0.0);
                            return std::max(0.0, std::min(limit, n));
                        };
                        LogInfo("[network-health] session=" + m_sessionId +
                            " viewer_rtt_ms=" + std::to_string(bounded("rtt_ms", 60000.0)) +
                            " viewer_jitter_ms=" + std::to_string(bounded("jitter_ms", 60000.0)) +
                            " viewer_playout_buffer_ms=" + std::to_string(bounded("jitter_buffer_ms", 60000.0)) +
                            " viewer_decoded_fps=" + std::to_string(bounded("fps", 120.0)) +
                            " viewer_receive_kbps=" + std::to_string(bounded("bitrate_kbps", 100000.0)) +
                            " viewer_packets_lost=" + std::to_string(bounded("packets_lost", 1e9)));
                        return;
                    }

                    // Mouse movement is high frequency and best-effort. Keep the
                    // native cursor path responsive, but do not force the expensive
                    // full-motion VP8 profile for every single mouse packet. Clicks,
                    // wheel and keyboard still boost to motion immediately.
                    if (kind == "mouse_move") {
                        m_externalHintMode = std::max(m_externalHintMode.load(), 1);
                    }
                    else {
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

    m_externalHintMode = std::max(m_externalHintMode.load(), 1);

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

    // The Viewer cannot infer intentional desktop idleness from a zero
    // decoded-FPS sample alone: missing RTP and a stopped screen look alike.
    // Send an authenticated, content-free capture state over the established
    // WebRTC control channel; refresh every five seconds so stale messages
    // cannot hide a genuine media stall. Retry when the channel is not open.
    const int currentMode = m_externalHintMode.load();
    if (currentMode != m_lastCaptureStatusMode ||
        m_lastCaptureStatusSentAt.time_since_epoch().count() == 0 ||
        now - m_lastCaptureStatusSentAt >= std::chrono::seconds(5)) {
        const char* modeLabel = currentMode == 0 ? "idle" :
                                currentMode == 1 ? "active" : "motion";
        try {
            const json status = {
                {"type", "stream_capture_status"},
                {"mode", modeLabel},
                {"target_fps", requestedFps}
            };
            if (sendControlMessage(status)) {
                m_lastCaptureStatusMode = currentMode;
                m_lastCaptureStatusSentAt = now;
            }
        } catch (...) {
            // Telemetry must never block capture or remote input.
        }
    }
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

static std::string normalizeOutgoingH264SdpForDesktop(std::string sdp, int bitrateKbps, bool sharp1080p) {
    // L3.1 is the known-good 720p baseline. The explicit higher-clarity
    // experiment uses Constrained Baseline L4.0 (42e028). Never send 1080p
    // under a falsely advertised L3.1 profile-level-id.
    const std::string profile = sharp1080p ? "profile-level-id=42e028" : "profile-level-id=42e01f";
    for (const char* source : {
            "profile-level-id=42e034", "profile-level-id=420034",
            "profile-level-id=42C034", "profile-level-id=42c034",
            "profile-level-id=42e01f", "profile-level-id=42001f",
            "profile-level-id=42e028", "profile-level-id=420028" }) {
        replaceAllInPlace(sdp, source, profile);
    }

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

    if (m_videoCodec == VideoCodec::H264) {
        const bool sharp = h264SharpTextTrialEnabled();
        sdp = normalizeOutgoingH264SdpForDesktop(sdp, m_bitrateKbps, sharp);
        std::cout << "[codec] outgoing H.264 SDP normalized session=" << m_sessionId
            << " profile_level_id=" << (sharp ? "42e028" : "42e01f")
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
    if (m_videoCodec == VideoCodec::H264) {
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
    media.addSSRC(m_ssrc, "video-stream");
    media.setBitrate(m_bitrateKbps * 1000);

    m_track = m_pc->addTrack(media);

    createInputDataChannel();

    m_track->onOpen([this]() {
        LogInfo("[track] open session=" + m_sessionId);
        m_canSend = true;

        if (m_mode == Mode::DirectCapture) {
            startStreamingThread();
        }
        });

    m_track->onClosed([this]() {
        LogWarn("[track] closed session=" + m_sessionId);
        m_canSend = false;
        if (m_connectionClosedFn) {
            m_connectionClosedFn("track_closed");
        }
        });

    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        LogInfo("[pc] state=" + std::to_string(static_cast<int>(state)) +
            " session=" + m_sessionId);
        // Disconnected is transient in WebRTC. Leave the sender alive so ICE
        // can recover; terminal Failed/Closed states still trigger cleanup.
        if (state == rtc::PeerConnection::State::Disconnected) {
            LogWarn("[pc] ICE temporarily disconnected, awaiting recovery session=" + m_sessionId);
        }
        if (state == rtc::PeerConnection::State::Failed ||
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

        // Media m-lines can still have port 0 while ICE gathering is in
        // progress. Sending that premature offer rejects desktop video in
        // browsers, even when TURN/STUN candidates later become available.
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            signalLocalOfferIfReady();
        }
        });

    // Do not signal from onLocalDescription: it fires before ICE gathering
    // completes and can expose an unusable m=video 0 offer. The Complete
    // callback above emits one fully gathered offer instead.
    m_pc->onLocalDescription([this](rtc::Description) {
        LogInfo("[pc] local SDP prepared; waiting for ICE gathering completion session=" + m_sessionId);
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
            LogInfo("[signal] got answer session=" + m_sessionId +
                " sdp_len=" + std::to_string(sdp.size()) +
                " type=" + sdpType);

            m_pc->setRemoteDescription(rtc::Description(sdp, sdpType));

            LogInfo("[signal] set remote description ok session=" + m_sessionId);
        }
        catch (const std::exception& ex) {
            LogError("[signal] set remote description failed session=" + m_sessionId +
                " error=" + ex.what());
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

            LogInfo("[signal] add remote candidate ok session=" + m_sessionId +
                " mid=" + mid);
        }
        catch (const std::exception& ex) {
            LogWarn("[signal] add remote candidate failed session=" + m_sessionId +
                " mid=" + mid + " error=" + ex.what());
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
    const uint32_t wireTimestamp = m_videoRtpClock.stamp();

    static std::atomic<uint64_t> vp9FrameLogCounter{ 0 };
    const uint64_t c = ++vp9FrameLogCounter;

    if (c <= 5 || (c % 60) == 0) {
        LogInfo(
            "[vp9] rtp packetize frame=" + std::to_string(c) +
            " payload_count=" + std::to_string(vp9Payloads.size()) +
            " bytes=" + std::to_string(frame.data.size()) +
            " ts=" + std::to_string(wireTimestamp) +
            " encoder_ts=" + std::to_string(frame.timestamp90k) +
            " keyframe=" + std::string(frame.keyframe ? "1" : "0")
        );
    }

    sendVp9PayloadPackets(vp9Payloads, wireTimestamp);
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
    const uint32_t wireTimestamp = m_videoRtpClock.stamp();
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
            " ts=" + std::to_string(wireTimestamp) +
            " encoder_ts=" + std::to_string(frame.timestamp90k) +
            " keyframe=" + std::string(frame.keyframe ? "1" : "0")
        );
    }
    sendH264NalPayloads(nalUnits, wireTimestamp);
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

void WebRtcSender::sendExternalRawI420(const I420Frame& frame, bool forceKeyframe) {
    static std::atomic<uint64_t> rawEntryLogCounter{ 0 };
    const uint64_t rawEntryCount = ++rawEntryLogCounter;

    if (rawEntryCount <= 10 || rawEntryCount % 120 == 0) {
        LogInfo(
            "[external] raw entry session=" + m_sessionId +
            " codec=" + m_codecMode +
            " video_codec=" + std::string(
                m_videoCodec == VideoCodec::H264 ? "h264" :
                (m_videoCodec == VideoCodec::VP9 ? "vp9" : "vp8")) +
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

    if (previousEffectiveMode <= 0 && effectiveMode > 0 && readEnvInt("HI5_VP8_FORCE_KEYFRAME_ON_WAKE", 1, 0, 1) == 1) {
        forceKeyframe = true;
    }

    Vp8RuntimeProfile profile = buildVp8RuntimeProfile(
        effectiveMode,
        m_externalHintFps.load(),
        m_externalHintBackstage.load(),
        m_externalHintSecure.load(),
        std::max(1, m_fps),
        std::max(250, m_bitrateKbps));

    // The present Media Foundation VP9 encoder starts only once and does not
    // retune its parameters across subsequent idle/active/motion profiles.
    // The first wake frame previously froze a remote session at 8 FPS/2 Mbps.
    // Provide a stable, higher initial VP9 ceiling; existing dirty-frame/idle
    // capture controls still limit actual CPU and network use.
    if (m_videoCodec == VideoCodec::VP9 && !m_externalHintBackstage.load()) {
        profile.name = "vp9-desktop-clarity";
        profile.fps = std::max(1, std::min(m_fps,
            readEnvInt("HI5_VP9_DESKTOP_FPS", 20, 1, 60)));
        profile.bitrateKbps = std::max(250, std::min(m_bitrateKbps,
            readEnvInt("HI5_VP9_DESKTOP_KBPS", 6000, 500, 20000)));
    }

    // H.264 must NOT inherit the VP8 idle profile (2fps / 1200kbps).
    // H.264 Media Foundation currently sets bitrate at encoder creation and
    // does not retune it on every wake/motion transition. Its first frame is
    // normally an idle frame, which previously locked the entire session to
    // a 1200kbps encode even with "Prioritise clarity" / 8000kbps selected.
    // Keep a stable real-time encoder configuration; capture cadence still
    // saves resources whenever the desktop is truly idle.
    if (m_videoCodec == VideoCodec::H264 && !m_externalHintBackstage.load()) {
        profile.name = "h264-desktop-clarity";
        profile.fps = std::max(1, std::min(m_fps,
            readEnvInt("HI5_H264_DESKTOP_FPS", 20, 1, 60)));
        profile.bitrateKbps = std::max(250, std::min(m_bitrateKbps,
            readEnvInt("HI5_H264_DESKTOP_KBPS", 8000, 250, 20000)));
        const int requestedFps = m_viewerTargetFps.load();
        const int manualFps = m_viewerManualFps.load();
        const int requestedKbps = m_viewerTargetBitrateKbps.load();
        if (manualFps > 0) profile.fps = std::min(m_fps, manualFps);
        else if (requestedFps > 0) profile.fps = std::min(profile.fps, requestedFps);
        if (requestedKbps > 0) profile.bitrateKbps = std::min(profile.bitrateKbps, requestedKbps);
    }

    // The feedback-driven Viewer caps both VP8 and VP9. Previously these
    // limits were applied only to VP9, which left VP8 at full resolution and
    // bitrate on poor links even when Viewer requested 720p.
    if ((m_videoCodec == VideoCodec::VP9 || m_videoCodec == VideoCodec::VP8) &&
        !m_externalHintBackstage.load()) {
        const int reqFps = m_viewerTargetFps.load();
        const int reqKbps = m_viewerTargetBitrateKbps.load();
        if (reqFps > 0) profile.fps = std::min(profile.fps, reqFps);
        if (reqKbps > 0) profile.bitrateKbps = std::min(profile.bitrateKbps, reqKbps);
    }

    // Scale only when Viewer sent an explicit bounded resolution. Avoid
    // copies on native/no-preference paths and preserve all monitor controls.
    const int vp8RequestedW = m_viewerMaxWidth.load();
    const int vp8RequestedH = m_viewerMaxHeight.load();
    I420Frame vp8ScaledFrame;
    const I420Frame* vp8Input = &frame;
    if (m_videoCodec == VideoCodec::VP8 && !m_externalHintBackstage.load() &&
        vp8RequestedW > 0 && vp8RequestedH > 0 &&
        (frame.width > vp8RequestedW || frame.height > vp8RequestedH)) {
        vp8ScaledFrame = scaleI420ToWebRtcBounds(frame, vp8RequestedW, vp8RequestedH);
        vp8Input = &vp8ScaledFrame;
    }
    const I420Frame& vp8Frame = *vp8Input;
    const bool sizeChanged = vp8Frame.width != m_externalEncoderWidth ||
        vp8Frame.height != m_externalEncoderHeight;
    const bool profileChanged = profile.fps != m_externalConfiguredFps ||
        profile.bitrateKbps != m_externalConfiguredBitrateKbps ||
        profile.cpuUsed != m_externalConfiguredCpuUsed ||
        profile.maxQuantizer != m_externalConfiguredMaxQuantizer;

    if (((m_videoCodec == VideoCodec::VP8) || m_h264Failed) && (!m_encoder || sizeChanged)) {
        m_externalEncoderWidth = vp8Frame.width;
        m_externalEncoderHeight = vp8Frame.height;
        m_externalConfiguredFps = profile.fps;
        m_externalConfiguredBitrateKbps = profile.bitrateKbps;
        m_externalConfiguredCpuUsed = profile.cpuUsed;
        m_externalConfiguredMaxQuantizer = profile.maxQuantizer;
        m_externalProfileName = profile.name;
        m_encoder = std::make_unique<Vp8Encoder>(
            vp8Frame.width,
            vp8Frame.height,
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
            << " -> " << vp8Frame.width << "x" << vp8Frame.height
            << " profile=" << m_externalProfileName
            << " fps=" << profile.fps
            << " bitrate=" << profile.bitrateKbps
            << " cpuused=" << profile.cpuUsed
            << " qmax=" << profile.maxQuantizer
            << "\n";
    }
    else if (((m_videoCodec == VideoCodec::VP8) || m_h264Failed) && profileChanged) {
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

    ++m_externalFrameCounter;

    // Low-CPU policy: keyframes are event based. Viewer connect, monitor switch,
    // UAC/secure desktop and encoder resize still force a keyframe. Periodic
    // recovery keyframes are much less frequent while idle and profile-aware.
    m_externalKeyframeEvery = std::max(profile.fps, std::max(1, profile.fps) * profile.keyframeSeconds);
    const bool periodicKeyframe = (m_externalFrameCounter % m_externalKeyframeEvery) == 0;
    // One recovery IDR after 20 seconds when end-to-end codec learning is
    // explicitly enabled. The current frame-only cadence can take minutes
    // to reach the next IDR on a quiet 2-FPS desktop, preventing the browser
    // from qualifying an otherwise healthy H.264 session. No repeated IDRs.
    const bool qualificationKeyframe = m_h264QualificationKeyframe.requestIfDue(
        m_videoCodec == VideoCodec::H264 &&
            readEnvInt("HI5_UNIVERSAL_CODEC_LEARN", 0, 0, 1) == 1,
        nowForProfile);
    if (qualificationKeyframe) {
        LogInfo("[codec-universal] one-shot H264 recovery IDR "
                "reason=qualify_subsequent_browser_decoded_keyframe");
    }
    // Consume any pending keyframe request even if the qualification IDR
    // satisfies it; otherwise the next idle frame emits a duplicate IDR.
    const bool queuedKeyframe = m_forceKeyframe.exchange(false);
    const bool keyframe = forceKeyframe || periodicKeyframe ||
        qualificationKeyframe || queuedKeyframe;

    if (m_videoCodec == VideoCodec::VP9 && !m_vp9Failed) {
        bool vp9Scaled = false;
        // Default remains 1080p. An explicit Native request preserves the
        // real desktop pixels up to a bounded 2560x1600. Native falls back
        // to 1080p on encoder failure or sustained overload.
        const int reqW = m_viewerMaxWidth.load(), reqH = m_viewerMaxHeight.load();
        const bool nativeRequested = reqW == 0 && reqH == 0 && !m_vp9NativeFailed;
        int vp9MaxW = readEnvInt("HI5_VP9_MAX_WIDTH", 1920, 0, 7680);
        int vp9MaxH = readEnvInt("HI5_VP9_MAX_HEIGHT", 1080, 0, 4320);
        if (nativeRequested) {
            vp9MaxW = readEnvInt("HI5_VP9_NATIVE_MAX_WIDTH", 2560, 2, 7680);
            vp9MaxH = readEnvInt("HI5_VP9_NATIVE_MAX_HEIGHT", 1600, 2, 4320);
        } else if (reqW > 0 && reqH > 0) {
            vp9MaxW = std::min(vp9MaxW, reqW);
            vp9MaxH = std::min(vp9MaxH, reqH);
        }
        // Native resolution can reuse the already-captured I420 planes as-is.
        // Avoid a redundant multi-megabyte copy on every 2560x1600 frame.
        I420Frame vp9Resized;
        const I420Frame* vp9Input = &frame;
        if (vp9MaxW > 0 && vp9MaxH > 0 &&
            (frame.width > vp9MaxW || frame.height > vp9MaxH)) {
            vp9Resized = scaleI420ToWebRtcBounds(frame, vp9MaxW, vp9MaxH, &vp9Scaled);
            vp9Input = &vp9Resized;
        }
        const I420Frame& vp9Frame = *vp9Input;
        const bool sizeChangedVp9 = vp9Frame.width != m_externalEncoderWidth || vp9Frame.height != m_externalEncoderHeight;
        const bool rateChangedVp9 = profile.fps != m_externalConfiguredFps ||
            profile.bitrateKbps != m_externalConfiguredBitrateKbps;

        // Media Foundation VP9 can take hundreds of milliseconds to reinit.
        // FPS and bitrate changes must NOT reconstruct the encoder. FPS is a
        // capture/cadence target; bitrate uses optional runtime ICodecAPI.
        // Only a real resolution change needs a new MFT instance.
        if (!m_vp9Encoder || sizeChangedVp9) {
            m_vp9RejectedBitrateKbps = 0;
            m_externalEncoderWidth = vp9Frame.width;
            m_externalEncoderHeight = vp9Frame.height;
            m_externalConfiguredFps = profile.fps;
            m_externalConfiguredBitrateKbps = profile.bitrateKbps;
            m_externalProfileName = std::string("mediafoundation-vp9-") + profile.name;

            auto enc = std::make_unique<Vp9MfEncoder>();
            std::string vp9Err;
            const bool preferHardware = (m_codecMode == "vp9_hw" || m_codecMode == "vp9" || m_codecMode == "auto");

            if (!enc->init(vp9Frame.width, vp9Frame.height, profile.fps, profile.bitrateKbps, preferHardware, &vp9Err)) {
                LogInfo("[vp9] init failed session=" + m_sessionId +
                    " error=" + vp9Err);
                if (nativeRequested) {
                    m_vp9NativeFailed = true;
                    m_vp9Encoder.reset();
                    m_forceKeyframe = true;
                    LogWarn("[quality] native VP9 init failed, reverting to 1080p session=" + m_sessionId);
                    return;
                }
                m_vp9Failed = true;
                if (m_encoderOverloadFn) m_encoderOverloadFn();
                m_vp9Encoder.reset();
            }
            else {
                m_vp9Encoder = std::move(enc);
                LogInfo(
                    "[vp9] encoder created session=" + m_sessionId +
                    " name=" + m_vp9Encoder->encoderName() +
                    " source=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                    " encoded_size=" + std::to_string(vp9Frame.width) + "x" + std::to_string(vp9Frame.height) +
                    " scaled=" + std::string(vp9Scaled ? "1" : "0") +
                    " fps=" + std::to_string(profile.fps) +
                    " bitrate=" + std::to_string(profile.bitrateKbps) +
                    " hw_preferred=" + std::string(preferHardware ? "1" : "0") +
                    " native_requested=" + std::string(nativeRequested ? "1" : "0")
                );
            }

            forceKeyframe = true;
        }
        else if (rateChangedVp9 && m_vp9Encoder && !m_vp9Failed) {
            // Do not reinitialise Media Foundation simply to adjust fps.
            // The capture helper already uses a paced, adaptive target FPS.
            m_externalConfiguredFps = profile.fps;
            if (profile.bitrateKbps != m_externalConfiguredBitrateKbps &&
                profile.bitrateKbps != m_vp9RejectedBitrateKbps) {
                std::string rateError;
                if (m_vp9Encoder->setTargetBitrateKbps(profile.bitrateKbps, &rateError)) {
                    m_externalConfiguredBitrateKbps = profile.bitrateKbps;
                    m_vp9RejectedBitrateKbps = 0;
                    LogInfo("[vp9] bitrate retuned without encoder restart session=" + m_sessionId +
                        " kbps=" + std::to_string(profile.bitrateKbps));
                }
                else {
                    // Some MFTs do not support dynamic bitrate. Keep the
                    // picture alive; avoid retrying on every captured frame.
                    m_vp9RejectedBitrateKbps = profile.bitrateKbps;
                    LogInfo("[vp9] retaining stable encoder rate session=" + m_sessionId +
                        " requested_kbps=" + std::to_string(profile.bitrateKbps) +
                        " actual_kbps=" + std::to_string(m_externalConfiguredBitrateKbps) +
                        " reason=" + rateError);
                }
            }
        }

        if (m_vp9Encoder && !m_vp9Failed) {
            try {
                const auto encodeStart = std::chrono::steady_clock::now();

                Vp9EncodedFrame encoded{};
                std::string vp9Err;
                const bool vp9Keyframe = keyframe || sizeChangedVp9;

                if (!m_vp9Encoder->encode(vp9Frame, vp9Keyframe, encoded, &vp9Err)) {
                    LogInfo("[vp9] encode failed session=" + m_sessionId +
                        " mode=" + m_codecMode +
                        " error=" + vp9Err);

                    const bool hardwareInputUnsupported =
                        m_codecMode == "vp9_hw" &&
                        vp9Err.find("0xc00d36b5") != std::string::npos;

                    if (hardwareInputUnsupported) {
                        LogInfo("[vp9] hardware ProcessInput failed; retrying with software VP9 session=" + m_sessionId);

                        auto swEnc = std::make_unique<Vp9MfEncoder>();
                        std::string swErr;

                        if (swEnc->init(vp9Frame.width, vp9Frame.height, profile.fps, profile.bitrateKbps, false, &swErr)) {
                            m_vp9Encoder = std::move(swEnc);
                            m_codecMode = "vp9_sw";
                            m_externalEncoderWidth = vp9Frame.width;
                            m_externalEncoderHeight = vp9Frame.height;
                            m_externalConfiguredFps = profile.fps;
                            m_externalConfiguredBitrateKbps = profile.bitrateKbps;
                            m_externalProfileName = std::string("software-vp9-fallback-") + profile.name;
                            m_vp9Failed = false;
                            m_forceKeyframe = true;

                            LogInfo("[vp9] software fallback encoder created session=" + m_sessionId +
                                " name=" + m_vp9Encoder->encoderName());

                            return;
                        }

                        LogInfo("[vp9] software fallback init failed session=" + m_sessionId +
                            " error=" + swErr);
                    }

                    if (nativeRequested) {
                        m_vp9NativeFailed = true;
                        m_vp9Encoder.reset();
                        m_forceKeyframe = true;
                        LogWarn("[quality] native VP9 encode failed, reverting to 1080p session=" + m_sessionId);
                        return;
                    }
                    m_vp9Failed = true;
                    if (m_encoderOverloadFn) m_encoderOverloadFn();
                    m_vp9Encoder.reset();
                    return;
                }

                const auto encodeEnd = std::chrono::steady_clock::now();

                const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
                if (nativeRequested) {
                    m_vp9SlowNativeFrames = encodeMs > 180.0
                        ? m_vp9SlowNativeFrames + 1
                        : std::max(0, m_vp9SlowNativeFrames - 2);
                    if (m_vp9SlowNativeFrames >= 12) {
                        m_vp9NativeFailed = true;
                        m_vp9Encoder.reset();
                        m_forceKeyframe = true;
                        LogWarn("[quality] native VP9 too slow, falling back to 1080p encode_ms=" +
                            std::to_string(encodeMs) + " session=" + m_sessionId);
                        return;
                    }
                }
                m_externalEncodeMsTotal += encodeMs;
        // Rolling 512-frame history survives five-second diagnostics
                    // windows. At 2 idle fps, ten samples would make the
                    // nearest-rank p95 equal to the single worst spike.
                    if (m_externalEncodeMsSamples.size() >= 512)
                        m_externalEncodeMsSamples.erase(m_externalEncodeMsSamples.begin());
                    m_externalEncodeMsSamples.push_back(encodeMs);
                m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs);
                ++m_externalEncodedFrames;

                if (!encoded.data.empty()) {
                    const auto sendStart = std::chrono::steady_clock::now();
                    sendRtpVp9Frame(encoded);
                    const auto sendEnd = std::chrono::steady_clock::now();

                    const double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
                    m_externalSendMsTotal += sendMs;
                    m_externalSendMsMax = std::max(m_externalSendMsMax, sendMs);
                    ++m_externalSentFrames;
                }
                else {
                    static std::atomic<uint64_t> vp9EmptyCounter{ 0 };
                    const uint64_t emptyCount = ++vp9EmptyCounter;
                    if (emptyCount <= 10 || emptyCount % 60 == 0) {
                        LogInfo("[vp9] encode returned empty session=" + m_sessionId +
                            " count=" + std::to_string(emptyCount) +
                            " frame=" + std::to_string(vp9Frame.width) + "x" + std::to_string(vp9Frame.height) +
                            " keyframe=" + std::string(vp9Keyframe ? "true" : "false"));
                    }
                }

                const auto healthNow = std::chrono::steady_clock::now();
                if (m_externalNextStatsLog.time_since_epoch().count() == 0) {
                    m_externalNextStatsLog = healthNow + std::chrono::seconds(5);
                }
                if (healthNow >= m_externalNextStatsLog) {
                    const double encodedCount = static_cast<double>(std::max<uint64_t>(1, m_externalEncodedFrames));
                    const double sentCount = static_cast<double>(std::max<uint64_t>(1, m_externalSentFrames));
                    // Two consecutive poor 5-second windows are stronger
                    // evidence than a short startup codec benchmark. Inform
                    // the owner to choose VP8 on a future Auto session only.
                    // Never switch RTP codec without a new SDP negotiation.
                    if (m_vp9Health.observeWindow(
                            static_cast<unsigned int>(std::min<uint64_t>(m_externalEncodedFrames, 100000)),
                            m_externalEncodeMsTotal, m_externalEncodeMsMax,
                            m_externalConfiguredFps)) {
                        LogWarn("[codec-auto] sustained VP9 overload session=" + m_sessionId +
                            " next_auto_session=vp8");
                        if (m_encoderOverloadFn) m_encoderOverloadFn();
                    }
                    LogInfo("[encoder-health] codec=VP9 session=" + m_sessionId +
                        " encoded=" + std::to_string(m_externalEncodedFrames) +
                        " sent=" + std::to_string(m_externalSentFrames) +
                        " encode_avg_ms=" + std::to_string(m_externalEncodeMsTotal / encodedCount) +
                        " encode_max_ms=" + std::to_string(m_externalEncodeMsMax) +
                        " rtp_enqueue_avg_ms=" + std::to_string(m_externalSendMsTotal / sentCount) +
                        " rtp_enqueue_max_ms=" + std::to_string(m_externalSendMsMax) +
                        " target_fps=" + std::to_string(m_externalConfiguredFps) +
                        " bitrate_kbps=" + std::to_string(m_externalConfiguredBitrateKbps));
                    // VPS-side diagnostics for real-time comparisons. Keep
                    // the exact same five-second interval as local logging.
                    // Never include screen pixels, keyboard/mouse contents or identifiers.
                    try {
                        json diag = {
                            {"type", "stream_encoder_diagnostics"},
                            {"session_id", m_sessionId},
                            {"codec", "VP9"},
                            {"encoder_mode", m_codecMode},
                            {"encoded_frames", m_externalEncodedFrames},
                            {"sent_frames", m_externalSentFrames},
                            {"encode_avg_ms", m_externalEncodeMsTotal / encodedCount},
                            {"encode_max_ms", m_externalEncodeMsMax},
                            {"encode_p95_ms", encodeP95Ms(m_externalEncodeMsSamples)},
                            {"send_avg_ms", m_externalSendMsTotal / sentCount},
                            {"send_max_ms", m_externalSendMsMax},
                            {"target_fps", m_externalConfiguredFps},
                            {"bitrate_kbps", m_externalConfiguredBitrateKbps},
                            {"width", m_externalEncoderWidth},
                            {"height", m_externalEncoderHeight}
                        };
                        m_signalSend(diag.dump());
                    } catch (...) {
                        // Metrics are best effort, never interrupt a stream.
                    }

                    m_externalEncodedFrames = 0;
                    m_externalSentFrames = 0;
                    m_externalEncodeMsTotal = 0.0;
                    m_externalEncodeMsMax = 0.0;
                    // Retain recent encoder latency history across stats windows.
                    m_externalSendMsTotal = 0.0;
                    m_externalSendMsMax = 0.0;
                    m_externalNextStatsLog = healthNow + std::chrono::seconds(5);
                }
                return;
            }
            catch (const std::exception& ex) {
                LogInfo("[vp9] exception session=" + m_sessionId +
                    " error=" + std::string(ex.what()));
                m_vp9Failed = true;
                if (m_encoderOverloadFn) m_encoderOverloadFn();
                m_vp9Encoder.reset();
                return;
            }
        }
    }

    // Important: if VP9 was negotiated in SDP, never send VP8/H.264 bytes on
    // that same payload type/track. If VP9 fails after the offer is negotiated,
    // drop frames and keep the session alive long enough for logs.
    if (m_videoCodec == VideoCodec::VP9) {
        return;
    }

    if (m_videoCodec == VideoCodec::H264 && !m_h264Failed) {
        bool h264Scaled = false;
        const I420Frame h264Frame = scaleI420ForH264WebRtc(frame, &h264Scaled);
        const bool sizeChangedH264 = h264Frame.width != m_externalEncoderWidth || h264Frame.height != m_externalEncoderHeight;
        const bool profileChangedH264 = profile.fps != m_externalConfiguredFps ||
            profile.bitrateKbps != m_externalConfiguredBitrateKbps;

        // Media Foundation's nominal frame rate is set at encoder
        // creation. Reinitialise on an explicit rate change so asking for
        // 30/45/60 does not keep the old 20 FPS encoder configuration.
        // This is one swap per user change, not a new encoder per frame.
        if (!m_h264Encoder || sizeChangedH264 ||
            profile.fps != m_externalConfiguredFps) {
            m_externalEncoderWidth = h264Frame.width;
            m_externalEncoderHeight = h264Frame.height;
            m_externalConfiguredFps = profile.fps;
            m_externalConfiguredBitrateKbps = profile.bitrateKbps;
            m_externalProfileName = std::string("hardware-h264-experimental-") + profile.name;

            auto enc = std::make_unique<H264MfEncoder>();
            std::string h264Err;
            const bool preferHardware = (m_codecMode == "h264_hw" || m_codecMode == "auto");
            if (!enc->init(h264Frame.width, h264Frame.height, profile.fps, profile.bitrateKbps, preferHardware, &h264Err)) {
                std::cerr << "[h264] init failed session=" << m_sessionId
                    << " error=" << h264Err
                    << " note=staying on negotiated track; set HI5_CODEC=vp8 to return to VP8\n";
                m_h264Failed = true;
                m_h264Encoder.reset();
            }
            else {
                m_h264Encoder = std::move(enc);
                LogInfo(
                    "[h264] encoder created session=" + m_sessionId +
                    " sharp_text_trial=" + std::string(h264SharpTextTrialEnabled() ? "1" : "0") +
                    " filtered_scaler=" + std::string(h264SharpTextTrialEnabled() &&
                        readEnvInt("HI5_H264_SHARP_FILTER", 1, 0, 1) == 1 ? "bilinear" : "nearest") +
                    " name=" + m_h264Encoder->encoderName() +
                    " source=" + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                    " encoded_size=" + std::to_string(h264Frame.width) + "x" + std::to_string(h264Frame.height) +
                    " scaled=" + std::string(h264Scaled ? "1" : "0") +
                    " fps=" + std::to_string(profile.fps) +
                    " bitrate=" + std::to_string(profile.bitrateKbps) +
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
                    LogInfo(
                        "[h264] encode failed session=" + m_sessionId +
                        " mode=" + m_codecMode +
                        " error=" + h264Err
                    );

                    const bool hardwareInputUnsupported =
                        m_codecMode == "h264_hw" &&
                        h264Err.find("0xc00d36b5") != std::string::npos;

                    if (hardwareInputUnsupported) {
                        LogInfo("[h264] hardware ProcessInput failed; retrying with software H.264 session=" + m_sessionId);

                        auto swEnc = std::make_unique<H264MfEncoder>();
                        std::string swErr;

                        if (swEnc->init(h264Frame.width, h264Frame.height, profile.fps, profile.bitrateKbps, false, &swErr)) {
                            m_h264Encoder = std::move(swEnc);
                            m_codecMode = "h264_sw";
                            m_externalEncoderWidth = h264Frame.width;
                            m_externalEncoderHeight = h264Frame.height;
                            m_externalConfiguredFps = profile.fps;
                            m_externalConfiguredBitrateKbps = profile.bitrateKbps;
                            m_externalProfileName = std::string("software-h264-fallback-") + profile.name;
                            m_h264Failed = false;
                            m_forceKeyframe = true;

                            LogInfo("[h264] software fallback encoder created session=" + m_sessionId +
                                " name=" + m_h264Encoder->encoderName());

                            return;
                        }

                        LogInfo("[h264] software fallback init failed session=" + m_sessionId +
                            " error=" + swErr);
                    }

                    m_h264Failed = true;
                    m_h264Encoder.reset();
                    return;
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
        // Rolling 512-frame history survives five-second diagnostics
                    // windows. At 2 idle fps, ten samples would make the
                    // nearest-rank p95 equal to the single worst spike.
                    if (m_externalEncodeMsSamples.size() >= 512)
                        m_externalEncodeMsSamples.erase(m_externalEncodeMsSamples.begin());
                    m_externalEncodeMsSamples.push_back(encodeMs);
                m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs);
                ++m_externalEncodedFrames;

                if (!encoded.data.empty()) {
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
                    LogInfo("[encoder-health] codec=H264 session=" + m_sessionId +
                        " encoded=" + std::to_string(m_externalEncodedFrames) +
                        " sent=" + std::to_string(m_externalSentFrames) +
                        " encode_avg_ms=" + std::to_string(m_externalEncodeMsTotal / encodedCount) +
                        " encode_max_ms=" + std::to_string(m_externalEncodeMsMax) +
                        " rtp_enqueue_avg_ms=" + std::to_string(m_externalSendMsTotal / sentCount) +
                        " rtp_enqueue_max_ms=" + std::to_string(m_externalSendMsMax) +
                        " target_fps=" + std::to_string(m_externalConfiguredFps) +
                        " bitrate_kbps=" + std::to_string(m_externalConfiguredBitrateKbps));

                    // VPS-side diagnostics for real-time comparisons. Keep
                    // the exact same five-second interval as local logging.
                    // Never include screen pixels, keyboard/mouse contents or identifiers.
                    try {
                        json diag = {
                            {"type", "stream_encoder_diagnostics"},
                            {"session_id", m_sessionId},
                            {"codec", "H264"},
                            {"encoder_mode", m_codecMode},
                            {"encoded_frames", m_externalEncodedFrames},
                            {"sent_frames", m_externalSentFrames},
                            {"encode_avg_ms", m_externalEncodeMsTotal / encodedCount},
                            {"encode_max_ms", m_externalEncodeMsMax},
                            {"encode_p95_ms", encodeP95Ms(m_externalEncodeMsSamples)},
                            {"send_avg_ms", m_externalSendMsTotal / sentCount},
                            {"send_max_ms", m_externalSendMsMax},
                            {"target_fps", m_externalConfiguredFps},
                            {"bitrate_kbps", m_externalConfiguredBitrateKbps},
                            {"width", m_externalEncoderWidth},
                            {"height", m_externalEncoderHeight}
                        };
                        m_signalSend(diag.dump());
                    } catch (...) {
                        // Metrics are best effort, never interrupt a stream.
                    }

                    m_externalEncodedFrames = 0;
                    m_externalSentFrames = 0;
                    m_externalEncodeMsTotal = 0.0;
                    m_externalEncodeMsMax = 0.0;
                    // Retain recent encoder latency history across stats windows.
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
        EncodedFrame encoded = m_encoder->encode(vp8Frame, keyframe);
        const auto encodeEnd = std::chrono::steady_clock::now();

        const double encodeMs = std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count();
        m_externalEncodeMsTotal += encodeMs;
        // Rolling 512-frame history survives five-second diagnostics
        // windows. At 2 idle fps, ten samples would make the
        // nearest-rank p95 equal to the single worst spike.
        if (m_externalEncodeMsSamples.size() >= 512)
            m_externalEncodeMsSamples.erase(m_externalEncodeMsSamples.begin());
        m_externalEncodeMsSamples.push_back(encodeMs);
        m_externalEncodeMsMax = std::max(m_externalEncodeMsMax, encodeMs);
        ++m_externalEncodedFrames;

        if (!encoded.data.empty()) {
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

            // Windows services have no reliable stdout console. Use the
            // Agent's structured logger so these measured encoder timings are
            // actually retained alongside capture and Viewer RTT diagnostics.
            LogInfo("[encoder-health] codec=VP8 session=" + m_sessionId +
                " encoded=" + std::to_string(m_externalEncodedFrames) +
                " sent=" + std::to_string(m_externalSentFrames) +
                " encode_avg_ms=" + std::to_string(encodeAvgMs) +
                " encode_max_ms=" + std::to_string(m_externalEncodeMsMax) +
                " rtp_enqueue_avg_ms=" + std::to_string(sendAvgMs) +
                " rtp_enqueue_max_ms=" + std::to_string(m_externalSendMsMax) +
                " target_fps=" + std::to_string(m_externalConfiguredFps) +
                " bitrate_kbps=" + std::to_string(m_externalConfiguredBitrateKbps));

            try {
                json diag = {
                    {"type", "stream_encoder_diagnostics"},
                    {"session_id", m_sessionId},
                    {"codec", "VP8"},
                    {"codec_runtime_mode", "vp8"},
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
                    {"encode_p95_ms", encodeP95Ms(m_externalEncodeMsSamples)},
                    {"send_avg_ms", sendAvgMs},
                    {"send_max_ms", m_externalSendMsMax},
                    {"bitrate_kbps", m_externalConfiguredBitrateKbps},
                    {"target_fps", m_externalConfiguredFps},
                    {"cpu_used", m_externalConfiguredCpuUsed},
                    {"max_quantizer", m_externalConfiguredMaxQuantizer},
                    {"keyframe_every_frames", m_externalKeyframeEvery},
                    // The actual VP8 encoder dimensions are needed to pair
                    // Agent frame evidence with the Viewer decode report.
                    {"width", m_externalEncoderWidth},
                    {"height", m_externalEncoderHeight}
                };
                m_signalSend(diag.dump());
                sendControlMessage(diag);
            }
            catch (...) {
            }

            m_externalEncodedFrames = 0;
            m_externalSentFrames = 0;
            m_externalEncodeMsTotal = 0.0;
            m_externalEncodeMsMax = 0.0;
            // Retain recent encoder latency history across stats windows.
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
    sendVp8PayloadPackets(vp8Payloads, m_videoRtpClock.stamp());
}

void WebRtcSender::sendRtpVp8Frame(const EncodedFrame& frame) {
    const auto vp8Payloads = packetizeVp8(frame);
    sendVp8PayloadPackets(vp8Payloads, m_videoRtpClock.stamp());
}