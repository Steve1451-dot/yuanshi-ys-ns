#include "stream/WorkerScheduling.hpp"
#include <cassert>
#include <limits>
#include <initializer_list>
using namespace opennow::stream;
int main() {
    constexpr auto all=std::numeric_limits<std::uint64_t>::max();
    static_assert(WorkerPriority(WorkerRole::Receive)>0x2c);
    static_assert(WorkerPriority(WorkerRole::Receive)<WorkerPriority(WorkerRole::Audio));
    static_assert(WorkerPriority(WorkerRole::Audio)<WorkerPriority(WorkerRole::Decode));
    static_assert(WorkerPriority(WorkerRole::Decode)<0x3b);
    static_assert(!PriorityAllowed(all,-1) && !PriorityAllowed(all,64));
    for(auto role:{WorkerRole::Receive,WorkerRole::Audio,WorkerRole::Decode,WorkerRole::Background}) {
        const int requested=WorkerPriority(role);
        assert(SelectWorkerPriority(role,0x3b,all)==requested);
        assert(SelectWorkerPriority(role,0x3b,0)==0x3b);
        assert(SelectWorkerPriority(role,0x3b,all&~(std::uint64_t{1}<<requested))==0x3b);
        assert(SelectWorkerPriority(role,-1,all)==-1);
        if(role!=WorkerRole::Background) assert(SelectWorkerPriority(role,0x2c,all)==0x2c);
    }
    WorkerScheduling host;host.apply(WorkerRole::Receive);
    assert(host.actual()==-1 && host.error()==0); // No host priority side effects.
}
