#include "remote_platform.h"
#include "platform_info.h"

#if defined(__linux__)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <map>
#include <mutex>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <vector>

namespace hi5 {
namespace {

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string runCommand(const std::string& command) {
    std::array<char, 2048> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
        if (output.size() > 256 * 1024) break;
    }
    pclose(pipe);
    return trim(output);
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::string xServerAuthorityForDisplay(const std::string& display) {
    if (display.empty()) return {};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec) break;
        const auto pid = entry.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            continue;
        }

        std::ifstream in(entry.path() / "cmdline", std::ios::binary);
        if (!in) continue;
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (raw.empty()) continue;

        std::vector<std::string> args;
        std::size_t start = 0;
        while (start < raw.size()) {
            const auto end = raw.find('\0', start);
            const auto value = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!value.empty()) args.push_back(value);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (args.empty()) continue;

        const auto executable = std::filesystem::path(args.front()).filename().string();
        const bool xServer =
            executable == "Xorg" || executable == "X" ||
            executable == "Xwayland" || executable == "Xwayland.bin";
        if (!xServer || std::find(args.begin(), args.end(), display) == args.end()) continue;

        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] != "-auth") continue;
            const auto& candidate = args[i + 1];
            std::error_code authEc;
            if (!candidate.empty() && std::filesystem::exists(candidate, authEc) && !authEc) {
                return candidate;
            }
        }
    }
    return {};
}

struct UserSession {
    std::string user;
    uid_t uid = static_cast<uid_t>(-1);
    std::string home;
    std::string display;
    std::string xauthority;
    std::string type;
    std::string dbusAddress;
    std::string runtimeDir;
};

