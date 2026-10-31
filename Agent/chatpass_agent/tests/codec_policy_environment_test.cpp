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
int main() {
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
    check("gibberish", "vp8");
    setCodec(nullptr);
    std::cout << "CODEC_POLICY_ENV_NORMALIZATION_PASSED\n";
}
