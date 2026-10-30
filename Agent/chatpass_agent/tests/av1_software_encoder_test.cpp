#include "../src/encoding/av1_software_encoder_experimental.h"
#include "../src/encoding/av1_rtp_payload_experimental.h"
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// Rebuild low-overhead AV1 OBUs from our one-element-per-RTP-packet prototype.
// This intentionally checks the actual Z/Y/W fragmentation, not just size.
static bool Reassemble(const hi5::Av1RtpPayloads& rtp, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<uint8_t> partial;
    bool awaitingContinuation = false;
    for (const auto& packet : rtp.packets) {
        if (packet.size() < 2) return false;
        const uint8_t agg = packet[0];
        const bool z = (agg & 0x80u) != 0;
        const bool y = (agg & 0x40u) != 0;
        if (((agg >> 4) & 0x03u) != 1 || (agg & 0x07u) != 0 ||
            z != awaitingContinuation) return false;
        partial.insert(partial.end(), packet.begin() + 1, packet.end());
        awaitingContinuation = y;
        if (y) continue;
        if (partial.empty() || (partial[0] & 0x83u) != 0) return false;
        const size_t headerBytes = (partial[0] & 0x04u) ? 2u : 1u;
        if (partial.size() < headerBytes) return false;
        out.push_back(static_cast<uint8_t>(partial[0] | 0x02u));
        if (headerBytes == 2) out.push_back(partial[1]);
        size_t size = partial.size() - headerBytes;
        do {
            uint8_t b = static_cast<uint8_t>(size & 0x7fu);
            size >>= 7;
            out.push_back(static_cast<uint8_t>(b | (size ? 0x80u : 0u)));
        } while (size != 0);
        out.insert(out.end(), partial.begin() + static_cast<std::ptrdiff_t>(headerBytes),
                   partial.end());
        partial.clear();
    }
    if (awaitingContinuation || !partial.empty()) { out.clear(); return false; }
    return !out.empty();
}