UserSession currentGraphicalSession() {
    UserSession session;

    const auto sessionId = runCommand(
        "loginctl list-sessions --no-legend 2>/dev/null | "
        "awk '$4==\"seat0\" && $7==\"yes\" {print $1; exit} "
        "$6==\"active\" && $3!=\"root\" {fallback=$1} "
        "END {if (fallback) print fallback}' | head -1");
    if (sessionId.empty()) return session;

    const auto quotedSession = shellQuote(sessionId);
    session.user = runCommand("loginctl show-session " + quotedSession + " -p Name --value 2>/dev/null");
    const auto uidText = runCommand("loginctl show-session " + quotedSession + " -p User --value 2>/dev/null");
    if (!uidText.empty()) {
        try { session.uid = static_cast<uid_t>(std::stoul(uidText)); } catch (...) {}
    }

    if (!session.user.empty()) {
        long size = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (size < 1024) size = 16384;
        std::vector<char> buffer(static_cast<std::size_t>(size));
        struct passwd pwd {};
        struct passwd* result = nullptr;
        if (getpwnam_r(session.user.c_str(), &pwd, buffer.data(), buffer.size(), &result) == 0 && result) {
            if (session.uid == static_cast<uid_t>(-1)) session.uid = pwd.pw_uid;
            session.home = pwd.pw_dir ? pwd.pw_dir : "";
        }
    }

    session.type = runCommand("loginctl show-session " + quotedSession + " -p Type --value 2>/dev/null");
    session.display = runCommand("loginctl show-session " + quotedSession + " -p Display --value 2>/dev/null");
    const auto leader = runCommand("loginctl show-session " + quotedSession + " -p Leader --value 2>/dev/null");
    if (!leader.empty()) {
        const std::string environPath = "/proc/" + leader + "/environ";
            std::ifstream in(environPath, std::ios::binary);
            if (in) {
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::size_t start = 0;
                while (start < data.size()) {
                    const auto end = data.find('\0', start);
                    const auto entry = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    const auto pos = entry.find('=');
                    if (pos != std::string::npos) {
                        const auto key = entry.substr(0, pos);
                        const auto value = entry.substr(pos + 1);
                        if (key == "DISPLAY" && session.display.empty()) session.display = value;
                        else if (key == "XAUTHORITY") session.xauthority = value;
                        else if (key == "DBUS_SESSION_BUS_ADDRESS") session.dbusAddress = value;
                        else if (key == "XDG_RUNTIME_DIR") session.runtimeDir = value;
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
        }

    if (session.runtimeDir.empty() && session.uid != static_cast<uid_t>(-1)) {
        session.runtimeDir = "/run/user/" + std::to_string(session.uid);
    }
    if (session.dbusAddress.empty() && !session.runtimeDir.empty()) {
        session.dbusAddress = "unix:path=" + session.runtimeDir + "/bus";
    }
    if (session.display.empty() && session.type == "x11") session.display = ":0";

    if (!session.xauthority.empty()) {
        std::error_code authEc;
        if (!std::filesystem::exists(session.xauthority, authEc) || authEc) session.xauthority.clear();
    }
    if (session.xauthority.empty()) {
        session.xauthority = xServerAuthorityForDisplay(session.display);
    }
    if (session.xauthority.empty() && !session.home.empty()) {
        const auto candidate = session.home + "/.Xauthority";
        std::error_code authEc;
        if (std::filesystem::exists(candidate, authEc) && !authEc) session.xauthority = candidate;
    }
    return session;
}

int maskShift(unsigned long mask) {
    int shift = 0;
    if (!mask) return 0;
    while ((mask & 1UL) == 0) {
        ++shift;
        mask >>= 1;
    }
    return shift;
}

int maskBits(unsigned long mask) {
    int bits = 0;
    while (mask) {
        bits += static_cast<int>(mask & 1UL);
        mask >>= 1;
    }
    return bits;
}

uint8_t extractChannel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    const int shift = maskShift(mask);
    const int bits = maskBits(mask >> shift);
    const unsigned long raw = (pixel & mask) >> shift;
    const unsigned long maxValue = bits >= static_cast<int>(sizeof(unsigned long) * 8)
        ? ~0UL
        : ((1UL << bits) - 1UL);
    if (!maxValue) return 0;
    return static_cast<uint8_t>((raw * 255UL + maxValue / 2UL) / maxValue);
}

uint8_t clampByte(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

I420Frame imageToI420(const XImage* image) {
    I420Frame frame;
    if (!image || image->width < 2 || image->height < 2 || !image->data) return frame;

    frame.width = image->width & ~1;
    frame.height = image->height & ~1;
    const int chromaWidth = frame.width / 2;
    const int chromaHeight = frame.height / 2;
    frame.y.resize(static_cast<std::size_t>(frame.width) * frame.height);
    frame.u.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);
    frame.v.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);

    const int bytesPerPixel = std::max(1, (image->bits_per_pixel + 7) / 8);
    auto pixelAt = [&](int x, int y) -> unsigned long {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(image->data) +
            static_cast<std::size_t>(y) * image->bytes_per_line +
            static_cast<std::size_t>(x) * bytesPerPixel;
        unsigned long value = 0;
        const int count = std::min<int>(bytesPerPixel, sizeof(value));
        if (image->byte_order == LSBFirst) {
            for (int i = 0; i < count; ++i) value |= static_cast<unsigned long>(p[i]) << (8 * i);
        } else {
            for (int i = 0; i < count; ++i) value = (value << 8) | p[i];
        }
        return value;
    };

    for (int y = 0; y < frame.height; y += 2) {
        for (int x = 0; x < frame.width; x += 2) {
            int sumR = 0, sumG = 0, sumB = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const auto pixel = pixelAt(x + dx, y + dy);
                    const int r = extractChannel(pixel, image->red_mask);
                    const int g = extractChannel(pixel, image->green_mask);
                    const int b = extractChannel(pixel, image->blue_mask);
                    sumR += r; sumG += g; sumB += b;
                    const int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    frame.y[static_cast<std::size_t>(y + dy) * frame.width + x + dx] = clampByte(yy);
                }
            }
            const int r = sumR / 4;
            const int g = sumG / 4;
            const int b = sumB / 4;
            const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            const auto ci = static_cast<std::size_t>(y / 2) * chromaWidth + x / 2;
            frame.u[ci] = clampByte(u);
            frame.v[ci] = clampByte(v);
        }
    }
    return frame;
}

