#pragma once
#include <cstdint>

namespace opennow::webrtc {
struct PeerDrainResult {
    unsigned packets=0;bool exhausted=false;
    std::uint64_t call_max_us=0,slow_calls=0;
};
// Cooperative work budget: cannot preempt a slow callback/socket call.
// Exhaustion is a hint, not proof of queued packets or packet loss.
template<class Poll,class Microseconds>
PeerDrainResult DrainPeerPacketsBudgeted(Poll&& poll,Microseconds&& microseconds) {
    const std::int64_t started=microseconds();
    auto previous=started;
    PeerDrainResult result;
    while(result.packets<64) {
        const int received=poll();
        const auto now=microseconds();
        const auto elapsed=static_cast<std::uint64_t>(now>previous ? now-previous : 0);
        if(elapsed>result.call_max_us) result.call_max_us=elapsed;
        if(elapsed>16667) ++result.slow_calls;
        previous=now;
        // Even an idle/error call can be slow; keep its timing, but do not
        // invent a backlog or busy-spin when it reported no work.
        if(received<=0) return result;
        ++result.packets;
        if(now-started>=2000) {result.exhausted=true;return result;}
    }
    result.exhausted=true;return result;
}
template<class Poll,class Microseconds>
unsigned DrainPeerPackets(Poll&& poll,Microseconds&& microseconds) {
    return DrainPeerPacketsBudgeted(poll,microseconds).packets;
}
template<class Drain,class Signal>
void PollMediaAroundSignaling(Drain&& drain,Signal&& signal) {
    drain();signal();drain();
}
} // namespace opennow::webrtc
