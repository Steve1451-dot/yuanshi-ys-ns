#pragma once
#include "mihoyo/sdk_json.hpp"
#include "mihoyo/genshin_native_config_policy.hpp"
#include "mihoyo/genshin_native_map.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cloudgenshin::sdk {
// Game-owned native settings, NOT a launcher-side button remapping table.
// The observed cloud_local_data format is encodeURIComponent(JSON), with
// GENERAL_DATA itself containing JSON text. Inspect only this known envelope.
//
// 0.2.120 review outcome: the previous "full config minus KeyboardMap plus
// forced switch values" payload was never validated on hardware, and forcing
// enableInputHotSwap / rewired* invented a state the game never wrote. Keybind
// mode filters maps and selected input fields. Exact is a separate opt-in
// comparison against Android's opaque file read-back; it never changes bytes.
enum class NativeBindReason {
    Okay,
    DecodeFailed,
    EnvelopeInvalid,
    ListsMissing,
    ListsMismatched,
    TooManyEntries,
    NoControllerMap,
    OverBudget,
    EncodeFailed,
    MalformedControllerXml,
};

struct NativeControllerRestore {
    std::size_t map_count = 0;
    std::size_t keyboard_map_count = 0;
    std::size_t unknown_map_count = 0;
    std::size_t dropped_empty_count = 0;
    std::size_t repaired_xml_count = 0;
    std::size_t invalid_xml_count = 0;
    NativeBindReason reason = NativeBindReason::Okay;
    std::optional<bool> hot_swap;
    std::optional<std::string> runtime_value;
};

// Safe, value-free summary used only for fixed diagnostic event codes.
struct NativeConfigInspection {
    bool valid = false;
    std::size_t map_count = 0;
    std::size_t keyboard_map_count = 0;
    std::size_t unknown_map_count = 0;
    bool hot_swap = false;
    bool keyboard_enabled = false;
    bool keyboard_disabled = false;
    unsigned joypad_user_count = 0;
};

