#include "../src/encoding/av1_ivf_experimental.h"
#include "../src/encoding/av1_rtp_payload_experimental.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using Byte = uint8_t;
static void le(std::vector<Byte>& bytes,uint64_t value,int count) {
    for(int i=0;i<count;++i){bytes.push_back(static_cast<Byte>(value));value>>=8;}
}
static std::vector<Byte> frame(uint64_t pts,const std::vector<Byte>& obu) {
    std::vector<Byte> b;
    le(b,obu.size(),4);
    le(b,pts,8);
    b.insert(b.end(),obu.begin(),obu.end());
    return b;
}
static std::vector<Byte> withHeader(const std::vector<Byte>& b) {
    std::vector<Byte> h{'D','K','I','F'};
    le(h,0,2);le(h,32,2);
    h.insert(h.end(),{'A','V','0','1'});
    le(h,1280,2);le(h,720,2);
    le(h,1000,4);le(h,1,4);le(h,0,4);le(h,0,4);
    assert(h.size()==32);
    h.insert(h.end(),b.begin(),b.end());
    return h;
}
int main(){
    hi5::ExperimentalAv1IvfExtractor e;
    std::vector<Byte> obu,first{0x0a,0x01,0x00},next{0x32,0x01,0xff};
    std::string error;
    auto firstWrapped=withHeader(frame(0,first));
    assert(e.unwrap(firstWrapped,obu,&error)&&obu==first);
    hi5::Av1RtpPayloads packets;
    assert(hi5::BuildAv1RtpPayloads(obu,1100,packets,&error));
    assert(packets.hasSequenceHeader&&!packets.packets.empty());
    auto nextWrapped=frame(33,next);
    assert(e.unwrap(nextWrapped,obu,&error)&&obu==next);
    assert(hi5::BuildAv1RtpPayloads(obu,1100,packets,&error));
    assert(!packets.packets.empty());
    assert(!e.unwrap(nextWrapped,obu,&error));
    assert(obu.empty());
    auto corrupted=withHeader(frame(0,first));
    corrupted[8]='H';
    hi5::ExperimentalAv1IvfExtractor bad;
    assert(!bad.unwrap(corrupted,obu,&error));
    corrupted=withHeader(frame(0,first));
    corrupted[6]=64;assert(!bad.unwrap(corrupted,obu,&error));
    corrupted=withHeader(frame(0,first));
    corrupted[32]=255;assert(!bad.unwrap(corrupted,obu,&error));
    corrupted=withHeader(frame(0,first));
    corrupted.pop_back();assert(!bad.unwrap(corrupted,obu,&error));
    assert(!bad.unwrap(frame(0,first),obu,&error)); // no DKIF allowed first
    std::cout<<"AV1_IVF_BOUNDED_EXTRACTION_TESTS_PASSED\n";
}
