#pragma once
// Viewer reports are authenticated by the RMM relay. Only a failure of the
// exact codec offered for this *same session* can quarantine a codec.
// No RTP codec is changed in-flight. The caller must create a fresh SDP
// handshake on reconnect, or fall back to VP8 if the receiver supports it.
#include "codec_universal_selector.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hi5::universal_codec {

class SessionCodecFailover {
    struct State {
        std::string selected;
        std::vector<std::string> denied;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, State> sessions_;
    static std::string modeCodec(const std::string& mode) {
        if (mode == "vp8") return "vp8";
        if (mode == "vp9" || mode == "vp9_sw" || mode == "vp9_hw") return "vp9";
        if (mode == "h264" || mode == "h264_sw" || mode == "h264_hw") return "h264";
        if (mode == "av1_sw" || mode == "av1_hw") return "av1";
        if (mode == "h265_hw") return "h265";
        return {};
    }
public:
    void selected(const std::string& session, const std::string& mode) {
        if (session.empty() || session.size()>160) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.size() >= 64 && !sessions_.count(session)) sessions_.clear();
        sessions_[session].selected = modeCodec(mode);
    }
    bool failure(const std::string& session, const std::string& codec,
                 const std::string& reason) {
        if (session.empty() || session.size() > 160 ||
            (reason != "media-negotiation-timeout" && reason != "decode-unavailable"))
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(session);
        const std::string normalized = normalizeCodec(codec);
        if (found == sessions_.end() ||
            found->second.selected.empty() ||
            found->second.selected != normalized) return false;
        auto& denied = found->second.denied;
        if (std::find(denied.begin(),denied.end(),normalized) == denied.end()) {
            if (denied.size() >= 5) return false;
            denied.push_back(normalized);
        }
        return true;
    }
    std::vector<Candidate> allowed(const std::string& session,
                                   const std::vector<Candidate>& candidates) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(session);
        if (found == sessions_.end()) return candidates;
        std::vector<Candidate> out;
        for (const auto& c : candidates) {
            if (std::find(found->second.denied.begin(),found->second.denied.end(),
                          c.codec) == found->second.denied.end()) out.push_back(c);
        }
        return out;
    }
    bool denied(const std::string& session, const std::string& codec) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(session);
        return found != sessions_.end() &&
            std::find(found->second.denied.begin(),found->second.denied.end(),
                      normalizeCodec(codec)) != found->second.denied.end();
    }
};
} // namespace hi5::universal_codec
