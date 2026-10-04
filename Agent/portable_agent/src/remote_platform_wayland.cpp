#include "remote_platform.h"

#if defined(__linux__)

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <linux/input-event-codes.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/buffers.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace hi5 {
namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kRemoteDesktopInterface = "org.freedesktop.portal.RemoteDesktop";
constexpr const char* kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kClipboardInterface = "org.freedesktop.portal.Clipboard";
constexpr const char* kHostRegistryInterface = "org.freedesktop.host.portal.Registry";
constexpr const char* kPortalAppId = "com.hi5central.RemoteHelper";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
constexpr const char* kSessionInterface = "org.freedesktop.portal.Session";

struct PortalReply {
    guint32 response = 2;
    GVariant* results = nullptr;

    PortalReply() = default;
    PortalReply(const PortalReply&) = delete;
    PortalReply& operator=(const PortalReply&) = delete;
    PortalReply(PortalReply&& other) noexcept
        : response(other.response), results(other.results) {
        other.results = nullptr;
    }
    PortalReply& operator=(PortalReply&& other) noexcept {
        if (this != &other) {
            if (results) g_variant_unref(results);
            response = other.response;
            results = other.results;
            other.results = nullptr;
        }
        return *this;
    }
    ~PortalReply() {
        if (results) g_variant_unref(results);
    }
};

struct RequestWait {
    std::mutex mutex;
    bool complete = false;
    guint32 response = 2;
    GVariant* results = nullptr;

    ~RequestWait() {
        if (results) g_variant_unref(results);
    }
};

void onRequestResponse(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant* parameters,
    gpointer userData) {

    auto* wait = static_cast<RequestWait*>(userData);
    if (!wait || !parameters) return;

    guint32 response = 2;
    GVariant* results = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    std::lock_guard<std::mutex> lock(wait->mutex);
    if (wait->complete) {
        if (results) g_variant_unref(results);
        return;
    }
    wait->response = response;
    wait->results = results;
    wait->complete = true;
}

std::string glibErrorMessage(GError* error, const std::string& fallback) {
    if (!error) return fallback;
    std::string message = error->message ? error->message : fallback;
    g_error_free(error);
    return message;
}

GVariant* emptyOptions() {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    return g_variant_builder_end(&builder);
}

std::string tokenPart(const std::string& prefix) {
    static std::atomic<std::uint64_t> counter{1};
    return prefix + std::to_string(
        static_cast<unsigned long long>(::getpid())) + "_" +
        std::to_string(static_cast<unsigned long long>(counter.fetch_add(1)));
}

std::string waylandRestoreTokenPath() {
    const char* managedStateDir = std::getenv("HI5CENTRAL_WAYLAND_STATE_DIR");
    if (managedStateDir && *managedStateDir) {
        return std::string(managedStateDir) + "/wayland-remote-desktop.token";
    }

    const gchar* stateDir = g_get_user_state_dir();
    if (!stateDir || !*stateDir) return {};
    return std::string(stateDir) + "/hi5central/wayland-remote-desktop.token";
}

std::string loadWaylandRestoreToken() {
    const std::string path = waylandRestoreTokenPath();
    if (path.empty()) return {};

    gchar* contents = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    if (!g_file_get_contents(path.c_str(), &contents, &length, &error)) {
        if (error) g_error_free(error);
        return {};
    }

    std::string token(contents ? contents : "", static_cast<std::size_t>(length));
    g_free(contents);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    return token;
}

bool saveWaylandRestoreToken(const std::string& token) {
    if (token.empty()) return false;
    const std::string path = waylandRestoreTokenPath();
    if (path.empty()) return false;

    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return false;
    const std::string directory = path.substr(0, slash);
    if (g_mkdir_with_parents(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }

    GError* error = nullptr;
    if (!g_file_set_contents(
            path.c_str(),
            token.data(),
            static_cast<gssize>(token.size()),
            &error)) {
        if (error) g_error_free(error);
        return false;
    }
    (void)::chmod(path.c_str(), 0600);
    return true;
}

void clearWaylandRestoreToken() {
    const std::string path = waylandRestoreTokenPath();
    if (!path.empty()) (void)::unlink(path.c_str());
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
    const auto found = special.find(code);
    if (found != special.end()) return found->second;

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

std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

I420Frame raw32ToI420(
    const std::uint8_t* data,
    int width,
    int height,
    int stride,
    enum spa_video_format format) {

    I420Frame frame;
    if (!data || width < 2 || height < 2 || stride == 0) return frame;
    frame.width = width & ~1;
    frame.height = height & ~1;
    const int chromaWidth = frame.width / 2;
    const int chromaHeight = frame.height / 2;
    frame.y.resize(static_cast<std::size_t>(frame.width) * frame.height);
    frame.u.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);
    frame.v.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);

    const bool bgr =
        format == SPA_VIDEO_FORMAT_BGRx ||
        format == SPA_VIDEO_FORMAT_BGRA;

    for (int y = 0; y < frame.height; y += 2) {
        for (int x = 0; x < frame.width; x += 2) {
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int sourceY = stride > 0 ? y + dy : (height - 1 - (y + dy));
                const auto* row =
                    data + static_cast<std::size_t>(sourceY) *
                    static_cast<std::size_t>(std::abs(stride));
                for (int dx = 0; dx < 2; ++dx) {
                    const auto* pixel = row + static_cast<std::size_t>(x + dx) * 4U;
                    const int r = bgr ? pixel[2] : pixel[0];
                    const int g = pixel[1];
                    const int b = bgr ? pixel[0] : pixel[2];
                    sumR += r;
                    sumG += g;
                    sumB += b;
                    const int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    frame.y[static_cast<std::size_t>(y + dy) * frame.width + x + dx] =
                        clampByte(yy);
                }
            }

            const int r = sumR / 4;
            const int g = sumG / 4;
            const int b = sumB / 4;
            const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            const auto chromaIndex =
                static_cast<std::size_t>(y / 2) * chromaWidth + x / 2;
            frame.u[chromaIndex] = clampByte(u);
            frame.v[chromaIndex] = clampByte(v);
        }
    }
    return frame;
}

