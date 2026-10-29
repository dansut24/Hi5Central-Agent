#include "../src/codec_runtime_proof.h"
#include <cassert>
#include <iostream>
using namespace hi5::universal_codec;
int main(){
 RuntimeProofStore store;
 EncoderWindow e;
 e.codec="h264";e.mode="h264_hw";e.frames=21;e.sent=21;
 e.width=1728;e.height=1080;e.maxEncodeMs=21;
 DecoderReport d;d.codec="h264";d.decoded=50;d.keyframes=1;
 d.packets=160;d.width=1728;d.height=1080;
 store.recordEncoder("a",e,1000);
 assert(store.confirm("a",d,1002,false).codec.empty());
 assert(store.confirm("a",d,1002,true).codec=="h264");
 assert(store.confirm("a",d,1002,true).codec.empty());
 store.recordEncoder("b",e,1000);d.keyframes=0;
 assert(store.confirm("b",d,1002,true).codec.empty());
 d.keyframes=1;d.width=1280;
 assert(store.confirm("b",d,1002,true).codec.empty());
 d.width=1728; assert(store.confirm("b",d,1032,true).codec.empty());
 store.recordEncoder("c",e,1000);d.dropped=100;
 assert(store.confirm("c",d,1002,true).codec.empty());
 d.dropped=0;
 auto sw=e;sw.mode="h264_sw";store.recordEncoder("d",sw,1000);
 assert(store.confirm("d",d,1002,false).codec=="h264");
 auto perf=e;perf.maxEncodeMs=100;
 store.recordEncoder("slow",perf,1000);
 assert(store.confirm("slow",d,1002,true).codec.empty());
 std::cout<<"CODEC_RUNTIME_PROOF_TESTS_PASSED\n";
}
