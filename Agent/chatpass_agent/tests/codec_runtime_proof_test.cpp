#include "../src/codec_runtime_proof.h"
#include <cassert>
#include <iostream>
using namespace hi5::universal_codec;

int main() {
 RuntimeProofStore store;
 EncoderWindow e{};
 e.codec="h264";e.mode="h264_hw";e.frames=21;e.sent=21;
 e.width=1728;e.height=1080;e.maxEncodeMs=21;
 DecoderReport d{};
 d.codec="h264";d.decoded=50;d.keyframes=1;
 d.packets=160;d.width=1728;d.height=1080;

 store.recordEncoder("hardware",e,1000);
 assert(store.confirm("hardware",d,1002,false).codec.empty());
 assert(store.confirm("hardware",d,1002,true).codec.empty());
 DecoderReport next=d;next.decoded=95;next.keyframes=2;next.packets=320;
 assert(store.confirm("hardware",next,1009,true).codec=="h264");
 assert(store.confirm("hardware",next,1010,true).codec.empty());

 store.recordEncoder("no-idr",e,1000);
 assert(store.confirm("no-idr",d,1002,true).codec.empty());
 next.keyframes=1;
 assert(store.confirm("no-idr",next,1009,true).codec.empty());
 next.keyframes=2;next.width=1280;
 assert(store.confirm("no-idr",next,1010,true).codec.empty());
 next.width=1728;
 assert(store.confirm("no-idr",next,1032,true).codec.empty());

 store.recordEncoder("drops",e,1000);
 auto drops=d;drops.dropped=100;
 assert(store.confirm("drops",drops,1002,true).codec.empty());

 auto sw=e;sw.mode="h264_sw";
 store.recordEncoder("software",sw,1000);
 assert(store.confirm("software",d,1002,false).codec.empty());
 assert(store.confirm("software",next,1009,false).codec=="h264");

 auto perf=e;perf.maxEncodeMs=100;
 store.recordEncoder("slow",perf,1000);
 assert(store.confirm("slow",d,1002,true).codec.empty());
 assert(store.confirm("slow",next,1009,true).codec.empty());

 auto vp9=e;vp9.codec="vp9";vp9.mode="vp9_hw";
 auto vp9report=d;vp9report.codec="vp9";
 store.recordEncoder("unproven-vp9-hw",vp9,1000);
 assert(store.confirm("unproven-vp9-hw",vp9report,1002,true).codec.empty());

 std::cout<<"CODEC_RUNTIME_PROOF_TESTS_PASSED second_idr=required "
             "no_unverified_vp9_gpu=1\n";
}
