#include "../src/vp9_rtp_payload.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    std::vector<uint8_t> data(6500);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i % 251);

    auto key = hi5::vp9_rtp::packetize(data, true, 1920, 1080, 0x1234);
    assert(key.size() > 5);
    std::vector<uint8_t> reconstructed;
    for (size_t i = 0; i < key.size(); ++i) {
        const auto& p = key[i];
        assert(p.size() <= 1200);
        assert((p[0] & 0x80) != 0); // Picture ID
        assert((p[0] & 0x40) == 0); // Intra frame
        assert(((p[0] & 0x08) != 0) == (i == 0));
        assert(((p[0] & 0x04) != 0) == (i + 1 == key.size()));
        assert(((p[0] & 0x02) != 0) == (i == 0));
        assert(p[1] == 0x92 && p[2] == 0x34);
        const size_t offset = i == 0 ? 8 : 3;
        if (i == 0) {
            assert(p[3] == 0x10); // spatial resolution structure
            assert(p[4] == 0x07 && p[5] == 0x80); // 1920
            assert(p[6] == 0x04 && p[7] == 0x38); // 1080
        }
        reconstructed.insert(reconstructed.end(), p.begin() + offset, p.end());
    }
    assert(reconstructed == data);

    auto inter = hi5::vp9_rtp::packetize(data, false, 1920, 1080, 0x1235);
    assert(inter.size() == key.size());
    for (const auto& p : inter) {
        assert((p[0] & 0x40) != 0);
        assert((p[0] & 0x02) == 0);
        assert(p[1] == 0x92 && p[2] == 0x35);
    }
    assert(hi5::vp9_rtp::packetize({}, true, 1920, 1080, 3).empty());
    assert(hi5::vp9_rtp::packetize(data, true, 1920, 1080, 3, 10).empty());
    return 0;
}
