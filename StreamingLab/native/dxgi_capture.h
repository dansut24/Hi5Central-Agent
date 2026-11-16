#pragma once
// Isolated interactive-desktop DXGI duplication probe. No agent/service coupling.
// If the interactive session cannot duplicate the desktop, the caller must
// retain the GDI path as a conservative fallback. Never steal/alter desktops.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace lab {
using Microsoft::WRL::ComPtr;
enum class FrameStatus { Fresh, Unchanged, Failed };

inline std::string hrString(HRESULT hr) {
    char buf[16]{};
    sprintf_s(buf,sizeof(buf),"0x%08X",static_cast<unsigned>(hr));
    return std::string(buf);
}

class DxgiCapture {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
    std::vector<uint8_t> pixels_;
    int width_=0,height_=0;
    std::string adapterName_;
public:
    DxgiCapture() {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if(FAILED(hr)) throw std::runtime_error("CreateDXGIFactory1 "+hrString(hr));

        ComPtr<IDXGIAdapter1> chosenAdapter;
        ComPtr<IDXGIOutput> chosenOutput;
        for(UINT ai=0;!chosenOutput; ++ai) {
            ComPtr<IDXGIAdapter1> adapter;
            hr=factory->EnumAdapters1(ai,&adapter);
            if(hr==DXGI_ERROR_NOT_FOUND) break;
            if(FAILED(hr)) continue;
            DXGI_ADAPTER_DESC1 adapterDesc{};
            if(SUCCEEDED(adapter->GetDesc1(&adapterDesc)) &&
               (adapterDesc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) continue;

            for(UINT oi=0; ; ++oi) {
                ComPtr<IDXGIOutput> output;
                hr=adapter->EnumOutputs(oi,&output);
                if(hr==DXGI_ERROR_NOT_FOUND) break;
                if(FAILED(hr)) continue;
                DXGI_OUTPUT_DESC outputDesc{};
                if(FAILED(output->GetDesc(&outputDesc)) || !outputDesc.AttachedToDesktop)
                    continue;
                if(outputDesc.DesktopCoordinates.left==0 &&
                   outputDesc.DesktopCoordinates.top==0) {
                    chosenAdapter=adapter;
                    chosenOutput=output;
                    break;
                }
            }
        }
        if(!chosenAdapter || !chosenOutput)
            throw std::runtime_error("No attached primary DXGI output");

        UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level{};
        hr=D3D11CreateDevice(chosenAdapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
                             flags,nullptr,0,D3D11_SDK_VERSION,
                             &device_,&level,&context_);
        if(FAILED(hr)) throw std::runtime_error("D3D11CreateDevice "+hrString(hr));

        ComPtr<IDXGIOutput1> output1;
        hr=chosenOutput.As(&output1);
        if(FAILED(hr))throw std::runtime_error("Query IDXGIOutput1 "+hrString(hr));
        hr=output1->DuplicateOutput(device_.Get(),&duplication_);
        if(FAILED(hr))throw std::runtime_error("DuplicateOutput "+hrString(hr));

        DXGI_OUTDUPL_DESC dupDesc{};
        duplication_->GetDesc(&dupDesc);
        width_=static_cast<int>(dupDesc.ModeDesc.Width);
        height_=static_cast<int>(dupDesc.ModeDesc.Height);
        if(width_<2 || height_<2 ||
           dupDesc.ModeDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Unsupported DXGI desktop format or dimensions");

        D3D11_TEXTURE2D_DESC texture{};
        texture.Width=static_cast<UINT>(width_);
        texture.Height=static_cast<UINT>(height_);
        texture.MipLevels=1;
        texture.ArraySize=1;
        texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.SampleDesc.Count=1;
        texture.Usage=D3D11_USAGE_STAGING;
        texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr=device_->CreateTexture2D(&texture,nullptr,&staging_);
        if(FAILED(hr))throw std::runtime_error("Create staging texture "+hrString(hr));
        pixels_.resize(static_cast<size_t>(width_)*height_*4);
    }

    int width() const{return width_;}
    int height() const{return height_;}
    const uint8_t* data() const{return pixels_.data();}
    int stride() const{return width_*4;}

    // The desktop is allowed to be idle. A timeout is not a dropped frame.
    FrameStatus capture() {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr=duplication_->AcquireNextFrame(0,&info,&resource);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT) return FrameStatus::Unchanged;
        if(FAILED(hr))return FrameStatus::Failed;

        FrameStatus status=FrameStatus::Failed;
        do {
            ComPtr<ID3D11Texture2D> frame;
            if(FAILED(resource.As(&frame)))break;
            D3D11_TEXTURE2D_DESC fd{};
            frame->GetDesc(&fd);
            if(static_cast<int>(fd.Width)!=width_ ||
               static_cast<int>(fd.Height)!=height_ ||
               fd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)break;
            context_->CopyResource(staging_.Get(),frame.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(context_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&mapped)))break;
            for(int y=0;y<height_;++y) {
                memcpy(pixels_.data()+static_cast<size_t>(y)*width_*4,
                       static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,
                       static_cast<size_t>(width_)*4);
            }
            context_->Unmap(staging_.Get(),0);
            status=FrameStatus::Fresh;
        }while(false);
        duplication_->ReleaseFrame();
        return status;
    }
};
}  // namespace lab
