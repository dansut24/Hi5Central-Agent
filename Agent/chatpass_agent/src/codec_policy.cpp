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
        const std::string requestedRaw = getenvString("HI5_CODEC");
        const std::string requested = lower(requestedRaw);

        CodecDecision decision;
        decision.requestedMode = requested.empty() ? "auto" : requested;

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