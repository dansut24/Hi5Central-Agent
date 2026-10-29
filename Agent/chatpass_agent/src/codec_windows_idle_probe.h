#pragma once
// Optional idle-only, device-neutral hardware H.264 qualification. No desktop
// pixels are read or uploaded. Offline output is never treated as browser proof.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "codec_universal_selector.h"
#include "codec_windows_qual_identity.h"
#include "h264_mf_encoder.h"

namespace hi5::universal_codec {
inline bool anyStreamerProcessActive() {
    const HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)return true; // fail closed
    PROCESSENTRY32W entry{};
    entry.dwSize=sizeof(entry);
    bool found=false;
    if(Process32FirstW(snap,&entry)) {
        do {
            if(_wcsicmp(entry.szExeFile,L"native_vp8_stream.exe")==0) {
                found=true;
                break;
            }
        } while(Process32NextW(snap,&entry));
    }
    CloseHandle(snap);
    return found;
}
inline bool probeIdleHardwareH264(const std::string& version,
                                 const std::filesystem::path& path) {
    // Invoked from a below-normal priority worker only. Abort if any remote
    // stream appears or the hardware driver cannot be fingerprinted.
    if(anyStreamerProcessActive())return false;
    const auto now=static_cast<std::int64_t>(std::time(nullptr));
    const CacheIdentity identity=windowsCacheIdentity(version,now);
    if(identity.gpuAndDriver.empty()||identity.operatingSystem.empty())return false;
    auto existing=loadQualificationCache(path,identity,now);
    for(const Candidate& c:existing) {
        if(c.codec=="h264" && c.hardware && c.emittedFrames &&
           c.testedFrames>=20)return true; // cache avoids repeat work
    }

    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(apartment))return false;
    bool success=false;
    do {
        constexpr int width=1280, height=720, fps=20;
        H264MfEncoder encoder;
        std::string error;
        if(!encoder.init(width,height,fps,5000,true,&error,true))break;
        I420Frame frame;
        frame.width=width;
        frame.height=height;
        frame.y.resize(static_cast<std::size_t>(width)*height,90);
        frame.u.resize(static_cast<std::size_t>(width/2)*(height/2),128);
        frame.v.resize(frame.u.size(),128);
        int produced=0;
        bool idr=false;
        double maxMs=0;
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<24;++i) {
            if(anyStreamerProcessActive() ||
               std::chrono::steady_clock::now()-start>
                   std::chrono::seconds(5))break;
            // Cheap moving synthetic text-like stripes; no screen capture.
            for(int row=i%9;row<height;row+=12){
                const std::size_t at=static_cast<std::size_t>(row)*width;
                for(int col=0;col<width;col+=8)
                    frame.y[at+col]=static_cast<std::uint8_t>((col+row+i*17)&255);
            }
            H264EncodedFrame encoded;
            const auto t0=std::chrono::steady_clock::now();
            if(!encoder.encode(frame,i==0,encoded,&error) ||
               encoded.data.empty())break;
            const double elapsed=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-t0).count();
            maxMs=std::max(maxMs,elapsed);
            idr=idr||encoded.keyframe;
            ++produced;
        }
        if(produced<20||!idr||maxMs<=0)break;
        Candidate c;
        c.codec="h264";
        c.backend="h264_hw";
        c.hardware=true;
        c.emittedFrames=true;
        c.keyframeRecovery=idr;
        c.decodedFrames=false;        // cannot prove playback without Viewer
        c.packetizationVerified=false;
        c.testedFrames=produced;
        c.maxWidth=width;c.maxHeight=height;
        c.p95EncodeMs=maxMs;          // conservative upper bound
        c.worstEncodeMs=maxMs;
        existing.erase(std::remove_if(existing.begin(),existing.end(),
            [](const Candidate& prior) {
                return prior.codec=="h264" && prior.hardware &&
                    !prior.decodedFrames;
            }),existing.end());
        existing.push_back(c);
        success=saveQualificationCache(path,identity,existing);
    }while(false);
    CoUninitialize();
    return success;
}
} // namespace hi5::universal_codec
