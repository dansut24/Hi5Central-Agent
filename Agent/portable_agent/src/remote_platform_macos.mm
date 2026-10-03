#include "remote_platform.h"

#if defined(__APPLE__)

#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hi5 {
namespace {

std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

I420Frame bgraToI420(CVPixelBufferRef pixelBuffer) {
    I420Frame frame;
    if (!pixelBuffer) return frame;
    if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_32BGRA) return frame;

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    const int sourceWidth = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
    const int sourceHeight = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
    const int width = sourceWidth & ~1;
    const int height = sourceHeight & ~1;
    const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const std::size_t stride = CVPixelBufferGetBytesPerRow(pixelBuffer);

    if (!base || width < 2 || height < 2) {
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        return frame;
    }

    frame.width = width;
    frame.height = height;
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    frame.y.resize(static_cast<std::size_t>(width) * height);
    frame.u.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);
    frame.v.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);

    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;

            for (int dy = 0; dy < 2; ++dy) {
                const auto* row = base + static_cast<std::size_t>(y + dy) * stride;
                for (int dx = 0; dx < 2; ++dx) {
                    const auto* px = row + static_cast<std::size_t>(x + dx) * 4;
                    const int b = px[0];
                    const int g = px[1];
                    const int r = px[2];
                    sumR += r;
                    sumG += g;
                    sumB += b;

                    const int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    frame.y[static_cast<std::size_t>(y + dy) * width + x + dx] = clampByte(yy);
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

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    return frame;
}

bool isLikelyBlackFrame(const I420Frame& frame) {
    if (frame.y.empty()) return false;

    const std::size_t step = std::max<std::size_t>(1, frame.y.size() / 2048);
    std::uint64_t sum = 0;
    std::uint8_t maxY = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < frame.y.size(); i += step) {
        const auto y = frame.y[i];
        sum += y;
        maxY = std::max(maxY, y);
        ++count;
    }
    if (!count) return false;

    const double average = static_cast<double>(sum) / static_cast<double>(count);
    return average <= 18.5 && maxY <= 24;
}

struct H264CaptureSize {
    int width = 0;
    int height = 0;
};

H264CaptureSize h264Level31CaptureSize(int sourceWidth, int sourceHeight) {
    sourceWidth &= ~1;
    sourceHeight &= ~1;
    if (sourceWidth < 2 || sourceHeight < 2) return {};

    constexpr double maxWidth = 1280.0;
    constexpr double maxHeight = 720.0;
    const double scale = std::min({
        1.0,
        maxWidth / static_cast<double>(sourceWidth),
        maxHeight / static_cast<double>(sourceHeight)
    });

    int width = std::max(2, static_cast<int>(
        std::floor(static_cast<double>(sourceWidth) * scale / 2.0)) * 2);
    int height = std::max(2, static_cast<int>(
        std::floor(static_cast<double>(sourceHeight) * scale / 2.0)) * 2);

    auto macroblocks = [](int value) { return (value + 15) / 16; };
    while (macroblocks(width) * macroblocks(height) > 3600) {
        if (width >= height && width > 2) width -= 2;
        else if (height > 2) height -= 2;
        else break;
    }

    return {width, height};
}

I420Frame cgImageToI420(CGImageRef image) {
    I420Frame frame;
    if (!image) return frame;

    const int sourceWidth = static_cast<int>(CGImageGetWidth(image)) & ~1;
    const int sourceHeight = static_cast<int>(CGImageGetHeight(image)) & ~1;
    const auto target = h264Level31CaptureSize(sourceWidth, sourceHeight);
    const int width = target.width;
    const int height = target.height;
    if (width < 2 || height < 2) return frame;

    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> bgra(stride * static_cast<std::size_t>(height));

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        bgra.data(),
        static_cast<std::size_t>(width),
        static_cast<std::size_t>(height),
        8,
        stride,
        colorSpace,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(colorSpace);

    if (!context) return frame;

    // CGImage drawing uses a bottom-left coordinate system by default.
    CGContextTranslateCTM(context, 0, height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);

    frame.width = width;
    frame.height = height;
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    frame.y.resize(static_cast<std::size_t>(width) * height);
    frame.u.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);
    frame.v.resize(static_cast<std::size_t>(chromaWidth) * chromaHeight);

    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;

            for (int dy = 0; dy < 2; ++dy) {
                const auto* row = bgra.data() + static_cast<std::size_t>(y + dy) * stride;
                for (int dx = 0; dx < 2; ++dx) {
                    const auto* px = row + static_cast<std::size_t>(x + dx) * 4;
                    const int b = px[0];
                    const int g = px[1];
                    const int r = px[2];
                    sumR += r;
                    sumG += g;
                    sumB += b;
                    const int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    frame.y[static_cast<std::size_t>(y + dy) * width + x + dx] = clampByte(yy);
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

struct CaptureState {
    std::mutex mutex;
    I420Frame latestFrame;
    std::uint64_t frameId = 0;
    bool hasFrame = false;
};

} // namespace
} // namespace hi5

