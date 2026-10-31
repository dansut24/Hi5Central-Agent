#include "av1_mf_encoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include "mf_async_pilot_events.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <comdef.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

    // Windows Media Foundation AV1 = FOURCC 'AV01' in the MF subtype GUID.
    // Device/OS availability is determined at runtime via MFTEnumEx.
    const GUID kAv1Subtype{0x31305641,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

    static std::string HrToString(HRESULT hr) {
        std::ostringstream oss;
        oss << "0x" << std::hex << static_cast<unsigned long>(hr);
        return oss.str();
    }

    static std::string WideToUtf8(const wchar_t* w) {
        if (!w || !*w) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        // Include the trailing NUL in the writable allocation. The old
        // len - 1 allocation was a one-byte overwrite on every name probe.
        std::string out(static_cast<size_t>(len), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr) != len)
            return {};
        out.resize(static_cast<size_t>(len - 1));
        return out;
    }

    static bool SetAttrSize(IMFAttributes* attrs, REFGUID key, UINT32 w, UINT32 h) {
        return SUCCEEDED(MFSetAttributeSize(attrs, key, w, h));
    }

    static bool SetAttrRatio(IMFAttributes* attrs, REFGUID key, UINT32 n, UINT32 d) {
        return SUCCEEDED(MFSetAttributeRatio(attrs, key, n, d));
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
        const size_t uvPlaneSize = static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight);
        const size_t uvInterleavedSize = uvPlaneSize * 2;
        const size_t required = ySize + uvInterleavedSize;

        if (required > static_cast<size_t>(maxLen)) {
            return false;
        }

        if (frame.y.size() < ySize ||
            frame.u.size() < uvPlaneSize ||
            frame.v.size() < uvPlaneSize) {
            return false;
        }

        std::memcpy(dst, frame.y.data(), ySize);

        BYTE* uv = dst + ySize;
        for (int row = 0; row < uvHeight; ++row) {
            for (int col = 0; col < uvWidth; ++col) {
                const size_t srcIndex =
                    static_cast<size_t>(row) * static_cast<size_t>(uvWidth) +
                    static_cast<size_t>(col);

                const size_t dstIndex = srcIndex * 2;
                uv[dstIndex + 0] = frame.u[srcIndex];
                uv[dstIndex + 1] = frame.v[srcIndex];
            }
        }

        if (written) {
            *written = static_cast<DWORD>(required);
        }

        return true;
    }

} // namespace

struct Av1MfEncoder::Impl {
    ComPtr<IMFTransform> transform;
    bool asynchronous = false;
    MfAsyncPilotEvents events;
    MFT_OUTPUT_STREAM_INFO outputInfo{};
    bool mfStarted = false;
};

Av1MfEncoder::Av1MfEncoder() = default;

Av1MfEncoder::~Av1MfEncoder() {
    shutdown();
}

void Av1MfEncoder::shutdown() {
    if (m_impl) {
        if (m_impl->transform) {
            m_impl->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        }

        delete m_impl;
        m_impl = nullptr;
    }

    m_open = false;
}

