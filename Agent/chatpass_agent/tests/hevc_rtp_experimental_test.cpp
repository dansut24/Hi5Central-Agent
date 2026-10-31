#include "hevc_rtp_experimental.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
static void append(std::vector<uint8_t>& b,const std::vector<uint8_t>& nal) {
    b.insert(b.end(),{0,0,0,1});
    b.insert(b.end(),nal.begin(),nal.end());
}
int main() {
    std::vector<uint8_t> b;
    append(b,{0x40,0x01,0x10}); // VPS 32
    append(b,{0x42,0x01,0x20}); // SPS 33
    append(b,{0x44,0x01,0x30}); // PPS 34
    std::vector<uint8_t> idr(3700,0x55);
    idr[0]=0x26;idr[1]=0x01; // IDR type 19
    append(b,idr);
    hi5::ExperimentalHevcPackets p;
    std::string err;
    assert(hi5::PacketizeHevcAnnexB(b,1100,p,&err));
    assert(p.hasVps && p.hasSps && p.hasPps && p.hasRandomAccess);
    assert(p.nalCount==4 && p.packets.size()>=7);
    assert(p.packets[0][0]==0x40);
    const auto& firstFu=p.packets[3];
    assert(firstFu.size()<=1100 && ((firstFu[0]>>1)&0x3f)==49);
    assert((firstFu[2]&0x80)!=0 && (firstFu[2]&0x40)==0);
    const auto& lastFu=p.packets.back();
    assert((lastFu[2]&0x40)!=0 && (lastFu[2]&0x80)==0);
    assert((lastFu[2]&0x3f)==19);
    std::vector<uint8_t> invalid={0,0,0,1,0x40,0};
    assert(!hi5::PacketizeHevcAnnexB(invalid,1100,p,&err));
    assert(p.packets.empty());
    invalid={0,0,0,1,0x40,1,0x11};
    assert(!hi5::PacketizeHevcAnnexB(invalid,7,p,&err));
    invalid={0,0,0,1,0x60,1,0x11}; // aggregation NAL type48
    assert(!hi5::PacketizeHevcAnnexB(invalid,1100,p,&err));
    invalid={0,0,0,1,0x40,1,0x11,0,0,1}; // truncated next start
    assert(!hi5::PacketizeHevcAnnexB(invalid,1100,p,&err));
    std::cout<<"HEVC_ANNEXB_RFC7798_PACKETS_TESTS_PASSED\n";
}