@interface Hi5ScreenCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) void* captureState;
@end

@implementation Hi5ScreenCaptureOutput

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
    ofType:(SCStreamOutputType)type API_AVAILABLE(macos(12.3)) {
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !sampleBuffer || !CMSampleBufferDataIsReady(sampleBuffer)) return;

    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixelBuffer) return;

    auto* state = static_cast<hi5::CaptureState*>(self.captureState);
    if (!state) return;

    auto frame = hi5::bgraToI420(pixelBuffer);
    if (frame.width < 2 || frame.height < 2) return;

    static std::atomic<std::uint64_t> captureFrameCount{0};
    const auto captureIndex = ++captureFrameCount;
    if (captureIndex <= 3 || (captureIndex % 300) == 0) {
        std::uint64_t sample = 0;
        if (!frame.y.empty()) {
            const std::size_t step = std::max<std::size_t>(1, frame.y.size() / 1024);
            for (std::size_t i = 0; i < frame.y.size(); i += step) sample += frame.y[i];
        }
        NSLog(@"Hi5Central remote capture frame=%llu size=%dx%d luma_sample=%llu",
              static_cast<unsigned long long>(captureIndex),
              frame.width,
              frame.height,
              static_cast<unsigned long long>(sample));
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->latestFrame = std::move(frame);
    state->hasFrame = true;
    ++state->frameId;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error API_AVAILABLE(macos(12.3)) {
    (void)stream;
    if (error) {
        NSLog(@"Hi5Central ScreenCaptureKit stopped: %@", error);
    }
}

@end

namespace hi5 {
namespace {

CGKeyCode keyCodeForBrowserCode(const std::string& code) {
    static const std::map<std::string, CGKeyCode> keys = {
        {"KeyA",0x00},{"KeyS",0x01},{"KeyD",0x02},{"KeyF",0x03},{"KeyH",0x04},{"KeyG",0x05},
        {"KeyZ",0x06},{"KeyX",0x07},{"KeyC",0x08},{"KeyV",0x09},{"KeyB",0x0B},{"KeyQ",0x0C},
        {"KeyW",0x0D},{"KeyE",0x0E},{"KeyR",0x0F},{"KeyY",0x10},{"KeyT",0x11},
        {"Digit1",0x12},{"Digit2",0x13},{"Digit3",0x14},{"Digit4",0x15},{"Digit6",0x16},{"Digit5",0x17},
        {"Equal",0x18},{"Digit9",0x19},{"Digit7",0x1A},{"Minus",0x1B},{"Digit8",0x1C},{"Digit0",0x1D},
        {"BracketRight",0x1E},{"KeyO",0x1F},{"KeyU",0x20},{"BracketLeft",0x21},{"KeyI",0x22},
        {"KeyP",0x23},{"Enter",0x24},{"KeyL",0x25},{"KeyJ",0x26},{"Quote",0x27},{"KeyK",0x28},
        {"Semicolon",0x29},{"Backslash",0x2A},{"Comma",0x2B},{"Slash",0x2C},{"KeyN",0x2D},
        {"KeyM",0x2E},{"Period",0x2F},{"Tab",0x30},{"Space",0x31},{"Backquote",0x32},
        {"Backspace",0x33},{"Escape",0x35},{"MetaLeft",0x37},{"ShiftLeft",0x38},{"CapsLock",0x39},
        {"AltLeft",0x3A},{"ControlLeft",0x3B},{"ShiftRight",0x3C},{"AltRight",0x3D},{"ControlRight",0x3E},
        {"F5",0x60},{"F6",0x61},{"F7",0x62},{"F3",0x63},{"F8",0x64},{"F9",0x65},{"F11",0x67},
        {"F10",0x6D},{"F12",0x6F},{"Home",0x73},{"PageUp",0x74},{"Delete",0x75},{"F4",0x76},
        {"End",0x77},{"F2",0x78},{"PageDown",0x79},{"F1",0x7A},{"ArrowLeft",0x7B},
        {"ArrowRight",0x7C},{"ArrowDown",0x7D},{"ArrowUp",0x7E}
    };
    const auto it = keys.find(code);
    return it == keys.end() ? static_cast<CGKeyCode>(UINT16_MAX) : it->second;
}

class MacRemotePlatform final : public RemotePlatform {
public:
    ~MacRemotePlatform() override { stop(); }

    bool start(std::string& error) override;
    void stop() override;
    FrameCaptureResult capture() override;
    std::vector<DisplayInfo> displays() const override;
    int currentDisplayIndex() const override { return currentDisplayIndex_; }
    bool setDisplayIndex(int index) override;
    bool handleInput(const nlohmann::json& message, std::string& error) override;
    std::string backendName() const override {
        return useWindowServerFallback_ ? "macos_windowserver" : "macos_screencapturekit";
    }
    bool requiresConsent() const override { return true; }

private:
    bool loadDisplays(std::string& error);
    bool startCaptureForIndex(int index, std::string& error);
    I420Frame captureWindowServerComposite() const;
    void stopStreamOnly();
    CGPoint pointFromMessage(const nlohmann::json& message) const;

    CaptureState state_;
    mutable std::mutex controlMutex_;
    NSArray<SCDisplay*>* __strong displays_ = nil;
    SCStream* __strong stream_ = nil;
    Hi5ScreenCaptureOutput* __strong output_ = nil;
    dispatch_queue_t captureQueue_ = nullptr;
    int currentDisplayIndex_ = 0;
    std::uint64_t lastReturnedFrameId_ = 0;
    std::uint64_t fallbackFrameId_ = 0;
    int consecutiveBlackFrames_ = 0;
    CGDirectDisplayID fallbackDisplayId_ = kCGNullDirectDisplay;
    bool useWindowServerFallback_ = false;
    bool accessibilityTrusted_ = false;
    bool screenCaptureTrusted_ = false;
};
bool MacRemotePlatform::start(std::string& error) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    if (@available(macOS 12.3, *)) {
        const CGDirectDisplayID mainDisplay = CGMainDisplayID();
        const std::uint32_t displayVendor = CGDisplayVendorNumber(mainDisplay);
        const bool vmwareDisplay = displayVendor == 0x15ad;
        const bool screenPreflight = CGPreflightScreenCaptureAccess();

        accessibilityTrusted_ = AXIsProcessTrusted();
        if (!accessibilityTrusted_) {
            NSDictionary* options = @{
                (__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES
            };
            accessibilityTrusted_ =
                AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
        }

        std::string captureError;
        bool captureReady = false;

        if (vmwareDisplay) {
            // VMware's virtual SVGA display can either return black
            // ScreenCaptureKit frames or hang SCShareableContent enumeration
            // after a reboot. Bypass ReplayKit/ScreenCaptureKit completely and
            // capture WindowServer's composited on-screen windows instead.
            screenCaptureTrusted_ = screenPreflight;
            if (!screenCaptureTrusted_) {
                screenCaptureTrusted_ = CGRequestScreenCaptureAccess();
            }

            if (screenCaptureTrusted_) {
                fallbackDisplayId_ = mainDisplay;
                currentDisplayIndex_ = 0;
                useWindowServerFallback_ = true;
                fallbackFrameId_ = 0;
                consecutiveBlackFrames_ = 0;

                I420Frame probe = captureWindowServerComposite();
                captureReady = probe.width > 0 && probe.height > 0;
                if (!captureReady) {
                    captureError =
                        "WindowServer could not capture the VMware virtual display.";
                    useWindowServerFallback_ = false;
                    fallbackDisplayId_ = kCGNullDirectDisplay;
                    screenCaptureTrusted_ = false;
                }
            }

            NSLog(@"Hi5Central VMware display vendor=0x%x direct_windowserver=%d screen_preflight=%d accessibility=%d",
                  displayVendor,
                  captureReady ? 1 : 0,
                  screenPreflight ? 1 : 0,
                  accessibilityTrusted_ ? 1 : 0);
        } else {
            captureReady = loadDisplays(captureError);
            if (captureReady) {
                captureReady = startCaptureForIndex(currentDisplayIndex_, captureError);
            }

            screenCaptureTrusted_ = captureReady;

            if (!captureReady && !screenPreflight) {
                const bool requested = CGRequestScreenCaptureAccess();
                if (requested) {
                    captureError.clear();
                    captureReady = loadDisplays(captureError);
                    if (captureReady) {
                        captureReady = startCaptureForIndex(currentDisplayIndex_, captureError);
                    }
                    screenCaptureTrusted_ = captureReady;
                }
            }

            NSLog(@"Hi5Central permissions screen_preflight=%d capture_ready=%d accessibility=%d",
                  screenPreflight ? 1 : 0,
                  screenCaptureTrusted_ ? 1 : 0,
                  accessibilityTrusted_ ? 1 : 0);
        }

        if (!screenCaptureTrusted_ || !accessibilityTrusted_ || !captureReady) {
            std::string missing;
            if (!screenCaptureTrusted_ || !captureReady) missing += "Screen Recording";
            if (!accessibilityTrusted_) {
                if (!missing.empty()) missing += " and ";
                missing += "Accessibility";
            }
            error = "macOS permission required: enable " + missing +
                " for Hi5Central Remote Helper in System Settings > Privacy & Security, then retry the remote session.";
            if (!captureError.empty()) {
                error += " Capture error: " + captureError;
            }
            return false;
        }

        return true;
    }

    error = "macOS 12.3 or newer is required for ScreenCaptureKit remote desktop.";
    return false;
}

bool MacRemotePlatform::loadDisplays(std::string& error) {
    __block SCShareableContent* shareable = nil;
    __block NSError* shareableError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                              onScreenWindowsOnly:NO
                                               completionHandler:^(SCShareableContent* content, NSError* err) {
        shareable = content;
        shareableError = err;
        dispatch_semaphore_signal(semaphore);
    }];

    const long waitResult = dispatch_semaphore_wait(
        semaphore,
        dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(5 * NSEC_PER_SEC)));

    if (waitResult != 0) {
        error = "Timed out while asking macOS for shareable displays.";
        return false;
    }
    if (shareableError) {
        error = "ScreenCaptureKit could not enumerate displays: " +
            std::string([[shareableError localizedDescription] UTF8String] ?: "unknown error");
        return false;
    }
    if (!shareable || shareable.displays.count == 0) {
        error = "No macOS display is available for capture.";
        return false;
    }

    displays_ = shareable.displays;
    const CGDirectDisplayID mainDisplay = CGMainDisplayID();
    currentDisplayIndex_ = 0;
    for (NSUInteger i = 0; i < displays_.count; ++i) {
        SCDisplay* display = displays_[i];
        if (display.displayID == mainDisplay) {
            currentDisplayIndex_ = static_cast<int>(i);
            break;
        }
    }
    return true;
}

bool MacRemotePlatform::startCaptureForIndex(int index, std::string& error) {
    if (!displays_ || index < 0 || static_cast<NSUInteger>(index) >= displays_.count) {
        error = "Requested macOS display is not available.";
        return false;
    }

    stopStreamOnly();

    SCDisplay* display = displays_[static_cast<NSUInteger>(index)];
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    const auto target = h264Level31CaptureSize(
        static_cast<int>(display.width),
        static_cast<int>(display.height));
    config.width = target.width;
    config.height = target.height;
    config.minimumFrameInterval = CMTimeMake(1, 30);
    NSLog(@"Hi5Central macOS H264 capture scale source=%ldx%ld stream=%dx%d",
          static_cast<long>(display.width),
          static_cast<long>(display.height),
          target.width,
          target.height);
    config.queueDepth = 5;
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = YES;

    output_ = [[Hi5ScreenCaptureOutput alloc] init];
    output_.captureState = &state_;
    captureQueue_ = dispatch_queue_create("com.hi5central.agent.screencapture", DISPATCH_QUEUE_SERIAL);
    stream_ = [[SCStream alloc] initWithFilter:filter configuration:config delegate:output_];

    NSError* addError = nil;
    if (![stream_ addStreamOutput:output_
                             type:SCStreamOutputTypeScreen
               sampleHandlerQueue:captureQueue_
                            error:&addError]) {
        error = "Unable to attach ScreenCaptureKit output: " +
            std::string([[addError localizedDescription] UTF8String] ?: "unknown error");
        stopStreamOnly();
        return false;
    }

    __block NSError* startError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [stream_ startCaptureWithCompletionHandler:^(NSError* err) {
        startError = err;
        dispatch_semaphore_signal(semaphore);
    }];

    const long waitResult = dispatch_semaphore_wait(
        semaphore,
        dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(5 * NSEC_PER_SEC)));

    if (waitResult != 0) {
        error = "Timed out while starting ScreenCaptureKit.";
        stopStreamOnly();
        return false;
    }
    if (startError) {
        error = "ScreenCaptureKit could not start: " +
            std::string([[startError localizedDescription] UTF8String] ?: "unknown error");
        stopStreamOnly();
        return false;
    }

    {
        std::lock_guard<std::mutex> frameLock(state_.mutex);
        state_.latestFrame = {};
        state_.frameId = 0;
        state_.hasFrame = false;
    }
    lastReturnedFrameId_ = 0;
    currentDisplayIndex_ = index;
    return true;
}

