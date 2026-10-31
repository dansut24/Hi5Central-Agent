#pragma once
// TEST-ONLY: unwrap Microsoft Media Foundation AV1 IVF output before
// handing low-overhead AV1 OBUs to the experimental RFC9364 RTP packetizer.
// No production sender uses this path.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hi5 {
class ExperimentalAv1IvfExtractor {
public:
    bool unwrap(const std::vector<uint8_t>& input, std::vector<uint8_t>& obu,
                std::string* error = nullptr) {
        obu.clear();
        const auto reject = [&](const char* why) {
            if (error) *error = why;
            return false;
        };
        if (input.empty() || input.size() > 4u * 1024u * 1024u + 44u)
            return reject("IVF sample outside bounded size");
        size_t offset = 0;
        if (!initialized_) {
            if (input.size() < 44 ||
                input[0] != 'D' || input[1] != 'K' ||
                input[2] != 'I' || input[3] != 'F')
                return reject("missing IVF DKIF header");
            if (u16(input, 4) != 0 || u16(input, 6) != 32)
                return reject("unsupported IVF version/header size");
            if (input[8] != 'A' || input[9] != 'V' ||
                input[10] != '0' || input[11] != '1')
                return reject("IVF codec is not AV01");
            if (u16(input, 12) == 0 || u16(input, 14) == 0 ||
                u16(input, 12) > 7680 || u16(input, 14) > 4320)
                return reject("unsupported IVF dimensions");
            if (u32(input, 16) == 0 || u32(input, 20) == 0)
                return reject("invalid IVF timebase");
            offset = 32;
        }
        if (input.size() < offset + 13)
            return reject("truncated IVF frame header or missing OBU");
        const uint32_t frameSize = u32(input, offset);
        if (frameSize == 0 || frameSize > 4u * 1024u * 1024u ||
            static_cast<size_t>(frameSize) != input.size() - offset - 12)
            return reject("invalid IVF frame byte count");
        const uint64_t pts = u64(input, offset + 4);
        if (initialized_ && pts <= lastPts_)
            return reject("IVF frame timestamp not increasing");
        obu.assign(input.begin() + static_cast<std::ptrdiff_t>(offset + 12),
                   input.end());
        initialized_ = true;
        lastPts_ = pts;
        return true;
    }
private:
    static uint16_t u16(const std::vector<uint8_t>& b,size_t at) {
        return static_cast<uint16_t>(b[at]) |
            (static_cast<uint16_t>(b[at+1]) << 8);
    }
    static uint32_t u32(const std::vector<uint8_t>& b,size_t at) {
        return static_cast<uint32_t>(b[at]) |
            (static_cast<uint32_t>(b[at+1]) << 8) |
            (static_cast<uint32_t>(b[at+2]) << 16) |
            (static_cast<uint32_t>(b[at+3]) << 24);
    }
    static uint64_t u64(const std::vector<uint8_t>& b,size_t at) {
        return static_cast<uint64_t>(u32(b,at)) |
            (static_cast<uint64_t>(u32(b,at+4)) << 32);
    }
    bool initialized_ = false;
    uint64_t lastPts_ = 0;
};
} // namespace hi5
