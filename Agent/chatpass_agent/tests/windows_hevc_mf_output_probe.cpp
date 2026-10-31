#include "hevc_mf_encoder_experimental.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

// Standalone synthetic transform qualification, never installed as a service.
// No desktop capture, credentials, WebRTC, or changes to agent configuration.
int main() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
        std::cout << "HEVC_MF_RESULT=coinit_failed\n";
        return 0;
    }
    HevcMfEncoder encoder;
    std::string error;
    constexpr int width=1280, height=720, fps=30;
    if (!encoder.init(width,height,fps,4000,true,&error)) {
        std::cout << "HEVC_MF_RESULT=init_failed reason=" << error << "\n";
        if (SUCCEEDED(co)) CoUninitialize();
        return 0;
    }
    std::cout << "HEVC_MF_INIT_OK encoder=" << encoder.encoderName() << "\n";
    I420Frame frame;
    frame.width=width;
    frame.height=height;
    frame.y.assign(static_cast<size_t>(width)*height, 95);
    frame.u.assign(static_cast<size_t>(width/2)*(height/2), 128);
    frame.v.assign(static_cast<size_t>(width/2)*(height/2), 128);
    int emitted=0, successfulInputs=0;
    unsigned firstHeaderSamples=0;
    unsigned annexBStartFrames=0;
    size_t emittedBytes=0;
    double maxMs=0, totalMs=0;
    for (int i=0;i<45;i++) {
        frame.y[static_cast<size_t>((i*17)%height)*width + (i*23)%width] =
            static_cast<uint8_t>(50+(i%120));
        const auto start=std::chrono::steady_clock::now();
        HevcEncodedFrame out;
        error.clear();
        if (!encoder.encode(frame,i==0,out,&error)) {
            std::cout << "HEVC_MF_RESULT=process_failed after=" << successfulInputs
                      << " reason=" << error << "\n";
            encoder.shutdown();
            if (SUCCEEDED(co)) CoUninitialize();
            return 0;
        }
        const double ms=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-start).count();
        maxMs=std::max(maxMs,ms);
        totalMs+=ms;
        successfulInputs++;
        if (!out.data.empty()) {
            emitted++;
            emittedBytes+=out.data.size();
            if (firstHeaderSamples < 4) {
                std::ostringstream hex;
                hex << std::hex << std::setfill('0');
                for (size_t j=0;j<std::min<size_t>(16,out.data.size());++j)
                    hex << std::setw(2) << static_cast<int>(out.data[j]);
                std::cout << "HEVC_MF_FRAME_HEADER=" << hex.str()
                          << " bytes=" << out.data.size()
                          << " claimed_keyframe=" << out.keyframe << "\n";
                ++firstHeaderSamples;
            }
            if (out.data.size() >= 4 && out.data[0] == 0 && out.data[1] == 0 &&
                (out.data[2] == 1 || (out.data[2] == 0 && out.data[3] == 1)))
                ++annexBStartFrames;
        }
    }
    encoder.shutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    std::cout << "HEVC_MF_RESULT=" << (emitted>=20 ? "emitted" : "insufficient_output")
              << " input_frames=" << successfulInputs
              << " encoded_frames=" << emitted
              << " encoded_bytes=" << emittedBytes
              << " annexb_start_frames=" << annexBStartFrames
              << " encode_avg_ms=" << totalMs/std::max(1,successfulInputs)
              << " encode_max_ms=" << maxMs
              << " synthetic_only=1 live_browser_verified=0\n";
}