void MacRemotePlatform::stopStreamOnly() {
    if (stream_) {
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [stream_ stopCaptureWithCompletionHandler:^(NSError*) {
            dispatch_semaphore_signal(semaphore);
        }];
        dispatch_semaphore_wait(
            semaphore,
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(2 * NSEC_PER_SEC)));
    }

    stream_ = nil;
    output_ = nil;
    captureQueue_ = nullptr;
}

void MacRemotePlatform::stop() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    stopStreamOnly();
    displays_ = nil;
    useWindowServerFallback_ = false;
    fallbackDisplayId_ = kCGNullDirectDisplay;
    fallbackFrameId_ = 0;
    consecutiveBlackFrames_ = 0;
    {
        std::lock_guard<std::mutex> frameLock(state_.mutex);
        state_.latestFrame = {};
        state_.hasFrame = false;
        state_.frameId = 0;
    }
}

I420Frame MacRemotePlatform::captureWindowServerComposite() const {
    CGDirectDisplayID displayId = fallbackDisplayId_;
    if (displayId == kCGNullDirectDisplay) {
        if (!displays_ || currentDisplayIndex_ < 0 ||
            static_cast<NSUInteger>(currentDisplayIndex_) >= displays_.count) {
            return {};
        }
        displayId = displays_[static_cast<NSUInteger>(currentDisplayIndex_)].displayID;
    }

    const CGRect bounds = CGDisplayBounds(displayId);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGImageRef image = CGWindowListCreateImage(
        bounds,
        kCGWindowListOptionOnScreenOnly,
        kCGNullWindowID,
        kCGWindowImageDefault);
#pragma clang diagnostic pop

    if (!image) return {};

    I420Frame frame = cgImageToI420(image);
    CGImageRelease(image);
    return frame;
}

