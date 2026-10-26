#include "windows_wgc_capture.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace D3D = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
std::string FormatError(const char* context, HRESULT hr) {
    std::ostringstream out;
    out << context << " hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

struct MonitorLookup {
    POINT center{};
    HMONITOR found = nullptr;
};
BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT bounds, LPARAM param) {
    auto& lookup = *reinterpret_cast<MonitorLookup*>(param);
    if (bounds && lookup.center.x >= bounds->left &&
        lookup.center.x < bounds->right &&
        lookup.center.y >= bounds->top &&
        lookup.center.y < bounds->bottom) {
        lookup.found = monitor;
        return FALSE;
    }
    return TRUE;
}
}

struct WgcMonitorCapture::Impl {
    bool apartmentInitialized = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    int stagingWidth = 0;
    int stagingHeight = 0;
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession session{nullptr};

    void Stop() {
        // Close capture objects before releasing D3D and the WinRT apartment.
        if (session) { session.Close(); session = nullptr; }
        if (pool) { pool.Close(); pool = nullptr; }
        item = nullptr;
        staging.Reset();
        context.Reset();
        device.Reset();
        stagingWidth = stagingHeight = 0;
        if (apartmentInitialized) {
            RoUninitialize();
            apartmentInitialized = false;
        }
    }

    bool Start(int x, int y, int w, int h, std::string& failure) {
        Stop();
        failure.clear();
        if (w <= 0 || h <= 0) {
            failure = "invalid selected monitor geometry";
            return false;
        }
        try {
            const HRESULT initHr = RoInitialize(RO_INIT_MULTITHREADED);
            if (SUCCEEDED(initHr)) apartmentInitialized = true;
            else if (initHr != RPC_E_CHANGED_MODE) {
                throw std::runtime_error(FormatError("RoInitialize", initHr));
            }

            if (!WGC::GraphicsCaptureSession::IsSupported()) {
                throw std::runtime_error("GraphicsCaptureSession::IsSupported=false");
            }

            MonitorLookup lookup{};
            lookup.center = { x + w / 2, y + h / 2 };
            EnumDisplayMonitors(nullptr, nullptr, FindMonitor,
                                reinterpret_cast<LPARAM>(&lookup));
            if (!lookup.found) {
                throw std::runtime_error("no HMONITOR for selected output");
            }

            auto activation = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
            auto interop = activation.as<IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForMonitor(
                lookup.found,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(item)));

            const HRESULT deviceHr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION,
                &device, nullptr, &context);
            if (FAILED(deviceHr)) {
                throw std::runtime_error(FormatError("D3D11CreateDevice(WGC)", deviceHr));
            }

            ComPtr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(device.As(&dxgiDevice));
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.Get(), inspectable.put()));
            auto winrtDevice = inspectable.as<D3D::IDirect3DDevice>();
            const auto size = item.Size();
            if (size.Width <= 0 || size.Height <= 0) {
                throw std::runtime_error("WGC monitor has invalid size");
            }
            pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2, size);
            session = pool.CreateCaptureSession(item);
            // Do not request borderless capture; the operating system's
            // privacy/consent indicator must remain visible.
            session.StartCapture();
            return true;
        } catch (const winrt::hresult_error& ex) {
            failure = FormatError("WGC WinRT initialization", ex.code());
        } catch (const std::exception& ex) {
            failure = ex.what();
        }
        Stop();
        return false;
    }

    bool TryReadBgra(std::vector<uint8_t>& bgra, int& width, int& height,
                     int& stride, std::string& failure) {
        failure.clear();
        if (!session || !pool || !device) return false;
        try {
            auto frame = pool.TryGetNextFrame();
            if (!frame) return false;
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) {
                throw std::runtime_error("WGC frame has invalid dimensions");
            }

            auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(texture.GetAddressOf())));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            width = std::min(static_cast<int>(desc.Width), size.Width);
            height = std::min(static_cast<int>(desc.Height), size.Height);
            if (width <= 0 || height <= 0 || width > 7680 || height > 4320 ||
                desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                throw std::runtime_error("WGC unexpected texture format or size");
            }
            if (!staging || stagingWidth != static_cast<int>(desc.Width) ||
                stagingHeight != static_cast<int>(desc.Height)) {
                staging.Reset();
                D3D11_TEXTURE2D_DESC cpuDesc = desc;
                cpuDesc.Usage = D3D11_USAGE_STAGING;
                cpuDesc.BindFlags = 0;
                cpuDesc.MiscFlags = 0;
                cpuDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                winrt::check_hresult(device->CreateTexture2D(
                    &cpuDesc, nullptr, &staging));
                stagingWidth = static_cast<int>(desc.Width);
                stagingHeight = static_cast<int>(desc.Height);
            }
            context->CopyResource(staging.Get(), texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(
                staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            stride = static_cast<int>(mapped.RowPitch);
            bgra.resize(static_cast<size_t>(stride) * height);
            std::memcpy(bgra.data(), mapped.pData, bgra.size());
            context->Unmap(staging.Get(), 0);
            return true;
        } catch (const winrt::hresult_error& ex) {
            failure = FormatError("WGC frame read", ex.code());
        } catch (const std::exception& ex) {
            failure = ex.what();
        }
        return false;
    }
};

WgcMonitorCapture::WgcMonitorCapture() : impl_(std::make_unique<Impl>()) {}
WgcMonitorCapture::~WgcMonitorCapture() { Stop(); }
bool WgcMonitorCapture::Start(int x, int y, int w, int h, std::string& failure) {
    return impl_->Start(x, y, w, h, failure);
}
bool WgcMonitorCapture::TryReadBgra(
    std::vector<uint8_t>& bgra, int& width, int& height, int& stride,
    std::string& failure) {
    return impl_->TryReadBgra(bgra, width, height, stride, failure);
}
void WgcMonitorCapture::Stop() { if (impl_) impl_->Stop(); }
bool WgcMonitorCapture::IsRunning() const {
    return impl_ && static_cast<bool>(impl_->session);
}
