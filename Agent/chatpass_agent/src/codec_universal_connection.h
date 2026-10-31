#pragma once
// Negotiation must happen BEFORE creating WebRTC SDP. Never reinterpret an
// existing H.264/VP9 RTP payload as VP8 after an encoder failure.
#include "codec_universal_selector.h"
#include <string>
#include <vector>
namespace hi5::universal_codec {
struct ConnectionSelection {
    std::string mode;       // existing WebRtcSender codec mode
    bool qualified = false;
    bool compatible = true;
    std::string reason;
};
// Sharp-text H.264 uses a Level 4 stream. A receiver that does not
// explicitly advertise L4 must not be offered that profile. Keep VP8/VP9
// intact; the policy then selects a compatible fallback before SDP.
inline bool enforceSharpH264ViewerLevel(Viewer& viewer, bool sharpText) {
    if (!sharpText || viewer.h264Level4 || !supports(viewer, "h264"))
        return false;
    viewer.receiveCodecs.erase(std::remove(viewer.receiveCodecs.begin(),
        viewer.receiveCodecs.end(), "h264"), viewer.receiveCodecs.end());
    return true;
}
inline ConnectionSelection chooseForConnection(
    bool optIn, const std::string& operatorMode,
    const std::string& legacyAutoMode,
    const Viewer& viewer,
    const std::vector<Candidate>& cachedQualifications,
    const Link& link,
    bool asyncHardwareH264Ready) {
    // An explicit operator choice always overrides the experiment. The
    // WebRtcSender must still fail closed if its encoder cannot initialise.
    if (operatorMode != "auto")
        return {operatorMode,false,true,"explicit operator codec remains unchanged"};
    if (!optIn || viewer.receiveCodecs.empty())
        return {legacyAutoMode,false,true,"legacy automatic codec preserved"};
    // A live sender/packetiser must exist; AV1 and HEVC remain offline-only
    // regardless of successful Media Foundation hardware enumeration.
    std::vector<Candidate> supported;
    for (const auto& c:cachedQualifications) {
        if(c.codec=="av1" || c.codec=="h265" || c.codec=="hevc")continue;
        if(c.codec=="h264" && c.hardware && !asyncHardwareH264Ready)continue;
        if(c.codec!="vp8" && c.codec!="vp9" && c.codec!="h264")continue;
        supported.push_back(c);
    }
    const Choice c=select(supported,viewer,link);
    if(c.qualified) {
        if(c.codec=="h264") return {c.hardware?"h264_hw":"h264_sw",true,true,
            "end-to-end qualified H264 for this device and Viewer"};
        if(c.codec=="vp9") return {c.hardware?"vp9_hw":"vp9_sw",true,true,
            "end-to-end qualified VP9 for this device and Viewer"};
        return {"vp8",true,true,"qualified VP8"};
    }
    // A short synthetic VP9 startup benchmark does not establish that this
    // Viewer can decode a sustained full-resolution session. Only select a
    // different codec automatically when the encoder, real decoder and RTP
    // keyframe recovery have all been proven and cached above.
    if(supports(viewer,"vp8"))
        return {"vp8",false,true,"safe VP8 until end-to-end qualification"};
    // If a Viewer advertises neither VP8 nor a fully qualified codec, reject
    // BEFORE SDP. Do not substitute a different RTP payload mid-session.
    return {"",false,false,"no mutually qualified receive codec"};
}
} // namespace hi5::universal_codec
