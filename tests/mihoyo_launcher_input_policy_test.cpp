#include "mihoyo/launcher_input_policy.hpp"
#include "mihoyo/stream_input_policy.hpp"
#include <cassert>

int main() {
    using namespace cloudgenshin::input;
    // Shared Genshin/Star Rail entry uses the pointer; gameplay Continue is
    // still gamepad-only after the SDK entry signal, never both input paths.
    for(const bool pad:{false,true}) {
        assert(!ContinueViaGamepad(pad,false));
        assert(ContinueViaGamepad(pad,true)==pad);
        assert(FaceButtonPointerShortcut(false));
        assert(!FaceButtonPointerShortcut(true));
        assert(LauncherInputSource(pad,true)==
               (pad?(kInputControlTouchScreen|kInputControlGamepad):kInputControlMouse));
        assert(LauncherInputSource(pad,false)==(pad?kInputControlGamepad:kInputControlMouse));
    }
    // Continue still maps to one held XInput A (Switch B), and the next
    // neutral frame releases it; physical A/B mappings are not swapped.
    SwitchControllerState physical;
    physical.a=true;
    const auto held=ComposeGamepadFrame(physical,true,false);
    assert(held.b && !held.a);
    assert(ControlsReleased(ComposeGamepadFrame({},false,false)));
    for(const bool pad:{false,true}) for(const bool touch_held:{false,true}) for(const bool pad_held:{false,true})
        assert(ManualContinueOwnsPointer(pad,touch_held,pad_held)==(!pad && !touch_held && !pad_held));
    // A pending automatic Up must be accepted before manual Down can start.
    ManualContinueClick manual;
    bool auto_held=true;
    assert(!ManualContinueOwnsPointer(false,auto_held,false) && !manual.active());
    auto_held=false; // Successful automatic release, not a failed send.
    assert(ManualContinueOwnsPointer(false,auto_held,false));
    assert(manual.request(1000));
    assert(manual.next(1000)==ManualContinueClick::Action::Move);
    // The successful mobile baseline has one stable tap, not a per-attempt
    // A/B/keyboard/coordinate rotation. Only accepted Down owns finger 0.
    using Tap=LauncherPointerTap;
    using Action=Tap::Action;
    static_assert(Tap::kX==0.5 && Tap::kY==0.947 && Tap::kHoldMs==120);
    Tap tap;
    assert(tap.next(0,false)==Action::None && !tap.held());
    for(unsigned attempt=0;attempt<60;++attempt) {
        const auto start=1000+attempt*3000;
        assert(tap.next(start,true)==Action::Down);
        tap.sent(Action::Down,true,start);
        assert(tap.held());
        for(int dt=0;dt<120;dt+=16)
            assert(tap.next(start+dt,true)==Action::None);
        // Even an already requested next tap cannot precede the release.
        assert(tap.next(start+120,true)==Action::Up);
        tap.sent(Action::Up,true,start+120);
        assert(!tap.held() && tap.next(start+121,false)==Action::None);
    }
    Tap failed;
    failed.sent(Action::Down,false,1000);
    assert(!failed.held() && failed.next(1499,true)==Action::None);
    assert(failed.next(1500,true)==Action::Down);
    failed.sent(Action::Down,true,1500);
    failed.sent(Action::Up,false,1620);
    assert(failed.held() && failed.next(1719,true)==Action::None);
    assert(failed.next(1720,false)==Action::Up);
    assert(!ManualContinueOwnsPointer(false,failed.held(),false));
    failed.sent(Action::Up,true,1720);
    assert(ManualContinueOwnsPointer(false,failed.held(),false));
    // SDK entry/manual takeover: release now, then retry a failed Up without
    // starting another Down, even though the automatic sequence has stopped.
    Tap stopped;
    stopped.sent(Action::Down,true,2000);
    stopped.stop(2001);
    assert(stopped.next(2001,false)==Action::Up);
    stopped.sent(Action::Up,false,2001);
    assert(stopped.next(2100,false)==Action::None);
    assert(stopped.next(2101,false)==Action::Up);
    stopped.sent(Action::Up,true,2101);
    assert(stopped.next(9999,false)==Action::None && !stopped.held());
    // A delayed worker still emits only Up, never a synthetic Move or Down.
    stopped.sent(Action::Down,true,10000);
    assert(stopped.next(11000,true)==Action::Up);
    stopped.reset(); // Lost channel: stale hold must not survive recovery.
    assert(!stopped.held() && stopped.next(11000,false)==Action::None);
    assert(stopped.next(11001,true)==Action::Down);
}
