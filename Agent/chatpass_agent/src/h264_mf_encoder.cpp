#include "h264_mf_encoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <sstream>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <comdef.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

    static std::string HrToString(HRESULT hr) {
        std::ostringstream oss;
        oss << "0x" << std::hex << static_cast<unsigned long>(hr);
        return oss.str();
    }

    static std::string WideToUtf8(const wchar_t* w) {
        if (!w || !*w) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
        return out;
    }

    static bool SetAttrSize(IMFAttributes* attrs, REFGUID key, UINT32 w, UINT32 h) {
        return SUCCEEDED(MFSetAttributeSize(attrs, key, w, h));
    }

    static bool SetAttrRatio(IMFAttributes* attrs, REFGUID key, UINT32 n, UINT32 d) {
        return SUCCEEDED(MFSetAttributeRatio(attrs, key, n, d));
    }

    static void I420ToNV12(const I420Frame& frame, std::vector<uint8_t>& out) {
        const int w = frame.width;
        const int h = frame.height;
        const int uvW = (w + 1) / 2;
        const int uvH = (h + 1) / 2;
        const size_t ySize = static_cast<size_t>(w) * h;
        const size_t uvSize = static_cast<size_t>(w) * uvH;
        out.resize(ySize + uvSize);

        if (frame.y.size() >= ySize) {
            std::memcpy(out.data(), frame.y.data(), ySize);
        }

        uint8_t* uv = out.data() + ySize;
        for (int row = 0; row < uvH; ++row) {
            for (int col = 0; col < uvW; ++col) {
                const size_t srcIdx = static_cast<size_t>(row) * uvW + col;
                const size_t dstIdx = static_cast<size_t>(row) * w + col * 2;
                uv[dstIdx + 0] = srcIdx < frame.u.size() ? frame.u[srcIdx] : 128;
                if (dstIdx + 1 < uvSize) {
                    uv[dstIdx + 1] = srcIdx < frame.v.size() ? frame.v[srcIdx] : 128;
                }
            }
        }
    }


    static bool EnvEnabled(const char* name) {
        char buf[32]{};
        DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf)) return false;
        std::string v(buf, buf + n);
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    static void AppendH264Dump(const std::vector<uint8_t>& data) {
        static bool enabled = EnvEnabled("HI5_H264_DUMP");
        static int frames = 0;
        if (!enabled || data.empty() || frames >= 180) return;
        CreateDirectoryA("C:\\ProgramData\\Hi5Central", nullptr);
        CreateDirectoryA("C:\\ProgramData\\Hi5Central\\Agent", nullptr);
        CreateDirectoryA("C:\\ProgramData\\Hi5Central\\Agent\\Temp", nullptr);
        std::ofstream f("C:\\ProgramData\\Hi5Central\\Agent\\Temp\\hi5-h264-dump.h264", std::ios::binary | std::ios::app);
        if (f) {
            f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            ++frames;
            if (frames == 1 || frames == 30 || frames == 180) {
                std::cout << "[h264] dump wrote frame_count=" << frames
                    << " path=C:\\ProgramData\\Hi5Central\\Agent\\Temp\\hi5-h264-dump.h264"
                    << " bytes=" << data.size() << "\n";
            }
        }
    }

    static bool ContainsH264Idr(const std::vector<uint8_t>& data) {
        auto nalTypeAt = [](uint8_t b) { return static_cast<int>(b & 0x1F); };
        for (size_t i = 0; i + 4 < data.size(); ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && ((data[i + 2] == 1) || (data[i + 2] == 0 && data[i + 3] == 1))) {
                const size_t nal = (data[i + 2] == 1) ? i + 3 : i + 4;
                if (nal < data.size() && nalTypeAt(data[nal]) == 5) return true;
            }
        }
        // AVCC length-prefixed fallback.
        size_t off = 0;
        while (off + 5 <= data.size()) {
            uint32_t len = (uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) | (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]);
            if (len == 0 || off + 4 + len > data.size()) break;
            if (nalTypeAt(data[off + 4]) == 5) return true;
            off += 4 + len;
        }
        return false;
    }


    static bool ContainsH264NalType(const std::vector<uint8_t>& data, int wantedType) {
        auto checkNal = [&](size_t pos) -> bool {
            return pos < data.size() && int(data[pos] & 0x1F) == wantedType;
            };

        for (size_t i = 0; i + 4 < data.size(); ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                if (checkNal(i + 3)) return true;
            }
            if (i + 5 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
                if (checkNal(i + 4)) return true;
            }
        }

        size_t off = 0;
        while (off + 5 <= data.size()) {
            uint32_t len = (uint32_t(data[off]) << 24) |
                (uint32_t(data[off + 1]) << 16) |
                (uint32_t(data[off + 2]) << 8) |
                uint32_t(data[off + 3]);
            if (len == 0 || off + 4 + len > data.size()) break;
            if (checkNal(off + 4)) return true;
            off += 4 + len;
        }

        return false;
    }

    static void AppendAnnexBNal(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
        if (!data || len == 0) return;
        static const uint8_t startCode[] = { 0x00, 0x00, 0x00, 0x01 };
        out.insert(out.end(), std::begin(startCode), std::end(startCode));
        out.insert(out.end(), data, data + len);
    }

    static std::vector<uint8_t> NormalizeSequenceHeaderToAnnexB(const uint8_t* data, UINT32 len) {
        std::vector<uint8_t> out;
        if (!data || len == 0) return out;

        // Already Annex-B.
        if (len >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
            out.assign(data, data + len);
            return out;
        }

        // AVCDecoderConfigurationRecord. Convert SPS/PPS to Annex-B NAL units so
        // the RTP packetizer can split them normally and the browser receives SPS/PPS
        // before IDR frames. Many Media Foundation H.264 MFTs expose SPS/PPS here
        // instead of repeating them in every encoded sample.
        if (len >= 7 && data[0] == 1) {
            size_t off = 5;
            const uint8_t numSps = data[off++] & 0x1F;
            for (uint8_t i = 0; i < numSps && off + 2 <= len; ++i) {
                const uint16_t spsLen = (uint16_t(data[off]) << 8) | uint16_t(data[off + 1]);
                off += 2;
                if (spsLen == 0 || off + spsLen > len) return out;
                AppendAnnexBNal(out, data + off, spsLen);
                off += spsLen;
            }

            if (off >= len) return out;
            const uint8_t numPps = data[off++];
            for (uint8_t i = 0; i < numPps && off + 2 <= len; ++i) {
                const uint16_t ppsLen = (uint16_t(data[off]) << 8) | uint16_t(data[off + 1]);
                off += 2;
                if (ppsLen == 0 || off + ppsLen > len) return out;
                AppendAnnexBNal(out, data + off, ppsLen);
                off += ppsLen;
            }

            return out;
        }

        // Unknown blob. Keep it raw as a last resort.
        out.assign(data, data + len);
        return out;
    }

    static std::vector<uint8_t> ReadH264SequenceHeader(IMFMediaType* type) {
        std::vector<uint8_t> out;
        if (!type) return out;

        UINT32 blobSize = 0;
        if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobSize)) || blobSize == 0) {
            return out;
        }

        UINT8* blob = nullptr;
        UINT32 blobLen = 0;
        if (SUCCEEDED(type->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &blobLen)) && blob && blobLen > 0) {
            out = NormalizeSequenceHeaderToAnnexB(blob, blobLen);
            CoTaskMemFree(blob);
        }
        return out;
    }

    static std::vector<uint8_t> ReadCurrentH264SequenceHeader(IMFTransform* transform) {
        if (!transform) return {};

        ComPtr<IMFMediaType> currentType;
        if (SUCCEEDED(transform->GetOutputCurrentType(0, &currentType)) && currentType) {
            return ReadH264SequenceHeader(currentType.Get());
        }

        ComPtr<IMFMediaType> availableType;
        if (SUCCEEDED(transform->GetOutputAvailableType(0, 0, &availableType)) && availableType) {
            return ReadH264SequenceHeader(availableType.Get());
        }

        return {};
    }

    static bool H264HasAnnexBStartCode(const std::vector<uint8_t>& data) {
        for (size_t i = 0; i + 4 < data.size(); ++i) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
                return true;
            }
        }
        return false;
    }

    static std::vector<uint8_t> ConvertAvccLengthPrefixedToAnnexB(const std::vector<uint8_t>& data) {
        if (data.empty()) return {};
        if (H264HasAnnexBStartCode(data)) return data;

        std::vector<uint8_t> out;
        size_t off = 0;
        while (off + 4 <= data.size()) {
            const uint32_t len = (uint32_t(data[off]) << 24) |
                (uint32_t(data[off + 1]) << 16) |
                (uint32_t(data[off + 2]) << 8) |
                uint32_t(data[off + 3]);
            off += 4;
            if (len == 0 || off + len > data.size()) {
                out.clear();
                return data;
            }
            AppendAnnexBNal(out, data.data() + off, len);
            off += len;
        }
        if (out.empty()) return data;
        return out;
    }


} // namespace

