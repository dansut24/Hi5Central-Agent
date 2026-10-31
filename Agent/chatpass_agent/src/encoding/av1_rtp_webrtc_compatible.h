#pragma once
// RFC 9364 AV1 RTP packing compatible with libWebRTC's RtpPacketizerAv1
// aggregation layout. Original Hi5Central implementation; no libWebRTC
// runtime or third-party sources linked into the Agent.
// References: WebRTC modules/rtp_rtcp/source/rtp_packetizer_av1.cc and
// rtp_packetizer_av1_unittest.cc (BSD-3-Clause), RFC 9364 section 4.3.
#include "av1_rtp_payload_experimental.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hi5 {
namespace av1_rtp_detail {
inline size_t LebSize(size_t value) {
    size_t n = 1;
    while (value > 127) { value >>= 7; ++n; }
    return n;
}
inline void WriteLeb(std::vector<uint8_t>& dst, size_t value) {
    do {
        uint8_t b = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        dst.push_back(static_cast<uint8_t>(b | (value ? 0x80 : 0)));
    } while (value);
}
struct Obu {
    uint8_t type = 0;
    std::vector<uint8_t> bytes;
};
struct Fragment {
    size_t index = 0;
    size_t offset = 0;
    size_t size = 0;
};
inline size_t PacketSize(const std::vector<Fragment>& fs) {
    size_t n = 1; // aggregation header
    for (size_t i = 0; i < fs.size(); ++i) {
        n += fs[i].size;
        if (i + 1 < fs.size()) n += LebSize(fs[i].size);
    }
    return n;
}
} // namespace av1_rtp_detail

inline bool BuildAv1WebRtcCompatibleRtpPayloads(
    const std::vector<uint8_t>& stream, size_t maxPayloadBytes,
    bool isKeyframe, Av1RtpPayloads& out, std::string* error = nullptr) {
    out = {};
    const auto reject = [&](const char* reason) {
        out = {};
        if (error) *error = reason;
        return false;
    };
    if (maxPayloadBytes < 4 || maxPayloadBytes > 1200)
        return reject("WebRTC AV1 max RTP payload outside 4..1200");
    if (stream.empty() || stream.size() > 4u * 1024u * 1024u)
        return reject("WebRTC AV1 empty/oversized bitstream");
    std::vector<av1_rtp_detail::Obu> obus;
    size_t at = 0;
    while (at < stream.size()) {
        if (obus.size() >= 1024) return reject("WebRTC AV1 too many OBUs");
        const uint8_t header = stream[at++];
        const uint8_t type = (header >> 3) & 0x0f;
        if ((header & 0x81u) || !(header & 0x02u) || type == 0 || type == 14)
            return reject("WebRTC AV1 forbidden OBU flags or type");
        const bool extension = (header & 0x04u) != 0;
        uint8_t ext = 0;
        if (extension) {
            if (at >= stream.size()) return reject("WebRTC AV1 truncated OBU extension");
            ext = stream[at++];
            if (ext & 0x07u) return reject("WebRTC AV1 reserved OBU extension bits");
        }
        uint32_t payloadSize = 0;
        if (!Av1ReadLeb128(stream, at, payloadSize) ||
            payloadSize > stream.size() - at)
            return reject("WebRTC AV1 invalid OBU length");
        // Same exclusions as libWebRTC: temporal delimiter, tile-list, padding.
        if (type != 2 && type != 8 && type != 15) {
            av1_rtp_detail::Obu obu;
            obu.type = type;
            obu.bytes.reserve(static_cast<size_t>(payloadSize) + (extension ? 2u : 1u));
            obu.bytes.push_back(static_cast<uint8_t>(header & ~uint8_t{0x02u}));
            if (extension) obu.bytes.push_back(ext);
            obu.bytes.insert(obu.bytes.end(), stream.begin() + static_cast<std::ptrdiff_t>(at),
                stream.begin() + static_cast<std::ptrdiff_t>(at + payloadSize));
            if (type == 1) out.hasSequenceHeader = true;
            obus.push_back(std::move(obu));
        }
        at += payloadSize;
    }
    if (obus.empty()) return reject("WebRTC AV1 no decodable OBUs");

    // Each RTP packet contains up to 3 OBU elements (W=1..3). For W>0 only
    // preceding elements have LEB128 lengths; the last is size-implicit.
    // Greedily aggregate small sequence headers with the next frame's first
    // fragment, unlike the old one-OBU-per-packet transmitter.
    size_t index = 0, offset = 0;
    while (index < obus.size()) {
        if (out.packets.size() >= 4096) return reject("WebRTC AV1 too many RTP packets");
        const bool continued = offset != 0;
        std::vector<av1_rtp_detail::Fragment> chunks;
        while (index < obus.size() && chunks.size() < 3) {
            const auto& bytes = obus[index].bytes;
            if (offset >= bytes.size()) return reject("WebRTC AV1 invalid OBU offset");
            const size_t remaining = bytes.size() - offset;
            const size_t used = av1_rtp_detail::PacketSize(chunks);
            const size_t previousSizeOverhead = chunks.empty()
                ? 0u : av1_rtp_detail::LebSize(chunks.back().size);
            const size_t budget = used + previousSizeOverhead < maxPayloadBytes
                ? maxPayloadBytes - used - previousSizeOverhead : 0u;
            const size_t fit = std::min(remaining, budget);
            if (fit == 0) {
                if (chunks.empty()) return reject("WebRTC AV1 cannot fit RTP OBU fragment");
                break;
            }
            chunks.push_back({index, offset, fit});
            if (fit != remaining) {
                offset += fit;
                break; // Fragmented OBU must terminate packet (Y=1).
            }
            ++index;
            offset = 0;
        }
        if (chunks.empty()) return reject("WebRTC AV1 empty RTP packet");
        const bool fragmentedEnd = offset != 0;
        const bool startsSequence = out.packets.empty() && isKeyframe &&
            obus[chunks.front().index].type == 1;
        const uint8_t agg = static_cast<uint8_t>(
            (continued ? 0x80u : 0u) | (fragmentedEnd ? 0x40u : 0u) |
            (static_cast<uint8_t>(chunks.size()) << 4) |
            (startsSequence ? 0x08u : 0u));
        std::vector<uint8_t> payload;
        payload.reserve(av1_rtp_detail::PacketSize(chunks));
        payload.push_back(agg);
        for (size_t i = 0; i < chunks.size(); ++i) {
            const auto& f = chunks[i];
            if (i + 1 < chunks.size()) av1_rtp_detail::WriteLeb(payload, f.size);
            const auto& v = obus[f.index].bytes;
            payload.insert(payload.end(), v.begin() + static_cast<std::ptrdiff_t>(f.offset),
                v.begin() + static_cast<std::ptrdiff_t>(f.offset + f.size));
        }
        if (payload.size() > maxPayloadBytes || payload.size() < 2)
            return reject("WebRTC AV1 packet exceeds RTP limit");
        out.packets.push_back(std::move(payload));
    }
    return true;
}
} // namespace hi5
