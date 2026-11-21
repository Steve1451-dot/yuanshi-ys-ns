#pragma once

#include "mihoyo/input_packet.hpp"

namespace cloudgenshin::input {

// Both supported titles need pointer entry before SDK game-entry confirmation.
// Star Rail A/B testing reached login with a pointer, not repeated XInput A.
// This does not change the gameplay pad wire format or local button mappings.
inline bool ContinueViaGamepad(bool gamepad_transport,bool entered_game) {
    return gamepad_transport && entered_game;
}

inline bool FaceButtonPointerShortcut(bool entered_game) {
    return !entered_game;
}

inline bool ManualContinueOwnsPointer(bool continue_via_gamepad,bool auto_touch_held,bool auto_pad_held) {
    return !continue_via_gamepad && !auto_touch_held && !auto_pad_held;
}

inline std::uint32_t LauncherInputSource(bool gamepad_transport,bool touch_launcher) {
    if(!gamepad_transport) return kInputControlMouse;
    // This mask clears both mobile input devices at a real handoff.
    // resetGameInputControl does NOT enable or select either device.
    return touch_launcher ? (kInputControlTouchScreen | kInputControlGamepad)
                          : kInputControlGamepad;
}

// Freeze the mobile title-entry contract used by the successful .133/.138
// runs: one finger, fixed coordinates, Down -> 120 ms -> Up. This is a
// baseline for further verification, not proof that every pod accepts it.
// Failed Up keeps ownership of finger 0, including after the entry sequence
// stops; a new/manual Down must not steal an unreleased contact.
class LauncherPointerTap {
public:
    enum class Action {None,Down,Up};
    static constexpr double kX=0.5,kY=0.947;
    static constexpr std::int64_t kHoldMs=120;
    bool held() const {return held_;}
    Action next(std::int64_t now,bool may_start) const {
        if(now<due_) return Action::None;
        if(held_) return Action::Up;
        return may_start ? Action::Down : Action::None;
    }
    // Only the transport owner's accepted write changes the held state.
    void sent(Action action,bool accepted,std::int64_t now) {
        if(action==Action::Down && !held_) {
            held_=accepted;
            due_=now+(accepted?kHoldMs:500);
        } else if(action==Action::Up && held_) {
            if(accepted) {held_=false;due_=0;}
            else due_=now+100;
        }
    }
    void stop(std::int64_t now) {if(held_ && due_>now) due_=now;}
    // A dead channel cannot be released. Its replacement clears stale input
    // once before accepting another tap; never replay the old Down there.
    void reset() {held_=false;due_=0;}
private:
    bool held_=false;
    std::int64_t due_=0;
};

} // namespace cloudgenshin::input
