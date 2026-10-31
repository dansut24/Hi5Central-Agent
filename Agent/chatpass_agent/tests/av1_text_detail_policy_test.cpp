#include "../src/encoding/av1_text_detail_policy.h"
#include <cassert>
#include <array>
#include <iostream>
using hi5::quality::ChooseAv1TextDetailProfile;
int main() {
 const auto normal=ChooseAv1TextDetailProfile(true,false,1920,1080,4500,6500);
 assert(normal.maxWidth==1280&&normal.maxHeight==720&&normal.bitrateKbps==4500&&!normal.enabled);
 const auto noRequest=ChooseAv1TextDetailProfile(true,true,0,0,4500,6500);
 assert(noRequest.maxWidth==1280&&noRequest.maxHeight==720&&!noRequest.enabled);
 for(const auto& dims : std::array<std::array<int,2>,5>{{{{1280,720}},{{-1,-1}},{{1920,720}},{{3840,2160}},{{1920,0}}}}) {
  const auto p=ChooseAv1TextDetailProfile(true,true,dims[0],dims[1],4500,6000);
  assert(!p.enabled&&p.maxWidth==1280&&p.maxHeight==720);
 }
 const auto hevc=ChooseAv1TextDetailProfile(false,true,1920,1080,4500,6500);
 assert(!hevc.enabled&&hevc.maxWidth==1280);
 const auto hi=ChooseAv1TextDetailProfile(true,true,1920,1080,4500,6000);
 assert(hi.enabled&&hi.maxWidth==1920&&hi.maxHeight==1080&&hi.bitrateKbps==6000);
 const auto capped=ChooseAv1TextDetailProfile(true,true,1920,1080,4500,20000);
 assert(capped.enabled&&capped.bitrateKbps==6500);
 const auto minimum=ChooseAv1TextDetailProfile(true,true,1920,1080,1200,0);
 assert(minimum.enabled&&minimum.bitrateKbps==4500);
 const auto baseline=ChooseAv1TextDetailProfile(true,false,-1,-1,1200,0);
 assert(baseline.bitrateKbps==1200 && !baseline.enabled);
 std::cout << "AV1_DETAIL_EXPLICIT_OPTIN_1080P_PRESERVE_DEFAULT_TESTS_PASSED\n";
}
