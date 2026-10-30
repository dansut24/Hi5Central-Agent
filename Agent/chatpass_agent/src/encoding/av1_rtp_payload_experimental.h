#pragma once
// Experimental AV1 RTP payload builder (RFC 9364). NOT a production sender.
// Accepts a single low-overhead AV1 temporal unit with size-delimited OBUs;
// rejects Annex-B, malformed inputs and packets outside the bounded budget.
// The packetizer deliberately makes one OBU element per RTP packet (W=1).
// An AV1 codec/SDP negotiation and end-to-end decode test are prerequisites
// before these bytes may ever be forwarded to a WebRTC RTP track.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace hi5 {
struct Av1RtpPayloads {
    std::vector<std::vector<uint8_t>> packets;
    bool hasSequenceHeader = false;
};

inline bool Av1ReadLeb128(const std::vector<uint8_t>& bytes, size_t& at,
                         uint32_t& value) {
    uint64_t decoded = 0;
    for (int i = 0; i < 8; ++i) {
        if (at == bytes.size()) return false;
        const uint8_t byte = bytes[at++];
        decoded |= static_cast<uint64_t>(byte & 0x7fu) << (7 * i);
        if (!(byte & 0x80u)) {
            if (decoded > std::numeric_limits<uint32_t>::max()) return false;
            value = static_cast<uint32_t>(decoded);
            return true;
        }
    }
    return false;
}

// Returns false on every unsupported bitstream, leaving the output empty.
// No payload from a rejected experiment may be sent on the established VP8
// track. A caller must confirm browser AV1 SDP support before RTP use.
inline bool BuildAv1RtpPayloads(const std::vector<uint8_t>& bitstream,
                               size_t maxPayloadBytes,
                               Av1RtpPayloads& out,
                               std::string* error = nullptr) {
    out = {};
    const auto reject = [&](const char* why) {
        out = {};
        if (error) *error = why;
        return false;
    };
    if (maxPayloadBytes < 4 || maxPayloadBytes > 1200)
        return reject("AV1 RTP payload budget outside 4..1200");
    if (bitstream.empty() || bitstream.size() > 4u * 1024u * 1024u)
        return reject("AV1 temporal unit empty or too large");

    size_t offset = 0;
    while (offset < bitstream.size()) {
        const uint8_t obuHeader = bitstream[offset++];
        if ((obuHeader & 0x81u) || !(obuHeader & 0x02u))
            return reject("AV1 OBU forbidden/reserved bits or missing size field");
        const uint8_t obuType = (obuHeader >> 3) & 0x0fu;
        const bool extension = (obuHeader & 0x04u) != 0;
        uint8_t extensionByte = 0;
        if (extension) {
            if (offset == bitstream.size()) return reject("AV1 truncated extension");
            extensionByte = bitstream[offset++];
            if ((extensionByte & 0x07u) != 0) return reject("AV1 invalid extension");
        }
        uint32_t obuSize = 0;
        if (!Av1ReadLeb128(bitstream, offset, obuSize))
            return reject("AV1 missing/invalid OBU size");
        if (obuSize > bitstream.size() - offset)
            return reject("AV1 OBU size exceeds bitstream");
        // Temporal delimiter and padding OBUs are excluded from RTP.
        const bool skip = obuType == 2 || obuType == 15;
        if (!skip) {
            const bool firstSequence = obuType == 1 && !out.hasSequenceHeader;
            if (obuType == 1) out.hasSequenceHeader = true;
            const size_t obuHeaderSize = extension ? 2u : 1u;
            const size_t obuTotal = obuHeaderSize + static_cast<size_t>(obuSize);
            size_t consumed = 0;
            while (consumed < obuTotal) {
                if (out.packets.size() >= 4096)
                    return reject("AV1 requires too many RTP packets");
                const size_t capacity = maxPayloadBytes - 1; // aggregation header
                const size_t chunk = std::min(capacity, obuTotal - consumed);
                const bool continuation = consumed != 0;
                const bool more = consumed + chunk < obuTotal;
                // Z=fragment continuation, Y=more fragments, W=one OBU, N=sequence header start.
                const uint8_t aggregation =
                    static_cast<uint8_t>((continuation ? 0x80u : 0) |
                                         (more ? 0x40u : 0) | 0x10u |
                                         (firstSequence && !continuation ? 0x08u : 0));
                std::vector<uint8_t> packet;
                packet.reserve(chunk + 1);
                packet.push_back(aggregation);
                // Remove the AV1 OBU length field. The header must be carried
                // with obu_has_size_field=0 in the RTP OBU element.
                for (size_t i = consumed; i < consumed + chunk; ++i) {
                    if (i == 0) packet.push_back(obuHeader & ~uint8_t{0x02u});
                    else if (extension && i == 1) packet.push_back(extensionByte);
                    else packet.push_back(bitstream[offset + i - obuHeaderSize]);
                }
                out.packets.push_back(std::move(packet));
                consumed += chunk;
            }
        }
        offset += static_cast<size_t>(obuSize);
    }
    if (out.packets.empty()) return reject("AV1 temporal unit has no decodable OBUs");
    return true;
}
} // namespace hi5
