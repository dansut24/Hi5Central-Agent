#pragma once

#include "frame_source.h"

#include <memory>
#include <string>

namespace hi5 {

class SharedGpuFrameReader {
public:
    SharedGpuFrameReader();
    ~SharedGpuFrameReader();

    SharedGpuFrameReader(const SharedGpuFrameReader&) = delete;
    SharedGpuFrameReader& operator=(const SharedGpuFrameReader&) = delete;

    bool ReadI420(const SharedGpuFrame& frame, I420Frame& out, std::string* error = nullptr);
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hi5
