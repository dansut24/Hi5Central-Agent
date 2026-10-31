#include "codec_policy.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace hi5 {

    static std::string getenvString(const char* name) {
#ifdef _WIN32
        char* value = nullptr;
        size_t len = 0;

        if (_dupenv_s(&value, &len, name) != 0 || !value) {
            return {};
        }

        std::string result(value);
        free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
#endif
    }

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });
        return value;
    }

    CodecDecision SelectCodecPolicy() {
        std::string requested = lower(getenvString("HI5_CODEC"));
        // PowerShell or an installer can accidentally persist the literal
        // string "$null" instead of clearing the machine environment value.
        // Such sentinels must not bypass normal automatic qualification.
        const auto first = requested.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) requested.clear();
        else {
            const auto last = requested.find_last_not_of(" \t\r\n");
            requested = requested.substr(first, last - first + 1);
        }
        if (requested.empty() || requested == "$null" ||
            requested == "null" || requested == "undefined" ||
            requested == "none") requested = "auto";
        // Unsupported operator values fail closed to the stable RTP sender,
        // never to an unqualified hardware codec.
        const bool experimental = getenvString("HI5_EXPERIMENTAL_LIVE_CODECS") == "1";
        const bool recognized = (experimental &&
            (requested == "av1_hw" || requested == "h265_hw")) ||
            requested == "auto" || requested == "vp8" ||
            requested == "vp9" || requested == "vp9_hw" ||
            requested == "vp9_sw" || requested == "h264" ||
            requested == "h264_hw" || requested == "h264_sw";
        if (!recognized) requested = "vp8";

        CodecDecision decision;
        decision.requestedMode = requested;

        if (requested == "av1_hw" || requested == "h265_hw") {
            decision.selectedCodec = requested == "av1_hw" ? "av1" : "h265";
            decision.encoder = "mediafoundation-experimental";
            decision.fps = 20;
            decision.bitrateKbps = 4500;
            decision.hardware = true; // preference; actual encoder must be probed
            decision.stable = false;
            decision.reason = "Explicit isolated RTP experiment, never automatic";
            return decision;
        }

        if (requested == "vp8") {
            decision.selectedCodec = "vp8";
            decision.encoder = "libvpx";
            decision.fps = 20;
            decision.bitrateKbps = 16000;
            decision.hardware = false;
            decision.stable = true;
            decision.reason = "VP8 explicitly requested";
            return decision;
        }

        if (requested == "vp9" || requested == "vp9_hw") {
            decision.selectedCodec = "vp9";
            decision.encoder = "mediafoundation-vp9-hw-fallback-sw";
            decision.fps = 20;
            decision.bitrateKbps = 6000;
            decision.hardware = true;
            decision.stable = true;
            decision.reason = "VP9 requested; hardware attempted with software fallback";
            return decision;
        }

        if (requested == "vp9_sw") {
            decision.selectedCodec = "vp9";
            decision.encoder = "mediafoundation-vp9-sw";
            decision.fps = 20;
            decision.bitrateKbps = 6000;
            decision.hardware = false;
            decision.stable = true;
            decision.reason = "Software VP9 explicitly requested";
            return decision;
        }

        if (requested == "h264_sw") {
            decision.selectedCodec = "h264";
            decision.encoder = "mediafoundation-h264-sw";
            decision.fps = 20;
            decision.bitrateKbps = 6000;
            decision.hardware = false;
            decision.stable = true;
            decision.reason = "Software H.264 explicitly requested";
            return decision;
        }

        if (requested == "h264_hw" || requested == "h264") {
            decision.selectedCodec = "h264";
            decision.encoder = "mediafoundation-h264-hw-fallback-sw";
            decision.fps = 20;
            decision.bitrateKbps = 6000;
            decision.hardware = true;
            decision.stable = true;
            decision.reason = "Hardware H.264 requested; software H.264 fallback enabled";
            return decision;
        }

        decision.selectedCodec = "h264";
        decision.encoder = "mediafoundation-h264-hw-fallback-sw";
        decision.fps = 20;
        decision.bitrateKbps = 6000;
        decision.hardware = true;
        decision.stable = true;
        decision.reason = "Auto selected H.264 with hardware attempt and software fallback";

        return decision;
    }

} // namespace hi5