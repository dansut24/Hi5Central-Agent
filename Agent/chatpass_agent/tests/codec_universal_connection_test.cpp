#include "../src/codec_universal_connection.h"
#include "../src/codec_qualification_keyframe.h"
#include "../src/codec_auto_fps.h"
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
 // Real Lenovo 1.0.195 proof, and receiver-L4 compatibility.
 auto real=intel;
 real.backend="h264_hw";real.testedFrames=33;
 real.maxWidth=1728;real.maxHeight=1080;
 real.p95EncodeMs=23.9889;real.worstEncodeMs=42.3373;
 assert(chooseForConnection(true,"auto","vp8",viewer,{real},link,true).mode=="h264_hw");
 Viewer limited=viewer;limited.h264Level4=false;
 assert(enforceSharpH264ViewerLevel(limited,true));
 assert(!supports(limited,"h264"));
 assert(chooseForConnection(true,"auto","vp8",limited,{real},link,true).mode=="vp8");
 Viewer full=viewer;
 assert(!enforceSharpH264ViewerLevel(full,true));
 assert(supports(full,"h264"));
 Viewer nonSharp=viewer;nonSharp.h264Level4=false;
 assert(!enforceSharpH264ViewerLevel(nonSharp,false));
 assert(supports(nonSharp,"h264"));
 Viewer noH264Again=viewer;
 noH264Again.receiveCodecs={"vp8","vp9"};
 assert(!enforceSharpH264ViewerLevel(noH264Again,true));
 assert(chooseForConnection(true,"auto","vp8",noH264Again,{real},link,true).mode=="vp8");
 // Bounded one-shot keyframe works at idle frame rates without
 // introducing repeated recovery traffic or changing legacy policy.
 {
  using Clock=std::chrono::steady_clock;
  SingleQualificationKeyframe gate;
  const auto origin=Clock::now();
  assert(!gate.requestIfDue(false,origin));
  assert(!gate.requestIfDue(true,origin));
  assert(!gate.requestIfDue(true,origin+std::chrono::seconds(5)));
  assert(!gate.requestIfDue(true,origin+std::chrono::seconds(19)));
  assert(gate.requestIfDue(true,origin+std::chrono::seconds(20)));
  assert(!gate.requestIfDue(true,origin+std::chrono::seconds(21)));
  assert(!gate.requestIfDue(true,origin+std::chrono::seconds(200)));
  SingleQualificationKeyframe disabled;
  for(int seconds=0;seconds<120;++seconds)
   assert(!disabled.requestIfDue(false,origin+std::chrono::seconds(seconds)));
 }
 // Automatic FPS must never interpret a synthetic AV1 benchmark as
 // successful browser streaming, nor advertise 120 on a 60Hz receiver.
 {
  FpsQualification q;
  q.codec="av1"; q.testedFps=60; q.width=1280; q.height=720;
  q.encodedFrames=120; q.capturedFrames=120; q.decodedFrames=120;
  q.recoveredKeyframes=1; q.measuredEncodeP95Ms=9.60;
  q.measuredEncodeWorstMs=20.0; q.decodedFps=59;
  q.droppedFraction=0; q.lossFraction=0;
  assert(selectAutomaticFps("av1",{q}).targetFps==20);
  assert(selectAutomaticFps("av1",{},0,15).targetFps==15);
  q.actualViewer=true; q.rtpPacketizationVerified=true;
  assert(fpsProven(q));
  assert(selectAutomaticFps("av1",{q},60).targetFps==60);
  assert(selectAutomaticFps("av1",{q},60,45).targetFps==45);
  assert(selectAutomaticFps("h264",{q},60).targetFps==20);
  q.measuredEncodeWorstMs=35.0;
  assert(!fpsProven(q));
  assert(selectAutomaticFps("av1",{q},60).targetFps==20);
  q.measuredEncodeWorstMs=20.0;
  q.testedFps=120; q.encodedFrames=240; q.capturedFrames=240;
  q.decodedFrames=240; q.decodedFps=119;
  q.measuredEncodeP95Ms=4.0; q.measuredEncodeWorstMs=8.0;
  assert(fpsProven(q));
  assert(selectAutomaticFps("av1",{q},120,120).targetFps==120);
  assert(selectAutomaticFps("av1",{q},60,120).targetFps==60);
  assert(selectAutomaticFps("av1",{q},0,120).targetFps==60);
  q.lossFraction=0.05;
  assert(selectAutomaticFps("av1",{q},120,120).targetFps==20);
 }
 auto av1=intel;av1.codec="av1";
 assert(chooseForConnection(true,"auto","vp8",viewer,{av1},link,true).mode=="vp8");
 Viewer missing;
 assert(chooseForConnection(true,"auto","vp9_sw",missing,{intel},link,true).mode=="vp9_sw");
 std::cout<<"UNIVERSAL_CONNECTION_SELECTION_TESTS_PASSED\n";
}