KeySym keySymForMessage(const nlohmann::json& message) {
    const std::string code = message.value("code", "");
    const std::string key = message.value("key", "");

    static const std::map<std::string, KeySym> special = {
        {"Enter", XK_Return}, {"NumpadEnter", XK_KP_Enter}, {"Escape", XK_Escape},
        {"Backspace", XK_BackSpace}, {"Tab", XK_Tab}, {"Space", XK_space},
        {"ArrowLeft", XK_Left}, {"ArrowRight", XK_Right}, {"ArrowUp", XK_Up}, {"ArrowDown", XK_Down},
        {"Home", XK_Home}, {"End", XK_End}, {"PageUp", XK_Page_Up}, {"PageDown", XK_Page_Down},
        {"Insert", XK_Insert}, {"Delete", XK_Delete},
        {"ShiftLeft", XK_Shift_L}, {"ShiftRight", XK_Shift_R},
        {"ControlLeft", XK_Control_L}, {"ControlRight", XK_Control_R},
        {"AltLeft", XK_Alt_L}, {"AltRight", XK_Alt_R},
        {"MetaLeft", XK_Super_L}, {"MetaRight", XK_Super_R},
        {"CapsLock", XK_Caps_Lock}, {"NumLock", XK_Num_Lock},
        {"F1", XK_F1}, {"F2", XK_F2}, {"F3", XK_F3}, {"F4", XK_F4},
        {"F5", XK_F5}, {"F6", XK_F6}, {"F7", XK_F7}, {"F8", XK_F8},
        {"F9", XK_F9}, {"F10", XK_F10}, {"F11", XK_F11}, {"F12", XK_F12}
    };
    const auto it = special.find(code);
    if (it != special.end()) return it->second;

    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        std::string one(1, static_cast<char>(std::tolower(static_cast<unsigned char>(code[3]))));
        return XStringToKeysym(one.c_str());
    }
    if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        std::string one(1, code[5]);
        return XStringToKeysym(one.c_str());
    }
    if (key.size() == 1) return XStringToKeysym(key.c_str());
    return NoSymbol;
}

class X11RemotePlatform final : public RemotePlatform {
public:
    ~X11RemotePlatform() override { stop(); }

