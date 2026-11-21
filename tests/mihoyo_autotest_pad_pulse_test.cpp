#include "mihoyo/autotest_pad_pulse.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>

int main() {
    using namespace cloudgenshin;
    for(const auto* key:{"A","B","X","Y","UP","DOWN","LEFT","RIGHT","L","R","ZL","ZR","PLUS","MINUS","L3","R3","LX+","LX-","LY+","LY-","RX+","RX-","RY+","RY-"}) {
        assert(AutoTestPadPulse::Parse(std::string("CGNX_PAD_PULSE_V1 1 ")+key+"\n"));
    }
    assert(AutoTestPadPulse::Parse("CGNX_PAD_PULSE_V1 32 B\r\n")->state.b);
    for(const auto* text:{"CGNX_PAD_PULSE_V1 0 B","CGNX_PAD_PULSE_V1 33 B","CGNX_PAD_PULSE_V1 -1 B",
        "CGNX_PAD_PULSE_V1 1 A+B","CGNX_PAD_PULSE_V1 1 B extra","CGNX_PAD_PULSE_V1 1 b","bad"})
        assert(!AutoTestPadPulse::Parse(text));
    const auto path=std::filesystem::temp_directory_path()/
        ("yuanshi-pad-pulse-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    AutoTestPadPulse pulse;
    assert(pulse.sample(path.string(),0,false).phase==0);
    {std::ofstream file(path);file<<"CGNX_PAD_PULSE_V1 1 B\n";}
    assert(!pulse.sample(path.string(),250,false).state.b && pulse.release_deadline()==0);
    assert(pulse.sample(path.string(),250,true).state.b && pulse.release_deadline()==650);
    assert(pulse.sample(path.string(),649,true).state.b);
    assert(!pulse.sample(path.string(),650,true).state.b && pulse.release_deadline()==0);
    assert(pulse.sample(path.string(),5000,true).phase==103); // Same request never repeats.
    {std::ofstream file(path);file<<"CGNX_PAD_PULSE_V1 2 RX+\n";}
    assert(pulse.sample(path.string(),5250,true).state.right_x==20000);
    assert(pulse.sample(path.string(),5400,false).state.right_x==0 && pulse.release_deadline()==0);
    assert(pulse.sample(path.string(),5650,true).state.right_x==0);
    {std::ofstream file(path);file<<"CGNX_PAD_PULSE_V1 1 A\n";}
    assert(!pulse.sample(path.string(),8000,true).state.a); // Old/out-of-order request ignored.
    std::filesystem::remove(path);
}
