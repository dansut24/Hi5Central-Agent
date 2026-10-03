#include "remote_webrtc.h"

#include "remote_platform.h"

#if defined(__APPLE__)
#include "remote_h264_vt_encoder.h"
#include "remote_software_video_encoder.h"

#include <rtc/av1rtppacketizer.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#else
#include "remote_vp8_encoder.h"
#endif

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace hi5 {
namespace {

using json = nlohmann::json;

std::uint32_t randomU32() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dist;
    return dist(gen);
}

std::uint16_t randomU16() {
    return static_cast<std::uint16_t>(randomU32() & 0xffffU);
}

std::vector<std::string> iceServersFromMessage(const json& message) {
    std::vector<std::string> out;
    const auto it = message.find("iceServers");
    if (it == message.end() || !it->is_array()) return out;
    for (const auto& value : *it) {
        if (value.is_string()) {
            const auto server = value.get<std::string>();
            if (!server.empty()) out.push_back(server);
        } else if (value.is_object()) {
            auto urls = value.find("urls");
            if (urls != value.end() && urls->is_string()) {
                const auto server = urls->get<std::string>();
                if (!server.empty()) out.push_back(server);
            }
        }
    }
    return out;
}

std::string normaliseCandidate(std::string candidate) {
    while (!candidate.empty() &&
        (candidate.back() == '\r' || candidate.back() == '\n' ||
         candidate.back() == ' ' || candidate.back() == '\t')) {
        candidate.pop_back();
    }
    while (!candidate.empty() &&
        (candidate.front() == ' ' || candidate.front() == '\t')) {
        candidate.erase(candidate.begin());
    }
    if (candidate.rfind("a=candidate:", 0) == 0) candidate.erase(0, 2);
    return candidate;
}

enum class VideoCodec {
    H264,
    VP8,
    VP9,
    AV1,
};

std::string lowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string codecName(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264: return "h264";
        case VideoCodec::VP8: return "vp8";
        case VideoCodec::VP9: return "vp9";
        case VideoCodec::AV1: return "av1";
    }
    return "unknown";
}

VideoCodec resolveVideoCodec(const std::string& requested, const std::string& backend) {
    const auto codec = lowerAscii(requested);
#if defined(__APPLE__)
    if (codec == "h264" || codec == "avc") return VideoCodec::H264;
    if (codec == "vp8") return VideoCodec::VP8;
    if (codec == "vp9") return VideoCodec::VP9;
    if (codec == "av1" || codec == "av01") return VideoCodec::AV1;

    // VMware's WindowServer path is software capture already. Prefer VP8 for
    // auto mode so it also bypasses VideoToolbox during compatibility testing.
    if (backend == "macos_windowserver") return VideoCodec::VP8;
    return VideoCodec::H264;
#else
    (void)backend;
    return VideoCodec::VP8;
#endif
}

int codecFps(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::VP8: return 20;
        case VideoCodec::VP9: return 15;
        case VideoCodec::AV1: return 8;
        case VideoCodec::H264: return 30;
    }
    return 30;
}

int codecBitrateKbps(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::VP8: return 7000;
        case VideoCodec::VP9: return 5500;
        case VideoCodec::AV1: return 3500;
        case VideoCodec::H264: return 12000;
    }
    return 8000;
}

#if defined(__APPLE__)
SoftwareVideoCodec softwareCodec(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::VP9: return SoftwareVideoCodec::VP9;
        case VideoCodec::AV1: return SoftwareVideoCodec::AV1;
        case VideoCodec::VP8:
        default:
            return SoftwareVideoCodec::VP8;
    }
}
#endif

} // namespace

class RemoteWebRtcSession {
public:
    using SendFn = RemoteDesktopManager::SendFn;

    RemoteWebRtcSession(
        std::string sessionId,
        std::vector<std::string> iceServers,
        std::string requestedCodec,
        SendFn send)
        : sessionId_(std::move(sessionId)),
          iceServers_(std::move(iceServers)),
          requestedCodec_(std::move(requestedCodec)),
          send_(std::move(send)),
          ssrc_(randomU32()),
          sequence_(randomU16()) {}

    ~RemoteWebRtcSession() { stop(); }

