#include "remote_h264_vt_encoder.h"

#if defined(__APPLE__)

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <stdexcept>

namespace hi5 {
namespace {

void appendAnnexB(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t size) {
    static constexpr std::uint8_t startCode[4] = {0, 0, 0, 1};
    if (!data || !size) return;
    out.insert(out.end(), std::begin(startCode), std::end(startCode));
    out.insert(out.end(), data, data + size);
}

std::uint32_t nextTimestamp90k(std::uint64_t frameIndex, int fps) {
    const std::uint64_t safeFps = static_cast<std::uint64_t>(std::max(1, fps));
    return static_cast<std::uint32_t>((frameIndex * 90000ULL) / safeFps);
}

} // namespace

struct H264VideoToolboxEncoder::Impl {
    VTCompressionSessionRef session = nullptr;
    int width = 0;
    int height = 0;
    int fps = 30;
    int bitrateKbps = 8000;
    std::uint64_t frameIndex = 0;

    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t callbackSequence = 0;
    std::vector<std::uint8_t> callbackData;
    bool callbackKeyframe = false;
    OSStatus callbackStatus = noErr;

    static void outputCallback(
        void* outputCallbackRefCon,
        void*,
        OSStatus status,
        VTEncodeInfoFlags,
        CMSampleBufferRef sampleBuffer) {

        auto* self = static_cast<Impl*>(outputCallbackRefCon);
        if (!self) return;

        std::vector<std::uint8_t> encoded;
        bool keyframe = false;

        if (status == noErr && sampleBuffer && CMSampleBufferDataIsReady(sampleBuffer)) {
            bool notSync = false;
            CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
            if (attachments && CFArrayGetCount(attachments) > 0) {
                CFDictionaryRef attachment = static_cast<CFDictionaryRef>(
                    CFArrayGetValueAtIndex(attachments, 0));
                notSync = CFDictionaryContainsKey(attachment, kCMSampleAttachmentKey_NotSync);
            }
            keyframe = !notSync;

            if (keyframe) {
                CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
                if (format) {
                    std::size_t count = 0;
                    int nalHeaderLength = 0;
                    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                            format, 0, nullptr, nullptr, &count, &nalHeaderLength) == noErr) {
                        for (std::size_t i = 0; i < count; ++i) {
                            const std::uint8_t* parameterSet = nullptr;
                            std::size_t parameterSetSize = 0;
                            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                                    format, i, &parameterSet, &parameterSetSize, nullptr, nullptr) == noErr) {
                                appendAnnexB(encoded, parameterSet, parameterSetSize);
                            }
                        }
                    }
                }
            }

            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
            if (block) {
                std::size_t totalLength = 0;
                char* dataPointer = nullptr;
                if (CMBlockBufferGetDataPointer(
                        block, 0, nullptr, &totalLength, &dataPointer) == kCMBlockBufferNoErr &&
                    dataPointer && totalLength > 4) {
                    std::size_t offset = 0;
                    while (offset + 4 <= totalLength) {
                        const auto* p = reinterpret_cast<const std::uint8_t*>(dataPointer + offset);
                        const std::uint32_t nalLength =
                            (static_cast<std::uint32_t>(p[0]) << 24) |
                            (static_cast<std::uint32_t>(p[1]) << 16) |
                            (static_cast<std::uint32_t>(p[2]) << 8) |
                            static_cast<std::uint32_t>(p[3]);
                        offset += 4;
                        if (!nalLength || offset + nalLength > totalLength) break;
                        appendAnnexB(
                            encoded,
                            reinterpret_cast<const std::uint8_t*>(dataPointer + offset),
                            nalLength);
                        offset += nalLength;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->callbackStatus = status;
            self->callbackData = std::move(encoded);
            self->callbackKeyframe = keyframe;
            ++self->callbackSequence;
        }
        self->cv.notify_all();
    }

    void configure() {
        OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault,
            width,
            height,
            kCMVideoCodecType_H264,
            nullptr,
            nullptr,
            nullptr,
            &Impl::outputCallback,
            this,
            &session);
        if (status != noErr || !session) {
            throw std::runtime_error("VideoToolbox H.264 encoder could not be created.");
        }

        VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(
            session,
            kVTCompressionPropertyKey_ProfileLevel,
            kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel);

        const int bitrate = std::max(500, bitrateKbps) * 1000;
        CFNumberRef bitrateNumber = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberIntType, &bitrate);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, bitrateNumber);
        CFRelease(bitrateNumber);

        const int expectedFps = std::max(1, fps);
        CFNumberRef fpsNumber = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberIntType, &expectedFps);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsNumber);
        CFRelease(fpsNumber);

        const int keyframeInterval = std::max(1, expectedFps * 3);
        CFNumberRef keyframeNumber = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberIntType, &keyframeInterval);
        VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyframeNumber);
        CFRelease(keyframeNumber);

        status = VTCompressionSessionPrepareToEncodeFrames(session);
        if (status != noErr) {
            throw std::runtime_error("VideoToolbox H.264 encoder failed to prepare.");
        }
    }
    CVPixelBufferRef pixelBufferFromI420(const I420Frame& frame) {
        CVPixelBufferRef pixelBuffer = nullptr;
        const CVReturn result = CVPixelBufferCreate(
            kCFAllocatorDefault,
            frame.width,
            frame.height,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            nullptr,
            &pixelBuffer);
        if (result != kCVReturnSuccess || !pixelBuffer) return nullptr;

        CVPixelBufferLockBaseAddress(pixelBuffer, 0);
        auto* yDst = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
        auto* uvDst = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
        const std::size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
        const std::size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(
                yDst + static_cast<std::size_t>(y) * yStride,
                frame.y.data() + static_cast<std::size_t>(y) * frame.width,
                static_cast<std::size_t>(frame.width));
        }

        const int chromaWidth = frame.width / 2;
        const int chromaHeight = frame.height / 2;
        for (int y = 0; y < chromaHeight; ++y) {
            auto* row = uvDst + static_cast<std::size_t>(y) * uvStride;
            const auto* u = frame.u.data() + static_cast<std::size_t>(y) * chromaWidth;
            const auto* v = frame.v.data() + static_cast<std::size_t>(y) * chromaWidth;
            for (int x = 0; x < chromaWidth; ++x) {
                row[x * 2] = u[x];
                row[x * 2 + 1] = v[x];
            }
        }

        CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
        return pixelBuffer;
    }
};

