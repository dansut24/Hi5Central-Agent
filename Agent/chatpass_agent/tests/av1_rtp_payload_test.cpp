#include "../src/encoding/av1_rtp_payload_experimental.h"
#include "../src/encoding/av1_mf_output_adapter.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
int main() {
    using namespace hi5;
    Av1RtpPayloads out;
    std::string why;
    // Sequence header OBU type 1, size=3 and a frame OBU type 6, size=2.
    const std::vector<uint8_t> unit{0x0a,3,0x11,0x22,0x33,0x32,2,0x44,0x55};
    assert(BuildAv1RtpPayloads(unit,1200,out,&why));
    assert(out.hasSequenceHeader && out.packets.size()==2);
    assert(out.packets[0]==(std::vector<uint8_t>{0x18,0x08,0x11,0x22,0x33}));
    assert(out.packets[1]==(std::vector<uint8_t>{0x10,0x30,0x44,0x55}));
    // Oversized frame fragments carry Z/Y and never fake N on continuations.
    std::vector<uint8_t> large{0x32,0x20};
    large.insert(large.end(),32,0xAB);
    assert(BuildAv1RtpPayloads(large,12,out,&why));
    assert(out.packets.size()==3); // OBU 1-byte header + 32 data / 11 bytes
    assert(out.packets.front()[0]==0x50);
    assert(out.packets.back()[0]==0x90);
    for(const auto& p:out.packets)assert(p.size()<=12);
    // Ignore temporal delimiter without introducing decoder-visible packets.
    const std::vector<uint8_t> withDelimiter{0x12,0,0x32,1,0x88};
    assert(BuildAv1RtpPayloads(withDelimiter,1200,out,&why));
    assert(out.packets.size()==1);
    assert(out.packets[0]==(std::vector<uint8_t>{0x10,0x30,0x88}));
    // OBU extension is retained but has_size_field must be cleared.
    const std::vector<uint8_t> extended{0x36,0x08,1,0x99};
    assert(BuildAv1RtpPayloads(extended,1200,out,&why));
    assert(out.packets[0]==(std::vector<uint8_t>{0x10,0x34,0x08,0x99}));
    for(const auto& invalid:std::vector<std::vector<uint8_t>>{
        {}, {0x32}, {0x30,0x02,0x00}, {0x32,0x03,0x01},
        {0xB2,0x01,0x00}, {0x32,0x80}, {0x32,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}
    }) {
        assert(!BuildAv1RtpPayloads(invalid,1200,out,&why));
        assert(out.packets.empty());
    }
    assert(!BuildAv1RtpPayloads(unit,1201,out,&why));
    // Intel Media Foundation output can use Annex B temporal units. Convert
    // only fully validated, bounded Annex B lengths before RTP packetizing.
    const std::vector<uint8_t> annex{9,8,3,0x08,0x11,0x22,3,0x30,0x44,0x55};
    assert(BuildAv1MfRtpPayloads(annex,1200,out,&why));
    assert(out.hasSequenceHeader && out.packets.size()==2);
    assert(out.packets[0]==(std::vector<uint8_t>{0x18,0x08,0x11,0x22}));
    assert(out.packets[1]==(std::vector<uint8_t>{0x10,0x30,0x44,0x55}));
    const std::vector<uint8_t> annexWithSizes{11,10,4,0x0a,2,0x11,0x22,4,0x32,2,0x44,0x55};
    assert(BuildAv1MfRtpPayloads(annexWithSizes,1200,out,&why));
    assert(out.packets.size()==2 && out.hasSequenceHeader);
    assert(BuildAv1MfRtpPayloads(unit,1200,out,&why));
    assert(out.hasSequenceHeader && out.packets.size()==2);
    for(const auto& malformed : std::vector<std::vector<uint8_t>>{
        {0},{0x80},{9,8,3,0x08,0x11},
        {9,8,3,0x08,0x11,0x22,3,0x30,0x44,0x55,0},
        {9,8,3,0x80,0x11,0x22,3,0x30,0x44,0x55},
        {9,8,4,0x0a,3,0x11,0x22,3,0x30,0x44,0x55},
    }) {
        assert(!BuildAv1MfRtpPayloads(malformed,1200,out,&why));
        assert(out.packets.empty());
    }
    std::cout<<"AV1 RTP unit and fragment qualification checks passed\n";
}