int main(int argc, char** argv) {
    int width = 640, height = 360, frames = 40, fps = 20;
    int bitrateKbps = 2500;
    if (argc == 2 && std::string(argv[1]) == "--720p") {
        width = 1280; height = 720; bitrateKbps = 4000; frames = 30;
    } else if (argc == 2 && std::string(argv[1]) == "--1080p") {
        width = 1920; height = 1080; bitrateKbps = 6000; frames = 24;
    } else if (argc == 2 && std::string(argv[1]) == "--720p60") {
        width = 1280; height = 720; bitrateKbps = 6000; frames = 90; fps = 60;
    } else if (argc != 1) {
        std::cerr << "Usage: av1_software_encoder_test [--720p|--1080p|--720p60]\n";
        return 2;
    }
    hi5::SoftwareAv1EncoderExperiment encoder;
    std::string err;
    assert(!encoder.initialize(639, 360, 20, 2500, 4, &err));
    if (!encoder.initialize(width, height, fps, bitrateKbps, 4, &err)) {
        std::cerr << "AV1 software init failed: " << err << "\n";
        return 2;
    }
    aom_codec_ctx_t decoder{};
    if (aom_codec_dec_init(&decoder, aom_codec_av1_dx(), nullptr, 0) != AOM_CODEC_OK) {
        std::cerr << "libaom decoder initialization failed\n";
        return 2;
    }
    hi5::SoftwareAv1Frame f{};
    f.width = width; f.height = height;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    f.y.assign(pixels, 64);
    f.u.assign(pixels / 4, 128);
    f.v.assign(pixels / 4, 128);

    int encoded = 0, packetized = 0, decoded = 0, keyframes = 0;
    int recoveredFrames = 0;
    double startupMs = 0, worstMs = 0;
    std::vector<double> steadyMs;
    const int warmupFrames = std::min(frames / 4, 8);
    const int recoveryAt = frames / 2;
    bool recoveryDecoderReset = false;
    for (int i = 0; i < frames; ++i) {
        // Non-uniform changing synthetic text/window blocks, never desktop pixels.
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < width; ++col) {
                const int block = (col / 32 + row / 24 + i * 3) % 13;
                f.y[static_cast<size_t>(row) * width + col] =
                    static_cast<uint8_t>(28 + block * 15);
            }
        const bool forceKeyframe = i == 0 || i == recoveryAt;
        std::vector<hi5::SoftwareAv1Packet> packets;
        const auto before = std::chrono::steady_clock::now();
        if (!encoder.encode(f, forceKeyframe, packets, &err)) {
            std::cerr << "AV1 encode failed: " << err << "\n";
            return 2;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - before).count();
        worstMs = std::max(worstMs, elapsed);
        if (i == 0) startupMs = elapsed;
        if (i >= warmupFrames) steadyMs.push_back(elapsed);
        for (const auto& p : packets) {
            ++encoded;
            if (p.keyframe) ++keyframes;
            hi5::Av1RtpPayloads rtp;
            if (!hi5::BuildAv1RtpPayloads(p.bytes, 1100, rtp, &err)) {
                std::cerr << "AV1 emitted unsupported RTP bitstream: " << err << "\n";
                return 2;
            }
            std::vector<uint8_t> assembled;
            if (!Reassemble(rtp, assembled)) {
                std::cerr << "AV1 RTP reassembly failed\n";
                return 2;
            }
            for (const auto& payload : rtp.packets) {
                if (payload.size() < 2 || payload.size() > 1100) return 2;
                ++packetized;
            }
            // Reset the decoder immediately before a requested mid-stream keyframe,
            // proving an independent decoder can resume without earlier frames.
            if (i == recoveryAt && !recoveryDecoderReset) {
                if (!p.keyframe) {
                    std::cerr << "AV1 forced recovery keyframe was not produced\n";
                    return 2;
                }
                aom_codec_destroy(&decoder);
                if (aom_codec_dec_init(&decoder, aom_codec_av1_dx(), nullptr, 0) != AOM_CODEC_OK)
                    return 2;
                recoveryDecoderReset = true;
            }
            const aom_codec_err_t result = aom_codec_decode(
                &decoder, assembled.data(), assembled.size(), nullptr);
            if (result != AOM_CODEC_OK) {
                std::cerr << "AV1 reassembled stream failed decode: "
                          << aom_codec_error(&decoder) << "\n";
                return 2;
            }
            aom_codec_iter_t iter = nullptr;
            while (aom_codec_get_frame(&decoder, &iter)) {
                ++decoded;
                if (recoveryDecoderReset) ++recoveredFrames;
            }
        }
    }
    encoder.shutdown();
    aom_codec_destroy(&decoder);
    if (encoded != frames || decoded != frames || keyframes < 2 ||
        !recoveryDecoderReset || recoveredFrames != frames - recoveryAt ||
        packetized < encoded || steadyMs.size() != static_cast<size_t>(frames - warmupFrames)) {
        std::cerr << "AV1 frame/decode/keyframe/recovery qualification failed: "
                  << "encoded=" << encoded << " decoded=" << decoded
                  << " recovered=" << recoveredFrames << " keyframes=" << keyframes << "\n";
        return 2;
    }
    std::sort(steadyMs.begin(), steadyMs.end());
    const double p95 = steadyMs[(steadyMs.size() * 95 + 99) / 100 - 1];
    const double steadyWorst = steadyMs.back();
    // Per-frame latency qualification. The 60-FPS budget is 16.67 ms;
    // keep the 20-FPS historical thresholds unchanged for comparability.
    const double budgetMs = 1000.0 / fps;
    const double p95BudgetMs = fps == 60 ? budgetMs * 0.75 : 35.0;
    const double worstBudgetMs = fps == 60 ? budgetMs * 1.50 : 50.0;
    const bool realtime = p95 <= p95BudgetMs && steadyWorst <= worstBudgetMs;
    std::cout << "LIBAOM_REALTIME_SCREEN_AV1_PASS"
              << " resolution=" << width << "x" << height
              << " target_fps=" << fps
              << " budget_ms=" << budgetMs
              << " frames=" << frames << " encoded_frames=" << encoded
              << " decoded_frames=" << decoded
              << " post_reset_decoded_frames=" << recoveredFrames
              << " keyframes=" << keyframes << " rtp_payloads=" << packetized
              << " first_frame_ms=" << startupMs
              << " p95_steady_ms=" << p95
              << " worst_steady_ms=" << steadyWorst
              << " worst_including_startup_ms=" << worstMs
              << " realtime_20fps_qualified=" << (realtime ? "yes" : "no")
              << " environment=synthetic_no_capture_network_or_browser\n";
    return 0;
}