inline char NativeConfigAsciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool NativeConfigContains(std::string_view text, std::string_view needle) {
    if (needle.empty() || text.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (NativeConfigAsciiLower(text[i + j]) != NativeConfigAsciiLower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// Non-gamepad rewrite maps. Rewired serialises every override into the same pair
// of arrays, so the only reliable exclusion is the keyboard/mouse/touch family.
inline bool IsKeyboardNativeMap(std::string_view key, std::string_view value) {
    return NativeConfigContains(value, "keyboardmap") ||
        NativeConfigContains(value, "mousemap") ||
        NativeConfigContains(value, "touchmap") ||
        NativeConfigContains(key, "keyboard") ||
        NativeConfigContains(key, "mouse") ||
        NativeConfigContains(key, "touch");
}

// A recognised controller/gamepad map root, used for reporting and reasoning.
// Selection does not depend on this list alone: dropping every map whose type is
// unknown would make restore fail for real saves (custom controllers, guitars,
// arcade sticks). Selection excludes only the keyboard/mouse/touch family and
// requires the value to look like a serialised map.
inline bool IsControllerNativeMap(std::string_view key, std::string_view value) {
    if (NativeConfigContains(value, "joystickmap") ||
        NativeConfigContains(value, "xinputmap") ||
        NativeConfigContains(value, "gamepadmap") ||
        NativeConfigContains(value, "customcontrollermap") ||
        NativeConfigContains(value, "controllermap") ||
        NativeConfigContains(value, "padmap")) return true;
    if (NativeConfigContains(key, "joystick") ||
        NativeConfigContains(key, "controller") ||
        NativeConfigContains(key, "xinput") ||
        NativeConfigContains(key, "gamepad") ||
        NativeConfigContains(key, "xbox") ||
        NativeConfigContains(key, "switch") ||
        NativeConfigContains(key, "dualshock") ||
        NativeConfigContains(key, "dualsense")) return true;
    return false;
}

// Values are serialised override documents. Accept XML-ish and JSON-ish forms so
// a different serialisation does not silently drop every real keybind.
inline bool LooksLikeSerializedMap(std::string_view value) {
    if (value.empty()) return false;
    const bool xml = value.find('<') != std::string_view::npos &&
        value.find('>') != std::string_view::npos;
    const bool json = value.find('{') != std::string_view::npos &&
        value.find('}') != std::string_view::npos;
    return xml || json;
}

// Single selection rule shared by restore, reporting and adoption comparison.
// Only the keyboard/mouse/touch family is excluded; any other override that
// still looks like a serialised map is kept. Dropping unrecognised-but-real
// controller maps would silently restore nothing, which defeats the purpose.
inline bool ShouldKeepNativeMap(std::string_view key, std::string_view value) {
    if (key.empty() || value.empty()) return false;
    if (IsKeyboardNativeMap(key, value)) return false;
    return LooksLikeSerializedMap(value);
}

inline std::optional<std::string> DecodeNativeConfigUri(std::string_view text) {
    if(text.empty() || text.size()>kSdkJsonMaximumBytes) return std::nullopt;
    const auto hex=[](char c)->int {
        if(c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        if(c>='A' && c<='F') return c-'A'+10;
        return -1;
    };
    std::string out;out.reserve(text.size());
    for(std::size_t i=0;i<text.size();++i) {
        if(text[i]!='%') {out+=text[i];continue;} // '+' is literal, not form-space.
        if(i+2>=text.size()) return std::nullopt;
        const int a=hex(text[i+1]),b=hex(text[i+2]);
        if(a<0 || b<0) return std::nullopt;
        out+=static_cast<char>((a<<4)|b);i+=2;
    }
    return out;
}
inline std::optional<std::string> EncodeNativeConfigUri(std::string_view text) {
    constexpr char hex[]="0123456789ABCDEF";
    std::string out;
    for(const unsigned char c:text) {
        const bool plain=(c>='a' && c<='z') || (c>='A' && c<='Z') ||
            (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='!' ||
            c=='~' || c=='*' || c=='\'' || c=='(' || c==')';
        const std::size_t count=plain?1:3;
        if(out.size()>kSdkJsonMaximumBytes-count) return std::nullopt;
        if(plain) out+=static_cast<char>(c);
        else {out+='%';out+=hex[c>>4];out+=hex[c&15];}
    }
    return out;
}

// Value-free comparison used as adoption evidence: do two saved configs carry
// the same controller-map key/value pairs? Never logs either payload.
inline bool CompareNativeConfigMaps(std::string_view written, std::string_view reply) {
    const auto decode = [](std::string_view text, nlohmann::json& maps_out) {
        const auto decoded = DecodeNativeConfigUri(text);
        if (!decoded) return false;
        const auto envelope = ParseSdkJson(*decoded);
        if (!envelope.is_object() || !envelope.contains("GENERAL_DATA") ||
            !envelope["GENERAL_DATA"].is_string()) return false;
        const auto general = ParseSdkJson(envelope["GENERAL_DATA"].get_ref<const std::string&>());
        constexpr auto keys = "_overrideControllerMapKeyList";
        constexpr auto maps = "_overrideControllerMapValueList";
        if (!general.is_object() || !general.contains(keys) || !general.contains(maps) ||
            !general[keys].is_array() || !general[maps].is_array() ||
            general[keys].size() != general[maps].size()) return false;
        maps_out = nlohmann::json::object();
        for (std::size_t i = 0; i < general[keys].size(); ++i) {
            if (!general[keys][i].is_string() || !general[maps][i].is_string()) continue;
            const auto& key = general[keys][i].get_ref<const std::string&>();
            const auto& map = general[maps][i].get_ref<const std::string&>();
            // Mirror selection order: an empty entry is dropped in every mode.
            if (!ShouldKeepNativeMap(key, map)) continue;
            maps_out[key] = map;
        }
        return true;
    };
    nlohmann::json a, b;
    if (!decode(written, a) || !decode(reply, b)) return false;
    return !a.empty() && a == b;
}

// Value-free structural description of a saved config, written to SD so the real
// field names and map root elements can be checked without a PC or a decrypted
// vault. Contains only key names, JSON types, entry counts and booleans; never
// map contents, tokens or account data.
inline std::string BuildNativeConfigStructureReport(std::string_view value) {
    std::string out;
    const auto decoded = DecodeNativeConfigUri(value);
    if (!decoded) return "decode=failed\n";
    const auto envelope = ParseSdkJson(*decoded);
    if (!envelope.is_object()) return "envelope=not_object\n";
    out += "envelope_bytes=" + std::to_string(value.size()) + "\n";
    out += "envelope_fields=" + std::to_string(envelope.size()) + "\n";
    for (auto it = envelope.begin(); it != envelope.end(); ++it) {
        // Only field names and primitive types; never values.
        const char* type = it.value().is_string() ? "string"
            : it.value().is_object() ? "object"
            : it.value().is_array() ? "array"
            : it.value().is_boolean() ? "bool"
            : it.value().is_number() ? "number" : "other";
        out += "envelope_field=" + it.key() + " type=" + type;
        if (it.value().is_string())
            out += " bytes=" + std::to_string(it.value().get_ref<const std::string&>().size());
        out += "\n";
    }
    if (!envelope.contains("GENERAL_DATA") || !envelope["GENERAL_DATA"].is_string())
        return out + "general=missing\n";
    const auto general = ParseSdkJson(envelope["GENERAL_DATA"].get_ref<const std::string&>());
    if (!general.is_object()) return out + "general=not_object\n";
    out += "general_fields=" + std::to_string(general.size()) + "\n";
    for (auto it = general.begin(); it != general.end(); ++it) {
        const char* type = it.value().is_string() ? "string"
            : it.value().is_object() ? "object"
            : it.value().is_array() ? "array"
            : it.value().is_boolean() ? "bool"
            : it.value().is_number() ? "number" : "other";
        out += "general_field=" + it.key() + " type=" + type;
        if (it.value().is_boolean()) out += std::string(" value=") + (it.value().get<bool>() ? "1" : "0");
        if (it.value().is_array()) out += " count=" + std::to_string(it.value().size());
        if (it.value().is_string())
            out += " bytes=" + std::to_string(it.value().get_ref<const std::string&>().size());
        out += "\n";
    }
    constexpr auto keys = "_overrideControllerMapKeyList";
    constexpr auto maps = "_overrideControllerMapValueList";
    if (!general.contains(keys) || !general[keys].is_array() ||
        !general.contains(maps) || !general[maps].is_array())
        return out + "map_lists=missing\n";
    out += "map_entries=" + std::to_string(general[keys].size()) + "\n";
    out += "map_entries_matched=" + std::to_string(general[maps].size()) + "\n";
    const auto root_of = [](std::string_view text) {
        // <Root ...> through the first '>' or whitespace, capped.
        if (text.empty() || text.front() != '<') return std::string("no-root");
        const auto end = text.find_first_of("> \t\r\n", 1);
        if (end == std::string_view::npos || end > 64) return std::string("no-root");
        return std::string(text.substr(1, end - 1));
    };
    for (std::size_t i = 0; i < general[keys].size() && i < general[maps].size(); ++i) {
        if (!general[keys][i].is_string() || !general[maps][i].is_string()) {
            out += "entry=" + std::to_string(i) + " key=non_string\n";
            continue;
        }
        const auto& key = general[keys][i].get_ref<const std::string&>();
        const auto& map = general[maps][i].get_ref<const std::string&>();
        out += "entry=" + std::to_string(i) +
            " key_bytes=" + std::to_string(key.size()) +
            " map_bytes=" + std::to_string(map.size()) +
            " root=" + root_of(map) +
            " keyboard=" + (IsKeyboardNativeMap(key, map) ? "1" : "0") +
            " controller=" + (IsControllerNativeMap(key, map) ? "1" : "0") + "\n";
    }
    return out;
}

inline NativeConfigInspection InspectNativeConfig(std::string_view value) {
    NativeConfigInspection result;
    const auto decoded=DecodeNativeConfigUri(value);
    if(!decoded) return result;
    auto envelope=ParseSdkJson(*decoded);
    if(!envelope.is_object() || !envelope.contains("GENERAL_DATA") ||
        !envelope["GENERAL_DATA"].is_string()) return result;
    auto general=ParseSdkJson(envelope["GENERAL_DATA"].get_ref<const std::string&>());
    if(!general.is_object()) return result;
    result.valid=true;
    constexpr auto keys="_overrideControllerMapKeyList",maps="_overrideControllerMapValueList";
    if(general.contains(keys) && general[keys].is_array() &&
        general.contains(maps) && general[maps].is_array() &&
        general[keys].size()==general[maps].size()) {
        result.map_count=general[keys].size();
        for(std::size_t i=0;i<general[keys].size();++i) {
            if(!general[keys][i].is_string() || !general[maps][i].is_string()) continue;
            const auto& key=general[keys][i].get_ref<const std::string&>();
            const auto& map=general[maps][i].get_ref<const std::string&>();
            if(IsKeyboardNativeMap(key,map)) ++result.keyboard_map_count;
            else if(!ShouldKeepNativeMap(key,map)) ++result.unknown_map_count;
        }
    }
    if(general.contains("enableInputHotSwap") && general["enableInputHotSwap"].is_boolean())
        result.hot_swap=general["enableInputHotSwap"].get<bool>();
    if(general.contains("rewiredEnableKeyboard") && general["rewiredEnableKeyboard"].is_boolean())
        result.keyboard_enabled=general["rewiredEnableKeyboard"].get<bool>();
    if(general.contains("rewiredDisableKeyboard") && general["rewiredDisableKeyboard"].is_boolean())
        result.keyboard_disabled=general["rewiredDisableKeyboard"].get<bool>();
    for(auto it=envelope.begin();it!=envelope.end();++it) {
        if(!it.key().starts_with("USD_") || it.key()=="USD_0") continue;
        const auto user=it.value().is_string() ? ParseSdkJson(it.value().get_ref<const std::string&>()) : it.value();
        if(user.is_object() && user.contains("isUsingJoypad") && user["isUsingJoypad"].is_boolean() &&
           user["isUsingJoypad"].get<bool>()) ++result.joypad_user_count;
    }
    return result;
}

inline constexpr std::size_t kMaximumNativeMapEntries = 64;

// No default argument: every caller must state the policy explicitly. The
// product default is Keybinds; Exact remains opt-in pending gameplay validation.
inline NativeControllerRestore PrepareNativeControllerRestore(
    std::string_view value,
    NativeConfigRestoreMode mode) {
    NativeControllerRestore result;
    if(mode==NativeConfigRestoreMode::Off) {result.reason=NativeBindReason::NoControllerMap;return result;}
    const auto decoded=DecodeNativeConfigUri(value);
    if(!decoded) {result.reason=NativeBindReason::DecodeFailed;return result;}
    auto envelope=ParseSdkJson(*decoded);
    if(!envelope.is_object() || !envelope.contains("GENERAL_DATA") ||
        !envelope["GENERAL_DATA"].is_string()) {
        result.reason=NativeBindReason::EnvelopeInvalid;
        return result;
    }
    auto general=ParseSdkJson(envelope["GENERAL_DATA"].get_ref<const std::string&>());
    if(mode==NativeConfigRestoreMode::Exact) {
        if(!general.is_object()) {result.reason=NativeBindReason::EnvelopeInvalid;return result;}
        const auto info=InspectNativeConfig(value);
        result.map_count=info.map_count;
        result.keyboard_map_count=info.keyboard_map_count;
        result.unknown_map_count=info.unknown_map_count;
        if(general.contains("enableInputHotSwap") && general["enableInputHotSwap"].is_boolean())
            result.hot_swap=general["enableInputHotSwap"].get<bool>();
        // Preserve the exact URI-encoded string. Re-encoding a parsed subset
        // loses device selection, GUIDs and future game-owned settings. Unlike
        // legacy Full, an already-enabled/missing hot-swap flag is also valid.
        result.runtime_value=std::string(value);
        return result;
    }
    constexpr auto keys="_overrideControllerMapKeyList",maps="_overrideControllerMapValueList";
    if(!general.is_object() || !general.contains(keys) || !general.contains(maps) ||
        !general[keys].is_array() || !general[maps].is_array()) {
        result.reason=NativeBindReason::ListsMissing;
        return result;
    }
    if(general[keys].size()!=general[maps].size()) {
        result.reason=NativeBindReason::ListsMismatched;
        return result;
    }
    if(general.contains("enableInputHotSwap") && general["enableInputHotSwap"].is_boolean())
        result.hot_swap=general["enableInputHotSwap"].get<bool>();

    // Select controller maps first, then enforce the cap. A legacy save with
    // more total entries than the cap may still contain usable gamepad maps.
    nlohmann::json kept_keys=nlohmann::json::array();
    nlohmann::json kept_maps=nlohmann::json::array();
    for(std::size_t i=0;i<general[keys].size();++i) {
        if(!general[keys][i].is_string() || !general[maps][i].is_string()) {
            ++result.dropped_empty_count;
            continue;
        }
        const auto& key=general[keys][i].get_ref<const std::string&>();
        const auto& map=general[maps][i].get_ref<const std::string&>();
        if(key.empty() || map.empty()) {++result.dropped_empty_count;continue;}
        if(IsKeyboardNativeMap(key,map)) {++result.keyboard_map_count;continue;}
        // Keep every non-keyboard/mouse/touch map that looks like a serialised
        // override. An unrecognised root name is counted, not silently dropped:
        // real saves may use custom-controller roots we have not seen yet.
        if(!ShouldKeepNativeMap(key,map)) {++result.unknown_map_count;continue;}
        if(!IsControllerNativeMap(key,map)) ++result.unknown_map_count;
        if(kept_keys.size()==kMaximumNativeMapEntries) {result.reason=NativeBindReason::TooManyEntries;break;}
        kept_keys.push_back(general[keys][i]);
        if(mode==NativeConfigRestoreMode::Keybinds) {
            const auto xml=PrepareNativeMapXml(map);
            if(xml.state==NativeMapXmlState::Invalid) {
                ++result.invalid_xml_count;
                result.reason=NativeBindReason::MalformedControllerXml;
                return result; // Keep the vault; don't serve a partly broken map set.
            }
            if(xml.state==NativeMapXmlState::Repaired) {
                ++result.repaired_xml_count;
                kept_maps.push_back(xml.repaired);
            } else kept_maps.push_back(general[maps][i]);
        } else kept_maps.push_back(general[maps][i]);
    }
    if(kept_keys.empty()) {
        if(result.reason==NativeBindReason::Okay)
            result.reason=NativeBindReason::NoControllerMap;
        result.map_count=0;
        return result;
    }
    result.map_count=kept_keys.size();
    if(result.reason==NativeBindReason::TooManyEntries) return result;
    if(mode==NativeConfigRestoreMode::Full) {
        // Legacy A/B path, kept only to reproduce the 0.2.116/0.2.117 failure.
        // It returns the whole saved config and still forces hot-swap on.
        if(!result.hot_swap.has_value()) return result;
        if(*result.hot_swap) return result;
        general["enableInputHotSwap"]=true;
    } else {
        // Keybinds: the two map arrays plus only the two fields that carry the
        // in-game input-mode choice. `inputData` and the Rewired/vibration
        // fields are deliberately NOT returned: they describe the local device
        // environment (script version, sensitivities) and returning a stale copy
        // made the pod stop accepting input entirely in 0.2.121 testing.
        nlohmann::json kept=nlohmann::json::object();
        kept[keys]=std::move(kept_keys);
        kept[maps]=std::move(kept_maps);
        for(const char* field:{"enableInputHotSwap","disableFallbackControllerType"}) {
            if(general.contains(field)) kept[field]=general[field];
        }
        general=std::move(kept);
    }
    envelope["GENERAL_DATA"]=general.dump();
    const auto plain=envelope.dump();
    if(!SdkJsonWithinBudget(plain)) {result.reason=NativeBindReason::OverBudget;return result;}
    result.runtime_value=EncodeNativeConfigUri(plain);
    if(!result.runtime_value) {result.reason=NativeBindReason::EncodeFailed;return result;}
    result.reason=NativeBindReason::Okay;
    return result;
}
} // namespace cloudgenshin::sdk
