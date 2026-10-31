#pragma once
#include <string>
namespace hi5::universal_codec {
enum class FidelityRequest { Automatic, HighQualityLossy, PixelPerfect };
struct FidelityCapabilities {
    std::string codec;
    bool encoderLosslessVerified=false;
    bool decoderLosslessProfileVerified=false;
    bool sourceIsUnscaled=false;
    bool preserves444OrRGB=false;
    bool fullRangePreserved=false;
};
struct FidelityDecision {
    bool pixelPerfect=false;
    bool highQualityLossy=false;
    std::string reason;
};
inline FidelityDecision chooseFidelity(
    FidelityRequest requested,const FidelityCapabilities& caps) {
    if(requested==FidelityRequest::Automatic)
        return {false,false,"adaptive-lossy"};
    if(requested==FidelityRequest::HighQualityLossy)
        return {false,true,"prioritise-readable-text-no-lossless-claim"};
    // VP8 is not a standard true-lossless encode path; QP=0 alone does not
    // establish lossless coding for H264 or HEVC. 4:2:0 conversion and any
    // scaling invalidate an RGB pixel-perfect claim for *every* codec.
    if (caps.codec!="vp9" && caps.codec!="h264" &&
        caps.codec!="h265" && caps.codec!="av1")
        return {false,true,"codec-has-no-qualified-lossless-path"};
    if (caps.encoderLosslessVerified && caps.decoderLosslessProfileVerified &&
        caps.sourceIsUnscaled && caps.preserves444OrRGB &&
        caps.fullRangePreserved)
        return {true,false,"end-to-end-bit-exact-profile-qualified"};
    return {false,true,"pixel-perfect-unavailable-use-high-quality-lossy"};
}
} // namespace hi5::universal_codec
