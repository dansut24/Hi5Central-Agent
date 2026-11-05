#include "../src/encoding/hevc_native_detail_policy.h"
#include <cassert>
#include <iostream>
using hi5::quality::ChooseHevcNativeDetailProfile;

int main() {
    // Unchanged default: existing qualified AV1/H.264/VP8/VP9 pipelines
    // and all existing HEVC sessions remain on the 720p/20fps baseline.
    auto base = ChooseHevcNativeDetailProfile(false, 0, 0, 1920, 1080,
                                               60, 60, 4500, 6500);
    assert(!base.enabled && base.maxWidth == 1280 && base.maxHeight == 720);
    assert(base.encoderFps == 20 && base.bitrateKbps == 4500);
    // Native and explicit 1080p get actual 1080p: no unnecessary upsample.
    auto native = ChooseHevcNativeDetailProfile(true, 0, 0, 1920, 1080,
                                                 60, 0, 4500, 0);
    assert(native.enabled && native.maxWidth == 1920 &&
           native.maxHeight == 1080 && native.bitrateKbps == 6000);
    assert(native.encoderFps == 20); // auto is evidence-gated
    auto p1080 = ChooseHevcNativeDetailProfile(true, 1920, 1080, 2560, 1600,
                                                60, 30, 4500, 5800);
    assert(p1080.enabled && p1080.maxWidth == 1920 &&
           p1080.maxHeight == 1080);
    assert(p1080.encoderFps == 30 && p1080.bitrateKbps == 5800);
    auto highFps = ChooseHevcNativeDetailProfile(true, 0, 0, 1920, 1080,
                                                   45, 60, 4500, 0);
    assert(highFps.encoderFps == 45); // never exceed sender's real cap
    // A 720p Viewer choice and a 720p source must not be upscaled.
    auto explicit720 = ChooseHevcNativeDetailProfile(true, 1280, 720, 1920,
                                                       1080, 60, 60, 4500, 0);
    assert(!explicit720.enabled && explicit720.maxHeight == 720);
    auto small = ChooseHevcNativeDetailProfile(true, 0, 0, 1280, 720,
                                                60, 60, 4500, 0);
    assert(!small.enabled && small.maxWidth == 1280);
    // Unknown/untrusted or partial preference fails back to conservative.
    auto odd = ChooseHevcNativeDetailProfile(true, 1920, -1, 1920, 1080,
                                              60, 30, 4500, 0);
    assert(!odd.enabled);
    // Never force premium bitrate when Viewer has supplied a low safe target.
    auto limited = ChooseHevcNativeDetailProfile(true, 0, 0, 1920, 1080,
                                                  30, 0, 4500, 2200);
    assert(limited.enabled && limited.bitrateKbps == 2200);
    auto bounded = ChooseHevcNativeDetailProfile(true, -1, -1, 1920, 1080,
                                                  60, 60, 99999, 99999);
    assert(bounded.bitrateKbps == 8000 && bounded.encoderFps == 60);
    // An encoder initiated at 20 FPS must honour an actual Viewer switch
    // to 30/45/60, but MUST NOT churn on every subsequent frame.
    assert(hi5::quality::ShouldRestartHevcForFps(true, true, 30, 20));
    assert(hi5::quality::ShouldRestartHevcForFps(true, true, 45, 30));
    assert(hi5::quality::ShouldRestartHevcForFps(true, true, 60, 45));
    assert(hi5::quality::ShouldRestartHevcForFps(true, true, 20, 60));
    assert(!hi5::quality::ShouldRestartHevcForFps(true, true, 30, 30));
    assert(!hi5::quality::ShouldRestartHevcForFps(true, false, 30, 20));
    assert(!hi5::quality::ShouldRestartHevcForFps(false, true, 30, 20));
    assert(!hi5::quality::ShouldRestartHevcForFps(true, true, 0, 20));
    assert(!hi5::quality::ShouldRestartHevcForFps(true, true, 20, 0));
    std::cout << "HEVC_NATIVE_1080P_POLICY_PASSED baseline=720p "
                 "native=1080p bitrate=bounded manual_fps=bounded "
                 "auto_fps=20 viewer720p=honoured\n";
    return 0;
}
