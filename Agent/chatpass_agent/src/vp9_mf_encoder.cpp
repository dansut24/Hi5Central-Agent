#include "vp9_mf_encoder.h"

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <wrl/client.h>

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

struct Vp9MfEncoder::Impl {
    ComPtr<IMFTransform> transform;
    MFT_OUTPUT_STREAM_INFO outputInfo{};
    bool mfStarted = false;
};

// The software VP9 experiment uses the same libvpx library already proven by
// the production VP8 sender.  MF VP9VideoExtensionEncoder can return data
// without producing a browser-decodable WebRTC stream on some Windows hosts.
struct Vp9MfEncoder::VpxImpl {
    vpx_codec_ctx_t codec{};
    vpx_codec_enc_cfg_t config{};
    bool initialized = false;
};

Vp9MfEncoder::Vp9MfEncoder() = default;

Vp9MfEncoder::~Vp9MfEncoder() {
    shutdown();
}

void Vp9MfEncoder::shutdown() {
    if (m_vpx) {
        if (m_vpx->initialized) vpx_codec_destroy(&m_vpx->codec);
        delete m_vpx;
        m_vpx = nullptr;
    }
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

bool Vp9MfEncoder::init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error) {
    shutdown();

    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        if (error) *error = "invalid VP9 I420 dimensions";
        return false;
    }

    // Software-only VP9 is deliberately libvpx, not another Media
    // Foundation MFT that can silently return an unusable bitstream.
    if (!preferHardware) {
        auto software = std::make_unique<VpxImpl>();
        const auto* iface = vpx_codec_vp9_cx();
        if (!iface || vpx_codec_enc_config_default(iface, &software->config, 0) != VPX_CODEC_OK) {
            if (error) *error = "libvpx VP9 encoder not available";
            return false;
        }
        auto& cfg = software->config;
        cfg.g_w = static_cast<unsigned int>(width);
        cfg.g_h = static_cast<unsigned int>(height);
        cfg.g_timebase.num = 1;
        cfg.g_timebase.den = 90000;
        cfg.g_threads = 4;
        cfg.g_lag_in_frames = 0;
        cfg.g_pass = VPX_RC_ONE_PASS;
        cfg.rc_end_usage = VPX_CBR;
        cfg.rc_target_bitrate = static_cast<unsigned int>(std::max(300, bitrateKbps));
        cfg.rc_resize_allowed = 0;
        cfg.rc_dropframe_thresh = 0;
        cfg.rc_min_quantizer = 8;
        cfg.rc_max_quantizer = 48;
        cfg.rc_buf_initial_sz = 200;
        cfg.rc_buf_optimal_sz = 350;
        cfg.rc_buf_sz = 600;
        cfg.kf_mode = VPX_KF_AUTO;
        cfg.kf_min_dist = 0;
        cfg.kf_max_dist = std::max(30, fps * 5);
#ifdef VPX_ERROR_RESILIENT_DEFAULT
        cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
#endif
        if (vpx_codec_enc_init(&software->codec, iface, &cfg, 0) != VPX_CODEC_OK) {
            if (error) *error = "libvpx VP9 encoder initialization failed";
            return false;
        }
        software->initialized = true;
        vpx_codec_control(&software->codec, VP8E_SET_CPUUSED, 8);
#ifdef VP9E_SET_TILE_COLUMNS
        vpx_codec_control(&software->codec, VP9E_SET_TILE_COLUMNS, 1);
#endif
#ifdef VP9E_SET_ROW_MT
        vpx_codec_control(&software->codec, VP9E_SET_ROW_MT, 1);
#endif
        m_width = width;
        m_height = height;
        m_fps = std::max(1, fps);
        m_bitrateKbps = std::max(300, bitrateKbps);
        m_frameIndex = 0;
        m_encoderName = "libvpx VP9 realtime software";
        m_vpx = software.release();
        m_open = true;
        return true;
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
    outInfo.guidSubtype = MFVideoFormat_VP90;

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
        if (error) *error = "MFTEnumEx VP9 encoder failed " + HrToString(hr);
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

    if (m_encoderName.empty()) {
        m_encoderName = "Media Foundation VP9 Encoder";
    }

    hr = chosen->ActivateObject(IID_PPV_ARGS(&m_impl->transform));
    if (FAILED(hr) || !m_impl->transform) {
        if (error) *error = "ActivateObject VP9 MFT failed " + HrToString(hr);
        return false;
    }

    {
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m_impl->transform->GetAttributes(&attrs)) && attrs) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
    }

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_VP90);
    outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(m_bitrateKbps * 1000));
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    SetAttrSize(outType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(m_width), static_cast<UINT32>(m_height));
    SetAttrRatio(outType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
    SetAttrRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_impl->transform->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        if (error) *error = "SetOutputType VP9 failed " + HrToString(hr);
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
        if (error) *error = "SetInputType VP9 NV12 failed " + HrToString(hr);
        return false;
    }

    m_impl->transform->GetOutputStreamInfo(0, &m_impl->outputInfo);

    m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_impl->mfStarted = true;

    m_open = true;
    return true;
}