class WaylandPortalRemotePlatform final : public RemotePlatform {
public:
    ~WaylandPortalRemotePlatform() override {
        stop();
    }

    void setPersistentAccessRequested(bool requested) override {
        persistentAccessRequested_ = requested;
    }

    bool start(std::string& error) override {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (started_) return true;

        GError* busError = nullptr;
        bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &busError);
        if (!bus_) {
            error = glibErrorMessage(
                busError,
                "Unable to connect to the active Wayland session bus.");
            return false;
        }

        std::string identityError;
        portalIdentityRegistered_ = registerPortalHostIdentity(identityError);
        if (!portalIdentityRegistered_ && !identityError.empty()) {
            std::cerr
                << "[wayland-portal] host app identity registration unavailable: "
                << identityError << "\n";
        }

        remoteDesktopPortalVersion_ = portalInterfaceVersion(kRemoteDesktopInterface);
        portalPersistenceSupported_ =
            persistentAccessRequested_ && remoteDesktopPortalVersion_ >= 2U;
        restoreToken_ = portalPersistenceSupported_
            ? loadWaylandRestoreToken()
            : std::string();
        std::cerr
            << "[wayland-portal] RemoteDesktop version="
            << remoteDesktopPortalVersion_
            << " persistence_requested=" << (persistentAccessRequested_ ? 1 : 0)
            << " persistence_available=" << (portalPersistenceSupported_ ? 1 : 0)
            << " restore_token=" << (restoreToken_.empty() ? "none" : "available")
            << "\n";

        if (!createPortalSession(error) ||
            !selectPortalDevices(error) ||
            !selectPortalSources(error)) {
            stopLocked();
            return false;
        }

        std::string clipboardError;
        clipboardRequested_ = requestClipboardAccess(clipboardError);
        if (!clipboardRequested_ && !clipboardError.empty()) {
            std::cerr << "[wayland-clipboard] unavailable: " << clipboardError << "\n";
        }

        if (!startPortalSession(error) ||
            !openPipeWireRemote(error) ||
            !startPipeWire(error)) {
            stopLocked();
            return false;
        }

