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
            decision.fps = 30;
            decision.bitrateKbps = 16000;
            decision.hardware = false;
            decision.stable = true;
            decision.reason = "VP8 explicitly requested";
            return decision;
        }

        if (requested == "av1" || requested == "av1_hw" || requested == "av1_sw") {
            decision.selectedCodec = "av1";
            decision.encoder = requested == "av1_sw" ? "mediafoundation-av1-sw" : "mediafoundation-av1-hw-fallback-sw";
            decision.fps = 20; decision.bitrateKbps = 6000;
            decision.hardware = requested != "av1_sw"; decision.stable = true;
            decision.reason = "AV1 requested; endpoint capability and Viewer negotiation validated per session";
            return decision;
        }

        if (requested == "vp9" || requested == "vp9_hw") {
            decision.selectedCodec = "vp9";
            decision.encoder = "mediafoundation-vp9-hw-fallback-libvpx";
            decision.fps = 20;
            decision.bitrateKbps = 6000;
            decision.hardware = true;
            decision.stable = true;
            decision.reason = "VP9 requested; hardware attempted with software fallback";
            return decision;
        }

        if (requested == "vp9_sw") {
            decision.selectedCodec = "vp9";
            decision.encoder = "libvpx-vp9";
            decision.fps = 20;
            decision.bitrateKbps = 6000;
            decision.hardware = false;
            decision.stable = true;
            decision.reason = "Software VP9 explicitly requested";
            return decision;
        }

        if (requested == "h265" || requested == "h265_hw" || requested == "h265_sw") {
            decision.selectedCodec = "h265";
            decision.encoder = requested == "h265_sw" ? "mediafoundation-h265-sw" : "mediafoundation-h265-hw-fallback-sw";
            decision.fps = 20; decision.bitrateKbps = 6000;
            decision.hardware = requested != "h265_sw"; decision.stable = true;
            decision.reason = "H.265 requested; Viewer SDP support required";
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

        decision.selectedCodec = "auto";
        decision.encoder = "stable-vp8-webrtc";
        // Production Auto currently negotiates the validated VP8 path only.
        // Keep the service ceiling interaction-ready; the streamer/encoder still
        // fall back to 1-2 FPS when idle and use 24 FPS for ordinary activity.
        decision.fps = 30;
        decision.bitrateKbps = 8000;
        decision.hardware = false;
        decision.stable = true;
        decision.reason = "Production Auto uses the validated VP8 path with a 30 FPS interaction ceiling; experimental codecs require explicit opt-in";

        return decision;
    }

} // namespace hi5