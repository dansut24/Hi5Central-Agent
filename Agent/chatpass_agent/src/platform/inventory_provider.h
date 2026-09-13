#pragma once

#include <memory>
#include <string>

namespace hi5 {

struct InventoryCapabilities {
    bool available = false;
    std::string providerName;
};

class IInventoryProvider {
public:
    virtual ~IInventoryProvider() = default;

    virtual InventoryCapabilities capabilities() const = 0;
};

std::unique_ptr<IInventoryProvider> CreateInventoryProvider();

} // namespace hi5