    bool start(std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (display_) return true;

        static std::once_flag xThreads;
        std::call_once(xThreads, []() { XInitThreads(); });

        session_ = currentGraphicalSession();
        if (session_.user.empty()) {
            error = "No active graphical user session is available.";
            return false;
        }
        if (session_.type != "x11") {
            error = "Active Linux desktop is not X11.";
            return false;
        }

        if (!session_.display.empty()) setenv("DISPLAY", session_.display.c_str(), 1);
        if (!session_.xauthority.empty()) setenv("XAUTHORITY", session_.xauthority.c_str(), 1);

        display_ = XOpenDisplay(session_.display.empty() ? nullptr : session_.display.c_str());
        if (!display_) {
            error = "Unable to open X11 display " + (session_.display.empty() ? std::string("(default)") : session_.display) + ".";
            return false;
        }

        screen_ = DefaultScreen(display_);
        root_ = RootWindow(display_, screen_);
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(display_, root_, &attrs) || attrs.width < 2 || attrs.height < 2) {
            error = "Unable to read X11 root desktop geometry.";
            stopLocked();
            return false;
        }
        width_ = attrs.width & ~1;
        height_ = attrs.height & ~1;
        initSharedImage();
        return true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopLocked();
    }

    FrameCaptureResult capture() override {
        std::lock_guard<std::mutex> lock(mutex_);
        FrameCaptureResult out;
        if (!display_) return out;

        XImage* image = nullptr;
        bool temporary = false;
        if (shmImage_ && shmAttached_) {
            if (!XShmGetImage(display_, root_, shmImage_, 0, 0, AllPlanes)) return out;
            image = shmImage_;
        } else {
            image = XGetImage(display_, root_, 0, 0,
                static_cast<unsigned int>(width_), static_cast<unsigned int>(height_),
                AllPlanes, ZPixmap);
            temporary = true;
        }
        if (!image) return out;

        out.frame = imageToI420(image);
        out.hasFrame = out.frame.width > 0 && out.frame.height > 0;
        out.changed = out.hasFrame;
        out.frameId = ++frameId_;

        if (temporary) XDestroyImage(image);
        return out;
    }

    std::vector<DisplayInfo> displays() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!display_) return {};
        return {{0, "X11 Desktop", 0, 0, width_, height_, true}};
    }

    int currentDisplayIndex() const override { return 0; }
    bool setDisplayIndex(int index) override { return index == 0 || index == -1; }

    bool handleInput(const nlohmann::json& message, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!display_) {
            error = "X11 display is not open.";
            return false;
        }

        const std::string kind = message.value("kind", message.value("type", ""));
        auto moveIfPresent = [&]() {
            if (!message.contains("x_norm") || !message.contains("y_norm")) return;
            const double xn = std::clamp(message.value("x_norm", 0.0), 0.0, 1.0);
            const double yn = std::clamp(message.value("y_norm", 0.0), 0.0, 1.0);
            const int x = std::clamp(static_cast<int>(std::llround(xn * std::max(0, width_ - 1))), 0, std::max(0, width_ - 1));
            const int y = std::clamp(static_cast<int>(std::llround(yn * std::max(0, height_ - 1))), 0, std::max(0, height_ - 1));
            XTestFakeMotionEvent(display_, screen_, x, y, CurrentTime);
        };

        if (kind == "mouse_move") {
            moveIfPresent();
        } else if (kind == "mouse_down" || kind == "mouse_up" || kind == "mouse_click") {
            moveIfPresent();
            const int browserButton = message.value("button", 0);
            const unsigned int button = browserButton == 1 ? 2U : (browserButton == 2 ? 3U : 1U);
            if (kind == "mouse_click") {
                XTestFakeButtonEvent(display_, button, True, CurrentTime);
                XTestFakeButtonEvent(display_, button, False, CurrentTime);
            } else {
                XTestFakeButtonEvent(display_, button, kind == "mouse_down" ? True : False, CurrentTime);
            }
        } else if (kind == "wheel") {
            moveIfPresent();
            const int dy = message.value("delta_y", 0);
            const int dx = message.value("delta_x", 0);
            auto wheel = [&](unsigned int button, int magnitude) {
                const int clicks = std::clamp(std::max(1, std::abs(magnitude) / 80), 1, 8);
                for (int i = 0; i < clicks; ++i) {
                    XTestFakeButtonEvent(display_, button, True, CurrentTime);
                    XTestFakeButtonEvent(display_, button, False, CurrentTime);
                }
            };
            if (dy) wheel(dy > 0 ? 5U : 4U, dy);
            if (dx) wheel(dx > 0 ? 7U : 6U, dx);
        } else if (kind == "key_down" || kind == "key_up") {
            const KeySym sym = keySymForMessage(message);
            if (sym == NoSymbol) {
                error = "Unsupported X11 key code.";
                return false;
            }
            const KeyCode code = XKeysymToKeycode(display_, sym);
            if (!code) {
                error = "X11 key is not present in the current keyboard map.";
                return false;
            }
            XTestFakeKeyEvent(display_, code, kind == "key_down" ? True : False, CurrentTime);
        } else if (kind == "text_input") {
            const std::string text = message.value("text", "");
            for (const unsigned char ch : text) {
                if (ch < 0x20 || ch > 0x7e) continue;
                const std::string one(1, static_cast<char>(ch));
                KeySym sym = XStringToKeysym(one.c_str());
                bool shift = std::isupper(ch) != 0;
                if (sym == NoSymbol) {
                    std::string lower(1, static_cast<char>(std::tolower(ch)));
                    sym = XStringToKeysym(lower.c_str());
                }
                const KeyCode code = sym == NoSymbol ? 0 : XKeysymToKeycode(display_, sym);
                if (!code) continue;
                const KeyCode shiftCode = XKeysymToKeycode(display_, XK_Shift_L);
                if (shift && shiftCode) XTestFakeKeyEvent(display_, shiftCode, True, CurrentTime);
                XTestFakeKeyEvent(display_, code, True, CurrentTime);
                XTestFakeKeyEvent(display_, code, False, CurrentTime);
                if (shift && shiftCode) XTestFakeKeyEvent(display_, shiftCode, False, CurrentTime);
            }
        } else {
            error = "Unsupported X11 input event: " + kind;
            return false;
        }

        XFlush(display_);
        return true;
    }

    std::string backendName() const override { return "x11"; }
    bool requiresConsent() const override { return false; }

