#pragma once
#include "mihoyo/input_packet.hpp"
#include <array>
// Enumeration for exhaustive physical-input tests, not a remapping table.
inline constexpr std::array<bool cloudgenshin::input::SwitchControllerState::*,16> kControllerTestButtons{
    &cloudgenshin::input::SwitchControllerState::a,&cloudgenshin::input::SwitchControllerState::b,
    &cloudgenshin::input::SwitchControllerState::x,&cloudgenshin::input::SwitchControllerState::y,
    &cloudgenshin::input::SwitchControllerState::l,&cloudgenshin::input::SwitchControllerState::r,
    &cloudgenshin::input::SwitchControllerState::zl,&cloudgenshin::input::SwitchControllerState::zr,
    &cloudgenshin::input::SwitchControllerState::stick_left,&cloudgenshin::input::SwitchControllerState::stick_right,
    &cloudgenshin::input::SwitchControllerState::plus,&cloudgenshin::input::SwitchControllerState::minus,
    &cloudgenshin::input::SwitchControllerState::dpad_up,&cloudgenshin::input::SwitchControllerState::dpad_down,
    &cloudgenshin::input::SwitchControllerState::dpad_left,&cloudgenshin::input::SwitchControllerState::dpad_right};