struct H264MfEncoder::Impl {
    struct PendingInputMeta {
        bool forceKeyframe = false;
        uint32_t timestamp90k = 0;
        int gpuSurfaceSlot = -1;
    };

    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> eventGenerator;
    DWORD inputStreamId = 0;
    DWORD outputStreamId = 0;
    MFT_OUTPUT_STREAM_INFO outputInfo{};
    ComPtr<IMFSample> outputSample;
    ComPtr<IMFMediaBuffer> outputBuffer;
    DWORD outputBufferSize = 0;
    std::vector<uint8_t> nv12;
    std::vector<uint8_t> sequenceHeaderAnnexB;
    std::deque<PendingInputMeta> pendingInputs;
    std::deque<H264EncodedFrame> pendingOutputs;
    int needInputCredits = 0;
    uint64_t noCreditDrops = 0;
    int consecutiveNoCreditDrops = 0;
    uint64_t unexpectedOutputWaits = 0;
    int consecutiveGpuPoolBusy = 0;
    bool asyncMode = false;
    bool sequenceHeaderLogged = false;
    bool mfStarted = false;
    struct GpuOpenedFrame { ComPtr<ID3D11Texture2D> texture; ComPtr<IDXGIKeyedMutex> mutex; };
    ComPtr<ID3D11Device> gpuDevice;
    ComPtr<ID3D11Device1> gpuDevice1;
    ComPtr<ID3D11DeviceContext> gpuContext;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11Texture2D> localBgra;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnumerator;
    ComPtr<ID3D11VideoProcessor> videoProcessor;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    std::array<ComPtr<ID3D11Texture2D>, 3> nv12Surfaces;
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, 3> outputViews;
    std::array<bool, 3> surfaceInUse{ false, false, false };
    size_t nextSurface = 0;
    std::unordered_map<uint64_t, GpuOpenedFrame> openedGpuFrames;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    UINT dxgiManagerToken = 0;
    LUID gpuAdapterLuid{};
    DXGI_FORMAT gpuInputFormat = DXGI_FORMAT_UNKNOWN;
    bool gpuReady = false;

