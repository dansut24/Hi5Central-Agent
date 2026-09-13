#include "platform/screen_provider.h"
#include "platform/input_provider.h"
#include "platform/inventory_provider.h"

#if defined(__APPLE__)

namespace hi5 {

class MacOSScreenCaptureProvider final : public IScreenCaptureProvider {
public:
    ScreenCaptureCapabilities capabilities() const override {
        ScreenCaptureCapabilities caps;
        caps.available = false;
        caps.requiresUserSession = true;
        caps.providerName = "macos-placeholder";
        return caps;
    }
};

class MacOSInputProvider final : public IInputProvider {
public:
    InputCapabilities capabilities() const override {
        InputCapabilities caps;
        caps.available = false;
        caps.requiresUserSession = true;
        caps.providerName = "macos-placeholder";
        return caps;
    }
};

class MacOSInventoryProvider final : public IInventoryProvider {
public:
    InventoryCapabilities capabilities() const override {
        InventoryCapabilities caps;
        caps.available = false;
        caps.providerName = "macos-placeholder";
        return caps;
    }
};

std::unique_ptr<IScreenCaptureProvider> CreateScreenCaptureProvider() {
    return std::make_unique<MacOSScreenCaptureProvider>();
}

std::unique_ptr<IInputProvider> CreateInputProvider() {
    return std::make_unique<MacOSInputProvider>();
}

std::unique_ptr<IInventoryProvider> CreateInventoryProvider() {
    return std::make_unique<MacOSInventoryProvider>();
}

} // namespace hi5

#endif