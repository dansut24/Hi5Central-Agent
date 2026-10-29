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

 // A single slow frame must not be misrepresented as the p95.
 auto spike=e;spike.maxEncodeMs=70;spike.p95EncodeMs=15;
 store.recordEncoder("spike",spike,1000);
 auto second=next;second.keyframes=2;
 assert(store.confirm("spike",d,1002,true).codec.empty());
 assert(store.confirm("spike",second,1009,true).codec=="h264");

 // Real idle streams produce 10 frames per five-second diagnostics window.
 // The proof must accumulate these windows, not demand 20 every 5 seconds.
 auto idle=e;idle.frames=10;idle.sent=10;
 idle.maxEncodeMs=70;idle.p95EncodeMs=70; // first 10-frame idle sample
 store.recordEncoder("idle",idle,1000);
 std::string reason;
 assert(store.confirm("idle",d,1002,true,&reason).codec.empty());
 assert(reason=="awaiting_20_encoded_frames");
 idle.maxEncodeMs=22;idle.p95EncodeMs=15; // later rolling-sample p95
 store.recordEncoder("idle",idle,1005);
 assert(store.confirm("idle",d,1007,true,&reason).codec.empty());
 assert(reason=="awaiting_second_decoded_keyframe");
 assert(store.confirm("idle",second,1013,true,&reason).codec=="h264");
 assert(reason=="end_to_end_qualified");

 // Unlike a spike, a real sustained high p95 must be rejected.
 auto slowP95=e;slowP95.maxEncodeMs=60;slowP95.p95EncodeMs=45;
 store.recordEncoder("slow-p95",slowP95,1000);
 assert(store.confirm("slow-p95",d,1002,true).codec.empty());
 assert(store.confirm("slow-p95",second,1009,true,&reason).codec.empty());
 assert(reason=="measured_encoder_latency_outside_limits");

 auto vp9=e;vp9.codec="vp9";vp9.mode="vp9_hw";
 auto vp9report=d;vp9report.codec="vp9";
 store.recordEncoder("unproven-vp9-hw",vp9,1000);
 assert(store.confirm("unproven-vp9-hw",vp9report,1002,true).codec.empty());

 std::cout<<"CODEC_RUNTIME_PROOF_TESTS_PASSED second_idr=required "
             "no_unverified_vp9_gpu=1\n";
}
