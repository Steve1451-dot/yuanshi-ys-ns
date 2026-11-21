#pragma once
#include <atomic>
#include <cstdint>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace opennow::stream {
enum class WorkerRole { Receive, Audio, Decode, Background };
// Below the usual UI priority (0x2c), above default newlib workers (0x3b).
// Preserve libnx's allowed core mask: do not pin workers or overclock.
constexpr int WorkerPriority(WorkerRole role) {
    switch(role) {
    case WorkerRole::Receive: return 0x32;
    case WorkerRole::Audio: return 0x33;
    case WorkerRole::Decode: return 0x34;
    case WorkerRole::Background: return 0x3c;
    }
    return 0x3b;
}
constexpr bool PriorityAllowed(std::uint64_t mask,int priority) {
    return priority>=0 && priority<64 && (mask & (std::uint64_t{1}<<priority));
}
constexpr int SelectWorkerPriority(WorkerRole role,int current,std::uint64_t allowed) {
    const int target=WorkerPriority(role);
    if(!PriorityAllowed(allowed,target) || current<0 || current>=64) return current;
    // Do not demote a foreground worker already assigned a higher priority.
    return role==WorkerRole::Background || target<current ? target : current;
}
class WorkerScheduling {
public:
    void apply(WorkerRole role) {
        actual_=-1;error_=0;
#ifdef __SWITCH__
        s32 current=-1;
        auto rc=svcGetThreadPriority(&current,CUR_THREAD_HANDLE);
        if(R_FAILED(rc)) {error_=static_cast<int>(rc);return;}
        actual_=current;
        u64 mask=0;
        rc=svcGetInfo(&mask,InfoType_PriorityMask,CUR_PROCESS_HANDLE,0);
        if(R_FAILED(rc)) {error_=static_cast<int>(rc);return;}
        const int selected=SelectWorkerPriority(role,current,mask);
        if(selected!=current) {
            rc=svcSetThreadPriority(CUR_THREAD_HANDLE,static_cast<u32>(selected));
            if(R_FAILED(rc)) {error_=static_cast<int>(rc);return;}
        }
        rc=svcGetThreadPriority(&current,CUR_THREAD_HANDLE);
        if(R_SUCCEEDED(rc)) actual_=current;
        else error_=static_cast<int>(rc);
#else
        (void)role; // Host tests must not alter the host's scheduler.
#endif
    }
    int actual() const {return actual_.load(std::memory_order_relaxed);}
    int error() const {return error_.load(std::memory_order_relaxed);}
private:
    std::atomic_int actual_{-1},error_{0};
};
}