    bool SetupGpu(const SharedGpuFrame& first, int fps, std::string* error) {
        gpuInputFormat = static_cast<DXGI_FORMAT>(first.dxgiFormat);
        ComPtr<IDXGIFactory1> factory; HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) { if (error) *error = "CreateDXGIFactory1 failed " + HrToString(hr); return false; }
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate; if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; if (FAILED(candidate->GetDesc1(&desc))) continue;
            if (desc.AdapterLuid.LowPart == first.adapterLuidLow && desc.AdapterLuid.HighPart == first.adapterLuidHigh) { adapter = candidate; gpuAdapterLuid = desc.AdapterLuid; break; }
        }
        if (!adapter) { if (error) *error = "matching D3D11 adapter not found"; return false; }
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &level, 1, D3D11_SDK_VERSION, &gpuDevice, nullptr, &gpuContext);
        if (FAILED(hr) || !gpuDevice || !gpuContext) { if (error) *error = "D3D11CreateDevice(video) failed " + HrToString(hr); return false; }
        if (FAILED(gpuDevice.As(&gpuDevice1)) || FAILED(gpuDevice.As(&videoDevice)) || FAILED(gpuContext.As(&videoContext))) { if (error) *error = "D3D11 video interfaces unavailable"; return false; }
        D3D11_TEXTURE2D_DESC bgra{}; bgra.Width = first.width; bgra.Height = first.height; bgra.MipLevels = 1; bgra.ArraySize = 1; bgra.Format = gpuInputFormat; bgra.SampleDesc.Count = 1; bgra.Usage = D3D11_USAGE_DEFAULT; bgra.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = gpuDevice->CreateTexture2D(&bgra, nullptr, &localBgra); if (FAILED(hr)) { if (error) *error = "CreateTexture2D(local BGRA) failed " + HrToString(hr); return false; }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{}; content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; content.InputFrameRate.Numerator = fps; content.InputFrameRate.Denominator = 1; content.InputWidth = first.width; content.InputHeight = first.height; content.OutputFrameRate.Numerator = fps; content.OutputFrameRate.Denominator = 1; content.OutputWidth = first.width; content.OutputHeight = first.height; content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = videoDevice->CreateVideoProcessorEnumerator(&content, &vpEnumerator); if (FAILED(hr) || !vpEnumerator) { if (error) *error = "CreateVideoProcessorEnumerator failed " + HrToString(hr); return false; }
        UINT support = 0; hr = vpEnumerator->CheckVideoProcessorFormat(gpuInputFormat, &support); if (FAILED(hr) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) { if (error) *error = "video processor lacks desktop input support"; return false; }
        support = 0; hr = vpEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support); if (FAILED(hr) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) { if (error) *error = "video processor lacks NV12 output support"; return false; }
        hr = videoDevice->CreateVideoProcessor(vpEnumerator.Get(), 0, &videoProcessor); if (FAILED(hr) || !videoProcessor) { if (error) *error = "CreateVideoProcessor failed " + HrToString(hr); return false; }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; iv.Texture2D.MipSlice = 0; iv.Texture2D.ArraySlice = 0;
        hr = videoDevice->CreateVideoProcessorInputView(localBgra.Get(), vpEnumerator.Get(), &iv, &inputView); if (FAILED(hr) || !inputView) { if (error) *error = "CreateVideoProcessorInputView failed " + HrToString(hr); return false; }
        for (size_t i = 0; i < nv12Surfaces.size(); ++i) {
            D3D11_TEXTURE2D_DESC nv{}; nv.Width = first.width; nv.Height = first.height; nv.MipLevels = 1; nv.ArraySize = 1; nv.Format = DXGI_FORMAT_NV12; nv.SampleDesc.Count = 1; nv.Usage = D3D11_USAGE_DEFAULT; nv.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = gpuDevice->CreateTexture2D(&nv, nullptr, &nv12Surfaces[i]); if (FAILED(hr) || !nv12Surfaces[i]) { if (error) *error = "CreateTexture2D(NV12) failed " + HrToString(hr); return false; }
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{}; ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D; ov.Texture2D.MipSlice = 0;
            hr = videoDevice->CreateVideoProcessorOutputView(nv12Surfaces[i].Get(), vpEnumerator.Get(), &ov, &outputViews[i]); if (FAILED(hr) || !outputViews[i]) { if (error) *error = "CreateVideoProcessorOutputView failed " + HrToString(hr); return false; }
        }
        hr = MFCreateDXGIDeviceManager(&dxgiManagerToken, &dxgiManager); if (FAILED(hr) || !dxgiManager) { if (error) *error = "MFCreateDXGIDeviceManager failed " + HrToString(hr); return false; }
        hr = dxgiManager->ResetDevice(gpuDevice.Get(), dxgiManagerToken); if (FAILED(hr)) { if (error) *error = "ResetDevice failed " + HrToString(hr); return false; }
        gpuReady = true; return true;
    }

    bool MakeGpuSample(const SharedGpuFrame& frame, int width, int height, uint64_t inputIndex, int fps, ComPtr<IMFSample>& sample, int& slot, std::string* error) {
        slot = -1; if (!gpuReady || !gpuDevice1 || !videoContext) { if (error) *error = "GPU pipeline not ready"; return false; }
        if (frame.width != width || frame.height != height || frame.adapterLuidLow != gpuAdapterLuid.LowPart || frame.adapterLuidHigh != gpuAdapterLuid.HighPart) { if (error) *error = "shared GPU frame geometry/adapter changed"; return false; }
        for (size_t n = 0; n < surfaceInUse.size(); ++n) { size_t candidate = (nextSurface + n) % surfaceInUse.size(); if (!surfaceInUse[candidate]) { slot = static_cast<int>(candidate); nextSurface = (candidate + 1) % surfaceInUse.size(); break; } }
        if (slot < 0) return true;
        auto it = openedGpuFrames.find(frame.sharedHandle);
        if (it == openedGpuFrames.end()) {
            GpuOpenedFrame opened; HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frame.sharedHandle)); HRESULT hr = gpuDevice1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened.texture));
            if (FAILED(hr) || !opened.texture) { if (error) *error = "OpenSharedResource1 failed " + HrToString(hr); return false; }
            hr = opened.texture.As(&opened.mutex); if (FAILED(hr) || !opened.mutex) { if (error) *error = "shared texture missing keyed mutex " + HrToString(hr); return false; }
            it = openedGpuFrames.emplace(frame.sharedHandle, std::move(opened)).first;
        }
        HRESULT hr = it->second.mutex->AcquireSync(frame.syncKey, 4); if (hr == WAIT_TIMEOUT) { slot = -1; return true; } if (hr != S_OK) { if (error) *error = "AcquireSync failed " + HrToString(hr); return false; }
        gpuContext->CopyResource(localBgra.Get(), it->second.texture.Get()); HRESULT releaseHr = it->second.mutex->ReleaseSync(0); if (releaseHr != S_OK) { if (error) *error = "ReleaseSync failed " + HrToString(releaseHr); return false; }
        RECT rect{0,0,width,height}; videoContext->VideoProcessorSetStreamFrameFormat(videoProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE); videoContext->VideoProcessorSetStreamSourceRect(videoProcessor.Get(), 0, TRUE, &rect); videoContext->VideoProcessorSetStreamDestRect(videoProcessor.Get(), 0, TRUE, &rect); videoContext->VideoProcessorSetOutputTargetRect(videoProcessor.Get(), TRUE, &rect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = inputView.Get(); hr = videoContext->VideoProcessorBlt(videoProcessor.Get(), outputViews[slot].Get(), 0, 1, &stream); if (FAILED(hr)) { if (error) *error = "VideoProcessorBlt BGRA->NV12 failed " + HrToString(hr); return false; }
        ComPtr<IMFMediaBuffer> dxgiBuffer; hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Surfaces[slot].Get(), 0, FALSE, &dxgiBuffer); if (FAILED(hr) || !dxgiBuffer) { if (error) *error = "MFCreateDXGISurfaceBuffer failed " + HrToString(hr); return false; }
        hr = MFCreateSample(&sample); if (FAILED(hr) || !sample || FAILED(sample->AddBuffer(dxgiBuffer.Get()))) { if (error) *error = "MFCreateSample/AddBuffer(DXGI) failed " + HrToString(hr); return false; }
        LONGLONG frameTime = static_cast<LONGLONG>((10000000.0 * static_cast<double>(inputIndex)) / static_cast<double>(fps)); LONGLONG frameDuration = static_cast<LONGLONG>(10000000.0 / static_cast<double>(fps)); sample->SetSampleTime(frameTime); sample->SetSampleDuration(frameDuration);
        return true;
    }
};

