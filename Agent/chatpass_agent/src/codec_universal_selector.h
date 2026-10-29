#pragma once
// Vendor-neutral, side-effect-free policy for Hi5Central remote codec selection.
// "Available" hardware MFT enumeration is NOT a successful qualification.
// Callers must provide real emitted/decoded frames, IDR recovery and timings.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hi5 {
namespace universal_codec {

struct Candidate {
    std::string codec;              // h264, vp9, vp8, av1 (RTP-ready only)
    std::string backend;            // Media Foundation, libvpx, VideoToolbox, ...
    bool hardware = false;
    bool emittedFrames = false;
    bool decodedFrames = false;
    bool keyframeRecovery = false;
    bool packetizationVerified = false;
    int testedFrames = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    double p95EncodeMs = 0.0;
    double worstEncodeMs = 0.0;
};

struct Viewer {
    std::vector<std::string> receiveCodecs;  // from actual viewer capabilities
    int maxWidth = 1920;
    int maxHeight = 1080;
    bool h264Level4 = false;         // true only after supported SDP negotiation
};

struct Link {
    int estimatedKbps = 0;          // 0 = unknown, never assume high bandwidth
    double rttMs = 0.0;
    double lossFraction = 0.0;
};

struct Choice {
    std::string codec = "vp8";
    std::string backend = "libvpx";
    bool hardware = false;
    int maxWidth = 1280;
    int maxHeight = 720;
    bool qualified = false;
    std::string reason = "universal fail-safe VP8";
};

inline bool supports(const Viewer& viewer, const std::string& codec) {
    return std::find(viewer.receiveCodecs.begin(), viewer.receiveCodecs.end(), codec)
        != viewer.receiveCodecs.end();
}

inline int requiredKbps(const std::string& codec, int width, int height) {
    // Conservative planning estimates, NOT encoder performance guarantees.
    const double pixels = static_cast<double>(width) * height;
    const double factor = pixels / (1280.0 * 720.0);
    const int base = codec == "av1" ? 2700 :
                     codec == "vp9" ? 3400 :
                     codec == "h264" ? 5200 : 6100;
    return static_cast<int>(std::ceil(base * factor));
}

inline bool proven(const Candidate& c, int targetFps) {
    if (c.codec != "h264" && c.codec != "vp9" &&
        c.codec != "vp8" && c.codec != "av1") return false;
    const double interval = 1000.0 / std::max(1, targetFps);
    return c.emittedFrames && c.decodedFrames && c.keyframeRecovery &&
        c.packetizationVerified && c.testedFrames >= 20 &&
        c.p95EncodeMs > 0.0 && c.p95EncodeMs < interval * 0.75 &&
        c.worstEncodeMs > 0.0 && c.worstEncodeMs < interval * 1.5 &&
        c.maxWidth >= 320 && c.maxHeight >= 240;
}

inline Choice select(
    const std::vector<Candidate>& qualifications,
    const Viewer& viewer,
    const Link& link,
    int desiredWidth = 1920,
    int desiredHeight = 1080,
    int targetFps = 20) {
    Choice fallback;
    if (!supports(viewer, "vp8")) {
        // No universal-compatible receiver was supplied. The caller MUST
        // negotiate a supported codec or reject the session; never send VP8
        // on a track that did not negotiate it.
        fallback.codec.clear();
        fallback.backend.clear();
        fallback.qualified = false;
        fallback.reason = "no mutually supported safe RTP codec";
    }
    double bestScore = -1e9;
    Choice best = fallback;
    for (const Candidate& c : qualifications) {
        if (!proven(c, targetFps) || !supports(viewer, c.codec)) continue;
        if (c.codec == "h264" && !viewer.h264Level4) {
            // L3.1 is valid only for up to 720p at our 20fps target.
            if (c.maxWidth <= 0 || c.maxHeight <= 0) continue;
        }
        const int capW = c.codec == "h264" && !viewer.h264Level4
            ? std::min(1280, c.maxWidth) : c.maxWidth;
        const int capH = c.codec == "h264" && !viewer.h264Level4
            ? std::min(720, c.maxHeight) : c.maxHeight;
        const int w = std::min({capW, viewer.maxWidth, std::max(2, desiredWidth)});
        const int h = std::min({capH, viewer.maxHeight, std::max(2, desiredHeight)});
        if (w < 320 || h < 240) continue;
        const int kbps = requiredKbps(c.codec, w, h);
        if (link.estimatedKbps > 0 && kbps > link.estimatedKbps * 0.75)
            continue;
        // Prioritise readable pixels, then real-time headroom, with a small
        // benefit for verified hardware when competing choices are similar.
        const double area = static_cast<double>(w) * h;
        const double score = 100.0 * area / (1920.0 * 1080.0) +
            (c.hardware ? 10.0 : 0.0) -
            c.p95EncodeMs * 1.1 -
            c.worstEncodeMs * 0.15 -
            (link.lossFraction > 0.03 ? requiredKbps(c.codec,w,h) / 400.0 : 0.0) -
            (link.rttMs > 180.0 ? c.p95EncodeMs * 0.3 : 0.0);
        if (score > bestScore) {
            bestScore = score;
            best.codec = c.codec;
            best.backend = c.backend;
            best.hardware = c.hardware;
            best.maxWidth = w;
            best.maxHeight = h;
            best.qualified = true;
            best.reason = "verified encoder+decoder, RTP, keyframes and latency";
        }
    }
    return best;
}

// Persisted qualifications must be invalidated when any part of the machine
// environment changes that can affect the encoder. Do not cache SDP/browser
// capabilities: those must always be checked per Viewer connection.
struct CacheIdentity {
    std::string agentBuild;
    std::string operatingSystem;
    std::string gpuAndDriver;
    std::string encoderConfigVersion;
    int64_t qualifiedAtUnixSeconds = 0;
};
inline bool cacheValid(const CacheIdentity& saved, const CacheIdentity& current,
                       int64_t nowUnixSeconds, int64_t maxAgeSeconds = 7 * 86400) {
    if (saved.qualifiedAtUnixSeconds <= 0 ||
        nowUnixSeconds < saved.qualifiedAtUnixSeconds ||
        nowUnixSeconds - saved.qualifiedAtUnixSeconds > maxAgeSeconds) return false;
    return !saved.agentBuild.empty() &&
        saved.agentBuild == current.agentBuild &&
        !saved.operatingSystem.empty() &&
        saved.operatingSystem == current.operatingSystem &&
        !saved.gpuAndDriver.empty() &&
        saved.gpuAndDriver == current.gpuAndDriver &&
        !saved.encoderConfigVersion.empty() &&
        saved.encoderConfigVersion == current.encoderConfigVersion;
}

} // namespace universal_codec
} // namespace hi5
