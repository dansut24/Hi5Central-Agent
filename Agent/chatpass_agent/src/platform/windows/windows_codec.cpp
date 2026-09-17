#include "codec_capabilities.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <wmcodecdsp.h>
#include <combaseapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace hi5 {
namespace {

static uint32_t FourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

static GUID MakeMfVideoSubtype(uint32_t fourcc) {
    // Media Foundation video subtypes use the standard video subtype GUID base:
    // XXXXXXXX-0000-0010-8000-00AA00389B71 where XXXXXXXX is the FOURCC.
    return GUID{ fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71} };
}

static std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static std::vector<std::string> EnumerateEncoderNames(const GUID& subtype, bool hardwareOnly) {
    std::vector<std::string> names;

    MFT_REGISTER_TYPE_INFO outputType{};
    outputType.guidMajorType = MFMediaType_Video;
    outputType.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    const UINT32 flags = hardwareOnly
        ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
        : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER);

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        flags,
        nullptr,
        &outputType,
        &activates,
        &count);

    if (FAILED(hr) || !activates || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return names;
    }

    for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* activate = activates[i];
        if (!activate) continue;

        wchar_t* friendly = nullptr;
        UINT32 friendlyLen = 0;
        if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) && friendly) {
            std::string name = WideToUtf8(friendly);
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
            CoTaskMemFree(friendly);
        }
        else {
            names.push_back(hardwareOnly ? "Unnamed hardware encoder" : "Unnamed software encoder");
        }

        activate->Release();
    }

    CoTaskMemFree(activates);
    return names;
}

static CodecEncoderCapability ProbeOne(const std::string& codec,
    const std::string& displayName,
    const GUID& subtype,
    bool viewerLikelySupported) {

    CodecEncoderCapability c{};
    c.codec = codec;
    c.displayName = displayName;
    c.viewerLikelySupported = viewerLikelySupported;
    c.hardwareEncoders = EnumerateEncoderNames(subtype, true);
    c.softwareEncoders = EnumerateEncoderNames(subtype, false);
    c.hardwareEncodeAvailable = !c.hardwareEncoders.empty();
    c.softwareEncodeAvailable = !c.softwareEncoders.empty();

    if (codec == "h264") {
        if (c.hardwareEncodeAvailable) {
            c.recommendation = "preferred for future auto mode once H.264 sender path is enabled";
        }
        else if (c.softwareEncodeAvailable) {
            c.recommendation = "available, but software H.264 may not beat current VP8 CPU";
        }
        else {
            c.recommendation = "not available locally";
        }
    }
    else if (codec == "vp8") {
        c.recommendation = "stable low-CPU fallback; implemented via bundled libvpx";
        c.softwareEncodeAvailable = true;
        if (c.softwareEncoders.empty()) c.softwareEncoders.push_back("libvpx VP8 encoder (bundled/current)");
    }
    else if (codec == "vp9") {
        c.recommendation = c.hardwareEncodeAvailable ? "hardware-first with bundled libvpx software fallback" : "bundled libvpx software fallback available";
        c.softwareEncodeAvailable = true;
        if (std::find(c.softwareEncoders.begin(), c.softwareEncoders.end(), "libvpx VP9 encoder (bundled)") == c.softwareEncoders.end())
            c.softwareEncoders.push_back("libvpx VP9 encoder (bundled)");
    }
    else if (codec == "av1") {
        c.recommendation = c.hardwareEncodeAvailable ? "prefer in Auto when Viewer also negotiates AV1" : "use only when a usable encoder is available";
    }
    else if (codec == "h265") {
        c.recommendation = c.hardwareEncodeAvailable ? "hardware HEVC available; select only when Viewer negotiates H.265" : "fallback only when local encoder exists";
    }
    else {
        c.recommendation = "codec available subject to live endpoint/Viewer validation";
    }

    return c;
}

static std::string NormalizeMode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (mode.empty()) return "auto";
    return mode;
}

} // namespace

