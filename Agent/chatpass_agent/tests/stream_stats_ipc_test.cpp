#include "../src/ipc/input_pipe.h"
#include <windows.h>
#include <cmath>
#include <iostream>
#include <string>

int main() {
    const std::string name = "Local\\H5CTestStreamStats-" + std::to_string(GetCurrentProcessId());
    hi5::InputPipeWriter writer;
    hi5::InputPipeReader reader;
    if (!writer.Create(name) || !reader.Open(name)) return 1;
    hi5::StreamStats stats{};
    stats.unixMs = 123456789ULL;
    stats.captureAttempts = 43;
    stats.writtenFrames = 42;
    stats.targetFps = 20;
    stats.captureAvgMs = 121.537;
    stats.captureMaxMs = 182.901;
    stats.sharedWriteAvgMs = 2.179;
    stats.sharedWriteMaxMs = 6.842;
    writer.PublishStreamStats(stats);
    hi5::StreamStats out{};
    if (!reader.ReadStreamStats(0, out)) return 2;
    if (out.captureAttempts != 43 || out.writtenFrames != 42 ||
        out.targetFps != 20 || out.unixMs != 123456789ULL) return 3;
    const auto closeEnough = [](double a, double b) { return std::abs(a-b) < 0.002; };
    if (!closeEnough(out.captureAvgMs, stats.captureAvgMs) ||
        !closeEnough(out.captureMaxMs, stats.captureMaxMs) ||
        !closeEnough(out.sharedWriteAvgMs, stats.sharedWriteAvgMs) ||
        !closeEnough(out.sharedWriteMaxMs, stats.sharedWriteMaxMs)) return 4;
    if (reader.ReadStreamStats(out.seq, out)) return 5;
    std::cout << "Windows helper-to-service timing roundtrip passed\n";
    return 0;
}
