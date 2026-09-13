#pragma once

#include <memory>
#include <string>

namespace hi5 {

struct InputCapabilities {
    bool available = false;
    bool requiresUserSession = true;
    std::string providerName;
};

class IInputProvider {
public:
    virtual ~IInputProvider() = default;

    virtual InputCapabilities capabilities() const = 0;
};

std::unique_ptr<IInputProvider> CreateInputProvider();

} // namespace hi5