H264MfEncoder::H264MfEncoder() = default;

H264MfEncoder::~H264MfEncoder() {
    shutdown();
}

void H264MfEncoder::shutdown() {
    if (m_impl) {
        if (m_impl->transform) {
            m_impl->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            if (m_impl->dxgiManager) m_impl->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        delete m_impl;
        m_impl = nullptr;
        // Balance the MFStartup() performed by init(); otherwise repeated
        // codec switches retain Media Foundation runtime resources indefinitely.
        MFShutdown();
    }
    m_open = false;
    m_gpuMode = false;
}

bool H264MfEncoder::init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error) {
    return initInternal(width, height, fps, bitrateKbps, preferHardware, nullptr, error);
}

bool H264MfEncoder::initGpu(const SharedGpuFrame& frame, int fps, int bitrateKbps, std::string* error) {
    return initInternal(frame.width, frame.height, fps, bitrateKbps, true, &frame, error);
}

bool H264MfEncoder::initInternal(int width, int height, int fps, int bitrateKbps, bool preferHardware, const SharedGpuFrame* gpuFrame, std::string* error) {
    shutdown();

    if (width <= 0 || height <= 0) {
        if (error) *error = "invalid size";
        return false;
    }

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        if (error) *error = "MFStartup failed " + HrToString(hr);
        return false;
    }

    m_impl = new Impl();
    m_gpuMode = gpuFrame != nullptr;
    if (m_gpuMode && !m_impl->SetupGpu(*gpuFrame, std::max(1, fps), error)) return false;
    m_width = width;
    m_height = height;
    m_fps = std::max(1, fps);
    m_bitrateKbps = std::max(300, bitrateKbps);
    m_frameIndex = 0;

    MFT_REGISTER_TYPE_INFO inInfo{};
    inInfo.guidMajorType = MFMediaType_Video;
    inInfo.guidSubtype = MFVideoFormat_NV12;

    MFT_REGISTER_TYPE_INFO outInfo{};
    outInfo.guidMajorType = MFMediaType_Video;
    outInfo.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;
    if (preferHardware) flags |= MFT_ENUM_FLAG_HARDWARE;
    else flags |= MFT_ENUM_FLAG_SYNCMFT;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo, &activates, &count);
    if (!m_gpuMode && (FAILED(hr) || count == 0)) {
        // Retry without the strict hardware flag; this is still experimental and
        // should fall back cleanly to VP8 at the caller if no MFT is usable.
        if (activates) {
            CoTaskMemFree(activates);
            activates = nullptr;
        }
        count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
            &inInfo, &outInfo, &activates, &count);
    }

    if (FAILED(hr) || count == 0 || !activates) {
        if (error) *error = "MFTEnumEx H.264 encoder failed " + HrToString(hr);
        return false;
    }

    ComPtr<IMFActivate> chosen;
    chosen.Attach(activates[0]);
    for (UINT32 i = 1; i < count; ++i) {
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);

    WCHAR* friendly = nullptr;
    UINT32 friendlyLen = 0;
    if (SUCCEEDED(chosen->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) && friendly) {
        m_encoderName = WideToUtf8(friendly);
        CoTaskMemFree(friendly);
    }
    if (m_encoderName.empty()) m_encoderName = "Media Foundation H.264 Encoder";

    hr = chosen->ActivateObject(IID_PPV_ARGS(&m_impl->transform));
    if (FAILED(hr) || !m_impl->transform) {
        if (error) *error = "ActivateObject H.264 MFT failed " + HrToString(hr);
        return false;
    }

    // MFT hardware encoders are commonly async. Unlock async mode and ask for
    // low latency where supported. Failures are non-fatal because some MFTs do
    // not expose all attributes.
    {
        ComPtr<IMFAttributes> mftAttrs;
        if (SUCCEEDED(m_impl->transform->GetAttributes(&mftAttrs)) && mftAttrs) {
            mftAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            mftAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
    }

    if (m_gpuMode) {
        ComPtr<IMFAttributes> gpuAttrs; UINT32 aware = FALSE;
        if (FAILED(m_impl->transform->GetAttributes(&gpuAttrs)) || !gpuAttrs || FAILED(gpuAttrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware) { if (error) *error = "hardware H.264 MFT is not D3D11-aware"; return false; }
        hr = m_impl->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_impl->dxgiManager.Get()));
        if (FAILED(hr)) { if (error) *error = "MFT_MESSAGE_SET_D3D_MANAGER failed " + HrToString(hr); return false; }
        std::cout << "[h264-gpu] D3D11 device manager attached before media types\n";
    }

    // CodecAPI is intentionally disabled in this build because some Windows SDK/MSVC
    // combinations do not expose ICodecAPI/CODECAPI_* cleanly. The encoder still
    // uses Media Foundation output/input media types, low-latency MFT attributes,
    // SPS/PPS extraction, AVCC->Annex-B normalization, and VP8 fallback.

    std::cout << "[h264] MFT activated encoder=" << m_encoderName
        << " size=" << m_width << "x" << m_height
        << " fps=" << m_fps
        << " bitrate=" << m_bitrateKbps
        << " prefer_hw=" << (preferHardware ? 1 : 0)
        << " codecapi=disabled" << "\n";

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(m_bitrateKbps * 1000));
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    SetAttrSize(outType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(m_width), static_cast<UINT32>(m_height));
    SetAttrRatio(outType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
    SetAttrRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // Baseline profile_idc (66) without codecapi.h enum dependency.
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, 66);

    hr = m_impl->transform->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        if (error) *error = "SetOutputType H.264 failed " + HrToString(hr);
        return false;
    }

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    SetAttrSize(inType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(m_width), static_cast<UINT32>(m_height));
    SetAttrRatio(inType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
    SetAttrRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_impl->transform->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        if (error) *error = "SetInputType NV12 failed " + HrToString(hr);
        return false;
    }

    m_impl->transform->GetOutputStreamInfo(0, &m_impl->outputInfo);
    m_impl->sequenceHeaderAnnexB = ReadCurrentH264SequenceHeader(m_impl->transform.Get());

    std::cout << "[h264] output stream info cbSize=" << m_impl->outputInfo.cbSize
        << " flags=" << m_impl->outputInfo.dwFlags
        << " sequence_header_bytes=" << m_impl->sequenceHeaderAnnexB.size()
        << "\n";

    m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_impl->mfStarted = true;

    // Hardware Media Foundation encoders are commonly asynchronous. Once
    // MF_TRANSFORM_ASYNC_UNLOCK is enabled they must be driven by the
    // IMFMediaEventGenerator NeedInput/HaveOutput handshake rather than by a
    // synchronous ProcessInput -> ProcessOutput poll. QueryInterface is the
    // authoritative test; genuinely synchronous MFTs simply keep the legacy path.
    ComPtr<IMFMediaEventGenerator> eventGenerator;
    if (SUCCEEDED(m_impl->transform.As(&eventGenerator)) && eventGenerator) {
        m_impl->eventGenerator = eventGenerator;
        m_impl->asyncMode = true;
    }
    std::cout << "[h264] async_event_model=" << (m_impl->asyncMode ? 1 : 0) << " gpu_input=" << (m_gpuMode ? 1 : 0) << "\n";

    m_open = true;
    return true;
}

