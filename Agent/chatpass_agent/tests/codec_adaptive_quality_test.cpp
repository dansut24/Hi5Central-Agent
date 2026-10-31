#include "../src/codec_adaptive_quality.h"
#include "../src/codec_fidelity_policy.h"
#include <cassert>
#include <iostream>
using namespace hi5::universal_codec;
int main(){
 AdaptiveQuality q;
 q.reset(6000);
 LinkSample invalid;invalid.lossFraction=.5;invalid.packetsSampled=0;
 assert(!q.observe(invalid,100).changed);
 LinkSample poor;poor.packetsSampled=100;poor.lossFraction=.05;
 assert(!q.observe(poor,101).changed);
 auto lower=q.observe(poor,102);
 assert(lower.changed && lower.tier==1 && lower.bitrateKbps==5400);
 assert(!q.observe(poor,103).changed); // no oscillation inside cooldown
 LinkSample healthy;healthy.packetsSampled=150;healthy.lossFraction=.002;
 for(int i=0;i<5;++i) assert(!q.observe(healthy,114+i).changed);
 auto raised=q.observe(healthy,119);
 assert(raised.changed && raised.tier==0 && raised.bitrateKbps==6000);
 q.reset(4500);
 assert(!q.observe(poor,200).changed);
 assert(q.observe(poor,201).bitrateKbps==4050);
 LinkSample forged=poor;forged.lossFraction=2;
 assert(!q.observe(forged,220).changed);
 FidelityCapabilities caps;caps.codec="av1";
 assert(!chooseFidelity(FidelityRequest::PixelPerfect,caps).pixelPerfect);
 caps.encoderLosslessVerified=caps.decoderLosslessProfileVerified=true;
 caps.sourceIsUnscaled=true;caps.preserves444OrRGB=true;
 caps.fullRangePreserved=true;
 assert(chooseFidelity(FidelityRequest::PixelPerfect,caps).pixelPerfect);
 caps.preserves444OrRGB=false; // I420 4:2:0 destroys original RGB detail
 assert(!chooseFidelity(FidelityRequest::PixelPerfect,caps).pixelPerfect);
 caps.codec="vp8";caps.preserves444OrRGB=true;
 assert(!chooseFidelity(FidelityRequest::PixelPerfect,caps).pixelPerfect);
 assert(chooseFidelity(FidelityRequest::HighQualityLossy,caps).highQualityLossy);
 std::cout<<"ADAPTIVE_QUALITY_AND_REAL_LOSSLESS_GATES_PASSED\n";
}