H264VideoToolboxEncoder::H264VideoToolboxEncoder(
    int width, int height, int fps, int bitrateKbps)
    : impl_(std::make_unique<Impl>()) {

    impl_->width = width & ~1;
    impl_->height = height & ~1;
    impl_->fps = std::max(1, fps);
    impl_->bitrateKbps = std::max(500, bitrateKbps);
    if (impl_->width < 2 || impl_->height < 2) {
        throw std::runtime_error("Invalid H.264 encoder dimensions.");
    }
    impl_->configure();
}

H264VideoToolboxEncoder::~H264VideoToolboxEncoder() {
    if (impl_ && impl_->session) {
        VTCompressionSessionCompleteFrames(impl_->session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(impl_->session);
        CFRelease(impl_->session);
        impl_->session = nullptr;
    }
}

H264EncodedFrame H264VideoToolboxEncoder::encode(
    const I420Frame& frame, bool forceKeyframe) {

    if (!impl_ || !impl_->session ||
        frame.width != impl_->width || frame.height != impl_->height ||
        frame.y.size() < static_cast<std::size_t>(frame.width) * frame.height ||
        frame.u.size() < static_cast<std::size_t>(frame.width / 2) * (frame.height / 2) ||
        frame.v.size() < static_cast<std::size_t>(frame.width / 2) * (frame.height / 2)) {
        throw std::runtime_error("Invalid I420 frame supplied to VideoToolbox.");
    }

    CVPixelBufferRef pixelBuffer = impl_->pixelBufferFromI420(frame);
    if (!pixelBuffer) {
        throw std::runtime_error("Could not allocate VideoToolbox input pixel buffer.");
    }

    std::uint64_t expectedSequence = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        expectedSequence = impl_->callbackSequence + 1;
        impl_->callbackData.clear();
        impl_->callbackStatus = noErr;
        impl_->callbackKeyframe = false;
    }

    CFDictionaryRef frameProperties = nullptr;
    if (forceKeyframe) {
        const void* keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* values[] = {kCFBooleanTrue};
        frameProperties = CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
    }

    const CMTime pts = CMTimeMake(
        static_cast<std::int64_t>(impl_->frameIndex),
        impl_->fps);

    const OSStatus status = VTCompressionSessionEncodeFrame(
        impl_->session,
        pixelBuffer,
        pts,
        kCMTimeInvalid,
        frameProperties,
        nullptr,
        nullptr);

    if (frameProperties) CFRelease(frameProperties);
    CVPixelBufferRelease(pixelBuffer);

    if (status != noErr) {
        throw std::runtime_error("VideoToolbox failed to encode the frame.");
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->cv.wait_for(
            lock,
            std::chrono::milliseconds(750),
            [&] { return impl_->callbackSequence >= expectedSequence; })) {
        throw std::runtime_error("Timed out waiting for VideoToolbox H.264 output.");
    }
    if (impl_->callbackStatus != noErr) {
        throw std::runtime_error("VideoToolbox returned an H.264 encode error.");
    }

    H264EncodedFrame out;
    out.data = impl_->callbackData;
    out.keyframe = impl_->callbackKeyframe;
    out.rtpTimestamp = nextTimestamp90k(impl_->frameIndex, impl_->fps);
    ++impl_->frameIndex;
    return out;
}

} // namespace hi5

#endif