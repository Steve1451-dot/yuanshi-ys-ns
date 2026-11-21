#include "mihoyo/autotest_pad_policy.hpp"
#include "mihoyo/controller_capture_policy.hpp"
#include "mihoyo/controller_source_policy.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>

int main() {
    using namespace cloudgenshin;
    AutoTestPadProbe probe;
    assert(probe.sample(999999).phase==0);
    probe.arm(1000);probe.arm(9000); // First game-enter only, no rearming.
    assert(probe.sample(999).phase==0 && probe.sample(15999).phase==0);
    assert(probe.sample(16000).state.right_x==20000);
    assert(probe.sample(18000).phase==2);
    // Plus is held for 700 ms so the game's polling loop reliably sees it.
    assert(probe.sample(21000).state.plus && probe.sample(21600).state.plus);
    assert(!probe.sample(21700).state.plus && probe.sample(21700).phase==4);
    assert(probe.sample(27000).phase==5 && !probe.sample(27000).state.a && !probe.sample(27000).state.b);
    assert(probe.sample(27200).phase==6 && !probe.sample(27200).state.plus);
    // A second Plus, well after the entry prompt, is held for the same 700 ms.
    // arm() happened at 1000 ms, so absolute sample times are +1000.
    assert(probe.sample(45999).phase==6 && !probe.sample(45999).state.plus);
    assert(probe.sample(46000).phase==7 && probe.sample(46600).state.plus);
    assert(probe.sample(46700).phase==8 && !probe.sample(46700).state.plus);
    // Reproduce the UI/network filtering order offline. Physical-neutral
    // observation must not erase the synthetic right stick or standalone Plus.
    input::ControllerSystemButtons buttons;
    input::InputReleaseGate gate;
    input::ControllerNeutralHandoff handoff;
    gate.arm(1000);
    for(const auto now:{2000,16000,17000,18000,21000,21600,21700,46000,46600,46700}) {
        buttons.observe({});
        auto state=probe.sample(now).state;
        const auto raw=state;
        if(gate.suppress(raw,now)) state={};
        buttons.filter(state);
        if(handoff.pending(1)) {state={};handoff.complete(1,true);}
        assert(state.right_x==((now==16000 || now==17000)?20000:0));
        assert(state.plus==(now==21000 || now==21600 || now==46000 || now==46600));
    }
    const auto path=std::filesystem::temp_directory_path()/
        ("cg-pad-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(!AutoTestPadEnabled(path.string()));
    {std::ofstream file(path,std::ios::binary);file<<"CGNX_PAD_PROBE_V1\n";}
    assert(AutoTestPadEnabled(path.string()));
    {std::ofstream file(path,std::ios::app|std::ios::binary);file<<"extra\n";}
    assert(!AutoTestPadEnabled(path.string()));
    std::filesystem::remove(path);
    assert(AutoTestXboxProductId(path.string(),true,true)==input::kXboxProductId);
    for(const auto token:{"CGNX_XBOX360_PROBE_V1","CGNX_XBOX360_PROBE_V1\n","CGNX_XBOX360_PROBE_V1\r\n"}) {
        {std::ofstream file(path,std::ios::binary);file<<token;}
        assert(AutoTestXboxProductId(path.string(),true,true)==0x028e);
        assert(AutoTestXboxProductId(path.string(),false,true)==input::kXboxProductId);
        assert(AutoTestXboxProductId(path.string(),true,false)==input::kXboxProductId);
    }
    for(const auto token:{"","CGNX_XBOX360_PROBE_V1 ","CGNX_XBOX360_PROBE_V1\nextra",
                          "CGNX_XBOX360_PROBE_V1\n\n","CGNX_XBOX360_PROBE_V10"}) {
        {std::ofstream file(path,std::ios::binary);file<<token;}
        assert(AutoTestXboxProductId(path.string(),true,true)==input::kXboxProductId);
    }
    std::filesystem::remove(path);
}
