#include "remote_platform.h"

#if defined(__APPLE__)

namespace hi5 {
namespace {

class MacRemotePlatformPlaceholder final : public RemotePlatform {
public:
    bool start(std::string& error) override {
        error = "macOS ScreenCaptureKit remote provider is not initialized in this build.";
        return false;
    }
    void stop() override {}
    FrameCaptureResult capture() override { return {}; }
    std::vector<DisplayInfo> displays() const override { return {}; }
    int currentDisplayIndex() const override { return 0; }
    bool setDisplayIndex(int) override { return false; }
    bool handleInput(const nlohmann::json&, std::string& error) override {
        error = "macOS input provider is not initialized.";
        return false;
    }
    std::string backendName() const override { return "macos_screencapturekit"; }
    bool requiresConsent() const override { return true; }
};

} // namespace

std::unique_ptr<RemotePlatform> createRemotePlatform() {
    return std::make_unique<MacRemotePlatformPlaceholder>();
}

} // namespace hi5

#endif