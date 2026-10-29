#include "../src/codec_universal_connection.h"
#include <cassert>
#include <iostream>
using namespace hi5::universal_codec;
int main() {
 Viewer viewer;viewer.receiveCodecs={"vp8","h264","vp9"};viewer.h264Level4=true;
 Candidate intel{};intel.codec="h264";intel.backend="Intel Media Foundation";
 intel.hardware=true;intel.emittedFrames=intel.decodedFrames=
 intel.keyframeRecovery=intel.packetizationVerified=true;
 intel.testedFrames=80;intel.maxWidth=1920;intel.maxHeight=1080;
 intel.p95EncodeMs=12;intel.worstEncodeMs=28;
 Link link;link.estimatedKbps=20000;
 auto c=chooseForConnection(true,"auto","vp8",viewer,{intel},link,true);
 assert(c.mode=="h264_hw" && c.qualified);
 auto amd=intel;amd.backend="AMD Media Foundation";
 assert(chooseForConnection(true,"auto","vp8",viewer,{amd},link,true).mode=="h264_hw");
 auto nvidia=intel;nvidia.backend="NVIDIA Media Foundation";
 assert(chooseForConnection(true,"auto","vp8",viewer,{nvidia},link,true).mode=="h264_hw");
 intel.decodedFrames=false;
 assert(chooseForConnection(true,"auto","vp8",viewer,{intel},link,true).mode=="vp8");
 intel.decodedFrames=true;
 assert(chooseForConnection(true,"auto","vp8",viewer,{intel},link,false).mode=="vp8");
 assert(chooseForConnection(false,"auto","vp8",viewer,{intel},link,true).mode=="vp8");
 assert(chooseForConnection(true,"h264_sw","vp8",viewer,{},link,true).mode=="h264_sw");
 Viewer noH264;noH264.receiveCodecs={"vp8","vp9"};
 assert(chooseForConnection(true,"auto","vp8",noH264,{intel},link,true).mode=="vp8");
 assert(chooseForConnection(true,"auto","vp9_sw",noH264,{},link,true).mode=="vp9_sw");
 Viewer noCommon;noCommon.receiveCodecs={"h264"};
 auto blocked=chooseForConnection(true,"auto","vp8",noCommon,{},link,true);
 assert(!blocked.compatible && blocked.mode.empty());
 auto av1=intel;av1.codec="av1";
 assert(chooseForConnection(true,"auto","vp8",viewer,{av1},link,true).mode=="vp8");
 Viewer missing;
 assert(chooseForConnection(true,"auto","vp9_sw",missing,{intel},link,true).mode=="vp9_sw");
 std::cout<<"UNIVERSAL_CONNECTION_SELECTION_TESTS_PASSED\n";
}
