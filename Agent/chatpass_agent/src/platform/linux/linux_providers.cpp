#include "platform/screen_provider.h"
#include "platform/input_provider.h"
#include "platform/inventory_provider.h"

#if defined(__linux__)

namespace hi5 {

class LinuxScreenCaptureProvider final : public IScreenCaptureProvider {
public:
    ScreenCaptureCapabilities capabilities() const override {
        ScreenCaptureCapabilities caps;
        caps.available = false;
        caps.requiresUserSession = true;
        caps.providerName = "linux-placeholder";
        return caps;
    }
};

class LinuxInputProvider final : public IInputProvider {
public:
    InputCapabilities capabilities() const override {
        InputCapabilities caps;
        caps.available = false;
        caps.requiresUserSession = true;
        caps.providerName = "linux-placeholder";
        return caps;
    }
};

class LinuxInventoryProvider final : public IInventoryProvider {
public:
    InventoryCapabilities capabilities() const override {
        InventoryCapabilities caps;
        caps.available = false;
        caps.providerName = "linux-placeholder";
        return caps;
    }
};

std::unique_ptr<IScreenCaptureProvider> CreateScreenCaptureProvider() {
    return std::make_unique<LinuxScreenCaptureProvider>();
}

std::unique_ptr<IInputProvider> CreateInputProvider() {
    return std::make_unique<LinuxInputProvider>();
}

std::unique_ptr<IInventoryProvider> CreateInventoryProvider() {
    return std::make_unique<LinuxInventoryProvider>();
}

} // namespace hi5

#endif