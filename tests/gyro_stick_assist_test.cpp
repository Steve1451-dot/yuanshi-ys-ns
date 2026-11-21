#include "mihoyo/gyro_stick_assist.hpp"
#include "mihoyo/controller_capture_policy.hpp"
#include "controller_test_fields.hpp"
#include <cassert>
#include <iostream>
#include <limits>
using namespace cloudgenshin::input;

static MotionSample Moving(std::int64_t now) {
    MotionTracker tracker;
    return tracker.update(0,1,now,{0,1,0},{0.10f,0.20f,0.30f});
}
static void SamePhysicalControls(const SwitchControllerState& raw,const SwitchControllerState& out) {
    for(const auto member:kControllerTestButtons) assert(raw.*member==out.*member);
    assert(raw.left_x==out.left_x && raw.left_y==out.left_y);
}
static SwitchControllerState ComposeOnce(GyroStickAssist& gyro,const GyroSettings& config,
        SwitchControllerState raw,MotionSample sample,bool allowed=true,std::optional<bool> zl={}) {
    gyro={};sample.sampled_ms=1000;
    const auto out=gyro.apply(config,raw,sample,1000,allowed,zl);
    SamePhysicalControls(raw,out);return out;
}
int main() {
    const auto sample=Moving(1000);assert(sample.valid);
    GyroStickAssist gyro;
    GyroSettings config;
    assert(config.gyro_assist && config.gyro_aim_only && config.gyro_invert_x);
    SwitchControllerState raw;raw.zl=true;raw.a=true;raw.left_x=12000;
    config.gyro_assist=false;
    assert(ComposeOnce(gyro,config,raw,sample).right_x==0 && !gyro.active());
    config.gyro_assist=true;
    const auto result=ComposeOnce(gyro,config,raw,sample);
    assert(result.right_x< -6000 && result.right_y>6000 && gyro.active());
    assert(result.left_x==raw.left_x && result.zl && result.a && raw.right_x==0);
    // The real ZL is the activation key, not whichever source maps to game LT.
    auto no_game_lt=raw;no_game_lt.zl=false;
    assert(ComposeOnce(gyro,config,no_game_lt,sample,true,true).right_x==result.right_x);
    assert(ComposeOnce(gyro,config,raw,sample,true,false).right_x==0 && !gyro.active());
    assert(ComposeOnce(gyro,config,no_game_lt,sample,true,false).right_y==0);
    config.gyro_invert_x=false;config.gyro_invert_y=true;
    const auto inverted=ComposeOnce(gyro,config,raw,sample);
    assert(inverted.right_x==-result.right_x && inverted.right_y==-result.right_y);
    config.gyro_invert_x=true;config.gyro_invert_y=false;
    raw.right_x=21000;raw.right_y=-22000;
    const auto override=ComposeOnce(gyro,config,raw,sample);
    assert(override.right_x==21000 && override.right_y==-22000 && !gyro.active());
    raw.right_x=raw.right_y=0;raw.zl=false;
    assert(ComposeOnce(gyro,config,raw,sample).right_x==0);
    config.gyro_aim_only=false;
    assert(ComposeOnce(gyro,config,raw,sample).right_x!=0);
    auto bad=sample;bad.valid=false;
    assert(ComposeOnce(gyro,config,raw,bad).right_x==0);
    bad=sample;bad.gyro[2]=std::numeric_limits<float>::infinity();
    assert(ComposeOnce(gyro,config,raw,bad).right_x==0);
    bad=sample;bad.sequence=0;assert(ComposeOnce(gyro,config,raw,bad).right_x==0);
    bad=sample;bad.source=-1;assert(ComposeOnce(gyro,config,raw,bad).right_x==0);
    bad=sample;bad.gyro={0,0,0};
    assert(ComposeOnce(gyro,config,raw,bad).right_x==0 && !gyro.active());
    bad.gyro={0.001f,0.001f,0.001f};
    assert(ComposeOnce(gyro,config,raw,bad).right_x==0 && !gyro.active());
    bad=sample;bad.gyro={20,20,20};
    const auto limited=ComposeOnce(gyro,config,raw,bad);
    assert(limited.right_x==26000 && limited.right_y==-26000);
    bad.gyro={-20,-20,-20};const auto reversed=ComposeOnce(gyro,config,raw,bad);
    assert(reversed.right_x==-26000 && reversed.right_y==26000);
    raw.right_x=std::numeric_limits<int>::min();
    assert(ComposeOnce(gyro,config,raw,sample).right_x==raw.right_x);
    config.gyro_sensitivity=51;raw.right_x=0;
    assert(ComposeOnce(gyro,config,raw,sample).right_x==0);
    assert(ComposeOnce(gyro,{},raw,sample,false).right_x==0);

    // No activation or resumption delay: compose on the first eligible tick.
    gyro={};config={};raw={};raw.zl=raw.zr=raw.a=true;raw.right_x=21000;
    for(int t:{0,99,100}) {
        const auto out=gyro.apply(config,raw,Moving(t),t,true);
        SamePhysicalControls(raw,out);assert(out.right_x==21000);
        assert(gyro.active());
    }
    // The existing 100ms sensor-freshness limit is NOT an activation delay.
    assert(gyro.apply(config,raw,Moving(0),101,true).right_y==0 && !gyro.active());
    assert(gyro.apply(config,raw,Moving(103),102,true).right_y==0 && !gyro.active());
    // Releasing aim / UI ownership blocks only while that condition holds.
    raw.zl=false;gyro.apply(config,raw,Moving(110),110,true);
    raw.zl=true;assert(gyro.apply(config,raw,Moving(120),120,true).right_y!=0);
    assert(gyro.apply(config,raw,Moving(219),219,true).right_y!=0);
    assert(gyro.apply(config,raw,Moving(220),220,true).right_y!=0);
    gyro.apply(config,raw,Moving(221),221,false);
    assert(gyro.apply(config,raw,Moving(222),222,true).right_y!=0);
    assert(gyro.apply(config,raw,Moving(322),322,true).right_y!=0);
    // A current sensor sample needs no historic activation timestamp.
    assert(gyro.apply(config,raw,Moving(200),200,true).right_y!=0);
    assert(gyro.apply(config,raw,Moving(299),299,true).right_y!=0);
    assert(gyro.apply(config,raw,Moving(300),300,true).right_y!=0);

    // Xbox B/X are Switch A/Y in the unchanged physical-position layout.
    // Both press orders, simultaneous input and LB after gyro is already on.
    for(const auto face:{&SwitchControllerState::a,&SwitchControllerState::y}) {
        for(int order:{0,1,2,3}) {
            gyro={};raw={};raw.zl=true;
            if(order==0) raw.l=true;
            if(order==1) raw.*face=true;
            if(order==2) {raw.l=true;raw.*face=true;}
            gyro.apply(config,raw,Moving(0),0,true);
            if(order==3) assert(gyro.apply(config,raw,Moving(100),100,true).right_y!=0);
            raw.l=true;raw.*face=true;
            for(int t:{150,249,250,1000}) {
                const auto out=gyro.apply(config,raw,Moving(t),t,true);
                SamePhysicalControls(raw,out);assert(out.right_x==0 && out.right_y==0 && !gyro.active());
                const auto pad=ToGamepadState(out,0);
                assert(pad.buttons==(face==&SwitchControllerState::a?0x2100u:0x4100u));
                assert(pad.left_trigger==255);
                DiagnosticsChord diagnostics;
                assert(!diagnostics.update(raw,t).capture);
            }
            // LB released first: keep face/aim and restore gyro immediately.
            raw.l=false;
            assert(gyro.apply(config,raw,Moving(1001),1001,true).right_y!=0);
            assert(gyro.apply(config,raw,Moving(1100),1100,true).right_y!=0);
            const auto resumed=gyro.apply(config,raw,Moving(1101),1101,true);
            SamePhysicalControls(raw,resumed);assert(resumed.right_y!=0);
            // Face released first: still no gyro while LB remains held.
            raw.l=true;raw.*face=false;
            assert(gyro.apply(config,raw,Moving(1200),1200,true).right_y==0);
            assert(gyro.apply(config,raw,Moving(2200),2200,true).right_y==0);
        }
    }
    // All button masks: no consumed controls, aim/shoot stays usable.
    for(unsigned mask=0;mask<65536;++mask) {
        raw={};for(unsigned i=0;i<16;++i) raw.*kControllerTestButtons[i]=(mask&(1u<<i))!=0;
        const auto out=ComposeOnce(gyro,config,raw,sample);
        if(raw.l || !raw.zl) assert(out.right_x==0 && out.right_y==0 && !gyro.active());
        else assert(out.right_x!=0 && gyro.active());
    }
    std::cout<<"PASS gyro: immediate activation/resume, LB priority, chord orders, all button masks, sensor/axis safety\n";
}
