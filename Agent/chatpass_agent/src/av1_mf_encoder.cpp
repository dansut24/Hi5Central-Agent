#include "av1_mf_encoder.h"

Av1MfEncoder::Av1MfEncoder() = default;

Av1MfEncoder::~Av1MfEncoder() {
    shutdown();
}

void Av1MfEncoder::shutdown() {
    m_open = false;
}

bool Av1MfEncoder::init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error) {
    (void)width;
    (void)height;
    (void)fps;
    (void)bitrateKbps;
    (void)preferHardware;

    if (error) {
        *error = "AV1 Media Foundation encoder not implemented yet";
    }

    m_open = false;
    m_encoderName = "AV1 Media Foundation Encoder";
    return false;
}

bool Av1MfEncoder::encode(const I420Frame& frame, bool forceKeyframe, Av1EncodedFrame& out, std::string* error) {
    (void)frame;
    (void)forceKeyframe;
    out = {};

    if (error) {
        *error = "AV1 Media Foundation encode not implemented yet";
    }

    return false;
}
