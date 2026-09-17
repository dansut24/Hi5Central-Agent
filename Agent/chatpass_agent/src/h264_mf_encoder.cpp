#include "h264_mf_encoder.h"

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

#include <algorithm>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iostream>
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
        CreateDirectoryA("C:\\ProgramData\\Hi5Central\\Agent\\Logs", nullptr);
        std::ofstream f("C:\\ProgramData\\Hi5Central\\Agent\\Logs\\hi5-h264-dump.h264", std::ios::binary | std::ios::app);
        if (f) {
            f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            ++frames;
            if (frames == 1 || frames == 30 || frames == 180) {
                std::cout << "[h264] dump wrote frame_count=" << frames
                    << " path=C:\\ProgramData\\Hi5Central\\Agent\\Logs\\hi5-h264-dump.h264"
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
    ComPtr<IMFTransform> transform;
    DWORD inputStreamId = 0;
    DWORD outputStreamId = 0;
    MFT_OUTPUT_STREAM_INFO outputInfo{};
    ComPtr<IMFSample> outputSample;
    ComPtr<IMFMediaBuffer> outputBuffer;
    DWORD outputBufferSize = 0;
    std::vector<uint8_t> nv12;
    std::vector<uint8_t> sequenceHeaderAnnexB;
    bool sequenceHeaderLogged = false;
    bool mfStarted = false;
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
        }
        delete m_impl;
        m_impl = nullptr;
        // Balance the MFStartup() performed by init(); otherwise repeated
        // codec switches retain Media Foundation runtime resources indefinitely.
        MFShutdown();
    }
    m_open = false;
}

bool H264MfEncoder::init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error) {
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
    outInfo.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;
    if (preferHardware) flags |= MFT_ENUM_FLAG_HARDWARE;
    else flags |= MFT_ENUM_FLAG_SYNCMFT;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
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
    out = {};
    if (!m_open || !m_impl || !m_impl->transform) {
        if (error) *error = "encoder not open";
        return false;
    }

    if (frame.width != m_width || frame.height != m_height) {
        if (error) *error = "frame size changed";
        return false;
    }

    // Forced keyframe via CodecAPI is disabled in this build. We still mark
    // IDR/keyframe output by inspecting encoded NAL units and prepend SPS/PPS
    // when present.

    const int uvWidth = (frame.width + 1) / 2;
    const int uvHeight = (frame.height + 1) / 2;
    const size_t ySize = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    const size_t uvPlaneSize = static_cast<size_t>(uvWidth) * static_cast<size_t>(uvHeight);
    const size_t nv12Size = ySize + (uvPlaneSize * 2);

    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        if (error) *error = "MFCreateSample failed " + HrToString(hr);
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12Size), &buffer);
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
                " maxLen=" + std::to_string(maxLen) +
                " y=" + std::to_string(frame.y.size()) +
                " u=" + std::to_string(frame.u.size()) +
                " v=" + std::to_string(frame.v.size());
        }
        return false;
    }

    hr = buffer->SetCurrentLength(written);
    if (FAILED(hr)) {
        if (error) *error = "SetCurrentLength failed " + HrToString(hr);
        return false;
    }

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        if (error) *error = "sample AddBuffer failed " + HrToString(hr);
        return false;
    }

    const LONGLONG frameTime = static_cast<LONGLONG>((10'000'000.0 * static_cast<double>(m_frameIndex)) / static_cast<double>(m_fps));
    const LONGLONG frameDuration = static_cast<LONGLONG>(10'000'000.0 / static_cast<double>(m_fps));
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
            // ProcessInput succeeded; the MFT buffered this frame and simply has no output yet.
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> newOut;
            if (SUCCEEDED(m_impl->transform->GetOutputAvailableType(0, 0, &newOut)) && newOut) {
                m_impl->transform->SetOutputType(0, newOut.Get(), 0);
            }
            m_impl->transform->GetOutputStreamInfo(0, &m_impl->outputInfo);
            m_impl->sequenceHeaderAnnexB = ReadCurrentH264SequenceHeader(m_impl->transform.Get());
            std::cout << "[h264] stream change handled sequence_header_bytes=" << m_impl->sequenceHeaderAnnexB.size() << "\n";
            continue;
        }
        if (FAILED(hr)) {
            if (error) *error = "ProcessOutput failed " + HrToString(hr);
            return false;
        }

        ComPtr<IMFSample> got = output.pSample;
        if (!got) {
            continue;
        }

        ComPtr<IMFMediaBuffer> contiguous;
        hr = got->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr) || !contiguous) {
            if (error) *error = "ConvertToContiguousBuffer failed " + HrToString(hr);
            return false;
        }

        BYTE* p = nullptr;
        DWORD max = 0;
        DWORD len = 0;
        contiguous->Lock(&p, &max, &len);
        if (len > 0) {
            const size_t old = out.data.size();
            out.data.resize(old + len);
            std::memcpy(out.data.data() + old, p, len);
        }
        contiguous->Unlock();

        if (output.pEvents) output.pEvents->Release();

        if (!out.data.empty()) {
            // Media Foundation encoders may output Annex-B or AVCC/length-prefixed
            // H.264. Normalize the encoded sample before prepending SPS/PPS, otherwise
            // mixed Annex-B + AVCC output creates malformed NAL units and Chrome/WebView
            // commonly displays a black frame while data channels still work.
            out.data = ConvertAvccLengthPrefixedToAnnexB(out.data);
            out.keyframe = ContainsH264Idr(out.data) || forceKeyframe;

            // Ensure browsers receive SPS/PPS. Some Media Foundation encoders
            // place SPS/PPS only in MF_MT_MPEG_SEQUENCE_HEADER, not in the first
            // sample. Without this the WebRTC connection can establish, data
            // channels work, but the viewer stays black.
            if (out.keyframe && !m_impl->sequenceHeaderAnnexB.empty() &&
                (!ContainsH264NalType(out.data, 7) || !ContainsH264NalType(out.data, 8))) {
                std::vector<uint8_t> withHeader;
                withHeader.reserve(m_impl->sequenceHeaderAnnexB.size() + out.data.size());
                withHeader.insert(withHeader.end(), m_impl->sequenceHeaderAnnexB.begin(), m_impl->sequenceHeaderAnnexB.end());
                withHeader.insert(withHeader.end(), out.data.begin(), out.data.end());
                out.data.swap(withHeader);
            }

            if (!m_impl->sequenceHeaderLogged) {
                m_impl->sequenceHeaderLogged = true;
                std::cout << "[h264] sequence header bytes=" << m_impl->sequenceHeaderAnnexB.size() << " keyframe=" << (out.keyframe ? 1 : 0) << " has_sps=" << (ContainsH264NalType(out.data, 7) ? 1 : 0) << " has_pps=" << (ContainsH264NalType(out.data, 8) ? 1 : 0) << " has_idr=" << (ContainsH264NalType(out.data, 5) ? 1 : 0) << " bytes=" << out.data.size() << "\n";
            }

            AppendH264Dump(out.data);

            out.timestamp90k = static_cast<uint32_t>((m_frameIndex * 90000ULL) / static_cast<uint64_t>(m_fps));
            return true;
        }
    }
}
