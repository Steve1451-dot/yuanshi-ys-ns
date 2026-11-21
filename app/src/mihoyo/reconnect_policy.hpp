#pragma once
#include <algorithm>
#include <cstdint>

namespace cloudgenshin::rtc {
// A burst of failures shares one budget. A successful handshake alone must
// not reset it: require 30 seconds of healthy operation before a fresh budget.
class ReconnectPolicy {
public:
    bool begin(std::int64_t now) {
        if(active_) return true;
        if(!used_ || (stable_ && now-stable_since_>=30000)) {
            attempts_=0;deadline_=now+30000;used_=true;
        }
        stable_=false;
        if(attempts_>=3 || now>=deadline_) return false;
        active_=true;next_=now+250;return true;
    }
    bool due(std::int64_t now) const {return active_ && now>=next_ && attempts_<3 && now<deadline_;}
    void attempted() {++attempts_;}
    void retry(std::int64_t now) {next_=now+std::min<std::int64_t>(2000,500LL*attempts_);}
    bool timed_out(std::int64_t now) const {return active_ && now>=deadline_;}
    bool no_attempts() const {return attempts_>=3;}
    unsigned attempts() const {return attempts_;}
    bool active() const {return active_;}
    void recovered(std::int64_t now) {active_=false;stable_=true;stable_since_=now;}
    void reset() {*this={};}
private:
    bool active_=false,used_=false,stable_=false;
    unsigned attempts_=0;
    std::int64_t deadline_=0,next_=0,stable_since_=0;
};
} // namespace cloudgenshin::rtc
