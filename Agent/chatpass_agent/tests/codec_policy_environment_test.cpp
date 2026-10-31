#include "../src/codec_policy.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

static void setCodec(const char* value) {
#ifdef _WIN32
    assert(_putenv_s("HI5_CODEC", value ? value : "") == 0);
#else
    if (value) assert(setenv("HI5_CODEC", value, 1) == 0);
    else assert(unsetenv("HI5_CODEC") == 0);
#endif
}
static void check(const char* input, const char* expected) {
    setCodec(input);
    const auto decision = hi5::SelectCodecPolicy();
    assert(decision.requestedMode == expected);
}
static void setLiveExperiment(bool enabled) {
#ifdef _WIN32
    assert(_putenv_s("HI5_EXPERIMENTAL_LIVE_CODECS", enabled ? "1" : "") == 0);
#else
    if (enabled) assert(setenv("HI5_EXPERIMENTAL_LIVE_CODECS", "1", 1) == 0);
    else assert(unsetenv("HI5_EXPERIMENTAL_LIVE_CODECS") == 0);
#endif
}
int main() {
    setLiveExperiment(false);
    check(nullptr, "auto");
    check("", "auto");
    check("$null", "auto");
    check(" $NULL ", "auto");
    check("null", "auto");
    check("undefined", "auto");
    check("auto", "auto");
    check(" VP9_HW ", "vp9_hw");
    check("h264_hw", "h264_hw");
    check("h264_sw", "h264_sw");
    check("vp8", "vp8");
    check("av1", "vp8"); // Unsupported live AV1 must not be forced.
    check("av1_hw", "vp8");
    check("av1_sw", "vp8"); // Even linked libaom requires explicit experiment flag.
    check("h265_hw", "vp8");
    setLiveExperiment(true);
    check("av1_hw", "av1_hw");
#ifdef HI5_ENABLE_LIBAOM_AV1
    check("av1_sw", "av1_sw");
    assert(hi5::SelectCodecPolicy().encoder == "libaom-realtime-experimental");
    assert(!hi5::SelectCodecPolicy().hardware);
    assert(!hi5::SelectCodecPolicy().stable);
#else
    check("av1_sw", "vp8"); // Unlinked targets must never offer software AV1.
#endif
    check(" H265_HW ", "h265_hw");
    assert(!hi5::SelectCodecPolicy().stable);
    setLiveExperiment(false);
    check("h265_hw", "vp8");
    check("gibberish", "vp8");
    setCodec(nullptr);
    std::cout << "CODEC_POLICY_ENV_NORMALIZATION_PASSED\n";
}
