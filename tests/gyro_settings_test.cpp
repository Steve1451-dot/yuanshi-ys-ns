#include "mihoyo/gyro_settings.hpp"
#include "mihoyo/gyro_stick_assist.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>

int main() {
    using namespace cloudgenshin::input;
    const auto path=std::filesystem::temp_directory_path()/
        ("yuanshi-gyro-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path);
    const auto home=path.string();
    assert(LoadGyroSettings(home)==GyroSettings{});
    // Removed remap/mode preferences must not influence buttons or gyro.
    {std::ofstream out(path/"controller-mapping-hk4e.txt");out<<"CGNX-PAD 4 hk4e\n0 0 0 0";}
    {std::ofstream out(path/"genshin-gamepad-mode.txt");out<<"YUANSHI-GAMEPAD 1 local\n";}
    assert(LoadGyroSettings(home)==GyroSettings{});
    GyroSettings config;config.gyro_assist=false;config.gyro_sensitivity=150;
    assert(SaveGyroSettings(home,config) && LoadGyroSettings(home)==config);
    std::filesystem::copy_file(path/"genshin-gyro.txt",path/"genshin-gyro.txt.bak");
    {std::ofstream out(path/"genshin-gyro.txt");out<<"damaged";}
    assert(LoadGyroSettings(home)==config);
    auto invalid=config;invalid.gyro_sensitivity=99;
    assert(!SaveGyroSettings(home,invalid) && LoadGyroSettings(home)==config);
    assert(!SaveGyroSettings(home+"/missing",config));
    GyroStickAssist gyro;
    SwitchControllerState pad;pad.a=pad.r=pad.zl=pad.minus=true;
    MotionSample sample;sample.valid=true;sample.source=0;sample.sequence=1;sample.sampled_ms=100;
    sample.gyro={0,-0.2f,-0.3f};
    config={};
    const auto aim=gyro.apply(config,pad,sample,100,true,pad.zl);
    assert(gyro.active() && aim.right_x<0 && aim.right_y>0);
    assert(aim.a && aim.r && aim.zl && aim.minus && !aim.b && !aim.x && !aim.y);
    assert(!gyro.apply(config,pad,sample,100,false,pad.zl).right_x); // Local menu owns input.
    std::filesystem::remove_all(path);
}