FrameCaptureResult MacRemotePlatform::capture() {
    FrameCaptureResult result;

    if (useWindowServerFallback_) {
        I420Frame fallback = captureWindowServerComposite();
        if (fallback.width > 0 && fallback.height > 0) {
            result.frame = std::move(fallback);
            result.hasFrame = true;
            result.changed = true;
            result.frameId = ++fallbackFrameId_;
        }
        return result;
    }

    I420Frame latest;
    std::uint64_t sourceFrameId = 0;
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (!state_.hasFrame || state_.frameId == 0) return result;
        latest = state_.latestFrame;
        sourceFrameId = state_.frameId;
    }

    const bool screenCaptureBlack = isLikelyBlackFrame(latest);
    if (screenCaptureBlack) {
        ++consecutiveBlackFrames_;
    } else {
        consecutiveBlackFrames_ = 0;
    }

    // Some virtual GPU/display implementations (notably VMware SVGA) expose
    // an online display to ScreenCaptureKit while returning only black pixels.
    // After repeated genuinely-black frames, try WindowServer's composited
    // on-screen window surfaces instead of the virtual framebuffer.
    if (!useWindowServerFallback_ && consecutiveBlackFrames_ >= 3) {
        I420Frame fallbackProbe = captureWindowServerComposite();
        if (fallbackProbe.width > 0 && fallbackProbe.height > 0 &&
            !isLikelyBlackFrame(fallbackProbe)) {
            fallbackDisplayId_ =
                displays_[static_cast<NSUInteger>(currentDisplayIndex_)].displayID;
            useWindowServerFallback_ = true;
            NSLog(@"Hi5Central switching macOS capture backend to WindowServer compositor vendor=0x%x stream=%dx%d",
                  CGDisplayVendorNumber(fallbackDisplayId_),
                  fallbackProbe.width,
                  fallbackProbe.height);
            latest = std::move(fallbackProbe);
        }
    }

    result.frame = std::move(latest);
    result.hasFrame = result.frame.width > 0 && result.frame.height > 0;
    result.changed = sourceFrameId != lastReturnedFrameId_;
    result.frameId = sourceFrameId;
    lastReturnedFrameId_ = sourceFrameId;
    return result;
}

