// Windows-only synthetic Media Foundation encoder qualification.
// Never reads the real desktop or uploads video. The production RTP codec
// MUST NOT be enabled solely because a Media Foundation MFT is enumerated.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include "h264_mf_encoder.h"
#include "av1_mf_encoder.h"
#include "h265_mf_encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <numeric>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

// Default smoke runs quickly on a Windows CI runner. --1080p uses the
// endpoint's actual Full HD workload and 40 frames, still synthetic.
struct BenchmarkConfig { int width=640; int height=360; int fps=20; int frames=16; bool hardwareOnly=false; };
static BenchmarkConfig config;

static GUID subtype(char a, char b, char c, char d) {
    const uint32_t fourcc = uint32_t(uint8_t(a)) |
        (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
        (uint32_t(uint8_t(d)) << 24);
    return GUID{fourcc, 0, 0x0010, {0x80,0,0,0xAA,0,0x38,0x9B,0x71}};
}
static bool dimensions(IMFMediaType* type, int w, int h, int fps) {
    return type && SUCCEEDED(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, w, h)) &&
        SUCCEEDED(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1)) &&
        SUCCEEDED(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
}
struct Result {
    std::string name;
    bool discovered = false;
    bool activated = false;
    bool configured = false;
    bool produced = false;
    int frames = 0;
    int attempts = 0;
    double avgMs = 0;
    double worstMs = 0;
    double p95Ms = 0;
    std::string encoderName;
    std::string deviceManagerStatus;
    std::string note;
};

static std::string formatHr(HRESULT hr) {
    char buf[20]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}
