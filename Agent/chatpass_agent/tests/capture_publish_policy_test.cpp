#include "../src/capture_publish_policy.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
int main() {
    hi5::SampledDesktopMotion motion;
    FrameCaptureResult frame;
    frame.hasFrame=true; frame.changed=true;
    frame.frame.width=640;frame.frame.height=360;
    frame.frame.y.assign(640u*360u,65);
    assert(hi5::ShouldPublishDesktopFrame(frame,motion)); // first GDI
    assert(!hi5::ShouldPublishDesktopFrame(frame,motion)); // static GDI
    frame.changedIsNative=true;
    assert(hi5::ShouldPublishDesktopFrame(frame,motion)); // WGC new frame
    assert(hi5::ShouldPublishDesktopFrame(frame,motion)); // two valid compositor updates
    frame.cursorOnly=true;
    assert(!hi5::ShouldPublishDesktopFrame(frame,motion));
    frame.cursorOnly=false;frame.changed=false;
    assert(!hi5::ShouldPublishDesktopFrame(frame,motion));
    frame.changed=true;frame.hasFrame=false;
    assert(!hi5::ShouldPublishDesktopFrame(frame,motion));
    frame.hasFrame=true;frame.changedIsNative=false;
    assert(!hi5::ShouldPublishDesktopFrame(frame,motion)); // static GDI still idle
    for(int y=130;y<190;++y)
      for(int x=190;x<320;++x)frame.frame.y[static_cast<size_t>(y)*640+x]=145;
    assert(hi5::ShouldPublishDesktopFrame(frame,motion)); // GDI real change
    assert(!hi5::ShouldPublishDesktopFrame(frame,motion));
    std::cout<<"CAPTURE_PUBLISH_POLICY_TESTS_PASSED native_update_preserved=1 gdi_static_idle=1\n";
}