bool Vp9MfEncoder::setTargetBitrateKbps(int bitrateKbps, std::string* error) {
    if (m_vpx && m_vpx->initialized) {
        const int requested = std::max(300, std::min(20000, bitrateKbps));
        if (requested == m_bitrateKbps) return true;
        auto next = m_vpx->config;
        next.rc_target_bitrate = static_cast<unsigned int>(requested);
        if (vpx_codec_enc_config_set(&m_vpx->codec, &next) != VPX_CODEC_OK) {
            if (error) *error = "libvpx VP9 bitrate reconfigure failed";
            return false;
        }
        m_vpx->config = next;
        m_bitrateKbps = requested;
        return true;
    }
    if (!m_open || !m_impl || !m_impl->transform) {
        if (error) *error = "encoder not open";
        return false;
    }
    const int requested = std::max(300, std::min(20000, bitrateKbps));
    if (requested == m_bitrateKbps) return true;

    // ICodecAPI is optional on Media Foundation VP9 MFTs. If unsupported,
    // retain the existing encoder/rate instead of forcing a costly teardown.
    ComPtr<ICodecAPI> api;
    HRESULT hr = m_impl->transform.As(&api);
    if (FAILED(hr) || !api) {
        if (error) *error = "dynamic bitrate ICodecAPI unavailable";
        return false;
    }
    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = static_cast<ULONG>(requested * 1000);
    hr = api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
    if (FAILED(hr)) {
        if (error) *error = "dynamic bitrate SetValue failed " + HrToString(hr);
        return false;
    }
    m_bitrateKbps = requested;
    return true;
}

bool Vp9MfEncoder::encode(const I420Frame& frame, bool forceKeyframe, Vp9EncodedFrame& out, std::string* error) {
    out = {};
    if (m_vpx && m_vpx->initialized) {
        if (!m_open || frame.width != m_width || frame.height != m_height ||
            frame.y.size() < static_cast<size_t>(m_width) * m_height ||
            frame.u.size() < static_cast<size_t>(m_width / 2) * (m_height / 2) ||
            frame.v.size() < static_cast<size_t>(m_width / 2) * (m_height / 2)) {
            if (error) *error = "invalid software VP9 I420 frame";
            return false;
        }
        vpx_image_t image{};
        image.fmt = VPX_IMG_FMT_I420;
        image.w = image.d_w = static_cast<unsigned int>(m_width);
        image.h = image.d_h = static_cast<unsigned int>(m_height);
        image.x_chroma_shift = image.y_chroma_shift = 1;
        image.bps = 12;
        image.planes[VPX_PLANE_Y] = const_cast<unsigned char*>(frame.y.data());
        image.planes[VPX_PLANE_U] = const_cast<unsigned char*>(frame.u.data());
        image.planes[VPX_PLANE_V] = const_cast<unsigned char*>(frame.v.data());
        image.stride[VPX_PLANE_Y] = m_width;
        image.stride[VPX_PLANE_U] = image.stride[VPX_PLANE_V] = m_width / 2;
        const auto pts = static_cast<vpx_codec_pts_t>(m_frameIndex * 90000ULL / m_fps);
        const auto duration = static_cast<unsigned long>(std::max(1, 90000 / m_fps));
        const auto flags = forceKeyframe ? VPX_EFLAG_FORCE_KF : 0;
        const auto status = vpx_codec_encode(
            &m_vpx->codec, &image, pts, duration, flags, VPX_DL_REALTIME);
        if (status != VPX_CODEC_OK) {
            if (error) *error = std::string("libvpx VP9 encode failed: ") +
                vpx_codec_err_to_string(status);
            return false;
        }
        ++m_frameIndex;
        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t* packet = nullptr;
        while ((packet = vpx_codec_get_cx_data(&m_vpx->codec, &iter)) != nullptr) {
            if (packet->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            const auto* ptr = static_cast<const uint8_t*>(packet->data.frame.buf);
            if (!ptr || packet->data.frame.sz == 0) continue;
            if (out.data.empty()) {
                out.keyframe = (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
                out.timestamp90k = static_cast<uint32_t>(packet->data.frame.pts);
            }
            out.data.insert(out.data.end(), ptr, ptr + packet->data.frame.sz);
        }
        // An encoder may buffer an output frame; the caller already accepts
        // an empty sample and will continue without sending empty RTP.
        return true;
    }

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

    hr = m_impl->transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        if (error) *error = "ProcessInput failed " + HrToString(hr);
        return false;
    }

    ++m_frameIndex;

    for (;;) {
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
            // A media transform is allowed to buffer initial frames. Empty
            // output is NOT an encoder failure; the next input may emit it.
            return true;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
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
}