    bool start(std::string& error) {
        platform_ = createRemotePlatform();
        if (!platform_) {
            error = "No remote desktop provider is available for this platform.";
            return false;
        }
        if (!platform_->start(error)) return false;

        activeCodec_ = resolveVideoCodec(requestedCodec_, platform_->backendName());
        fps_ = codecFps(activeCodec_);
        bitrateKbps_ = codecBitrateKbps(activeCodec_);

        sendState("starting", {
            {"backend", platform_->backendName()},
            {"codec", codecName(activeCodec_)},
            {"fps", fps_},
            {"bitrate_kbps", bitrateKbps_},
            {"requires_user_consent", platform_->requiresConsent()}
        });

        try {
            createPeerConnection();
            sendMonitorInfo();
            return true;
        } catch (const std::exception& ex) {
            error = ex.what();
            stop();
            return false;
        }
    }

    void stop() {
        running_.store(false);
        canSend_.store(false);
        if (streamThread_.joinable()) streamThread_.join();
#if defined(__APPLE__)
        h264Encoder_.reset();
        softwareEncoder_.reset();
        av1Packetizer_.reset();
        av1RtpConfig_.reset();
#else
        encoder_.reset();
#endif
        track_.reset();
        if (pc_) {
            try { pc_->close(); } catch (...) {}
            pc_.reset();
        }
        if (platform_) {
            platform_->stop();
            platform_.reset();
        }
    }

    void handleSignaling(const json& message) {
        if (!pc_) return;
        const std::string type = message.value("type", "");

        if (type == "webrtc_answer" || type == "answer" || type == "viewer_answer") {
            if (answerHandled_.exchange(true)) return;
            std::string sdp;
            std::string sdpType = "answer";
            if (message.contains("sdp") && message["sdp"].is_string()) {
                sdp = message["sdp"].get<std::string>();
            } else if (message.contains("description") && message["description"].is_object()) {
                const auto& description = message["description"];
                sdp = description.value("sdp", "");
                sdpType = description.value("type", "answer");
            } else if (message.contains("answer") && message["answer"].is_object()) {
                const auto& answer = message["answer"];
                sdp = answer.value("sdp", "");
                sdpType = answer.value("type", "answer");
            }
            if (message.contains("sdp_type") && message["sdp_type"].is_string()) {
                sdpType = message["sdp_type"].get<std::string>();
            }
            if (sdp.empty()) {
                sendError("invalid_answer", "Viewer answer did not contain SDP.");
                return;
            }
            try {
                pc_->setRemoteDescription(rtc::Description(sdp, sdpType));
            } catch (const std::exception& ex) {
                sendError("answer_failed", ex.what());
            }
            return;
        }

        if (type == "ice_candidate" || type == "candidate") {
            std::string candidate;
            std::string mid = "0";
            if (message.contains("candidate") && message["candidate"].is_string()) {
                candidate = message["candidate"].get<std::string>();
            } else if (message.contains("candidate") && message["candidate"].is_object()) {
                candidate = message["candidate"].value("candidate", "");
                mid = message["candidate"].value("sdpMid", "0");
            }
            if (message.contains("sdpMid") && message["sdpMid"].is_string()) {
                mid = message["sdpMid"].get<std::string>();
            }
            candidate = normaliseCandidate(candidate);
            if (candidate.empty()) return;
            try {
                pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
            } catch (const std::exception& ex) {
                sendError("candidate_failed", ex.what());
            }
        }
    }

    void handleInput(const json& message) {
        if (!platform_) return;
        std::string error;
        if (!platform_->handleInput(message, error) && !error.empty()) {
            sendError("input_failed", error);
        }
    }

