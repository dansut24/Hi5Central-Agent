#pragma once

#include "remote_frame.h"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hi5 {

class RemotePlatform {
public:
    virtual ~RemotePlatform() = default;

    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;

    virtual FrameCaptureResult capture() = 0;
    virtual std::vector<DisplayInfo> displays() const = 0;
    virtual int currentDisplayIndex() const = 0;
    virtual bool setDisplayIndex(int index) = 0;

    virtual bool handleInput(const nlohmann::json& message, std::string& error) = 0;

    virtual std::string backendName() const = 0;
    virtual bool requiresConsent() const = 0;
};

std::unique_ptr<RemotePlatform> createRemotePlatform();

} // namespace hi5