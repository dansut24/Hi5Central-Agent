// src/ipc/input_pipe.h
//
// Lock-free SPSC ring buffer over named shared memory for relaying
// remote-control input commands from the Session-0 service to the
// Session-1 streamer process.
//
// The service writes InputCmd structs; the streamer reads and dispatches
// them to SendInput via the input_injector module.

#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace hi5 {

// ── Input command types ──────────────────────────────────────────────────────

enum class InputCmdType : uint8_t {
    None = 0,
    MouseMove,       // absolute move
    MouseButton,     // press/release
    MouseWheel,      // scroll
    KeyEvent,        // key press/release
    ClipboardSet,    // set clipboard text on remote
    ClipboardGet,    // request clipboard text from remote
    ClipboardPaste,  // set clipboard text on remote, then send Ctrl+V
    PasteText,       // type text directly using Unicode keystrokes
    Shortcut,        // native/system shortcut action
    SwitchMonitor,   // switch to a different monitor
    ChatOpen,        // show remote chat window
    ChatClose,       // hide remote chat window
    ChatMessage,     // display incoming chat text
};

enum class ShortcutAction : uint16_t {
    None = 0,
    CtrlAltDel,
    LockWorkstation,
    TaskManager,
    Explorer,
    StartMenu,
    WinD,          // Show desktop / restore desktop
    WinR,          // Run dialog
    WinE,          // Explorer
    WinTab,        // Task view
    AltTab,        // Legacy app switcher tap
    AltTabBegin,   // Hold Alt and press Tab once, leaving Alt down
    AltTabNext,    // Press Tab while Alt remains held
    AltTabEnd,     // Release Alt to select highlighted window
    AltF4,         // Close active window
    CtrlShiftEsc,  // Task Manager
    CtrlEsc,       // Start menu fallback
};

// Fixed-size command struct (fits in a ring slot).
// For clipboard, we use a separate larger buffer.
#pragma pack(push, 1)
struct InputCmd {
    InputCmdType type = InputCmdType::None;

    union {
        struct {
            int32_t x, y;           // absolute coords in capture space
            int32_t remoteW, remoteH;
            int32_t monitorIndex;   // which monitor these coords refer to
        } mouseMove;

        struct {
            uint8_t button;         // 0=left, 1=right, 2=middle
            uint8_t down;           // 1=press, 0=release
        } mouseButton;

        struct {
            int32_t deltaX;         // horizontal scroll (0 for vertical-only)
            int32_t deltaY;         // vertical scroll (positive = up)
        } mouseWheel;

        struct {
            uint16_t vk;            // virtual key code
            uint16_t scanCode;      // hardware scan code (0 = auto)
            uint8_t  down;          // 1=press, 0=release
            uint8_t  isExtended;    // extended key flag
        } key;

        struct {
            uint32_t length;        // length of clipboard text in the overflow buffer
            uint32_t offsetInClip;  // offset into the clipboard shmem region
        } clipboard;

        struct {
            uint16_t action;        // ShortcutAction value
            uint16_t reserved;
            uint32_t flags;
        } shortcut;

        struct {
            int32_t monitorIndex;   // target monitor index
        } switchMonitor;

        struct {
            uint32_t length;
            uint32_t offsetInClip;
        } chatMessage;
    };
};
#pragma pack(pop)

static_assert(sizeof(InputCmd) <= 64, "InputCmd must fit in a ring slot");

// ── Ring buffer layout ───────────────────────────────────────────────────────

static constexpr int      kInputRingSlots    = 256;   // plenty for input events
static constexpr uint32_t kInputSlotBytes    = 64;    // >= sizeof(InputCmd)
static constexpr uint32_t kClipboardBufBytes = 128 * 1024;  // 128 KB for inbound clipboard/text commands
static constexpr uint32_t kClipboardResponseBufBytes = 128 * 1024;  // 128 KB for remote->local clipboard responses