struct HardwareMfContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    ComPtr<IMFDXGIDeviceManager> manager;
    HRESULT deviceHr = E_FAIL;
    HRESULT managerHr = E_FAIL;
    HardwareMfContext() {
        D3D_FEATURE_LEVEL feature{};
        deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr,
                                    0, D3D11_SDK_VERSION, &device, &feature,
                                    &deviceContext);
        if (FAILED(deviceHr)) {
            deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &device,
                                        &feature, &deviceContext);
        }
        if (FAILED(deviceHr)) return;
        UINT resetToken = 0;
        managerHr = MFCreateDXGIDeviceManager(&resetToken, &manager);
        if (SUCCEEDED(managerHr)) managerHr = manager->ResetDevice(device.Get(), resetToken);
    }
};
static Result run(const char* name, const GUID& outSubtype,
                  HardwareMfContext* hardware) {
    Result r{};
    r.name = name;
    const int width=config.width, height=config.height, fps=config.fps;
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, outSubtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    // Hardware-only mode must not enumerate generic software MFTs and
    // must not silently fall back to CPU encoding.
    const UINT32 flags = config.hardwareOnly
        ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
        : (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT |
           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);
    const HRESULT found = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
        &inInfo, &outInfo, &activates, &count);
    if (FAILED(found) || count == 0 || !activates) {
        r.note = "no suitable NV12 encoder MFT registered";
        if (activates) CoTaskMemFree(activates);
        return r;
    }
    r.discovered = true;
    for (UINT32 i = 0; i < count && !r.produced; ++i) {
        ComPtr<IMFTransform> transform;
        if (!activates[i] || FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform))) || !transform) continue;
        r.activated = true;
        if (hardware && SUCCEEDED(hardware->managerHr) && hardware->manager) {
            const auto hr = transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(hardware->manager.Get()));
            r.deviceManagerStatus = formatHr(hr);
        } else if (hardware) {
            r.deviceManagerStatus = "hardware D3D11/MF device manager unavailable";
        }
        WCHAR* friendly = nullptr;
        UINT32 friendlyLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) && friendly) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, friendly, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string name(static_cast<size_t>(bytes), '\0');
                WideCharToMultiByte(CP_UTF8, 0, friendly, -1, name.data(), bytes, nullptr, nullptr);
                name.resize(static_cast<size_t>(bytes - 1));
                r.encoderName = name;
            }
            CoTaskMemFree(friendly);
        }
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(transform->GetAttributes(&attrs)) && attrs) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        ComPtr<IMFMediaType> output, input;
        if (FAILED(MFCreateMediaType(&output)) || FAILED(MFCreateMediaType(&input))) continue;
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, outSubtype);
        output->SetUINT32(MF_MT_AVG_BITRATE, config.width>=1920 ? 6000000 : 2500000);
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (!dimensions(output.Get(),width,height,fps) || !dimensions(input.Get(),width,height,fps) ||
            FAILED(transform->SetOutputType(0,output.Get(),0)) ||
            FAILED(transform->SetInputType(0,input.Get(),0))) {
            r.note = "MFT rejected requested NV12 resolution/low-latency configuration";
            continue;
        }
        r.configured = true;
        MFT_OUTPUT_STREAM_INFO outputInfo{};
        transform->GetOutputStreamInfo(0, &outputInfo);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0);
        const size_t imageSize = size_t(width) * size_t(height) * 3 / 2;
        const auto wholeStart = std::chrono::steady_clock::now();
        double totalMs = 0, worst = 0;
        std::vector<double> encodeSamples;
        for (int frame = 0; frame < config.frames; ++frame) {
            const auto tick = std::chrono::steady_clock::now();
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&sample)) ||
                FAILED(MFCreateMemoryBuffer(DWORD(imageSize), &buffer))) break;
            BYTE* data = nullptr; DWORD capacity = 0, length = 0;
            if (FAILED(buffer->Lock(&data,&capacity,&length))) break;
            // Changing synthetic blocks model window motion and text updates.
            // No real desktop image is read; avoid a trivial uniform frame.
            for (int row=0;row<height;++row) {
                const int yoffset=row*width;
                for(int col=0;col<width;++col) {
                    const int block=(col/32 + row/24 + frame*3)%13;
                    data[yoffset+col]=BYTE(28+block*15);
                }
            }
            std::fill(data + width*height, data + imageSize, BYTE(128));
            buffer->Unlock();
            buffer->SetCurrentLength(DWORD(imageSize));
            sample->AddBuffer(buffer.Get());
            const LONGLONG duration = 10000000LL / fps;
            sample->SetSampleTime(LONGLONG(frame) * duration);
            sample->SetSampleDuration(duration);
            DWORD inputFlags=0;
            const HRESULT ready = transform->GetInputStatus(0,&inputFlags);
            const HRESULT inputHr = transform->ProcessInput(0,sample.Get(),0);
            if (FAILED(inputHr)) {
                r.note = "MFT ProcessInput failed hr=" + formatHr(inputHr) +
                         " input_status=" + formatHr(ready) +
                         " flags=" + std::to_string(inputFlags);
                break;
            }
            for (int poll=0;poll<2;++poll) {
                ComPtr<IMFSample> outSample;
                ComPtr<IMFMediaBuffer> outBuffer;
                MFT_OUTPUT_DATA_BUFFER packet{};
                if (!(outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                    if (FAILED(MFCreateSample(&outSample)) ||
                        FAILED(MFCreateMemoryBuffer(std::max<DWORD>(outputInfo.cbSize,1024*1024),&outBuffer))) break;
                    outSample->AddBuffer(outBuffer.Get());
                    packet.pSample=outSample.Get();
                }
                DWORD status=0;
                HRESULT hr=transform->ProcessOutput(0,1,&packet,&status);
                if (hr==MF_E_TRANSFORM_NEED_MORE_INPUT) break;
                if (hr==MF_E_TRANSFORM_STREAM_CHANGE) {
                    transform->GetOutputStreamInfo(0,&outputInfo);
                    continue;
                }
                if (FAILED(hr)) {
                    r.note="MFT ProcessOutput failed";
                    break;
                }
                ComPtr<IMFSample> got=packet.pSample;
                if(!got && outSample)got=outSample;
                ComPtr<IMFMediaBuffer> result;
                DWORD encoded=0;
                if(got && SUCCEEDED(got->ConvertToContiguousBuffer(&result)) && result)
                    result->GetCurrentLength(&encoded);
                if(packet.pEvents) packet.pEvents->Release();
                if(encoded>0) {
                    r.produced=true;
                    ++r.frames;
                }
                break;
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count();
            totalMs+=ms;
            encodeSamples.push_back(ms);
            ++r.attempts;
            worst=std::max(worst,ms);
            if(std::chrono::steady_clock::now()-wholeStart>std::chrono::seconds(4)) {
                r.note="encoder smoke budget exceeded";
                break;
            }
        }
        r.avgMs=totalMs/std::max(1,r.attempts);
        r.worstMs=worst;
        if (!encodeSamples.empty()) {
            std::sort(encodeSamples.begin(), encodeSamples.end());
            const size_t rank = (encodeSamples.size()*95 + 99)/100 - 1;
            r.p95Ms = encodeSamples[rank];
        }
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING,0);
    }
    for(UINT32 i=0;i<count;++i)if(activates[i])activates[i]->Release();
    CoTaskMemFree(activates);
    if(r.produced)r.note="encoded nonempty samples; WebRTC RTP/decode not yet verified";
    else if(r.note.empty())r.note="encoder MFT found but no encoded frame";
    return r;
}

