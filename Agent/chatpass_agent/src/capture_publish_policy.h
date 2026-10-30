#pragma once
// Do not re-filter authentic WGC or DXGI changed frames with the sparse GDI
// detector: it can discard real text/cursor/small-window updates and leave
// the Viewer at 1-2 decoded frames per second.
#include "frame_source.h"
#include "capture_motion_samples.h"
namespace hi5 {
inline bool ShouldPublishDesktopFrame(const FrameCaptureResult& captured,
                                     SampledDesktopMotion& gdiMotion) {
    if (!captured.hasFrame || !captured.changed || captured.cursorOnly)
        return false;
    if (captured.changedIsNative)
        return true;
    return gdiMotion.observe(captured.frame.y,
                             captured.frame.width, captured.frame.height);
}
} // namespace hi5
