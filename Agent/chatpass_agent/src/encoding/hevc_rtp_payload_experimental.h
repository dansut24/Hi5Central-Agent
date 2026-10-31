#pragma once
// HEVC/H.265 RTP payloads (RFC 7798), experimental and disabled by default.
// Only Annex-B access units and a single temporal/layer stream are supported.
// No decoding order numbers: SDP must NOT negotiate sprop-max-don-diff > 0.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hi5 {
struct HevcRtpPayloads {
    std::vector<std::vector<uint8_t>> packets;
    bool hasVps = false;
    bool hasSps = false;
    bool hasPps = false;
    bool hasIrap = false;
};
inline bool BuildHevcRtpPayloads(const std::vector<uint8_t>& data,
                                size_t maxPayloadBytes,
                                HevcRtpPayloads& out,
                                std::string* error = nullptr) {
    out = {};
    const auto reject = [&](const char* why) {
        out = {};
        if (error) *error = why;
        return false;
    };
    if (maxPayloadBytes < 5 || maxPayloadBytes > 1200)
        return reject("HEVC RTP payload budget outside 5..1200");
    if (data.empty() || data.size() > 4u * 1024u * 1024u)
        return reject("HEVC access unit empty or too large");

    // Require Annex-B. Do not accidentally transmit length-prefixed hvcC.
    const auto startCode = [&](size_t at) -> size_t {
        if (at + 3 <= data.size() &&
            data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1)
            return 3;
        if (at + 4 <= data.size() &&
            data[at] == 0 && data[at + 1] == 0 &&
            data[at + 2] == 0 && data[at + 3] == 1)
            return 4;
        return 0;
    };
    size_t at = startCode(0);
    if (!at) return reject("HEVC requires Annex-B NAL start codes");
    while (at < data.size()) {
        const size_t nalStart = at;
        size_t nalEnd = at;
        while (nalEnd < data.size() && !startCode(nalEnd)) ++nalEnd;
        // Allow Annex-B zero_byte padding between NAL units.
        size_t end = nalEnd;
        if (nalEnd < data.size()) {
            while (end > nalStart && data[end - 1] == 0) --end;
        }
        if (end - nalStart < 3)
            return reject("HEVC truncated NAL header or empty payload");
        const uint8_t h0 = data[nalStart];
        const uint8_t h1 = data[nalStart + 1];
        const uint8_t nalType = (h0 >> 1) & 0x3fu;
        // One-layer, one temporal stream; unsupported aggregation and PACI
        // cannot be sent as if they were ordinary NAL units.
        if ((h0 & 0x80u) || (h0 & 0x01u) || (h1 & 0xF8u) ||
            !(h1 & 0x07u) || nalType >= 48)
            return reject("HEVC invalid/unsupported NAL header");
        out.hasVps |= nalType == 32;
        out.hasSps |= nalType == 33;
        out.hasPps |= nalType == 34;
        out.hasIrap |= nalType >= 16 && nalType <= 23;
        const size_t len = end - nalStart;
        if (len <= maxPayloadBytes) {
            out.packets.emplace_back(data.begin() + nalStart, data.begin() + end);
        } else {
            size_t pos = nalStart + 2;
            const size_t chunkMax = maxPayloadBytes - 3;
            bool start = true;
            while (pos < end) {
                if (out.packets.size() >= 4096)
                    return reject("HEVC requires too many RTP packets");
                const size_t chunk = std::min(chunkMax, end - pos);
                const bool last = pos + chunk == end;
                // 2-byte payload header: F/LayerId/TID copied; type=49 (FU).
                std::vector<uint8_t> packet{
                    static_cast<uint8_t>((h0 & 0x81u) | (49u << 1)),
                    h1,
                    static_cast<uint8_t>((start ? 0x80u : 0) |
                                         (last ? 0x40u : 0) | nalType)
                };
                packet.insert(packet.end(), data.begin() + pos, data.begin() + pos + chunk);
                out.packets.push_back(std::move(packet));
                pos += chunk;
                start = false;
            }
        }
        if (out.packets.size() > 4096) return reject("HEVC packet limit exceeded");
        if (nalEnd == data.size()) break;
        at = nalEnd + startCode(nalEnd);
        if (at == data.size()) return reject("HEVC trailing empty NAL unit");
    }
    if (out.packets.empty()) return reject("HEVC contained no valid NAL units");
    return true;
}
} // namespace hi5
