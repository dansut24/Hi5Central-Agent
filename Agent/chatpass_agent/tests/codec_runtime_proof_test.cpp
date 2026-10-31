#include "../src/codec_runtime_proof.h"
#include <cassert>
#include <iostream>
using namespace hi5::universal_codec;

int main() {
 RuntimeProofStore store;
 EncoderWindow e{};
 e.codec="h264";e.mode="h264_hw";e.frames=21;e.sent=21;
 e.width=1728;e.height=1080;e.maxEncodeMs=21;e.p95EncodeMs=15;
 DecoderReport d{};
 d.codec="h264";d.decoded=50;d.keyframes=1;
 d.packets=160;d.width=1728;d.height=1080;
 DecoderReport next=d;next.decoded=95;next.keyframes=2;next.packets=320;
 std::string reason;

 // Two healthy encoder windows and a later decoded keyframe are both needed.
 store.recordEncoder("hardware",e,1000);
 assert(store.confirm("hardware",d,1002,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");
 store.recordEncoder("hardware",e,1005);
 assert(store.confirm("hardware",d,1006,false).codec.empty());
 assert(store.confirm("hardware",d,1006,true,&reason).codec.empty());
 assert(reason=="awaiting_second_decoded_keyframe");
 assert(store.confirm("hardware",next,1011,true).codec=="h264");
 assert(store.confirm("hardware",next,1012,true).codec.empty());

 store.recordEncoder("no-idr",e,1000);
 store.recordEncoder("no-idr",e,1005);
 assert(store.confirm("no-idr",d,1006,true).codec.empty());
 next.keyframes=1;
 assert(store.confirm("no-idr",next,1011,true).codec.empty());
 next.keyframes=2;next.width=1280;
 assert(store.confirm("no-idr",next,1012,true).codec.empty());
 next.width=1728;
 assert(store.confirm("no-idr",next,1036,true).codec.empty());

 store.recordEncoder("drops",e,1000);
 store.recordEncoder("drops",e,1005);
 auto drops=d;drops.dropped=100;
 assert(store.confirm("drops",drops,1006,true).codec.empty());

 auto sw=e;sw.mode="h264_sw";
 store.recordEncoder("software",sw,1000);
 store.recordEncoder("software",sw,1005);
 assert(store.confirm("software",d,1006,false).codec.empty());
 assert(store.confirm("software",next,1011,false).codec=="h264");

 auto perf=e;perf.maxEncodeMs=100;
 store.recordEncoder("slow",perf,1000);
 store.recordEncoder("slow",perf,1005);
 assert(store.confirm("slow",d,1006,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");
 assert(store.confirm("slow",next,1011,true).codec.empty());

 // A sub-75ms outlier with healthy p95 is allowed only after two healthy
 // windows; a cold-start max over 75ms can also recover without relaxing
 // the original strict latency thresholds.
 auto spike=e;spike.maxEncodeMs=70;spike.p95EncodeMs=15;
 store.recordEncoder("spike",spike,1000);
 store.recordEncoder("spike",spike,1005);
 assert(store.confirm("spike",d,1006,true).codec.empty());
 assert(store.confirm("spike",next,1011,true).codec=="h264");

 auto cold=e;cold.maxEncodeMs=77;cold.p95EncodeMs=15;
 store.recordEncoder("cold-start",cold,1000);
 assert(store.confirm("cold-start",d,1002,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");
 cold.maxEncodeMs=27;cold.p95EncodeMs=18;
 store.recordEncoder("cold-start",cold,1005);
 assert(store.confirm("cold-start",d,1006,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");
 store.recordEncoder("cold-start",cold,1010);
 assert(store.confirm("cold-start",d,1011,true,&reason).codec.empty());
 assert(reason=="awaiting_second_decoded_keyframe");
 assert(store.confirm("cold-start",next,1016,true,&reason).codec=="h264");
 assert(reason=="end_to_end_qualified");

 // Real idle 2FPS uses ~10 encoded frames per five-second window; count
 // across windows, but don't count a slow idle window as healthy.
 auto idle=e;idle.frames=10;idle.sent=10;
 idle.maxEncodeMs=70;idle.p95EncodeMs=70;
 store.recordEncoder("idle",idle,1000);
 assert(store.confirm("idle",d,1002,true,&reason).codec.empty());
 assert(reason=="awaiting_20_encoded_frames");
 idle.maxEncodeMs=22;idle.p95EncodeMs=15;
 store.recordEncoder("idle",idle,1005);
 assert(store.confirm("idle",d,1007,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");
 store.recordEncoder("idle",idle,1010);
 assert(store.confirm("idle",d,1012,true,&reason).codec.empty());
 assert(reason=="awaiting_second_decoded_keyframe");
 assert(store.confirm("idle",next,1016,true,&reason).codec=="h264");

 // A sustained or renewed overload must reset consecutive headroom.
 auto slowP95=e;slowP95.maxEncodeMs=60;slowP95.p95EncodeMs=45;
 store.recordEncoder("slow-p95",slowP95,1000);
 store.recordEncoder("slow-p95",slowP95,1005);
 assert(store.confirm("slow-p95",d,1006,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");
 store.recordEncoder("unstable",e,1000);
 store.recordEncoder("unstable",e,1005);
 store.recordEncoder("unstable",slowP95,1010);
 assert(store.confirm("unstable",d,1011,true,&reason).codec.empty());
 assert(reason=="awaiting_two_healthy_encoder_windows");

 auto vp9=e;vp9.codec="vp9";vp9.mode="vp9_hw";
 auto vp9report=d;vp9report.codec="vp9";
 store.recordEncoder("unproven-vp9-hw",vp9,1000);
 store.recordEncoder("unproven-vp9-hw",vp9,1005);
 assert(store.confirm("unproven-vp9-hw",vp9report,1006,true).codec.empty());

 std::cout<<"CODEC_RUNTIME_PROOF_TESTS_PASSED sustained_headroom=2 "
             "second_idr=required no_unverified_vp9_gpu=1\n";
}
