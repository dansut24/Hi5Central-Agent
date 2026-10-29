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
        std::int64_t lastReportUnix = 0;
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
           metric.maxEncodeMs>30000)return;
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
        evidence.lastReportUnix=timestamp;
    }
    // Returns "unqualified" unless both the local encoder and the real
    // authenticated browser independently emitted convincing evidence.
    Candidate confirm(const std::string& session, const DecoderReport& decoder,
                      std::int64_t now, bool asyncH264) {
        Candidate proof;
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found=sessions_.find(session);
        if(found==sessions_.end())return proof;
        const Evidence& e=found->second;
        const EncoderWindow& m=e.last;
        if(now<e.lastReportUnix || now-e.lastReportUnix>30 ||
           decoder.codec!=m.codec || e.encoded<20 || e.sent<20 ||
           decoder.decoded<30 || decoder.keyframes<1 ||
           decoder.packets<decoder.decoded ||
           decoder.dropped>decoder.decoded/5 ||
           decoder.width!=m.width || decoder.height!=m.height ||
           e.maxEncodeMs<=0)return proof;
        const bool hardware=(m.mode=="h264_hw" && asyncH264) ||
            m.mode=="vp9_hw";
        if(m.codec=="h264" && m.mode=="h264_hw" && !asyncH264)return proof;
        if((m.codec=="h264" && m.mode!="h264_hw" && m.mode!="h264_sw") ||
           (m.codec=="vp9" && m.mode!="vp9_hw" && m.mode!="vp9_sw") ||
           (m.codec=="vp8" && m.mode!="vp8"))return proof;
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
        // Conservative upper bound: a measured maximum is >= p95.
        // Using it for p95 errs toward rejection rather than overclaiming.
        proof.p95EncodeMs=e.maxEncodeMs;
        proof.worstEncodeMs=e.maxEncodeMs;
        if(!proven(proof,20))return Candidate{};
        // One proof per session. Duplicate browser reports cannot inflate data.
        sessions_.erase(found);
        return proof;
    }
};
} // namespace hi5::universal_codec
