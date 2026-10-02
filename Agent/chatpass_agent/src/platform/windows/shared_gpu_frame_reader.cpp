#include "platform/windows/shared_gpu_frame_reader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <libyuv/convert_from_argb.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace hi5 {
namespace {

bool BgraToI420(const uint8_t* src, int stride, int width, int height, I420Frame& out) {
    out.width = width;
    out.height = height;
    out.y.resize(static_cast<size_t>(width) * height);
    const int uvWidth = (width + 1) / 2;
    const int uvHeight = (height + 1) / 2;
    out.u.resize(static_cast<size_t>(uvWidth) * uvHeight);
    out.v.resize(static_cast<size_t>(uvWidth) * uvHeight);

    // DXGI Desktop Duplication publishes DXGI_FORMAT_B8G8R8A8_UNORM.
    // libyuv's ARGB API consumes that byte order on little-endian Windows and
    // dispatches to SSSE3/AVX2 instead of walking every pixel in scalar C++.
    return libyuv::ARGBToI420(
        src, stride,
        out.y.data(), width,
        out.u.data(), uvWidth,
        out.v.data(), uvWidth,
        width, height) == 0;
}

std::string HrString(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

} // namespace

struct SharedGpuFrameReader::Impl {
    struct OpenedFrame {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
        HANDLE duplicatedHandle = nullptr;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    std::unordered_map<uint64_t, OpenedFrame> opened;
    LUID adapterLuid{};
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    void Reset() {
        staging.Reset();
        for (auto& kv : opened) {
            if (kv.second.duplicatedHandle) CloseHandle(kv.second.duplicatedHandle);
        }
        opened.clear();
        context.Reset();
        device1.Reset();
        device.Reset();
        adapterLuid = {};
        width = 0;
        height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    bool EnsureDevice(const SharedGpuFrame& frame, std::string* error) {
        if (device && adapterLuid.LowPart == frame.adapterLuidLow &&
            adapterLuid.HighPart == frame.adapterLuidHigh) {
            return true;
        }

        Reset();
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            if (error) *error = "CreateDXGIFactory1 failed " + HrString(hr);
            return false;
        }

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) continue;
            if (desc.AdapterLuid.LowPart == frame.adapterLuidLow &&
                desc.AdapterLuid.HighPart == frame.adapterLuidHigh) {
                adapter = candidate;
                adapterLuid = desc.AdapterLuid;
                break;
            }
        }
        if (!adapter) {
            if (error) *error = "matching D3D11 adapter not found";
            return false;
        }

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION,
            &device, nullptr, &context);
        if (FAILED(hr) || !device || !context) {
            if (error) *error = "D3D11CreateDevice failed " + HrString(hr);
            Reset();
            return false;
        }
        hr = device.As(&device1);
        if (FAILED(hr) || !device1) {
            if (error) *error = "ID3D11Device1 unavailable " + HrString(hr);
            Reset();
            return false;
        }
        return true;
    }

    bool EnsureStaging(const SharedGpuFrame& frame, std::string* error) {
        const auto wantedFormat = static_cast<DXGI_FORMAT>(frame.dxgiFormat);
        if (staging && width == frame.width && height == frame.height && format == wantedFormat) {
            return true;
        }

        staging.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(frame.width);
        desc.Height = static_cast<UINT>(frame.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = wantedFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr) || !staging) {
            if (error) *error = "CreateTexture2D(staging) failed " + HrString(hr);
            return false;
        }
        width = frame.width;
        height = frame.height;
        format = wantedFormat;
        return true;
    }

    bool OpenFrame(const SharedGpuFrame& frame, OpenedFrame*& openedFrame, std::string* error) {
        auto it = opened.find(frame.sharedHandle);
        if (it == opened.end()) {
            OpenedFrame entry;
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frame.sharedHandle));
            HRESULT hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&entry.texture));
            if (FAILED(hr) || !entry.texture) {
                CloseHandle(handle);
                if (error) *error = "OpenSharedResource1 failed " + HrString(hr);
                return false;
            }
            entry.duplicatedHandle = handle;
            hr = entry.texture.As(&entry.mutex);
            if (FAILED(hr) || !entry.mutex) {
                CloseHandle(entry.duplicatedHandle);
                entry.duplicatedHandle = nullptr;
                if (error) *error = "shared texture missing IDXGIKeyedMutex " + HrString(hr);
                return false;
            }
            it = opened.emplace(frame.sharedHandle, std::move(entry)).first;
        }
        openedFrame = &it->second;
        return true;
    }
};

SharedGpuFrameReader::SharedGpuFrameReader() : impl_(std::make_unique<Impl>()) {}
SharedGpuFrameReader::~SharedGpuFrameReader() = default;

bool SharedGpuFrameReader::ReadI420(const SharedGpuFrame& frame, I420Frame& out, std::string* error) {
    if (!impl_ || frame.width <= 0 || frame.height <= 0 || frame.sharedHandle == 0 || frame.syncKey == 0) {
        if (error) *error = "invalid shared GPU frame descriptor";
        return false;
    }
    if (static_cast<DXGI_FORMAT>(frame.dxgiFormat) != DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (error) *error = "unsupported shared GPU pixel format " + std::to_string(frame.dxgiFormat);
        return false;
    }
    if (!impl_->EnsureDevice(frame, error) || !impl_->EnsureStaging(frame, error)) return false;

    Impl::OpenedFrame* openedFrame = nullptr;
    if (!impl_->OpenFrame(frame, openedFrame, error) || !openedFrame) return false;

    const HRESULT acquireHr = openedFrame->mutex->AcquireSync(frame.syncKey, 4);
    if (acquireHr != S_OK) {
        if (error) *error = "AcquireSync failed/expired " + HrString(acquireHr);
        return false;
    }

    bool ok = false;
    impl_->context->CopyResource(impl_->staging.Get(), openedFrame->texture.Get());

    // The shared texture is no longer needed once its pixels have been queued
    // into our private staging texture. Release it before the blocking Map and
    // CPU colour conversion so the capture worker can reuse this slot while
    // MediaHost finishes the current frame.
    const HRESULT releaseHr = openedFrame->mutex->ReleaseSync(0);
    if (releaseHr != S_OK) {
        if (error) *error = "ReleaseSync failed " + HrString(releaseHr);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = impl_->context->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(mapHr) && mapped.pData) {
        ok = BgraToI420(static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
            frame.width, frame.height, out);
        impl_->context->Unmap(impl_->staging.Get(), 0);
        if (!ok && error) *error = "libyuv ARGBToI420 failed";
    } else if (error) {
        *error = "Map shared GPU staging failed " + HrString(mapHr);
    }
    return ok;
}

void SharedGpuFrameReader::Reset() {
    if (impl_) impl_->Reset();
}

} // namespace hi5
