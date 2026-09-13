#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hi5 {

    enum class EncodedVideoCodec : uint32_t {
        Unknown = 0,
        VP8 = 1,
    };

    struct EncodedVideoFrame {
        EncodedVideoCodec codec = EncodedVideoCodec::Unknown;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rtpTimestamp = 0;
        bool keyframe = false;
        uint64_t tsNs = 0;
        std::vector<uint8_t> data;
    };

    static constexpr int      kEncodedRingSlots = 6;
    static constexpr uint32_t kEncodedMaxFrameBytes = 8u * 1024u * 1024u;

    struct EncodedSlotHeader {
        std::atomic<uint32_t> size; // 0 means not committed
        uint32_t codec;
        uint32_t width;
        uint32_t height;
        uint32_t rtpTimestamp;
        uint32_t keyframe;
        uint64_t tsNs;
        uint8_t _pad[32];
    };

    struct EncodedRingHeader {
        std::atomic<uint64_t> writeIdx;
        std::atomic<uint64_t> readIdx;
        uint32_t slotBytes;
        uint32_t numSlots;
        uint32_t format; // 2 = encoded video
        uint8_t _pad[28];
    };

    static constexpr size_t kEncodedSlotBytes = sizeof(EncodedSlotHeader) + kEncodedMaxFrameBytes;
    static constexpr size_t kEncodedTotalBytes = sizeof(EncodedRingHeader) + kEncodedRingSlots * kEncodedSlotBytes;

    class EncodedFrameRing {
    public:
        ~EncodedFrameRing() { Close(); }

        bool Create(const std::string& name) {
            name_ = name;

            SECURITY_DESCRIPTOR sd{};
            InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
            SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), &sd, FALSE };

            hMap_ = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
                PAGE_READWRITE, 0,
                static_cast<DWORD>(kEncodedTotalBytes),
                name.c_str());
            if (!hMap_) return false;

            base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kEncodedTotalBytes);
            if (!base_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }

            auto* hdr = header();
            hdr->writeIdx.store(0, std::memory_order_relaxed);
            hdr->readIdx.store(0, std::memory_order_relaxed);
            hdr->slotBytes = static_cast<uint32_t>(kEncodedSlotBytes);
            hdr->numSlots = kEncodedRingSlots;
            hdr->format = 2;

            for (uint32_t i = 0; i < kEncodedRingSlots; ++i) {
                auto* sh = slotHeader(i);
                sh->size.store(0, std::memory_order_relaxed);
                sh->codec = 0;
                sh->width = 0;
                sh->height = 0;
                sh->rtpTimestamp = 0;
                sh->keyframe = 0;
                sh->tsNs = 0;
            }

            return true;
        }

        bool Open(const std::string& name) {
            name_ = name;
            hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
            if (!hMap_) return false;
            base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kEncodedTotalBytes);
            if (!base_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }
            return true;
        }

        bool Write(const EncodedVideoFrame& frame) {
            if (!base_ || frame.data.empty() || frame.data.size() > kEncodedMaxFrameBytes) {
                return false;
            }

            auto* hdr = header();
            const uint64_t w = hdr->writeIdx.load(std::memory_order_relaxed);
            const uint64_t r = hdr->readIdx.load(std::memory_order_acquire);

            if (w - r >= kEncodedRingSlots) {
                hdr->readIdx.store(r + 1, std::memory_order_release);
            }

            const uint32_t slot = static_cast<uint32_t>(w % kEncodedRingSlots);
            auto* sh = slotHeader(slot);
            auto* payload = reinterpret_cast<uint8_t*>(sh) + sizeof(EncodedSlotHeader);

            sh->size.store(0, std::memory_order_relaxed);
            sh->codec = static_cast<uint32_t>(frame.codec);
            sh->width = frame.width;
            sh->height = frame.height;
            sh->rtpTimestamp = frame.rtpTimestamp;
            sh->keyframe = frame.keyframe ? 1u : 0u;
            sh->tsNs = frame.tsNs;

            std::memcpy(payload, frame.data.data(), frame.data.size());
            std::atomic_thread_fence(std::memory_order_release);
            sh->size.store(static_cast<uint32_t>(frame.data.size()), std::memory_order_release);
            hdr->writeIdx.store(w + 1, std::memory_order_release);
            return true;
        }

        bool Read(EncodedVideoFrame& frame) {
            if (!base_) return false;

            auto* hdr = header();
            const uint64_t r = hdr->readIdx.load(std::memory_order_relaxed);
            const uint64_t w = hdr->writeIdx.load(std::memory_order_acquire);
            if (r >= w) return false;

            const uint32_t slot = static_cast<uint32_t>(r % kEncodedRingSlots);
            auto* sh = slotHeader(slot);
            const uint32_t size = sh->size.load(std::memory_order_acquire);

            if (size == 0 || size > kEncodedMaxFrameBytes) {
                hdr->readIdx.store(r + 1, std::memory_order_release);
                return false;
            }

            const auto* payload = reinterpret_cast<const uint8_t*>(sh) + sizeof(EncodedSlotHeader);
            frame.codec = static_cast<EncodedVideoCodec>(sh->codec);
            frame.width = sh->width;
            frame.height = sh->height;
            frame.rtpTimestamp = sh->rtpTimestamp;
            frame.keyframe = sh->keyframe != 0;
            frame.tsNs = sh->tsNs;
            frame.data.resize(size);
            std::memcpy(frame.data.data(), payload, size);

            sh->size.store(0, std::memory_order_relaxed);
            hdr->readIdx.store(r + 1, std::memory_order_release);
            return true;
        }

        bool HasFrame() const {
            if (!base_) return false;
            auto* hdr = const_cast<EncodedFrameRing*>(this)->header();
            return hdr->writeIdx.load(std::memory_order_acquire) > hdr->readIdx.load(std::memory_order_relaxed);
        }

        void Close() {
            if (base_) {
                UnmapViewOfFile(base_);
                base_ = nullptr;
            }
            if (hMap_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
            }
        }

    private:
        EncodedRingHeader* header() {
            return reinterpret_cast<EncodedRingHeader*>(base_);
        }

        EncodedSlotHeader* slotHeader(uint32_t slot) {
            auto* p = reinterpret_cast<uint8_t*>(base_) + sizeof(EncodedRingHeader) + slot * kEncodedSlotBytes;
            return reinterpret_cast<EncodedSlotHeader*>(p);
        }

    private:
        std::string name_;
        HANDLE hMap_ = nullptr;
        void* base_ = nullptr;
    };

} // namespace hi5