        started_ = true;
        return true;
    }

    void stop() override {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        stopLocked();
    }

    FrameCaptureResult capture() override {
        std::lock_guard<std::mutex> lock(frameMutex_);
        FrameCaptureResult out;
        if (!latestFrame_.width || latestFrame_.y.empty()) return out;
        out.frame = latestFrame_;
        out.hasFrame = true;
        out.changed = latestFrameId_ != deliveredFrameId_;
        out.frameId = latestFrameId_;
        deliveredFrameId_ = latestFrameId_;
        return out;
    }

    std::vector<DisplayInfo> displays() const override {
        std::lock_guard<std::mutex> lock(frameMutex_);
        const int width = logicalWidth_ > 0
            ? logicalWidth_
            : latestFrame_.width;
        const int height = logicalHeight_ > 0
            ? logicalHeight_
            : latestFrame_.height;
        if (width < 1 || height < 1) return {};
        return {{0, "Wayland Desktop", 0, 0, width, height, true}};
    }

    int currentDisplayIndex() const override {
        return 0;
    }

    bool setDisplayIndex(int index) override {
        return index == 0 || index == -1;
    }

    bool handleInput(const nlohmann::json& message, std::string& error) override {
        if (!bus_ || sessionHandle_.empty() || !streamNodeId_) {
            error = "Wayland portal session is not active.";
            return false;
        }

        const std::string kind =
            message.value("kind", message.value("type", std::string()));

        auto pointerPosition = [&]() -> std::pair<double, double> {
            const double xNorm =
                std::clamp(message.value("x_norm", 0.0), 0.0, 1.0);
            const double yNorm =
                std::clamp(message.value("y_norm", 0.0), 0.0, 1.0);
            int width = logicalWidth_;
            int height = logicalHeight_;
            if (width < 1 || height < 1) {
                std::lock_guard<std::mutex> lock(frameMutex_);
                width = std::max(1, latestFrame_.width);
                height = std::max(1, latestFrame_.height);
            }
            return {
                xNorm * std::max(1, width - 1),
                yNorm * std::max(1, height - 1)
            };
        };

        if (kind == "mouse_move") {
            const auto [x, y] = pointerPosition();
            return notifyPointerAbsolute(x, y, error);
        }

        if (kind == "mouse_down" || kind == "mouse_up" || kind == "mouse_click") {
            if (message.contains("x_norm") && message.contains("y_norm")) {
                const auto [x, y] = pointerPosition();
                if (!notifyPointerAbsolute(x, y, error)) return false;
            }

            const int browserButton = message.value("button", 0);
            const int button = browserButton == 1
                ? BTN_MIDDLE
                : (browserButton == 2 ? BTN_RIGHT : BTN_LEFT);

            if (kind == "mouse_click") {
                return notifyPointerButton(button, 1, error) &&
                    notifyPointerButton(button, 0, error);
            }
            return notifyPointerButton(
                button,
                kind == "mouse_down" ? 1U : 0U,
                error);
        }

        if (kind == "wheel") {
            const double dx =
                static_cast<double>(message.value("delta_x", 0)) / 100.0;
            const double dy =
                static_cast<double>(message.value("delta_y", 0)) / 100.0;
            return notifyPointerAxis(dx, dy, error);
        }

        if (kind == "key_down" || kind == "key_up") {
            const KeySym sym = keySymForMessage(message);
            if (sym == NoSymbol) {
                error = "Unsupported Wayland key code.";
                return false;
            }
            return notifyKeysym(
                static_cast<std::int32_t>(sym),
                kind == "key_down" ? 1U : 0U,
                error);
        }

        if (kind == "text_input") {
            const std::string text = message.value("text", "");
            const gchar* cursor = text.c_str();
            const gchar* end = cursor + text.size();
            while (cursor < end && *cursor) {
                const gunichar codepoint = g_utf8_get_char_validated(
                    cursor,
                    static_cast<gssize>(end - cursor));
                if (codepoint == static_cast<gunichar>(-1) ||
                    codepoint == static_cast<gunichar>(-2)) {
                    ++cursor;
                    continue;
                }
                cursor = g_utf8_next_char(cursor);

                std::uint32_t keysym = 0;
                if (codepoint == '\n' || codepoint == '\r') {
                    keysym = static_cast<std::uint32_t>(XK_Return);
                } else if (codepoint == '\t') {
                    keysym = static_cast<std::uint32_t>(XK_Tab);
                } else if (codepoint < 0x20 || codepoint == 0x7f) {
                    continue;
                } else {
                    keysym = codepoint <= 0xff
                        ? static_cast<std::uint32_t>(codepoint)
                        : (0x01000000U | static_cast<std::uint32_t>(codepoint));
                }
                if (!notifyKeysym(static_cast<std::int32_t>(keysym), 1U, error) ||
                    !notifyKeysym(static_cast<std::int32_t>(keysym), 0U, error)) {
                    return false;
                }
            }
            return true;
        }

        error = "Unsupported Wayland input event: " + kind;
        return false;
    }

    bool readClipboardText(std::string& text, std::string& error) override {
        text.clear();
        if (!clipboardEnabled_) {
            error = "Wayland clipboard access was not granted for this remote session.";
            return false;
        }

        if (readClipboardMime("text/plain;charset=utf-8", text, error)) return true;
        const std::string firstError = error;
        error.clear();
        if (readClipboardMime("text/plain", text, error)) return true;
        if (error.empty()) error = firstError;
        return false;
    }

    bool supportsClipboardRead() const override {
        return clipboardEnabled_;
    }

    std::string backendName() const override {
        return "wayland_portal";
    }

    bool requiresConsent() const override {
        return true;
    }

