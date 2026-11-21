#pragma once
#include "mihoyo/input_packet.hpp"
#include <cstdint>
#include <fstream>
#include <string>

namespace cloudgenshin {
inline bool AutoTestPadEnabled(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    char bytes[32]{};
    file.read(bytes,sizeof(bytes));
    return file.eof() && std::string(bytes,static_cast<std::size_t>(file.gcount()))=="CGNX_PAD_PROBE_V1\n";
}
// One-variable diagnostic comparison, never a production preference or a local
// button map. Android's device allow-list is client-side; it does not prove
// which VID/PID combinations the cloud's virtual-pad driver accepts.
inline std::uint32_t AutoTestXboxProductId(const std::string& path,bool enabled,
                                          bool is_genshin) {
    if(!enabled || !is_genshin) return input::kXboxProductId;
    std::ifstream file(path,std::ios::binary);
    char bytes[48]{};
    file.read(bytes,sizeof(bytes));
    if(file.bad()) return input::kXboxProductId;
    std::string_view value(bytes,static_cast<std::size_t>(file.gcount()));
    if(value.ends_with("\r\n")) value.remove_suffix(2);
    else if(value.ends_with("\n")) value.remove_suffix(1);
    return value=="CGNX_XBOX360_PROBE_V1" ? 0x028e : input::kXboxProductId;
}
// A bounded, opt-in camera/menu probe, not gameplay automation. Apply before
// overlay/release handling so it traverses the same route as physical input.
// Never moves the character, attacks, confirms a dialog, purchases or chats.
class AutoTestPadProbe {
public:
    struct Sample {input::SwitchControllerState state{};int phase=0;};
    void arm(std::int64_t game_enter_ms) {if(start_ms_==0 && game_enter_ms>0) start_ms_=game_enter_ms;}
    Sample sample(std::int64_t now_ms) const {
        if(start_ms_==0 || now_ms<start_ms_) return {};
        const auto elapsed=now_ms-start_ms_;
        Sample result;
        if(elapsed<15000) return result;
        if(elapsed<17000) {result.phase=1;result.state.right_x=20000;}
        else if(elapsed<20000) result.phase=2;
        // A real Plus press is held for a few hundred ms. 200 ms was too short
        // for the title's polling loop to open the menu, so the probe reported
        // a false negative even when the pad worked.
        else if(elapsed<20700) {result.phase=3;result.state.plus=true;}
        else if(elapsed<26000) result.phase=4;
        // Confirm/cancel can be swapped in the user's game settings. Stop at
        // the menu instead of guessing a face button that could confirm Shop.
        else if(elapsed<26200) result.phase=5;
        // The first Plus lands while the "recommended quest" entry prompt can
        // still own the UI, which can swallow it. Press Plus once more well
        // after that prompt has cleared so the menu probe gets a fair chance.
        else if(elapsed<45000) result.phase=6;
        else if(elapsed<45700) {result.phase=7;result.state.plus=true;}
        else result.phase=8;
        return result;
    }
private:
    std::int64_t start_ms_=0;
};
}