std::vector<DisplayInfo> MacRemotePlatform::displays() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::vector<DisplayInfo> result;

    if (useWindowServerFallback_ && fallbackDisplayId_ != kCGNullDirectDisplay) {
        const CGRect bounds = CGDisplayBounds(fallbackDisplayId_);
        DisplayInfo info;
        info.index = 0;
        info.name = "Display 1";
        info.x = static_cast<int>(std::lround(bounds.origin.x));
        info.y = static_cast<int>(std::lround(bounds.origin.y));
        info.width = static_cast<int>(std::lround(bounds.size.width));
        info.height = static_cast<int>(std::lround(bounds.size.height));
        info.primary = true;
        result.push_back(std::move(info));
        return result;
    }

    if (!displays_) return result;

    const CGDirectDisplayID mainDisplay = CGMainDisplayID();
    for (NSUInteger i = 0; i < displays_.count; ++i) {
        SCDisplay* display = displays_[i];
        const CGRect bounds = CGDisplayBounds(display.displayID);
        DisplayInfo info;
        info.index = static_cast<int>(i);
        info.name = "Display " + std::to_string(i + 1);
        info.x = static_cast<int>(std::lround(bounds.origin.x));
        info.y = static_cast<int>(std::lround(bounds.origin.y));
        info.width = static_cast<int>(display.width);
        info.height = static_cast<int>(display.height);
        info.primary = display.displayID == mainDisplay;
        result.push_back(std::move(info));
    }
    return result;
}

