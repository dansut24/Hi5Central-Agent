#include "../src/encoding/hevc_rtp_payload_experimental.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
int main() {
    using namespace hi5;
    HevcRtpPayloads out;
    std::string error;
    // Annex-B VPS, SPS, PPS, IDR; marker belongs only to last RTP packet.
    const std::vector<uint8_t> au{
        0,0,0,1, 0x40,0x01,0x11,
        0,0,1, 0x42,0x01,0x22,
        0,0,1, 0x44,0x01,0x33,
        0,0,0,1, 0x26,0x01,0xAA,0xBB
    };
    assert(BuildHevcRtpPayloads(au,1200,out,&error));
    assert(out.hasVps && out.hasSps && out.hasPps && out.hasIrap);
    assert(out.packets.size()==4);
    assert(out.packets[0]==(std::vector<uint8_t>{0x40,0x01,0x11}));
    assert(out.packets[3]==(std::vector<uint8_t>{0x26,0x01,0xAA,0xBB}));

    // Single oversized IDR NAL -> RFC 7798 type-49 FUs with S/E flags.
    std::vector<uint8_t> large{0,0,0,1,0x26,0x01};
    large.insert(large.end(),30,0xAB);
    assert(BuildHevcRtpPayloads(large,13,out,&error));
    assert(out.packets.size()==3);
    assert(out.hasIrap);
    assert(out.packets[0].size()==13);
    assert(out.packets[0][0]==0x62 && out.packets[0][1]==1 &&
           out.packets[0][2]==(0x80|19));
    assert(out.packets[1][2]==19);
    assert(out.packets[2][2]==(0x40|19));
    for(const auto& p:out.packets) assert(p.size()<=13);

    for(const auto& bad: std::vector<std::vector<uint8_t>>{
        {},{0x26,0x01,0x11},{0,0,1},{0,0,1,0x26,0x01},
        {0,0,1,0x80,0x01,0x11}, // forbidden bit
        {0,0,1,0x26,0x00,0x11}, // invalid temporal id
        {0,0,1,0x26,0x09,0x11}, // unsupported temporal layer
        {0,0,1,0x62,0x01,0x11}, // incoming FU not a NAL unit
    }) {
        assert(!BuildHevcRtpPayloads(bad,1200,out,&error));
        assert(out.packets.empty());
    }
    assert(!BuildHevcRtpPayloads(au,1201,out,&error));
    std::cout << "HEVC RFC 7798 single-NAL and FU RTP tests passed\n";
}