    void switchMonitor(int index) {
        if (!platform_) return;
        if (platform_->setDisplayIndex(index)) {
#if defined(__APPLE__)
            h264Encoder_.reset();
            softwareEncoder_.reset();
#else
            encoder_.reset();
#endif
            forceKeyframe_.store(true);
            sendMonitorInfo();
        }
    }

private:
    void createPeerConnection() {
        rtc::Configuration config;
        config.forceMediaTransport = true;
        if (iceServers_.empty()) {
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        } else {
            for (const auto& server : iceServers_) config.iceServers.emplace_back(server);
        }

        pc_ = std::make_shared<rtc::PeerConnection>(config);

        rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
#if defined(__APPLE__)
        switch (activeCodec_) {
            case VideoCodec::H264: media.addH264Codec(payloadType_); break;
            case VideoCodec::VP8: media.addVP8Codec(payloadType_); break;
            case VideoCodec::VP9: media.addVP9Codec(payloadType_); break;
            case VideoCodec::AV1: media.addAV1Codec(payloadType_); break;
        }
#else
        media.addVP8Codec(payloadType_);
#endif
        media.addSSRC(ssrc_, "video-stream");
        media.setBitrate(bitrateKbps_ * 1000);
        track_ = pc_->addTrack(media);

#if defined(__APPLE__)
        if (activeCodec_ == VideoCodec::AV1) {
            av1RtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc_,
                "video-stream",
                payloadType_,
                rtc::AV1RtpPacketizer::ClockRate);
            av1Packetizer_ = std::make_shared<rtc::AV1RtpPacketizer>(
                rtc::AV1RtpPacketizer::Packetization::TemporalUnit,
                av1RtpConfig_,
                1200);
            auto srReporter = std::make_shared<rtc::RtcpSrReporter>(av1RtpConfig_);
            av1Packetizer_->addToChain(srReporter);
            av1Packetizer_->addToChain(std::make_shared<rtc::RtcpNackResponder>());
            track_->setMediaHandler(av1Packetizer_);
        }
#endif

        track_->onOpen([this]() {
            canSend_.store(true);
            sendState("connected", {
                {"backend", platform_ ? platform_->backendName() : "unknown"},
                {"codec", codecName(activeCodec_)},
                {"fps", fps_},
                {"bitrate_kbps", bitrateKbps_}
            });
            startStreaming();
        });
        track_->onClosed([this]() {
            canSend_.store(false);
            running_.store(false);
        });

