#include "mihoyo/idle_gamepad_policy.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace cloudgenshin::input;
    assert(SuppressIdleGamepadReports("4",3));
    assert(!SuppressIdleGamepadReports("4",10));
    assert(!SuppressIdleGamepadReports("8",3));
    IdleGamepadPolicy policy;
    assert(policy.needs_report(false,1));
    // A failed first send/release must not consume the pending neutral.
    assert(policy.needs_report(false,1));
    policy.delivered(false,1);
    for(int frame=0;frame<600;++frame) assert(!policy.needs_report(false,1));
    assert(policy.needs_report(false,1,true)); // UI capture forces its own handoff.
    assert(policy.needs_report(true,1));
    policy.delivered(true,1);
    assert(policy.needs_report(true,1)); // Held input still refreshes over weak links.
    assert(policy.needs_report(false,1)); // Release always sends.
    assert(policy.needs_report(false,1)); // Failed release retries unchanged.
    policy.delivered(false,1);
    assert(!policy.needs_report(false,1));
    assert(policy.needs_report(false,2)); // Recovery cannot reuse the old cache.
    policy.delivered(false,2);
    assert(!policy.needs_report(false,2));
    policy.invalidate(); // Successful reset/device re-announcement invalidates.
    assert(policy.needs_report(false,2));
    std::cout<<"PASS idle gamepad policy: scope, no neutral flood, held refresh, release retry and route/UI handoff\n";
}
