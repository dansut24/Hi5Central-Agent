#pragma once
// TEST-ONLY HEVC Annex B -> RFC 7798 single-NAL/FU payloads.
// No SDP offer, codec activation or production Agent code uses this class.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace hi5 {
struct ExperimentalHevcPackets {
    std::vector<std::vector<uint8_t>> packets;
    bool hasVps = false, hasSps = false, hasPps = false, hasRandomAccess = false;
    unsigned nalCount = 0;
};
inline bool PacketizeHevcAnnexB(const std::vector<uint8_t>& data,
      size_t maxPayload, ExperimentalHevcPackets& result,
      std::string* error = nullptr) {
    result = {};
    const auto fail = [&](const char* why) {
        result = {};
        if (error) *error = why;
        return false;
    };
    if (maxPayload < 8 || maxPayload > 1200 ||
        data.size() < 6 || data.size() > 4u * 1024u * 1024u)
        return fail("HEVC malformed bytes or bounded RTP size");
    const auto startAt = [&](size_t at)->size_t {
        if (at+3 <= data.size() && data[at]==0 && data[at+1]==0) {
            if (data[at+2]==1) return 3;
            if (at+4<=data.size() && data[at+2]==0 && data[at+3]==1)
                return 4;
        }
        return 0;
    };
    size_t pos=0;
    while(pos<data.size()) {
        const size_t startLen=startAt(pos);
        if (!startLen) return fail("HEVC missing Annex B start code");
        const size_t begin=pos+startLen;
        if (begin+2>data.size()) return fail("HEVC NAL header truncated");
        size_t end=begin+2;
        while(end<data.size() && !startAt(end)) ++end;
        // An Annex B NAL may have 1-3 zero-byte padding at end; trim only
        // exact zero bytes, never remove the nonzero encoded payload.
        size_t trimmedEnd=end;
        while(trimmedEnd>begin+2 && data[trimmedEnd-1]==0) --trimmedEnd;
        const size_t len=trimmedEnd-begin;
        if (len<2) return fail("HEVC NAL too small");
        const uint8_t h0=data[begin],h1=data[begin+1];
        const uint8_t nalType=(h0>>1)&0x3f;
        if (h0&0x80 || (h1&0x07)==0 ||
            nalType==48 || nalType==49 || nalType==50)
            return fail("HEVC forbidden bit, temporal ID or unsupported NAL type");
        if (nalType==32) result.hasVps=true;
        if (nalType==33) result.hasSps=true;
        if (nalType==34) result.hasPps=true;
        if (nalType>=16 && nalType<=21) result.hasRandomAccess=true;
        ++result.nalCount;
        if (result.packets.size() > 4090) return fail("HEVC packet cap");
        if (len<=maxPayload) {
            result.packets.emplace_back(data.begin()+static_cast<std::ptrdiff_t>(begin),
                                        data.begin()+static_cast<std::ptrdiff_t>(trimmedEnd));
        } else {
            // RFC 7798 Fragmentation Unit (FU), payload type 49; retain
            // F + original layer_id + TID, with start/end bits and FuType.
            const uint8_t fu0=static_cast<uint8_t>((h0&0x81u)|(49u<<1));
            const uint8_t fu1=h1;
            const size_t capacity=maxPayload-3;
            size_t n=begin+2;
            while(n<trimmedEnd) {
                if (result.packets.size()>=4096) return fail("HEVC packet cap");
                const size_t count=std::min(capacity,trimmedEnd-n);
                const bool first=n==begin+2;
                const bool last=n+count==trimmedEnd;
                std::vector<uint8_t> fragment;
                fragment.reserve(count+3);
                fragment.push_back(fu0);
                fragment.push_back(fu1);
                fragment.push_back(static_cast<uint8_t>(
                    (first?0x80u:0u)|(last?0x40u:0u)|nalType));
                fragment.insert(fragment.end(),
                    data.begin()+static_cast<std::ptrdiff_t>(n),
                    data.begin()+static_cast<std::ptrdiff_t>(n+count));
                result.packets.push_back(std::move(fragment));
                n+=count;
            }
        }
        pos=end;
    }
    if(result.packets.empty()) return fail("HEVC empty access unit");
    return true;
}
} // namespace hi5
