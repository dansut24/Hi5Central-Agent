#pragma once
// Convert AV1 Annex B temporal units, if present, to the low-overhead OBUs
// required by our RFC 9364 packetizer. No heuristic reinterprets arbitrary
// malformed bytes as valid AV1. A normal low-overhead input is unchanged.
#include "av1_rtp_payload_experimental.h"
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace hi5 {
inline void Av1WriteLeb128(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        out.push_back(static_cast<uint8_t>(byte | (value ? 0x80 : 0)));
    } while (value);
}
// AV1 Annex B: temporal_unit_size, frame_unit_size, obu_length, OBU bytes.
// Each length is unsigned LEB128 and includes all bytes after the length
// field until its corresponding boundary. Re-encoding keeps all OBU payloads
// intact; it adds obu_has_size_field where necessary.
inline bool Av1AnnexBToLowOverhead(const std::vector<uint8_t>& input,
                                   std::vector<uint8_t>& normalized) {
    normalized.clear();
    if (input.empty() || input.size() > 4u * 1024u * 1024u) return false;
    size_t at = 0;
    size_t obuCount = 0;
    while (at < input.size()) {
        uint32_t temporalSize = 0;
        if (!Av1ReadLeb128(input, at, temporalSize) ||
            !temporalSize || temporalSize > input.size() - at) return false;
        const size_t temporalEnd = at + temporalSize;
        while (at < temporalEnd) {
            uint32_t frameSize = 0;
            if (!Av1ReadLeb128(input, at, frameSize) ||
                at > temporalEnd || !frameSize ||
                frameSize > temporalEnd - at) return false;
            const size_t frameEnd = at + frameSize;
            while (at < frameEnd) {
                uint32_t obuLength = 0;
                if (!Av1ReadLeb128(input, at, obuLength) ||
                    at > frameEnd || !obuLength ||
                    obuLength > frameEnd - at ||
                    ++obuCount > 1024) return false;
                const size_t obuEnd = at + obuLength;
                const uint8_t header = input[at++];
                const uint8_t type = (header >> 3) & 0x0fu;
                if ((header & 0x81u) || type == 0 || type == 14) return false;
                const bool extension = (header & 0x04u) != 0;
                uint8_t extensionByte = 0;
                if (extension) {
                    if (at >= obuEnd) return false;
                    extensionByte = input[at++];
                    if ((extensionByte & 0x07u) != 0) return false;
                }
                if (header & 0x02u) {
                    uint32_t declaredSize = 0;
                    if (!Av1ReadLeb128(input, at, declaredSize) ||
                        at > obuEnd || declaredSize != obuEnd - at) return false;
                }
                if (at > obuEnd) return false;
                const size_t payloadSize = obuEnd - at;
                if (payloadSize > 4u * 1024u * 1024u ||
                    normalized.size() + payloadSize + 7 > 4u * 1024u * 1024u)
                    return false;
                normalized.push_back(header | uint8_t{0x02u});
                if (extension) normalized.push_back(extensionByte);
                Av1WriteLeb128(normalized, static_cast<uint32_t>(payloadSize));
                normalized.insert(normalized.end(), input.begin() + at,
                                  input.begin() + obuEnd);
                at = obuEnd;
            }
            if (at != frameEnd) return false;
        }
        if (at != temporalEnd) return false;
    }
    return obuCount != 0 && at == input.size() && !normalized.empty();
}
// Fail closed. A raw AV1 sample with an unsupported framing is NOT sent.
// The caller must still require a sequence header before RTP and a valid
// decoder observation before qualifying hardware AV1 automatically.
inline bool BuildAv1MfRtpPayloads(const std::vector<uint8_t>& sample,
                                  size_t maxPayloadBytes,
                                  Av1RtpPayloads& out,
                                  std::string* error = nullptr) {
    std::string lowError;
    if (BuildAv1RtpPayloads(sample, maxPayloadBytes, out, &lowError))
        return true;
    std::vector<uint8_t> converted;
    if (Av1AnnexBToLowOverhead(sample, converted)) {
        if (BuildAv1RtpPayloads(converted, maxPayloadBytes, out, error))
            return true;
    }
    out = {};
    if (error) *error = lowError + "; no valid Annex B temporal-unit structure";
    return false;
}
} // namespace hi5
