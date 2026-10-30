// Standalone, read-only frame cadence probe for Hi5Central issue #83.
// No Agent dependency, no screen pixels, no encoding, no network or stream.
// --launch runs two short child probes in the active console session:
// first with the signed-in user's token, then with session-bound SYSTEM.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <roapi.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace D3D = winrt::Windows::Graphics::DirectX::Direct3D11;
constexpr wchar_t const* kOutUser = L"C:\\Users\\Public\\Documents\\H5C-WGC-independent-user-20261030.txt";
constexpr wchar_t const* kOutSystem = L"C:\\Users\\Public\\Documents\\H5C-WGC-independent-system-20261030.txt";

std::string Hex(HRESULT hr) {
    std::ostringstream s; s << "0x" << std::hex << std::uppercase
                            << static_cast<uint32_t>(hr); return s.str();
}
std::string DesktopName(HDESK desk) {
    wchar_t name[128]{}; DWORD needed=0;
    if (!desk || !GetUserObjectInformationW(desk, UOI_NAME, name,
            sizeof(name), &needed)) return "unavailable";
    std::string result;
    for (wchar_t* p=name; *p; ++p) result.push_back(*p<128?char(*p):'?');
    return result;
}
bool Enable(const wchar_t* name) {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES,&token)) return false;
    LUID luid{};
    bool ok=false;
    if(LookupPrivilegeValueW(nullptr,name,&luid)) {
        TOKEN_PRIVILEGES tp{};tp.PrivilegeCount=1;tp.Privileges[0].Luid=luid;
        tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok=AdjustTokenPrivileges(token,FALSE,&tp,0,nullptr,nullptr) &&
            GetLastError()==ERROR_SUCCESS;
    }
    CloseHandle(token);return ok;
}
struct Monitor {
    HMONITOR handle=nullptr;
    RECT rect{};
};
BOOL CALLBACK VisitMon(HMONITOR h,HDC,LPRECT rect,LPARAM param) {
    auto* monitor=reinterpret_cast<Monitor*>(param);
    MONITORINFO info{}; info.cbSize=sizeof(info);
    if (GetMonitorInfoW(h,&info) && (info.dwFlags & MONITORINFOF_PRIMARY)) {
        monitor->handle=h;monitor->rect=*rect;
    }
    return TRUE;
}
class AnimatedWindow {
public:
    std::atomic<uint64_t> paints{0},ticks{0};
    std::atomic<bool> ready{false};
    std::atomic<HWND> hwnd{nullptr};
    std::thread worker;
    static LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* self=reinterpret_cast<AnimatedWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {
            self=reinterpret_cast<AnimatedWindow*>(
                reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if(msg==WM_PAINT) {
            PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);
            if(self && dc) {
                // Change a visibly large region each 16ms; this is a REAL local
                // window redraw, not synthetic WGC frame duplication.
                const auto index=self->paints.fetch_add(1,std::memory_order_relaxed);
                RECT rect{};GetClientRect(h,&rect);
                HBRUSH bg=CreateSolidBrush((index&1)?RGB(14,62,172):RGB(172,52,14));
                FillRect(dc,&rect,bg);DeleteObject(bg);
                RECT bar{LONG((index*31)%390),48,LONG((index*31)%390)+90,172};
                HBRUSH fg=CreateSolidBrush(RGB(255,230,96));
                FillRect(dc,&bar,fg);DeleteObject(fg);
                GdiFlush();
            }
            EndPaint(h,&ps);
            return 0;
        }
        if(msg==WM_CLOSE) {DestroyWindow(h);return 0;}
        if(msg==WM_DESTROY) {PostQuitMessage(0);return 0;}
        return DefWindowProcW(h,msg,w,l);
    }
    bool Start() {
        if(worker.joinable())return false;
        worker=std::thread([this]{
            const wchar_t* cls=L"H5CWgcFrameCadenceTest20261030";
            WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;
            wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=cls;
            RegisterClassW(&wc);
            // Small, non-topmost, non-activating visible diagnostic window.
            HWND h=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
                cls,L"Hi5Central WGC 60Hz local animation diagnostic",
                WS_POPUP|WS_VISIBLE,85,90,480,210,nullptr,nullptr,
                GetModuleHandleW(nullptr),this);
            hwnd.store(h,std::memory_order_release);
            ready.store(true,std::memory_order_release);
            if(!h)return;
            ShowWindow(h,SW_SHOWNOACTIVATE);
            // WM_TIMER on this Lenovo delivered only ~34 paints/s despite
            // a 16ms request. Use a high-resolution waitable timer instead,
            // identical to the already-tested host capture pacer approach.
            HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            if(!timer) timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
            MSG msg{};
            bool running=true;
            while(running) {
                while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                    if(msg.message==WM_QUIT) {running=false;break;}
                    TranslateMessage(&msg);DispatchMessageW(&msg);
                }
                if(!running)break;
                LARGE_INTEGER due{};due.QuadPart=-166667; // 16.6667 ms
                if(timer && SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)) {
                    WaitForSingleObject(timer,1000);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                if(!IsWindow(h))break;
                ticks.fetch_add(1,std::memory_order_relaxed);
                InvalidateRect(h,nullptr,FALSE);
                UpdateWindow(h);
            }
            if(timer)CloseHandle(timer);
            if(IsWindow(h))DestroyWindow(h);
            hwnd.store(nullptr,std::memory_order_release);
        });
        for(int i=0;i<50&&!ready.load(std::memory_order_acquire);++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return hwnd.load(std::memory_order_acquire)!=nullptr;
    }
    void Stop() {
        const HWND h=hwnd.load(std::memory_order_acquire);
        if(h)PostMessageW(h,WM_CLOSE,0,0);
        if(worker.joinable())worker.join();
    }
    ~AnimatedWindow(){Stop();}
};

