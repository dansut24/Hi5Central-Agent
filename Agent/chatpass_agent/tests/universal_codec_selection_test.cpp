#include "../src/codec_universal_selector.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
using namespace hi5::universal_codec;
static Candidate candidate(const std::string& codec, const std::string& backend,
                           bool hardware, int width = 1920, int height = 1080,
                           double p95 = 11.0, double worst = 30.0) {
    Candidate c;
    c.codec=codec; c.backend=backend; c.hardware=hardware;
    c.emittedFrames=c.decodedFrames=c.keyframeRecovery=c.packetizationVerified=true;
    c.testedFrames=60; c.maxWidth=width; c.maxHeight=height;
    c.p95EncodeMs=p95; c.worstEncodeMs=worst;
    return c;
}
int main() {
    Viewer v; v.receiveCodecs={"vp8","h264","vp9","av1"}; v.h264Level4=true;
    Link good; good.estimatedKbps=24000; good.rttMs=15;
    const auto vp8=candidate("vp8","libvpx",false,1920,1080,24,44);
    const auto h264Intel=candidate("h264","Intel QSV MFT",true,1920,1080,12,28);
    const auto h264Nvidia=candidate("h264","NVIDIA MFT",true,1920,1080,10,26);
    const auto h264Amd=candidate("h264","AMD MFT",true,1920,1080,13,29);
    for (const Candidate& hw : {h264Intel,h264Nvidia,h264Amd}) {
        Choice choice=select({vp8,hw},v,good);
        assert(choice.qualified && choice.codec=="h264" && choice.hardware);
        assert(choice.maxWidth==1920 && choice.maxHeight==1080);
    }
    // MFT enumeration alone, a broken IDR or an unverified RTP path can never
    // outrank the proven fallback.
    for (int failure=0;failure<4;++failure) {
        auto broken=h264Intel;
        if(failure==0) broken.testedFrames=0;
        if(failure==1) broken.emittedFrames=false;
        if(failure==2) broken.keyframeRecovery=false;
        if(failure==3) broken.packetizationVerified=false;
        Choice choice=select({vp8,broken},v,good);
        assert(choice.codec=="vp8");
    }
    Viewer old=v; old.h264Level4=false;
    auto legacy=select({h264Intel,vp8},old,good);
    assert(legacy.codec=="vp8" || (legacy.codec=="h264" && legacy.maxHeight<=720));
    Viewer noH264=v; noH264.receiveCodecs={"vp8","vp9"};
    assert(select({h264Intel,vp8},noH264,good).codec=="vp8");
    auto h265Proof=candidate("h265","Intel HEVC MFT",true,1280,720,10,22);
    Viewer hevcViewer;hevcViewer.receiveCodecs={"h265"};
    // The generic future scoring layer can understand HEVC, but the
    // connection gate above forbids it until the RTP sender is integrated.
    auto hevcScored=select({h265Proof},hevcViewer,good,1280,720);
    assert(hevcScored.qualified && hevcScored.codec=="h265");
        Viewer noCommon=v; noCommon.receiveCodecs={"h265"};
    assert(select({h264Intel,vp8},noCommon,good).codec.empty());
    Link limited;limited.estimatedKbps=1800;limited.rttMs=220;
    auto noFast=select({vp8,h264Intel},v,limited);
    assert(!noFast.qualified && noFast.codec=="vp8");
    CacheIdentity a{"1.0.170","Windows 11","Intel Xe|driver 123","policy-1",1000};
    CacheIdentity same=a;
    assert(cacheValid(a,same,1020));
    same.gpuAndDriver="Intel Xe|driver 124";
    assert(!cacheValid(a,same,1020));
    same=a;same.agentBuild="1.0.171";
    assert(!cacheValid(a,same,1020));
    assert(!cacheValid(a,a,1000 + 8*86400));
    const auto cache = std::filesystem::temp_directory_path() /
        "hi5central-universal-codec-qualification-test.cache";
    std::filesystem::remove(cache);
    assert(saveQualificationCache(cache,a,{h264Intel,vp8}));
    auto loaded=loadQualificationCache(cache,a,1100);
    assert(loaded.size()==2 && loaded[0].codec=="h264" && loaded[1].codec=="vp8");
    assert(loadQualificationCache(cache,a,1000+8*86400).empty());
    assert(loadQualificationCache(cache,same,1100).empty()); // new Agent build
    std::filesystem::remove(cache);
    std::cout<<"UNIVERSAL_CODEC_SELECTOR_TESTS_PASSED vendor_neutral=1 "
                "viewer_compatible=1 fail_safe=1 cache_invalidation=1\n";
    return 0;
}
