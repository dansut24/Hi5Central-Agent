#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// RFC 7741: one spatial layer, flexible-free VP9 RTP payload descriptor.
// Every fragment carries a stable 15-bit Picture ID. A keyframe carries
// spatial layer resolution so the receiver can initialize its decoder.
// Never replace a keyframe flag with a claim unsupported by the encoder.
namespace hi5::vp9_rtp {

inline std::vector<std::vector<uint8_t>> packetize(
    const std::vector<uint8_t>& bitstream,
    bool actualKeyframe,
    int width,
    int height,
    uint16_t pictureId,
    size_t maxPayload = 1200) {
    std::vector<std::vector<uint8_t>> out;
    if (bitstream.empty() || width <= 0 || height <= 0 ||
        width > 65535 || height > 65535 || maxPayload < 16)
        return out;

    const uint8_t high = static_cast<uint8_t>(0x80 | ((pictureId >> 8) & 0x7f));
    const uint8_t low = static_cast<uint8_t>(pictureId & 0xff);
    size_t offset = 0;
    while (offset < bitstream.size()) {
        const bool begin = offset == 0;
        const bool includeSs = begin && actualKeyframe;
        const size_t overhead = 3 + (includeSs ? 5 : 0);
        const size_t length = std::min(maxPayload - overhead, bitstream.size() - offset);
        const bool end = offset + length == bitstream.size();
        std::vector<uint8_t> payload;
        payload.reserve(overhead + length);

        uint8_t descriptor = 0x80; // I: Picture ID
        if (!actualKeyframe) descriptor |= 0x40; // P: inter-picture prediction
        if (begin) descriptor |= 0x08; // B: start of frame
        if (end) descriptor |= 0x04; // E: end of frame
        if (includeSs) descriptor |= 0x02; // V: scalability structure present
        payload.push_back(descriptor);
        payload.push_back(high);
        payload.push_back(low);
        if (includeSs) {
            payload.push_back(0x10); // 1 spatial layer, Y: resolution present
            payload.push_back(static_cast<uint8_t>(width >> 8));
            payload.push_back(static_cast<uint8_t>(width));
            payload.push_back(static_cast<uint8_t>(height >> 8));
            payload.push_back(static_cast<uint8_t>(height));
        }
        payload.insert(payload.end(), bitstream.begin() + static_cast<std::ptrdiff_t>(offset),
                       bitstream.begin() + static_cast<std::ptrdiff_t>(offset + length));
        out.push_back(std::move(payload));
        offset += length;
    }
    return out;
}
} // namespace hi5::vp9_rtp