bool Av1MfEncoder::init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error) {
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
    outInfo.guidSubtype = kAv1Subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    UINT32 flags =
        MFT_ENUM_FLAG_SORTANDFILTER |
        MFT_ENUM_FLAG_LOCALMFT |
        MFT_ENUM_FLAG_SYNCMFT |
        MFT_ENUM_FLAG_ASYNCMFT;

    if (preferHardware) {
        flags |= MFT_ENUM_FLAG_HARDWARE;
    }

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        flags,
        &inInfo,
        &outInfo,
        &activates,
        &count
    );

    if (FAILED(hr) || count == 0 || !activates) {
        if (activates) {
            CoTaskMemFree(activates);
            activates = nullptr;
        }

        count = 0;
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SORTANDFILTER |
            MFT_ENUM_FLAG_LOCALMFT |
            MFT_ENUM_FLAG_SYNCMFT |
            MFT_ENUM_FLAG_ASYNCMFT,
            &inInfo,
            &outInfo,
            &activates,
            &count
        );
    }

    if (FAILED(hr) || count == 0 || !activates) {
        if (error) *error = "MFTEnumEx AV1 encoder failed " + HrToString(hr);
        return false;
    }

    // Chromium's Windows MF encoder skips this specific Intel *hybrid*
    // AV1 transform, which can advertise encoding but produce unusable
    // output. Do not select the first MFT blindly.
    // Source: Chromium media_foundation_video_encode_accelerator_win.cc
    // kIntelAV1HybridEncoderCLSID.
    const GUID kUnsupportedIntelHybridAv1Clsid{
        0x62c053ce, 0x5357, 0x4794,
        {0x8c, 0x5a, 0xfb, 0xef, 0xfe, 0xff, 0xb8, 0x2d}
    };
    ComPtr<IMFActivate> chosen;
    UINT32 skippedHybrid = 0;

    // Prefer a genuine hardware AV1 MFT, then try eligible software MFTs
    // if the hardware enumeration contains only the rejected hybrid.
    for (int pass = 0; pass < 2 && !chosen; ++pass) {
        if (pass == 1) {
            if (activates) {
                for (UINT32 i = 0; i < count; ++i) {
                    if (activates[i]) activates[i]->Release();
                }
                CoTaskMemFree(activates);
                activates = nullptr;
            }
            count = 0;
            hr = MFTEnumEx(
                MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_SORTANDFILTER |
                MFT_ENUM_FLAG_LOCALMFT |
                MFT_ENUM_FLAG_SYNCMFT |
                MFT_ENUM_FLAG_ASYNCMFT,
                &inInfo, &outInfo, &activates, &count);
            if (FAILED(hr) || !activates) break;
        }
        for (UINT32 i = 0; i < count; ++i) {
            if (!activates[i]) continue;
            GUID clsid{};
            if (SUCCEEDED(activates[i]->GetGUID(
                    MFT_TRANSFORM_CLSID_Attribute, &clsid)) &&
                IsEqualGUID(clsid, kUnsupportedIntelHybridAv1Clsid)) {
                ++skippedHybrid;
                continue;
            }
            chosen = activates[i];
            break;
        }
    }
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    if (!chosen) {
        if (error) {
            *error = "No eligible AV1 Media Foundation encoder; skipped " +
                std::to_string(skippedHybrid) + " known Intel hybrid AV1 MFT(s)";
        }
        return false;
    }

    WCHAR* friendly = nullptr;
    UINT32 friendlyLen = 0;
    if (SUCCEEDED(chosen->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen)) && friendly) {
        m_encoderName = WideToUtf8(friendly);
        CoTaskMemFree(friendly);
    }

    if (m_encoderName.empty()) {
        m_encoderName = "Media Foundation AV1 Encoder";
    }

    hr = chosen->ActivateObject(IID_PPV_ARGS(&m_impl->transform));
    if (FAILED(hr) || !m_impl->transform) {
        if (error) *error = "ActivateObject AV1 MFT failed " + HrToString(hr);
        return false;
    }

    {
        ComPtr<IMFAttributes> attrs;
        UINT32 asyncFlag = FALSE;
        if (SUCCEEDED(m_impl->transform->GetAttributes(&attrs)) && attrs) {
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
            if (asyncFlag) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        m_impl->asynchronous = asyncFlag != FALSE;
        if (m_impl->asynchronous &&
            (FAILED(m_impl->transform.As(&m_impl->events.generator)) ||
             !m_impl->events.generator)) {
            if (error) *error = "asynchronous MFT has no event generator";
            return false;
        }
    }

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, kAv1Subtype);
    outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(m_bitrateKbps * 1000));
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    SetAttrSize(outType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(m_width), static_cast<UINT32>(m_height));
    SetAttrRatio(outType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
    SetAttrRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_impl->transform->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        if (error) *error = "SetOutputType AV1 failed " + HrToString(hr);
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
        if (error) *error = "SetInputType AV1 NV12 failed " + HrToString(hr);
        return false;
    }

    m_impl->transform->GetOutputStreamInfo(0, &m_impl->outputInfo);

    m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_impl->mfStarted = true;

    m_open = true;
    return true;
}

