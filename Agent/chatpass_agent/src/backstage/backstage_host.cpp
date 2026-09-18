#include "backstage_host.h"

#include "frame_source.h"
#include "ipc/input_pipe.h"
#include "ipc/shmem_ring.h"
#include "ipc/session_launcher.h"
#include "util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <wtsapi32.h>
#include <cstdio>
#include <shlwapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <deque>

namespace {

    std::atomic<bool> g_running{ true };

    void SignalHandler(int) {
        g_running.store(false);
    }

    int EnvInt(const char* name, int fallback, int minValue, int maxValue) {
        char buf[64]{};
        DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf)) return fallback;
        try {
            int v = std::stoi(std::string(buf));
            return std::max(minValue, std::min(maxValue, v));
        }
        catch (...) {
            return fallback;
        }
    }

    struct Args {
        std::string sessionId;
        std::string shmemName;
        std::string inputPipeName;
        std::string stopEventName;
        int fps = 15;
        int width = 1280;
        int height = 720;
        bool nativeDesktop = false;
    };

    bool HasArg(int argc, char** argv, const std::string& key) {
        for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == key) return true;
        return false;
    }

    std::optional<std::string> GetArgValue(int argc, char** argv, const std::string& key) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == key) return std::string(argv[i + 1]);
        }
        return std::nullopt;
    }

    Args ParseArgs(int argc, char** argv) {
        Args a;
        if (auto v = GetArgValue(argc, argv, "--session")) a.sessionId = *v;
        if (auto v = GetArgValue(argc, argv, "--shmem")) a.shmemName = *v;
        if (auto v = GetArgValue(argc, argv, "--input-pipe")) a.inputPipeName = *v;
        if (auto v = GetArgValue(argc, argv, "--stop-event")) a.stopEventName = *v;
        if (auto v = GetArgValue(argc, argv, "--fps")) a.fps = std::max(5, std::min(30, std::stoi(*v)));
        if (auto v = GetArgValue(argc, argv, "--width")) a.width = std::max(800, std::min(3840, std::stoi(*v)));
        if (auto v = GetArgValue(argc, argv, "--height")) a.height = std::max(600, std::min(2160, std::stoi(*v)));
        a.nativeDesktop = HasArg(argc, argv, "--native-desktop");
        return a;
    }

    uint8_t ClampByte(int v) {
        return static_cast<uint8_t>(std::max(0, std::min(255, v)));
    }

    void BgraToI420(const uint8_t* src, int srcStride, int width, int height, I420Frame& out) {
        out.width = width;
        out.height = height;
        out.y.resize(static_cast<size_t>(width) * height);
        out.u.resize(static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2));
        out.v.resize(static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2));

        for (int y = 0; y < height; ++y) {
            const uint8_t* row = src + y * srcStride;
            for (int x = 0; x < width; ++x) {
                const uint8_t b = row[x * 4 + 0];
                const uint8_t g = row[x * 4 + 1];
                const uint8_t r = row[x * 4 + 2];
                const int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                out.y[static_cast<size_t>(y) * width + x] = ClampByte(Y);
            }
        }

        const int uvW = (width + 1) / 2;
        const int uvH = (height + 1) / 2;
        for (int by = 0; by < uvH; ++by) {
            for (int bx = 0; bx < uvW; ++bx) {
                int sumU = 0, sumV = 0, count = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = bx * 2 + dx;
                        const int y = by * 2 + dy;
                        if (x >= width || y >= height) continue;
                        const uint8_t* px = src + y * srcStride + x * 4;
                        const uint8_t b = px[0], g = px[1], r = px[2];
                        const int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                        const int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                        sumU += U; sumV += V; ++count;
                    }
                }
                if (count <= 0) count = 1;
                out.u[static_cast<size_t>(by) * uvW + bx] = ClampByte(sumU / count);
                out.v[static_cast<size_t>(by) * uvW + bx] = ClampByte(sumV / count);
            }
        }
    }

    std::wstring Utf8ToWide(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(std::max(0, n - 1), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    std::string WideToUtf8(const std::wstring& value) {
        if (value.empty()) return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::wstring BytesToWideOem(const std::string& bytes) {
        if (bytes.empty()) return {};
        int needed = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        UINT cp = CP_OEMCP;
        if (needed <= 0) {
            cp = CP_UTF8;
            needed = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (needed <= 0) return L"[output conversion failed]";
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), needed);
        return out;
    }

    std::wstring CurrentExePathW() {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return n ? std::wstring(buf, buf + n) : std::wstring();
    }

    std::wstring FileSizeText(unsigned long long size) {
        wchar_t buf[64]{};
        if (size >= 1024ull * 1024ull * 1024ull) swprintf_s(buf, L"%.1f GB", static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
        else if (size >= 1024ull * 1024ull) swprintf_s(buf, L"%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
        else if (size >= 1024ull) swprintf_s(buf, L"%.1f KB", static_cast<double>(size) / 1024.0);
        else swprintf_s(buf, L"%llu B", size);
        return buf;
    }

    static bool LineHasUpdateRow(const std::wstring& line) {
        return line.rfind(L"[ ] ", 0) == 0 || line.rfind(L"[x] ", 0) == 0;
    }

    static std::wstring ProgressBarText(int percent, const std::wstring& label) {
        percent = std::max(0, std::min(100, percent));
        const int blocks = 30;
        const int filled = (percent * blocks) / 100;
        std::wstring bar = L"Progress: [";
        for (int i = 0; i < blocks; ++i) bar += (i < filled ? L'#' : L'-');
        bar += L"] " + std::to_wstring(percent) + L"%  " + label;
        return bar;
    }

    enum class WindowKind {
        Services,
        Files,
        Processes,
        Apps,
        SystemInfo,
        Sessions,
        Events,
        Terminal,
        Notepad,
        Registry,
        Updates,
        Devices,
        Disks,
        Experimental,
        NativeApp
    };

    struct ToolSpec {
        const wchar_t* id;
        const wchar_t* title;
        const wchar_t* sub;
        const wchar_t* iconPath;
        WindowKind kind;
        const wchar_t* terminalMode;
    };

    struct NativeApp {
        std::wstring title;
        std::wstring exe;
        std::wstring args;
    };

    struct NativeMaintenanceApp {
        std::wstring group;
        std::wstring title;
        std::wstring exe;
        std::wstring args;
    };

    struct ServiceRow {
        std::wstring name;
        std::wstring display;
        std::wstring stateText;
        std::wstring startTypeText;
        DWORD state = 0;
        DWORD startType = SERVICE_DEMAND_START;
    };

    struct FileRow {
        std::wstring name;
        std::wstring path;
        bool isDir = false;
        unsigned long long size = 0;
    };

    struct AppRow {
        std::wstring name;
        std::wstring publisher;
        std::wstring version;
        std::wstring uninstall;
        std::wstring quietUninstall;
    };

    struct BackgroundJob {
        std::atomic<bool> running{ false };
        std::atomic<bool> done{ false };
        std::atomic<int> percent{ 0 };
        uint64_t startedTick = 0;
        uint64_t lastUiTick = 0;
        std::mutex mu;
        std::wstring title;
        std::wstring phase;
        std::wstring output;
        std::wstring finalStatus;
    };

    struct BackstageWindow {
        int id = 0;
        WindowKind kind = WindowKind::SystemInfo;
        std::wstring toolId;
        std::wstring title;
        std::wstring subtitle;
        std::wstring nativeExe;
        std::wstring nativeArgs;
        std::wstring terminalMode;
        RECT rect{ 360, 86, 1160, 610 };
        RECT restoreRect{ 360, 86, 1160, 610 };
        bool minimized = false;
        bool closed = false;
        bool maximized = false;
        bool snappedLeft = false;
        bool snappedRight = false;
        int selected = -1;
        int scroll = 0;
        std::wstring path = L"C:\\";
        std::vector<ServiceRow> services;
        std::vector<FileRow> files;
        std::vector<AppRow> apps;
        std::vector<std::wstring> lines;
        std::wstring noteText;
        std::wstring statusText;
        DWORD lastActionTick = 0;
        std::shared_ptr<BackgroundJob> updateJob;

        HANDLE process = nullptr;
        HANDLE inWrite = nullptr;
        std::thread reader;
        std::mutex terminalMu;
        std::vector<std::wstring> terminalLines;
        std::wstring terminalInput;
        std::atomic<bool> terminalStop{ false };

        DWORD nativePid = 0;
        HANDLE nativeProcess = nullptr;
        HWND nativeHwnd = nullptr;
        bool nativePlaced = false;

        int zOrder = 0;
    };

    class BackstageRenderer {
    public:
        BackstageRenderer(int width, int height, const std::string& sessionId, bool nativeDesktop)
            : w_(width), h_(height), sessionId_(sessionId), nativeModeRequested_(nativeDesktop) {
            CreateDib();
            InitDesktopIcons();
            if (nativeModeRequested_) nativeModeActive_ = InitializeNativeDesktop();
        }

        ~BackstageRenderer() {
            for (auto& win : windows_) if (!win.closed) StopTerminal(win);
            StopNativeDesktopProcesses();
            if (privateDesktop_) { CloseDesktop(privateDesktop_); privateDesktop_ = nullptr; }
            if (dib_) DeleteObject(dib_);
            if (memDc_) DeleteDC(memDc_);
        }

        void PublishMonitorInfo(hi5::InputPipeReader& pipe) {
            hi5::InputRingHeader::MonitorInfo mi{};
            mi.x = 0; mi.y = 0; mi.w = w_; mi.h = h_; mi.primary = 1;
            pipe.SetMonitorInfo(1, &mi);
            pipe.SetUACActive(false);
        }

        int HandleInput(hi5::InputPipeReader& pipe) {
            if (nativeModeActive_) return HandleNativeDesktopInput(pipe);
            int handled = 0;
            hi5::InputCmd cmd{};
            while (pipe.Read(cmd)) {
                ++handled;
                switch (cmd.type) {
                case hi5::InputCmdType::MouseMove:
                    mouseX_ = std::max(0, std::min(w_ - 1, cmd.mouseMove.x));
                    mouseY_ = std::max(0, std::min(h_ - 1, cmd.mouseMove.y));
                    OnMouseMove(mouseX_, mouseY_);
                    break;
                case hi5::InputCmdType::MouseButton:
                    if (cmd.mouseButton.button == 0) {
                        if (cmd.mouseButton.down) OnMouseDown(mouseX_, mouseY_);
                        else OnMouseUp(mouseX_, mouseY_);
                    }
                    break;
                case hi5::InputCmdType::MouseWheel:
                    OnMouseWheel(cmd.mouseWheel.deltaY);
                    break;
                case hi5::InputCmdType::KeyEvent:
                    OnKey(cmd);
                    break;
                case hi5::InputCmdType::PasteText:
                case hi5::InputCmdType::ClipboardPaste: {
                    std::string text;
                    if (pipe.ReadClipboard(cmd.clipboard.offsetInClip, cmd.clipboard.length, text)) {
                        OnTextInput(Utf8ToWide(text));
                    }
                    break;
                }
                case hi5::InputCmdType::Shortcut:
                    HandleShortcut(cmd);
                    break;
                default:
                    break;
                }
            }
            return handled;
        }

        bool Render(I420Frame& out) {
            SyncBackgroundJobs();
            if (nativeModeActive_) {
                if (!DrawNativeDesktop()) {
                    nativeModeActive_ = false;
                    LogWarn("[background-native] private desktop render unavailable; fallback=synthetic");
                    DrawSynthetic();
                }
            } else {
                DrawSynthetic();
            }
            BgraToI420(reinterpret_cast<const uint8_t*>(bits_), w_ * 4, w_, h_, out);
            return true;
        }

        bool NativeModeActive() const { return nativeModeActive_; }

        bool HasVisibleNativeWindows() const {
            return nativeModeActive_ && nativeVisibleWindowCount_ > 0;
        }

        bool HasActiveBackgroundJob() const {
            for (const auto& win : windows_) {
                if (!win.closed && win.updateJob && !win.updateJob->done.load()) return true;
            }
            return false;
        }

    private:
        void CreateDib() {
            memDc_ = CreateCompatibleDC(nullptr);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w_;
            bi.bmiHeader.biHeight = -h_;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits_), nullptr, 0);
            SelectObject(memDc_, dib_);
        }

        const std::vector<ToolSpec>& Tools() const {
            static const std::vector<ToolSpec> tools = {
                { L"explorer",   L"File Explorer",   L"Backstage file browser",      L"C:\\Windows\\explorer.exe",       WindowKind::Files,      L"" },
                { L"services",   L"Services",        L"Service manager",             L"C:\\Windows\\System32\\services.msc", WindowKind::Services,   L"" },
                { L"taskmgr",    L"Task Manager",    L"Processes",                   L"C:\\Windows\\System32\\taskmgr.exe", WindowKind::Processes,  L"" },
                { L"cmd",        L"Command Prompt",  L"Interactive CMD",             L"C:\\Windows\\System32\\cmd.exe", WindowKind::Terminal,   L"cmd" },
                { L"powershell", L"PowerShell",      L"Interactive PowerShell",      L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", WindowKind::Terminal, L"powershell" },
                { L"apps",       L"Installed Apps",  L"Software inventory",          L"C:\\Windows\\System32\\appwiz.cpl", WindowKind::Apps,      L"" },
                { L"events",     L"Event Viewer",    L"Recent system events",        L"C:\\Windows\\System32\\eventvwr.msc", WindowKind::Events,    L"" },
                { L"system",     L"System Info",     L"System overview",             L"C:\\Windows\\System32\\SystemPropertiesComputerName.exe", WindowKind::SystemInfo, L"" },
                { L"sessions",   L"Users & Sessions", L"Interactive sessions and SYSTEM context", L"C:\\Windows\\System32\\taskmgr.exe", WindowKind::Sessions, L"" },
                { L"notepad",    L"Notepad",         L"Backstage notes",             L"C:\\Windows\\System32\\notepad.exe", WindowKind::Notepad,   L"" },
                { L"updates",    L"Windows Update",  L"Scan/install/reboot status",  L"C:\\Windows\\System32\\UsoClient.exe", WindowKind::Updates,   L"" },
                { L"registry",   L"Registry",        L"Registry browser",            L"C:\\Windows\\regedit.exe", WindowKind::Registry, L"" },
                { L"devices",    L"Device Manager",  L"Hardware/device list",        L"C:\\Windows\\System32\\devmgmt.msc", WindowKind::Devices, L"" },
                { L"disks",      L"Disks",           L"Volumes and free space",      L"C:\\Windows\\System32\\diskmgmt.msc", WindowKind::Disks, L"" }, };
            return tools;
        }

        const std::vector<NativeApp>& ExperimentalNativeApps() const {
            static const std::vector<NativeApp> apps = {
                { L"Services MMC", L"C:\\Windows\\System32\\mmc.exe", L"services.msc" },
                { L"Computer Management", L"C:\\Windows\\System32\\mmc.exe", L"compmgmt.msc" },
                { L"Device Manager", L"C:\\Windows\\System32\\mmc.exe", L"devmgmt.msc" },
                { L"Event Viewer", L"C:\\Windows\\System32\\mmc.exe", L"eventvwr.msc" },
                { L"Disk Management", L"C:\\Windows\\System32\\mmc.exe", L"diskmgmt.msc" },
                { L"Registry Editor", L"C:\\Windows\\regedit.exe", L"" },
                { L"Programs and Features", L"C:\\Windows\\System32\\control.exe", L"appwiz.cpl" },
                { L"Task Manager", L"C:\\Windows\\System32\\taskmgr.exe", L"" },
                { L"Command Prompt", L"C:\\Windows\\System32\\cmd.exe", L"" },
                { L"PowerShell", L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", L"" },
            };
            return apps;
        }


        const std::vector<NativeMaintenanceApp>& NativeMaintenanceApps() const {
            static const std::vector<NativeMaintenanceApp> apps = {
                { L"Windows Tools", L"Services", L"C:\\Windows\\System32\\mmc.exe", L"services.msc" },
                { L"Windows Tools", L"Computer Management", L"C:\\Windows\\System32\\mmc.exe", L"compmgmt.msc" },
                { L"Windows Tools", L"Event Viewer", L"C:\\Windows\\System32\\mmc.exe", L"eventvwr.msc" },
                { L"Windows Tools", L"Device Manager", L"C:\\Windows\\System32\\mmc.exe", L"devmgmt.msc" },
                { L"Windows Tools", L"Disk Management", L"C:\\Windows\\System32\\mmc.exe", L"diskmgmt.msc" },
                { L"Windows Tools", L"Registry Editor", L"C:\\Windows\\regedit.exe", L"" },
                { L"Windows Tools", L"Task Manager", L"C:\\Windows\\System32\\taskmgr.exe", L"" },
                { L"Windows Tools", L"Programs and Features", L"C:\\Windows\\System32\\control.exe", L"appwiz.cpl" },
                { L"Windows Accessories", L"Command Prompt", L"C:\\Windows\\System32\\cmd.exe", L"" },
                { L"Windows Accessories", L"PowerShell", L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", L"" },
                { L"Windows Accessories", L"Notepad", L"C:\\Windows\\System32\\notepad.exe", L"" },
                { L"Windows Accessories", L"System Information", L"C:\\Windows\\System32\\msinfo32.exe", L"" },
                { L"System Settings", L"Control Panel", L"C:\\Windows\\System32\\control.exe", L"" },
                { L"System Settings", L"System Properties", L"C:\\Windows\\System32\\SystemPropertiesAdvanced.exe", L"" },
                { L"System Settings", L"Network Connections", L"C:\\Windows\\System32\\control.exe", L"ncpa.cpl" },
                { L"System Settings", L"Windows Features", L"C:\\Windows\\System32\\OptionalFeatures.exe", L"" },
                { L"System Settings", L"Task Scheduler", L"C:\\Windows\\System32\\mmc.exe", L"taskschd.msc" },
                { L"System Settings", L"Resource Monitor", L"C:\\Windows\\System32\\resmon.exe", L"" },
                { L"System Settings", L"Local Users and Groups", L"C:\\Windows\\System32\\mmc.exe", L"lusrmgr.msc" },
                { L"System Settings", L"System Configuration", L"C:\\Windows\\System32\\msconfig.exe", L"" },
                { L"System Settings", L"Windows Update", L"", L"" },
                { L"System Settings", L"Users & Sessions", L"", L"" },
                { L"Network Tools", L"Hi5 Web", L"", L"" },
            };
            return apps;
        }

        void InitDesktopIcons() {
            iconPositions_.clear();
            for (size_t i = 0; i < Tools().size(); ++i) {
                const int col = static_cast<int>(i / 7);
                const int row = static_cast<int>(i % 7);
                iconPositions_.push_back(POINT{ 30 + col * 112, 58 + row * 88 });
            }
        }

        RECT DesktopIconRect(size_t i) const {
            if (i >= iconPositions_.size()) return RECT{ 0,0,0,0 };
            POINT p = iconPositions_[i];
            return RECT{ p.x, p.y, p.x + 94, p.y + 82 };
        }

        RECT StartRect() const {
            const int top = h_ - taskbarH_;
            return RECT{ (w_ / 2) - 360, top + 5, (w_ / 2) - 322, top + 39 };
        }

        RECT SearchRect() const {
            const int top = h_ - taskbarH_;
            return RECT{ (w_ / 2) - 310, top + 5, (w_ / 2) - 90, top + 39 };
        }

        RECT PinnedIconRect(size_t i) const {
            const int top = h_ - taskbarH_;
            const int icon = 34;
            const int gap = 7;
            const int x0 = (w_ / 2) - 78;
            return RECT{ x0 + static_cast<int>(i) * (icon + gap), top + 5, x0 + static_cast<int>(i) * (icon + gap) + icon, top + 39 };
        }

        RECT StartMenuRect() const {
            const int menuW = 650;
            const int menuH = std::min(570, h_ - 110);
            return RECT{ (w_ - menuW) / 2, h_ - taskbarH_ - menuH - 10, (w_ + menuW) / 2, h_ - taskbarH_ - 10 };
        }

        RECT StartSearchBoxRect() const {
            RECT m = StartMenuRect();
            return RECT{ m.left + 28, m.top + 26, m.right - 28, m.top + 64 };
        }

        RECT StartTileRect(size_t visibleIndex) const {
            RECT m = StartMenuRect();
            const int col = static_cast<int>(visibleIndex % 3);
            const int row = static_cast<int>(visibleIndex / 3);
            const int x = m.left + 30 + col * 198;
            const int y = m.top + 112 + row * 78;
            return RECT{ x, y, x + 178, y + 64 };
        }

        RECT TitleBarRect(const BackstageWindow& win) const {
            return RECT{ win.rect.left, win.rect.top, win.rect.right, win.rect.top + titleH_ };
        }

        RECT CloseRect(const BackstageWindow& win) const {
            return RECT{ win.rect.right - 46, win.rect.top + 1, win.rect.right - 2, win.rect.top + titleH_ - 1 };
        }

        RECT MaxRect(const BackstageWindow& win) const {
            return RECT{ win.rect.right - 92, win.rect.top + 1, win.rect.right - 48, win.rect.top + titleH_ - 1 };
        }

        RECT MinRect(const BackstageWindow& win) const {
            return RECT{ win.rect.right - 138, win.rect.top + 1, win.rect.right - 94, win.rect.top + titleH_ - 1 };
        }

        RECT ContentRect(const BackstageWindow& win) const {
            return RECT{ win.rect.left + 1, win.rect.top + titleH_, win.rect.right - 1, win.rect.bottom - 1 };
        }

        RECT ManagedWorkspaceRect() const {
            const int shellTaskbar = nativeModeActive_ ? nativeTaskbarH_ : taskbarH_;
            const int top = nativeModeActive_ ? kNativeShellTopH : 0;
            return RECT{ 0, top, w_, std::max(top + 120, h_ - shellTaskbar) };
        }

        RECT DefaultManagedWindowRect(size_t ordinal) const {
            const int margin = 14;
            const RECT workspace = ManagedWorkspaceRect();
            const int workLeft = workspace.left + margin;
            const int workTop = workspace.top + margin;
            const int workRight = std::max(workLeft + 360L, workspace.right - margin);
            const int workBottom = std::max(workTop + 280L, workspace.bottom - margin);
            const int workW = std::max(360, workRight - workLeft);
            const int workH = std::max(280, workBottom - workTop);

            if (workW < 860 || workH < 520) {
                const int ww = std::min(workW, 820);
                const int wh = std::min(workH, 540);
                const int shift = static_cast<int>(ordinal % 5) * 22;
                int left = workLeft + shift;
                int top = workTop + shift;
                if (left + ww > workRight) left = std::max(workLeft, workRight - ww);
                if (top + wh > workBottom) top = std::max(workTop, workBottom - wh);
                return RECT{ left, top, left + ww, top + wh };
            }

            const int slot = static_cast<int>(ordinal % 5);
            if (slot == 0) {
                return RECT{ workLeft, workTop,
                    workLeft + (workW * 52) / 100,
                    workTop + (workH * 55) / 100 };
            }
            if (slot == 1) {
                return RECT{ workLeft + (workW * 54) / 100, workTop,
                    workRight, workTop + (workH * 56) / 100 };
            }
            if (slot == 2) {
                return RECT{ workLeft, workTop + (workH * 58) / 100,
                    workLeft + (workW * 40) / 100, workBottom };
            }
            if (slot == 3) {
                return RECT{ workLeft + (workW * 42) / 100, workTop + (workH * 58) / 100,
                    workLeft + (workW * 70) / 100, workBottom };
            }
            return RECT{ workLeft + (workW * 72) / 100, workTop + (workH * 58) / 100,
                workRight, workBottom };
        }

        int TaskbarRunStartX() const {
            const int icon = 34;
            const int gap = 7;
            const int pinnedStart = (w_ / 2) - 78;
            return pinnedStart + static_cast<int>(PinnedCount()) * (icon + gap) + 16;
        }
        int TaskbarRunButtonW() const { return 38; }
        int TaskbarRunGap() const { return 7; }
        int TaskbarRunStep() const { return TaskbarRunButtonW() + TaskbarRunGap(); }

        size_t TaskbarVisibleSlots() const {
            const int reservedRight = 140;
            const int available = std::max(0, (w_ - reservedRight) - TaskbarRunStartX() - 68);
            return static_cast<size_t>(std::max(1, available / TaskbarRunStep()));
        }

        RECT TaskbarWindowButtonRect(size_t visibleIndex) const {
            const int top = h_ - taskbarH_;
            const int startX = TaskbarRunStartX();
            const int x = startX + static_cast<int>(visibleIndex) * TaskbarRunStep();
            return RECT{ x, top + 5, x + TaskbarRunButtonW(), top + 39 };
        }

        RECT TaskbarPrevPageRect() const {
            const int top = h_ - taskbarH_;
            const int x = TaskbarRunStartX() - 28;
            return RECT{ x, top + 8, x + 22, top + 36 };
        }

        RECT TaskbarNextPageRect() const {
            const int top = h_ - taskbarH_;
            const int x = TaskbarRunStartX() + static_cast<int>(TaskbarVisibleSlots()) * TaskbarRunStep() + 4;
            return RECT{ x, top + 8, x + 22, top + 36 };
        }

        std::vector<BackstageWindow*> UnpinnedTaskbarWindows() {
            std::vector<BackstageWindow*> out;
            for (auto& win : windows_) {
                if (!win.closed && !IsPinnedToolId(win.toolId)) out.push_back(&win);
            }
            return out;
        }

        std::vector<const BackstageWindow*> UnpinnedTaskbarWindowsConst() const {
            std::vector<const BackstageWindow*> out;
            for (const auto& win : windows_) {
                if (!win.closed && !IsPinnedToolId(win.toolId)) out.push_back(&win);
            }
            return out;
        }

        int FindWindowIndexById(int id) const {
            for (size_t i = 0; i < windows_.size(); ++i) if (!windows_[i].closed && windows_[i].id == id) return static_cast<int>(i);
            return -1;
        }

        BackstageWindow* ActiveWindow() {
            int idx = FindWindowIndexById(activeWindowId_);
            if (idx < 0) return nullptr;
            return &windows_[static_cast<size_t>(idx)];
        }

        const BackstageWindow* ActiveWindow() const {
            int idx = FindWindowIndexById(activeWindowId_);
            if (idx < 0) return nullptr;
            return &windows_[static_cast<size_t>(idx)];
        }

        void BringToFront(int id) {
            int idx = FindWindowIndexById(id);
            if (idx < 0) return;
            activeWindowId_ = id;
            windows_[static_cast<size_t>(idx)].minimized = false;
            windows_[static_cast<size_t>(idx)].zOrder = ++zCounter_;
            // Do not physically move BackstageWindow objects here. Terminal windows own
            // reader threads and pipe handles, so moving the object can invalidate the
            // reader target. Z-order is handled by focus/highlighting for this pass.
        }

        void CloseWindow(int id) {
            int idx = FindWindowIndexById(id);
            if (idx < 0) return;
            BackstageWindow& win = windows_[static_cast<size_t>(idx)];
            StopTerminal(win);
            StopNativeApp(win);
            win.closed = true;
            win.minimized = true;
            if (activeWindowId_ == id) {
                activeWindowId_ = 0;
                for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
                    if (!it->closed) { activeWindowId_ = it->id; break; }
                }
            }
        }

        void ToggleMaximize(BackstageWindow& win) {
            if (!win.maximized) {
                win.restoreRect = win.rect;
                win.rect = ManagedWorkspaceRect();
                win.maximized = true;
                win.snappedLeft = win.snappedRight = false;
            }
            else {
                win.rect = win.restoreRect;
                win.maximized = false;
            }
        }

        void SnapWindow(BackstageWindow& win, int mode) {
            const RECT work = ManagedWorkspaceRect();
            const int gap = 8;
            const int mid = work.left + (work.right - work.left) / 2;
            win.restoreRect = win.rect;
            if (mode == 1) {
                win.rect = RECT{ work.left + gap, work.top + gap, mid - (gap / 2), work.bottom - gap };
                win.snappedLeft = true; win.snappedRight = false; win.maximized = false;
            }
            else if (mode == 2) {
                win.rect = RECT{ mid + (gap / 2), work.top + gap, work.right - gap, work.bottom - gap };
                win.snappedLeft = false; win.snappedRight = true; win.maximized = false;
            }
            else if (mode == 3) {
                win.rect = work;
                win.maximized = true; win.snappedLeft = win.snappedRight = false;
            }
        }

        struct SearchResult {
            bool experimental = false;
            size_t index = 0;
            std::wstring title;
            std::wstring sub;
            std::wstring iconPath;
        };

        std::vector<SearchResult> MatchingSearchResults() const {
            std::vector<SearchResult> out;
            const std::wstring q = ToLower(startSearch_);
            for (size_t i = 0; i < Tools().size(); ++i) {
                std::wstring title = Tools()[i].title ? Tools()[i].title : L"";
                std::wstring sub = Tools()[i].sub ? Tools()[i].sub : L"";
                std::wstring blob = ToLower(title + L" " + sub + L" " + (Tools()[i].id ? Tools()[i].id : L""));
                if (q.empty() || blob.find(q) != std::wstring::npos) {
                    out.push_back(SearchResult{ false, i, title, sub, Tools()[i].iconPath ? Tools()[i].iconPath : L"" });
                }
            }
            return out;
        }

        std::vector<size_t> MatchingToolIndexes() const {
            std::vector<size_t> out;
            for (const auto& r : MatchingSearchResults()) {
                if (!r.experimental) out.push_back(r.index);
            }
            return out;
        }

        std::wstring ToLower(std::wstring s) const {
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            return s;
        }

        void OnMouseDown(int x, int y) {
            mouseDown_ = true;
            dragMode_ = DragMode::None;

            POINT pt{ x, y };

            if (startMenuOpen_) {
                if (HandleStartMenuClick(x, y)) return;
                if (!PtInRect(&StartMenuRect(), pt) && !PtInRect(&StartRect(), pt)) {
                    startMenuOpen_ = false;
                    return;
                }
                return;
            }

            if (PtInRect(&StartRect(), pt)) {
                startMenuOpen_ = true;
                startSearch_.clear();
                focusStartSearch_ = true;
                return;
            }

            if (PtInRect(&SearchRect(), pt)) {
                startMenuOpen_ = true;
                focusStartSearch_ = true;
                return;
            }

            if (HandleTaskbarClick(x, y)) return;

            // Top-most visible window first by z-order.
            std::vector<BackstageWindow*> hitOrder;
            for (auto& win : windows_) {
                if (!win.closed && !win.minimized) hitOrder.push_back(&win);
            }
            std::sort(hitOrder.begin(), hitOrder.end(), [](const BackstageWindow* a, const BackstageWindow* b) { return a->zOrder > b->zOrder; });
            for (BackstageWindow* candidate : hitOrder) {
                if (!candidate || !PtInRect(&candidate->rect, pt)) continue;
                BringToFront(candidate->id);
                BackstageWindow* active = ActiveWindow();
                if (!active) return;

                if (PtInRect(&CloseRect(*active), pt)) { CloseWindow(active->id); return; }
                if (PtInRect(&MaxRect(*active), pt)) { ToggleMaximize(*active); return; }
                if (PtInRect(&MinRect(*active), pt)) { active->minimized = true; return; }
                if (PtInRect(&TitleBarRect(*active), pt)) {
                    dragMode_ = DragMode::Window;
                    dragWindowId_ = active->id;
                    dragStart_ = pt;
                    dragOriginal_ = active->rect;
                    active->maximized = false;
                    return;
                }
                HandleWindowContentClick(*active, x, y);
                return;
            }

            // Desktop icons.
            const auto& tools = Tools();
            for (size_t i = 0; i < tools.size(); ++i) {
                RECT r = DesktopIconRect(i);
                if (PtInRect(&r, pt)) {
                    selectedDesktopIcon_ = static_cast<int>(i);
                    dragMode_ = DragMode::Icon;
                    dragIconIndex_ = static_cast<int>(i);
                    dragStart_ = pt;
                    dragIconOriginal_ = iconPositions_[i];
                    return;
                }
            }
        }

        void OnMouseMove(int x, int y) {
            if (!mouseDown_) {
                return;
            }
            POINT pt{ x, y };
            if (dragMode_ == DragMode::Window) {
                int idx = FindWindowIndexById(dragWindowId_);
                if (idx < 0) return;
                BackstageWindow& win = windows_[static_cast<size_t>(idx)];
                const int dx = pt.x - dragStart_.x;
                const int dy = pt.y - dragStart_.y;
                win.rect = RECT{ dragOriginal_.left + dx, dragOriginal_.top + dy, dragOriginal_.right + dx, dragOriginal_.bottom + dy };
                ClampWindowRect(win.rect);
            }
            else if (dragMode_ == DragMode::Icon && dragIconIndex_ >= 0 && dragIconIndex_ < static_cast<int>(iconPositions_.size())) {
                const int dx = pt.x - dragStart_.x;
                const int dy = pt.y - dragStart_.y;
                iconPositions_[static_cast<size_t>(dragIconIndex_)] = POINT{ dragIconOriginal_.x + dx, dragIconOriginal_.y + dy };
            }
        }

        void OnMouseUp(int x, int y) {
            mouseDown_ = false;
            if (dragMode_ == DragMode::Window) {
                int idx = FindWindowIndexById(dragWindowId_);
                if (idx >= 0) {
                    BackstageWindow& win = windows_[static_cast<size_t>(idx)];
                    if (y <= 4) SnapWindow(win, 3);
                    else if (x <= 6) SnapWindow(win, 1);
                    else if (x >= w_ - 7) SnapWindow(win, 2);
                }
            }
            else if (dragMode_ == DragMode::Icon &&
                dragIconIndex_ >= 0 &&
                dragIconIndex_ < static_cast<int>(Tools().size())) {
                // Treat a click/release on the desktop icon as launch. Only keep it
                // as a drag when the pointer actually moved a meaningful amount.
                const int dx = x - dragStart_.x;
                const int dy = y - dragStart_.y;
                const int moved = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                if (moved <= 8) {
                    selectedDesktopIcon_ = dragIconIndex_;
                    LaunchTool(Tools()[static_cast<size_t>(dragIconIndex_)]);
                }
            }
            dragMode_ = DragMode::None;
            dragWindowId_ = 0;
            dragIconIndex_ = -1;
        }

        void ClampWindowRect(RECT& r) const {
            const RECT work = ManagedWorkspaceRect();
            const int ww = std::max(300, static_cast<int>(r.right - r.left));
            const int wh = std::max(220, static_cast<int>(r.bottom - r.top));
            const int titleVisible = 72;

            if (r.left > work.right - titleVisible) {
                r.left = work.right - titleVisible;
                r.right = r.left + ww;
            }
            if (r.top < work.top) {
                r.top = work.top;
                r.bottom = r.top + wh;
            }
            if (r.top > work.bottom - titleH_) {
                r.top = work.bottom - titleH_;
                r.bottom = r.top + wh;
            }
            if (r.right < work.left + titleVisible) {
                r.right = work.left + titleVisible;
                r.left = r.right - ww;
            }
        }

        bool HandleTaskbarClick(int x, int y) {
            POINT pt{ x, y };
            const auto& tools = Tools();
            const size_t pinCount = PinnedCount();
            for (size_t i = 0; i < pinCount; ++i) {
                RECT r = PinnedIconRect(i);
                if (PtInRect(&r, pt)) {
                    const std::wstring id = tools[i].id ? tools[i].id : L"";
                    if (BackstageWindow* existing = FindOpenTool(id)) {
                        if (activeWindowId_ == existing->id && !existing->minimized) {
                            existing->minimized = true;
                        }
                        else {
                            BringToFront(existing->id);
                        }
                    }
                    else {
                        LaunchTool(tools[i]);
                    }
                    return true;
                }
            }

            auto unpinned = UnpinnedTaskbarWindows();
            const size_t slots = TaskbarVisibleSlots();
            const size_t maxPage = unpinned.empty() ? 0 : ((unpinned.size() - 1) / slots);
            if (taskbarPage_ > maxPage) taskbarPage_ = maxPage;

            if (unpinned.size() > slots) {
                RECT prev = TaskbarPrevPageRect();
                RECT next = TaskbarNextPageRect();
                if (PtInRect(&prev, pt)) { if (taskbarPage_ > 0) --taskbarPage_; return true; }
                if (PtInRect(&next, pt)) { if (taskbarPage_ < maxPage) ++taskbarPage_; return true; }
            }

            const size_t start = taskbarPage_ * slots;
            for (size_t local = 0; local < slots && start + local < unpinned.size(); ++local) {
                BackstageWindow* win = unpinned[start + local];
                if (!win) continue;
                RECT r = TaskbarWindowButtonRect(local);
                if (PtInRect(&r, pt)) {
                    if (activeWindowId_ == win->id && !win->minimized) win->minimized = true;
                    else BringToFront(win->id);
                    return true;
                }
            }
            return false;
        }

        bool HandleStartMenuClick(int x, int y) {
            POINT pt{ x, y };
            if (PtInRect(&StartRect(), pt)) {
                startMenuOpen_ = !startMenuOpen_;
                return true;
            }
            RECT search = StartSearchBoxRect();
            if (PtInRect(&search, pt)) {
                focusStartSearch_ = true;
                return true;
            }
            auto matches = MatchingSearchResults();
            for (size_t v = 0; v < matches.size(); ++v) {
                if (v >= 12) break;
                RECT r = StartTileRect(v);
                if (PtInRect(&r, pt)) {
                    LaunchTool(Tools()[matches[v].index]);
                    startMenuOpen_ = false;
                    return true;
                }
            }
            return PtInRect(&StartMenuRect(), pt) != 0;
        }

        void HandleWindowContentClick(BackstageWindow& win, int x, int y) {
            RECT c = ContentRect(win);
            if (!PtInRect(&c, POINT{ x, y })) return;
            if (HandleScrollbarClick(win, x, y)) return;
            if (HandleActionButtons(win, x, y)) return;
            if (win.kind == WindowKind::Files) {
                const int rowTop = WindowRowTop(win, c);
                const int rowH = WindowRowH(win);
                const int visibleIdx = (y - rowTop) / rowH;
                const int idx = win.scroll + visibleIdx;
                if (visibleIdx >= 0 && idx >= 0 && idx < static_cast<int>(win.files.size())) {
                    win.selected = idx;
                    if (win.files[static_cast<size_t>(idx)].isDir) {
                        win.path = win.files[static_cast<size_t>(idx)].path;
                        win.scroll = 0;
                        LoadFiles(win);
                    }
                }
            }
            else if (win.kind == WindowKind::Services) {
                const int rowTop = WindowRowTop(win, c);
                const int rowH = WindowRowH(win);
                const int visibleIdx = (y - rowTop) / rowH;
                const int idx = win.scroll + visibleIdx;
                if (visibleIdx >= 0 && idx >= 0 && idx < static_cast<int>(win.services.size())) win.selected = idx;
            }
            else if (win.kind == WindowKind::Processes || win.kind == WindowKind::Apps || win.kind == WindowKind::SystemInfo || win.kind == WindowKind::Sessions || win.kind == WindowKind::Events || win.kind == WindowKind::Updates || win.kind == WindowKind::Registry || win.kind == WindowKind::Devices || win.kind == WindowKind::Disks) {
                const int rowTop = WindowRowTop(win, c);
                const int rowH = WindowRowH(win);
                const int visibleIdx = (y - rowTop) / rowH;
                const int idx = win.scroll + visibleIdx;
                if (visibleIdx >= 0 && idx >= 0 && idx < static_cast<int>(win.lines.size())) {
                    win.selected = idx;
                    if (win.kind == WindowKind::Updates && idx >= 0 && idx < static_cast<int>(win.lines.size())) {
                        std::wstring& line = win.lines[static_cast<size_t>(idx)];
                        if (line.rfind(L"[ ] ", 0) == 0) { line.replace(0, 3, L"[x]"); return; }
                        if (line.rfind(L"[x] ", 0) == 0) { line.replace(0, 3, L"[ ]"); return; }
                    }
                    if (win.kind == WindowKind::Registry) HandleRegistryRowClick(win, idx);
                    else if (win.kind == WindowKind::Events) HandleEventRowClick(win, idx);
                }
            }
        }

        void LaunchExperimentalRow(int idx) {
            // Rows 0-2 are informational header rows in the Experimental window.
            const int appIndex = idx - 3;
            if (appIndex >= 0 && appIndex < static_cast<int>(ExperimentalNativeApps().size())) {
                LaunchExperimentalNativeApp(static_cast<size_t>(appIndex));
            }
        }

        void LaunchExperimentalNativeApp(size_t index) {
            if (index >= ExperimentalNativeApps().size()) return;
            const auto& app = ExperimentalNativeApps()[index];

            BackstageWindow& win = windows_.emplace_back();
            win.id = nextWindowId_++;
            win.kind = WindowKind::NativeApp;
            win.toolId = L"native:" + app.title;
            win.title = app.title;
            win.subtitle = L"Experimental real Windows app on private Backstage desktop";
            win.nativeExe = app.exe;
            win.nativeArgs = app.args;
            int offset = static_cast<int>(windows_.size() % 5) * 34;
            win.rect = RECT{ 330 + offset, 78 + offset, std::min(w_ - 34, 1040 + offset), std::min(h_ - taskbarH_ - 16, 610 + offset) };
            win.restoreRect = win.rect;
            win.zOrder = ++zCounter_;
            win.lines = {
                L"Launching experimental native Windows app...",
                L"App: " + app.title,
                L"Executable: " + app.exe,
                app.args.empty() ? L"Arguments: (none)" : L"Arguments: " + app.args,
                L"",
                L"This uses a private Backstage desktop so it should not appear on the user's visible desktop.",
                L"Rendering real Windows GUI apps here remains experimental without a virtual display backend."
            };
            activeWindowId_ = win.id;
            const bool ok = LaunchOnPrivateDesktop(app.exe, app.args, win.nativePid, win.nativeProcess);
            if (ok) {
                win.lines.push_back(L"Launch request sent to private Backstage desktop. Waiting for app window...");
                LogInfo("[backstage] PASS20Q native app window launched title=" + WideToUtf8(app.title) + " pid=" + std::to_string(win.nativePid));
            }
            else {
                win.lines.push_back(L"Launch failed. See agent log for details.");
            }
        }


        int WindowTotalRows(const BackstageWindow& win) const {
            if (win.kind == WindowKind::Services) return static_cast<int>(win.services.size());
            if (win.kind == WindowKind::Files) return static_cast<int>(win.files.size());
            return static_cast<int>(win.lines.size());
        }

        int WindowRowTop(const BackstageWindow& win, const RECT& c) const {
            if (win.kind == WindowKind::Services) return c.top + 62;
            if (win.kind == WindowKind::Files) return c.top + 82;
            return c.top + 54;
        }

        int WindowRowH(const BackstageWindow& win) const {
            if (win.kind == WindowKind::Services || win.kind == WindowKind::Files) return 30;
            return 24;
        }

        bool HandleScrollbarClick(BackstageWindow& win, int x, int y) {
            RECT c = ContentRect(win);
            const int total = WindowTotalRows(win);
            const int rowTop = WindowRowTop(win, c);
            const int rowH = WindowRowH(win);
            const int visible = VisibleRows(c, rowTop, rowH);
            if (total <= visible) return false;

            RECT track{ c.right - 18, c.top + 48, c.right, c.bottom - 4 };
            POINT pt{ x, y };
            if (!PtInRect(&track, pt)) return false;

            const int maxScroll = std::max(0, total - visible);
            const int trackH = std::max(24, static_cast<int>(track.bottom - track.top));
            const int thumbH = std::max(20, trackH * visible / std::max(1, total));
            const int thumbY = track.top + (trackH - thumbH) * std::max(0, std::min(win.scroll, maxScroll)) / std::max(1, maxScroll);

            if (y < thumbY) win.scroll = std::max(0, win.scroll - visible);
            else if (y > thumbY + thumbH) win.scroll = std::min(maxScroll, win.scroll + visible);
            else {
                const int rel = std::max(0, std::min(static_cast<int>(trackH), static_cast<int>(y - track.top)));
                win.scroll = std::min(maxScroll, (rel * std::max(1, maxScroll)) / std::max(1, trackH));
            }
            ClampScroll(win);
            return true;
        }

        bool LooksLikeProductCode(const std::wstring& s) const {
            return s.size() >= 38 && s.front() == L'{' && s.back() == L'}';
        }

        std::wstring ExtractProductCode(const std::wstring& command) const {
            const size_t start = command.find(L'{');
            if (start == std::wstring::npos) return L"";
            const size_t end = command.find(L'}', start);
            if (end == std::wstring::npos || end <= start) return L"";
            std::wstring code = command.substr(start, end - start + 1);
            return LooksLikeProductCode(code) ? code : L"";
        }

        bool HasToken(const std::wstring& lower, const std::wstring& token) const {
            return lower.find(token) != std::wstring::npos;
        }

        std::wstring QuoteIfNeeded(const std::wstring& path) const {
            if (path.empty()) return path;
            if (path.front() == L'\"') return path;
            if (path.find(L' ') != std::wstring::npos || path.find(L'\\') != std::wstring::npos) return L"\"" + path + L"\"";
            return path;
        }

        std::wstring BuildSilentUninstallCommand(const AppRow& app, bool* confident = nullptr) const {
            if (confident) *confident = false;
            if (!app.quietUninstall.empty()) { if (confident) *confident = true; return app.quietUninstall; }
            std::wstring cmd = app.uninstall;
            if (cmd.empty()) return L"";
            std::wstring lower = ToLower(cmd);

            const std::wstring code = ExtractProductCode(cmd);
            if (!code.empty() && (HasToken(lower, L"msiexec") || HasToken(lower, L"msiexec.exe") || HasToken(lower, L"msi"))) {
                if (confident) *confident = true;
                return L"msiexec.exe /x " + code + L" /qn /norestart";
            }

            // Only run EXE uninstallers when we can derive a recognised silent command.
            // This prevents GUI uninstallers appearing on the user's visible desktop.
            if (HasToken(lower, L" /quiet") || HasToken(lower, L" /silent") || HasToken(lower, L" /verysilent") ||
                HasToken(lower, L" /s ") || HasToken(lower, L" /s/") || HasToken(lower, L" /qn") || HasToken(lower, L"--silent") || HasToken(lower, L"--quiet")) {
                if (confident) *confident = true;
                if (!HasToken(lower, L"norestart")) cmd += L" /norestart";
                return cmd;
            }

            if (HasToken(lower, L"unins") || HasToken(lower, L"isuninst") || HasToken(lower, L"inno")) {
                if (confident) *confident = true;
                return cmd + L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-";
            }
            if (HasToken(lower, L"uninstall.exe") || HasToken(lower, L"uninstaller.exe") || HasToken(lower, L"uninstallhelper.exe") || HasToken(lower, L"nsis")) {
                if (confident) *confident = true;
                return cmd + L" /S";
            }
            if (HasToken(ToLower(app.name), L"git")) {
                if (confident) *confident = true;
                return cmd + L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-";
            }
            return L"";
        }

        std::wstring StripHi5ProgressLines(const std::wstring& text) const {
            std::wistringstream iss(text);
            std::wstring out;
            std::wstring line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.rfind(L"HI5_PROGRESS:", 0) == 0) continue;
                if (!out.empty()) out += L"\n";
                out += line;
            }
            return out;
        }

        void ApplyHi5ProgressMarkers(const std::shared_ptr<BackgroundJob>& job, const std::wstring& text) const {
            if (!job) return;
            std::wistringstream iss(text);
            std::wstring line;
            int lastPercent = -1;
            std::wstring lastPhase;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                const std::wstring prefix = L"HI5_PROGRESS:";
                if (line.rfind(prefix, 0) != 0) continue;
                const size_t p1 = prefix.size();
                const size_t p2 = line.find(L':', p1);
                if (p2 == std::wstring::npos) continue;
                try {
                    lastPercent = std::max(0, std::min(100, std::stoi(line.substr(p1, p2 - p1))));
                    lastPhase = line.substr(p2 + 1);
                }
                catch (...) {}
            }
            if (lastPercent >= 0) {
                std::lock_guard<std::mutex> lock(job->mu);
                job->percent.store(lastPercent);
                if (!lastPhase.empty()) job->phase = lastPhase;
            }
        }

        std::wstring RunCommandCaptureStreaming(const std::wstring& command, DWORD timeoutMs, const std::shared_ptr<BackgroundJob>& job) {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE outRead = nullptr, outWrite = nullptr;
            if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return L"CreatePipe failed: " + std::to_wstring(GetLastError());
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = outWrite;
            si.hStdError = outWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION pi{};
            std::wstring mutableCmd = L"cmd.exe /c " + command;
            BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(outWrite);
            if (!ok) {
                DWORD err = GetLastError();
                CloseHandle(outRead);
                return L"CreateProcess failed: " + std::to_wstring(err);
            }

            std::string bytes;
            char buf[4096];
            const uint64_t start = GetTickCount64();
            uint64_t lastPublish = 0;
            for (;;) {
                DWORD avail = 0;
                if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    DWORD got = 0;
                    if (ReadFile(outRead, buf, std::min<DWORD>(static_cast<DWORD>(sizeof(buf)), avail), &got, nullptr) && got) {
                        bytes.append(buf, buf + got);
                    }
                }

                const uint64_t now = GetTickCount64();
                if (job && (now - lastPublish >= 500)) {
                    std::wstring wide = BytesToWideOem(bytes);
                    ApplyHi5ProgressMarkers(job, wide);
                    {
                        std::lock_guard<std::mutex> lock(job->mu);
                        job->output = StripHi5ProgressLines(wide);
                        job->lastUiTick = now;
                    }
                    lastPublish = now;
                }

                DWORD wait = WaitForSingleObject(pi.hProcess, 50);
                if (wait == WAIT_OBJECT_0) {
                    while (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                        DWORD got = 0;
                        if (!ReadFile(outRead, buf, std::min<DWORD>(static_cast<DWORD>(sizeof(buf)), avail), &got, nullptr) || !got) break;
                        bytes.append(buf, buf + got);
                    }
                    break;
                }
                if (now - start > timeoutMs) {
                    TerminateProcess(pi.hProcess, 124);
                    bytes += "\r\n[command timed out]\r\n";
                    break;
                }
            }

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(outRead);

            std::wstring wide = BytesToWideOem(bytes);
            if (job) {
                ApplyHi5ProgressMarkers(job, wide);
                std::lock_guard<std::mutex> lock(job->mu);
                job->output = StripHi5ProgressLines(wide);
                job->lastUiTick = GetTickCount64();
            }
            return StripHi5ProgressLines(wide);
        }

        void SyncBackgroundJobs() {
            const uint64_t now = GetTickCount64();
            for (auto& win : windows_) {
                if (win.closed || !win.updateJob) continue;
                auto job = win.updateJob;
                std::wstring title, phase, output, finalStatus;
                int percent = job->percent.load();
                bool running = job->running.load();
                bool done = job->done.load();
                uint64_t started = job->startedTick;
                {
                    std::lock_guard<std::mutex> lock(job->mu);
                    title = job->title;
                    phase = job->phase;
                    output = job->output;
                    finalStatus = job->finalStatus;
                }
                if (phase.empty()) phase = running ? L"working" : L"ready";

                std::vector<std::wstring> lines;
                lines.push_back(L"Windows Update");
                lines.push_back(ProgressBarText(percent, phase));
                if (running && started > 0) lines.push_back(L"Elapsed: " + std::to_wstring((now - started) / 1000) + L" seconds");
                lines.push_back(L"Backstage suppresses automatic reboot; reboot must be triggered separately.");
                lines.push_back(L"");
                if (!title.empty()) lines.push_back(title);
                std::wistringstream iss(output);
                std::wstring line;
                int count = 0;
                while (std::getline(iss, line) && count < 220) {
                    if (!line.empty() && line.back() == L'\r') line.pop_back();
                    if (!line.empty()) { lines.push_back(line); ++count; }
                }
                if (done && !finalStatus.empty()) {
                    lines.push_back(L"");
                    lines.push_back(finalStatus);
                }
                win.lines = std::move(lines);
                win.statusText = done ? finalStatus : phase;
            }
        }

        void StartWindowsUpdateBackgroundJob(BackstageWindow& win, const std::wstring& title, const std::wstring& command, DWORD timeoutMs) {
            auto job = std::make_shared<BackgroundJob>();
            job->running.store(true);
            job->done.store(false);
            job->percent.store(1);
            job->startedTick = GetTickCount64();
            job->lastUiTick = job->startedTick;
            {
                std::lock_guard<std::mutex> lock(job->mu);
                job->title = title;
                job->phase = L"starting";
                job->output = L"";
                job->finalStatus = L"";
            }
            win.updateJob = job;
            win.noteText = L"WU_BACKGROUND_JOB";
            win.scroll = 0;
            win.selected = -1;
            SyncBackgroundJobs();

            std::thread([this, job, command, timeoutMs]() {
                std::wstring result = RunCommandCaptureStreaming(command, timeoutMs, job);
                bool failed = (result.find(L"failed") != std::wstring::npos || result.find(L"timed out") != std::wstring::npos || result.find(L"Exception") != std::wstring::npos);
                {
                    std::lock_guard<std::mutex> lock(job->mu);
                    job->output = result;
                    job->phase = failed ? L"completed with errors" : L"completed";
                    job->finalStatus = failed ? L"Windows Update action completed with errors. Review output above." : L"Windows Update action completed.";
                }
                job->percent.store(failed ? std::max(90, job->percent.load()) : 100);
                job->running.store(false);
                job->done.store(true);
            }).detach();
        }

        void StartWindowsUpdateScan(BackstageWindow& win) {
            const std::wstring ps = LR"(powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Continue'; Write-Output 'HI5_PROGRESS:5:initialising update session'; try { $session=New-Object -ComObject Microsoft.Update.Session; $session.ClientApplicationID='Hi5Central Backstage'; $searcher=$session.CreateUpdateSearcher(); Write-Output 'HI5_PROGRESS:35:searching Microsoft/WSUS update sources'; $result=$searcher.Search('IsInstalled=0 and IsHidden=0'); Write-Output 'HI5_PROGRESS:80:reading available update metadata'; Write-Output ('Found: ' + $result.Updates.Count); for($i=0;$i -lt $result.Updates.Count;$i++){ $u=$result.Updates.Item($i); $kb=''; try { $kb=($u.KBArticleIDs -join ',') } catch {}; $size=0; try { $size=[math]::Round(($u.MaxDownloadSize/1MB),1) } catch {}; $reboot=''; try { if($u.RebootRequired){ $reboot=' [reboot]' } } catch {}; Write-Output ('[ ] ' + $i + '  ' + $u.Title + '  [KB ' + $kb + '] [' + $size + ' MB]' + $reboot) }; Write-Output 'HI5_PROGRESS:100:scan completed' } catch { Write-Output ('Windows Update scan failed: ' + $_.Exception.Message); Write-Output 'HI5_PROGRESS:100:scan failed' }")";
            StartWindowsUpdateBackgroundJob(win, L"Available updates", ps, 300000);
        }

        std::vector<int> SelectedUpdateIndices(const BackstageWindow& win) const {
            std::vector<int> out;
            for (const auto& line : win.lines) {
                if (line.rfind(L"[x] ", 0) != 0) continue;
                size_t p = 4;
                while (p < line.size() && line[p] == L' ') ++p;
                int val = 0;
                bool any = false;
                while (p < line.size() && iswdigit(line[p])) { any = true; val = val * 10 + (line[p] - L'0'); ++p; }
                if (any) out.push_back(val);
            }
            return out;
        }

        std::wstring PowerShellIntArray(const std::vector<int>& values) const {
            if (values.empty()) return L"@()";
            std::wstring out = L"@(";
            for (size_t i = 0; i < values.size(); ++i) {
                if (i) out += L",";
                out += std::to_wstring(values[i]);
            }
            out += L")";
            return out;
        }

        bool HasUpdateRows(const BackstageWindow& win) const {
            for (const auto& line : win.lines) if (LineHasUpdateRow(line)) return true;
            return false;
        }

        void RefreshWindowsUpdateView(BackstageWindow& win) {
            if (win.updateJob && !win.updateJob->done.load()) {
                SyncBackgroundJobs();
                return;
            }
            if (HasUpdateRows(win) || win.noteText == L"WU_SCAN_RESULTS" || win.noteText == L"WU_INSTALL_RESULTS" || win.noteText == L"WU_BACKGROUND_JOB") {
                StartWindowsUpdateScan(win);
                return;
            }
            LoadUpdates(win);
        }

        void StartWindowsUpdateInstall(BackstageWindow& win) {
            const std::vector<int> selected = SelectedUpdateIndices(win);
            const std::wstring selectedArray = PowerShellIntArray(selected);
            const std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Continue'; try { Write-Output 'HI5_PROGRESS:5:initialising update session'; $selected=" + selectedArray + L"; $session=New-Object -ComObject Microsoft.Update.Session; $session.ClientApplicationID='Hi5Central Backstage'; $searcher=$session.CreateUpdateSearcher(); Write-Output 'HI5_PROGRESS:20:searching current update set'; $updates=$searcher.Search('IsInstalled=0 and IsHidden=0').Updates; if($updates.Count -eq 0){ Write-Output 'No applicable updates found.'; Write-Output 'HI5_PROGRESS:100:nothing to install'; exit 0 }; $coll=New-Object -ComObject Microsoft.Update.UpdateColl; for($i=0;$i -lt $updates.Count;$i++){ if($selected.Count -gt 0 -and -not ($selected -contains $i)){ continue }; $u=$updates.Item($i); if(-not $u.EulaAccepted){ $u.AcceptEula() }; [void]$coll.Add($u); Write-Output ('Queued: ' + $u.Title) }; if($coll.Count -eq 0){ Write-Output 'No selected updates were queued.'; Write-Output 'HI5_PROGRESS:100:nothing queued'; exit 0 }; Write-Output ('Queued update count: ' + $coll.Count); Write-Output 'HI5_PROGRESS:35:downloading selected updates'; $downloader=$session.CreateUpdateDownloader(); $downloader.Updates=$coll; $downloadResult=$downloader.Download(); Write-Output ('DownloadResultCode: ' + $downloadResult.ResultCode); Write-Output 'HI5_PROGRESS:70:installing selected updates'; $installer=$session.CreateUpdateInstaller(); $installer.Updates=$coll; $installer.ForceQuiet=$true; $installer.AllowSourcePrompts=$false; $result=$installer.Install(); Write-Output 'HI5_PROGRESS:95:reading install result'; Write-Output ('InstallResultCode: ' + $result.ResultCode); Write-Output ('RebootRequired: ' + $result.RebootRequired); for($i=0;$i -lt $coll.Count;$i++){ try { $ur=$result.GetUpdateResult($i); $title=$coll.Item($i).Title; Write-Output ('InstalledAttempt['+$i+']: ' + $title); Write-Output ('UpdateResult['+$i+']: ' + $ur.ResultCode + ' HResult=' + $ur.HResult) } catch {} }; Write-Output 'Recent Windows Update Agent history:'; try { $hist=$searcher.QueryHistory(0,[Math]::Min($searcher.GetTotalHistoryCount(),20)); for($j=0;$j -lt $hist.Count;$j++){ $h=$hist.Item($j); Write-Output (('[{0:yyyy-MM-dd HH:mm:ss}] Result={1} Operation={2} - {3}' -f $h.Date,$h.ResultCode,$h.Operation,$h.Title)) } } catch { Write-Output ('History query failed: ' + $_.Exception.Message) }; Write-Output 'Backstage suppresses automatic reboot; reboot must be triggered separately.'; Write-Output 'HI5_PROGRESS:100:install completed' } catch { Write-Output ('Windows Update install failed: ' + $_.Exception.Message); Write-Output 'HI5_PROGRESS:100:install failed' }\"";
            StartWindowsUpdateBackgroundJob(win,
                selected.empty() ? L"Install result - all applicable updates" : L"Install result - selected updates only",
                ps,
                1800000);
        }


        void LoadWindowsUpdateHistory(BackstageWindow& win) {
            if (win.updateJob && !win.updateJob->done.load()) {
                SyncBackgroundJobs();
                return;
            }
            win.title = L"Windows Update";
            win.noteText = L"WU_HISTORY";
            win.lines = {
                L"Windows Update history",
                L"This view combines Windows Update Agent history, installed hotfixes, and recent driver package install events.",
                L"It can show more detail than the Settings app, especially before a reboot or for driver updates.",
                L""
            };

            const std::wstring ps = LR"(powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Continue'; function ResultName($v){ switch([int]$v){ 0 {'NotStarted'} 1 {'InProgress'} 2 {'Succeeded'} 3 {'SucceededWithErrors'} 4 {'Failed'} 5 {'Aborted'} default { 'ResultCode=' + $v } } }; function OperationName($v){ switch([int]$v){ 1 {'Installation'} 2 {'Uninstallation'} default { 'Operation=' + $v } } }; Write-Output '=== Windows Update Agent history - newest first ==='; try { $session=New-Object -ComObject Microsoft.Update.Session; $session.ClientApplicationID='Hi5Central Backstage'; $searcher=$session.CreateUpdateSearcher(); $total=$searcher.GetTotalHistoryCount(); Write-Output ('Total history entries: ' + $total); $take=[Math]::Min([int]$total, 80); if($take -gt 0){ $hist=$searcher.QueryHistory(0,$take); for($i=0;$i -lt $hist.Count;$i++){ $h=$hist.Item($i); $title=''; $date=''; $op=''; $res=''; $hr=''; $client=''; $uid=''; try { $title=[string]$h.Title } catch {}; try { $date=([datetime]$h.Date).ToString('yyyy-MM-dd HH:mm:ss') } catch {}; try { $op=OperationName $h.Operation } catch {}; try { $res=ResultName $h.ResultCode } catch {}; try { $hr=('0x{0:X8}' -f ([uint32]$h.HResult)) } catch {}; try { $client=[string]$h.ClientApplicationID } catch {}; try { $uid=[string]$h.UpdateIdentity.UpdateID } catch {}; Write-Output ('['+$date+'] '+$res+' '+$op+' - '+$title); if($client){ Write-Output ('    Client: '+$client) }; if($uid){ Write-Output ('    UpdateID: '+$uid+'  HResult: '+$hr) } } } else { Write-Output 'No Windows Update Agent history entries found.' } } catch { Write-Output ('Windows Update Agent history failed: ' + $_.Exception.Message) }; Write-Output ''; Write-Output '=== Installed hotfixes / CBS QFE entries ==='; try { Get-HotFix | Sort-Object InstalledOn -Descending | Select-Object -First 40 HotFixID,InstalledOn,Description,InstalledBy | ForEach-Object { $d=''; try { if($_.InstalledOn){ $d=([datetime]$_.InstalledOn).ToString('yyyy-MM-dd') } } catch {}; Write-Output (($d.PadRight(12)) + ' ' + ([string]$_.HotFixID).PadRight(14) + ' ' + ([string]$_.Description) + ' ' + ([string]$_.InstalledBy)) } } catch { Write-Output ('Get-HotFix failed: ' + $_.Exception.Message) }; Write-Output ''; Write-Output '=== Recent driver package install events ==='; try { $events=Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-DriverFrameworks-UserMode/Operational'; StartTime=(Get-Date).AddDays(-30)} -MaxEvents 30 -ErrorAction SilentlyContinue; if($events){ $events | Sort-Object TimeCreated -Descending | ForEach-Object { Write-Output ('['+$_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss')+'] Event '+$_.Id+' - '+($_.ProviderName)); $m=($_.Message -replace '\r?\n',' '); if($m.Length -gt 240){ $m=$m.Substring(0,240)+'...' }; Write-Output ('    '+$m) } } else { Write-Output 'No recent DriverFrameworks install events found.' } } catch { Write-Output ('Driver event history failed: ' + $_.Exception.Message) }; Write-Output ''; Write-Output 'Reboot required registry signal:'; if(Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'){ Write-Output 'YES - updates may not appear finalised in Settings until reboot.' } else { Write-Output 'not detected' }" )";
            std::wstring output = RunCommandCapture(ps, 60000);
            std::wistringstream iss(output);
            std::wstring line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                win.lines.push_back(line);
            }
            win.scroll = 0;
            win.selected = -1;
        }

        RECT ActionButtonRect(const RECT& c, int index) const {
            return RECT{ c.left + 12 + index * 92, c.top + 8, c.left + 94 + index * 92, c.top + 36 };
        }

        bool HandleActionButtons(BackstageWindow& win, int x, int y) {
            RECT c = ContentRect(win);
            POINT pt{ x, y };
            if (win.kind == WindowKind::Services) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { ServiceControlSelected(win, SERVICE_CONTROL_CONTINUE, L"start"); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { ServiceControlSelected(win, SERVICE_CONTROL_STOP, L"stop"); return true; }
                if (PtInRect(&ActionButtonRect(c, 2), pt)) { ServiceControlSelected(win, SERVICE_CONTROL_STOP, L"restart"); return true; }
                if (PtInRect(&ActionButtonRect(c, 3), pt)) { LoadServices(win); return true; }
            }
            else if (win.kind == WindowKind::Processes) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { KillSelectedProcess(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { LoadProcesses(win); return true; }
            }
            else if (win.kind == WindowKind::Files) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { RunSelectedFile(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { DeleteSelectedFile(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 2), pt)) { CreateNewFolder(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 3), pt)) { LoadFiles(win); return true; }
            }
            else if (win.kind == WindowKind::Apps) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { UninstallSelectedApp(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { LoadApps(win); return true; }
            }
            else if (win.kind == WindowKind::Updates) {
                if (win.updateJob && !win.updateJob->done.load()) {
                    if (PtInRect(&ActionButtonRect(c, 2), pt)) { SyncBackgroundJobs(); return true; }
                    return true;
                }
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { StartWindowsUpdateScan(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { StartWindowsUpdateInstall(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 2), pt)) { RefreshWindowsUpdateView(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 3), pt)) { LoadWindowsUpdateHistory(win); return true; }
            }
            else if (win.kind == WindowKind::Events) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { win.noteText = L"EVENTS_ROOT"; LoadEvents(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { LoadEvents(win); return true; }
            }
            else if (win.kind == WindowKind::Registry) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { win.noteText = L"REG_ROOT"; LoadRegistry(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 1), pt)) { RegistryCreateKey(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 2), pt)) { RegistryDeleteSelectedKey(win); return true; }
                if (PtInRect(&ActionButtonRect(c, 3), pt)) { LoadRegistry(win); return true; }
            }
            else if (win.kind == WindowKind::Sessions) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { LoadSessions(win); return true; }
            }
            else if (win.kind == WindowKind::Devices) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { LoadDevices(win); return true; }
            }
            else if (win.kind == WindowKind::Disks) {
                if (PtInRect(&ActionButtonRect(c, 0), pt)) { LoadDisks(win); return true; }
            }
            return false;
        }


        std::wstring RegistryParentPath(const std::wstring& path) {
            if (path.empty() || path == L"REG_ROOT") return L"REG_ROOT";
            const size_t pos = path.find_last_of(L'\\');
            if (pos == std::wstring::npos) return L"REG_ROOT";
            return path.substr(0, pos);
        }

        bool OpenRegistryPath(const std::wstring& fullPath, REGSAM access, HKEY* outKey) {
            if (!outKey) return false;
            *outKey = nullptr;
            HKEY root = nullptr;
            std::wstring sub;
            if (fullPath.rfind(L"HKLM", 0) == 0) { root = HKEY_LOCAL_MACHINE; sub = fullPath.size() > 5 ? fullPath.substr(5) : L""; }
            else if (fullPath.rfind(L"HKCU", 0) == 0) { root = HKEY_CURRENT_USER; sub = fullPath.size() > 5 ? fullPath.substr(5) : L""; }
            else if (fullPath.rfind(L"HKCR", 0) == 0) { root = HKEY_CLASSES_ROOT; sub = fullPath.size() > 5 ? fullPath.substr(5) : L""; }
            else if (fullPath.rfind(L"HKU", 0) == 0) { root = HKEY_USERS; sub = fullPath.size() > 4 ? fullPath.substr(4) : L""; }
            else if (fullPath.rfind(L"HKCC", 0) == 0) { root = HKEY_CURRENT_CONFIG; sub = fullPath.size() > 5 ? fullPath.substr(5) : L""; }
            else return false;
            return RegOpenKeyExW(root, sub.empty() ? nullptr : sub.c_str(), 0, access, outKey) == ERROR_SUCCESS;
        }

        void HandleRegistryRowClick(BackstageWindow& win, int idx) {
            if (idx < 0 || idx >= static_cast<int>(win.lines.size())) return;
            const std::wstring row = win.lines[static_cast<size_t>(idx)];
            if (row == L"..") { win.noteText = RegistryParentPath(win.noteText); LoadRegistry(win); return; }
            if (row.rfind(L"[Hive] ", 0) == 0) { win.noteText = row.substr(7); LoadRegistry(win); return; }
            if (row.rfind(L"[Key] ", 0) == 0) { const std::wstring child = row.substr(6); win.noteText = (!win.noteText.empty() && win.noteText != L"REG_ROOT") ? (win.noteText + L"\\" + child) : child; LoadRegistry(win); }
        }

        void HandleEventRowClick(BackstageWindow& win, int idx) {
            if (idx < 0 || idx >= static_cast<int>(win.lines.size())) return;
            const std::wstring row = win.lines[static_cast<size_t>(idx)];
            if (row == L"..") { win.noteText = L"EVENTS_ROOT"; LoadEvents(win); return; }
            if (row.rfind(L"[Log] ", 0) == 0) { win.noteText = L"LOG:" + row.substr(6); LoadEvents(win); }
        }

        void RegistryCreateKey(BackstageWindow& win) {
            if (win.noteText.empty() || win.noteText == L"REG_ROOT") return;
            HKEY key = nullptr;
            if (!OpenRegistryPath(win.noteText, KEY_CREATE_SUB_KEY, &key) || !key) { win.lines.insert(win.lines.begin(), L"Unable to create key here. Access denied or invalid hive."); return; }
            for (int i = 1; i < 100; ++i) {
                std::wstring name = (i == 1) ? L"New Key" : (L"New Key (" + std::to_wstring(i) + L")");
                HKEY created = nullptr; DWORD disp = 0;
                LONG rc = RegCreateKeyExW(key, name.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &created, &disp);
                if (rc == ERROR_SUCCESS && disp == REG_CREATED_NEW_KEY) { if (created) RegCloseKey(created); break; }
                if (created) RegCloseKey(created);
            }
            RegCloseKey(key); LoadRegistry(win);
        }

        void RegistryDeleteSelectedKey(BackstageWindow& win) {
            if (win.selected < 0 || win.selected >= static_cast<int>(win.lines.size())) return;
            const std::wstring row = win.lines[static_cast<size_t>(win.selected)];
            if (row.rfind(L"[Key] ", 0) != 0 || win.noteText.empty() || win.noteText == L"REG_ROOT") return;
            HKEY key = nullptr;
            if (!OpenRegistryPath(win.noteText, KEY_WRITE, &key) || !key) { win.lines.insert(win.lines.begin(), L"Unable to open parent key for delete."); return; }
            const std::wstring child = row.substr(6);
            LONG rc = RegDeleteTreeW(key, child.c_str());
            RegCloseKey(key); LoadRegistry(win);
            win.lines.insert(win.lines.begin(), rc == ERROR_SUCCESS ? (L"Deleted key: " + child) : (L"Delete failed: " + child + L" error=" + std::to_wstring(rc)));
        }

        void ServiceControlSelected(BackstageWindow& win, DWORD control, const std::wstring& action) {
            if (win.selected < 0 || win.selected >= static_cast<int>(win.services.size())) return;
            const std::wstring svcName = win.services[static_cast<size_t>(win.selected)].name;
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm) return;
            SC_HANDLE svc = OpenServiceW(scm, svcName.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS status{};
                if (action == L"start") StartServiceW(svc, 0, nullptr);
                else if (action == L"restart") { ControlService(svc, SERVICE_CONTROL_STOP, &status); Sleep(500); StartServiceW(svc, 0, nullptr); }
                else ControlService(svc, control, &status);
                CloseServiceHandle(svc);
            }
            CloseServiceHandle(scm);
            LoadServices(win);
        }

        void KillSelectedProcess(BackstageWindow& win) {
            if (win.selected < 0 || win.selected >= static_cast<int>(win.lines.size())) return;
            std::wistringstream iss(win.lines[static_cast<size_t>(win.selected)]);
            DWORD pid = 0; iss >> pid;
            if (pid <= 4) return;
            HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (p) { TerminateProcess(p, 1); CloseHandle(p); }
            LoadProcesses(win);
        }

        void RunSelectedFile(BackstageWindow& win) {
            if (win.selected < 0 || win.selected >= static_cast<int>(win.files.size())) return;
            const FileRow& f = win.files[static_cast<size_t>(win.selected)];
            if (f.isDir) return;
            ShellExecuteW(nullptr, L"open", f.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        void DeleteSelectedFile(BackstageWindow& win) {
            if (win.selected < 0 || win.selected >= static_cast<int>(win.files.size())) return;
            const FileRow& f = win.files[static_cast<size_t>(win.selected)];
            std::error_code ec;
            if (f.isDir) std::filesystem::remove_all(f.path, ec);
            else std::filesystem::remove(f.path, ec);
            LoadFiles(win);
        }

        void CreateNewFolder(BackstageWindow& win) {
            namespace fs = std::filesystem;
            fs::path base(win.path.empty() ? L"C:\\" : win.path);
            for (int i = 1; i < 100; ++i) {
                fs::path candidate = base / (i == 1 ? L"New Folder" : (L"New Folder (" + std::to_wstring(i) + L")"));
                std::error_code ec;
                if (!fs::exists(candidate, ec)) {
                    fs::create_directory(candidate, ec);
                    break;
                }
            }
            LoadFiles(win);
        }

        void UninstallSelectedApp(BackstageWindow& win) {
            if (win.selected < 0 || win.selected >= static_cast<int>(win.apps.size())) return;
            const AppRow& app = win.apps[static_cast<size_t>(win.selected)];
            bool confident = false;
            std::wstring cmd = BuildSilentUninstallCommand(app, &confident);
            if (cmd.empty() || !confident) {
                win.lines.insert(win.lines.begin(), L"Silent uninstall unavailable for: " + app.name);
                win.lines.insert(win.lines.begin() + 1, L"Backstage only runs known silent uninstall commands to prevent UI appearing on the user's desktop.");
                win.lines.insert(win.lines.begin() + 2, L"Supported: QuietUninstallString, MSI product-code uninstall, and common Inno/NSIS silent uninstallers.");
                return;
            }
            win.lines.insert(win.lines.begin(), L"Silent uninstall running: " + app.name);
            win.lines.insert(win.lines.begin() + 1, L"Command: " + cmd);
            std::wstring output = RunCommandCapture(cmd, 300000);
            win.lines.insert(win.lines.begin(), L"Silent uninstall finished/request completed: " + app.name);
            if (!output.empty()) PushCommandOutputLines(win, L"Uninstall output", output);
        }


        void OnMouseWheel(int deltaY) {
            BackstageWindow* target = nullptr;
            POINT pt{ mouseX_, mouseY_ };
            std::vector<BackstageWindow*> hitOrder;
            for (auto& win : windows_) {
                if (!win.closed && !win.minimized) hitOrder.push_back(&win);
            }
            std::sort(hitOrder.begin(), hitOrder.end(), [](const BackstageWindow* a, const BackstageWindow* b) { return a->zOrder > b->zOrder; });
            for (BackstageWindow* candidate : hitOrder) {
                if (candidate && PtInRect(&candidate->rect, pt)) { target = candidate; break; }
            }
            if (!target) target = ActiveWindow();
            if (!target || target->minimized) return;

            const int lines = (deltaY > 0) ? -3 : 3;
            target->scroll = std::max(0, target->scroll + lines);
            ClampScroll(*target);
        }

        void OnKey(const hi5::InputCmd& cmd) {
            const WORD vk = cmd.key.vk;
            if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
                shiftDown_ = cmd.key.down != 0;
                return;
            }
            if (!cmd.key.down) return;

            if (startMenuOpen_ && focusStartSearch_) {
                if (vk == VK_BACK) {
                    if (!startSearch_.empty()) startSearch_.pop_back();
                    return;
                }
                if (vk == VK_ESCAPE) {
                    startMenuOpen_ = false;
                    return;
                }
                if (vk == VK_RETURN) {
                    auto matches = MatchingSearchResults();
                    if (!matches.empty()) {
                        LaunchTool(Tools()[matches[0].index]);
                        startMenuOpen_ = false;
                    }
                    return;
                }
                const wchar_t ch = VkToTextChar(vk);
                if (ch) startSearch_.push_back(ch);
                return;
            }

            BackstageWindow* win = ActiveWindow();
            if (!win || win->minimized) return;
            if (win->kind == WindowKind::Terminal) {
                if (vk == VK_BACK) {
                    if (!win->terminalInput.empty()) win->terminalInput.pop_back();
                    return;
                }
                if (vk == VK_RETURN) {
                    SendTerminalLine(*win);
                    return;
                }
                if (vk == VK_ESCAPE) {
                    win->terminalInput.clear();
                    return;
                }
                const wchar_t ch = VkToTextChar(vk);
                if (ch) win->terminalInput.push_back(ch);
            }
            else if (win->kind == WindowKind::Notepad) {
                if (vk == VK_BACK) {
                    if (!win->noteText.empty()) win->noteText.pop_back();
                    return;
                }
                if (vk == VK_RETURN) {
                    win->noteText.push_back(L'\n');
                    return;
                }
                if (vk == VK_TAB) {
                    win->noteText += L"    ";
                    return;
                }
                const wchar_t ch = VkToTextChar(vk);
                if (ch) win->noteText.push_back(ch);
            }
        }

        void OnTextInput(const std::wstring& text) {
            if (text.empty()) return;

            if (startMenuOpen_ && focusStartSearch_) {
                for (wchar_t ch : text) {
                    if (ch == L'\r') continue;
                    if (ch == L'\n') {
                        auto matches = MatchingSearchResults();
                        if (!matches.empty()) {
                            LaunchTool(Tools()[matches[0].index]);
                            startMenuOpen_ = false;
                        }
                        continue;
                    }
                    if (ch >= 32) startSearch_.push_back(ch);
                }
                return;
            }

            BackstageWindow* win = ActiveWindow();
            if (!win || win->minimized) return;

            if (win->kind == WindowKind::Terminal) {
                for (wchar_t ch : text) {
                    if (ch == L'\r') continue;
                    if (ch == L'\n') { SendTerminalLine(*win); continue; }
                    if (ch >= 32) win->terminalInput.push_back(ch);
                }
                return;
            }

            if (win->kind == WindowKind::Notepad) {
                for (wchar_t ch : text) {
                    if (ch == L'\r') continue;
                    if (ch == L'\n' || ch >= 32) win->noteText.push_back(ch);
                }
            }
        }

        void HandleShortcut(const hi5::InputCmd& cmd) {
            const auto action = static_cast<hi5::ShortcutAction>(cmd.shortcut.action);
            switch (action) {
            case hi5::ShortcutAction::StartMenu:
                startMenuOpen_ = !startMenuOpen_;
                focusStartSearch_ = startMenuOpen_;
                if (startMenuOpen_) startSearch_.clear();
                break;
            case hi5::ShortcutAction::CtrlAltDel:
                // Backstage is a private synthetic workspace, so SAS cannot be shown here.
                // Keep this visible rather than silently doing nothing.
                startMenuOpen_ = false;
                if (BackstageWindow* win = ActiveWindow()) {
                    win->lines.insert(win->lines.begin(), L"Ctrl+Alt+Del is only available in console mode.");
                }
                break;
            case hi5::ShortcutAction::TaskManager:
                LaunchTool(Tools()[2]);
                break;
            case hi5::ShortcutAction::Explorer:
            case hi5::ShortcutAction::WinE:
                LaunchTool(Tools()[0]);
                break;
            case hi5::ShortcutAction::WinR:
                LaunchTool(Tools()[3]);
                if (BackstageWindow* win = ActiveWindow()) {
                    win->lines.insert(win->lines.begin(), L"Run shortcut received. Use Terminal for command execution in Backstage mode.");
                }
                break;
            case hi5::ShortcutAction::WinD: {
                bool anyVisible = false;
                for (const auto& w : windows_) { if (!w.closed && !w.minimized) { anyVisible = true; break; } }
                for (auto& w : windows_) { if (!w.closed) w.minimized = anyVisible; }
                startMenuOpen_ = false;
                break;
            }
            case hi5::ShortcutAction::AltTab:
            case hi5::ShortcutAction::AltTabBegin:
            case hi5::ShortcutAction::AltTabNext:
            case hi5::ShortcutAction::WinTab: {
                std::vector<int> ids;
                for (const auto& w : windows_) { if (!w.closed && !w.minimized) ids.push_back(w.id); }
                if (!ids.empty()) {
                    auto it = std::find(ids.begin(), ids.end(), activeWindowId_);
                    if (it == ids.end() || ++it == ids.end()) activeWindowId_ = ids.front();
                    else activeWindowId_ = *it;
                    BringToFront(activeWindowId_);
                }
                startMenuOpen_ = false;
                break;
            }
            case hi5::ShortcutAction::AltTabEnd:
                break;
            case hi5::ShortcutAction::AltF4:
                if (BackstageWindow* win = ActiveWindow()) CloseWindow(win->id);
                startMenuOpen_ = false;
                break;
            case hi5::ShortcutAction::CtrlShiftEsc:
                LaunchTool(Tools()[1]);
                startMenuOpen_ = false;
                break;
            case hi5::ShortcutAction::CtrlEsc:
                startMenuOpen_ = !startMenuOpen_;
                focusStartSearch_ = startMenuOpen_;
                if (startMenuOpen_) startSearch_.clear();
                break;
            default:
                break;
            }
        }

        wchar_t VkToTextChar(WORD vk) const {
            const bool shift = shiftDown_;
            if (vk >= 'A' && vk <= 'Z') return static_cast<wchar_t>(shift ? vk : (vk - 'A' + L'a'));
            if (vk >= '0' && vk <= '9') {
                static const wchar_t shifted[] = L")!@#$%^&*(";
                return shift ? shifted[vk - '0'] : static_cast<wchar_t>(vk);
            }
            switch (vk) {
            case VK_SPACE: return L' ';
            case VK_OEM_PERIOD: return shift ? L'>' : L'.';
            case VK_OEM_COMMA: return shift ? L'<' : L',';
            case VK_OEM_MINUS: return shift ? L'_' : L'-';
            case VK_OEM_PLUS: return shift ? L'+' : L'=';
            case VK_OEM_1: return shift ? L':' : L';';
            case VK_OEM_2: return shift ? L'?' : L'/';
            case VK_OEM_3: return shift ? L'~' : L'`';
            case VK_OEM_4: return shift ? L'{' : L'[';
            case VK_OEM_5: return shift ? L'|' : L'\\';
            case VK_OEM_6: return shift ? L'}' : L']';
            case VK_OEM_7: return shift ? L'"' : L'\'';
            default: return 0;
            }
        }

        bool LaunchTool(const ToolSpec& spec) {
            BackstageWindow* existing = FindOpenTool(spec.id ? spec.id : L"");
            if (existing) {
                BringToFront(existing->id);
                return true;
            }

            size_t openOrdinal = 0;
            for (const auto& item : windows_) if (!item.closed) ++openOrdinal;

            BackstageWindow& win = windows_.emplace_back();
            win.id = nextWindowId_++;
            win.kind = spec.kind;
            win.toolId = spec.id ? spec.id : L"";
            win.title = spec.title ? spec.title : L"Tool";
            win.subtitle = spec.sub ? spec.sub : L"";
            win.terminalMode = spec.terminalMode ? spec.terminalMode : L"";
            win.rect = DefaultManagedWindowRect(openOrdinal);
            win.zOrder = ++zCounter_;
            win.restoreRect = win.rect;

            if (win.kind == WindowKind::Services) { win.scroll = 0; LoadServices(win); }
            else if (win.kind == WindowKind::Files) { win.scroll = 0; LoadFiles(win); }
            else if (win.kind == WindowKind::Processes) LoadProcesses(win);
            else if (win.kind == WindowKind::Apps) LoadApps(win);
            else if (win.kind == WindowKind::SystemInfo) LoadSystemInfo(win);
            else if (win.kind == WindowKind::Sessions) LoadSessions(win);
            else if (win.kind == WindowKind::Events) { win.noteText = L"EVENTS_ROOT"; LoadEvents(win); }
            else if (win.kind == WindowKind::Updates) LoadUpdates(win);
            else if (win.kind == WindowKind::Registry) { win.noteText = L"REG_ROOT"; LoadRegistry(win); }
            else if (win.kind == WindowKind::Devices) LoadDevices(win);
            else if (win.kind == WindowKind::Disks) LoadDisks(win);
            else if (win.kind == WindowKind::Terminal) StartTerminal(win);
            else if (win.kind == WindowKind::Notepad) win.noteText = L"";

            activeWindowId_ = win.id;
            LogInfo("[backstage] PASS20L open window title=" + WideToUtf8(win.title));
            return true;
        }

        BackstageWindow* FindOpenTool(const std::wstring& id) {
            for (auto& w : windows_) {
                if (!w.closed && w.toolId == id) return &w;
            }
            return nullptr;
        }


        std::wstring QueryRegString(HKEY key, const wchar_t* name) {
            DWORD type = 0;
            DWORD bytes = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return L"";
            if (type != REG_SZ && type != REG_EXPAND_SZ) return L"";
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) return L"";
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            return value;
        }

        std::wstring RunCommandCapture(const std::wstring& command, DWORD timeoutMs = 15000) {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE outRead = nullptr, outWrite = nullptr;
            if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return L"CreatePipe failed: " + std::to_wstring(GetLastError());
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = outWrite;
            si.hStdError = outWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION pi{};
            std::wstring mutableCmd = L"cmd.exe /c " + command;
            BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(outWrite);
            if (!ok) {
                DWORD err = GetLastError();
                CloseHandle(outRead);
                return L"CreateProcess failed: " + std::to_wstring(err);
            }

            std::string bytes;
            char buf[4096];
            const auto start = GetTickCount64();
            for (;;) {
                DWORD avail = 0;
                if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    DWORD got = 0;
                    if (ReadFile(outRead, buf, std::min<DWORD>(static_cast<DWORD>(sizeof(buf)), avail), &got, nullptr) && got) bytes.append(buf, buf + got);
                }
                DWORD wait = WaitForSingleObject(pi.hProcess, 25);
                if (wait == WAIT_OBJECT_0) {
                    while (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                        DWORD got = 0;
                        if (!ReadFile(outRead, buf, std::min<DWORD>(static_cast<DWORD>(sizeof(buf)), avail), &got, nullptr) || !got) break;
                        bytes.append(buf, buf + got);
                    }
                    break;
                }
                if (GetTickCount64() - start > timeoutMs) {
                    TerminateProcess(pi.hProcess, 124);
                    bytes += "\r\n[command timed out]";
                    break;
                }
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(outRead);
            return BytesToWideOem(bytes);
        }

        void PushCommandOutputLines(BackstageWindow& win, const std::wstring& title, const std::wstring& output) {
            win.lines.clear();
            win.lines.push_back(title);
            win.lines.push_back(L"────────────────────────────────────────────────────────────");
            std::wistringstream iss(output);
            std::wstring line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                win.lines.push_back(line);
                if (win.lines.size() > 500) break;
            }
            win.scroll = 0;
        }

        void LoadServices(BackstageWindow& win) {
            win.services.clear();
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
            if (!scm) {
                win.lines = { L"Failed to open Service Control Manager: " + std::to_wstring(GetLastError()) };
                return;
            }
            DWORD bytesNeeded = 0, count = 0, resume = 0;
            EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &bytesNeeded, &count, &resume, nullptr);
            std::vector<BYTE> buffer(bytesNeeded + 4096);
            if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesNeeded, &count, &resume, nullptr)) {
                auto* rows = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
                for (DWORD i = 0; i < count; ++i) {
                    ServiceRow r;
                    r.name = rows[i].lpServiceName ? rows[i].lpServiceName : L"";
                    r.display = rows[i].lpDisplayName ? rows[i].lpDisplayName : r.name;
                    r.state = rows[i].ServiceStatusProcess.dwCurrentState;
                    r.stateText = ServiceStateText(r.state);
                    SC_HANDLE svc = OpenServiceW(scm, r.name.c_str(), SERVICE_QUERY_CONFIG);
                    if (svc) {
                        DWORD needed = 0;
                        QueryServiceConfigW(svc, nullptr, 0, &needed);
                        if (needed) {
                            std::vector<BYTE> cfgBuf(needed);
                            auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
                            if (QueryServiceConfigW(svc, cfg, needed, &needed)) {
                                r.startType = cfg->dwStartType;
                                r.startTypeText = ServiceStartTypeText(cfg->dwStartType);
                            }
                        }
                        CloseServiceHandle(svc);
                    }
                    if (r.startTypeText.empty()) r.startTypeText = L"Unknown";
                    win.services.push_back(std::move(r));
                }
                std::sort(win.services.begin(), win.services.end(), [](const ServiceRow& a, const ServiceRow& b) { return a.display < b.display; });
            }
            CloseServiceHandle(scm);
            win.selected = -1;
        }

        std::wstring ServiceStateText(DWORD state) const {
            switch (state) {
            case SERVICE_STOPPED: return L"Stopped";
            case SERVICE_START_PENDING: return L"Starting";
            case SERVICE_STOP_PENDING: return L"Stopping";
            case SERVICE_RUNNING: return L"Running";
            case SERVICE_CONTINUE_PENDING: return L"Continuing";
            case SERVICE_PAUSE_PENDING: return L"Pausing";
            case SERVICE_PAUSED: return L"Paused";
            default: return L"Unknown";
            }
        }

        std::wstring ServiceStartTypeText(DWORD type) const {
            switch (type) {
            case SERVICE_AUTO_START: return L"Automatic";
            case SERVICE_BOOT_START: return L"Boot";
            case SERVICE_SYSTEM_START: return L"System";
            case SERVICE_DEMAND_START: return L"Manual";
            case SERVICE_DISABLED: return L"Disabled";
            default: return L"Unknown";
            }
        }

        void LoadFiles(BackstageWindow& win) {
            namespace fs = std::filesystem;
            win.files.clear();
            std::error_code ec;
            fs::path p(win.path.empty() ? L"C:\\" : win.path);
            if (p.has_parent_path()) {
                FileRow up;
                up.name = L"..";
                up.path = p.parent_path().wstring();
                if (up.path.empty()) up.path = L"C:\\";
                up.isDir = true;
                win.files.push_back(std::move(up));
            }
            for (const auto& de : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
                FileRow r;
                r.name = de.path().filename().wstring();
                r.path = de.path().wstring();
                r.isDir = de.is_directory(ec);
                if (!r.isDir) r.size = de.file_size(ec);
                win.files.push_back(std::move(r));
            }
            std::sort(win.files.begin(), win.files.end(), [](const FileRow& a, const FileRow& b) {
                if (a.isDir != b.isDir) return a.isDir > b.isDir;
                return a.name < b.name;
                });
            win.selected = -1;
        }

        void LoadProcesses(BackstageWindow& win) {
            struct ProcessDisplayRow {
                DWORD pid = 0;
                DWORD threads = 0;
                SIZE_T workingSet = 0;
                std::wstring name;
            };

            std::vector<ProcessDisplayRow> rows;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) {
                win.lines = { L"Failed to snapshot processes: " + std::to_wstring(GetLastError()) };
                return;
            }

            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    ProcessDisplayRow row;
                    row.pid = pe.th32ProcessID;
                    row.threads = pe.cntThreads;
                    row.name = pe.szExeFile;

                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, row.pid);
                    if (process) {
                        PROCESS_MEMORY_COUNTERS pmc{};
                        if (GetProcessMemoryInfo(process, &pmc, sizeof(pmc))) row.workingSet = pmc.WorkingSetSize;
                        CloseHandle(process);
                    }
                    rows.push_back(std::move(row));
                } while (Process32NextW(snap, &pe) && rows.size() < 500);
            }
            CloseHandle(snap);

            std::sort(rows.begin(), rows.end(), [](const ProcessDisplayRow& a, const ProcessDisplayRow& b) {
                const int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                if (cmp != 0) return cmp < 0;
                return a.pid < b.pid;
            });

            win.lines.clear();
            for (const auto& row : rows) {
                std::wstring line = std::to_wstring(row.pid) + L"    " + row.name +
                    L"    Threads " + std::to_wstring(row.threads);
                if (row.workingSet > 0) {
                    line += L"    " + std::to_wstring(row.workingSet / (1024ull * 1024ull)) + L" MB";
                }
                win.lines.push_back(std::move(line));
            }
            win.subtitle = L"Processes · " + std::to_wstring(rows.size()) + L" running · maintenance context SYSTEM";
            win.selected = -1;
            win.scroll = 0;
        }

        void LoadApps(BackstageWindow& win) {
            win.lines.clear();
            win.apps.clear();
            const wchar_t* roots[] = {
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
            };
            for (const wchar_t* root : roots) {
                HKEY key = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
                for (DWORD i = 0;; ++i) {
                    wchar_t sub[256]{};
                    DWORD subLen = 256;
                    if (RegEnumKeyExW(key, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    HKEY app = nullptr;
                    if (RegOpenKeyExW(key, sub, 0, KEY_READ, &app) == ERROR_SUCCESS) {
                        AppRow row;
                        row.name = QueryRegString(app, L"DisplayName");
                        if (!row.name.empty()) {
                            row.publisher = QueryRegString(app, L"Publisher");
                            row.version = QueryRegString(app, L"DisplayVersion");
                            row.uninstall = QueryRegString(app, L"UninstallString");
                            row.quietUninstall = QueryRegString(app, L"QuietUninstallString");
                            bool silentOk = false;
                            std::wstring derivedSilent = BuildSilentUninstallCommand(row, &silentOk);
                            if (row.quietUninstall.empty() && silentOk) row.quietUninstall = derivedSilent;
                            win.apps.push_back(std::move(row));
                        }
                        RegCloseKey(app);
                    }
                }
                RegCloseKey(key);
            }
            std::sort(win.apps.begin(), win.apps.end(), [](const AppRow& a, const AppRow& b) { return a.name < b.name; });
            for (const auto& a : win.apps) {
                std::wstring line = a.name;
                if (!a.version.empty()) line += L"  " + a.version;
                if (!a.publisher.empty()) line += L"  -  " + a.publisher;
                if (!a.quietUninstall.empty()) line += L"  [silent uninstall available]";
                else if (!a.uninstall.empty()) line += L"  [interactive uninstall blocked]";
                win.lines.push_back(line);
            }
            win.selected = -1;
        }

        void LoadSystemInfo(BackstageWindow& win) {
            wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
            DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
            GetComputerNameW(computer, &len);
            OSVERSIONINFOEXW os{};
            os.dwOSVersionInfoSize = sizeof(os);
#pragma warning(push)
#pragma warning(disable:4996)
            GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&os));
#pragma warning(pop)
            SYSTEM_INFO si{};
            GetNativeSystemInfo(&si);
            MEMORYSTATUSEX mem{};
            mem.dwLength = sizeof(mem);
            GlobalMemoryStatusEx(&mem);
            win.lines = {
                L"Computer name: " + std::wstring(computer),
                L"Windows version: " + std::to_wstring(os.dwMajorVersion) + L"." + std::to_wstring(os.dwMinorVersion) + L" build " + std::to_wstring(os.dwBuildNumber),
                L"Processors: " + std::to_wstring(si.dwNumberOfProcessors),
                L"Memory total: " + std::to_wstring(mem.ullTotalPhys / (1024ull * 1024ull)) + L" MB",
                L"Memory available: " + std::to_wstring(mem.ullAvailPhys / (1024ull * 1024ull)) + L" MB",
                L"Session: Backstage SYSTEM maintenance workspace"
            };
        }

        static std::wstring WtsStateText(WTS_CONNECTSTATE_CLASS state) {
            switch (state) {
            case WTSActive: return L"Active";
            case WTSConnected: return L"Connected";
            case WTSConnectQuery: return L"ConnectQuery";
            case WTSShadow: return L"Shadow";
            case WTSDisconnected: return L"Disconnected";
            case WTSIdle: return L"Idle";
            case WTSListen: return L"Listen";
            case WTSReset: return L"Reset";
            case WTSDown: return L"Down";
            case WTSInit: return L"Init";
            default: return L"Unknown";
            }
        }

        static std::wstring QueryWtsString(DWORD sessionId, WTS_INFO_CLASS infoClass) {
            LPWSTR value = nullptr;
            DWORD bytes = 0;
            std::wstring out;
            if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, infoClass,
                &value, &bytes) && value && bytes >= sizeof(wchar_t)) {
                out.assign(value);
            }
            if (value) WTSFreeMemory(value);
            return out;
        }

        void LoadSessions(BackstageWindow& win) {
            win.lines.clear();
            win.lines.push_back(L"Background security context: NT AUTHORITY\\SYSTEM");
            win.lines.push_back(L"Private desktop: " + privateDesktopFullName_);
            win.lines.push_back(L"SYSTEM is a security context, not a separate interactive logged-on user.");
            win.lines.push_back(L"");

            const DWORD consoleSession = WTSGetActiveConsoleSessionId();
            PWTS_SESSION_INFOW sessions = nullptr;
            DWORD count = 0;
            if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
                win.lines.push_back(L"Unable to enumerate Windows sessions. Error: " + std::to_wstring(GetLastError()));
                return;
            }

            size_t interactiveCount = 0;
            for (DWORD i = 0; i < count; ++i) {
                const auto& si = sessions[i];
                std::wstring user = QueryWtsString(si.SessionId, WTSUserName);
                std::wstring domain = QueryWtsString(si.SessionId, WTSDomainName);
                std::wstring station = si.pWinStationName ? si.pWinStationName : L"";
                if (!user.empty()) ++interactiveCount;

                std::wstring identity = user.empty() ? L"(no interactive user)" :
                    (domain.empty() ? user : domain + L"\\" + user);
                std::wstring line = L"Session " + std::to_wstring(si.SessionId);
                if (si.SessionId == consoleSession) line += L"  [Console]";
                line += L"  " + WtsStateText(si.State) + L"  " + identity;
                if (!station.empty()) line += L"  (" + station + L")";
                win.lines.push_back(std::move(line));
            }
            WTSFreeMemory(sessions);

            win.subtitle = L"Windows sessions · " + std::to_wstring(interactiveCount) +
                L" interactive user" + (interactiveCount == 1 ? L"" : L"s") + L" · Background runs as SYSTEM";
            win.selected = -1;
            win.scroll = 0;
        }

        void LoadEvents(BackstageWindow& win) {
            if (win.noteText.empty()) win.noteText = L"EVENTS_ROOT";
            win.lines.clear();
            if (win.noteText == L"EVENTS_ROOT") {
                win.title = L"Event Viewer";
                win.lines = { L"Event Viewer", L"Click a Windows log to view recent events.", L"[Log] Application", L"[Log] System", L"[Log] Security", L"[Log] Setup", L"[Log] Microsoft-Windows-WindowsUpdateClient/Operational" };
                win.scroll = 0; return;
            }
            std::wstring logName = L"System";
            if (win.noteText.rfind(L"LOG:", 0) == 0) logName = win.noteText.substr(4);
            win.lines.push_back(L"Event Viewer - " + logName);
            win.lines.push_back(L"..");
            win.lines.push_back(L"Recent events. Click Home to choose another section.");
            win.lines.push_back(L"");
            const std::wstring cmd = L"wevtutil qe \"" + logName + L"\" /c:45 /rd:true /f:text";
            PushCommandOutputLines(win, L"", RunCommandCapture(cmd, 15000));
            win.scroll = 0;
        }



        void LoadUpdates(BackstageWindow& win) {
            win.lines = {
                L"Windows Update",
                L"Actions: Scan / Install / Refresh / History. These use Windows built-in update client where available.",
                L"",
                L"Recent installed updates:"
            };
            std::wstring output = RunCommandCapture(LR"(powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-HotFix | Sort-Object InstalledOn -Descending | Select-Object -First 20 HotFixID,InstalledOn,Description | Format-Table -AutoSize")", 15000);
            std::wistringstream iss(output);
            std::wstring line;
            while (std::getline(iss, line)) { if (!line.empty() && line.back() == L'\r') line.pop_back(); win.lines.push_back(line); }
            HKEY rebootKey = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired", 0, KEY_READ, &rebootKey) == ERROR_SUCCESS) {
                win.lines.insert(win.lines.begin() + 2, L"Reboot required: YES");
                RegCloseKey(rebootKey);
            }
            else {
                win.lines.insert(win.lines.begin() + 2, L"Reboot required: not detected");
            }
            win.scroll = 0;
        }

        void LoadRegistry(BackstageWindow& win) {
            win.lines.clear();
            if (win.noteText.empty()) win.noteText = L"REG_ROOT";
            if (win.noteText == L"REG_ROOT") {
                win.lines = { L"Registry Editor", L"Click a hive to browse. New/Delete work on selected child keys only.", L"[Hive] HKCR", L"[Hive] HKCU", L"[Hive] HKLM", L"[Hive] HKU", L"[Hive] HKCC" };
                win.scroll = 0; return;
            }
            win.lines.push_back(L"Registry: " + win.noteText);
            win.lines.push_back(L"Click a key to drill down. Use New Key/Delete Key actions for child keys.");
            win.lines.push_back(L"..");
            HKEY key = nullptr;
            if (OpenRegistryPath(win.noteText, KEY_READ, &key) && key) {
                for (DWORD i = 0; i < 300; ++i) { wchar_t name[256]{}; DWORD len = 256; if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; win.lines.push_back(L"[Key] " + std::wstring(name)); }
                for (DWORD i = 0; i < 120; ++i) { wchar_t name[256]{}; DWORD len = 256; DWORD type = 0; if (RegEnumValueW(key, i, name, &len, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break; std::wstring typeText = (type == REG_SZ) ? L"REG_SZ" : (type == REG_DWORD ? L"REG_DWORD" : (type == REG_EXPAND_SZ ? L"REG_EXPAND_SZ" : L"VALUE")); win.lines.push_back(L"[Value] " + std::wstring(name) + L"    " + typeText); }
                RegCloseKey(key);
            }
            else { win.lines.push_back(L"Unable to open key. Error: " + std::to_wstring(GetLastError())); }
            win.scroll = 0;
        }

        void LoadDevices(BackstageWindow& win) {
            const std::wstring cmd = LR"(powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-PnpDevice | Sort-Object Class,FriendlyName | Select-Object -First 120 Class,Status,FriendlyName | Format-Table -AutoSize")";
            PushCommandOutputLines(win, L"Device Manager - PnP devices", RunCommandCapture(cmd, 15000));
        }

        void LoadDisks(BackstageWindow& win) {
            win.lines.clear();
            DWORD mask = GetLogicalDrives();
            for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
                if ((mask & (1u << (letter - L'A'))) == 0) continue;
                wchar_t root[] = { letter, L':', L'\\', 0 };
                ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFree{};
                std::wstring line = std::wstring(root);
                UINT type = GetDriveTypeW(root);
                line += L"  ";
                line += (type == DRIVE_FIXED ? L"Fixed" : type == DRIVE_REMOVABLE ? L"Removable" : type == DRIVE_REMOTE ? L"Network" : L"Drive");
                if (GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, &totalFree)) {
                    line += L"  Free: " + FileSizeText(freeBytes.QuadPart) + L" / " + FileSizeText(totalBytes.QuadPart);
                }
                win.lines.push_back(line);
            }
        }

        void LoadExperimental(BackstageWindow& win) {
            win.lines = {
                L"Experimental Native Apps",
                L"These attempt to launch real Windows tools. Rendering in Backstage may be unreliable without a virtual display backend.",
                L"",
                L"1. Services MMC  - services.msc",
                L"2. Computer Management - compmgmt.msc",
                L"3. Device Manager - devmgmt.msc",
                L"4. Event Viewer - eventvwr.msc",
                L"5. Disk Management - diskmgmt.msc",
                L"6. Registry Editor - regedit.exe",
                L"7. Programs and Features - appwiz.cpl",
                L"",
                L"For reliable maintenance, use the Backstage-native tools first."
            };
        }

        void RunDetached(const std::wstring& command) {
            std::vector<wchar_t> cmd(command.begin(), command.end());
            cmd.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }

        bool EnsurePrivateDesktop() {
            if (privateDesktop_) return true;
            privateDesktopName_ = Utf8ToWide("Hi5CentralBackground_" + sessionId_);
            privateDesktopFullName_ = L"winsta0\\" + privateDesktopName_;

            PSECURITY_DESCRIPTOR sd = nullptr;
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &sd, nullptr)) {
                LogWarn("[background-native] desktop ACL create failed err=" + std::to_string(GetLastError()));
                return false;
            }
            sa.lpSecurityDescriptor = sd;
            privateDesktop_ = CreateDesktopW(privateDesktopName_.c_str(), nullptr, nullptr, 0,
                DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE |
                DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP |
                DESKTOP_WRITEOBJECTS | GENERIC_ALL, &sa);
            LocalFree(sd);
            if (!privateDesktop_) {
                LogWarn("[background-native] private desktop create failed err=" + std::to_string(GetLastError()));
                return false;
            }
            LogInfo("[background-native] private desktop ready name=" + WideToUtf8(privateDesktopFullName_));
            return true;
        }

        struct NativeDesktopWindowInfo {
            HWND hwnd = nullptr;
            RECT windowRect{};
            RECT sourceRect{};
            std::wstring className;
            std::wstring title;
        };

        struct NativeWindowCache {
            RECT sourceRect{};
            int width = 0;
            int height = 0;
            std::vector<uint8_t> bgra;
            ULONGLONG lastCaptureTick = 0;
            bool valid = false;
        };

        struct NativeDesktopEnumCtx {
            std::vector<NativeDesktopWindowInfo>* windows = nullptr;
        };

        static BOOL CALLBACK EnumNativeDesktopCaptureProc(HWND hwnd, LPARAM lparam) {
            auto* ctx = reinterpret_cast<NativeDesktopEnumCtx*>(lparam);
            if (!ctx || !ctx->windows || !hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return TRUE;
            RECT r{};
            if (!GetWindowRect(hwnd, &r)) return TRUE;
            if ((r.right - r.left) < 32 || (r.bottom - r.top) < 24) return TRUE;
            wchar_t cls[128]{};
            wchar_t title[256]{};
            GetClassNameW(hwnd, cls, 128);
            GetWindowTextW(hwnd, title, 256);
            NativeDesktopWindowInfo info{};
            info.hwnd = hwnd;
            info.windowRect = r;
            info.sourceRect = r;

            // GetWindowRect includes invisible resize/shadow margins on modern
            // Windows. PrintWindow commonly leaves those margins black on an
            // inactive private desktop, so composite only the visible DWM frame
            // when Windows exposes one.
            RECT frameBounds{};
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                &frameBounds, sizeof(frameBounds))) &&
                frameBounds.right > frameBounds.left &&
                frameBounds.bottom > frameBounds.top) {
                info.sourceRect = frameBounds;
            }

            info.className = cls;
            info.title = title;
            ctx->windows->push_back(std::move(info));
            return TRUE;
        }

        std::vector<NativeDesktopWindowInfo> NativeDesktopWindows() const {
            std::vector<NativeDesktopWindowInfo> out;
            if (!privateDesktop_) return out;
            NativeDesktopEnumCtx ctx{ &out };
            EnumDesktopWindows(privateDesktop_, EnumNativeDesktopCaptureProc, reinterpret_cast<LPARAM>(&ctx));
            return out;
        }

        RECT NativeViewerRect(const RECT& source) const {
            const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int sw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
            const int sh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
            RECT r{};
            r.left = static_cast<LONG>((static_cast<long long>(source.left - sx) * w_) / sw);
            r.top = static_cast<LONG>((static_cast<long long>(source.top - sy) * h_) / sh);
            r.right = static_cast<LONG>((static_cast<long long>(source.right - sx) * w_) / sw);
            r.bottom = static_cast<LONG>((static_cast<long long>(source.bottom - sy) * h_) / sh);
            r.left = std::max<LONG>(0, std::min<LONG>(w_ - 1, r.left));
            r.top = std::max<LONG>(0, std::min<LONG>(h_ - 1, r.top));
            r.right = std::max<LONG>(r.left + 1, std::min<LONG>(w_, r.right));
            r.bottom = std::max<LONG>(r.top + 1, std::min<LONG>(h_, r.bottom));
            return r;
        }

        POINT NativeScreenPoint(int viewerX, int viewerY) const {
            const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int sw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
            const int sh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
            return POINT{
                sx + static_cast<LONG>((static_cast<long long>(std::max(0, std::min(w_ - 1, viewerX))) * sw) / std::max(1, w_)),
                sy + static_cast<LONG>((static_cast<long long>(std::max(0, std::min(h_ - 1, viewerY))) * sh) / std::max(1, h_))
            };
        }

        static bool NativeRectsEqual(const RECT& a, const RECT& b) {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }

        bool RefreshNativeWindowCache(const NativeDesktopWindowInfo& info, NativeWindowCache& cache) {
            const int fullW = PositiveDim(info.windowRect.right - info.windowRect.left, 32);
            const int fullH = PositiveDim(info.windowRect.bottom - info.windowRect.top, 24);
            const int cropX = std::max(0, std::min(fullW - 1,
                static_cast<int>(info.sourceRect.left - info.windowRect.left)));
            const int cropY = std::max(0, std::min(fullH - 1,
                static_cast<int>(info.sourceRect.top - info.windowRect.top)));
            const int cropW = std::max(1, std::min(fullW - cropX,
                static_cast<int>(info.sourceRect.right - info.sourceRect.left)));
            const int cropH = std::max(1, std::min(fullH - cropY,
                static_cast<int>(info.sourceRect.bottom - info.sourceRect.top)));

            HDC captureDc = CreateCompatibleDC(memDc_);
            if (!captureDc) return false;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = fullW;
            bi.bmiHeader.biHeight = -fullH;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            void* captureBits = nullptr;
            HBITMAP bmp = CreateDIBSection(captureDc, &bi, DIB_RGB_COLORS, &captureBits, nullptr, 0);
            if (!bmp || !captureBits) {
                if (bmp) DeleteObject(bmp);
                DeleteDC(captureDc);
                return false;
            }

            HGDIOBJ old = SelectObject(captureDc, bmp);
            FillRectColor(captureDc, 0, 0, fullW, fullH, RGB(0, 0, 0));
            BOOL ok = PrintWindow(info.hwnd, captureDc, 0x00000002);
            if (!ok) ok = PrintWindow(info.hwnd, captureDc, 0);

            if (ok) {
                cache.width = cropW;
                cache.height = cropH;
                cache.sourceRect = info.sourceRect;
                cache.bgra.resize(static_cast<size_t>(cropW) * static_cast<size_t>(cropH) * 4u);

                const auto* srcBase = static_cast<const uint8_t*>(captureBits);
                for (int row = 0; row < cropH; ++row) {
                    const auto* src = srcBase +
                        (static_cast<size_t>(cropY + row) * static_cast<size_t>(fullW) +
                         static_cast<size_t>(cropX)) * 4u;
                    auto* dst = cache.bgra.data() +
                        static_cast<size_t>(row) * static_cast<size_t>(cropW) * 4u;
                    std::memcpy(dst, src, static_cast<size_t>(cropW) * 4u);
                }
                cache.lastCaptureTick = GetTickCount64();
                cache.valid = true;
            }

            SelectObject(captureDc, old);
            DeleteObject(bmp);
            DeleteDC(captureDc);
            return ok != FALSE;
        }

        bool CompositeNativeDesktopWindow(const NativeDesktopWindowInfo& info, HDC targetDc) {
            auto& cache = nativeWindowCache_[info.hwnd];
            const ULONGLONG now = GetTickCount64();
            const bool active = !nativeSyntheticFocus_ && info.hwnd == NativeFocusedTopLevel();
            const ULONGLONG refreshMs = active ? 70ull : 700ull;
            const bool geometryChanged = !cache.valid || !NativeRectsEqual(cache.sourceRect, info.sourceRect);
            const bool due = !cache.valid || geometryChanged ||
                now - cache.lastCaptureTick >= refreshMs;

            if (due) {
                RefreshNativeWindowCache(info, cache);
            }
            if (!cache.valid || cache.bgra.empty()) return false;

            const RECT dst = NativeViewerRect(info.sourceRect);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = cache.width;
            bi.bmiHeader.biHeight = -cache.height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(targetDc, HALFTONE);
            const int copied = StretchDIBits(targetDc,
                dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                0, 0, cache.width, cache.height,
                cache.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            return copied != GDI_ERROR;
        }

        void PruneNativeWindowCache() {
            for (auto it = nativeWindowCache_.begin(); it != nativeWindowCache_.end();) {
                if (!it->first || !IsWindow(it->first)) it = nativeWindowCache_.erase(it);
                else ++it;
            }
        }

        bool LaunchNativeUserProcess(const std::string& exe, const std::string& args) {
            if (!EnsurePrivateDesktop()) return false;
            HANDLE process = hi5::LaunchInInteractiveSessionOnDesktop(exe, args, privateDesktopFullName_);
            if (!process) return false;
            nativeDesktopProcesses_.push_back(process);
            LogInfo("[background-native] user process launched exe=" + exe +
                " pid=" + std::to_string(GetProcessId(process)) +
                " desktop=" + WideToUtf8(privateDesktopFullName_));
            return true;
        }

        bool LaunchNativeElevatedProcess(const std::string& exe, const std::string& args) {
            if (!EnsurePrivateDesktop()) return false;
            HANDLE process = hi5::LaunchInElevatedSessionOnDesktop(exe, args, privateDesktopFullName_);
            if (!process) return false;
            nativeDesktopProcesses_.push_back(process);
            LogInfo("[background-native] elevated process launched exe=" + exe +
                " pid=" + std::to_string(GetProcessId(process)) +
                " desktop=" + WideToUtf8(privateDesktopFullName_));
            return true;
        }

        bool InitializeNativeDesktop() {
            if (!EnsurePrivateDesktop()) return false;
            LogInfo("[background-native] mode=private-hdesk shell=hi5-system-maintenance apps_opened=0 account=SYSTEM");
            return true;
        }

        void StopNativeDesktopProcesses() {
            for (HANDLE process : nativeDesktopProcesses_) {
                if (!process) continue;
                if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0);
                WaitForSingleObject(process, 500);
                CloseHandle(process);
            }
            nativeDesktopProcesses_.clear();
            nativeFocusHwnd_ = nullptr;
            nativePreferredHwnd_ = nullptr;
            nativeSyntheticFocus_ = false;
            nativeHybridPointerCaptured_ = false;
            nativeWindowCache_.clear();
            nativeRestoreRects_.clear();
        }

        RECT NativeTaskbarRect() const {
            return RECT{ 0, h_ - nativeTaskbarH_, w_, h_ };
        }

        RECT NativeStartButtonRect() const {
            return RECT{ 8, h_ - nativeTaskbarH_ + 6, 50, h_ - 6 };
        }

        RECT NativeStartMenuRect() const {
            const int menuW = std::min(520, std::max(360, w_ - 32));
            const int menuH = std::min(580, std::max(360, h_ - nativeTaskbarH_ - 24));
            return RECT{ 8, h_ - nativeTaskbarH_ - menuH - 8, 8 + menuW, h_ - nativeTaskbarH_ - 8 };
        }

        RECT NativeMenuRowRect(size_t row) const {
            RECT m = NativeStartMenuRect();
            const int top = m.top + 72 + static_cast<int>(row) * 42;
            return RECT{ m.left + 14, top, m.right - 14, top + 38 };
        }

        RECT NativeRunDialogRect() const {
            const int ww = std::min(620, std::max(420, w_ - 80));
            const int hh = 190;
            return RECT{ (w_ - ww) / 2, std::max(24, (h_ - hh) / 2),
                (w_ + ww) / 2, std::max(24, (h_ - hh) / 2) + hh };
        }

        RECT NativeRunInputRect() const {
            RECT d = NativeRunDialogRect();
            return RECT{ d.left + 22, d.top + 72, d.right - 22, d.top + 108 };
        }

        RECT NativeRunOkRect() const {
            RECT d = NativeRunDialogRect();
            return RECT{ d.right - 200, d.bottom - 52, d.right - 112, d.bottom - 18 };
        }

        RECT NativeRunCancelRect() const {
            RECT d = NativeRunDialogRect();
            return RECT{ d.right - 104, d.bottom - 52, d.right - 16, d.bottom - 18 };
        }

        static constexpr int kNativeShellTopH = 70;
        static constexpr int kNativeShellSideW = 252; // retained for dormant launcher helpers

        bool NativeShellSidebarVisible() const {
            return false;
        }

        RECT NativeShellWorkspaceRect() const {
            return RECT{ 0, kNativeShellTopH, w_, h_ - nativeTaskbarH_ };
        }

        RECT NativeShellTopActionRect(size_t index) const {
            const int startX = 176;
            const int itemW = 86;
            const int x = startX + static_cast<int>(index) * itemW;
            return RECT{ x, 5, x + itemW - 4, kNativeShellTopH - 5 };
        }

        RECT NativeShellSearchRect() const {
            return RECT{ 14, kNativeShellTopH + 16, kNativeShellSideW - 14, kNativeShellTopH + 52 };
        }

        RECT NativeShellSideActionRect(size_t index) const {
            const int y = kNativeShellTopH + 84 + static_cast<int>(index) * 44;
            return RECT{ 10, y, kNativeShellSideW - 10, y + 40 };
        }

        const std::vector<std::wstring>& NativeShellTopActions() const {
            static const std::vector<std::wstring> ids{
                L"explorer", L"cmd", L"taskmgr", L"services", L"registry",
                L"events", L"hi5web", L"updates", L"devices", L"more"
            };
            return ids;
        }

        const std::vector<std::wstring>& NativeShellSideActions() const {
            static const std::vector<std::wstring> ids{
                L"hi5web", L"cmd", L"powershell", L"explorer", L"taskmgr", L"services",
                L"events", L"registry", L"devices", L"disks", L"updates", L"system"
            };
            return ids;
        }

        std::wstring NativeShellActionTitle(const std::wstring& id) const {
            if (id == L"hi5web") return L"Hi5 Web";
            if (id == L"explorer") return L"File Browser";
            if (id == L"cmd") return L"Command";
            if (id == L"taskmgr") return L"Task Manager";
            if (id == L"services") return L"Services";
            if (id == L"registry") return L"Registry";
            if (id == L"events") return L"Event Viewer";
            if (id == L"updates") return L"Windows Update";
            if (id == L"devices") return L"Device Manager";
            if (id == L"powershell") return L"PowerShell";
            if (id == L"disks") return L"Disk Management";
            if (id == L"system") return L"System Information";
            if (id == L"more") return L"More";
            return id;
        }

        std::wstring NativeShellActionSubtitle(const std::wstring& id) const {
            if (id == L"hi5web") return L"Browse the web (isolated)";
            if (id == L"cmd") return L"Run CMD as SYSTEM";
            if (id == L"powershell") return L"Run PowerShell as SYSTEM";
            if (id == L"explorer") return L"Browse and manage files";
            if (id == L"taskmgr") return L"View and manage processes";
            if (id == L"services") return L"Manage Windows services";
            if (id == L"events") return L"View system and application logs";
            if (id == L"registry") return L"Edit the Windows registry";
            if (id == L"devices") return L"View and manage hardware";
            if (id == L"disks") return L"Manage disks and volumes";
            if (id == L"updates") return L"Check and install updates";
            if (id == L"system") return L"View detailed system information";
            return L"";
        }

        std::wstring NativeShellActionIconPath(const std::wstring& id) const {
            if (id == L"hi5web" || id == L"more") return CurrentExePathW();
            if (const ToolSpec* spec = FindToolSpec(id)) {
                return spec->iconPath ? spec->iconPath : L"";
            }
            return L"";
        }

        bool LaunchNativeShellAction(const std::wstring& id) {
            if (id == L"more") {
                nativeLauncherOpen_ = true;
                nativeLauncherFolder_.clear();
                return true;
            }
            if (id == L"hi5web") {
                const auto& apps = NativeMaintenanceApps();
                for (size_t i = 0; i < apps.size(); ++i) {
                    if (apps[i].title == L"Hi5 Web") return LaunchNativeMaintenanceApp(i);
                }
                return false;
            }
            if (const ToolSpec* spec = FindToolSpec(id)) {
                const bool ok = LaunchTool(*spec);
                if (ok) {
                    nativeSyntheticFocus_ = true;
                    nativeFocusHwnd_ = nullptr;
                    nativePreferredHwnd_ = nullptr;
                }
                return ok;
            }
            return false;
        }

        bool HandleNativeShellChromeClick(int x, int y) {
            POINT pt{ x, y };
            const auto& top = NativeShellTopActions();
            const int rightReserve = 230;
            for (size_t i = 0; i < top.size(); ++i) {
                RECT r = NativeShellTopActionRect(i);
                if (r.right > w_ - rightReserve) break;
                if (PtInRect(&r, pt)) return LaunchNativeShellAction(top[i]);
            }

            if (NativeShellSidebarVisible()) {
                RECT search = NativeShellSearchRect();
                if (PtInRect(&search, pt)) {
                    nativeLauncherOpen_ = true;
                    nativeLauncherFolder_.clear();
                    return true;
                }
                const auto& side = NativeShellSideActions();
                for (size_t i = 0; i < side.size(); ++i) {
                    RECT r = NativeShellSideActionRect(i);
                    if (r.bottom > h_ - nativeTaskbarH_ - 10) break;
                    if (PtInRect(&r, pt)) return LaunchNativeShellAction(side[i]);
                }
            }

            if (y < kNativeShellTopH) return true;
            if (NativeShellSidebarVisible() &&
                x < kNativeShellSideW &&
                y < h_ - nativeTaskbarH_) return true;
            return false;
        }

        std::vector<size_t> NativeLauncherRows() const {
            std::vector<size_t> rows;
            const auto& apps = NativeMaintenanceApps();
            if (nativeLauncherFolder_.empty()) return rows;
            for (size_t i = 0; i < apps.size(); ++i) {
                if (apps[i].group == nativeLauncherFolder_) rows.push_back(i);
            }
            return rows;
        }

        std::vector<BackstageWindow*> NativeTaskbarSyntheticWindows() {
            std::vector<BackstageWindow*> out;
            for (auto& win : windows_) {
                if (!win.closed) out.push_back(&win);
            }
            return out;
        }

        void DrawNativeShellBackdrop(HDC dc) {
            FillRectColor(dc, 0, 0, w_, h_, RGB(6, 31, 57));
            RECT work = NativeShellWorkspaceRect();
            FillRectColor(dc, work.left, work.top,
                work.right - work.left, work.bottom - work.top, RGB(18, 76, 132));

            const int workW = std::max(1L, work.right - work.left);
            const int workH = std::max(1L, work.bottom - work.top);
            RoundRectColor(dc,
                work.left + (workW * 42) / 100,
                work.top + (workH * 10) / 100,
                (workW * 42) / 100,
                (workH * 62) / 100,
                RGB(21, 103, 184), RGB(21, 103, 184), 96);
            RoundRectColor(dc,
                work.left + (workW * 18) / 100,
                work.top + (workH * 48) / 100,
                (workW * 58) / 100,
                (workH * 34) / 100,
                RGB(16, 88, 160), RGB(16, 88, 160), 90);
        }

        void DrawNativeShellChrome(HDC dc) {
            FillRectColor(dc, 0, 0, w_, kNativeShellTopH, RGB(5, 27, 50));
            FillRectColor(dc, 0, kNativeShellTopH - 1, w_, 1, RGB(30, 66, 98));

            Text(dc, 16, 14, L"Hi5", 21, RGB(73, 170, 255), true);
            Text(dc, 50, 14, L"Central", 21, RGB(247, 250, 255), true);
            Text(dc, 18, 43, L"Background Tools", 11, RGB(224, 233, 244), false);

            const auto& top = NativeShellTopActions();
            const int rightReserve = 230;
            POINT mouse{ mouseX_, mouseY_ };
            for (size_t i = 0; i < top.size(); ++i) {
                RECT r = NativeShellTopActionRect(i);
                if (r.right > w_ - rightReserve) break;
                const bool hover = PtInRect(&r, mouse) != 0;
                if (hover) {
                    RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                        RGB(18, 52, 82), RGB(46, 86, 122), 8);
                }
                const std::wstring iconPath = NativeShellActionIconPath(top[i]);
                if (!iconPath.empty()) {
                    const int iconX = r.left + ((r.right - r.left) - 24) / 2;
                    DrawIconFromFile(dc, iconPath, iconX, r.top + 7, 24);
                }
                TextClipped(dc, RECT{ r.left + 2, r.top + 35, r.right - 2, r.bottom - 2 },
                    NativeShellActionTitle(top[i]), 10, RGB(241, 246, 252), false,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            const int statusX = std::max(0, w_ - 212);
            RoundRectColor(dc, statusX, 25, 9, 9, RGB(54, 211, 93), RGB(54, 211, 93), 5);
            TextClipped(dc, RECT{ statusX + 16, 12, w_ - 14, 50 },
                L"Connected (Background)", 11, RGB(246, 249, 252), false,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (!NativeShellSidebarVisible()) return;

            FillRectColor(dc, 0, kNativeShellTopH, kNativeShellSideW,
                h_ - kNativeShellTopH - nativeTaskbarH_, RGB(18, 39, 62));
            FillRectColor(dc, kNativeShellSideW - 1, kNativeShellTopH, 1,
                h_ - kNativeShellTopH - nativeTaskbarH_, RGB(44, 72, 101));

            Text(dc, 18, kNativeShellTopH + 10, L"Hi5 Background", 14, RGB(247, 250, 255), true);

            RECT search = NativeShellSearchRect();
            RoundRectColor(dc, search.left, search.top, search.right - search.left, search.bottom - search.top,
                RGB(31, 55, 81), RGB(49, 78, 108), 8);
            TextClipped(dc, RECT{ search.left + 12, search.top, search.right - 10, search.bottom },
                L"Search tools and apps...", 11, RGB(178, 195, 214), false,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            Text(dc, 18, kNativeShellTopH + 62, L"System Tools", 11, RGB(220, 230, 241), true);

            const auto& side = NativeShellSideActions();
            for (size_t i = 0; i < side.size(); ++i) {
                RECT r = NativeShellSideActionRect(i);
                if (r.bottom > h_ - nativeTaskbarH_ - 10) break;
                const bool hover = PtInRect(&r, mouse) != 0;
                bool active = false;
                if (side[i] != L"hi5web") {
                    if (BackstageWindow* open = FindOpenTool(side[i])) {
                        active = !open->minimized && open->id == activeWindowId_ && nativeSyntheticFocus_;
                    }
                }

                if (hover || active) {
                    RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                        active ? RGB(33, 77, 116) : RGB(27, 58, 88),
                        active ? RGB(54, 117, 172) : RGB(45, 82, 116), 7);
                }

                const std::wstring iconPath = NativeShellActionIconPath(side[i]);
                if (!iconPath.empty()) DrawIconFromFile(dc, iconPath, r.left + 10, r.top + 7, 26);
                TextClipped(dc, RECT{ r.left + 46, r.top + 3, r.right - 8, r.top + 20 },
                    NativeShellActionTitle(side[i]), 11, RGB(244, 248, 252), true,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                TextClipped(dc, RECT{ r.left + 46, r.top + 19, r.right - 8, r.bottom - 2 },
                    NativeShellActionSubtitle(side[i]), 9, RGB(160, 181, 203), false,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }

        void DrawNativeTaskbar(HDC dc, const std::vector<NativeDesktopWindowInfo>& windows) {
            const RECT bar = NativeTaskbarRect();
            FillRectColor(dc, bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top, RGB(32, 32, 32));
            HPEN edge = CreatePen(PS_SOLID, 1, RGB(66, 66, 66));
            HGDIOBJ old = SelectObject(dc, edge);
            MoveToEx(dc, 0, bar.top, nullptr); LineTo(dc, w_, bar.top);
            SelectObject(dc, old); DeleteObject(edge);

            RECT start = NativeStartButtonRect();
            if (nativeLauncherOpen_) FillRectColor(dc, start.left, start.top, start.right - start.left, start.bottom - start.top, RGB(62, 62, 62));
            DrawStartIcon(dc, start.left + 12, start.top + 9, 18);

            int x = start.right + 8;
            const int maxX = std::max(x, w_ - 150);
            size_t shown = 0;

            for (BackstageWindow* win : NativeTaskbarSyntheticWindows()) {
                if (!win || x + 150 > maxX || shown >= 8) break;
                RECT r{ x, bar.top + 5, x + 146, bar.bottom - 5 };
                const bool active = nativeSyntheticFocus_ && win->id == activeWindowId_ && !win->minimized;
                RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                    active ? RGB(58, 80, 108) : RGB(45, 45, 45), RGB(80, 80, 80), 6);
                const ToolSpec* spec = FindToolSpec(win->toolId);
                int textLeft = r.left + 10;
                if (spec && spec->iconPath) {
                    DrawIconFromFile(dc, spec->iconPath, r.left + 7, r.top + 5, 24);
                    textLeft = r.left + 38;
                }
                TextClipped(dc, RECT{ textLeft, r.top, r.right - 8, r.bottom }, win->title, 12, RGB(238, 238, 238), false);
                x += 152;
                ++shown;
            }

            for (const auto& info : windows) {
                if (!info.hwnd || x + 150 > maxX || shown >= 8) continue;
                RECT r{ x, bar.top + 5, x + 146, bar.bottom - 5 };
                const bool active = !nativeSyntheticFocus_ && info.hwnd == NativeFocusedTopLevel();
                RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                    active ? RGB(72, 72, 72) : RGB(45, 45, 45), RGB(80, 80, 80), 6);
                std::wstring title = info.title.empty() ? info.className : info.title;
                TextClipped(dc, RECT{ r.left + 10, r.top, r.right - 8, r.bottom }, title, 12, RGB(238, 238, 238), false);
                x += 152;
                ++shown;
            }

            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t clock[32]{};
            swprintf_s(clock, L"%02u:%02u", st.wHour, st.wMinute);
            TextClipped(dc, RECT{ std::max(0, w_ - 112), bar.top, w_ - 12, bar.bottom }, clock, 12, RGB(235, 235, 235), false, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        void DrawNativeLauncher(HDC dc) {
            if (!nativeLauncherOpen_) return;
            RECT m = NativeStartMenuRect();
            RoundRectColor(dc, m.left, m.top, m.right - m.left, m.bottom - m.top, RGB(28, 28, 28), RGB(74, 74, 74), 10);
            Text(dc, m.left + 18, m.top + 18,
                nativeLauncherFolder_.empty() ? L"Hi5Central Background" : nativeLauncherFolder_,
                18, RGB(245, 245, 245), true);

            if (nativeLauncherFolder_.empty()) {
                const std::vector<std::wstring> folders{ L"Windows Tools", L"Windows Accessories", L"System Settings", L"Network Tools" };
                for (size_t i = 0; i < folders.size(); ++i) {
                    RECT r = NativeMenuRowRect(i);
                    FillRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, RGB(39, 39, 39));
                    TextClipped(dc, RECT{ r.left + 14, r.top, r.right - 40, r.bottom }, folders[i], 14, RGB(245, 245, 245), true);
                    TextClipped(dc, RECT{ r.right - 34, r.top, r.right - 12, r.bottom }, L">", 15, RGB(210, 210, 210), true, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                RECT run = NativeMenuRowRect(4);
                FillRectColor(dc, run.left, run.top, run.right - run.left, run.bottom - run.top, RGB(39, 39, 39));
                TextClipped(dc, RECT{ run.left + 14, run.top, run.right - 12, run.bottom }, L"Run...", 14, RGB(245, 245, 245), true);
                TextClipped(dc, RECT{ m.left + 18, m.bottom - 56, m.right - 18, m.bottom - 20 },
                    L"Maintenance desktop - apps run as SYSTEM", 11, RGB(160, 160, 160));
                return;
            }

            RECT back = NativeMenuRowRect(0);
            FillRectColor(dc, back.left, back.top, back.right - back.left, back.bottom - back.top, RGB(39, 39, 39));
            TextClipped(dc, RECT{ back.left + 14, back.top, back.right - 12, back.bottom }, L"<  Back", 13, RGB(225, 225, 225), true);

            auto rows = NativeLauncherRows();
            const auto& apps = NativeMaintenanceApps();
            for (size_t j = 0; j < rows.size() && j < 10; ++j) {
                const auto& app = apps[rows[j]];
                RECT r = NativeMenuRowRect(j + 1);
                FillRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, RGB(39, 39, 39));

                std::wstring iconPath = app.exe;
                if (iconPath.empty() && app.title == L"Hi5 Web") iconPath = CurrentExePathW();

                int textLeft = r.left + 14;
                if (!iconPath.empty()) {
                    DrawIconFromFile(dc, iconPath, r.left + 10, r.top + 6, 26);
                    textLeft = r.left + 46;
                }
                TextClipped(dc, RECT{ textLeft, r.top, r.right - 12, r.bottom }, app.title, 13, RGB(245, 245, 245), false);
            }
        }

        static std::wstring TrimNativeRunText(std::wstring value) {
            const auto first = value.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos) return {};
            const auto last = value.find_last_not_of(L" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        static std::wstring LowerNativePath(std::wstring value) {
            for (auto& ch : value) ch = static_cast<wchar_t>(towlower(ch));
            return value;
        }

        bool ParseNativeRunCommand(const std::wstring& command, std::wstring& exe, std::wstring& args) {
            std::wstring text = TrimNativeRunText(command);
            if (text.empty()) return false;
            if (text.front() == L'"') {
                const size_t end = text.find(L'"', 1);
                if (end == std::wstring::npos) return false;
                exe = text.substr(1, end - 1);
                args = TrimNativeRunText(text.substr(end + 1));
                return !exe.empty();
            }

            const size_t firstSpace = text.find_first_of(L" \t");
            if (firstSpace == std::wstring::npos) {
                exe = text;
                args.clear();
                return true;
            }

            // Unquoted paths containing spaces should be quoted, exactly like the
            // Windows Run dialog. Keep parsing deterministic rather than guessing.
            exe = text.substr(0, firstSpace);
            args = TrimNativeRunText(text.substr(firstSpace + 1));
            return !exe.empty();
        }

        bool LaunchNativeRunCommand(const std::wstring& command) {
            std::wstring exe;
            std::wstring args;
            if (!ParseNativeRunCommand(command, exe, args)) {
                nativeRunStatus_ = L"Enter an executable/package path. Quote paths that contain spaces.";
                return false;
            }

            const std::wstring lower = LowerNativePath(exe);
            auto endsWith = [&](const wchar_t* suffix) {
                const std::wstring sfx = suffix;
                return lower.size() >= sfx.size() &&
                    lower.compare(lower.size() - sfx.size(), sfx.size(), sfx) == 0;
            };

            std::wstring launchExe = exe;
            std::wstring launchArgs = args;
            if (endsWith(L".msi")) {
                launchExe = L"C:\\Windows\\System32\\msiexec.exe";
                launchArgs = L"/i \"" + exe + L"\"";
                if (!args.empty()) launchArgs += L" " + args;
            } else if (endsWith(L".cmd") || endsWith(L".bat") || endsWith(L".com")) {
                launchExe = L"C:\\Windows\\System32\\cmd.exe";
                launchArgs = L"/c \"\"" + exe + L"\"";
                if (!args.empty()) launchArgs += L" " + args;
                launchArgs += L"\"";
            } else if (endsWith(L".ps1")) {
                launchExe = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
                launchArgs = L"-NoExit -ExecutionPolicy Bypass -File \"" + exe + L"\"";
                if (!args.empty()) launchArgs += L" " + args;
            } else if (!endsWith(L".exe")) {
                nativeRunStatus_ = L"Supported: .exe, .com, .msi, .cmd, .bat and .ps1.";
                return false;
            }

            const bool ok = LaunchNativeElevatedProcess(WideToUtf8(launchExe), WideToUtf8(launchArgs));
            if (ok) {
                nativeRunStatus_.clear();
                nativeRunDialogOpen_ = false;
                nativeRunText_.clear();
            } else {
                nativeRunStatus_ = L"Launch failed. Check the Agent diagnostics for the Windows error.";
            }
            LogInfo("[background-native] run command=" + WideToUtf8(command) +
                " resolved_exe=" + WideToUtf8(launchExe) + " ok=" + (ok ? std::string("1") : std::string("0")));
            return ok;
        }

        void DrawNativeRunDialog(HDC dc) {
            if (!nativeRunDialogOpen_) return;
            RECT d = NativeRunDialogRect();
            RoundRectColor(dc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                RGB(31, 31, 31), RGB(92, 92, 92), 10);
            TextClipped(dc, RECT{ d.left + 22, d.top + 14, d.right - 22, d.top + 45 },
                L"Run on Background desktop", 17, RGB(245, 245, 245), true);
            TextClipped(dc, RECT{ d.left + 22, d.top + 42, d.right - 22, d.top + 68 },
                L"Run an EXE, MSI, CMD/BAT or PowerShell script as SYSTEM.", 11, RGB(170, 170, 170));

            RECT input = NativeRunInputRect();
            FillRectColor(dc, input.left, input.top, input.right - input.left, input.bottom - input.top, RGB(255, 255, 255));
            TextClipped(dc, RECT{ input.left + 10, input.top, input.right - 10, input.bottom },
                nativeRunText_.empty() ? L"C:\\path\\to\\setup.exe" : nativeRunText_ + L"_",
                13, nativeRunText_.empty() ? RGB(125, 125, 125) : RGB(20, 20, 20));

            if (!nativeRunStatus_.empty()) {
                TextClipped(dc, RECT{ d.left + 22, d.top + 112, d.right - 220, d.bottom - 16 },
                    nativeRunStatus_, 10, RGB(245, 170, 90), false, DT_LEFT | DT_TOP | DT_WORDBREAK);
            }

            RECT ok = NativeRunOkRect();
            RECT cancel = NativeRunCancelRect();
            RoundRectColor(dc, ok.left, ok.top, ok.right - ok.left, ok.bottom - ok.top, RGB(0, 103, 192), RGB(0, 103, 192), 6);
            TextClipped(dc, ok, L"Run", 12, RGB(255, 255, 255), true, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RoundRectColor(dc, cancel.left, cancel.top, cancel.right - cancel.left, cancel.bottom - cancel.top, RGB(52, 52, 52), RGB(92, 92, 92), 6);
            TextClipped(dc, cancel, L"Cancel", 12, RGB(235, 235, 235), false, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        HWND FindNativeWindowByTitle(const std::wstring& title) const {
            if (title.empty()) return nullptr;
            std::wstring needle = title;
            std::transform(needle.begin(), needle.end(), needle.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

            for (const auto& info : NativeDesktopWindows()) {
                if (!info.hwnd || !IsWindow(info.hwnd) || info.title.empty()) continue;
                std::wstring hay = info.title;
                std::transform(hay.begin(), hay.end(), hay.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                if (hay == needle || hay.find(needle) != std::wstring::npos) return info.hwnd;
            }
            return nullptr;
        }

        const ToolSpec* HybridToolForMaintenanceTitle(const std::wstring& title) const {
            struct Map { const wchar_t* title; const wchar_t* toolId; };
            static const Map maps[] = {
                { L"Services", L"services" },
                { L"Event Viewer", L"events" },
                { L"Device Manager", L"devices" },
                { L"Disk Management", L"disks" },
                { L"Registry Editor", L"registry" },
                { L"Task Manager", L"taskmgr" },
                { L"Programs and Features", L"apps" },
                { L"Command Prompt", L"cmd" },
                { L"PowerShell", L"powershell" },
                { L"Notepad", L"notepad" },
                { L"System Information", L"system" },
                { L"Windows Update", L"updates" },
                { L"Users & Sessions", L"sessions" },
            };
            for (const auto& map : maps) {
                if (title == map.title) return FindToolSpec(map.toolId);
            }
            return nullptr;
        }

        bool LaunchNativeMaintenanceApp(size_t index) {
            const auto& apps = NativeMaintenanceApps();
            if (index >= apps.size()) return false;
            const auto& app = apps[index];

            if (app.title == L"Hi5 Web") {
                if (HWND existing = FindNativeWindowByTitle(L"Hi5 Web")) {
                    NativeActivateTopLevel(existing);
                    LogInfo("[background-native] launcher app=Hi5 Web reused existing private window");
                    return true;
                }

                const std::wstring currentExe = CurrentExePathW();
                const bool ok = !currentExe.empty() &&
                    LaunchNativeElevatedProcess(
                        WideToUtf8(currentExe),
                        "--mode backstage-browser --url about:blank");
                if (ok) nativeSyntheticFocus_ = false;
                LogInfo("[background-native] launcher app=Hi5 Web group=Network Tools"
                    " surface=webview2-private-hdesk account=SYSTEM ok=" +
                    std::string(ok ? "1" : "0"));
                return ok;
            }

            if (const ToolSpec* hybrid = HybridToolForMaintenanceTitle(app.title)) {
                const bool ok = LaunchTool(*hybrid);
                if (ok) {
                    nativeSyntheticFocus_ = true;
                    nativeFocusHwnd_ = nullptr;
                }
                LogInfo("[background-native] launcher app=" + WideToUtf8(app.title) +
                    " group=" + WideToUtf8(app.group) +
                    " surface=hi5-managed account=SYSTEM ok=" + (ok ? std::string("1") : std::string("0")));
                return ok;
            }

            if (HWND existing = FindNativeWindowByTitle(app.title)) {
                NativeActivateTopLevel(existing);
                LogInfo("[background-native] launcher reused existing native window title=" +
                    WideToUtf8(app.title));
                return true;
            }

            const bool ok = LaunchNativeElevatedProcess(WideToUtf8(app.exe), WideToUtf8(app.args));
            if (ok) nativeSyntheticFocus_ = false;
            LogInfo("[background-native] launcher app=" + WideToUtf8(app.title) +
                " group=" + WideToUtf8(app.group) +
                " surface=native-hwnd account=SYSTEM ok=" + (ok ? std::string("1") : std::string("0")));
            return ok;
        }

        bool HandleNativeShellClick(int x, int y) {
            POINT pt{ x, y };

            if (nativeRunDialogOpen_) {
                RECT d = NativeRunDialogRect();
                RECT ok = NativeRunOkRect();
                RECT cancel = NativeRunCancelRect();
                if (PtInRect(&ok, pt)) { LaunchNativeRunCommand(nativeRunText_); return true; }
                if (PtInRect(&cancel, pt)) {
                    nativeRunDialogOpen_ = false;
                    nativeRunText_.clear();
                    nativeRunStatus_.clear();
                    return true;
                }
                return true;
            }

            if (HandleNativeShellChromeClick(x, y)) return true;

            RECT start = NativeStartButtonRect();
            if (PtInRect(&start, pt)) {
                nativeLauncherOpen_ = !nativeLauncherOpen_;
                if (!nativeLauncherOpen_) nativeLauncherFolder_.clear();
                return true;
            }

            if (nativeLauncherOpen_) {
                RECT menu = NativeStartMenuRect();
                if (!PtInRect(&menu, pt)) {
                    nativeLauncherOpen_ = false;
                    nativeLauncherFolder_.clear();
                    return false;
                }

                if (nativeLauncherFolder_.empty()) {
                    RECT toolsRow = NativeMenuRowRect(0);
                    RECT accessoriesRow = NativeMenuRowRect(1);
                    RECT settingsRow = NativeMenuRowRect(2);
                    RECT networkRow = NativeMenuRowRect(3);
                    RECT runRow = NativeMenuRowRect(4);
                    if (PtInRect(&toolsRow, pt)) { nativeLauncherFolder_ = L"Windows Tools"; return true; }
                    if (PtInRect(&accessoriesRow, pt)) { nativeLauncherFolder_ = L"Windows Accessories"; return true; }
                    if (PtInRect(&settingsRow, pt)) { nativeLauncherFolder_ = L"System Settings"; return true; }
                    if (PtInRect(&networkRow, pt)) { nativeLauncherFolder_ = L"Network Tools"; return true; }
                    if (PtInRect(&runRow, pt)) {
                        nativeLauncherOpen_ = false;
                        nativeLauncherFolder_.clear();
                        nativeRunDialogOpen_ = true;
                        nativeRunText_.clear();
                        nativeRunStatus_.clear();
                        return true;
                    }
                    return true;
                }

                RECT backRow = NativeMenuRowRect(0);
                if (PtInRect(&backRow, pt)) {
                    nativeLauncherFolder_.clear();
                    return true;
                }
                auto rows = NativeLauncherRows();
                for (size_t j = 0; j < rows.size() && j < 10; ++j) {
                    RECT r = NativeMenuRowRect(j + 1);
                    if (PtInRect(&r, pt)) {
                        LaunchNativeMaintenanceApp(rows[j]);
                        nativeLauncherOpen_ = false;
                        nativeLauncherFolder_.clear();
                        return true;
                    }
                }
                return true;
            }

            RECT bar = NativeTaskbarRect();
            if (PtInRect(&bar, pt)) {
                auto windows = NativeDesktopWindows();
                int bx = NativeStartButtonRect().right + 8;
                const int maxX = std::max(bx, w_ - 150);
                size_t shown = 0;

                for (BackstageWindow* win : NativeTaskbarSyntheticWindows()) {
                    if (!win || bx + 150 > maxX || shown >= 8) break;
                    RECT r{ bx, bar.top + 5, bx + 146, bar.bottom - 5 };
                    if (PtInRect(&r, pt)) {
                        if (nativeSyntheticFocus_ && activeWindowId_ == win->id && !win->minimized) {
                            win->minimized = true;
                        } else {
                            BringToFront(win->id);
                        }
                        nativeSyntheticFocus_ = true;
                        nativeFocusHwnd_ = nullptr;
                        nativePreferredHwnd_ = nullptr;
                        return true;
                    }
                    bx += 152;
                    ++shown;
                }

                for (const auto& info : windows) {
                    if (!info.hwnd || bx + 150 > maxX || shown >= 8) continue;
                    RECT r{ bx, bar.top + 5, bx + 146, bar.bottom - 5 };
                    if (PtInRect(&r, pt)) {
                        NativeActivateTopLevel(info.hwnd);
                        return true;
                    }
                    bx += 152;
                    ++shown;
                }
                return true;
            }
            return false;
        }

        void DrawHybridSyntheticWindows(HDC dc) {
            std::vector<const BackstageWindow*> drawOrder;
            for (const auto& win : windows_) {
                if (!win.closed && !win.minimized) drawOrder.push_back(&win);
            }
            std::sort(drawOrder.begin(), drawOrder.end(),
                [](const BackstageWindow* a, const BackstageWindow* b) { return a->zOrder < b->zOrder; });
            for (const BackstageWindow* win : drawOrder) {
                DrawWindowFrame(dc, *win);
                DrawWindowContent(dc, *win);
            }
        }

        bool DrawNativeDesktop() {
            DrawNativeShellBackdrop(memDc_);
            auto windows = NativeDesktopWindows();
            nativeVisibleWindowCount_ = windows.size();

            if (!nativeWindowInventoryLogged_ && !windows.empty()) {
                nativeWindowInventoryLogged_ = true;
                LogInfo("[background-native] first private desktop windows=" + std::to_string(windows.size()));
                for (size_t i = 0; i < windows.size() && i < 12; ++i) {
                    const auto& info = windows[i];
                    LogInfo("[background-native] window hwnd=0x" +
                        PtrToHex(reinterpret_cast<uintptr_t>(info.hwnd)) +
                        " class=" + WideToUtf8(info.className) +
                        " title=" + WideToUtf8(info.title) +
                        " rect=" + std::to_string(info.windowRect.left) + "," + std::to_string(info.windowRect.top) +
                        "," + std::to_string(info.windowRect.right) + "," + std::to_string(info.windowRect.bottom) +
                        " frame=" + std::to_string(info.sourceRect.left) + "," + std::to_string(info.sourceRect.top) +
                        "," + std::to_string(info.sourceRect.right) + "," + std::to_string(info.sourceRect.bottom) +
                        " iconic=" + std::string(IsIconic(info.hwnd) ? "1" : "0"));
                }
            }

            size_t captured = 0;
            const bool nativeOnTop = !nativeSyntheticFocus_ && nativePreferredHwnd_ && IsWindow(nativePreferredHwnd_);

            for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
                if (IsIconic(it->hwnd)) continue;
                if (nativeOnTop && it->hwnd == nativePreferredHwnd_) continue;
                if (CompositeNativeDesktopWindow(*it, memDc_)) ++captured;
            }

            DrawHybridSyntheticWindows(memDc_);

            if (nativeOnTop) {
                for (const auto& info : windows) {
                    if (info.hwnd == nativePreferredHwnd_ && !IsIconic(info.hwnd)) {
                        if (CompositeNativeDesktopWindow(info, memDc_)) ++captured;
                        break;
                    }
                }
            }

            if (captured > 0 && !nativeFirstCaptureLogged_) {
                nativeFirstCaptureLogged_ = true;
                LogInfo("[background-native] first native capture ok captured=" + std::to_string(captured) +
                    " windows=" + std::to_string(windows.size()));
            }
            DrawNativeShellChrome(memDc_);
            DrawNativeLauncher(memDc_);
            DrawNativeTaskbar(memDc_, windows);
            DrawNativeRunDialog(memDc_);
            PruneNativeWindowCache();
            return true;
        }

        HWND NativeTopLevelAt(int x, int y) const {
            POINT p{ x, y };
            auto windows = NativeDesktopWindows();
            for (const auto& info : windows) {
                RECT dst = NativeViewerRect(info.sourceRect);
                if (PtInRect(&dst, p)) return info.hwnd;
            }
            return nullptr;
        }

        HWND NativeTargetAt(int x, int y, POINT& clientPoint) const {
            HWND top = NativeTopLevelAt(x, y);
            if (!top) return nullptr;
            POINT screen = NativeScreenPoint(x, y);
            POINT p = screen;
            ScreenToClient(top, &p);
            HWND target = top;
            for (int depth = 0; depth < 8; ++depth) {
                HWND child = ChildWindowFromPointEx(target, p, CWP_SKIPDISABLED | CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
                if (!child || child == target) break;
                POINT childPoint = p;
                MapWindowPoints(target, child, &childPoint, 1);
                target = child;
                p = childPoint;
            }
            clientPoint = p;
            return target;
        }

        HWND NativeFocusedTopLevel() const {
            if (!nativeFocusHwnd_ || !IsWindow(nativeFocusHwnd_)) return nullptr;
            HWND root = GetAncestor(nativeFocusHwnd_, GA_ROOT);
            return root && IsWindow(root) ? root : nativeFocusHwnd_;
        }

        LRESULT NativeNonClientHitTest(HWND hwnd, int viewerX, int viewerY) const {
            if (!hwnd || !IsWindow(hwnd)) return HTNOWHERE;
            POINT screen = NativeScreenPoint(viewerX, viewerY);
            DWORD_PTR result = HTNOWHERE;
            if (!SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0,
                MAKELPARAM(static_cast<SHORT>(screen.x), static_cast<SHORT>(screen.y)),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result)) {
                return HTNOWHERE;
            }
            return static_cast<LRESULT>(result);
        }

        static bool NativeHitIsResize(LRESULT hit) {
            return hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
                hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT;
        }

        void NativeActivateTopLevel(HWND hwnd) {
            if (!hwnd || !IsWindow(hwnd)) return;
            ShowWindow(hwnd, SW_RESTORE);
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
            nativeFocusHwnd_ = hwnd;
            nativePreferredHwnd_ = hwnd;
            nativeSyntheticFocus_ = false;
            PostMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);
            PostMessageW(hwnd, WM_SETFOCUS, 0, 0);
        }

        void NativeToggleMaximize(HWND hwnd) {
            if (!hwnd || !IsWindow(hwnd)) return;
            auto it = nativeRestoreRects_.find(hwnd);
            if (it != nativeRestoreRects_.end()) {
                const RECT restore = it->second;
                SetWindowPos(hwnd, HWND_TOP, restore.left, restore.top,
                    std::max<LONG>(120, restore.right - restore.left),
                    std::max<LONG>(80, restore.bottom - restore.top),
                    SWP_SHOWWINDOW | SWP_NOACTIVATE);
                nativeRestoreRects_.erase(it);
                NativeActivateTopLevel(hwnd);
                return;
            }

            RECT current{};
            if (!GetWindowRect(hwnd, &current)) return;
            nativeRestoreRects_[hwnd] = current;

            const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int sw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
            const int sh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
            const int reservedBottom = std::max(1,
                static_cast<int>((static_cast<long long>(nativeTaskbarH_) * sh) / std::max(1, h_)));
            const int reservedTop = std::max(1,
                static_cast<int>((static_cast<long long>(kNativeShellTopH) * sh) / std::max(1, h_)));
            SetWindowPos(hwnd, HWND_TOP, sx, sy + reservedTop, sw,
                std::max(100, sh - reservedTop - reservedBottom),
                SWP_SHOWWINDOW | SWP_NOACTIVATE);
            NativeActivateTopLevel(hwnd);
        }

        void NativeBeginNonClientDrag(HWND hwnd, LRESULT hit, int x, int y) {
            if (!hwnd || !IsWindow(hwnd)) return;
            nativeNonClientHwnd_ = hwnd;
            nativeNonClientHit_ = hit;
            nativeNonClientStartScreen_ = NativeScreenPoint(x, y);
            GetWindowRect(hwnd, &nativeNonClientStartRect_);
            nativeLeftDown_ = true;
            NativeActivateTopLevel(hwnd);
        }

        void NativeUpdateNonClientDrag(int x, int y) {
            if (!nativeNonClientHwnd_ || !IsWindow(nativeNonClientHwnd_) || !nativeLeftDown_) return;
            if (nativeNonClientHit_ != HTCAPTION && !NativeHitIsResize(nativeNonClientHit_)) return;

            POINT now = NativeScreenPoint(x, y);
            const int dx = now.x - nativeNonClientStartScreen_.x;
            const int dy = now.y - nativeNonClientStartScreen_.y;
            RECT r = nativeNonClientStartRect_;

            if (nativeNonClientHit_ == HTCAPTION) {
                OffsetRect(&r, dx, dy);
            } else {
                if (nativeNonClientHit_ == HTLEFT || nativeNonClientHit_ == HTTOPLEFT || nativeNonClientHit_ == HTBOTTOMLEFT) r.left += dx;
                if (nativeNonClientHit_ == HTRIGHT || nativeNonClientHit_ == HTTOPRIGHT || nativeNonClientHit_ == HTBOTTOMRIGHT) r.right += dx;
                if (nativeNonClientHit_ == HTTOP || nativeNonClientHit_ == HTTOPLEFT || nativeNonClientHit_ == HTTOPRIGHT) r.top += dy;
                if (nativeNonClientHit_ == HTBOTTOM || nativeNonClientHit_ == HTBOTTOMLEFT || nativeNonClientHit_ == HTBOTTOMRIGHT) r.bottom += dy;
                if (r.right - r.left < 220) {
                    if (nativeNonClientHit_ == HTLEFT || nativeNonClientHit_ == HTTOPLEFT || nativeNonClientHit_ == HTBOTTOMLEFT) r.left = r.right - 220;
                    else r.right = r.left + 220;
                }
                if (r.bottom - r.top < 140) {
                    if (nativeNonClientHit_ == HTTOP || nativeNonClientHit_ == HTTOPLEFT || nativeNonClientHit_ == HTTOPRIGHT) r.top = r.bottom - 140;
                    else r.bottom = r.top + 140;
                }
            }

            SetWindowPos(nativeNonClientHwnd_, HWND_TOP, r.left, r.top,
                r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }

        bool NativeHandleNonClientButton(const hi5::InputCmd& cmd, int x, int y) {
            if (cmd.mouseButton.button != 0) return false;
            const bool down = cmd.mouseButton.down != 0;

            if (down) {
                HWND top = NativeTopLevelAt(x, y);
                if (!top) return false;
                const LRESULT hit = NativeNonClientHitTest(top, x, y);
                if (hit == HTCLIENT || hit == HTNOWHERE || hit == HTERROR) return false;
                NativeBeginNonClientDrag(top, hit, x, y);
                return true;
            }

            if (!nativeNonClientHwnd_) return false;
            HWND hwnd = nativeNonClientHwnd_;
            const LRESULT hit = nativeNonClientHit_;
            nativeLeftDown_ = false;
            nativeNonClientHwnd_ = nullptr;
            nativeNonClientHit_ = HTNOWHERE;

            const LRESULT releaseHit = NativeNonClientHitTest(hwnd, x, y);
            if (hit == HTMINBUTTON && releaseHit == HTMINBUTTON) {
                ShowWindow(hwnd, SW_MINIMIZE);
                if (NativeFocusedTopLevel() == hwnd) nativeFocusHwnd_ = nullptr;
            } else if (hit == HTMAXBUTTON && releaseHit == HTMAXBUTTON) {
                NativeToggleMaximize(hwnd);
            } else if (hit == HTCLOSE && releaseHit == HTCLOSE) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                nativeRestoreRects_.erase(hwnd);
                if (NativeFocusedTopLevel() == hwnd) nativeFocusHwnd_ = nullptr;
            }
            return true;
        }

        void NativeMouseMove(int x, int y) {
            if (nativeNonClientHwnd_ && nativeLeftDown_) {
                NativeUpdateNonClientDrag(x, y);
                return;
            }
            POINT client{};
            HWND target = NativeTargetAt(x, y, client);
            if (!target) return;
            PostMessageW(target, WM_MOUSEMOVE, nativeLeftDown_ ? MK_LBUTTON : 0,
                MAKELPARAM(static_cast<SHORT>(client.x), static_cast<SHORT>(client.y)));
        }

        void NativeMouseButton(const hi5::InputCmd& cmd, int x, int y) {
            if (NativeHandleNonClientButton(cmd, x, y)) return;

            POINT client{};
            HWND target = NativeTargetAt(x, y, client);
            if (!target && nativeFocusHwnd_ && IsWindow(nativeFocusHwnd_)) {
                target = nativeFocusHwnd_;
                POINT screen = NativeScreenPoint(x, y);
                client = screen;
                ScreenToClient(target, &client);
            }
            if (!target) return;
            const bool down = cmd.mouseButton.down != 0;
            UINT msg = 0;
            WPARAM wp = 0;
            if (cmd.mouseButton.button == 0) { msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; wp = down ? MK_LBUTTON : 0; nativeLeftDown_ = down; }
            else if (cmd.mouseButton.button == 1) { msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; wp = down ? MK_RBUTTON : 0; }
            else { msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; wp = down ? MK_MBUTTON : 0; }
            if (down) {
                HWND top = GetAncestor(target, GA_ROOT);
                if (top && IsWindow(top)) NativeActivateTopLevel(top);
                nativeFocusHwnd_ = target;
                PostMessageW(target, WM_SETFOCUS, 0, 0);
            }
            PostMessageW(target, msg, wp, MAKELPARAM(static_cast<SHORT>(client.x), static_cast<SHORT>(client.y)));
        }

        void NativeMouseWheel(int deltaY, int x, int y) {
            POINT client{};
            HWND target = NativeTargetAt(x, y, client);
            if (!target) return;
            POINT screen = NativeScreenPoint(x, y);
            PostMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<SHORT>(deltaY)),
                MAKELPARAM(static_cast<SHORT>(screen.x), static_cast<SHORT>(screen.y)));
        }

        void NativeKey(const hi5::InputCmd& cmd) {
            if (nativeRunDialogOpen_) {
                if (!cmd.key.down) return;
                if (cmd.key.vk == VK_RETURN) { LaunchNativeRunCommand(nativeRunText_); return; }
                if (cmd.key.vk == VK_ESCAPE) {
                    nativeRunDialogOpen_ = false;
                    nativeRunText_.clear();
                    nativeRunStatus_.clear();
                    return;
                }
                if (cmd.key.vk == VK_BACK) {
                    if (!nativeRunText_.empty()) nativeRunText_.pop_back();
                    nativeRunStatus_.clear();
                    return;
                }
                return;
            }

            HWND target = nativeFocusHwnd_;
            if (!target || !IsWindow(target)) {
                auto windows = NativeDesktopWindows();
                if (!windows.empty()) target = windows.front().hwnd;
            }
            if (!target) return;
            const UINT msg = cmd.key.down ? WM_KEYDOWN : WM_KEYUP;
            const LPARAM lp = 1 | (static_cast<LPARAM>(cmd.key.scanCode) << 16) |
                (cmd.key.isExtended ? (1LL << 24) : 0) |
                (!cmd.key.down ? ((1LL << 30) | (1LL << 31)) : 0);
            PostMessageW(target, msg, static_cast<WPARAM>(cmd.key.vk), lp);
        }

        void NativeText(const std::wstring& text) {
            if (nativeRunDialogOpen_) {
                for (wchar_t ch : text) {
                    if (ch == L'\r' || ch == L'\n') continue;
                    if (ch >= 32 && nativeRunText_.size() < 2048) nativeRunText_.push_back(ch);
                }
                nativeRunStatus_.clear();
                return;
            }

            HWND target = nativeFocusHwnd_;
            if (!target || !IsWindow(target)) return;
            for (wchar_t ch : text) PostMessageW(target, WM_CHAR, static_cast<WPARAM>(ch), 1);
        }

        BackstageWindow* HybridSyntheticWindowAt(int x, int y) {
            POINT pt{ x, y };
            BackstageWindow* hit = nullptr;
            for (auto& win : windows_) {
                if (win.closed || win.minimized || !PtInRect(&win.rect, pt)) continue;
                if (!hit || win.zOrder > hit->zOrder) hit = &win;
            }
            return hit;
        }

        void HandleNativeShortcut(const hi5::InputCmd& cmd) {
            const auto action = static_cast<hi5::ShortcutAction>(cmd.shortcut.action);
            switch (action) {
            case hi5::ShortcutAction::StartMenu:
            case hi5::ShortcutAction::CtrlEsc:
                if (nativeRunDialogOpen_) {
                    nativeRunDialogOpen_ = false;
                    nativeRunText_.clear();
                    nativeRunStatus_.clear();
                } else {
                    nativeLauncherOpen_ = !nativeLauncherOpen_;
                    if (!nativeLauncherOpen_) nativeLauncherFolder_.clear();
                }
                break;
            case hi5::ShortcutAction::TaskManager:
            case hi5::ShortcutAction::CtrlShiftEsc: {
                const auto& apps = NativeMaintenanceApps();
                for (size_t i = 0; i < apps.size(); ++i) {
                    if (apps[i].title == L"Task Manager") { LaunchNativeMaintenanceApp(i); break; }
                }
                break;
            }
            case hi5::ShortcutAction::WinD: {
                auto nativeWindows = NativeDesktopWindows();
                for (const auto& info : nativeWindows) if (info.hwnd) ShowWindow(info.hwnd, SW_MINIMIZE);
                for (auto& win : windows_) if (!win.closed) win.minimized = true;
                nativeFocusHwnd_ = nullptr;
                nativePreferredHwnd_ = nullptr;
                nativeSyntheticFocus_ = false;
                nativeLauncherOpen_ = false;
                nativeLauncherFolder_.clear();
                break;
            }
            case hi5::ShortcutAction::AltF4: {
                if (nativeSyntheticFocus_) {
                    if (BackstageWindow* win = ActiveWindow()) CloseWindow(win->id);
                } else {
                    HWND top = NativeFocusedTopLevel();
                    if (top) PostMessageW(top, WM_CLOSE, 0, 0);
                    nativeFocusHwnd_ = nullptr;
                    nativePreferredHwnd_ = nullptr;
                }
                break;
            }
            case hi5::ShortcutAction::AltTab:
            case hi5::ShortcutAction::AltTabBegin:
            case hi5::ShortcutAction::AltTabNext:
            case hi5::ShortcutAction::WinTab: {
                if (nativeSyntheticFocus_) {
                    HandleShortcut(cmd);
                    break;
                }
                auto windows = NativeDesktopWindows();
                if (windows.empty()) break;
                size_t next = 0;
                for (size_t i = 0; i < windows.size(); ++i) {
                    if (windows[i].hwnd == NativeFocusedTopLevel()) { next = (i + 1) % windows.size(); break; }
                }
                HWND hwnd = windows[next].hwnd;
                if (hwnd) NativeActivateTopLevel(hwnd);
                break;
            }
            case hi5::ShortcutAction::Explorer:
            case hi5::ShortcutAction::WinE:
                LogInfo("[background-native] Explorer shortcut ignored; use Viewer Files to avoid user-shell activation");
                break;
            default:
                break;
            }
        }

        int HandleNativeDesktopInput(hi5::InputPipeReader& pipe) {
            int handled = 0;
            hi5::InputCmd cmd{};
            while (pipe.Read(cmd)) {
                bool visualDirty = true;

                switch (cmd.type) {
                case hi5::InputCmdType::MouseMove:
                    mouseX_ = std::max(0, std::min(w_ - 1, cmd.mouseMove.x));
                    mouseY_ = std::max(0, std::min(h_ - 1, cmd.mouseMove.y));
                    if (nativeHybridPointerCaptured_) {
                        OnMouseMove(mouseX_, mouseY_);
                    } else {
                        const bool movingNativeWindow = nativeNonClientHwnd_ && nativeLeftDown_;
                        NativeMouseMove(mouseX_, mouseY_);
                        visualDirty = movingNativeWindow;
                    }
                    break;

                case hi5::InputCmdType::MouseButton:
                    if (cmd.mouseButton.button == 0) {
                        if (cmd.mouseButton.down) {
                            nativeShellPointerCaptured_ = HandleNativeShellClick(mouseX_, mouseY_);
                            if (nativeShellPointerCaptured_) break;

                            BackstageWindow* syntheticHit = HybridSyntheticWindowAt(mouseX_, mouseY_);
                            const bool preferredNativeHit =
                                !nativeSyntheticFocus_ && nativePreferredHwnd_ &&
                                NativeTopLevelAt(mouseX_, mouseY_) == nativePreferredHwnd_;
                            if (syntheticHit && !preferredNativeHit) {
                                nativeSyntheticFocus_ = true;
                                nativeFocusHwnd_ = nullptr;
                                nativePreferredHwnd_ = nullptr;
                                nativeHybridPointerCaptured_ = true;
                                OnMouseDown(mouseX_, mouseY_);
                                break;
                            }
                        } else {
                            if (nativeShellPointerCaptured_) {
                                nativeShellPointerCaptured_ = false;
                                break;
                            }
                            if (nativeHybridPointerCaptured_) {
                                OnMouseUp(mouseX_, mouseY_);
                                nativeHybridPointerCaptured_ = false;
                                break;
                            }
                        }
                    }
                    NativeMouseButton(cmd, mouseX_, mouseY_);
                    break;

                case hi5::InputCmdType::MouseWheel: {
                    BackstageWindow* syntheticHit = HybridSyntheticWindowAt(mouseX_, mouseY_);
                    const bool preferredNativeHit =
                        !nativeSyntheticFocus_ && nativePreferredHwnd_ &&
                        NativeTopLevelAt(mouseX_, mouseY_) == nativePreferredHwnd_;
                    if (syntheticHit && !preferredNativeHit) {
                        nativeSyntheticFocus_ = true;
                        nativeFocusHwnd_ = nullptr;
                        nativePreferredHwnd_ = nullptr;
                        OnMouseWheel(cmd.mouseWheel.deltaY);
                    } else {
                        NativeMouseWheel(cmd.mouseWheel.deltaY, mouseX_, mouseY_);
                    }
                    break;
                }

                case hi5::InputCmdType::KeyEvent:
                    if (nativeSyntheticFocus_) OnKey(cmd);
                    else NativeKey(cmd);
                    break;

                case hi5::InputCmdType::PasteText:
                case hi5::InputCmdType::ClipboardPaste: {
                    std::string text;
                    if (pipe.ReadClipboard(cmd.clipboard.offsetInClip, cmd.clipboard.length, text)) {
                        if (nativeSyntheticFocus_) OnTextInput(Utf8ToWide(text));
                        else NativeText(Utf8ToWide(text));
                    }
                    break;
                }

                case hi5::InputCmdType::Shortcut:
                    HandleNativeShortcut(cmd);
                    break;

                default:
                    visualDirty = false;
                    break;
                }

                if (visualDirty) ++handled;
            }
            return handled;
        }

        bool LaunchOnPrivateDesktop(const std::wstring& exe, const std::wstring& args, DWORD& outPid, HANDLE& outProcess) {
            outPid = 0;
            outProcess = nullptr;
            if (!EnsurePrivateDesktop()) return false;

            std::wstring cmd;
            if (!exe.empty()) {
                cmd = L"\"" + exe + L"\"";
                if (!args.empty()) cmd += L" " + args;
            }
            if (cmd.empty()) return false;

            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.lpDesktop = const_cast<LPWSTR>(privateDesktopName_.c_str());
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOW;

            PROCESS_INFORMATION pi{};
            DWORD flags = CREATE_NEW_PROCESS_GROUP;
            BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
            if (!ok) {
                LogWarn("[backstage] experimental native launch failed exe=" + WideToUtf8(exe) + " err=" + std::to_string(GetLastError()));
                return false;
            }
            LogInfo("[backstage] experimental native launched on private desktop exe=" + WideToUtf8(exe) + " args=" + WideToUtf8(args) + " pid=" + std::to_string(pi.dwProcessId));
            outPid = pi.dwProcessId;
            outProcess = pi.hProcess;
            CloseHandle(pi.hThread);
            return true;
        }


        struct EnumNativeCtx { DWORD pid = 0; HWND hwnd = nullptr; };

        static BOOL CALLBACK EnumNativeWindowProc(HWND hwnd, LPARAM lparam) {
            auto* ctx = reinterpret_cast<EnumNativeCtx*>(lparam);
            if (!ctx || !hwnd || !IsWindow(hwnd)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (ctx->pid != 0 && pid != ctx->pid) return TRUE;
            wchar_t cls[128]{};
            GetClassNameW(hwnd, cls, 128);
            if (_wcsicmp(cls, L"ConsoleWindowClass") == 0) return TRUE;
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if ((style & WS_DISABLED) != 0) return TRUE;
            RECT r{};
            GetWindowRect(hwnd, &r);
            if ((r.right - r.left) < 100 || (r.bottom - r.top) < 70) return TRUE;
            ctx->hwnd = hwnd;
            return FALSE;
        }

        static std::string PtrToHex(uintptr_t value) {
            std::ostringstream oss;
            oss << std::hex << value;
            return oss.str();
        }

        static int PositiveDim(LONG value, int minimum = 1) {
            return std::max(minimum, static_cast<int>(value));
        }

        void RefreshNativeHwnd(BackstageWindow& win) {
            if (win.kind != WindowKind::NativeApp || win.nativePid == 0 || !privateDesktop_) return;
            if (win.nativeHwnd && IsWindow(win.nativeHwnd)) return;
            EnumNativeCtx ctx{};
            ctx.pid = win.nativePid;
            EnumDesktopWindows(privateDesktop_, EnumNativeWindowProc, reinterpret_cast<LPARAM>(&ctx));
            if (ctx.hwnd) {
                win.nativeHwnd = ctx.hwnd;
                win.nativePlaced = false;
                LogInfo("[backstage] native hwnd detected title=" + WideToUtf8(win.title) +
                    " pid=" + std::to_string(win.nativePid) +
                    " hwnd=0x" + PtrToHex(reinterpret_cast<uintptr_t>(win.nativeHwnd)));
            }
        }

        void PlaceNativeWindow(BackstageWindow& win, const RECT& content) {
            if (!win.nativeHwnd || !IsWindow(win.nativeHwnd)) return;
            const int ww = PositiveDim(content.right - content.left, 320);
            const int wh = PositiveDim(content.bottom - content.top, 240);
            ShowWindow(win.nativeHwnd, SW_SHOWNORMAL);
            MoveWindow(win.nativeHwnd, 0, 0, ww, wh, TRUE);
            SetWindowPos(win.nativeHwnd, HWND_TOP, 0, 0, ww, wh, SWP_SHOWWINDOW);
            RedrawWindow(win.nativeHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            win.nativePlaced = true;
        }

        bool CaptureNativeWindowIntoDc(BackstageWindow& win, HDC targetDc, const RECT& content) {
            RefreshNativeHwnd(win);
            if (!win.nativeHwnd || !IsWindow(win.nativeHwnd)) return false;
            PlaceNativeWindow(win, content);

            RECT wr{};
            GetWindowRect(win.nativeHwnd, &wr);
            const int srcW = PositiveDim(wr.right - wr.left, PositiveDim(content.right - content.left, 320));
            const int srcH = PositiveDim(wr.bottom - wr.top, PositiveDim(content.bottom - content.top, 240));

            HDC winDc = CreateCompatibleDC(targetDc);
            if (!winDc) return false;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = srcW;
            bi.bmiHeader.biHeight = -srcH;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* captureBits = nullptr;
            HBITMAP bmp = CreateDIBSection(winDc, &bi, DIB_RGB_COLORS, &captureBits, nullptr, 0);
            if (!bmp || !captureBits) {
                if (bmp) DeleteObject(bmp);
                DeleteDC(winDc);
                return false;
            }
            HGDIOBJ old = SelectObject(winDc, bmp);
            FillRectColor(winDc, 0, 0, srcW, srcH, RGB(250, 250, 250));
            BOOL ok = PrintWindow(win.nativeHwnd, winDc, 0x00000002);
            if (!ok) ok = PrintWindow(win.nativeHwnd, winDc, 0);
            if (!ok) {
                HDC realDc = GetWindowDC(win.nativeHwnd);
                if (realDc) {
                    ok = BitBlt(winDc, 0, 0, srcW, srcH, realDc, 0, 0, SRCCOPY | CAPTUREBLT);
                    ReleaseDC(win.nativeHwnd, realDc);
                }
            }
            if (ok) {
                const int dstW = PositiveDim(content.right - content.left, 320);
                const int dstH = PositiveDim(content.bottom - content.top, 240);
                StretchBlt(targetDc, content.left, content.top, dstW, dstH, winDc, 0, 0, srcW, srcH, SRCCOPY);
            }
            SelectObject(winDc, old);
            DeleteObject(bmp);
            DeleteDC(winDc);
            return ok != FALSE;
        }

        LPARAM NativeClientLParam(BackstageWindow& win, const RECT& content, int x, int y) {
            POINT p{ x - content.left, y - content.top };
            RECT wr{};
            GetWindowRect(win.nativeHwnd, &wr);
            const int srcW = PositiveDim(wr.right - wr.left, PositiveDim(content.right - content.left, 320));
            const int srcH = PositiveDim(wr.bottom - wr.top, PositiveDim(content.bottom - content.top, 240));
            const int dstW = PositiveDim(content.right - content.left, 320);
            const int dstH = PositiveDim(content.bottom - content.top, 240);
            p.x = std::max(0, std::min(srcW - 1, static_cast<int>(p.x * srcW / dstW)));
            p.y = std::max(0, std::min(srcH - 1, static_cast<int>(p.y * srcH / dstH)));
            return MAKELPARAM(static_cast<SHORT>(p.x), static_cast<SHORT>(p.y));
        }

        bool ForwardMouseToNativeApp(BackstageWindow& win, UINT msg, WPARAM wp, int x, int y) {
            RECT c = ContentRect(win);
            if (!win.nativeHwnd || !IsWindow(win.nativeHwnd) || !PtInRect(&c, POINT{ x, y })) return false;
            const LPARAM lp = NativeClientLParam(win, c, x, y);
            PostMessageW(win.nativeHwnd, WM_MOUSEMOVE, 0, lp);
            PostMessageW(win.nativeHwnd, msg, wp, lp);
            return true;
        }

        bool ForwardMouseWheelToNativeApp(BackstageWindow& win, int deltaY, int x, int y) {
            RECT c = ContentRect(win);
            if (!win.nativeHwnd || !IsWindow(win.nativeHwnd) || !PtInRect(&c, POINT{ x, y })) return false;
            PostMessageW(win.nativeHwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<SHORT>(deltaY)), MAKELPARAM(static_cast<SHORT>(x), static_cast<SHORT>(y)));
            return true;
        }

        bool ForwardKeyToNativeApp(BackstageWindow& win, const hi5::InputCmd& cmd) {
            if (!win.nativeHwnd || !IsWindow(win.nativeHwnd)) return false;
            const UINT msg = cmd.key.down ? WM_KEYDOWN : WM_KEYUP;
            const WPARAM wp = static_cast<WPARAM>(cmd.key.vk);
            const LPARAM lp = 1 | (static_cast<LPARAM>(cmd.key.scanCode) << 16) |
                (cmd.key.isExtended ? (1LL << 24) : 0) |
                (!cmd.key.down ? ((1LL << 30) | (1LL << 31)) : 0);
            PostMessageW(win.nativeHwnd, msg, wp, lp);
            return true;
        }

        void StopNativeApp(BackstageWindow& win) {
            win.nativeHwnd = nullptr;
            win.nativePid = 0;
            if (win.nativeProcess) {
                TerminateProcess(win.nativeProcess, 0);
                WaitForSingleObject(win.nativeProcess, 500);
                CloseHandle(win.nativeProcess);
                win.nativeProcess = nullptr;
            }
        }

        int VisibleRows(const RECT& c, int rowTop, int rowH) const {
            return std::max(1, static_cast<int>((c.bottom - rowTop) / rowH));
        }

        void ClampScroll(BackstageWindow& win) {
            int total = 0;
            int visible = 1;
            RECT c = ContentRect(win);
            if (win.kind == WindowKind::Services) { total = static_cast<int>(win.services.size()); visible = VisibleRows(c, WindowRowTop(win, c), WindowRowH(win)); }
            else if (win.kind == WindowKind::Files) { total = static_cast<int>(win.files.size()); visible = VisibleRows(c, WindowRowTop(win, c), WindowRowH(win)); }
            else if (win.kind == WindowKind::Processes || win.kind == WindowKind::Apps || win.kind == WindowKind::SystemInfo || win.kind == WindowKind::Events || win.kind == WindowKind::Updates || win.kind == WindowKind::Registry || win.kind == WindowKind::Devices || win.kind == WindowKind::Disks) { total = static_cast<int>(win.lines.size()); visible = VisibleRows(c, WindowRowTop(win, c), WindowRowH(win)); }
            const int maxScroll = std::max(0, total - visible);
            win.scroll = std::max(0, std::min(win.scroll, maxScroll));
        }

        void DrawScrollbar(HDC dc, const RECT& c, int total, int visible, int scroll) {
            if (total <= visible) return;
            const int trackX = c.right - 12;
            const int trackTop = c.top + 48;
            const int trackH = std::max(24, static_cast<int>(c.bottom - trackTop - 4));
            FillRectColor(dc, trackX, trackTop, 8, trackH, RGB(226, 232, 240));
            const int thumbH = std::max(20, trackH * visible / std::max(1, total));
            const int maxScroll = std::max(1, total - visible);
            const int thumbY = trackTop + (trackH - thumbH) * std::max(0, std::min(scroll, maxScroll)) / maxScroll;
            FillRectColor(dc, trackX, thumbY, 8, thumbH, RGB(126, 145, 170));
        }

        std::wstring TerminalCommandLine(const BackstageWindow& win) const {
            if (win.terminalMode == L"powershell") return L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass";
            return L"cmd.exe";
        }

        void StartTerminal(BackstageWindow& win) {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
            if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0)) {
                win.terminalLines.push_back(L"Failed to create terminal pipes.");
                return;
            }
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

            std::wstring cmd = TerminalCommandLine(win);
            std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
            mutableCmd.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdInput = inRead;
            si.hStdOutput = outWrite;
            si.hStdError = outWrite;
            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(inRead);
            CloseHandle(outWrite);
            if (!ok) {
                CloseHandle(outRead); CloseHandle(inWrite);
                win.terminalLines.push_back(L"Failed to start shell. Error " + std::to_wstring(GetLastError()));
                return;
            }
            win.process = pi.hProcess;
            CloseHandle(pi.hThread);
            win.inWrite = inWrite;
            win.terminalStop.store(false);
            BackstageWindow* ptr = &win;
            win.reader = std::thread([this, ptr, outRead]() {
                char buf[4096];
                while (!ptr->terminalStop.load()) {
                    DWORD got = 0;
                    BOOL okRead = ReadFile(outRead, buf, sizeof(buf), &got, nullptr);
                    if (!okRead || got == 0) break;
                    std::wstring text = BytesToWideOem(std::string(buf, buf + got));
                    AppendTerminalOutput(*ptr, text);
                }
                CloseHandle(outRead);
                });
            LogInfo("[backstage] PASS20L terminal started title=" + WideToUtf8(win.title));
        }

        void StopTerminal(BackstageWindow& win) {
            win.terminalStop.store(true);
            if (win.inWrite) { CloseHandle(win.inWrite); win.inWrite = nullptr; }
            if (win.process) {
                TerminateProcess(win.process, 0);
                WaitForSingleObject(win.process, 500);
                CloseHandle(win.process);
                win.process = nullptr;
            }
            if (win.reader.joinable()) win.reader.join();
        }

        void AppendTerminalOutput(BackstageWindow& win, const std::wstring& text) {
            std::lock_guard<std::mutex> lock(win.terminalMu);
            std::wstring current;
            for (wchar_t ch : text) {
                if (ch == L'\r') continue;
                if (ch == L'\n') {
                    win.terminalLines.push_back(current);
                    current.clear();
                }
                else {
                    current.push_back(ch);
                }
            }
            if (!current.empty()) win.terminalLines.push_back(current);
            if (win.terminalLines.size() > 800) win.terminalLines.erase(win.terminalLines.begin(), win.terminalLines.begin() + static_cast<long long>(win.terminalLines.size() - 800));
        }

        void SendTerminalLine(BackstageWindow& win) {
            std::wstring line = win.terminalInput;
            win.terminalInput.clear();
            if (!win.inWrite) return;
            std::string data = WideToUtf8(line + L"\r\n");
            DWORD written = 0;
            WriteFile(win.inWrite, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        }

        void FillRectColor(HDC dc, int x, int y, int w, int h, COLORREF c) {
            HBRUSH b = CreateSolidBrush(c);
            RECT r{ x, y, x + w, y + h };
            FillRect(dc, &r, b);
            DeleteObject(b);
        }

        void RoundRectColor(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF border, int radius = 16) {
            HBRUSH b = CreateSolidBrush(fill);
            HPEN p = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ oldB = SelectObject(dc, b);
            HGDIOBJ oldP = SelectObject(dc, p);
            RoundRect(dc, x, y, x + w, y + h, radius, radius);
            SelectObject(dc, oldB); SelectObject(dc, oldP);
            DeleteObject(b); DeleteObject(p);
        }

        void Text(HDC dc, int x, int y, const std::wstring& s, int size, COLORREF color, bool bold = false) {
            HFONT font = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, font);
            SetTextColor(dc, color);
            SetBkMode(dc, TRANSPARENT);
            TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
            SelectObject(dc, old);
            DeleteObject(font);
        }

        void TextClipped(HDC dc, const RECT& rc, const std::wstring& s, int size, COLORREF color, bool bold = false, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
            HFONT font = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, font);
            SetTextColor(dc, color);
            SetBkMode(dc, TRANSPARENT);
            RECT copy = rc;
            DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &copy, flags);
            SelectObject(dc, old);
            DeleteObject(font);
        }

        void DrawIconFromFile(HDC dc, const std::wstring& path, int x, int y, int size) {
            if (!path.empty()) {
                SHFILEINFOW sfi{};
                UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
                if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags) && sfi.hIcon) {
                    DrawIconEx(dc, x, y, sfi.hIcon, size, size, 0, nullptr, DI_NORMAL);
                    DestroyIcon(sfi.hIcon);
                    return;
                }
            }
            RoundRectColor(dc, x, y, size, size, RGB(232, 242, 255), RGB(120, 170, 230), 8);
            HPEN pen1 = CreatePen(PS_SOLID, 2, RGB(44, 112, 204));
            HBRUSH brush1 = CreateSolidBrush(RGB(70, 145, 230));
            HGDIOBJ oldP = SelectObject(dc, pen1); HGDIOBJ oldB = SelectObject(dc, brush1);
            RoundRect(dc, x + size / 5, y + size / 5, x + size - size / 5, y + size - size / 5, 6, 6);
            SelectObject(dc, oldP); SelectObject(dc, oldB); DeleteObject(pen1); DeleteObject(brush1);
        }

        void DrawStartIcon(HDC dc, int x, int y, int s) {
            const COLORREF c = RGB(0, 103, 192);
            FillRectColor(dc, x, y, s / 2 - 1, s / 2 - 1, c);
            FillRectColor(dc, x + s / 2 + 1, y, s / 2 - 1, s / 2 - 1, c);
            FillRectColor(dc, x, y + s / 2 + 1, s / 2 - 1, s / 2 - 1, c);
            FillRectColor(dc, x + s / 2 + 1, y + s / 2 + 1, s / 2 - 1, s / 2 - 1, c);
        }

        void DrawWallpaper(HDC dc) {
            // Windows 11 inspired blue glass wallpaper, drawn procedurally so we do
            // not ship Microsoft wallpaper assets.
            for (int y = 0; y < h_; ++y) {
                const int r = 26 + (y * 22 / std::max(1, h_));
                const int g = 82 + (y * 48 / std::max(1, h_));
                const int b = 152 + (y * 64 / std::max(1, h_));
                FillRectColor(dc, 0, y, w_, 1, RGB(r, g, b));
            }

            HPEN glow1 = CreatePen(PS_SOLID, 3, RGB(115, 190, 245));
            HPEN glow2 = CreatePen(PS_SOLID, 2, RGB(55, 120, 210));
            HGDIOBJ oldP = SelectObject(dc, glow1);
            Arc(dc, -160, h_ / 3, w_ / 2 + 80, h_ + 280, 0, 0, 0, 0);
            SelectObject(dc, glow2);
            Arc(dc, w_ / 3, -160, w_ + 300, h_ + 80, 0, 0, 0, 0);
            Arc(dc, w_ / 2, h_ / 6, w_ + 220, h_ + 260, 0, 0, 0, 0);
            SelectObject(dc, oldP);
            DeleteObject(glow1);
            DeleteObject(glow2);

            // Subtle workstation watermark.
            Text(dc, 34, 24, L"Hi5Central Backstage", 18, RGB(218, 232, 250), true);
            Text(dc, 34, 48, L"SYSTEM maintenance desktop", 12, RGB(166, 194, 226));
        }

        void DrawDesktopIcons(HDC dc) {
            const auto& tools = Tools();
            for (size_t i = 0; i < tools.size(); ++i) {
                RECT r = DesktopIconRect(i);
                if (static_cast<int>(i) == selectedDesktopIcon_) {
                    RoundRectColor(dc, r.left - 4, r.top - 4, r.right - r.left + 8, r.bottom - r.top + 8, RGB(225, 239, 255), RGB(122, 178, 235), 10);
                }
                DrawIconFromFile(dc, tools[i].iconPath ? tools[i].iconPath : L"", r.left + 25, r.top + 4, 38);
                RECT label{ r.left + 2, r.top + 46, r.right - 2, r.bottom };
                TextClipped(dc, label, tools[i].title ? tools[i].title : L"Tool", 13, RGB(255, 255, 255), true, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
            }
        }

        void DrawStartMenu(HDC dc) {
            if (!startMenuOpen_) return;
            RECT m = StartMenuRect();
            RoundRectColor(dc, m.left, m.top, m.right - m.left, m.bottom - m.top, RGB(245, 248, 252), RGB(214, 222, 232), 20);
            Text(dc, m.left + 30, m.top + 80, L"Pinned maintenance tools", 17, RGB(30, 42, 60), true);

            RECT search = StartSearchBoxRect();
            RoundRectColor(dc, search.left, search.top, search.right - search.left, search.bottom - search.top, RGB(255, 255, 255), RGB(190, 210, 235), 12);
            std::wstring q = startSearch_.empty() ? L"Search tools" : startSearch_;
            Text(dc, search.left + 16, search.top + 10, q, 15, startSearch_.empty() ? RGB(115, 125, 140) : RGB(25, 35, 50));

            auto matches = MatchingSearchResults();
            for (size_t v = 0; v < matches.size() && v < 12; ++v) {
                const auto& rmeta = matches[v];
                RECT r = StartTileRect(v);
                RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, RGB(255, 255, 255), RGB(226, 232, 240), 14);
                DrawIconFromFile(dc, rmeta.iconPath, r.left + 14, r.top + 14, 34);
                TextClipped(dc, RECT{ r.left + 58, r.top + 8, r.right - 10, r.top + 31 }, rmeta.title, 14, RGB(30, 40, 60), true);
                TextClipped(dc, RECT{ r.left + 58, r.top + 32, r.right - 10, r.bottom - 6 }, rmeta.sub, 12, RGB(90, 100, 112));
            }
        }



        const ToolSpec* FindToolSpec(const std::wstring& id) const {
            for (const auto& t : Tools()) if (id == (t.id ? t.id : L"")) return &t;
            return nullptr;
        }

        size_t PinnedCount() const {
            return std::min<size_t>(7, Tools().size());
        }

        bool IsPinnedToolId(const std::wstring& id) const {
            const auto& tools = Tools();
            const size_t pinCount = PinnedCount();
            for (size_t i = 0; i < pinCount; ++i) {
                if (id == (tools[i].id ? tools[i].id : L"")) return true;
            }
            return false;
        }

        const BackstageWindow* FindOpenToolConst(const std::wstring& id) const {
            for (const auto& w : windows_) {
                if (!w.closed && w.toolId == id) return &w;
            }
            return nullptr;
        }

        void DrawTaskbarPreview(HDC dc, const BackstageWindow& win, const RECT& iconRect) {
            const int pw = 240;
            const int ph = 142;
            int x = iconRect.left + (iconRect.right - iconRect.left) / 2 - pw / 2;
            x = std::max(8, std::min(static_cast<int>(w_ - pw - 8), x));
            const int y = h_ - taskbarH_ - ph - 10;
            RoundRectColor(dc, x, y, pw, ph, RGB(252, 254, 255), RGB(194, 210, 230), 12);
            TextClipped(dc, RECT{ x + 12, y + 8, x + pw - 12, y + 30 }, win.title, 13, RGB(28, 40, 58), true);
            RECT body{ x + 12, y + 34, x + pw - 12, y + ph - 12 };
            RoundRectColor(dc, body.left, body.top, body.right - body.left, body.bottom - body.top, RGB(240, 245, 252), RGB(220, 228, 238), 8);
            std::wstring summary = win.minimized ? L"Minimized" : L"Open";
            if (win.kind == WindowKind::Terminal) summary += L" terminal";
            else if (win.kind == WindowKind::Files) summary += L" file browser";
            else if (win.kind == WindowKind::Services) summary += L" services";
            TextClipped(dc, RECT{ body.left + 10, body.top + 10, body.right - 10, body.bottom - 10 }, summary, 12, RGB(60, 74, 92), false, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }

        void DrawTaskbar(HDC dc) {
            const int top = h_ - taskbarH_;
            FillRectColor(dc, 0, top, w_, taskbarH_, RGB(242, 246, 252));
            HPEN p = CreatePen(PS_SOLID, 1, RGB(214, 222, 232));
            HGDIOBJ old = SelectObject(dc, p);
            MoveToEx(dc, 0, top, nullptr); LineTo(dc, w_, top);
            SelectObject(dc, old); DeleteObject(p);

            RECT start = StartRect();
            RoundRectColor(dc, start.left, start.top, start.right - start.left, start.bottom - start.top, startMenuOpen_ ? RGB(222, 236, 252) : RGB(248, 251, 255), RGB(214, 224, 238), 10);
            DrawStartIcon(dc, start.left + 12, start.top + 10, 17);

            RECT search = SearchRect();
            RoundRectColor(dc, search.left, search.top, search.right - search.left, search.bottom - search.top, RGB(255, 255, 255), RGB(214, 224, 238), 12);
            Text(dc, search.left + 15, search.top + 9, L"Search", 14, RGB(90, 100, 115));

            const auto& tools = Tools();
            const size_t pinCount = PinnedCount();
            for (size_t i = 0; i < pinCount; ++i) {
                RECT r = PinnedIconRect(i);
                const std::wstring id = tools[i].id ? tools[i].id : L"";
                const BackstageWindow* running = FindOpenToolConst(id);
                const bool active = running && running->id == activeWindowId_ && !running->minimized;
                const bool open = running != nullptr;
                COLORREF fill = active ? RGB(221, 235, 252) : (open ? RGB(245, 249, 255) : RGB(248, 251, 255));
                RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, fill, RGB(222, 229, 238), 10);
                DrawIconFromFile(dc, tools[i].iconPath ? tools[i].iconPath : L"", r.left + 7, r.top + 7, 22);
                if (open) FillRectColor(dc, r.left + 11, r.bottom - 3, r.right - r.left - 22, 2, active ? RGB(0, 103, 192) : RGB(120, 145, 175));
                if (running && mouseX_ >= r.left && mouseX_ < r.right && mouseY_ >= r.top && mouseY_ < r.bottom) {
                    DrawTaskbarPreview(dc, *running, r);
                }
            }

            auto unpinned = UnpinnedTaskbarWindowsConst();
            const size_t slots = TaskbarVisibleSlots();
            const size_t maxPage = unpinned.empty() ? 0 : ((unpinned.size() - 1) / slots);
            if (taskbarPage_ > maxPage) const_cast<BackstageRenderer*>(this)->taskbarPage_ = maxPage;

            if (unpinned.size() > slots) {
                RECT prev = TaskbarPrevPageRect();
                RECT next = TaskbarNextPageRect();
                RoundRectColor(dc, prev.left, prev.top, prev.right - prev.left, prev.bottom - prev.top, taskbarPage_ > 0 ? RGB(255, 255, 255) : RGB(232, 236, 242), RGB(216, 224, 235), 8);
                TextClipped(dc, prev, L"‹", 18, taskbarPage_ > 0 ? RGB(40, 55, 75) : RGB(150, 158, 170), true, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RoundRectColor(dc, next.left, next.top, next.right - next.left, next.bottom - next.top, taskbarPage_ < maxPage ? RGB(255, 255, 255) : RGB(232, 236, 242), RGB(216, 224, 235), 8);
                TextClipped(dc, next, L"›", 18, taskbarPage_ < maxPage ? RGB(40, 55, 75) : RGB(150, 158, 170), true, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            const size_t startIdx = taskbarPage_ * slots;
            for (size_t local = 0; local < slots && startIdx + local < unpinned.size(); ++local) {
                const BackstageWindow& win = *unpinned[startIdx + local];
                RECT r = TaskbarWindowButtonRect(local);
                COLORREF fill = (win.id == activeWindowId_ && !win.minimized) ? RGB(221, 235, 252) : RGB(255, 255, 255);
                RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, fill, RGB(216, 224, 235), 10);
                const ToolSpec* spec = FindToolSpec(win.toolId);
                const std::wstring iconPath = spec && spec->iconPath ? spec->iconPath : (win.nativeExe.empty() ? L"C:\\Windows\\System32\\shell32.dll" : win.nativeExe);
                DrawIconFromFile(dc, iconPath, r.left + 7, r.top + 6, 24);
                if (win.id == activeWindowId_ && !win.minimized) FillRectColor(dc, r.left + 10, r.bottom - 3, r.right - r.left - 20, 2, RGB(0, 103, 192));
                if (mouseX_ >= r.left && mouseX_ < r.right && mouseY_ >= r.top && mouseY_ < r.bottom) DrawTaskbarPreview(dc, win, r);
            }

            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t clock[64]{};
            swprintf_s(clock, L"%02u:%02u\n%02u/%02u/%04u", st.wHour, st.wMinute, st.wDay, st.wMonth, st.wYear);
            TextClipped(dc, RECT{ w_ - 118, top + 6, w_ - 12, top + 45 }, clock, 12, RGB(32, 45, 62), false, DT_RIGHT | DT_VCENTER | DT_WORDBREAK);
        }

        void DrawWindowFrame(HDC dc, const BackstageWindow& win) {
            if (win.minimized) return;
            const bool active = win.id == activeWindowId_;
            const int ww = win.rect.right - win.rect.left;
            const int wh = win.rect.bottom - win.rect.top;

            // Reference-style Windows 11 frame: a soft offset shadow, restrained
            // active border, compact title bar and clean white application surface.
            if (!win.maximized) {
                RoundRectColor(dc, win.rect.left + 6, win.rect.top + 7, ww, wh,
                    RGB(33, 52, 77), RGB(33, 52, 77), 12);
            }
            RoundRectColor(dc, win.rect.left, win.rect.top, ww, wh,
                RGB(252, 253, 255), active ? RGB(147, 181, 219) : RGB(186, 198, 214), 12);

            RECT title = TitleBarRect(win);
            FillRectColor(dc, title.left + 1, title.top + 1, title.right - title.left - 2, title.bottom - title.top - 1,
                active ? RGB(249, 251, 254) : RGB(246, 248, 251));
            FillRectColor(dc, title.left + 1, title.bottom - 1, title.right - title.left - 2, 1, RGB(224, 229, 236));

            const ToolSpec* spec = FindToolSpec(win.toolId);
            if (spec) DrawIconFromFile(dc, spec->iconPath ? spec->iconPath : L"", title.left + 12, title.top + 8, 22);
            TextClipped(dc, RECT{ title.left + 42, title.top, title.right - 146, title.bottom },
                win.title, 14, RGB(28, 38, 52), true, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawCaptionButtons(dc, win);
        }

        void DrawCaptionButtons(HDC dc, const BackstageWindow& win) {
            RECT minR = MinRect(win), maxR = MaxRect(win), closeR = CloseRect(win);
            POINT mouse{ mouseX_, mouseY_ };
            const bool hoverMin = PtInRect(&minR, mouse) != 0;
            const bool hoverMax = PtInRect(&maxR, mouse) != 0;
            const bool hoverClose = PtInRect(&closeR, mouse) != 0;

            FillRectColor(dc, minR.left, minR.top, minR.right - minR.left, minR.bottom - minR.top,
                hoverMin ? RGB(232, 236, 242) : RGB(249, 251, 254));
            FillRectColor(dc, maxR.left, maxR.top, maxR.right - maxR.left, maxR.bottom - maxR.top,
                hoverMax ? RGB(232, 236, 242) : RGB(249, 251, 254));
            FillRectColor(dc, closeR.left, closeR.top, closeR.right - closeR.left, closeR.bottom - closeR.top,
                hoverClose ? RGB(196, 43, 28) : RGB(249, 251, 254));

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(62, 70, 82));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            const int my = (minR.top + minR.bottom) / 2 + 5;
            MoveToEx(dc, minR.left + 16, my, nullptr);
            LineTo(dc, minR.right - 16, my);
            RECT sq{ maxR.left + 16, maxR.top + 11, maxR.right - 16, maxR.bottom - 11 };
            if (win.maximized) {
                Rectangle(dc, sq.left + 3, sq.top, sq.right, sq.bottom - 3);
                Rectangle(dc, sq.left, sq.top + 3, sq.right - 3, sq.bottom);
            } else {
                Rectangle(dc, sq.left, sq.top, sq.right, sq.bottom);
            }
            SelectObject(dc, oldPen);
            DeleteObject(pen);

            HPEN closePen = CreatePen(PS_SOLID, 1, hoverClose ? RGB(255, 255, 255) : RGB(62, 70, 82));
            oldPen = SelectObject(dc, closePen);
            MoveToEx(dc, closeR.left + 16, closeR.top + 12, nullptr);
            LineTo(dc, closeR.right - 16, closeR.bottom - 12);
            MoveToEx(dc, closeR.right - 16, closeR.top + 12, nullptr);
            LineTo(dc, closeR.left + 16, closeR.bottom - 12);
            SelectObject(dc, oldPen);
            DeleteObject(closePen);
        }

        void DrawWindowContent(HDC dc, const BackstageWindow& win) {
            if (win.minimized) return;
            RECT c = ContentRect(win);
            if (win.kind == WindowKind::Services) DrawServices(dc, win, c);
            else if (win.kind == WindowKind::Files) DrawFiles(dc, win, c);
            else if (win.kind == WindowKind::Processes || win.kind == WindowKind::Apps || win.kind == WindowKind::SystemInfo || win.kind == WindowKind::Events || win.kind == WindowKind::Updates || win.kind == WindowKind::Registry || win.kind == WindowKind::Devices || win.kind == WindowKind::Disks) DrawLines(dc, win, c);
            else if (win.kind == WindowKind::Terminal) DrawTerminal(dc, win, c);
            else if (win.kind == WindowKind::Notepad) DrawNotepad(dc, win, c);
        }

        void DrawHeader(HDC dc, const RECT& c, const std::wstring&, const std::wstring& right = L"") {
            // Compact command strip, closer to current Windows utilities than a
            // second card-like header inside every managed tool window.
            FillRectColor(dc, c.left, c.top, c.right - c.left, 42, RGB(252, 253, 255));
            FillRectColor(dc, c.left, c.top + 41, c.right - c.left, 1, RGB(226, 231, 237));
            if (!right.empty()) {
                TextClipped(dc, RECT{ c.right - 230, c.top, c.right - 12, c.top + 41 },
                    right, 11, RGB(92, 103, 118), false, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        void DrawActionButton(HDC dc, const RECT& c, int index, const std::wstring& label) {
            RECT r = ActionButtonRect(c, index);
            POINT mouse{ mouseX_, mouseY_ };
            const bool hover = PtInRect(&r, mouse) != 0;
            RoundRectColor(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                hover ? RGB(235, 242, 251) : RGB(252, 253, 255),
                hover ? RGB(178, 199, 225) : RGB(221, 228, 237), 7);
            TextClipped(dc, r, label, 11, RGB(35, 53, 76), true, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }



        void DrawNativeApp(HDC dc, BackstageWindow& win, const RECT& c) {
            RoundRectColor(dc, c.left, c.top, c.right - c.left, c.bottom - c.top, RGB(248, 250, 253), RGB(218, 226, 238), 10);
            if (!CaptureNativeWindowIntoDc(win, dc, c)) {
                DrawHeader(dc, c, win.title, L"Experimental native app");
                int y = c.top + 54;
                for (const auto& line : win.lines) {
                    TextClipped(dc, RECT{ c.left + 14, y, c.right - 14, y + 22 }, line, 12, RGB(55, 70, 92));
                    y += 22;
                }
                if (win.nativePid != 0) {
                    TextClipped(dc, RECT{ c.left + 14, c.bottom - 34, c.right - 14, c.bottom - 10 },
                        L"Process is running on the private Backstage desktop; waiting for a capturable window...",
                        12, RGB(125, 75, 45));
                }
            }
        }

        void DrawServices(HDC dc, const BackstageWindow& win, const RECT& c) {
            DrawHeader(dc, c, L"Services", std::to_wstring(win.services.size()) + L" services");
            DrawActionButton(dc, c, 0, L"Start"); DrawActionButton(dc, c, 1, L"Stop"); DrawActionButton(dc, c, 2, L"Restart"); DrawActionButton(dc, c, 3, L"Refresh");
            const int rowTop = c.top + 62;
            const int rowH = 30;
            FillRectColor(dc, c.left, rowTop - 24, c.right - c.left, 22, RGB(238, 244, 252));
            TextClipped(dc, RECT{ c.left + 14, rowTop - 24, c.right - 340, rowTop - 2 }, L"Display name", 12, RGB(70, 82, 100), true);
            TextClipped(dc, RECT{ c.right - 318, rowTop - 24, c.right - 210, rowTop - 2 }, L"Startup", 12, RGB(70, 82, 100), true, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            TextClipped(dc, RECT{ c.right - 200, rowTop - 24, c.right - 20, rowTop - 2 }, L"State", 12, RGB(70, 82, 100), true, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            const int visible = VisibleRows(c, rowTop, rowH);
            const int start = std::max(0, win.scroll);
            for (int i = 0; i < visible && i + start < static_cast<int>(win.services.size()); ++i) {
                const int rowIndex = i + start;
                const int y = rowTop + i * rowH;
                const ServiceRow& svc = win.services[static_cast<size_t>(rowIndex)];
                COLORREF fill = (rowIndex == win.selected) ? RGB(219, 236, 255) : ((rowIndex % 2) ? RGB(250, 252, 255) : RGB(255, 255, 255));
                FillRectColor(dc, c.left, y, c.right - c.left, rowH, fill);
                TextClipped(dc, RECT{ c.left + 14, y, c.right - 340, y + rowH }, svc.display, 13, RGB(28, 38, 52));
                TextClipped(dc, RECT{ c.right - 318, y, c.right - 210, y + rowH }, svc.startTypeText, 12, RGB(85, 98, 112), false, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                RECT pill{ c.right - 126, y + 6, c.right - 18, y + rowH - 6 };
                const bool running = svc.state == SERVICE_RUNNING;
                RoundRectColor(dc, pill.left, pill.top, pill.right - pill.left, pill.bottom - pill.top,
                    running ? RGB(219, 248, 231) : RGB(246, 239, 232),
                    running ? RGB(144, 220, 176) : RGB(218, 190, 162), 10);
                TextClipped(dc, pill, svc.stateText, 11, running ? RGB(16, 112, 66) : RGB(126, 75, 42), true, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            DrawScrollbar(dc, c, static_cast<int>(win.services.size()), visible, win.scroll);
        }

        void DrawFiles(HDC dc, const BackstageWindow& win, const RECT& c) {
            DrawHeader(dc, c, L"File Explorer", L"Backstage file actions");
            DrawActionButton(dc, c, 0, L"Run"); DrawActionButton(dc, c, 1, L"Delete"); DrawActionButton(dc, c, 2, L"New folder"); DrawActionButton(dc, c, 3, L"Refresh");
            RECT pathBar{ c.left + 12, c.top + 42, c.right - 12, c.top + 70 };
            RoundRectColor(dc, pathBar.left, pathBar.top, pathBar.right - pathBar.left, pathBar.bottom - pathBar.top, RGB(255, 255, 255), RGB(216, 226, 240), 9);
            TextClipped(dc, RECT{ pathBar.left + 12, pathBar.top, pathBar.right - 12, pathBar.bottom }, win.path, 12, RGB(55, 70, 90));
            const int rowTop = c.top + 82;
            const int rowH = 30;
            const int visible = VisibleRows(c, rowTop, rowH);
            const int start = std::max(0, win.scroll);
            for (int i = 0; i < visible && i + start < static_cast<int>(win.files.size()); ++i) {
                const int rowIndex = i + start;
                const int y = rowTop + i * rowH;
                const FileRow& f = win.files[static_cast<size_t>(rowIndex)];
                COLORREF fill = (rowIndex == win.selected) ? RGB(219, 236, 255) : ((rowIndex % 2) ? RGB(250, 252, 255) : RGB(255, 255, 255));
                FillRectColor(dc, c.left, y, c.right - c.left, rowH, fill);
                TextClipped(dc, RECT{ c.left + 14, y, c.left + 42, y + rowH }, f.isDir ? L"[D]" : L"[F]", 12, f.isDir ? RGB(200, 145, 32) : RGB(80, 104, 135), true);
                TextClipped(dc, RECT{ c.left + 48, y, c.right - 170, y + rowH }, f.name, 13, RGB(28, 38, 52));
                if (!f.isDir) TextClipped(dc, RECT{ c.right - 150, y, c.right - 18, y + rowH }, FileSizeText(f.size), 12, RGB(90, 102, 116), false, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            DrawScrollbar(dc, c, static_cast<int>(win.files.size()), visible, win.scroll);
        }

        void DrawLines(HDC dc, const BackstageWindow& win, const RECT& c) {
            DrawHeader(dc, c, win.title, std::to_wstring(win.lines.size()) + L" rows");
            if (win.kind == WindowKind::Processes) { DrawActionButton(dc, c, 0, L"Kill"); DrawActionButton(dc, c, 1, L"Refresh"); }
            if (win.kind == WindowKind::Apps) { DrawActionButton(dc, c, 0, L"Uninstall"); DrawActionButton(dc, c, 1, L"Refresh"); }
            if (win.kind == WindowKind::Updates) { DrawActionButton(dc, c, 0, L"Scan"); DrawActionButton(dc, c, 1, L"Install"); DrawActionButton(dc, c, 2, L"Refresh"); DrawActionButton(dc, c, 3, L"History"); }
            if (win.kind == WindowKind::Events) { DrawActionButton(dc, c, 0, L"Home"); DrawActionButton(dc, c, 1, L"Refresh"); }
            if (win.kind == WindowKind::Sessions || win.kind == WindowKind::Devices || win.kind == WindowKind::Disks) { DrawActionButton(dc, c, 0, L"Refresh"); }
            if (win.kind == WindowKind::Registry) { DrawActionButton(dc, c, 0, L"Home"); DrawActionButton(dc, c, 1, L"New Key"); DrawActionButton(dc, c, 2, L"Delete Key"); DrawActionButton(dc, c, 3, L"Refresh"); }
            const int y0 = c.top + 54;
            const int lineH = 24;
            const int visible = VisibleRows(c, y0, lineH);
            int start = std::max(0, win.scroll);
            for (int i = 0; i < visible && i + start < static_cast<int>(win.lines.size()); ++i) {
                const int rowIndex = i + start;
                const int y = y0 + i * lineH;
                COLORREF fill = (rowIndex == win.selected) ? RGB(219, 236, 255) : ((rowIndex % 2) ? RGB(250, 252, 255) : RGB(255, 255, 255));
                FillRectColor(dc, c.left, y, c.right - c.left, lineH, fill);
                TextClipped(dc, RECT{ c.left + 14, y, c.right - 18, y + lineH }, win.lines[static_cast<size_t>(rowIndex)], 12, RGB(38, 50, 67));
            }
            DrawScrollbar(dc, c, static_cast<int>(win.lines.size()), visible, win.scroll);
        }

        void DrawTerminal(HDC dc, const BackstageWindow& win, const RECT& c) {
            RoundRectColor(dc, c.left, c.top, c.right - c.left, c.bottom - c.top, RGB(12, 16, 24), RGB(45, 60, 82), 10);
            std::vector<std::wstring> lines;
            {
                std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(win.terminalMu));
                lines = win.terminalLines;
            }
            const int lineH = 19;
            const int maxLines = std::max(1, static_cast<int>((c.bottom - c.top - 40) / lineH));
            int start = std::max(0, static_cast<int>(lines.size()) - maxLines);
            int y = c.top + 12;
            for (int i = start; i < static_cast<int>(lines.size()); ++i) {
                TextClipped(dc, RECT{ c.left + 12, y, c.right - 12, y + lineH }, lines[static_cast<size_t>(i)], 12, RGB(220, 235, 245));
                y += lineH;
            }
            TextClipped(dc, RECT{ c.left + 12, c.bottom - 28, c.right - 12, c.bottom - 8 }, L"> " + win.terminalInput + L"_", 13, RGB(125, 220, 145));
        }

        void DrawNotepad(HDC dc, const BackstageWindow& win, const RECT& c) {
            RoundRectColor(dc, c.left, c.top, c.right - c.left, c.bottom - c.top, RGB(255, 255, 252), RGB(225, 228, 235), 10);
            RECT text{ c.left + 14, c.top + 12, c.right - 14, c.bottom - 12 };
            TextClipped(dc, text, win.noteText.empty() ? L"Start typing..." : win.noteText + L"_", 14, win.noteText.empty() ? RGB(120, 126, 135) : RGB(20, 28, 40), false, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }

        void DrawSynthetic() {
            HDC dc = memDc_;
            DrawWallpaper(dc);
            DrawDesktopIcons(dc);
            std::vector<const BackstageWindow*> drawOrder;
            for (const auto& win : windows_) if (!win.closed && !win.minimized) drawOrder.push_back(&win);
            std::sort(drawOrder.begin(), drawOrder.end(), [](const BackstageWindow* a, const BackstageWindow* b) { return a->zOrder < b->zOrder; });
            for (const BackstageWindow* win : drawOrder) {
                DrawWindowFrame(dc, *win);
                DrawWindowContent(dc, *win);
            }
            DrawStartMenu(dc);
            DrawTaskbar(dc);
        }

        void DrawCursor(HDC dc) {
            POINT p{ mouseX_, mouseY_ };
            HPEN white = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN black = CreatePen(PS_SOLID, 1, RGB(10, 10, 10));
            HGDIOBJ old = SelectObject(dc, white);
            MoveToEx(dc, p.x, p.y, nullptr); LineTo(dc, p.x + 13, p.y + 21); LineTo(dc, p.x + 17, p.y + 14); LineTo(dc, p.x + 25, p.y + 14); LineTo(dc, p.x, p.y);
            SelectObject(dc, black);
            MoveToEx(dc, p.x + 1, p.y + 1, nullptr); LineTo(dc, p.x + 13, p.y + 19); LineTo(dc, p.x + 16, p.y + 13); LineTo(dc, p.x + 23, p.y + 13); LineTo(dc, p.x + 1, p.y + 1);
            SelectObject(dc, old);
            DeleteObject(white); DeleteObject(black);
        }

    private:
        enum class DragMode { None, Window, Icon };

        int w_ = 1280;
        int h_ = 720;
        std::string sessionId_;
        HDC memDc_ = nullptr;
        HBITMAP dib_ = nullptr;
        uint8_t* bits_ = nullptr;

        int mouseX_ = 200;
        int mouseY_ = 200;
        bool mouseDown_ = false;
        bool shiftDown_ = false;

        const int taskbarH_ = 48;
        const int titleH_ = 40;

        std::vector<POINT> iconPositions_;
        int selectedDesktopIcon_ = -1;

        std::deque<BackstageWindow> windows_;
        int nextWindowId_ = 1;
        int activeWindowId_ = 0;
        int zCounter_ = 0;

        bool startMenuOpen_ = false;
        bool focusStartSearch_ = false;
        std::wstring startSearch_;
        size_t taskbarPage_ = 0;
        HDESK privateDesktop_ = nullptr;
        std::wstring privateDesktopName_;
        std::wstring privateDesktopFullName_;
        bool nativeModeRequested_ = false;
        bool nativeModeActive_ = false;
        bool nativeWindowInventoryLogged_ = false;
        bool nativeFirstCaptureLogged_ = false;
        size_t nativeVisibleWindowCount_ = 0;
        bool nativeLauncherOpen_ = false;
        bool nativeShellPointerCaptured_ = false;
        std::wstring nativeLauncherFolder_;
        bool nativeRunDialogOpen_ = false;
        std::wstring nativeRunText_;
        std::wstring nativeRunStatus_;
        const int nativeTaskbarH_ = 48;
        bool nativeLeftDown_ = false;
        HWND nativeFocusHwnd_ = nullptr;
        HWND nativePreferredHwnd_ = nullptr;
        bool nativeSyntheticFocus_ = false;
        bool nativeHybridPointerCaptured_ = false;
        std::unordered_map<HWND, NativeWindowCache> nativeWindowCache_;
        HWND nativeNonClientHwnd_ = nullptr;
        LRESULT nativeNonClientHit_ = HTNOWHERE;
        POINT nativeNonClientStartScreen_{};
        RECT nativeNonClientStartRect_{};
        std::unordered_map<HWND, RECT> nativeRestoreRects_;
        std::vector<HANDLE> nativeDesktopProcesses_;

        DragMode dragMode_ = DragMode::None;
        int dragWindowId_ = 0;
        int dragIconIndex_ = -1;
        POINT dragStart_{};
        RECT dragOriginal_{};
        POINT dragIconOriginal_{};
    };

} // namespace

namespace hi5 {

    int RunBackstageHostMain(int argc, char** argv) {
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        Args args = ParseArgs(argc, argv);
        LogInfo("[backstage] start session=" + args.sessionId +
            " shmem=" + args.shmemName +
            " input=" + args.inputPipeName +
            " fps=" + std::to_string(args.fps) +
            " native_desktop=" + std::string(args.nativeDesktop ? "1" : "0"));

        HANDLE stopEvent = nullptr;
        if (!args.stopEventName.empty()) {
            stopEvent = OpenEventA(SYNCHRONIZE, FALSE, args.stopEventName.c_str());
            if (!stopEvent) LogWarn("[backstage] failed to open stop event err=" + std::to_string(GetLastError()));
        }

        ShmemRing shmem;
        if (!shmem.OpenProducer(args.shmemName)) {
            LogError("[backstage] failed to open shmem producer err=" + std::to_string(GetLastError()));
            if (stopEvent) CloseHandle(stopEvent);
            return 1;
        }

        InputPipeReader inputPipe;
        bool inputPipeOk = false;
        if (!args.inputPipeName.empty()) {
            inputPipeOk = inputPipe.Open(args.inputPipeName);
            if (!inputPipeOk) LogWarn("[backstage] failed to open input pipe err=" + std::to_string(GetLastError()));
        }

        BackstageRenderer renderer(args.width, args.height, args.sessionId, args.nativeDesktop);
        LogInfo("[backstage] renderer mode=" + std::string(renderer.NativeModeActive() ? "native-private-desktop" : "synthetic"));
        if (inputPipeOk) renderer.PublishMonitorInfo(inputPipe);

        const int idleFps = EnvInt("HI5_BACKSTAGE_IDLE_FPS", 1, 1, 10);
        const int activeFps = std::max(idleFps, std::min(args.fps, EnvInt("HI5_BACKSTAGE_ACTIVE_FPS", 20, 2, 30)));
        const int motionFps = std::max(activeFps, std::min(args.fps, EnvInt("HI5_BACKSTAGE_MOTION_FPS", 30, 5, 30)));
        const int nativeRefreshFps = std::max(idleFps, std::min(args.fps, EnvInt("HI5_BACKSTAGE_NATIVE_REFRESH_FPS", 5, 1, 15)));
        const int activeHoldMs = EnvInt("HI5_BACKSTAGE_ACTIVE_HOLD_MS", 900, 100, 5000);
        const int monitorPublishMs = EnvInt("HI5_BACKSTAGE_MONITOR_PUBLISH_MS", 1000, 250, 10000);

        auto IntervalForFps = [](int fps) {
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(std::max(1, fps))));
        };

        uint64_t frames = 0;
        uint64_t skippedTicks = 0;
        uint64_t inputEvents = 0;
        bool dirty = true;
        auto now0 = std::chrono::steady_clock::now();
        auto lastInput = now0 - std::chrono::milliseconds(activeHoldMs + 1);
        auto lastRender = now0 - std::chrono::seconds(2);
        auto lastMonitorPublish = now0 - std::chrono::seconds(2);
        auto nextHealth = now0 + std::chrono::seconds(5);

        LogInfo("[backstage] low-cpu mode session=" + args.sessionId +
            " idle_fps=" + std::to_string(idleFps) +
            " active_fps=" + std::to_string(activeFps) +
            " motion_fps=" + std::to_string(motionFps) +
            " active_hold_ms=" + std::to_string(activeHoldMs));

        while (g_running.load()) {
            const auto loopStart = std::chrono::steady_clock::now();
            if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                LogInfo("[backstage] stop event signaled");
                break;
            }

            int handled = 0;
            if (inputPipeOk) {
                handled = renderer.HandleInput(inputPipe);
                if (handled > 0) {
                    inputEvents += static_cast<uint64_t>(handled);
                    dirty = true;
                    lastInput = loopStart;
                }

                if (loopStart - lastMonitorPublish >= std::chrono::milliseconds(monitorPublishMs)) {
                    renderer.PublishMonitorInfo(inputPipe);
                    lastMonitorPublish = loopStart;
                }
            }

            const auto sinceInputMs = std::chrono::duration_cast<std::chrono::milliseconds>(loopStart - lastInput).count();
            const bool activeJob = renderer.HasActiveBackgroundJob();
            const bool nativeRefresh = renderer.HasVisibleNativeWindows();
            const bool active = sinceInputMs <= activeHoldMs || activeJob;
            const bool interacting = active && handled > 0;
            const int targetFps = activeJob
                ? std::max(2, std::min(activeFps, 5))
                : (interacting ? motionFps : (active ? activeFps : (nativeRefresh ? nativeRefreshFps : idleFps)));
            const auto frameInterval = IntervalForFps(targetFps);
            const bool dueForFrame = (loopStart - lastRender) >= frameInterval;

            // Pointer-only movement is handled by the Viewer overlay and does not
            // dirty video. Real native HWNDs still get a bounded idle refresh so
            // progress/dialog changes remain visible without constant PrintWindow.
            if (dueForFrame && (dirty || !active || activeJob || nativeRefresh)) {
                I420Frame frame;
                if (renderer.Render(frame)) {
                    const uint64_t tsNs = static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                    shmem.WriteRawI420Frame(frame, tsNs);
                    ++frames;
                    dirty = false;
                    lastRender = loopStart;
                }
            }
            else {
                ++skippedTicks;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextHealth) {
                LogInfo("[backstage] health session=" + args.sessionId +
                    " frames=" + std::to_string(frames) +
                    " skipped_ticks=" + std::to_string(skippedTicks) +
                    " input_events=" + std::to_string(inputEvents) +
                    " mode=" + std::string(active ? (interacting ? "interaction" : "active") : (nativeRefresh ? "native-refresh" : "idle")) +
                    " target_fps=" + std::to_string(targetFps) +
                    " pass=backstage_lowcpu_1");
                frames = 0;
                skippedTicks = 0;
                inputEvents = 0;
                nextHealth = now + std::chrono::seconds(5);
            }

            const auto elapsed = std::chrono::steady_clock::now() - loopStart;
            const auto tickSleep = std::chrono::milliseconds(active ? 6 : 20);
            if (elapsed < tickSleep) std::this_thread::sleep_for(tickSleep - elapsed);
        }

        if (inputPipeOk) inputPipe.Close();
        shmem.Close();
        if (stopEvent) CloseHandle(stopEvent);
        LogInfo("[backstage] stopped session=" + args.sessionId);
        return 0;
    }

} // namespace hi5