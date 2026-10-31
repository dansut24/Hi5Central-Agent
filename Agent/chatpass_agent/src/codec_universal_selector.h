#pragma once
// Vendor-neutral, side-effect-free policy for Hi5Central remote codec selection.
// "Available" hardware MFT enumeration is NOT a successful qualification.
// Callers must provide real emitted/decoded frames, IDR recovery and timings.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace hi5 {
namespace universal_codec {

inline std::string normalizeCodec(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto first = name.find_first_not_of(" \\t\\r\\n");
    if (first == std::string::npos) return {};
    const auto last = name.find_last_not_of(" \\t\\r\\n");
    name = name.substr(first, last - first + 1);
    return name == "hevc" ? "h265" : name;
}

struct Candidate {
    std::string codec;              // h264, vp9, vp8, av1, h265; RTP-ready only after proof
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
                     codec == "h265" ? 3300 :
                     codec == "vp9" ? 3400 :
                     codec == "h264" ? 5200 : 6100;
    return static_cast<int>(std::ceil(base * factor));
}

inline bool proven(const Candidate& c, int targetFps) {
    if (c.codec != "h264" && c.codec != "vp9" &&
        c.codec != "vp8" && c.codec != "av1" && c.codec != "h265") return false;
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

// Only bounded, non-identifying performance observations are persisted.
// Never cache screen pixels, account names, network addresses or SDP offers.
// The cache is advisory: if missing, stale or corrupt, fall back safely.
inline bool saveQualificationCache(const std::filesystem::path& path,
                                   const CacheIdentity& identity,
                                   const std::vector<Candidate>& candidates) {
    if (identity.qualifiedAtUnixSeconds <= 0 || candidates.size() > 32) return false;
    const auto temp = std::filesystem::path(path.string() + ".new");
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(temp, std::ios::trunc | std::ios::binary);
        if (!file) return false;
        file << "H5C-CODEC-QUALIFICATION-1\n"
             << std::quoted(identity.agentBuild) << " "
             << std::quoted(identity.operatingSystem) << " "
             << std::quoted(identity.gpuAndDriver) << " "
             << std::quoted(identity.encoderConfigVersion) << " "
             << identity.qualifiedAtUnixSeconds << "\n"
             << candidates.size() << "\n" << std::setprecision(17);
        for (const auto& c : candidates) {
            file << std::quoted(c.codec) << " " << std::quoted(c.backend) << " "
                 << c.hardware << " " << c.emittedFrames << " "
                 << c.decodedFrames << " " << c.keyframeRecovery << " "
                 << c.packetizationVerified << " " << c.testedFrames << " "
                 << c.maxWidth << " " << c.maxHeight << " "
                 << c.p95EncodeMs << " " << c.worstEncodeMs << "\n";
        }
        file.flush();
        if (!file.good()) return false;
        file.close();
        // Failure to replace must never interfere with the Agent session.
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp, path, ec);
        return !ec;
    } catch (...) { return false; }
}
inline std::vector<Candidate> loadQualificationCache(
    const std::filesystem::path& path, const CacheIdentity& current,
    int64_t nowUnixSeconds) {
    std::vector<Candidate> candidates;
    try {
        if (!std::filesystem::exists(path) ||
            std::filesystem::file_size(path) > 16384) return candidates;
        std::ifstream file(path, std::ios::binary);
        std::string magic;
        if (!std::getline(file, magic) || magic != "H5C-CODEC-QUALIFICATION-1")
            return {};
        CacheIdentity id;
        if (!(file >> std::quoted(id.agentBuild) >> std::quoted(id.operatingSystem)
                   >> std::quoted(id.gpuAndDriver) >> std::quoted(id.encoderConfigVersion)
                   >> id.qualifiedAtUnixSeconds) ||
            !cacheValid(id, current, nowUnixSeconds)) return {};
        size_t count = 0;
        if (!(file >> count) || count > 32) return {};
        for (size_t i=0;i<count;++i) {
            Candidate c;
            if (!(file >> std::quoted(c.codec) >> std::quoted(c.backend)
                       >> c.hardware >> c.emittedFrames >> c.decodedFrames
                       >> c.keyframeRecovery >> c.packetizationVerified
                       >> c.testedFrames >> c.maxWidth >> c.maxHeight
                       >> c.p95EncodeMs >> c.worstEncodeMs))
                return {};
            if (c.testedFrames < 0 || c.maxWidth < 0 || c.maxHeight < 0 ||
                !std::isfinite(c.p95EncodeMs) || !std::isfinite(c.worstEncodeMs))
                return {};
            candidates.push_back(std::move(c));
        }
    } catch (...) { return {}; }
    return candidates;
}

} // namespace universal_codec
} // namespace hi5
