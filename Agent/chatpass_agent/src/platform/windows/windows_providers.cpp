#include "platform/screen_provider.h"
#include "platform/input_provider.h"
#include "platform/inventory_provider.h"

#ifdef _WIN32

namespace hi5 {

class WindowsScreenCaptureProvider final : public IScreenCaptureProvider {
public:
    ScreenCaptureCapabilities capabilities() const override {
        ScreenCaptureCapabilities caps;
        caps.available = true;
        caps.requiresUserSession = true;
        caps.providerName = "windows-dxgi";
        return caps;
    }
};

class WindowsInputProvider final : public IInputProvider {
public:
    InputCapabilities capabilities() const override {
        InputCapabilities caps;
        caps.available = true;
        caps.requiresUserSession = true;
        caps.providerName = "windows-sendinput";
        return caps;
    }
};

class WindowsInventoryProvider final : public IInventoryProvider {
public:
    InventoryCapabilities capabilities() const override {
        InventoryCapabilities caps;
        caps.available = true;
        caps.providerName = "windows-wmi-registry";
        return caps;
    }
};

std::unique_ptr<IScreenCaptureProvider> CreateScreenCaptureProvider() {
    return std::make_unique<WindowsScreenCaptureProvider>();
}

std::unique_ptr<IInputProvider> CreateInputProvider() {
    return std::make_unique<WindowsInputProvider>();
}

std::unique_ptr<IInventoryProvider> CreateInventoryProvider() {
    return std::make_unique<WindowsInventoryProvider>();
}

} // namespace hi5

#endif