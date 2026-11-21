#include "mihoyo/reconnect_policy.hpp"
#include <cassert>
int main() {
    using cloudgenshin::rtc::ReconnectPolicy;
    ReconnectPolicy p;
    assert(p.begin(0) && !p.due(249) && p.due(250));
    p.attempted();p.retry(300);assert(!p.due(799) && p.due(800));
    p.attempted();p.recovered(1000);assert(!p.active());
    assert(p.begin(1100));p.attempted();p.retry(1200);
    assert(p.no_attempts() && !p.due(9000));
    p.recovered(2000);assert(!p.begin(31999)); // A flap cannot reset the budget.
    p.reset();assert(p.begin(0));p.attempted();p.recovered(1000);
    assert(p.begin(31000) && p.attempts()==0);
    assert(!p.timed_out(60999) && p.timed_out(61000));
    p.reset();assert(p.begin(50));assert(p.timed_out(30050));
}