private:
    void initSharedImage() {
        if (!display_ || !XShmQueryExtension(display_)) return;
        shmImage_ = XShmCreateImage(
            display_, DefaultVisual(display_, screen_), DefaultDepth(display_, screen_),
            ZPixmap, nullptr, &shm_, static_cast<unsigned int>(width_), static_cast<unsigned int>(height_));
        if (!shmImage_) return;

        shm_.shmid = shmget(IPC_PRIVATE,
            static_cast<std::size_t>(shmImage_->bytes_per_line) * shmImage_->height,
            IPC_CREAT | 0600);
        if (shm_.shmid < 0) {
            XDestroyImage(shmImage_);
            shmImage_ = nullptr;
            return;
        }

        shm_.shmaddr = static_cast<char*>(shmat(shm_.shmid, nullptr, 0));
        if (shm_.shmaddr == reinterpret_cast<char*>(-1)) {
            shmctl(shm_.shmid, IPC_RMID, nullptr);
            shm_.shmid = -1;
            XDestroyImage(shmImage_);
            shmImage_ = nullptr;
            return;
        }
        shmImage_->data = shm_.shmaddr;
        shm_.readOnly = False;
        if (!XShmAttach(display_, &shm_)) {
            shmImage_->data = nullptr;
            XDestroyImage(shmImage_);
            shmImage_ = nullptr;
            shmdt(shm_.shmaddr);
            shmctl(shm_.shmid, IPC_RMID, nullptr);
            shm_.shmaddr = nullptr;
            shm_.shmid = -1;
            return;
        }
        XSync(display_, False);
        shmctl(shm_.shmid, IPC_RMID, nullptr);
        shmAttached_ = true;
    }

    void stopLocked() {
        if (display_ && shmAttached_) {
            XShmDetach(display_, &shm_);
            XSync(display_, False);
            shmAttached_ = false;
        }
        if (shmImage_) {
            shmImage_->data = nullptr;
            XDestroyImage(shmImage_);
            shmImage_ = nullptr;
        }
        if (shm_.shmaddr && shm_.shmaddr != reinterpret_cast<char*>(-1)) {
            shmdt(shm_.shmaddr);
            shm_.shmaddr = nullptr;
        }
        if (display_) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
        width_ = height_ = 0;
    }

    mutable std::mutex mutex_;
    UserSession session_;
    Display* display_ = nullptr;
    int screen_ = 0;
    Window root_ = 0;
    int width_ = 0;
    int height_ = 0;
    XShmSegmentInfo shm_{};
    XImage* shmImage_ = nullptr;
    bool shmAttached_ = false;
    std::uint64_t frameId_ = 0;
};

} // namespace

std::unique_ptr<RemotePlatform> createRemotePlatform() {
    return std::make_unique<X11RemotePlatform>();
}

} // namespace hi5

#endif