        pc_->onStateChange([this](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                canSend_.store(false);
                if (state == rtc::PeerConnection::State::Failed) {
                    sendError("peer_connection_failed", "WebRTC peer connection failed.");
                }
            }
        });

        pc_->onLocalDescription([this](rtc::Description description) {
            json payload = {
                {"type", "webrtc_offer"},
                {"session_id", sessionId_},
                {"sdp", std::string(description)},
                {"sdp_type", description.typeString()}
            };
            send_(payload);
        });

        pc_->onLocalCandidate([this](rtc::Candidate candidate) {
            send_({
                {"type", "ice_candidate"},
                {"session_id", sessionId_},
                {"candidate", std::string(candidate)},
                {"sdpMid", candidate.mid()},
                {"sdpMLineIndex", 0}
            });
        });

        pc_->setLocalDescription();
    }

    void startStreaming() {
        if (running_.exchange(true)) return;
        streamThread_ = std::thread([this]() { streamLoop(); });
    }

    void streamLoop() {
        const auto interval = std::chrono::milliseconds(1000 / fps_);
        std::uint64_t frameCounter = 0;

        while (running_.load()) {
            const auto started = std::chrono::steady_clock::now();
            if (!canSend_.load() || !track_ || !track_->isOpen() || !platform_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            try {
                auto captured = platform_->capture();
                if (captured.hasFrame && captured.frame.width > 0 && captured.frame.height > 0) {
                    const bool periodicKeyframe = (frameCounter % static_cast<std::uint64_t>(fps_ * 3)) == 0;
                    const bool force = forceKeyframe_.exchange(false) || periodicKeyframe;

#if defined(__APPLE__)
                    if (activeCodec_ == VideoCodec::H264) {
                        if (!h264Encoder_ ||
                            captured.frame.width != encoderWidth_ ||
                            captured.frame.height != encoderHeight_) {
                            encoderWidth_ = captured.frame.width;
                            encoderHeight_ = captured.frame.height;
                            softwareEncoder_.reset();
                            h264Encoder_ = std::make_unique<H264VideoToolboxEncoder>(
                                encoderWidth_, encoderHeight_, fps_, bitrateKbps_);
                            forceKeyframe_.store(true);
                        }

                        const auto encoded = h264Encoder_->encode(captured.frame, force);
                        if (!encoded.data.empty()) {
                            sendH264(encoded);
                            ++frameCounter;
                        }
                    } else {
                        if (!softwareEncoder_ ||
                            captured.frame.width != encoderWidth_ ||
                            captured.frame.height != encoderHeight_) {
                            encoderWidth_ = captured.frame.width;
                            encoderHeight_ = captured.frame.height;
                            h264Encoder_.reset();
                            softwareEncoder_ = std::make_unique<SoftwareVideoEncoder>(
                                softwareCodec(activeCodec_),
                                encoderWidth_,
                                encoderHeight_,
                                fps_,
                                bitrateKbps_,
                                std::max(2, std::min(8, static_cast<int>(std::thread::hardware_concurrency()))));
                            forceKeyframe_.store(true);
                        }

                        const auto encoded = softwareEncoder_->encode(captured.frame, force);
                        if (!encoded.data.empty()) {
                            if (activeCodec_ == VideoCodec::VP8) sendVp8Software(encoded);
                            else if (activeCodec_ == VideoCodec::VP9) sendVp9(encoded);
                            else if (activeCodec_ == VideoCodec::AV1) sendAv1(encoded);
                            ++frameCounter;
                        }
                    }
#else
                    if (!encoder_ ||
                        captured.frame.width != encoderWidth_ ||
                        captured.frame.height != encoderHeight_) {
                        encoderWidth_ = captured.frame.width;
                        encoderHeight_ = captured.frame.height;
                        encoder_ = std::make_unique<Vp8Encoder>(
                            encoderWidth_, encoderHeight_, fps_, bitrateKbps_,
                            6, 4, 36, std::max(2, std::min(6, static_cast<int>(std::thread::hardware_concurrency()))));
                        forceKeyframe_.store(true);
                    }

                    const auto encoded = encoder_->encode(captured.frame, force);
                    if (!encoded.data.empty()) {
                        sendVp8(encoded);
                        ++frameCounter;
                    }
#endif
                }
            } catch (const std::exception& ex) {
                sendError("capture_failed", ex.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed < interval) std::this_thread::sleep_for(interval - elapsed);
        }
    }

#if defined(__APPLE__)
    std::vector<std::vector<std::uint8_t>> splitH264NalUnits(const std::vector<std::uint8_t>& frame) {
        std::vector<std::vector<std::uint8_t>> nalUnits;
        if (frame.empty()) return nalUnits;

        auto isStart3 = [&](std::size_t i) {
            return i + 3 <= frame.size() &&
                frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1;
        };
        auto isStart4 = [&](std::size_t i) {
            return i + 4 <= frame.size() &&
                frame[i] == 0 && frame[i + 1] == 0 &&
                frame[i + 2] == 0 && frame[i + 3] == 1;
        };

        std::size_t i = 0;
        while (i < frame.size()) {
            while (i < frame.size() && !isStart3(i) && !isStart4(i)) ++i;
            if (i >= frame.size()) break;
            const std::size_t startCode = isStart4(i) ? 4 : 3;
            const std::size_t nalStart = i + startCode;
            i = nalStart;
            while (i < frame.size() && !isStart3(i) && !isStart4(i)) ++i;
            if (i > nalStart) {
                nalUnits.emplace_back(
                    frame.begin() + static_cast<std::ptrdiff_t>(nalStart),
                    frame.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        if (nalUnits.empty()) nalUnits.push_back(frame);
        return nalUnits;
    }

    void sendH264(const H264EncodedFrame& frame) {
        if (!canSend_.load() || !track_ || !track_->isOpen()) return;

        const auto nalUnits = splitH264NalUnits(frame.data);
        constexpr std::size_t maxPayload = 1200;
        std::lock_guard<std::mutex> lock(sendMutex_);

        for (std::size_t ni = 0; ni < nalUnits.size(); ++ni) {
            const auto& nal = nalUnits[ni];
            if (nal.empty()) continue;

            if (nal.size() <= maxPayload) {
                const bool marker = ni + 1 == nalUnits.size();
                std::vector<std::uint8_t> packet(12 + nal.size());
                packet[0] = 0x80;
                packet[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (payloadType_ & 0x7f));
                packet[2] = static_cast<std::uint8_t>((sequence_ >> 8) & 0xff);
                packet[3] = static_cast<std::uint8_t>(sequence_ & 0xff);
                packet[4] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 24) & 0xff);
                packet[5] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 16) & 0xff);
                packet[6] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 8) & 0xff);
                packet[7] = static_cast<std::uint8_t>(frame.rtpTimestamp & 0xff);
                packet[8] = static_cast<std::uint8_t>((ssrc_ >> 24) & 0xff);
                packet[9] = static_cast<std::uint8_t>((ssrc_ >> 16) & 0xff);
                packet[10] = static_cast<std::uint8_t>((ssrc_ >> 8) & 0xff);
                packet[11] = static_cast<std::uint8_t>(ssrc_ & 0xff);
                std::copy(nal.begin(), nal.end(), packet.begin() + 12);

                rtc::binary binary;
                binary.reserve(packet.size());
                for (const auto byte : packet) binary.push_back(static_cast<std::byte>(byte));
                track_->send(binary);
                ++sequence_;
                continue;
            }

            const std::uint8_t nalHeader = nal[0];
            const std::uint8_t fuIndicator =
                static_cast<std::uint8_t>((nalHeader & 0xe0) | 28);
            const std::uint8_t nalType = static_cast<std::uint8_t>(nalHeader & 0x1f);
            std::size_t offset = 1;
            bool start = true;

            while (offset < nal.size()) {
                const std::size_t chunk = std::min(maxPayload - 2, nal.size() - offset);
                const bool end = offset + chunk >= nal.size();
                const bool marker = end && ni + 1 == nalUnits.size();

                std::vector<std::uint8_t> packet(14 + chunk);
                packet[0] = 0x80;
                packet[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (payloadType_ & 0x7f));
                packet[2] = static_cast<std::uint8_t>((sequence_ >> 8) & 0xff);
                packet[3] = static_cast<std::uint8_t>(sequence_ & 0xff);
                packet[4] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 24) & 0xff);
                packet[5] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 16) & 0xff);
                packet[6] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 8) & 0xff);
                packet[7] = static_cast<std::uint8_t>(frame.rtpTimestamp & 0xff);
                packet[8] = static_cast<std::uint8_t>((ssrc_ >> 24) & 0xff);
                packet[9] = static_cast<std::uint8_t>((ssrc_ >> 16) & 0xff);
                packet[10] = static_cast<std::uint8_t>((ssrc_ >> 8) & 0xff);
                packet[11] = static_cast<std::uint8_t>(ssrc_ & 0xff);
                packet[12] = fuIndicator;
                packet[13] = static_cast<std::uint8_t>(
                    (start ? 0x80 : 0x00) |
                    (end ? 0x40 : 0x00) |
                    nalType);
                std::copy(
                    nal.begin() + static_cast<std::ptrdiff_t>(offset),
                    nal.begin() + static_cast<std::ptrdiff_t>(offset + chunk),
                    packet.begin() + 14);

                rtc::binary binary;
                binary.reserve(packet.size());
                for (const auto byte : packet) binary.push_back(static_cast<std::byte>(byte));
                track_->send(binary);
                ++sequence_;
                offset += chunk;
                start = false;
            }
        }
    }

    void sendRtpPayload(
        const std::uint8_t* payload,
        std::size_t payloadSize,
        std::uint32_t timestamp,
        bool marker) {

        std::vector<std::uint8_t> packet(12 + payloadSize);
        packet[0] = 0x80;
        packet[1] = static_cast<std::uint8_t>(
            (marker ? 0x80 : 0x00) | (payloadType_ & 0x7f));
        packet[2] = static_cast<std::uint8_t>((sequence_ >> 8) & 0xff);
        packet[3] = static_cast<std::uint8_t>(sequence_ & 0xff);
        packet[4] = static_cast<std::uint8_t>((timestamp >> 24) & 0xff);
        packet[5] = static_cast<std::uint8_t>((timestamp >> 16) & 0xff);
        packet[6] = static_cast<std::uint8_t>((timestamp >> 8) & 0xff);
        packet[7] = static_cast<std::uint8_t>(timestamp & 0xff);
        packet[8] = static_cast<std::uint8_t>((ssrc_ >> 24) & 0xff);
        packet[9] = static_cast<std::uint8_t>((ssrc_ >> 16) & 0xff);
        packet[10] = static_cast<std::uint8_t>((ssrc_ >> 8) & 0xff);
        packet[11] = static_cast<std::uint8_t>(ssrc_ & 0xff);
        std::copy(payload, payload + payloadSize, packet.begin() + 12);

        rtc::binary binary;
        binary.reserve(packet.size());
        for (const auto byte : packet) binary.push_back(static_cast<std::byte>(byte));
        track_->send(binary);
        ++sequence_;
    }

    void sendVp8Software(const SoftwareEncodedFrame& frame) {
        if (!canSend_.load() || !track_ || !track_->isOpen()) return;

        constexpr std::size_t maxRtpPayload = 1200;
        std::size_t offset = 0;
        bool first = true;
        std::lock_guard<std::mutex> lock(sendMutex_);

        while (offset < frame.data.size()) {
            const std::size_t chunk =
                std::min(frame.data.size() - offset, maxRtpPayload - 1);
            const bool marker = offset + chunk >= frame.data.size();

            std::vector<std::uint8_t> payload(1 + chunk);
            payload[0] = first ? 0x10 : 0x00;
            std::copy(
                frame.data.begin() + static_cast<std::ptrdiff_t>(offset),
                frame.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk),
                payload.begin() + 1);

            sendRtpPayload(
                payload.data(),
                payload.size(),
                frame.rtpTimestamp,
                marker);

            offset += chunk;
            first = false;
        }
    }

    void sendVp9(const SoftwareEncodedFrame& frame) {
        if (!canSend_.load() || !track_ || !track_->isOpen()) return;

        // RFC 7741 permits a one-byte descriptor when Picture ID, layer
        // information and scalability structure are omitted. P marks an
        // inter-picture predicted frame; B/E delimit this frame.
        constexpr std::size_t maxRtpPayload = 1200;
        std::size_t offset = 0;
        bool first = true;
        std::lock_guard<std::mutex> lock(sendMutex_);

        while (offset < frame.data.size()) {
            const std::size_t chunk =
                std::min(frame.data.size() - offset, maxRtpPayload - 1);
            const bool marker = offset + chunk >= frame.data.size();

            std::uint8_t descriptor = frame.keyframe ? 0x00 : 0x40;
            if (first) descriptor |= 0x08;
            if (marker) descriptor |= 0x04;

            std::vector<std::uint8_t> payload(1 + chunk);
            payload[0] = descriptor;
            std::copy(
                frame.data.begin() + static_cast<std::ptrdiff_t>(offset),
                frame.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk),
                payload.begin() + 1);

            sendRtpPayload(
                payload.data(),
                payload.size(),
                frame.rtpTimestamp,
                marker);

            offset += chunk;
            first = false;
        }
    }

    void sendAv1(const SoftwareEncodedFrame& frame) {
        if (!canSend_.load() || !track_ || !track_->isOpen() || !av1Packetizer_) return;

        rtc::binary binary;
        binary.reserve(frame.data.size());
        for (const auto byte : frame.data) binary.push_back(static_cast<std::byte>(byte));

        std::lock_guard<std::mutex> lock(sendMutex_);
        track_->sendFrame(std::move(binary), rtc::FrameInfo(frame.rtpTimestamp));
    }