std::string DxgiCheck(HMONITOR target) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if(FAILED(hr)) return "DXGI_CREATE_FACTORY="+Hex(hr);
    for(UINT a=0;a<20;++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        for(UINT o=0;o<20;++o) {
            ComPtr<IDXGIOutput> output;
            if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            if(FAILED(output->GetDesc(&d)) || d.Monitor!=target) continue;
            ComPtr<IDXGIOutput1> output1;
            hr=output.As(&output1);
            if(FAILED(hr)) return "DXGI_OUTPUT1="+Hex(hr);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
            hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &level,1,D3D11_SDK_VERSION,&device,nullptr,&context);
            if(FAILED(hr)) return "DXGI_D3D_DEVICE="+Hex(hr);
            ComPtr<IDXGIOutputDuplication> duplication;
            hr=output1->DuplicateOutput(device.Get(),&duplication);
            const HRESULT legacy=hr;
            duplication.Reset();
            ComPtr<IDXGIOutput5> output5;
            const HRESULT check5=output.As(&output5);
            HRESULT result1=check5;
            if(SUCCEEDED(check5)) {
                DXGI_FORMAT formats[1]={DXGI_FORMAT_B8G8R8A8_UNORM};
                result1=output5->DuplicateOutput1(device.Get(),0,1,formats,&duplication);
            }
            ComPtr<IDXGIAdapter1> infoAdapter=adapter;
            DXGI_ADAPTER_DESC1 desc{};
            infoAdapter->GetDesc1(&desc);
            return "DXGI_ADAPTER="+std::to_string(a)+" OUTPUT="+std::to_string(o)+
                " VENDOR_ID="+std::to_string(desc.VendorId)+
                " DEVICE_ID="+std::to_string(desc.DeviceId)+
                " DUPLICATEOUTPUT="+Hex(legacy)+
                " OUTPUT5_QI="+Hex(check5)+
                " DUPLICATEOUTPUT1="+Hex(result1)+
                " OUTPUT_ATTACHED_TO_DESKTOP="+std::to_string(d.AttachedToDesktop?1:0)+
                " OUTPUT_WIDTH="+std::to_string(d.DesktopCoordinates.right-d.DesktopCoordinates.left)+
                " OUTPUT_HEIGHT="+std::to_string(d.DesktopCoordinates.bottom-d.DesktopCoordinates.top);
        }
    }
    return "DXGI_NO_MATCHING_MONITOR";
}
struct Samples {
    std::atomic<uint64_t> arrived{0}, consumed{0}, exceptions{0};
    std::atomic<uint64_t> gapCount{0},gapTotalNs{0},gapMaxNs{0};
    std::atomic<uint64_t> sourceGapCount{0},sourceGapTotal100ns{0};
    std::atomic<int64_t> lastArrivalNs{0},lastSource100ns{0};
    void AddGap(std::atomic<uint64_t>& total,std::atomic<uint64_t>& count,
                uint64_t gap) noexcept {
        total.fetch_add(gap,std::memory_order_relaxed);
        count.fetch_add(1,std::memory_order_relaxed);
    }
};
int RunProbe(std::ostream& out) {
    DWORD sessionId=0;ProcessIdToSessionId(GetCurrentProcessId(),&sessionId);
    const auto active=WTSGetActiveConsoleSessionId();
    auto d=GetThreadDesktop(GetCurrentThreadId());
    HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    const std::string t=DesktopName(d),i=DesktopName(input);
    if(input)CloseDesktop(input);
    out << "PROBE pid="<<GetCurrentProcessId()<<" session="<<sessionId
        <<" active_session="<<active<<" thread_desktop="<<t
        <<" input_desktop="<<i<<"\n";
    if(sessionId!=active || t!="Default" || i!="Default") {
        out << "ABORT_NO_NORMAL_INTERACTIVE_DESKTOP\n";return 11;
    }
    // Read-only DXGI access triage; no token or desktop ACL mutations.
    auto station=GetProcessWindowStation();
    const std::string stationName=DesktopName(reinterpret_cast<HDESK>(station));
    HDESK verify=OpenDesktopW(L"Default",0,FALSE,
        DESKTOP_READOBJECTS|DESKTOP_ENUMERATE);
    const DWORD verifyError=verify?ERROR_SUCCESS:GetLastError();
    if(verify)CloseDesktop(verify);
    DWORD tokenLevel=0;
    HANDLE processToken=nullptr;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&processToken)) {
        DWORD bytes=0;
        GetTokenInformation(processToken,TokenIntegrityLevel,nullptr,0,&bytes);
        if(bytes>=sizeof(TOKEN_MANDATORY_LABEL)&&bytes<4096) {
            std::vector<BYTE> buf(bytes);
            if(GetTokenInformation(processToken,TokenIntegrityLevel,
                buf.data(),bytes,&bytes)) {
                auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data())->Label.Sid;
                const DWORD count=*GetSidSubAuthorityCount(sid);
                if(count)tokenLevel=*GetSidSubAuthority(sid,count-1);
            }
        }
        CloseHandle(processToken);
    }
    out<<"ACCESS_WINDOW_STATION="<<stationName
       <<" DEFAULT_DESKTOP_READ_ENUM="<<(verify?1:0)
       <<" ACCESS_ERROR="<<verifyError
       <<" TOKEN_INTEGRITY_RID="<<tokenLevel
       <<" REMOTE_SESSION_FLAG="<<GetSystemMetrics(SM_REMOTESESSION)<<"\n";
    Monitor monitor;
    EnumDisplayMonitors(nullptr,nullptr,VisitMon,reinterpret_cast<LPARAM>(&monitor));
    if(!monitor.handle) {out<<"NO_PRIMARY_MONITOR\n";return 12;}
    out << "DISPLAY_PHYSICAL="<<(monitor.rect.right-monitor.rect.left)<<"x"
        <<(monitor.rect.bottom-monitor.rect.top)
        << " SYSTEM_DPI="<<GetDpiForSystem()
        << " DPI_CONTEXT_PER_MONITOR_V2="<<
           (AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
               DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)?1:0)<<"\n";
    out << DxgiCheck(monitor.handle)<<"\n"; // Duplication is released before WGC.
    const HRESULT init=RoInitialize(RO_INIT_MULTITHREADED);
    if(FAILED(init) && init!=RPC_E_CHANGED_MODE) {
        out << "WINRT_INIT="<<Hex(init)<<"\n";return 13;
    }
    const bool uninit=SUCCEEDED(init);
    try {
        auto activation=winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
        auto interop=activation.as<IGraphicsCaptureItemInterop>();
        WGC::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForMonitor(monitor.handle,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,
            nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,
            D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        winrt::com_ptr<::IInspectable> inspect;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspect.put()));
        auto winrtDevice=inspect.as<D3D::IDirect3DDevice>();
        auto pool=WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,item.Size());
        auto session=pool.CreateCaptureSession(item);
        out<<"WGC_ITEM_SIZE="<<item.Size().Width<<"x"<<item.Size().Height
           <<" WGC_MIN_INTERVAL_100NS="<<session.MinUpdateInterval().count()<<"\n";
        AnimatedWindow animation;
        auto samples=std::make_shared<Samples>();
        auto token=pool.FrameArrived([samples](
            WGC::Direct3D11CaptureFramePool const& sender,
            winrt::Windows::Foundation::IInspectable const&) noexcept {
            const auto now=std::chrono::steady_clock::now().time_since_epoch();
            const int64_t ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            int64_t prev=samples->lastArrivalNs.exchange(ns,std::memory_order_relaxed);
            if(prev>0 && ns>prev) {
                uint64_t gap=static_cast<uint64_t>(ns-prev);
                samples->AddGap(samples->gapTotalNs,samples->gapCount,gap);
                uint64_t old=samples->gapMaxNs.load(std::memory_order_relaxed);
                while(gap>old && !samples->gapMaxNs.compare_exchange_weak(
                    old,gap,std::memory_order_relaxed)) {}
            }
            samples->arrived.fetch_add(1,std::memory_order_relaxed);
            try {
                // NO GPU staging, memcpy, pixel access, colour conversion, encoder or IPC.
                // Consume and immediately release only the WinRT frame reference.
                auto frame=sender.TryGetNextFrame();
                if(frame) {
                    int64_t stamp=frame.SystemRelativeTime().count();
                    int64_t last=samples->lastSource100ns.exchange(stamp,std::memory_order_relaxed);
                    if(last>0 && stamp>last)
                        samples->AddGap(samples->sourceGapTotal100ns,
                                        samples->sourceGapCount,
                                        static_cast<uint64_t>(stamp-last));
                    samples->consumed.fetch_add(1,std::memory_order_relaxed);
                }
            } catch (...) {samples->exceptions.fetch_add(1,std::memory_order_relaxed);}
        });
        session.StartCapture();
        out << "WGC_STARTED probe_seconds=15 baseline_first_3sec then_60Hz_GDI_local_animation_12sec privacy_border=default\n";
        uint64_t prev=0,priorPaints=0,priorTicks=0;
        for(int i=1;i<=5;++i) {
            if(i==2) {
                bool started=animation.Start();
                out<<"LOCAL_ANIMATION_STARTED="<<(started?1:0)<<"\n";
                if(!started) {out<<"LOCAL_ANIMATION_UNAVAILABLE\n";break;}
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
            auto n=samples->consumed.load(std::memory_order_relaxed);
            const uint64_t nowPaints=animation.paints.load(std::memory_order_relaxed);
            const uint64_t nowTicks=animation.ticks.load(std::memory_order_relaxed);
            double arrivalMeanMs=samples->gapCount.load()?
                (double(samples->gapTotalNs.load())/double(samples->gapCount.load()))/1e6:0.;
            double sourceMeanMs=samples->sourceGapCount.load()?
                (double(samples->sourceGapTotal100ns.load())/double(samples->sourceGapCount.load()))/1e4:0.;
            out << "WINDOW="<<i<<" frames="<<(n-prev)<<" fps="<<std::fixed
                <<std::setprecision(2)<<(n-prev)/3.0
                <<" total_frames="<<n<<" callbacks="<<samples->arrived.load()
                <<" arrival_gap_ms="<<arrivalMeanMs
                <<" source_gap_ms="<<sourceMeanMs
                <<" max_arrival_gap_ms="<<samples->gapMaxNs.load()/1e6
                <<" errors="<<samples->exceptions.load()
                <<" animation_paints="<<(nowPaints-priorPaints)
                <<" animation_paint_fps="<<(nowPaints-priorPaints)/3.0
                <<" timer_ticks="<<(nowTicks-priorTicks)<<"\n";
            prev=n;priorPaints=nowPaints;priorTicks=nowTicks;out.flush();
        }
        animation.Stop();
        session.Close();
        pool.FrameArrived(token);
        pool.Close();
        out<<"PROBE_COMPLETE frames="<<samples->consumed.load()<<" callbacks="
           <<samples->arrived.load()<<" errors="<<samples->exceptions.load()<<"\n";
    } catch(winrt::hresult_error const& ex) {
        out<<"WGC_HRESULT="<<Hex(ex.code())<<"\n";
        if(uninit)RoUninitialize();return 14;
    } catch(std::exception const& ex) {
        out<<"WGC_EXCEPTION="<<ex.what()<<"\n";
        if(uninit)RoUninitialize();return 15;
    }
    if(uninit)RoUninitialize();
    return 0;
}
int Spawn(bool useSystem) {
    const DWORD active=WTSGetActiveConsoleSessionId();
    if(active==0xffffffff) {std::cout<<"NO_ACTIVE_CONSOLE\n";return 21;}
    if(!Enable(L"SeTcbPrivilege")) std::cout<<"WARN_NO_TCB_PRIVILEGE\n";
    Enable(L"SeAssignPrimaryTokenPrivilege");
    Enable(L"SeIncreaseQuotaPrivilege");
    HANDLE token=nullptr;
    if(useSystem) {
        HANDLE original=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY|
            TOKEN_QUERY|TOKEN_ADJUST_SESSIONID,&original)) {
            std::cout<<"OPEN_SYSTEM_TOKEN_FAIL="<<GetLastError()<<"\n";return 22;
        }
        const BOOL dup=DuplicateTokenEx(original,MAXIMUM_ALLOWED,nullptr,
            SecurityImpersonation,TokenPrimary,&token);
        CloseHandle(original);
        if(!dup) {std::cout<<"DUP_SYSTEM_TOKEN_FAIL="<<GetLastError()<<"\n";return 23;}
        if(!SetTokenInformation(token,TokenSessionId,
             const_cast<DWORD*>(&active),sizeof(active))) {
            std::cout<<"SET_SYSTEM_SESSION_FAIL="<<GetLastError()<<"\n";
            CloseHandle(token);return 24;
        }
    } else if(!WTSQueryUserToken(active,&token)) {
        std::cout<<"WTS_QUERY_USER_TOKEN_FAIL="<<GetLastError()<<"\n";return 25;
    }
    wchar_t path[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr,path,MAX_PATH)) {
        CloseHandle(token);return 26;
    }
    const wchar_t* resultPath=useSystem?kOutSystem:kOutUser;
    DeleteFileW(resultPath);
    std::wstring cmd=L"\""+std::wstring(path)+
        (useSystem?L"\" --run-system":L"\" --run-user");
    std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end());mutableCmd.push_back(0);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    startup.lpDesktop=const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    void* env=nullptr;
    CreateEnvironmentBlock(&env,token,FALSE);
    const BOOL launched=CreateProcessAsUserW(token,path,mutableCmd.data(),
        nullptr,nullptr,FALSE,CREATE_NO_WINDOW|(env?CREATE_UNICODE_ENVIRONMENT:0),
        env,nullptr,&startup,&pi);
    const DWORD err=GetLastError();
    if(env)DestroyEnvironmentBlock(env);
    CloseHandle(token);
    if(!launched){std::cout<<"SPAWN_FAIL="<<err<<"\n";return 27;}
    DWORD wait=WaitForSingleObject(pi.hProcess,35000);
    DWORD exitCode=0;GetExitCodeProcess(pi.hProcess,&exitCode);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    std::cout<<"CHILD="<<(useSystem?"SYSTEM":"USER")
             <<" wait="<<wait<<" exit="<<exitCode<<"\n";
    std::ifstream f(resultPath);
    if(!f) {std::cout<<"CHILD_RESULT_FILE_MISSING\n";return 28;}
    std::cout<<f.rdbuf();
    return wait==WAIT_OBJECT_0?int(exitCode):29;
}
int wmain(int argc,wchar_t** argv) {
    // This occurs BEFORE monitor enumeration, WGC creation, and Win32 windows.
    const BOOL dpi=SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!dpi && GetLastError()!=ERROR_ACCESS_DENIED) {
        std::cout<<"DPI_AWARENESS_SET_ERROR="<<GetLastError()<<"\n";
    }
    if(argc!=2) {
        std::cout<<"USAGE: Hi5WgcProbe.exe --launch|--run-user|--run-system\n";return 2;
    }
    const std::wstring mode=argv[1];
    if(mode==L"--launch") {
        std::cout<<"HI5_WGC_STANDALONE_DIAGNOSTIC_ONLY no_encoder no_network\n";
        const int user=Spawn(false);
        const int system=Spawn(true);
        std::cout<<"FINAL user_exit="<<user<<" system_exit="<<system<<"\n";
        return user==0&&system==0?0:3;
    }
    if(mode==L"--run-user"||mode==L"--run-system") {
        std::ofstream out(mode==L"--run-user"?kOutUser:kOutSystem,std::ios::trunc);
        if(!out) {std::cout<<"OUTPUT_OPEN_FAILED\n";return 31;}
        return RunProbe(out);
    }
    return 2;
}
