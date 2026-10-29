#include "../src/encoding/av1_software_encoder_experimental.h"
#include "../src/encoding/av1_rtp_payload_experimental.h"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
int main() {
    hi5::SoftwareAv1EncoderExperiment encoder;
    std::string err;
    assert(!encoder.initialize(639,360,20,2500,4,&err)); // even-sized I420
    assert(encoder.initialize(640,360,20,2500,4,&err));
    hi5::SoftwareAv1Frame f{};
    f.width=640; f.height=360;
    f.y.assign(640*360,64);
    f.u.assign(640*360/4,128);
    f.v.assign(640*360/4,128);
    unsigned int encoded=0, packetized=0, keyframes=0;
    double worstMs=0.0;
    double startupMs=0.0;
    std::vector<double> steadyMs;
    constexpr int warmupFrames = 8;
    constexpr int totalFrames = 40;
    for (int i=0;i<totalFrames;++i) {
        // Nontrivial changing blocks approximate window movement without
        // reading the real desktop, credentials or UI.
        for(int row=0;row<f.height;++row) {
            for(int col=0;col<f.width;++col) {
                const int block=(col/32 + row/24 + i*3)%13;
                f.y[static_cast<size_t>(row)*f.width+col]=static_cast<uint8_t>(28+block*15);
            }
        }
        std::vector<hi5::SoftwareAv1Packet> packets;
        const auto before=std::chrono::steady_clock::now();
        assert(encoder.encode(f,i==0,packets,&err));
        const double elapsed=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-before).count();
        worstMs=std::max(elapsed,worstMs);
        if (i==0) startupMs=elapsed;
        if (i>=warmupFrames) steadyMs.push_back(elapsed);
        for(const auto& p:packets) {
            ++encoded;
            if(p.keyframe) ++keyframes;
            hi5::Av1RtpPayloads rtp;
            if(!hi5::BuildAv1RtpPayloads(p.bytes,1100,rtp,&err)) {
                std::cerr<<"AV1 emitted unsupported RTP bitstream: "<<err<<"\n";
                return 2;
            }
            for(const auto& payload:rtp.packets) {
                assert(payload.size()>1 && payload.size()<=1100);
                ++packetized;
            }
        }
    }
    assert(encoded>0);
    assert(keyframes>0);
    assert(packetized>0);
    encoder.shutdown();
    assert(steadyMs.size()==totalFrames-warmupFrames);
    std::sort(steadyMs.begin(),steadyMs.end());
    const double p95Ms=steadyMs[(steadyMs.size()*95+99)/100-1];
    const double steadyWorstMs=steadyMs.back();
    const bool realTime20Fps = p95Ms <= 35.0 && steadyWorstMs <= 50.0;
    std::cout<<"LIBAOM_REALTIME_SCREEN_AV1_PASS "
             <<"encoded_frames="<<encoded
             <<" rtp_payloads="<<packetized
             <<" first_frame_ms="<<startupMs
             <<" p95_steady_ms="<<p95Ms
             <<" worst_steady_ms="<<steadyWorstMs
             <<" worst_including_startup_ms="<<worstMs
             <<" realtime_20fps_qualified="<<(realTime20Fps?"yes":"no")<<"\n";
}