// Lightweight stream diagnostics published by the interactive streamer and
// read by the service. These counters are deliberately primitive/atomic so the
// reader never blocks the capture loop.
struct StreamStats {
    uint64_t seq = 0;
    uint64_t unixMs = 0;
    uint64_t captureAttempts = 0;
    uint64_t changedFrames = 0;
    uint64_t skippedFrames = 0;
    uint64_t writtenFrames = 0;
    uint64_t inputEvents = 0;
    uint64_t resetCount = 0;
    int32_t targetFps = 0;
    int32_t streamMode = 0; // 0=idle, 1=active-change, 2=recent-input/motion
    int32_t secureDesktopActive = 0;
    int32_t displayIndex = 0;
};

// Latest-position fast mouse target. Unlike the normal input ring, this is
// not a queue: the service overwrites it with the newest cursor target and
// the interactive streamer applies only the newest sequence. This prevents
// stale mouse_move events from backing up and keeps native cursor control
// low latency.
struct FastMouseTarget {
    uint64_t seq = 0;
    uint64_t unixMs = 0;
    uint64_t clientTsMs = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t monitorIndex = 0;
};

struct InputRingHeader {
    std::atomic<uint64_t> writeIdx;
    std::atomic<uint64_t> readIdx;
    uint32_t              numSlots;
    uint32_t              slotBytes;
    // Monitor info published by the streamer
    std::atomic<int32_t>  monitorCount;
    struct MonitorInfo {
        int32_t x, y, w, h;        // virtual desktop coordinates
        uint8_t primary;
        uint8_t _pad[3];
    } monitors[8];                  // up to 8 monitors
    // UAC / Secure Desktop flag (set by streamer, read by service)
    std::atomic<int32_t>  uacActive;  // 1 = UAC/Secure Desktop active, 0 = normal

    // Stream diagnostics, written by streamer and read by service.
    std::atomic<uint64_t> streamStatsSeq;
    std::atomic<uint64_t> streamStatsUnixMs;
    std::atomic<uint64_t> streamStatsCaptureAttempts;
    std::atomic<uint64_t> streamStatsChangedFrames;
    std::atomic<uint64_t> streamStatsSkippedFrames;
    std::atomic<uint64_t> streamStatsWrittenFrames;
    std::atomic<uint64_t> streamStatsInputEvents;
    std::atomic<uint64_t> streamStatsResetCount;
    std::atomic<int32_t>  streamStatsTargetFps;
    std::atomic<int32_t>  streamStatsMode;
    std::atomic<int32_t>  streamStatsSecureDesktopActive;
    std::atomic<int32_t>  streamStatsDisplayIndex;

    // Fast latest-position native mouse path, written by the service and read
    // by the interactive streamer. Store fastMouseSeq last using release
    // ordering after x/y/client timestamp have been written.
    std::atomic<uint64_t> fastMouseSeq;
    std::atomic<uint64_t> fastMouseUnixMs;
    std::atomic<uint64_t> fastMouseClientTsMs;
    std::atomic<int32_t>  fastMouseX;
    std::atomic<int32_t>  fastMouseY;
    std::atomic<int32_t>  fastMouseMonitorIndex;

    // Remote->local clipboard response, written by streamer and read by service.
    std::atomic<uint64_t> clipboardResponseSeq;
    std::atomic<uint32_t> clipboardResponseLen;
    std::atomic<int32_t>  clipboardResponseOk;
    uint8_t _pad[4];
};

static constexpr size_t kInputRingDataOffset = sizeof(InputRingHeader);
static constexpr size_t kInputClipOffset = kInputRingDataOffset + kInputRingSlots * kInputSlotBytes;
static constexpr size_t kInputClipResponseOffset = kInputClipOffset + kClipboardBufBytes;
static constexpr size_t kInputShmTotalBytes = kInputClipResponseOffset + kClipboardResponseBufBytes;

// ── Writer (used by the service, Session 0) ──────────────────────────────────

class InputPipeWriter {
public:
    ~InputPipeWriter() { Close(); }

