#pragma once
#include "mihoyo/input_packet.hpp"
#include <charconv>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace cloudgenshin {
inline bool AutoTestManualPadEnabled(const std::string& path) {
    std::ifstream file(path,std::ios::binary);char bytes[40]{};file.read(bytes,sizeof(bytes));
    return file.eof() && std::string_view(bytes,static_cast<std::size_t>(file.gcount()))=="CGNX_PAD_MANUAL_V1\n";
}
// Supervised developer test, not a production input/mapping interface. The
// caller requires BOTH normal autotest and pad-test opt-ins plus the separate
// manual-pad marker. At most 32 single-control pulses, no chords or commands.
class AutoTestPadPulse {
public:
    struct Sample {input::SwitchControllerState state{};int phase=0;};
    struct Request {unsigned id=0;input::SwitchControllerState state{};};
    static std::optional<Request> Parse(std::string_view text) {
        constexpr std::string_view prefix="CGNX_PAD_PULSE_V1 ";
        if(text.ends_with("\r\n")) text.remove_suffix(2);
        else if(text.ends_with("\n")) text.remove_suffix(1);
        if(!text.starts_with(prefix) || text.size()>64) return {};
        text.remove_prefix(prefix.size());
        const auto space=text.find(' ');if(space==text.npos) return {};
        unsigned id=0;
        const auto parsed=std::from_chars(text.data(),text.data()+space,id);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+space || id==0 || id>32) return {};
        text.remove_prefix(space+1);
        input::SwitchControllerState state;
        using State=input::SwitchControllerState;
        struct Button {std::string_view name;bool State::*member;};
        constexpr Button keys[]={{"A",&State::a},{"B",&State::b},{"X",&State::x},{"Y",&State::y},
            {"UP",&State::dpad_up},{"DOWN",&State::dpad_down},{"LEFT",&State::dpad_left},{"RIGHT",&State::dpad_right},
            {"L",&State::l},{"R",&State::r},{"ZL",&State::zl},{"ZR",&State::zr},
            {"PLUS",&State::plus},{"MINUS",&State::minus},{"L3",&State::stick_left},{"R3",&State::stick_right}};
        for(const auto& key:keys) if(text==key.name) {state.*key.member=true;return Request{id,state};}
        struct Axis {std::string_view name;std::int32_t State::*member;};
        constexpr Axis axes[]={{"LX",&State::left_x},{"LY",&State::left_y},{"RX",&State::right_x},{"RY",&State::right_y}};
        for(const auto& axis:axes) if(text.size()==3 && text.substr(0,2)==axis.name && (text[2]=='+' || text[2]=='-')) {
            state.*axis.member=text[2]=='+'?20000:-20000;return Request{id,state};
        }
        return {};
    }
    Sample sample(const std::string& path,std::int64_t now,bool game_entered) {
        // Title/login is touch-only. Even an existing request file must not
        // inject a pad confirm before the SDK's real game-entry transition.
        if(!game_entered) {held_=false;phase_=0;return {};}
        if(held_ && now>=release_at_) {held_=false;phase_=static_cast<int>(last_id_*2+101);}
        if(held_) return {state_,phase_};
        if(now>=next_read_ && now>=settle_at_) {
            next_read_=now+250;
            std::ifstream file(path,std::ios::binary);char bytes[65]{};file.read(bytes,sizeof(bytes));
            const auto request=file.eof()?Parse(std::string_view(bytes,static_cast<std::size_t>(file.gcount()))):std::nullopt;
            if(request && request->id>last_id_) {
                last_id_=request->id;state_=request->state;held_=true;
                release_at_=now+400;settle_at_=now+1400;
                phase_=static_cast<int>(last_id_*2+100);
                return {state_,phase_};
            }
        }
        return {{},phase_}; // Accepted requests always return to neutral; no repeat.
    }
    std::int64_t release_deadline() const {return held_?release_at_:0;}
private:
    unsigned last_id_=0;
    int phase_=0;
    bool held_=false;
    input::SwitchControllerState state_;
    std::int64_t release_at_=0,settle_at_=0,next_read_=0;
};
}
