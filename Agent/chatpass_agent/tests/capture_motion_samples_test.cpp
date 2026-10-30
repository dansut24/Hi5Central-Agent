#include "../src/capture_motion_samples.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
int main() {
    const int w=1280,h=720;
    std::vector<uint8_t> image(static_cast<size_t>(w)*h,65);
    hi5::SampledDesktopMotion sample;
    assert(sample.observe(image,w,h));
    assert(!sample.observe(image,w,h));
    for(int y=150;y<260;++y)
        for(int x=180;x<330;++x) image[static_cast<size_t>(y)*w+x]=210;
    assert(sample.observe(image,w,h));
    assert(!sample.observe(image,w,h));
    for(int y=10;y<20;++y)
        for(int x=10;x<20;++x) image[static_cast<size_t>(y)*w+x]=140;
    assert(!sample.observe(image,w,h)); // tiny cursor change
    assert(!sample.observe({},w,h)); // failed capture cannot trigger motion
    // The old 48x27/3-sample gate could discard ordinary 36x24 desktop
    // elements even when the screen had visibly changed.
    for(int y=380;y<404;++y)
        for(int x=600;x<636;++x)
            image[static_cast<size_t>(y)*w+x]=195;
    assert(sample.observe(image,w,h)); // small label/window transition
    assert(!sample.observe(image,w,h)); // static UI is still suppressed
    for(int y=200;y<610;++y)
        for(int x=240;x<1000;++x) image[static_cast<size_t>(y)*w+x]=90;
    assert(sample.observe(image,w,h));
    std::cout<<"GDI_LUMA_MOTION_TESTS_PASSED idle_static=1 active_scroll=1 cursor_filter=1\n";
}
