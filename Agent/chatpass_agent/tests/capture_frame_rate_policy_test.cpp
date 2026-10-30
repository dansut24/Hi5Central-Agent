#include "../src/capture_frame_rate_policy.h"
#include "../src/codec_auto_fps.h"
#include <cassert>
#include <chrono>
#include <iostream>
int main() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    auto origin = Clock::now();
    auto old = origin - 60s;
    auto decide = [&](Clock::time_point now, Clock::time_point input,
                      Clock::time_point changed, bool newFrame, int maxFps = 60) {
        return hi5::SelectCaptureFrameRate(now,input,changed,newFrame,
            maxFps,20,30,2,900ms,300ms);
    };
    assert(decide(origin,old,old,false).targetFps == 2);
    auto first = decide(origin,old,old,true);
    assert(first.targetFps == 20 && first.mode == 1);
    auto next = decide(origin+500ms,old,origin,false);
    assert(next.targetFps == 20 && next.mode == 1);
    assert(decide(origin+1100ms,old,origin,false).targetFps == 2);
    auto mouse = decide(origin+500ms,origin,origin,false);
    assert(mouse.targetFps == 30 && mouse.mode == 2);
    assert(decide(origin+500ms,old,origin,true,15).targetFps == 15);
    assert(decide(origin+500ms,old,origin,true,60).targetFps == 20);
    assert(hi5::SelectManualSessionCeilingFps(60) == 60);
    assert(hi5::SelectManualSessionCeilingFps(55) == 45);
    assert(hi5::SelectManualSessionCeilingFps(40) == 30);
    assert(hi5::SelectManualSessionCeilingFps(20) == 15);
    assert(hi5::SelectManualSessionCeilingFps(15) == 15);
    // Explicit viewer rates must reach the capture helper (not just SDP),
    // while Automatic and static desktop retain low-CPU behaviour.
    for (int requested : {15, 30, 45, 60}) {
        assert(hi5::universal_codec::manualFpsAllowed(requested));
        auto manualChanged = hi5::ApplyManualCaptureFps(first, requested, 60);
        assert(manualChanged.targetFps == requested && manualChanged.mode == 1);
        auto manualInput = hi5::ApplyManualCaptureFps(mouse, requested, 60);
        assert(manualInput.targetFps == requested && manualInput.mode == 2);
        assert(hi5::ApplyManualCaptureFps(first, requested, 30).targetFps ==
               std::min(requested, 30));
    }
    assert(!hi5::universal_codec::manualFpsAllowed(0));
    assert(!hi5::universal_codec::manualFpsAllowed(20));
    assert(!hi5::universal_codec::manualFpsAllowed(120));
    assert(hi5::ApplyManualCaptureFps(first, 0, 60).targetFps == 20);
    assert(hi5::ApplyManualCaptureFps(first, 120, 60).targetFps == 20);
    assert(hi5::ApplyManualCaptureFps(decide(origin,old,old,false), 60, 60).targetFps == 2);
    // Regression: no changed pixels over two idle periods -> low CPU idle.
    assert(decide(origin+2500ms,old,origin,false).mode == 0);
    std::cout << "CAPTURE_RATE_POLICY_TESTS_PASSED idle=2 changed=20 input=30 no_idle_lock=1\n";
}
