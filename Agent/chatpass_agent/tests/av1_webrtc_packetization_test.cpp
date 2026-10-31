#include "../src/encoding/av1_rtp_webrtc_compatible.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>
using namespace hi5;
static std::vector<uint8_t> build(const std::vector<std::vector<uint8_t>>& parts) {
 std::vector<uint8_t> r;
 for(const auto& p:parts){ r.push_back(p.front() | 0x02u); av1_rtp_detail::WriteLeb(r,p.size()-1);r.insert(r.end(),p.begin()+1,p.end()); }
 return r;
}
static std::vector<std::vector<uint8_t>> unpack(const Av1RtpPayloads& out){
 std::vector<std::vector<uint8_t>> result;
 bool prevContinued=false;
 for(const auto& pkt:out.packets){
  assert(pkt.size()>=2 && (pkt[0]&7)==0);
  bool continued=(pkt[0]&0x80u)!=0,fragmented=(pkt[0]&0x40u)!=0;
  assert(continued==prevContinued);prevContinued=fragmented;
  unsigned w=(pkt[0]>>4)&3;assert(w>=1 && w<=3);
  size_t at=1;
  for(unsigned i=0;i<w;++i){
   size_t n=0;
   if(i+1==w){n=pkt.size()-at;}
   else {uint32_t len=0;assert(Av1ReadLeb128(pkt,at,len));n=len;}
   assert(n>0 && n<=pkt.size()-at);
   bool continuation=i==0 && continued;
   if(continuation) {assert(!result.empty());result.back().insert(result.back().end(),pkt.begin()+at,pkt.begin()+at+n);}
   else result.emplace_back(pkt.begin()+at,pkt.begin()+at+n);
   at+=n;
  }
  assert(at==pkt.size());
 }
 assert(!prevContinued);
 return result;
}
int main(){
 std::string why;Av1RtpPayloads out;
 const auto small=build({{0x08,1,2,3},{0x30,0xa1,0xa2}});
 assert(BuildAv1WebRtcCompatibleRtpPayloads(small,1200,true,out,&why));
 assert(out.hasSequenceHeader && out.packets.size()==1);
 assert(out.packets[0]==(std::vector<uint8_t>{0x28,4,0x08,1,2,3,0x30,0xa1,0xa2}));
 assert(BuildAv1WebRtcCompatibleRtpPayloads(small,1200,false,out,&why));
 assert(out.packets[0][0]==0x20); // N bit never on inter-frame.
 auto largeFrame=std::vector<uint8_t>(42000,0x5a);largeFrame[0]=0x30;
 const auto huge=build({{0x08,0x55,0x66},largeFrame});
 assert(BuildAv1WebRtcCompatibleRtpPayloads(huge,1200,true,out,&why));
 assert(out.packets.size()>30 && (out.packets[0][0]&0x70)==0x60);
 assert((out.packets[0][0]&0x08)!=0 && (out.packets[1][0]&0x80)!=0);
 assert((out.packets.back()[0]&0x40)==0);
 auto units=unpack(out);assert(units.size()==2 && units[0]==(std::vector<uint8_t>{0x08,0x55,0x66}) && units[1]==largeFrame);
 assert(BuildAv1WebRtcCompatibleRtpPayloads(build({{0x10},{0x40,0x00},{0x30,1,2}}),1200,false,out,&why));
 assert(out.packets.size()==1 && out.packets[0]==(std::vector<uint8_t>{0x10,0x30,1,2})); // skips temporal delimiter, tile list
 assert(BuildAv1WebRtcCompatibleRtpPayloads(build({{0x08,1},{0x28,2},{0x18,3},{0x20,4}}),1200,true,out,&why));
 assert(unpack(out).size()==4);
 for(const auto& invalid:std::vector<std::vector<uint8_t>>{{},{0x32},{0x32,0x82},{0xb2,0},{0x08,1},{0x30,1},{0x32,0xff}}) {
  assert(!BuildAv1WebRtcCompatibleRtpPayloads(invalid,1200,false,out,&why));assert(out.packets.empty());
 }
 std::mt19937 rng(9364);
 for(int test=0;test<2000;++test){
  size_t size=static_cast<size_t>(rng()%14000+1); std::vector<uint8_t> frame(size+1,0x33);frame[0]=0x30;
  for(size_t i=1;i<frame.size();++i)frame[i]=static_cast<uint8_t>(rng());
  std::vector<std::vector<uint8_t>> elements{{0x08,0x42,0x43}};
  if(test%2) elements.push_back({0x28,0x44,0x45,0x46});
  elements.push_back(frame);
  size_t maxPayload=static_cast<size_t>(rng()%1150+50);
  assert(BuildAv1WebRtcCompatibleRtpPayloads(build(elements),maxPayload,true,out,&why));
  assert(out.packets.size()<=4096 && out.hasSequenceHeader);
  for(const auto& p:out.packets)assert(p.size()<=maxPayload && p.size()>=2);
  assert(unpack(out)==elements);
 }
 std::cout<<"RFC9364_WEBRTC_COMPAT_AGGREGATION_AND_ROUNDTRIP_2000_PASSED\n";
}