#else
    void sendVp8(const EncodedFrame& frame) {
        if (!canSend_.load() || !track_ || !track_->isOpen()) return;

        constexpr std::size_t maxRtpPayload = 1200;
        std::size_t offset = 0;
        bool first = true;
        std::lock_guard<std::mutex> lock(sendMutex_);

        while (offset < frame.data.size()) {
            const std::size_t remaining = frame.data.size() - offset;
            const std::size_t chunk = std::min(remaining, maxRtpPayload - 1);
            const bool marker = offset + chunk >= frame.data.size();

            std::vector<std::uint8_t> packet(13 + chunk);
            packet[0] = 0x80;
            packet[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | payloadType_);
            packet[2] = static_cast<std::uint8_t>((sequence_ >> 8) & 0xff);
            packet[3] = static_cast<std::uint8_t>(sequence_ & 0xff);
            packet[4] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 24) & 0xff);
            packet[5] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 16) & 0xff);
            packet[6] = static_cast<std::uint8_t>((frame.rtpTimestamp >> 8) & 0xff);
            packet[7] = static_cast<std::uint8_t>(frame.rtpTimestamp & 0xff);
            packet[8] = static_cast<std::uint8_t>((ssrc_ >> 24) & 0xff);
            packet[9] = static_cast<std::uint8_t>((ssrc_ >> 16) & 0xff);
            packet[10] = static_cast<std::uint8_t>((ssrc_ >> 8) & 0xff);
            packet[11] = static_cast<std::uint8_t>(ssrc_ & 0xff);
            packet[12] = first ? 0x10 : 0x00;
            std::copy(
                frame.data.begin() + static_cast<std::ptrdiff_t>(offset),
                frame.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk),
                packet.begin() + 13);

            rtc::binary binary;
            binary.reserve(packet.size());
            for (const auto byte : packet) binary.push_back(static_cast<std::byte>(byte));
            track_->send(binary);

            ++sequence_;
            offset += chunk;
            first = false;
        }
    }