bool Av1MfEncoder::encode(const I420Frame& frame, bool forceKeyframe, Av1EncodedFrame& out, std::string* error) {
    (void)forceKeyframe;

    out = {};

    if (!m_open || !m_impl || !m_impl->transform) {
        if (error) *error = "encoder not open";
        return false;
    }

    if (frame.width != m_width || frame.height != m_height) {
        if (error) *error = "frame size changed";
        return false;
    }

    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        if (error) *error = "MFCreateSample failed " + HrToString(hr);
        return false;
    }

    const int uvWidth = (m_width + 1) / 2;
    const int uvHeight = (m_height + 1) / 2;
    const size_t required =
        static_cast<size_t>(m_width) * static_cast<size_t>(m_height) +
        static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight) * 2;

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(required), &buffer);
    if (FAILED(hr)) {
        if (error) *error = "MFCreateMemoryBuffer failed " + HrToString(hr);
        return false;
    }

    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;

    hr = buffer->Lock(&dst, &maxLen, &curLen);
    if (FAILED(hr)) {
        if (error) *error = "input buffer Lock failed " + HrToString(hr);
        return false;
    }

    DWORD written = 0;
    const bool copied = CopyI420ToNV12Buffer(frame, dst, maxLen, &written);

    buffer->Unlock();

    if (!copied) {
        if (error) {
            *error =
                "I420->NV12 copy failed frame=" +
                std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                " maxLen=" + std::to_string(maxLen);
        }
        return false;
    }

    buffer->SetCurrentLength(written);
    sample->AddBuffer(buffer.Get());

    const LONGLONG frameTime =
        static_cast<LONGLONG>((10'000'000.0 * static_cast<double>(m_frameIndex)) / static_cast<double>(m_fps));

    const LONGLONG frameDuration =
        static_cast<LONGLONG>(10'000'000.0 / static_cast<double>(m_fps));

    sample->SetSampleTime(frameTime);
    sample->SetSampleDuration(frameDuration);

    // Intel HEVC/AV1 hardware MFTs are asynchronous. The previous direct
    // ProcessInput returned 0xC00D36B5 (MF_E_NOTACCEPTING) even on the first
    // frame. Consume the encoder's NeedInput event before submitting a sample.
    if (m_impl->asynchronous &&
        !m_impl->events.waitFor(true, 750, error)) return false;

    hr = m_impl->transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        if (error) *error = "ProcessInput failed " + HrToString(hr);
        return false;
    }

    ++m_frameIndex;

    // Each output call on an asynchronous MFT must consume one HaveOutput
    // event. A codec may buffer its first few frames without producing
    // output, so a short no-output wait is not a fatal stream failure.
    // Bound stream-change retry count: a broken driver must not spin forever.
    for (int outputAttempt = 0; outputAttempt < 8; ++outputAttempt) {
        if (m_impl->asynchronous) {
            std::string eventError;
            if (!m_impl->events.waitFor(false, 200, &eventError)) {
                if (eventError ==
                    "async Media Foundation timed out waiting for METransformHaveOutput")
                    return true; // buffered frame; retry on the next capture
                if (error) *error = eventError;
                return false;
            }
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        DWORD status = 0;

        ComPtr<IMFSample> outSample;
        ComPtr<IMFMediaBuffer> outBuffer;

        if (!(m_impl->outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            MFCreateSample(&outSample);
            MFCreateMemoryBuffer(std::max<DWORD>(m_impl->outputInfo.cbSize, 1024 * 1024), &outBuffer);
            outSample->AddBuffer(outBuffer.Get());
            output.pSample = outSample.Get();
        }

        hr = m_impl->transform->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (output.pEvents) output.pEvents->Release();
            // An AV1 encoder may buffer initial frames; not an error.
            return true;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE ||
            output.dwStatus == MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
            if (output.pEvents) output.pEvents->Release();
            if (output.pSample && output.pSample != outSample.Get())
                output.pSample->Release();
            ComPtr<IMFMediaType> newOut;
            if (SUCCEEDED(m_impl->transform->GetOutputAvailableType(0, 0, &newOut)) && newOut) {
                m_impl->transform->SetOutputType(0, newOut.Get(), 0);
            }

            m_impl->transform->GetOutputStreamInfo(0, &m_impl->outputInfo);
            continue;
        }

        if (FAILED(hr)) {
            if (error) *error = "ProcessOutput failed " + HrToString(hr);
            return false;
        }

        ComPtr<IMFSample> got = output.pSample;
        if (!got && outSample) got = outSample;

        if (!got) {
            if (output.pEvents) output.pEvents->Release();
            continue;
        }

        ComPtr<IMFMediaBuffer> contiguous;
        hr = got->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr) || !contiguous) {
            if (output.pEvents) output.pEvents->Release();
            if (error) *error = "ConvertToContiguousBuffer failed " + HrToString(hr);
            return false;
        }

        BYTE* p = nullptr;
        DWORD max = 0;
        DWORD len = 0;

        hr = contiguous->Lock(&p, &max, &len);
        if (FAILED(hr)) {
            if (output.pEvents) output.pEvents->Release();
            if (error) *error = "output buffer Lock failed " + HrToString(hr);
            return false;
        }

        if (len > 0) {
            const size_t old = out.data.size();
            out.data.resize(old + len);
            std::memcpy(out.data.data() + old, p, len);
        }

        contiguous->Unlock();

        if (output.pEvents) {
            output.pEvents->Release();
        }

        if (!out.data.empty()) {
            out.timestamp90k =
                static_cast<uint32_t>((m_frameIndex * 90000ULL) / static_cast<uint64_t>(m_fps));

            out.keyframe = forceKeyframe || m_frameIndex <= 2;
            return true;
        }
    }
    if (error) *error = "encoder returned repeated empty or changing output samples";
    return false;
}