    bool Create(const std::string& name) {
        name_ = name;

        SECURITY_DESCRIPTOR sd{};
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), &sd, FALSE };

        hMap_ = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
            PAGE_READWRITE, 0,
            static_cast<DWORD>(kInputShmTotalBytes),
            name.c_str());
        if (!hMap_) return false;

        base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kInputShmTotalBytes);
        if (!base_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }

        auto* hdr = header();
        hdr->writeIdx.store(0);
        hdr->readIdx.store(0);
        hdr->numSlots  = kInputRingSlots;
        hdr->slotBytes = kInputSlotBytes;
        hdr->monitorCount.store(0);
        hdr->uacActive.store(0);
        hdr->streamStatsSeq.store(0);
        hdr->streamStatsUnixMs.store(0);
        hdr->streamStatsCaptureAttempts.store(0);
        hdr->streamStatsChangedFrames.store(0);
        hdr->streamStatsSkippedFrames.store(0);
        hdr->streamStatsWrittenFrames.store(0);
        hdr->streamStatsInputEvents.store(0);
        hdr->streamStatsResetCount.store(0);
        hdr->streamStatsTargetFps.store(0);
        hdr->streamStatsMode.store(0);
        hdr->streamStatsSecureDesktopActive.store(0);
        hdr->streamStatsDisplayIndex.store(0);
        hdr->fastMouseSeq.store(0);
        hdr->fastMouseUnixMs.store(0);
        hdr->fastMouseClientTsMs.store(0);
        hdr->fastMouseX.store(0);
        hdr->fastMouseY.store(0);
        hdr->fastMouseMonitorIndex.store(0);
        hdr->clipboardResponseSeq.store(0);
        hdr->clipboardResponseLen.store(0);
        hdr->clipboardResponseOk.store(0);
        return true;
    }

    bool Write(const InputCmd& cmd) {
        if (!base_) return false;
        auto* hdr = header();
        uint64_t w = hdr->writeIdx.load(std::memory_order_relaxed);
        uint64_t r = hdr->readIdx.load(std::memory_order_acquire);
        if (w - r >= kInputRingSlots) return false;  // full (drop oldest-ish)

        uint32_t slot = static_cast<uint32_t>(w % kInputRingSlots);
        uint8_t* dst = slotPtr(slot);
        std::memcpy(dst, &cmd, sizeof(InputCmd));
        hdr->writeIdx.store(w + 1, std::memory_order_release);
        return true;
    }

    // Write clipboard text into the overflow buffer and return the offset/length.
    bool WriteClipboard(const char* text, uint32_t len, uint32_t& outOffset) {
        if (!base_ || len > kClipboardBufBytes) return false;
        outOffset = 0;
        uint8_t* clipBuf = reinterpret_cast<uint8_t*>(base_) + kInputClipOffset;
        std::memcpy(clipBuf + outOffset, text, len);
        return true;
    }

    // Read monitor info published by the streamer
    int GetMonitorCount() const {
        if (!base_) return 0;
        return header()->monitorCount.load(std::memory_order_acquire);
    }

    InputRingHeader::MonitorInfo GetMonitorInfo(int idx) const {
        if (!base_ || idx < 0 || idx >= 8) return {};
        return header()->monitors[idx];
    }

    // Read UAC active flag published by the streamer
    bool GetUACActive() const {
        if (!base_) return false;
        return header()->uacActive.load(std::memory_order_acquire) != 0;
    }

    bool PublishFastMouseTarget(int32_t x, int32_t y, int32_t monitorIndex, uint64_t seq, uint64_t clientTsMs) {
        if (!base_) return false;
        auto* hdr = header();

        if (seq == 0) {
            seq = hdr->fastMouseSeq.load(std::memory_order_relaxed) + 1;
        }

        const uint64_t unixMs = static_cast<uint64_t>(GetTickCount64());

        hdr->fastMouseX.store(x, std::memory_order_relaxed);
        hdr->fastMouseY.store(y, std::memory_order_relaxed);
        hdr->fastMouseMonitorIndex.store(monitorIndex, std::memory_order_relaxed);
        hdr->fastMouseClientTsMs.store(clientTsMs, std::memory_order_relaxed);
        hdr->fastMouseUnixMs.store(unixMs, std::memory_order_relaxed);
        hdr->fastMouseSeq.store(seq, std::memory_order_release);
        return true;
    }

    bool ReadStreamStats(uint64_t minSeq, StreamStats& out) const {
        if (!base_) return false;
        auto* hdr = header();
        const uint64_t seq = hdr->streamStatsSeq.load(std::memory_order_acquire);
        if (seq <= minSeq) return false;

        out.seq = seq;
        out.unixMs = hdr->streamStatsUnixMs.load(std::memory_order_acquire);
        out.captureAttempts = hdr->streamStatsCaptureAttempts.load(std::memory_order_acquire);
        out.changedFrames = hdr->streamStatsChangedFrames.load(std::memory_order_acquire);
        out.skippedFrames = hdr->streamStatsSkippedFrames.load(std::memory_order_acquire);
        out.writtenFrames = hdr->streamStatsWrittenFrames.load(std::memory_order_acquire);
        out.inputEvents = hdr->streamStatsInputEvents.load(std::memory_order_acquire);
        out.resetCount = hdr->streamStatsResetCount.load(std::memory_order_acquire);
        out.targetFps = hdr->streamStatsTargetFps.load(std::memory_order_acquire);
        out.streamMode = hdr->streamStatsMode.load(std::memory_order_acquire);
        out.secureDesktopActive = hdr->streamStatsSecureDesktopActive.load(std::memory_order_acquire);
        out.displayIndex = hdr->streamStatsDisplayIndex.load(std::memory_order_acquire);
        return true;
    }

    uint64_t GetClipboardResponseSeq() const {
        if (!base_) return 0;
        return header()->clipboardResponseSeq.load(std::memory_order_acquire);
    }

    bool ReadClipboardResponse(uint64_t minSeq, std::string& out, bool& ok, uint64_t& outSeq) const {
        out.clear(); ok = false; outSeq = 0;
        if (!base_) return false;
        auto* hdr = header();
        const uint64_t seq = hdr->clipboardResponseSeq.load(std::memory_order_acquire);
        if (seq <= minSeq) return false;
        const uint32_t len = hdr->clipboardResponseLen.load(std::memory_order_acquire);
        if (len > kClipboardResponseBufBytes) return false;
        const uint8_t* buf = reinterpret_cast<const uint8_t*>(base_) + kInputClipResponseOffset;
        out.assign(reinterpret_cast<const char*>(buf), len);
        ok = hdr->clipboardResponseOk.load(std::memory_order_acquire) != 0;
        outSeq = seq;
        return true;
    }

    void Close() {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
    }

