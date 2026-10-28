#pragma once

// Independent of WebRTC and Media Foundation so the sustained health policy
// can be tested on both Windows and Linux. No codec switches on a live RTP
// stream: a trip only informs the next negotiated Auto session.
class SustainedEncoderHealth {
public:
    bool observeWindow(unsigned int frames, double encodeTotalMs,
                       double encodeMaxMs, int targetFps) {
        if (tripped_) return false;
        if (frames < 8 || targetFps < 12) {
            unhealthyWindows_ = 0;
            return false;
        }
        const double budgetMs = 1000.0 / static_cast<double>(targetFps);
        const double averageMs = encodeTotalMs / static_cast<double>(frames);
        // Require both sustained average pressure and a material tail
        // latency problem. A single slow keyframe must not demote VP9.
        const bool unhealthy = averageMs > 0.72 * budgetMs &&
                               encodeMaxMs > 1.4 * budgetMs;
        unhealthyWindows_ = unhealthy ? unhealthyWindows_ + 1 : 0;
        if (unhealthyWindows_ < 2) return false;
        tripped_ = true;
        return true;
    }

private:
    unsigned int unhealthyWindows_ = 0;
    bool tripped_ = false;
};