#endif

    void sendMonitorInfo() {
        if (!platform_) return;
        json monitors = json::array();
        for (const auto& display : platform_->displays()) {
            monitors.push_back({
                {"index", display.index},
                {"name", display.name},
                {"x", display.x},
                {"y", display.y},
                {"w", display.width},
                {"h", display.height},
                {"primary", display.primary}
            });
        }
        send_({
            {"type", "monitor_info"},
            {"session_id", sessionId_},
            {"current", platform_->currentDisplayIndex()},
            {"monitors", monitors}
        });
    }

    void sendState(const std::string& state, json extra = json::object()) {
        json payload = {
            {"type", "session_state"},
            {"session_id", sessionId_},
            {"state", state}
        };
        if (extra.is_object()) {
            for (auto it = extra.begin(); it != extra.end(); ++it) payload[it.key()] = it.value();
        }
        send_(payload);
    }

    void sendError(const std::string& code, const std::string& message) {
        send_({
            {"type", "remote_error"},
            {"session_id", sessionId_},
            {"code", code},
            {"message", message}
        });
    }

    std::string sessionId_;
    std::vector<std::string> iceServers_;
    std::string requestedCodec_ = "auto";
    SendFn send_;

    VideoCodec activeCodec_ = VideoCodec::VP8;
    std::unique_ptr<RemotePlatform> platform_;