private:
    InputRingHeader* header() const {
        return reinterpret_cast<InputRingHeader*>(base_);
    }
    uint8_t* slotPtr(uint32_t slot) const {
        return reinterpret_cast<uint8_t*>(base_) + kInputRingDataOffset + slot * kInputSlotBytes;
    }

    HANDLE hMap_ = nullptr;
    void*  base_ = nullptr;
    std::string name_;
};

// ── Reader (used by the streamer, Session 1) ─────────────────────────────────

class InputPipeReader {
public:
    ~InputPipeReader() { Close(); }

    bool Open(const std::string& name) {
        name_ = name;
        hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!hMap_) return false;
        base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kInputShmTotalBytes);
        if (!base_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }
        return true;
    }

    bool Read(InputCmd& cmd) {
        if (!base_) return false;
        auto* hdr = header();
        uint64_t r = hdr->readIdx.load(std::memory_order_relaxed);
        uint64_t w = hdr->writeIdx.load(std::memory_order_acquire);
        if (r >= w) return false;

        uint32_t slot = static_cast<uint32_t>(r % kInputRingSlots);
        const uint8_t* src = slotPtr(slot);
        std::memcpy(&cmd, src, sizeof(InputCmd));
        hdr->readIdx.store(r + 1, std::memory_order_release);
        return true;
    }

    // Read clipboard text from the overflow buffer
    bool ReadClipboard(uint32_t offset, uint32_t len, std::string& out) {
        if (!base_ || offset + len > kClipboardBufBytes) return false;
        const uint8_t* clipBuf = reinterpret_cast<const uint8_t*>(base_) + kInputClipOffset;
        out.assign(reinterpret_cast<const char*>(clipBuf + offset), len);
        return true;
    }

    // Publish monitor info so the service can relay it to the viewer
    void SetMonitorInfo(int count, const InputRingHeader::MonitorInfo* infos) {
        if (!base_) return;
        auto* hdr = header();
        int n = (count > 8) ? 8 : count;
        for (int i = 0; i < n; ++i) {
            hdr->monitors[i] = infos[i];
        }
        hdr->monitorCount.store(n, std::memory_order_release);
    }

    // Publish UAC active status so the service can react
    void SetUACActive(bool active) {
        if (!base_) return;
        header()->uacActive.store(active ? 1 : 0, std::memory_order_release);
    }

    bool ReadFastMouseTarget(uint64_t& lastSeq, FastMouseTarget& out) {
        if (!base_) return false;
        auto* hdr = header();

        const uint64_t seq = hdr->fastMouseSeq.load(std::memory_order_acquire);
        if (seq == 0 || seq <= lastSeq) return false;

        out.seq = seq;
        out.unixMs = hdr->fastMouseUnixMs.load(std::memory_order_relaxed);
        out.clientTsMs = hdr->fastMouseClientTsMs.load(std::memory_order_relaxed);
        out.x = hdr->fastMouseX.load(std::memory_order_relaxed);
        out.y = hdr->fastMouseY.load(std::memory_order_relaxed);
        out.monitorIndex = hdr->fastMouseMonitorIndex.load(std::memory_order_relaxed);

        lastSeq = seq;
        return true;
    }

    void PublishStreamStats(const StreamStats& stats) {
        if (!base_) return;
        auto* hdr = header();
        hdr->streamStatsUnixMs.store(stats.unixMs, std::memory_order_relaxed);
        hdr->streamStatsCaptureAttempts.store(stats.captureAttempts, std::memory_order_relaxed);
        hdr->streamStatsChangedFrames.store(stats.changedFrames, std::memory_order_relaxed);
        hdr->streamStatsSkippedFrames.store(stats.skippedFrames, std::memory_order_relaxed);
        hdr->streamStatsWrittenFrames.store(stats.writtenFrames, std::memory_order_relaxed);
        hdr->streamStatsInputEvents.store(stats.inputEvents, std::memory_order_relaxed);
        hdr->streamStatsResetCount.store(stats.resetCount, std::memory_order_relaxed);
        hdr->streamStatsTargetFps.store(stats.targetFps, std::memory_order_relaxed);
        hdr->streamStatsMode.store(stats.streamMode, std::memory_order_relaxed);
        hdr->streamStatsSecureDesktopActive.store(stats.secureDesktopActive, std::memory_order_relaxed);
        hdr->streamStatsDisplayIndex.store(stats.displayIndex, std::memory_order_relaxed);
        hdr->streamStatsSeq.fetch_add(1, std::memory_order_acq_rel);
    }

    bool PublishClipboardResponse(const std::string& text, bool ok) {
        if (!base_ || text.size() > kClipboardResponseBufBytes) return false;
        auto* hdr = header();
        uint8_t* buf = reinterpret_cast<uint8_t*>(base_) + kInputClipResponseOffset;
        if (!text.empty()) std::memcpy(buf, text.data(), text.size());
        hdr->clipboardResponseLen.store(static_cast<uint32_t>(text.size()), std::memory_order_release);
        hdr->clipboardResponseOk.store(ok ? 1 : 0, std::memory_order_release);
        hdr->clipboardResponseSeq.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    void Close() {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
    }

private:
    InputRingHeader* header() const {
        return reinterpret_cast<InputRingHeader*>(base_);
    }
    const uint8_t* slotPtr(uint32_t slot) const {
        return reinterpret_cast<const uint8_t*>(base_) + kInputRingDataOffset + slot * kInputSlotBytes;
    }

    HANDLE hMap_ = nullptr;
    void*  base_ = nullptr;
    std::string name_;
};

} // namespace hi5
