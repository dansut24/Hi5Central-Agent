#include "../src/codec_universal_failover.h"
#include "../src/codec_universal_connection.h"
#include <cassert>
#include <iostream>
using namespace hi5::universal_codec;
static Candidate ready(const char* codec,const char* mode) {
 Candidate c{};c.codec=codec;c.backend=mode;
 c.emittedFrames=c.decodedFrames=c.keyframeRecovery=c.packetizationVerified=true;
 c.testedFrames=60;c.maxWidth=1920;c.maxHeight=1080;
 c.p95EncodeMs=10;c.worstEncodeMs=25;
 return c;
}
int main() {
 SessionCodecFailover gate;
 auto av1=ready("av1","av1_sw");
 auto h264=ready("h264","h264_sw");
 Viewer viewer; viewer.receiveCodecs={"vp8","av1","h264"};
 viewer.h264Level4=true;
 Link link; link.estimatedKbps=24000;
 gate.selected("a","av1_sw");
 assert(!gate.failure("b","av1","media-negotiation-timeout"));
 assert(!gate.failure("a","h264","media-negotiation-timeout"));
 assert(!gate.failure("a","av1","network-disconnected"));
 assert(gate.failure("a","av1","media-negotiation-timeout"));
 assert(gate.denied("a","av1"));
 const auto fallback=chooseForConnection(true,"auto","vp8",viewer,
   gate.allowed("a",{av1,h264}),link,true,true);
 assert(fallback.mode=="h264_sw");
 assert(gate.allowed("b",{av1,h264}).size()==2);
 gate.selected("a","h264_sw");
 assert(gate.failure("a","h264","decode-unavailable"));
 const auto next=chooseForConnection(true,"auto","vp8",viewer,
   gate.allowed("a",{av1,h264}),link,true,true);
 assert(next.mode=="vp8");
 assert(!gate.denied("a","vp8"));
 gate.selected("a","vp8");
 assert(gate.failure("a","vp8","media-negotiation-timeout"));
 assert(gate.denied("a","vp8")); // caller must abort rather than loop
 // Real Intel Agent 1.0.305 H265 proof must be demoted only for the
 // failed authenticated session. The same cache stays valid for others.
 auto hevc=ready("h265","h265_hw");hevc.hardware=true;
 hevc.testedFrames=244;hevc.maxWidth=1280;hevc.maxHeight=720;
 hevc.p95EncodeMs=22.7519;hevc.worstEncodeMs=36.6522;
 Viewer five;five.receiveCodecs={"vp8","h265"};
 gate.selected("intel-session","h265_hw");
 const auto beforeFailure=chooseForConnection(true,"auto","vp8",five,
   gate.allowed("intel-session",{hevc}),link,false,true,0);
 assert(beforeFailure.mode=="h265_hw" && beforeFailure.qualified);
 assert(!gate.failure("intel-session","av1","decode-unavailable"));
 assert(gate.failure("intel-session","h265","decode-unavailable"));
 const auto afterFailure=chooseForConnection(true,"auto","vp8",five,
   gate.allowed("intel-session",{hevc}),link,false,true,0);
 assert(afterFailure.mode=="vp8" && !afterFailure.qualified);
 const auto other=chooseForConnection(true,"auto","vp8",five,
   gate.allowed("other-healthy-session",{hevc}),link,false,true,0);
 assert(other.mode=="h265_hw" && other.qualified);
 std::cout<<"SESSION_CODEC_FAILOVER_ISOLATED_AND_BOUNDED_PASSED\n";
}