private:
    bool registerPortalHostIdentity(std::string& error) const {
        if (!bus_) {
            error = "Wayland portal session bus is unavailable.";
            return false;
        }

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

        GError* callError = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            bus_,
            kPortalName,
            kPortalPath,
            kHostRegistryInterface,
            "Register",
            g_variant_new("(sa{sv})", kPortalAppId, &options),
            G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            nullptr,
            &callError);
        if (!result) {
            error = glibErrorMessage(
                callError,
                "XDG host application registry is unavailable.");
            return false;
        }

        g_variant_unref(result);
        std::cerr
            << "[wayland-portal] registered host app identity "
            << kPortalAppId << "\n";
        return true;
    }

    guint32 portalInterfaceVersion(const char* interfaceName) const {
        if (!bus_ || !interfaceName || !*interfaceName) return 0;

        GError* callError = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            bus_,
            kPortalName,
            kPortalPath,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", interfaceName, "version"),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            nullptr,
            &callError);
        if (!result) {
            if (callError) g_error_free(callError);
            return 0;
        }

        GVariant* boxed = nullptr;
        g_variant_get(result, "(@v)", &boxed);
        g_variant_unref(result);
        if (!boxed) return 0;

        GVariant* value = g_variant_get_variant(boxed);
        g_variant_unref(boxed);
        if (!value) return 0;

        guint32 version = 0;
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
            version = g_variant_get_uint32(value);
        }
        g_variant_unref(value);
        return version;
    }

    PortalReply portalRequest(
        const char* interfaceName,
        const char* methodName,
        GVariant* parameters,
        int timeoutMs,
        std::string& error) {

        PortalReply reply;
        if (!bus_) {
            error = "Wayland portal session bus is unavailable.";
            return reply;
        }

        RequestWait wait;
        const guint subscription = g_dbus_connection_signal_subscribe(
            bus_,
            kPortalName,
            kRequestInterface,
            "Response",
            nullptr,
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            onRequestResponse,
            &wait,
            nullptr);

        GError* callError = nullptr;
        GVariant* methodReply = g_dbus_connection_call_sync(
            bus_,
            kPortalName,
            kPortalPath,
            interfaceName,
            methodName,
            parameters,
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            timeoutMs,
            nullptr,
            &callError);

        if (!methodReply) {
            g_dbus_connection_signal_unsubscribe(bus_, subscription);
            error = glibErrorMessage(
                callError,
                std::string("Wayland portal ") + methodName + " call failed.");
            return reply;
        }

        const gchar* requestHandle = nullptr;
        g_variant_get(methodReply, "(&o)", &requestHandle);
        const std::string handle = requestHandle ? requestHandle : "";
        g_variant_unref(methodReply);

        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(wait.mutex);
                if (wait.complete) break;
            }
            while (g_main_context_iteration(nullptr, FALSE)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        g_dbus_connection_signal_unsubscribe(bus_, subscription);

        {
            std::lock_guard<std::mutex> lock(wait.mutex);
            if (!wait.complete) {
                error = "Timed out waiting for Wayland portal approval: " +
                    std::string(methodName) +
                    (handle.empty() ? "" : " (" + handle + ")");
                return reply;
            }
            reply.response = wait.response;
            reply.results = wait.results;
            wait.results = nullptr;
        }

        if (reply.response != 0) {
            error = reply.response == 1
                ? "Wayland screen sharing was cancelled by the user."
                : "Wayland portal rejected the remote desktop request.";
        }
        return reply;
    }

    bool createPortalSession(std::string& error) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        const auto handleToken = tokenPart("hi5req");
        const auto sessionToken = tokenPart("hi5session");
        g_variant_builder_add(
            &options,
            "{sv}",
            "handle_token",
            g_variant_new_string(handleToken.c_str()));
        g_variant_builder_add(
            &options,
            "{sv}",
            "session_handle_token",
            g_variant_new_string(sessionToken.c_str()));

        auto reply = portalRequest(
            kRemoteDesktopInterface,
            "CreateSession",
            g_variant_new("(a{sv})", &options),
            15000,
            error);
        if (reply.response != 0 || !reply.results) return false;

        const gchar* session = nullptr;
        if (!g_variant_lookup(
                reply.results,
                "session_handle",
                "&s",
                &session) ||
            !session ||
            !*session) {
            error = "Wayland portal did not return a remote desktop session handle.";
            return false;
        }
        sessionHandle_ = session;
        return true;
    }

    bool selectPortalDevices(std::string& error) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        const auto handleToken = tokenPart("hi5devices");
        g_variant_builder_add(
            &options,
            "{sv}",
            "handle_token",
            g_variant_new_string(handleToken.c_str()));
        g_variant_builder_add(
            &options,
            "{sv}",
            "types",
            g_variant_new_uint32(1U | 2U));

        if (portalPersistenceSupported_) {
            g_variant_builder_add(
                &options,
                "{sv}",
                "persist_mode",
                g_variant_new_uint32(2U));
            if (!restoreToken_.empty()) {
                g_variant_builder_add(
                    &options,
                    "{sv}",
                    "restore_token",
                    g_variant_new_string(restoreToken_.c_str()));
            }
        }

        auto reply = portalRequest(
            kRemoteDesktopInterface,
            "SelectDevices",
            g_variant_new(
                "(oa{sv})",
                sessionHandle_.c_str(),
                &options),
            15000,
            error);
        return reply.response == 0;
    }

    bool selectPortalSources(std::string& error) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        const auto handleToken = tokenPart("hi5sources");
        g_variant_builder_add(
            &options,
            "{sv}",
            "handle_token",
            g_variant_new_string(handleToken.c_str()));
        g_variant_builder_add(
            &options,
            "{sv}",
            "types",
            g_variant_new_uint32(1U));
        g_variant_builder_add(
            &options,
            "{sv}",
            "multiple",
            g_variant_new_boolean(FALSE));
        g_variant_builder_add(
            &options,
            "{sv}",
            "cursor_mode",
            g_variant_new_uint32(2U));

        auto reply = portalRequest(
            kScreenCastInterface,
            "SelectSources",
            g_variant_new(
                "(oa{sv})",
                sessionHandle_.c_str(),
                &options),
            15000,
            error);
        return reply.response == 0;
    }

    bool requestClipboardAccess(std::string& error) {
        if (!bus_ || sessionHandle_.empty()) {
            error = "Wayland remote desktop session is not ready for clipboard access.";
            return false;
        }

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        GError* callError = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            bus_,
            kPortalName,
            kPortalPath,
            kClipboardInterface,
            "RequestClipboard",
            g_variant_new(
                "(oa{sv})",
                sessionHandle_.c_str(),
                &options),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            nullptr,
            &callError);
        if (!result) {
            error = glibErrorMessage(
                callError,
                "Wayland clipboard portal is unavailable.");
            return false;
        }
        g_variant_unref(result);
        return true;
    }

    bool readClipboardMime(
        const char* mimeType,
        std::string& text,
        std::string& error) {

        GUnixFDList* fdList = nullptr;
        GError* callError = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            bus_,
            kPortalName,
            kPortalPath,
            kClipboardInterface,
            "SelectionRead",
            g_variant_new(
                "(os)",
                sessionHandle_.c_str(),
                mimeType),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            nullptr,
            &fdList,
            nullptr,
            &callError);

        if (!result || !fdList) {
            if (result) g_variant_unref(result);
            if (fdList) g_object_unref(fdList);
            error = glibErrorMessage(
                callError,
                std::string("Unable to read Wayland clipboard type ") + mimeType + ".");
            return false;
        }

        gint32 handle = -1;
        g_variant_get(result, "(h)", &handle);
        g_variant_unref(result);

        GError* fdError = nullptr;
        const int fd = g_unix_fd_list_get(fdList, handle, &fdError);
        g_object_unref(fdList);
        if (fd < 0) {
            error = glibErrorMessage(
                fdError,
                "Wayland clipboard portal returned an invalid file descriptor.");
            return false;
        }

        constexpr std::size_t kMaxClipboardBytes = 4U * 1024U * 1024U;
        std::array<char, 8192> buffer {};
        std::string value;
        bool ok = true;
        while (value.size() < kMaxClipboardBytes) {
            const std::size_t remaining = kMaxClipboardBytes - value.size();
            const ssize_t count = ::read(
                fd,
                buffer.data(),
                std::min(buffer.size(), remaining));
            if (count > 0) {
                value.append(buffer.data(), static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) break;
            if (errno == EINTR) continue;
            ok = false;
            error = "Unable to read data from the Wayland clipboard portal.";
            break;
        }
        ::close(fd);

        if (!ok) return false;
        text = std::move(value);
        return true;
    }

    bool startPortalSession(std::string& error) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        const auto handleToken = tokenPart("hi5start");
        g_variant_builder_add(
            &options,
            "{sv}",
            "handle_token",
            g_variant_new_string(handleToken.c_str()));

        auto reply = portalRequest(
            kRemoteDesktopInterface,
            "Start",
            g_variant_new(
                "(osa{sv})",
                sessionHandle_.c_str(),
                "",
                &options),
            120000,
            error);
        if (reply.response != 0 || !reply.results) return false;

        if (portalPersistenceSupported_) {
            const gchar* refreshedToken = nullptr;
            if (g_variant_lookup(
                    reply.results,
                    "restore_token",
                    "&s",
                    &refreshedToken) &&
                refreshedToken &&
                *refreshedToken) {
                restoreToken_ = refreshedToken;
                if (saveWaylandRestoreToken(restoreToken_)) {
                    std::cerr
                        << "[wayland-portal] persistent permission token refreshed\n";
                } else {
                    std::cerr
                        << "[wayland-portal] unable to persist refreshed permission token\n";
                }
            } else if (!restoreToken_.empty()) {
                // Restore tokens are single-use. If a successful Start did not
                // issue a replacement, do not keep retrying a stale token.
                restoreToken_.clear();
                clearWaylandRestoreToken();
                std::cerr
                    << "[wayland-portal] persistence not granted; cleared stale restore token\n";
            }
        }

        guint32 devices = 0;
        (void)g_variant_lookup(reply.results, "devices", "u", &devices);

        gboolean clipboardEnabled = FALSE;
        if (clipboardRequested_) {
            (void)g_variant_lookup(
                reply.results,
                "clipboard_enabled",
                "b",
                &clipboardEnabled);
        }
        clipboardEnabled_ = clipboardEnabled == TRUE;

        if ((devices & 2U) == 0U) {
            error = "Wayland portal did not grant pointer control.";
            return false;
        }

        GVariant* streams = g_variant_lookup_value(
            reply.results,
            "streams",
            G_VARIANT_TYPE("a(ua{sv})"));
        if (!streams) {
            error = "Wayland portal did not return a screen capture stream.";
            return false;
        }

        GVariantIter iterator;
        g_variant_iter_init(&iterator, streams);
        guint32 nodeId = 0;
        GVariant* properties = nullptr;
        const gboolean found =
            g_variant_iter_next(&iterator, "(u@a{sv})", &nodeId, &properties);
        g_variant_unref(streams);

        if (!found || !nodeId || !properties) {
            if (properties) g_variant_unref(properties);
            error = "Wayland portal returned an empty PipeWire stream list.";
            return false;
        }

        streamNodeId_ = nodeId;

        GVariant* size = g_variant_lookup_value(
            properties,
            "size",
            G_VARIANT_TYPE("(ii)"));
        if (size) {
            gint32 width = 0;
            gint32 height = 0;
            g_variant_get(size, "(ii)", &width, &height);
            logicalWidth_ = std::max(0, static_cast<int>(width));
            logicalHeight_ = std::max(0, static_cast<int>(height));
            g_variant_unref(size);
        }
        g_variant_unref(properties);
        return true;
    }

    bool openPipeWireRemote(std::string& error) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

        GUnixFDList* fdList = nullptr;
        GError* callError = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            bus_,
            kPortalName,
            kPortalPath,
            kScreenCastInterface,
            "OpenPipeWireRemote",
            g_variant_new(
                "(oa{sv})",
                sessionHandle_.c_str(),
                &options),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            15000,
            nullptr,
            &fdList,
            nullptr,
            &callError);

        if (!result || !fdList) {
            if (result) g_variant_unref(result);
            if (fdList) g_object_unref(fdList);
            error = glibErrorMessage(
                callError,
                "Wayland portal could not open the PipeWire remote.");
            return false;
        }

        gint32 handle = -1;
        g_variant_get(result, "(h)", &handle);
        g_variant_unref(result);

        GError* fdError = nullptr;
        pipeWireFd_ = g_unix_fd_list_get(fdList, handle, &fdError);
        g_object_unref(fdList);
        if (pipeWireFd_ < 0) {
            error = glibErrorMessage(
                fdError,
                "Wayland portal returned an invalid PipeWire file descriptor.");
            return false;
        }
        return true;
    }

    static void onPipeWireParamChanged(
        void* data,
        uint32_t id,
        const struct spa_pod* param) {

        auto* self = static_cast<WaylandPortalRemotePlatform*>(data);
        if (!self || !param || id != SPA_PARAM_Format) return;

        spa_video_info_raw parsed {};
        if (spa_format_video_raw_parse(param, &parsed) < 0) return;

        {
            std::lock_guard<std::mutex> lock(self->frameMutex_);
            self->videoInfo_ = parsed;
            self->videoInfoValid_ = true;
        }

        // GNOME/Mutter commonly prefers DMA-BUF for portal screencasts.
        // The VP8 software encoder needs CPU-addressable pixels, so request a
        // PipeWire buffer pool backed by MemPtr/MemFd. With MAP_BUFFERS,
        // MemFd buffers are mapped and spa_data::data becomes readable.
        if (self->pipeWireStream_ && parsed.size.width > 0 && parsed.size.height > 0) {
            const int stride = static_cast<int>(parsed.size.width) * 4;
            const int size = stride * static_cast<int>(parsed.size.height);
            std::uint8_t bufferBytes[1024];
            spa_pod_builder builder =
                SPA_POD_BUILDER_INIT(bufferBytes, sizeof(bufferBytes));
            const spa_pod* bufferParams[1];
            bufferParams[0] = static_cast<const spa_pod*>(
                spa_pod_builder_add_object(
                    &builder,
                    SPA_TYPE_OBJECT_ParamBuffers,
                    SPA_PARAM_Buffers,
                    SPA_PARAM_BUFFERS_buffers,
                    SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                    SPA_PARAM_BUFFERS_blocks,
                    SPA_POD_Int(1),
                    SPA_PARAM_BUFFERS_size,
                    SPA_POD_Int(size),
                    SPA_PARAM_BUFFERS_stride,
                    SPA_POD_Int(stride),
                    SPA_PARAM_BUFFERS_dataType,
                    SPA_POD_CHOICE_FLAGS_Int(
                        (1 << SPA_DATA_MemPtr) |
                        (1 << SPA_DATA_MemFd))));
            pw_stream_update_params(self->pipeWireStream_, bufferParams, 1);
        }
    }

    static void onPipeWireProcess(void* data) {
        auto* self = static_cast<WaylandPortalRemotePlatform*>(data);
        if (!self || !self->pipeWireStream_) return;

        static std::atomic<std::uint64_t> processCallbacks{0};
        const auto callbackNumber = ++processCallbacks;
        pw_buffer* pipeBuffer = pw_stream_dequeue_buffer(self->pipeWireStream_);
        if (!pipeBuffer) {
            if (callbackNumber <= 3) {
                std::cerr << "[wayland-capture] process without buffer n="
                          << callbackNumber << "\n";
            }
            return;
        }

        spa_buffer* buffer = pipeBuffer->buffer;
        if (buffer && buffer->n_datas > 0) {
            spa_data& source = buffer->datas[0];
            spa_video_info_raw info {};
            bool valid = false;
            {
                std::lock_guard<std::mutex> lock(self->frameMutex_);
                info = self->videoInfo_;
                valid = self->videoInfoValid_;
            }

            if (callbackNumber <= 3) {
                std::cerr << "[wayland-capture] process n=" << callbackNumber
                          << " valid=" << (valid ? 1 : 0)
                          << " type=" << source.type
                          << " fd=" << source.fd
                          << " data=" << (source.data ? 1 : 0)
                          << " maxsize=" << source.maxsize
                          << " chunk=" << (source.chunk ? 1 : 0)
                          << " chunk_size=" << (source.chunk ? source.chunk->size : 0)
                          << " chunk_offset=" << (source.chunk ? source.chunk->offset : 0)
                          << " stride=" << (source.chunk ? source.chunk->stride : 0)
                          << " format=" << static_cast<int>(info.format)
                          << " size=" << info.size.width << "x" << info.size.height
                          << "\n";
            }

            if (valid &&
                source.data &&
                source.chunk &&
                source.chunk->size > 0 &&
                info.size.width >= 2 &&
                info.size.height >= 2 &&
                (info.format == SPA_VIDEO_FORMAT_BGRx ||
                 info.format == SPA_VIDEO_FORMAT_BGRA ||
                 info.format == SPA_VIDEO_FORMAT_RGBx ||
                 info.format == SPA_VIDEO_FORMAT_RGBA)) {

                const int width = static_cast<int>(info.size.width);
                const int height = static_cast<int>(info.size.height);
                int stride = source.chunk->stride;
                if (!stride) stride = width * 4;
                const auto absoluteStride =
                    static_cast<std::size_t>(std::abs(stride));
                const auto required =
                    absoluteStride * static_cast<std::size_t>(height);
                const auto offset =
                    static_cast<std::size_t>(source.chunk->offset);
                if (offset <= source.maxsize &&
                    required <= source.maxsize - offset) {
                    const auto* pixels =
                        static_cast<const std::uint8_t*>(source.data) + offset;
                    auto frame = raw32ToI420(
                        pixels,
                        width,
                        height,
                        stride,
                        info.format);
                    if (frame.width > 0 && !frame.y.empty()) {
                        static std::atomic<std::uint64_t> convertedFrames{0};
                        const auto convertedNumber = ++convertedFrames;
                        if (convertedNumber <= 3) {
                            std::cerr << "[wayland-capture] converted frame n="
                                      << convertedNumber
                                      << " size=" << frame.width << "x" << frame.height
                                      << " y_bytes=" << frame.y.size() << "\n";
                        }
                        std::lock_guard<std::mutex> lock(self->frameMutex_);
                        self->latestFrame_ = std::move(frame);
                        ++self->latestFrameId_;
                    }
                }
            }
        }

        pw_stream_queue_buffer(self->pipeWireStream_, pipeBuffer);
    }

    bool startPipeWire(std::string& error) {
        pw_init(nullptr, nullptr);

        pipeWireLoop_ = pw_thread_loop_new(
            "hi5central-wayland-capture",
            nullptr);
        if (!pipeWireLoop_) {
            error = "Unable to create the PipeWire capture loop.";
            return false;
        }

        pipeWireContext_ = pw_context_new(
            pw_thread_loop_get_loop(pipeWireLoop_),
            nullptr,
            0);
        if (!pipeWireContext_) {
            error = "Unable to create the PipeWire context.";
            return false;
        }

        pipeWireCore_ = pw_context_connect_fd(
            pipeWireContext_,
            pipeWireFd_,
            nullptr,
            0);
        if (!pipeWireCore_) {
            error = "Unable to connect to the portal PipeWire remote.";
            return false;
        }
        pipeWireFd_ = -1;

        pipeWireStream_ = pw_stream_new(
            pipeWireCore_,
            "Hi5Central Wayland Capture",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Video",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Screen",
                nullptr));
        if (!pipeWireStream_) {
            error = "Unable to create the PipeWire capture stream.";
            return false;
        }

        static const pw_stream_events events = [] {
            pw_stream_events value {};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.param_changed = onPipeWireParamChanged;
            value.process = onPipeWireProcess;
            return value;
        }();
        pw_stream_add_listener(
            pipeWireStream_,
            &pipeWireStreamListener_,
            &events,
            this);

        std::uint8_t podBuffer[2048];
        spa_pod_builder builder =
            SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        const spa_rectangle defaultSize = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        const spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);
        // GNOME/Mutter portal streams commonly advertise a variable
        // framerate as 0/1 with maxFramerate carrying the supported range.
        // Requiring framerate >= 1 here makes PipeWire reject an otherwise
        // compatible BGRx/BGRA stream with "no more input formats".
        const spa_fraction variableRate = SPA_FRACTION(0, 1);
        const spa_fraction defaultMaxRate = SPA_FRACTION(30, 1);
        const spa_fraction minRate = SPA_FRACTION(1, 1);
        const spa_fraction maxRate = SPA_FRACTION(60, 1);

        const spa_pod* params[1];
        params[0] = static_cast<const spa_pod*>(
            spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format,
                SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType,
                SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,
                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,
                SPA_POD_CHOICE_ENUM_Id(
                    5,
                    SPA_VIDEO_FORMAT_BGRx,
                    SPA_VIDEO_FORMAT_BGRx,
                    SPA_VIDEO_FORMAT_BGRA,
                    SPA_VIDEO_FORMAT_RGBx,
                    SPA_VIDEO_FORMAT_RGBA),
                SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(
                    &defaultSize,
                    &minSize,
                    &maxSize),
                SPA_FORMAT_VIDEO_framerate,
                SPA_POD_Fraction(&variableRate),
                SPA_FORMAT_VIDEO_maxFramerate,
                SPA_POD_CHOICE_RANGE_Fraction(
                    &defaultMaxRate,
                    &minRate,
                    &maxRate)));

        const int connectResult = pw_stream_connect(
            pipeWireStream_,
            PW_DIRECTION_INPUT,
            streamNodeId_,
            static_cast<pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS),
            params,
            1);
        if (connectResult < 0) {
            error = "Unable to connect the PipeWire screen-capture stream.";
            return false;
        }

        if (pw_thread_loop_start(pipeWireLoop_) < 0) {
            error = "Unable to start the PipeWire capture loop.";
            return false;
        }
        pipeWireLoopStarted_ = true;
        return true;
    }

    bool notify(
        const char* method,
        GVariant* parameters,
        std::string& error) {

        GError* callError = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            bus_,
            kPortalName,
            kPortalPath,
            kRemoteDesktopInterface,
            method,
            parameters,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            nullptr,
            &callError);
        if (!result) {
            error = glibErrorMessage(
                callError,
                std::string("Wayland input call failed: ") + method);
            return false;
        }
        g_variant_unref(result);
        return true;
    }

    bool notifyPointerAbsolute(
        double x,
        double y,
        std::string& error) {

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        return notify(
            "NotifyPointerMotionAbsolute",
            g_variant_new(
                "(oa{sv}udd)",
                sessionHandle_.c_str(),
                &options,
                streamNodeId_,
                x,
                y),
            error);
    }

    bool notifyPointerButton(
        int button,
        guint32 state,
        std::string& error) {

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        return notify(
            "NotifyPointerButton",
            g_variant_new(
                "(oa{sv}iu)",
                sessionHandle_.c_str(),
                &options,
                button,
                state),
            error);
    }

    bool notifyPointerAxis(
        double dx,
        double dy,
        std::string& error) {

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        return notify(
            "NotifyPointerAxis",
            g_variant_new(
                "(oa{sv}dd)",
                sessionHandle_.c_str(),
                &options,
                dx,
                dy),
            error);
    }

    bool notifyKeysym(
        std::int32_t keysym,
        guint32 state,
        std::string& error) {

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        return notify(
            "NotifyKeyboardKeysym",
            g_variant_new(
                "(oa{sv}iu)",
                sessionHandle_.c_str(),
                &options,
                keysym,
                state),
            error);
    }

    void stopLocked() {
        if (pipeWireLoop_ && pipeWireLoopStarted_) {
            pw_thread_loop_stop(pipeWireLoop_);
            pipeWireLoopStarted_ = false;
        }
        if (pipeWireStream_) {
            pw_stream_destroy(pipeWireStream_);
            pipeWireStream_ = nullptr;
        }
        if (pipeWireCore_) {
            pw_core_disconnect(pipeWireCore_);
            pipeWireCore_ = nullptr;
        }
        if (pipeWireContext_) {
            pw_context_destroy(pipeWireContext_);
            pipeWireContext_ = nullptr;
        }
        if (pipeWireLoop_) {
            pw_thread_loop_destroy(pipeWireLoop_);
            pipeWireLoop_ = nullptr;
        }
        if (pipeWireFd_ >= 0) {
            ::close(pipeWireFd_);
            pipeWireFd_ = -1;
        }

        if (bus_ && !sessionHandle_.empty()) {
            GError* ignored = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                bus_,
                kPortalName,
                sessionHandle_.c_str(),
                kSessionInterface,
                "Close",
                nullptr,
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                3000,
                nullptr,
                &ignored);
            if (result) g_variant_unref(result);
            if (ignored) g_error_free(ignored);
        }

        if (bus_) {
            g_object_unref(bus_);
            bus_ = nullptr;
        }

        sessionHandle_.clear();
        clipboardRequested_ = false;
        clipboardEnabled_ = false;
        restoreToken_.clear();
        portalIdentityRegistered_ = false;
        portalPersistenceSupported_ = false;
        remoteDesktopPortalVersion_ = 0;
        streamNodeId_ = 0;
        logicalWidth_ = 0;
        logicalHeight_ = 0;
        started_ = false;

        std::lock_guard<std::mutex> frameLock(frameMutex_);
        latestFrame_ = {};
        latestFrameId_ = 0;
        deliveredFrameId_ = 0;
        videoInfo_ = {};
        videoInfoValid_ = false;
    }

    mutable std::mutex stateMutex_;
    mutable std::mutex frameMutex_;
    GDBusConnection* bus_ = nullptr;
    std::string sessionHandle_;
    std::string restoreToken_;
    bool persistentAccessRequested_ = false;
    bool portalIdentityRegistered_ = false;
    bool portalPersistenceSupported_ = false;
    guint32 remoteDesktopPortalVersion_ = 0;
    bool clipboardRequested_ = false;
    bool clipboardEnabled_ = false;
    guint32 streamNodeId_ = 0;
    int logicalWidth_ = 0;
    int logicalHeight_ = 0;
    int pipeWireFd_ = -1;

    pw_thread_loop* pipeWireLoop_ = nullptr;
    pw_context* pipeWireContext_ = nullptr;
    pw_core* pipeWireCore_ = nullptr;
    pw_stream* pipeWireStream_ = nullptr;
    spa_hook pipeWireStreamListener_ {};
    bool pipeWireLoopStarted_ = false;

    spa_video_info_raw videoInfo_ {};
    bool videoInfoValid_ = false;
    I420Frame latestFrame_;
    std::uint64_t latestFrameId_ = 0;
    std::uint64_t deliveredFrameId_ = 0;
    bool started_ = false;
};

} // namespace

std::unique_ptr<RemotePlatform> createWaylandPortalRemotePlatform() {
    return std::make_unique<WaylandPortalRemotePlatform>();
}

} // namespace hi5

#endif