bool MacRemotePlatform::setDisplayIndex(int index) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (useWindowServerFallback_) return index == 0;
    if (index == currentDisplayIndex_) return true;
    std::string error;
    return startCaptureForIndex(index, error);
}

CGPoint MacRemotePlatform::pointFromMessage(const nlohmann::json& message) const {
    // Click/down/up messages from older Viewer builds may not carry pointer
    // coordinates. In that case use the current macOS pointer location rather
    // than defaulting the event to the top-left corner of the display.
    if (!message.contains("x_norm") || !message.contains("y_norm")) {
        CGEventRef current = CGEventCreate(nullptr);
        if (!current) return CGPointZero;
        const CGPoint location = CGEventGetLocation(current);
        CFRelease(current);
        return location;
    }

    CGDirectDisplayID displayId = fallbackDisplayId_;
    if (displayId == kCGNullDirectDisplay) {
        if (!displays_ || currentDisplayIndex_ < 0 ||
            static_cast<NSUInteger>(currentDisplayIndex_) >= displays_.count) {
            return CGPointZero;
        }
        displayId = displays_[static_cast<NSUInteger>(currentDisplayIndex_)].displayID;
    }

    const CGRect bounds = CGDisplayBounds(displayId);
    const double xn = std::clamp(message.value("x_norm", 0.0), 0.0, 1.0);
    const double yn = std::clamp(message.value("y_norm", 0.0), 0.0, 1.0);
    return CGPointMake(
        bounds.origin.x + xn * std::max(1.0, bounds.size.width - 1.0),
        bounds.origin.y + yn * std::max(1.0, bounds.size.height - 1.0));
}
bool MacRemotePlatform::handleInput(const nlohmann::json& message, std::string& error) {
    if (!AXIsProcessTrusted()) {
        error = "macOS Accessibility permission is not currently granted.";
        return false;
    }

    const std::string kind = message.value("kind", message.value("type", ""));
    const CGPoint point = pointFromMessage(message);

    if (kind == "mouse_move") {
        CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
        if (!event) { error = "Unable to create macOS mouse event."; return false; }
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        return true;
    }

    if (kind == "mouse_down" || kind == "mouse_up" || kind == "mouse_click") {
        const int browserButton = message.value("button", 0);
        const CGMouseButton button =
            browserButton == 1 ? kCGMouseButtonCenter :
            browserButton == 2 ? kCGMouseButtonRight :
            kCGMouseButtonLeft;

        CGEventType downType = kCGEventLeftMouseDown;
        CGEventType upType = kCGEventLeftMouseUp;
        if (button == kCGMouseButtonRight) {
            downType = kCGEventRightMouseDown;
            upType = kCGEventRightMouseUp;
        } else if (button == kCGMouseButtonCenter) {
            downType = kCGEventOtherMouseDown;
            upType = kCGEventOtherMouseUp;
        }

        auto postMouse = [&](CGEventType type) -> bool {
            CGEventRef event = CGEventCreateMouseEvent(nullptr, type, point, button);
            if (!event) return false;
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
            return true;
        };

        if (kind == "mouse_click") {
            if (!postMouse(downType) || !postMouse(upType)) {
                error = "Unable to post macOS mouse click.";
                return false;
            }
        } else if (!postMouse(kind == "mouse_down" ? downType : upType)) {
            error = "Unable to post macOS mouse event.";
            return false;
        }
        return true;
    }

    if (kind == "wheel") {
        const int dy = message.value("delta_y", 0);
        const int dx = message.value("delta_x", 0);
        CGEventRef event = CGEventCreateScrollWheelEvent(
            nullptr,
            kCGScrollEventUnitPixel,
            2,
            -dy,
            -dx);
        if (!event) {
            error = "Unable to create macOS scroll event.";
            return false;
        }
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        return true;
    }

    if (kind == "key_down" || kind == "key_up") {
        const std::string code = message.value("code", "");
        const CGKeyCode keyCode = keyCodeForBrowserCode(code);
        if (keyCode == static_cast<CGKeyCode>(UINT16_MAX)) {
            error = "Unsupported macOS key code: " + code;
            return false;
        }

        CGEventRef event = CGEventCreateKeyboardEvent(
            nullptr,
            keyCode,
            kind == "key_down");
        if (!event) {
            error = "Unable to create macOS keyboard event.";
            return false;
        }
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        return true;
    }

    if (kind == "text_input") {
        const std::string text = message.value("text", "");
        if (text.empty()) return true;

        NSString* nsText = [NSString stringWithUTF8String:text.c_str()];
        if (!nsText) {
            error = "Unable to decode remote text input.";
            return false;
        }

        const NSUInteger length = nsText.length;
        std::vector<UniChar> chars(length);
        [nsText getCharacters:chars.data() range:NSMakeRange(0, length)];

        CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
        if (!down || !up) {
            if (down) CFRelease(down);
            if (up) CFRelease(up);
            error = "Unable to create macOS text input event.";
            return false;
        }

        CGEventKeyboardSetUnicodeString(down, length, chars.data());
        CGEventKeyboardSetUnicodeString(up, 0, nullptr);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
        return true;
    }

    error = "Unsupported macOS input event: " + kind;
    return false;
}

} // namespace

std::unique_ptr<RemotePlatform> createRemotePlatform() {
    return std::make_unique<MacRemotePlatform>();
}

} // namespace hi5

#endif