static bool CopyI420ToNV12Buffer(const I420Frame& frame, BYTE* dst, DWORD maxLen, DWORD* written) {
    if (!dst || frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    const int width = frame.width;
    const int height = frame.height;
    const int uvWidth = (width + 1) / 2;
    const int uvHeight = (height + 1) / 2;

    const size_t ySize = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t uvInterleavedSize = static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight) * 2;
    const size_t required = ySize + uvInterleavedSize;

    if (required > static_cast<size_t>(maxLen)) {
        return false;
    }

    if (frame.y.size() < ySize ||
        frame.u.size() < static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight) ||
        frame.v.size() < static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight)) {
        return false;
    }

    std::memcpy(dst, frame.y.data(), ySize);

    BYTE* uv = dst + ySize;
    for (int row = 0; row < uvHeight; ++row) {
        for (int col = 0; col < uvWidth; ++col) {
            const size_t srcIndex = static_cast<size_t>(row) * static_cast<size_t>(uvWidth) + static_cast<size_t>(col);
            const size_t dstIndex = (static_cast<size_t>(row) * static_cast<size_t>(uvWidth) + static_cast<size_t>(col)) * 2;

            uv[dstIndex + 0] = frame.u[srcIndex];
            uv[dstIndex + 1] = frame.v[srcIndex];
        }
    }

    if (written) {
        *written = static_cast<DWORD>(required);
    }

    return true;
}

