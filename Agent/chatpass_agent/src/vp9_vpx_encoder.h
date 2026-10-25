#pragma once
#include "vp9_mf_encoder.h"
#include <memory>

// VP9 software path. "Lossless" means exact I420 frames, NOT lossless
// conversion from the source Windows RGB desktop (which is subsampled).
class Vp9VpxEncoder {
public:
    Vp9VpxEncoder(int width, int height, int fps, int bitrateKbps);
    ~Vp9VpxEncoder();
    bool reconfigure(int fps, int bitrateKbps);
    Vp9EncodedFrame encode(const I420Frame& frame, bool keyframe, bool lossless);
    bool losslessActive() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
