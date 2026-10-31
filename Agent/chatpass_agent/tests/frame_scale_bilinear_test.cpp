#include "../src/frame_scale_bilinear.h"
#include "../src/h264_scaling_policy.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using hi5::quality::ScalePlaneBilinear;

int main() {
    using hi5::quality::UseFilteredH264Scaling;
    assert(!UseFilteredH264Scaling(false, false, false));
    assert(!UseFilteredH264Scaling(false, true, false));
    assert(UseFilteredH264Scaling(false, false, true));
    assert(UseFilteredH264Scaling(true, true, false));
    assert(!UseFilteredH264Scaling(true, false, true));
    std::vector<std::uint8_t> src(16);
    for (int y=0;y<4;++y) for (int x=0;x<4;++x)
        src[static_cast<std::size_t>(y)*4+x] = static_cast<std::uint8_t>(x*32+y*24);
    std::vector<std::uint8_t> dst;
    ScalePlaneBilinear(src,4,4,dst,2,2);
    assert(dst.size()==4);
    // Pixel-centred 2x downsample averages adjacent source pixels;
    // nearest-neighbour would produce src[0], src[2], src[8], src[10].
    assert(dst[0]==28 && dst[1]==92 && dst[2]==76 && dst[3]==140);
    std::vector<std::uint8_t> same;
    ScalePlaneBilinear(src,4,4,same,4,4);
    assert(same==src);
    std::vector<std::uint8_t> solid(8*8,117);
    ScalePlaneBilinear(solid,8,8,dst,6,6);
    assert(dst.size()==36);
    for (auto pixel:dst) assert(pixel==117);
    ScalePlaneBilinear({},4,4,dst,2,2);
    assert(dst.empty());
    // Subsampled I420 chroma planes use identical edge-clamped sampling.
    std::vector<std::uint8_t> chroma={12,34,56,78};
    ScalePlaneBilinear(chroma,2,2,dst,1,1);
    assert(dst.size()==1 && dst[0]==45);
    std::cout<<"H264_FILTERED_TEXT_SCALING_TESTS_PASSED\n";
}
