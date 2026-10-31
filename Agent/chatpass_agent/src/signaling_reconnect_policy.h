#pragma once
// Bounded retry for transient RMM API restarts. Never use a codec or frame
// transport failure as a reason to kill the Windows Agent service.
#include <algorithm>
namespace hi5::signaling {
inline int reconnectDelaySeconds(int consecutiveFailures) {
    constexpr int values[] = {1,2,4,8,16,30};
    return values[std::clamp(consecutiveFailures,1,6)-1];
}
inline bool shouldReconnect(bool intentionalServiceStop,
                            bool signalConnected,
                            bool reconnectPending,
                            bool retryDeadlineReached) {
    return !intentionalServiceStop && !signalConnected &&
           reconnectPending && retryDeadlineReached;
}
}
