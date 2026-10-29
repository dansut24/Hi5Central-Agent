#pragma once
// Correlates two independent sources: Agent-encoded RTP output and authenticated
// Viewer decoding. A Media Foundation enum alone can NEVER create a proof.
#include "codec_universal_selector.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hi5::universal_codec {

struct EncoderWindow {
    std::string codec;           // h264, vp9, vp8
    std::string mode;            // h264_hw, h264_sw, vp9_hw, vp9_sw, vp8
    unsigned int frames = 0;
    unsigned int sent = 0;
    int width = 0;
    int height = 0;
    double maxEncodeMs = 0;
    double p95EncodeMs = 0;  // measured, never inferred from the maximum
};
struct DecoderReport {
    std::string codec;
    std::uint64_t decoded = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t dropped = 0;
    std::uint64_t packets = 0;
    int width = 0;
    int height = 0;
};
class RuntimeProofStore {
    struct Evidence {
        EncoderWindow last{};
        unsigned int encoded = 0;
        unsigned int sent = 0;
        double maxEncodeMs = 0;
        double p95EncodeMs = 0;
        std::int64_t lastReportUnix = 0;
        // A startup IDR is not evidence that the decoder can recover later.
        // Require a SECOND decoded keyframe after a later observation.
        std::uint64_t initialDecoded = 0;
        std::uint64_t initialKeyframes = 0;
        std::uint64_t initialPackets = 0;
        std::int64_t firstViewerObservationUnix = 0;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, Evidence> sessions_;
public:
    void recordEncoder(const std::string& session, const EncoderWindow& metric,
                       std::int64_t timestamp) {
        if(session.empty() || session.size()>160 || timestamp<=0 ||
           (metric.codec!="h264" && metric.codec!="vp9" && metric.codec!="vp8") ||
           metric.frames==0 || metric.frames>10000 || metric.sent>metric.frames ||
           metric.width<320 || metric.height<240 ||
           metric.width>7680 || metric.height>4320 ||
           !std::isfinite(metric.maxEncodeMs) || metric.maxEncodeMs<=0 ||
           metric.maxEncodeMs>30000 ||
           !std::isfinite(metric.p95EncodeMs) || metric.p95EncodeMs<=0 ||
           metric.p95EncodeMs>metric.maxEncodeMs)return;
        std::lock_guard<std::mutex> guard(mutex_);
        if(sessions_.size()>64) sessions_.clear();
        Evidence& evidence=sessions_[session];
        if(evidence.last.codec!=metric.codec || evidence.last.mode!=metric.mode ||
           evidence.last.width!=metric.width || evidence.last.height!=metric.height ||
           timestamp-evidence.lastReportUnix>90) evidence=Evidence{};
        evidence.last=metric;
        evidence.encoded=std::min(100000u,evidence.encoded+metric.frames);
        evidence.sent=std::min(100000u,evidence.sent+metric.sent);
        evidence.maxEncodeMs=std::max(evidence.maxEncodeMs,metric.maxEncodeMs);
        // The sender sends an exact rolling-window p95 on each report.
        // Keeping the maximum of old p95 readings would permanently poison
        // a healthy 2fps idle stream after one ten-frame sample spikes.
        evidence.p95EncodeMs=metric.p95EncodeMs;
        evidence.lastReportUnix=timestamp;
    }
    // Returns "unqualified" unless both the local encoder and the real
    // authenticated browser independently emitted convincing evidence.
    Candidate confirm(const std::string& session, const DecoderReport& decoder,
                      std::int64_t now, bool asyncH264,
                      std::string* reason = nullptr) {
        Candidate proof;
        const auto reject = [reason](const char* why) -> Candidate {
            if (reason) *reason=why;
            return Candidate{};
        };
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found=sessions_.find(session);
        if(found==sessions_.end())return reject("no_agent_encoder_sample");
        Evidence& e=found->second;
        const EncoderWindow& m=e.last;
        if(now<e.lastReportUnix || now-e.lastReportUnix>30)
            return reject("stale_agent_encoder_sample");
        if(decoder.codec!=m.codec) return reject("encoder_decoder_codec_mismatch");
        if(e.encoded<20 || e.sent<20) return reject("awaiting_20_encoded_frames");
        if(decoder.decoded<30 || decoder.keyframes<1 ||
           decoder.packets<decoder.decoded || decoder.dropped>decoder.decoded/5)
            return reject("insufficient_decoder_or_keyframe_evidence");
        if(decoder.width!=m.width || decoder.height!=m.height)
            return reject("encoder_decoder_resolution_mismatch");
        if(e.maxEncodeMs<=0 || e.p95EncodeMs<=0)
            return reject("measured_encoder_latency_missing");
        // VP9's current "prefer hardware" MFT path may select a software
        // transform. Until it exposes actual hardware-only MFT identity,
        // NEVER label a VP9 result as GPU accelerated.
        if(m.mode=="vp9_hw")return reject("vp9_hardware_transform_unverified");
        const bool hardware=(m.mode=="h264_hw" && asyncH264);
        if(m.codec=="h264" && m.mode=="h264_hw" && !asyncH264)
            return reject("hardware_h264_async_not_confirmed");
        if((m.codec=="h264" && m.mode!="h264_hw" && m.mode!="h264_sw") ||
           (m.codec=="vp9" && m.mode!="vp9_hw" && m.mode!="vp9_sw") ||
           (m.codec=="vp8" && m.mode!="vp8"))return reject("encoder_mode_unqualified");
        // The first keyframe generally arrives on initial connection;
        // this alone is NOT keyframe recovery. Wait for a later keyframe
        // and meaningful decoding progress before trusting this device.
        if(e.firstViewerObservationUnix == 0) {
            e.initialDecoded=decoder.decoded;
            e.initialKeyframes=decoder.keyframes;
            e.initialPackets=decoder.packets;
            e.firstViewerObservationUnix=now;
            return reject("awaiting_second_decoded_keyframe");
        }
        if(now-e.firstViewerObservationUnix < 3 ||
           decoder.keyframes <= e.initialKeyframes ||
           decoder.decoded < e.initialDecoded + 20 ||
           decoder.packets <= e.initialPackets)
            return reject("awaiting_second_decoded_keyframe");
        proof.codec=m.codec;
        proof.backend=m.mode;
        proof.hardware=hardware;
        proof.emittedFrames=true;
        proof.decodedFrames=true;
        proof.keyframeRecovery=true;
        proof.packetizationVerified=true;
        proof.testedFrames=static_cast<int>(e.sent);
        proof.maxWidth=m.width;
        proof.maxHeight=m.height;
        // A rare high-latency frame does not imply an equally slow p95.
        // Both measurements come from actual encoded frame timings.
        proof.p95EncodeMs=e.p95EncodeMs;
        proof.worstEncodeMs=e.maxEncodeMs;
        if(!proven(proof,20))return reject("measured_encoder_latency_outside_limits");
        // One proof per session. Duplicate browser reports cannot inflate data.
        sessions_.erase(found);
        if (reason) *reason="end_to_end_qualified";
        return proof;
    }
};
} // namespace hi5::universal_codec