#if defined(__APPLE__)
    std::unique_ptr<H264VideoToolboxEncoder> h264Encoder_;
    std::unique_ptr<SoftwareVideoEncoder> softwareEncoder_;
    std::shared_ptr<rtc::RtpPacketizationConfig> av1RtpConfig_;
    std::shared_ptr<rtc::AV1RtpPacketizer> av1Packetizer_;
#else
    std::unique_ptr<Vp8Encoder> encoder_;
#endif
    int encoderWidth_ = 0;
    int encoderHeight_ = 0;

    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> track_;

    std::thread streamThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> canSend_{false};
    std::atomic<bool> answerHandled_{false};
    std::atomic<bool> forceKeyframe_{true};
    std::mutex sendMutex_;

    std::uint32_t ssrc_ = 0;
    std::uint16_t sequence_ = 0;
    const int payloadType_ = 96;
    int fps_ = 30;
    int bitrateKbps_ = 12000;
};

RemoteDesktopManager::RemoteDesktopManager(SendFn send) : send_(std::move(send)) {}

RemoteDesktopManager::~RemoteDesktopManager() {
    stopAll();
}

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
                {"message", "Unix remote desktop currently supports console sessions only."}
            });
            return true;
        }

        const std::string requestedCodec =
            message.value("video_codec", message.value("codec", "auto"));

        // A codec change/reconnect for the same authorised session must never
        // leave two macOS capture pipelines running at once.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto existing = sessions_.find(sessionId);
            if (existing != sessions_.end()) {
                existing->second->stop();
                sessions_.erase(existing);
            }
        }

        auto session = std::make_unique<RemoteWebRtcSession>(
            sessionId,
            iceServersFromMessage(message),
            requestedCodec,
            send_);
        std::string error;
        if (!session->start(error)) {
            send_({
                {"type", "remote_error"},
                {"session_id", sessionId},
                {"code", "remote_start_failed"},
                {"message", error.empty() ? "Remote desktop could not start." : error}
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

    if (type == "webrtc_answer" || type == "answer" || type == "viewer_answer" ||
        type == "ice_candidate" || type == "candidate") {
        it->second->handleSignaling(message);
        return true;
    }
    if (type == "input_event") {
        it->second->handleInput(message);
        return true;
    }
    if (type == "switch_monitor") {
        it->second->switchMonitor(message.value("monitor_index", 0));
        return true;
    }
    if (type == "viewer_disconnected" || type == "viewer_closed" ||
        type == "viewer_left" || type == "end_session" || type == "stop_webrtc") {
        it->second->stop();
        sessions_.erase(it);
        return true;
    }

    return false;
}

void RemoteDesktopManager::stopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, session] : sessions_) {
        if (session) session->stop();
    }
    sessions_.clear();
}

} // namespace hi5