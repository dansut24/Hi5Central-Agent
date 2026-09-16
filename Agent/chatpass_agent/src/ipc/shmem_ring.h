// src/ipc/shmem_ring.h
//
// Single-producer / single-consumer RAW I420 frame ring over Windows shared memory.
// This replaces the old encoded-VP8 handoff so the service owns one VP8 encoder
// and one RTP/WebRTC timeline across normal, UAC, Winlogon and user-switch frames.

#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "frame_source.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hi5 {

    static constexpr int      kShmRingSlots = 2;
    static constexpr uint32_t kShmMaxFrameBytes = 16u * 1024u * 1024u; // one 4K/5K-ultrawide I420 frame; latest-frame-wins only needs two slots

    struct ShmFrameHeader {
        std::atomic<uint32_t> size;   // payload bytes; 0 means slot not committed yet
        uint32_t              width;
        uint32_t              height;
        uint32_t              ySize;
        uint32_t              uSize;
        uint32_t              vSize;
        uint64_t              tsNs;
        uint8_t               _pad[32];
    };

    struct ShmemHeader {
        std::atomic<uint64_t> writeIdx;
        std::atomic<uint64_t> readIdx;
        uint32_t              slotBytes;
        uint32_t              numSlots;
        uint32_t              format; // 1 = raw I420
        uint8_t               _pad[28];
    };

    static constexpr size_t kShmSlotBytes = sizeof(ShmFrameHeader) + kShmMaxFrameBytes;
    static constexpr size_t kShmTotalBytes = sizeof(ShmemHeader) + kShmRingSlots * kShmSlotBytes;

    class ShmemRing {
    public:
        ~ShmemRing() { Close(); }

        bool CreateProducer(const std::string& name) {
            name_ = name;

            SECURITY_DESCRIPTOR sd{};
            InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
            SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), &sd, FALSE };

            hMap_ = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
                PAGE_READWRITE, 0,
                static_cast<DWORD>(kShmTotalBytes),
                name.c_str());
            if (!hMap_) return false;

            base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kShmTotalBytes);
            if (!base_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }

            auto* hdr = header();
            hdr->writeIdx.store(0, std::memory_order_relaxed);
            hdr->readIdx.store(0, std::memory_order_relaxed);
            hdr->slotBytes = static_cast<uint32_t>(kShmSlotBytes);
            hdr->numSlots = kShmRingSlots;
            hdr->format = 1;

            for (uint32_t i = 0; i < kShmRingSlots; ++i) {
                auto* fh = frameHeader(i);
                fh->size.store(0, std::memory_order_relaxed);
                fh->width = 0;
                fh->height = 0;
                fh->ySize = 0;
                fh->uSize = 0;
                fh->vSize = 0;
                fh->tsNs = 0;
            }

            producer_ = true;
            return true;
        }

        bool OpenProducer(const std::string& name) {
            name_ = name;
            hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
            if (!hMap_) return false;
            base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kShmTotalBytes);
            if (!base_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }
            producer_ = true;
            return true;
        }

        bool OpenConsumer(const std::string& name) {
            name_ = name;
            hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
            if (!hMap_) return false;
            base_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kShmTotalBytes);
            if (!base_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
                return false;
            }
            producer_ = false;
            return true;
        }

        bool WriteRawI420Frame(const I420Frame& frame, uint64_t tsNs) {
            if (!base_ || frame.width <= 0 || frame.height <= 0) {
                return false;
            }

            const uint32_t ySize = static_cast<uint32_t>(frame.y.size());
            const uint32_t uSize = static_cast<uint32_t>(frame.u.size());
            const uint32_t vSize = static_cast<uint32_t>(frame.v.size());
            const uint32_t total = ySize + uSize + vSize;

            if (total == 0 || total > kShmMaxFrameBytes) {
                return false;
            }

            auto* hdr = header();
            const uint64_t w = hdr->writeIdx.load(std::memory_order_relaxed);
            const uint64_t r = hdr->readIdx.load(std::memory_order_acquire);

            // Latest-frame-wins for desktop remoting. Raw I420 frames are independent,
            // so it is safe to drop the oldest slot if the service falls behind.
            if (w - r >= kShmRingSlots) {
                hdr->readIdx.store(r + 1, std::memory_order_release);
            }

            const uint32_t slot = static_cast<uint32_t>(w % kShmRingSlots);
            auto* fh = frameHeader(slot);
            auto* payload = reinterpret_cast<uint8_t*>(fh) + sizeof(ShmFrameHeader);

            fh->size.store(0, std::memory_order_relaxed);
            fh->width = static_cast<uint32_t>(frame.width);
            fh->height = static_cast<uint32_t>(frame.height);
            fh->ySize = ySize;
            fh->uSize = uSize;
            fh->vSize = vSize;
            fh->tsNs = tsNs;

            std::memcpy(payload, frame.y.data(), ySize);
            std::memcpy(payload + ySize, frame.u.data(), uSize);
            std::memcpy(payload + ySize + uSize, frame.v.data(), vSize);

            std::atomic_thread_fence(std::memory_order_release);
            fh->size.store(total, std::memory_order_release);
            hdr->writeIdx.store(w + 1, std::memory_order_release);
            return true;
        }

        bool ReadRawI420Frame(I420Frame& frame, uint64_t& tsNs) {
            if (!base_) return false;

            auto* hdr = header();
            const uint64_t r = hdr->readIdx.load(std::memory_order_relaxed);
            const uint64_t w = hdr->writeIdx.load(std::memory_order_acquire);

            if (r >= w) {
                return false;
            }

            const uint32_t slot = static_cast<uint32_t>(r % kShmRingSlots);
            auto* fh = frameHeader(slot);
            const uint32_t size = fh->size.load(std::memory_order_acquire);

            if (size == 0 || size > kShmMaxFrameBytes || size != fh->ySize + fh->uSize + fh->vSize) {
                hdr->readIdx.store(r + 1, std::memory_order_release);
                return false;
            }

            const auto* payload = reinterpret_cast<const uint8_t*>(fh) + sizeof(ShmFrameHeader);

            frame.width = static_cast<int>(fh->width);
            frame.height = static_cast<int>(fh->height);
            frame.y.resize(fh->ySize);
            frame.u.resize(fh->uSize);
            frame.v.resize(fh->vSize);

            std::memcpy(frame.y.data(), payload, fh->ySize);
            std::memcpy(frame.u.data(), payload + fh->ySize, fh->uSize);
            std::memcpy(frame.v.data(), payload + fh->ySize + fh->uSize, fh->vSize);
            tsNs = fh->tsNs;

            fh->size.store(0, std::memory_order_relaxed);
            hdr->readIdx.store(r + 1, std::memory_order_release);
            return true;
        }

        bool HasFrame() const {
            if (!base_) return false;
            auto* hdr = const_cast<ShmemRing*>(this)->header();
            return hdr->writeIdx.load(std::memory_order_acquire) >
                hdr->readIdx.load(std::memory_order_relaxed);
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

        // Compatibility stubs: the new pipeline should not call these, but keeping
        // the signatures avoids compile breakage in old branches while migrating.
        bool WriteFrame(const uint8_t*, uint32_t, uint64_t, bool) { return false; }
        bool ReadFrame(std::vector<uint8_t>&, uint64_t&, bool&) { return false; }

    private:
        ShmemHeader* header() {
            return reinterpret_cast<ShmemHeader*>(base_);
        }

        ShmFrameHeader* frameHeader(uint32_t slot) {
            auto* p = reinterpret_cast<uint8_t*>(base_) + sizeof(ShmemHeader) + slot * kShmSlotBytes;
            return reinterpret_cast<ShmFrameHeader*>(p);
        }

        HANDLE hMap_ = nullptr;
        void* base_ = nullptr;
        bool producer_ = false;
        std::string name_;
    };

} // namespace hi5