bool H264MfEncoder::encode(const I420Frame& frame, bool forceKeyframe, H264EncodedFrame& out, std::string* error) {
    return encodeInternal(&frame, nullptr, forceKeyframe, out, error);
}

bool H264MfEncoder::encodeGpu(const SharedGpuFrame& frame, bool forceKeyframe, H264EncodedFrame& out, std::string* error) {
    return encodeInternal(nullptr, &frame, forceKeyframe, out, error);
}

bool H264MfEncoder::encodeInternal(const I420Frame* frame, const SharedGpuFrame* gpuFrame, bool forceKeyframe, H264EncodedFrame& out, std::string* error) {
    out = {};
    if (!m_open || !m_impl || !m_impl->transform) {
        if (error) *error = "encoder not open";
        return false;
    }

    const int sourceWidth = gpuFrame ? gpuFrame->width : (frame ? frame->width : 0);
    const int sourceHeight = gpuFrame ? gpuFrame->height : (frame ? frame->height : 0);
    if (sourceWidth != m_width || sourceHeight != m_height || (m_gpuMode != (gpuFrame != nullptr))) {
        if (error) *error = "frame size/input mode changed";
        return false;
    }

    auto finalizeOutput = [&](IMFSample* got, H264EncodedFrame& encoded) -> bool {
        if (!got) return true;

        ComPtr<IMFMediaBuffer> contiguous;
        HRESULT hr = got->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr) || !contiguous) {
            if (error) *error = "ConvertToContiguousBuffer failed " + HrToString(hr);
            return false;
        }

        BYTE* p = nullptr;
        DWORD max = 0;
        DWORD len = 0;
        hr = contiguous->Lock(&p, &max, &len);
        if (FAILED(hr)) {
            if (error) *error = "output buffer Lock failed " + HrToString(hr);
            return false;
        }
        if (len > 0 && p) encoded.data.assign(p, p + len);
        contiguous->Unlock();

        Impl::PendingInputMeta meta{};
        if (!m_impl->pendingInputs.empty()) {
            meta = m_impl->pendingInputs.front();
            m_impl->pendingInputs.pop_front();
        }
        if (meta.gpuSurfaceSlot >= 0 && meta.gpuSurfaceSlot < static_cast<int>(m_impl->surfaceInUse.size())) {
            m_impl->surfaceInUse[meta.gpuSurfaceSlot] = false;
            m_impl->consecutiveGpuPoolBusy = 0;
        }

        if (encoded.data.empty()) {
            encoded.timestamp90k = meta.timestamp90k;
            return true;
        }

        encoded.data = ConvertAvccLengthPrefixedToAnnexB(encoded.data);
        encoded.keyframe = ContainsH264Idr(encoded.data) || meta.forceKeyframe;
        encoded.timestamp90k = meta.timestamp90k;

        if (encoded.keyframe && !m_impl->sequenceHeaderAnnexB.empty() &&
            (!ContainsH264NalType(encoded.data, 7) || !ContainsH264NalType(encoded.data, 8))) {
            std::vector<uint8_t> withHeader;
            withHeader.reserve(m_impl->sequenceHeaderAnnexB.size() + encoded.data.size());
            withHeader.insert(withHeader.end(), m_impl->sequenceHeaderAnnexB.begin(), m_impl->sequenceHeaderAnnexB.end());
            withHeader.insert(withHeader.end(), encoded.data.begin(), encoded.data.end());
            encoded.data.swap(withHeader);
        }

        if (!m_impl->sequenceHeaderLogged) {
            m_impl->sequenceHeaderLogged = true;
            std::cout << "[h264] sequence header bytes=" << m_impl->sequenceHeaderAnnexB.size()
                << " keyframe=" << (encoded.keyframe ? 1 : 0)
                << " has_sps=" << (ContainsH264NalType(encoded.data, 7) ? 1 : 0)
                << " has_pps=" << (ContainsH264NalType(encoded.data, 8) ? 1 : 0)
                << " has_idr=" << (ContainsH264NalType(encoded.data, 5) ? 1 : 0)
                << " bytes=" << encoded.data.size() << "\n";
        }
        AppendH264Dump(encoded.data);
        return true;
    };

    auto drainOneOutput = [&](H264EncodedFrame& encoded, bool& produced) -> bool {
        produced = false;
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER output{};
            DWORD status = 0;
            HRESULT hr = S_OK;

            if (!(m_impl->outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                const DWORD outputBytes = std::max<DWORD>(m_impl->outputInfo.cbSize, 1024 * 1024);
                if (!m_impl->outputSample || !m_impl->outputBuffer || m_impl->outputBufferSize < outputBytes) {
                    m_impl->outputSample.Reset();
                    m_impl->outputBuffer.Reset();
                    hr = MFCreateSample(&m_impl->outputSample);
                    if (FAILED(hr)) {
                        if (error) *error = "MFCreateSample output failed " + HrToString(hr);
                        return false;
                    }
                    hr = MFCreateMemoryBuffer(outputBytes, &m_impl->outputBuffer);
                    if (FAILED(hr)) {
                        if (error) *error = "MFCreateMemoryBuffer output failed " + HrToString(hr);
                        return false;
                    }
                    hr = m_impl->outputSample->AddBuffer(m_impl->outputBuffer.Get());
                    if (FAILED(hr)) {
                        if (error) *error = "output sample AddBuffer failed " + HrToString(hr);
                        return false;
                    }
                    m_impl->outputBufferSize = outputBytes;
                }
                hr = m_impl->outputBuffer->SetCurrentLength(0);
                if (FAILED(hr)) {
                    if (error) *error = "output buffer reset failed " + HrToString(hr);
                    return false;
                }
                output.pSample = m_impl->outputSample.Get();
            }

            hr = m_impl->transform->ProcessOutput(0, 1, &output, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (output.pEvents) output.pEvents->Release();
                return true;
            }
            // Intel Quick Sync async MFTs can transiently return E_UNEXPECTED
            // even after METransformHaveOutput. This means the output surface is
            // not ready yet; it is not an encoder failure. Keep the accepted
            // input/surface queued and wait for the next output event.
            if (m_impl->asyncMode && hr == E_UNEXPECTED) {
                if (output.pEvents) output.pEvents->Release();
                ++m_impl->unexpectedOutputWaits;
                if (m_impl->unexpectedOutputWaits == 1 || (m_impl->unexpectedOutputWaits % 120) == 0) {
                    std::cout << "[h264] async ProcessOutput E_UNEXPECTED; output not ready count="
                        << m_impl->unexpectedOutputWaits << "\n";
                }
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (output.pEvents) output.pEvents->Release();
                ComPtr<IMFMediaType> newOut;
                if (SUCCEEDED(m_impl->transform->GetOutputAvailableType(0, 0, &newOut)) && newOut) {
                    m_impl->transform->SetOutputType(0, newOut.Get(), 0);
                }
                m_impl->transform->GetOutputStreamInfo(0, &m_impl->outputInfo);
                m_impl->sequenceHeaderAnnexB = ReadCurrentH264SequenceHeader(m_impl->transform.Get());
                std::cout << "[h264] stream change handled sequence_header_bytes="
                    << m_impl->sequenceHeaderAnnexB.size() << "\n";
                continue;
            }
            if (FAILED(hr)) {
                if (output.pEvents) output.pEvents->Release();
                if (error) *error = "ProcessOutput failed " + HrToString(hr);
                return false;
            }

            m_impl->unexpectedOutputWaits = 0;
            ComPtr<IMFSample> got;
            if (output.pSample) {
                if (m_impl->outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
                    // ProcessOutput transfers a caller-owned reference for MFT-provided
                    // samples. Attach consumes that reference without AddRef, so it is
                    // released exactly once when got leaves scope.
                    got.Attach(output.pSample);
                } else {
                    // This is our reusable sample; keep its owning reference in Impl and
                    // take a temporary AddRef while extracting this output.
                    got = output.pSample;
                }
            }
            if (output.pEvents) output.pEvents->Release();
            if (!got) return true;
            if (!finalizeOutput(got.Get(), encoded)) return false;
            produced = !encoded.data.empty();
            return true;
        }
    };

    auto pumpAsyncEvents = [&](bool waitForInputCredit) -> bool {
        if (!m_impl->asyncMode || !m_impl->eventGenerator) return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
        for (;;) {
            bool sawEvent = false;
            for (;;) {
                ComPtr<IMFMediaEvent> event;
                HRESULT hr = m_impl->eventGenerator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
                if (hr == MF_E_NO_EVENTS_AVAILABLE) break;
                if (FAILED(hr)) {
                    if (error) *error = "IMFMediaEventGenerator::GetEvent failed " + HrToString(hr);
                    return false;
                }
                if (!event) break;
                sawEvent = true;

                MediaEventType type = MEUnknown;
                HRESULT eventStatus = S_OK;
                event->GetType(&type);
                event->GetStatus(&eventStatus);
                if (FAILED(eventStatus)) {
                    if (error) *error = "async MFT event failed " + HrToString(eventStatus);
                    return false;
                }
                if (type == METransformNeedInput) {
                    ++m_impl->needInputCredits;
                } else if (type == METransformHaveOutput) {
                    H264EncodedFrame encoded;
                    bool produced = false;
                    if (!drainOneOutput(encoded, produced)) return false;
                    if (produced) m_impl->pendingOutputs.push_back(std::move(encoded));
                } else if (type == MEError) {
                    if (error) *error = "async MFT emitted MEError";
                    return false;
                }
            }

            if (!waitForInputCredit || m_impl->needInputCredits > 0 ||
                std::chrono::steady_clock::now() >= deadline) break;

            SwitchToThread();
            if (!sawEvent) YieldProcessor();
        }
        return true;
    };

    if (m_impl->asyncMode) {
        if (!pumpAsyncEvents(true)) return false;
        if (m_impl->needInputCredits <= 0) {
            ++m_impl->noCreditDrops;
            ++m_impl->consecutiveNoCreditDrops;
            if (m_impl->noCreditDrops == 1 || (m_impl->noCreditDrops % 120) == 0) {
                std::cout << "[h264] async no-input-credit drop count=" << m_impl->noCreditDrops
                    << " consecutive=" << m_impl->consecutiveNoCreditDrops << "\n";
            }
            if (m_impl->consecutiveNoCreditDrops >= 8) {
                if (error) *error = "async MFT stalled waiting for METransformNeedInput";
                return false;
            }
            if (!m_impl->pendingOutputs.empty()) {
                out = std::move(m_impl->pendingOutputs.front());
                m_impl->pendingOutputs.pop_front();
            }
            return true;
        }
    }

    ComPtr<IMFSample> sample;
    HRESULT hr = S_OK;
    int gpuSurfaceSlot = -1;
    const uint64_t inputIndex = m_frameIndex;
    if (gpuFrame) {
        if (!m_impl->MakeGpuSample(*gpuFrame, m_width, m_height, inputIndex, m_fps, sample, gpuSurfaceSlot, error)) return false;
        if (!sample) {
            ++m_impl->consecutiveGpuPoolBusy;
            if (m_impl->consecutiveGpuPoolBusy == 1) {
                std::cout << "[h264-gpu] NV12 surface pool busy; dropping capture frame\n";
            }
            if (m_impl->consecutiveGpuPoolBusy >= 12) {
                if (error) *error = "GPU NV12 surface pool stalled waiting for async encoder output";
                return false;
            }
            return true;
        }
        m_impl->consecutiveGpuPoolBusy = 0;
    } else {
        const int uvWidth = (frame->width + 1) / 2; const int uvHeight = (frame->height + 1) / 2;
        const size_t ySize = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height); const size_t uvPlaneSize = static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight); const size_t nv12Size = ySize + (uvPlaneSize * 2);
        hr = MFCreateSample(&sample); if (FAILED(hr)) { if (error) *error = "MFCreateSample failed " + HrToString(hr); return false; }
        ComPtr<IMFMediaBuffer> buffer; hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12Size), &buffer); if (FAILED(hr)) { if (error) *error = "MFCreateMemoryBuffer failed " + HrToString(hr); return false; }
        BYTE* dst = nullptr; DWORD maxLen = 0, curLen = 0; hr = buffer->Lock(&dst, &maxLen, &curLen); if (FAILED(hr)) { if (error) *error = "input buffer Lock failed " + HrToString(hr); return false; }
        DWORD written = 0; const bool copied = CopyI420ToNV12Buffer(*frame, dst, maxLen, &written); buffer->Unlock();
        if (!copied) { if (error) *error = "I420->NV12 copy failed"; return false; }
        if (FAILED(buffer->SetCurrentLength(written)) || FAILED(sample->AddBuffer(buffer.Get()))) { if (error) *error = "input sample buffer attach failed"; return false; }
    }
    const LONGLONG frameTime = static_cast<LONGLONG>((10'000'000.0 * static_cast<double>(inputIndex)) / static_cast<double>(m_fps));
    const LONGLONG frameDuration = static_cast<LONGLONG>(10'000'000.0 / static_cast<double>(m_fps));
    sample->SetSampleTime(frameTime);
    sample->SetSampleDuration(frameDuration);

    if (gpuSurfaceSlot >= 0) m_impl->surfaceInUse[gpuSurfaceSlot] = true;
    hr = m_impl->transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        if (gpuSurfaceSlot >= 0) m_impl->surfaceInUse[gpuSurfaceSlot] = false;
        if (error) *error = std::string(m_gpuMode ? "ProcessInput(DXGI NV12) failed " : "ProcessInput failed ") + HrToString(hr);
        return false;
    }

    if (m_impl->asyncMode) {
        --m_impl->needInputCredits;
        m_impl->consecutiveNoCreditDrops = 0;
    }
    Impl::PendingInputMeta meta{};
    meta.forceKeyframe = forceKeyframe;
    meta.timestamp90k = static_cast<uint32_t>((inputIndex * 90000ULL) / static_cast<uint64_t>(m_fps));
    meta.gpuSurfaceSlot = gpuSurfaceSlot;
    m_impl->pendingInputs.push_back(meta);
    ++m_frameIndex;

    if (m_impl->asyncMode) {
        if (!pumpAsyncEvents(false)) return false;
        if (!m_impl->pendingOutputs.empty()) {
            out = std::move(m_impl->pendingOutputs.front());
            m_impl->pendingOutputs.pop_front();
        }
        return true;
    }

    bool produced = false;
    if (!drainOneOutput(out, produced)) return false;
    return true;
}