CodecSelectionResult ProbeCodecCapabilitiesAndSelect(const std::string& requestedModeRaw) {
    CodecSelectionResult result{};
    result.requestedMode = NormalizeMode(requestedModeRaw);
    result.selectedCodec = "vp8";
    result.selectedReason = "adaptive codec capability probe";

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInit = SUCCEEDED(coHr);

    HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfHr)) {
        result.capabilities.push_back(CodecEncoderCapability{
            "vp8", "VP8", true, false, true, {}, {"libvpx VP8 encoder (bundled/current)"},
            "current stable fallback; Media Foundation probe failed"
        });
        result.selectedReason += "; Media Foundation probe failed";
        if (coInit) CoUninitialize();
        return result;
    }

    result.capabilities.push_back(ProbeOne("h264", "H.264/AVC", MakeMfVideoSubtype(FourCC('H','2','6','4')), true));
    result.capabilities.push_back(ProbeOne("h265", "H.265/HEVC", MakeMfVideoSubtype(FourCC('H','E','V','C')), false));
    result.capabilities.push_back(ProbeOne("vp8", "VP8", MakeMfVideoSubtype(FourCC('V','P','8','0')), true));
    result.capabilities.push_back(ProbeOne("vp9", "VP9", MakeMfVideoSubtype(FourCC('V','P','9','0')), true));
    result.capabilities.push_back(ProbeOne("av1", "AV1", MakeMfVideoSubtype(FourCC('A','V','0','1')), true));

    for (const auto& c : result.capabilities) {
        if (c.codec == "h264" && c.hardwareEncodeAvailable) {
            result.hardwareH264Available = true;
            break;
        }
    }

    if (result.requestedMode == "vp8") {
        result.selectedCodec = "vp8";
        result.selectedReason = "HI5_CODEC=vp8 requested; using bundled libvpx VP8";
    }
    else if (result.requestedMode == "vp9" || result.requestedMode == "vp9_hw" || result.requestedMode == "vp9_sw") {
        result.selectedCodec = "vp9";
        result.selectedReason = result.requestedMode == "vp9_sw" ? "bundled libvpx VP9 requested" : "VP9 hardware-first with bundled libvpx fallback";
    }
    else if (result.requestedMode == "av1" || result.requestedMode == "av1_hw" || result.requestedMode == "av1_sw") {
        result.selectedCodec = "av1";
        result.selectedReason = "AV1 requested; live encoder and Viewer negotiation validation required";
    }
    else if (result.requestedMode == "h265" || result.requestedMode == "h265_hw" || result.requestedMode == "h265_sw") {
        result.selectedCodec = "h265";
        result.selectedReason = "H.265 requested; live encoder and Viewer negotiation validation required";
    }
    else if (result.requestedMode == "h264_hw" || result.requestedMode == "h264" || result.requestedMode == "h264_sw") {
        result.selectedCodec = "h264";
        result.selectedReason = "H.264 requested with hardware/software fallback policy";
    }
    else if (result.requestedMode == "auto") {
        result.selectedCodec = "auto";
        result.selectedReason = "adaptive mode negotiates AV1/VP9/H.265/H.264/VP8 and selects by endpoint hardware plus live encode health";
    }
    else {
        result.selectedCodec = "vp8";
        result.selectedReason = "unknown codec mode; safe VP8 fallback selected";
    }

    MFShutdown();
    if (coInit) CoUninitialize();
    return result;
}

std::string CodecCapabilitiesToLogString(const CodecSelectionResult& result) {
    std::ostringstream oss;
    oss << "codec mode requested=" << result.requestedMode
        << " selected=" << result.selectedCodec
        << " reason=" << result.selectedReason;

    for (const auto& c : result.capabilities) {
        oss << "\n[codec] " << c.displayName
            << " viewer_likely=" << (c.viewerLikelySupported ? 1 : 0)
            << " hw=" << (c.hardwareEncodeAvailable ? 1 : 0)
            << " sw=" << (c.softwareEncodeAvailable ? 1 : 0)
            << " recommendation=" << c.recommendation;

        for (const auto& name : c.hardwareEncoders) {
            oss << "\n[codec]   hw_encoder=" << name;
        }
        for (const auto& name : c.softwareEncoders) {
            oss << "\n[codec]   sw_encoder=" << name;
        }
    }

    return oss.str();
}

} // namespace hi5
