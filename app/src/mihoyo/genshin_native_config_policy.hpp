#pragma once
#include <fstream>
#include <string>
#include <string_view>

namespace cloudgenshin {
// Native ys configuration read-back policy.
// - Keybinds (default): the two native controller-map arrays plus the input-mode
//   fields exactly as the game saved them. This is what makes the in-game
//   "control device = gamepad" and "main-UI device auto-switch" switches survive
//   to the next session. Nothing is forced.
// - Exact (opt-in): return the original game string, including its input-device
//   settings, without reserializing, filtering maps or forcing any mode switch.
// - Off: save only; cloud_get_data returns "" for the saved keys.
// - Full: legacy A/B path that returns the whole saved GENERAL_DATA and forces
//   enableInputHotSwap=true. It reproduces the 0.2.116/0.2.117 failure and is
//   kept only for controlled comparison.
enum class NativeConfigRestoreMode { Off, Keybinds, Full, Exact };

inline const char* NativeConfigRestoreModeName(NativeConfigRestoreMode mode) {
    switch(mode) {
    case NativeConfigRestoreMode::Keybinds: return "GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON";
    case NativeConfigRestoreMode::Full: return "GAME_SDK_NATIVE_CONFIG_RESTORE_ON";
    case NativeConfigRestoreMode::Exact: return "GAME_SDK_NATIVE_CONFIG_EXACT_ON";
    default: return "GAME_SDK_NATIVE_CONFIG_RESTORE_OFF";
    }
}

// Missing marker means Keybinds: the validated behaviour is "return the saved
// maps and the game's own input-mode values". An empty marker is an explicit
// opt-out to save-only.
inline NativeConfigRestoreMode NativeConfigRestoreModeFor(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file.is_open()) return NativeConfigRestoreMode::Keybinds;
    char bytes[64]{};
    file.read(bytes,sizeof(bytes));
    if(file.bad()) return NativeConfigRestoreMode::Off;
    std::string_view value(bytes,static_cast<std::size_t>(file.gcount()));
    // Accept LF, CRLF, or no trailing newline.
    if(value.ends_with("\r\n")) value.remove_suffix(2);
    else if(value.ends_with("\n")) value.remove_suffix(1);
    if(value.empty()) return NativeConfigRestoreMode::Off;
    if(value=="CGNX_NATIVE_CONFIG_RESTORE_OFF_V1") return NativeConfigRestoreMode::Off;
    if(value=="CGNX_NATIVE_CONFIG_RESTORE_V1") return NativeConfigRestoreMode::Full;
    if(value=="CGNX_NATIVE_CONFIG_EXACT_V1") return NativeConfigRestoreMode::Exact;
    if(value=="CGNX_NATIVE_KEYBINDS_RESTORE_V1") return NativeConfigRestoreMode::Keybinds;
    // Unknown content (including a BOM or trailing spaces) must not silently
    // enable a restore path. The caller reports this distinctly from a user
    // opt-out so the log cannot be misread.
    return NativeConfigRestoreMode::Off;
}

// True when the marker exists but its content is not a known token. Separated
// so the log can show "unknown marker", not "user chose Off".
inline bool NativeConfigMarkerUnknown(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file.is_open()) return false;
    char bytes[64]{};
    file.read(bytes,sizeof(bytes));
    if(file.bad()) return false;
    std::string_view value(bytes,static_cast<std::size_t>(file.gcount()));
    if(value.ends_with("\r\n")) value.remove_suffix(2);
    else if(value.ends_with("\n")) value.remove_suffix(1);
    if(value.empty()) return false;
    return value!="CGNX_NATIVE_CONFIG_RESTORE_OFF_V1" &&
        value!="CGNX_NATIVE_CONFIG_RESTORE_V1" &&
        value!="CGNX_NATIVE_CONFIG_EXACT_V1" &&
        value!="CGNX_NATIVE_KEYBINDS_RESTORE_V1";
}
} // namespace cloudgenshin