template <class Encoder, class EncodedFrame>
static void exerciseAgentEncoder(const char* codec) {
    const int width=config.width, height=config.height;
    Encoder encoder;
    std::string error;
    if (!encoder.init(width,height,config.fps,width>=1920?6000:2500,false,&error)) {
        std::cout << codec << " Agent encoder: unavailable: " << error << "\n";
        return;
    }
    I420Frame frame{};
    frame.width=width;
    frame.height=height;
    frame.y.assign(size_t(width)*height, 72);
    frame.u.assign(size_t(width)*height/4, 128);
    frame.v.assign(size_t(width)*height/4, 128);
    unsigned int successes=0, empty=0;
    double worstMs=0, totalMs=0;
    int attempts=0;
    for(int i=0;i<config.frames;++i) {
        for (int row=0;row<height;++row) {
            for(int col=0;col<width;++col) {
                const int block=(col/32+row/24+i*3)%13;
                frame.y[size_t(row)*width+col]=uint8_t(28+block*15);
            }
        }
        EncodedFrame sample{};
        const auto started=std::chrono::steady_clock::now();
        const bool ok=encoder.encode(frame,i==0,sample,&error);
        const double ms=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-started).count();
        worstMs=std::max(worstMs,ms);
        totalMs+=ms;
        ++attempts;
        if(!ok && !error.empty()) break;
        if(!sample.data.empty())++successes;
        else ++empty;
    }
    std::cout << codec << " Agent encoder: emitted=" << successes
        << " buffered=" << empty << " attempts=" << attempts
        << " avg_ms=" << totalMs/std::max(1,attempts)
        << " max_ms=" << worstMs << " status="
        << (successes>0?"sample-produced":error.empty()?"no-output":error) << "\n";
    encoder.shutdown();
}

int main(int argc, char** argv) {
    // Strict hardware qualification is a separate, opt-in workload.
    // It never captures the real desktop and does not enable SDP/RTP.
    for (int i=1;i<argc;++i) {
        const std::string arg(argv[i]);
        if (arg=="--1080p") {
            config.width=1920; config.height=1080; config.frames=40;
        } else if (arg=="--hardware-only") {
            config.hardwareOnly=true;
        } else {
            std::cerr << "Usage: windows_codec_matrix.exe [--1080p] [--hardware-only]\n";
            return 2;
        }
    }
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_LITE))) {
        std::cerr<<"Media Foundation unavailable\n";
        return 1;
    }
    HardwareMfContext hardware;
    const auto print=[](const Result& r) {
        std::cout<<r.name<<": detected="<<r.discovered
            <<" active="<<r.activated<<" configured="<<r.configured
            <<" frames="<<r.frames<<" attempts="<<r.attempts<<" avg_ms="<<std::fixed<<std::setprecision(1)
            <<r.avgMs<<" p95_ms="<<r.p95Ms<<" max_ms="<<r.worstMs
            <<" encoder="<<r.encoderName
            <<" d3d_manager="<<r.deviceManagerStatus
            <<" note="<<r.note<<"\n";
    };
    std::cout<<"Synthetic Windows MFT encoder qualification (not browser RTP validation) "
        <<config.width<<"x"<<config.height<<" @"<<config.fps<<"fps frames="<<config.frames
        <<" hardware_only="<<config.hardwareOnly<<"\n";
    std::cout<<"VP8: existing libvpx production encoder; not benchmarked by this MFT-only test\n";
    std::cout<<"D3D11 hardware device="<<formatHr(hardware.deviceHr)
             <<" MF device manager="<<formatHr(hardware.managerHr)<<"\n";
    print(run("H264",subtype('H','2','6','4'),&hardware));
    print(run("H265/HEVC",subtype('H','E','V','C'),&hardware));
    print(run("VP9",subtype('V','P','9','0'),&hardware));
    print(run("AV1",subtype('A','V','0','1'),&hardware));
    if (!config.hardwareOnly) {
        // These encoder wrappers may fall back to software; never count
        // their output as evidence of GPU encoding in hardware-only mode.
        exerciseAgentEncoder<H264MfEncoder,H264EncodedFrame>("H264");
        exerciseAgentEncoder<Av1MfEncoder,Av1EncodedFrame>("AV1");
        exerciseAgentEncoder<H265MfEncoder,H265EncodedFrame>("H265/HEVC");
    }
    MFShutdown();
    if(SUCCEEDED(init)) CoUninitialize();
    return 0; // Hardware absence is a capability result, not CI failure.
}
