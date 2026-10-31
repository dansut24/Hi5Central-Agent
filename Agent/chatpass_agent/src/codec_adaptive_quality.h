#pragma once
// Session-local, transport-neutral adaptive bitrate policy.
// Quality preservation: never silently downscale an explicit 1080p profile.
// This policy does not claim the encoder is lossless or alter RTP codecs.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
namespace hi5::universal_codec {

struct LinkSample {
    double rttMs=0;
    double lossFraction=0;
    double jitterBufferMs=0;
    int packetsSampled=0;
};
struct AdaptiveDecision {
    int bitrateKbps=0; // zero: no change
    int tier=0;
    bool changed=false;
    std::string reason;
};
class AdaptiveQuality {
    mutable std::mutex mutex_;
    int baseKbps_=6000;
    int tier_=0;
    int bad_=0;
    int good_=0;
    std::int64_t lastChange_=0;
    static constexpr int tiers_[5] = {100,90,80,70,60};
public:
    void reset(int viewerRequestedKbps) {
        std::lock_guard<std::mutex> lock(mutex_);
        baseKbps_=std::clamp(viewerRequestedKbps,1500,16000);
        tier_=bad_=good_=0;
        lastChange_=0;
    }
    AdaptiveDecision observe(const LinkSample& sample,
                             std::int64_t nowSeconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        AdaptiveDecision out;
        out.tier=tier_;
        if (nowSeconds <= 0 || sample.packetsSampled < 20 ||
            sample.packetsSampled > 500000 ||
            !std::isfinite(sample.rttMs) ||
            !std::isfinite(sample.lossFraction) ||
            !std::isfinite(sample.jitterBufferMs) ||
            sample.rttMs < 0 || sample.rttMs > 60000 ||
            sample.lossFraction < 0 || sample.lossFraction > 1 ||
            sample.jitterBufferMs < 0 || sample.jitterBufferMs > 60000) {
            out.reason="insufficient-or-invalid-receiver-evidence";
            return out;
        }
        const bool poor=sample.lossFraction>=0.035 ||
            (sample.rttMs > 230 && sample.jitterBufferMs > 80) ||
            sample.jitterBufferMs > 180;
        const bool healthy=sample.lossFraction<0.006 &&
            (sample.rttMs==0 || sample.rttMs<140) &&
            sample.jitterBufferMs<70;
        if (poor) { bad_++;good_=0; }
        else if (healthy) {good_++;bad_=0;}
        else {bad_=good_=0;}
        const bool cooldown=lastChange_!=0 && nowSeconds-lastChange_<12;
        if (!cooldown && bad_>=2 && tier_<4) {
            ++tier_;lastChange_=nowSeconds;bad_=good_=0;
            out.changed=true;out.reason="sustained-real-network-congestion";
        } else if (!cooldown && good_>=6 && tier_>0) {
            --tier_;lastChange_=nowSeconds;bad_=good_=0;
            out.changed=true;out.reason="sustained-recovery";
        }
        out.tier=tier_;
        if (out.changed) out.bitrateKbps=
            (baseKbps_*tiers_[tier_])/100;
        return out;
    }
};
} // namespace hi5::universal_codec
