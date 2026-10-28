#include "../src/encoding/sustained_encoder_health.h"

#include <cassert>
#include <iostream>

int main() {
    // Healthy endpoint retains VP9 even if one expensive keyframe occurs.
    SustainedEncoderHealth healthy;
    assert(!healthy.observeWindow(90, 90.0 * 12.0, 110.0, 20));
    assert(!healthy.observeWindow(90, 90.0 * 13.0, 90.0, 20));

    // One overloaded window is not enough; two consecutive overloaded
    // windows with at least eight real frames are required.
    SustainedEncoderHealth slow;
    assert(!slow.observeWindow(28, 28.0 * 75.0, 140.0, 20));
    assert(slow.observeWindow(30, 30.0 * 81.0, 170.0, 20));
    assert(!slow.observeWindow(35, 35.0 * 90.0, 190.0, 20));

    // An idle desktop does not provide sufficient motion evidence.
    SustainedEncoderHealth idle;
    assert(!idle.observeWindow(3, 3.0 * 200.0, 210.0, 20));
    assert(!idle.observeWindow(3, 3.0 * 220.0, 230.0, 20));
    assert(!idle.observeWindow(20, 20.0 * 90.0, 170.0, 2));

    // A recovered window resets the consecutive-overload requirement.
    SustainedEncoderHealth recovered;
    assert(!recovered.observeWindow(20, 20.0 * 85.0, 120.0, 20));
    assert(!recovered.observeWindow(20, 20.0 * 10.0, 12.0, 20));
    assert(!recovered.observeWindow(20, 20.0 * 85.0, 120.0, 20));
    assert(recovered.observeWindow(20, 20.0 * 85.0, 120.0, 20));

    std::cout << "sustained encoder health policy: 4/4 passed\n";
}
