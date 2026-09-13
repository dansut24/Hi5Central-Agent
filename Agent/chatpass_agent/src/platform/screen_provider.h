#pragma once

#include <memory>
#include <string>

namespace hi5 {

struct ScreenCaptureCapabilities {
    bool available = false;
    bool requiresUserSession = true;
    std::string providerName;
};

class IScreenCaptureProvider {
public:
    virtual ~IScreenCaptureProvider() = default;

    virtual ScreenCaptureCapabilities capabilities() const = 0;
};

std::unique_ptr<IScreenCaptureProvider> CreateScreenCaptureProvider();

} // namespace hi5