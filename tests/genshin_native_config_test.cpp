#include "mihoyo/genshin_native_config.hpp"
#include "mihoyo/genshin_native_config_policy.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

using Json=nlohmann::json;
using namespace cloudgenshin::sdk;
namespace {
std::string Encoded(const Json& general) {
    const auto value=EncodeNativeConfigUri(Json{{"GENERAL_DATA",general.dump()},{"other","keep + % 中文"}}.dump());
    assert(value);return *value;
}
Json General(const std::string& value) {
    const auto decoded=DecodeNativeConfigUri(value);assert(decoded);
    return Json::parse(Json::parse(*decoded).at("GENERAL_DATA").get<std::string>());
}
}
int main(int argc,char** argv) {
    // Optional local verification against a game-only session-start snapshot.
    // No values/keys/identifiers are printed or persisted as test fixtures.
    if(argc==2) {
        std::ifstream file(argv[1],std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)),{});
        const auto snapshot=ParseSdkJson(bytes);
        assert(snapshot.is_object() && snapshot.contains("data"));
        unsigned configs=0;
        for(auto it=snapshot["data"].begin();it!=snapshot["data"].end();++it) {
            if(!it.key().starts_with("cloud_local_data_") || !it.value().is_string()) continue;
            const auto result=PrepareNativeControllerRestore(it.value().get<std::string>(),
                cloudgenshin::NativeConfigRestoreMode::Keybinds);
            assert(result.runtime_value);
            const auto restored=General(*result.runtime_value);
            for(const auto& map:restored["_overrideControllerMapValueList"])
                assert(PrepareNativeMapXml(map.get<std::string>()).state==NativeMapXmlState::Valid);
            std::cout<<"PASS local native snapshot maps="<<result.map_count
                <<" repaired_xml="<<result.repaired_xml_count<<" invalid_xml="<<result.invalid_xml_count<<'\n';
            ++configs;
        }
        assert(configs>0);
        return 0;
    }
    const Json maps=Json::array({"<CJoystickMap>\n<action id=\"19\">+</action></CJoystickMap>",
        std::string("<FJoystickMap>")+std::string(65000,'x')+"</FJoystickMap>"});
    Json general{{"_overrideControllerMapKeyList",Json::array({"stable-xbox-c","stable-xbox-f"})},
        {"_overrideControllerMapValueList",maps},{"enableInputHotSwap",false},
        {"inputData",R"({"joypadSenseIndex":4,"joypadInvertCameraY":true})"},
        {"unknown",Json{{"large",std::uint64_t{18446744073709551615ULL}},{"float",0.8999999761581421},
            {"unicode","中文 + literal %"},{"nestedFlag",Json{{"enableInputHotSwap",false}}}}}};
    const auto original=Encoded(general);

    // Real-world regression shape: old URI rewriting protected form '+' in
    // XML markup. A map looking like XML was not necessarily parseable XML.
    const std::string broken=R"(<?xml+version="1.0"+encoding="utf-16"?><JoystickMap+dataVersion="2"+hardwareName="XInput+Gamepad"><categoryId>0</categoryId><layoutId>0</layoutId><buttonMaps><ActionElementMap><actionId>43</actionId><elementIdentifierId>16</elementIdentifierId></ActionElementMap></buttonMaps></JoystickMap>)";
    const Json broken_general{{"_overrideControllerMapKeyList",Json::array({"same-key"})},
        {"_overrideControllerMapValueList",Json::array({broken})},{"enableInputHotSwap",false}};
    const auto broken_encoded=Encoded(broken_general);
    const auto fixed=PrepareNativeControllerRestore(broken_encoded,cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(fixed.runtime_value && fixed.repaired_xml_count==1 && fixed.invalid_xml_count==0);
    const auto fixed_general=General(*fixed.runtime_value);
    assert(fixed_general["_overrideControllerMapKeyList"]==broken_general["_overrideControllerMapKeyList"]);
    assert(fixed_general["enableInputHotSwap"]==false);
    assert(PrepareNativeMapXml(fixed_general["_overrideControllerMapValueList"][0].get<std::string>()).state==NativeMapXmlState::Valid);
    assert(General(broken_encoded)==broken_general); // The persisted original is untouched.
    assert(*PrepareNativeControllerRestore(broken_encoded,cloudgenshin::NativeConfigRestoreMode::Exact).runtime_value==broken_encoded);
    auto malformed=broken_general;
    malformed["_overrideControllerMapValueList"][0]="<JoystickMap><broken></JoystickMap>";
    const auto rejected=PrepareNativeControllerRestore(Encoded(malformed),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(!rejected.runtime_value && rejected.invalid_xml_count==1 && rejected.reason==NativeBindReason::MalformedControllerXml);

    // Exact follows Android's opaque file read-back contract. It preserves all
    // game-owned fields, string formatting and URI escapes, not a parsed subset.
    const auto exact=PrepareNativeControllerRestore(original,cloudgenshin::NativeConfigRestoreMode::Exact);
    assert(exact.runtime_value && *exact.runtime_value==original);
    assert(exact.map_count==2 && exact.hot_swap==false);
    assert(General(*exact.runtime_value)==general);
    for(const Json& extra:{Json{{"enableInputHotSwap",true}}, Json::object(),
                         Json{{"inputData","future-device-data"}}}) {
        const auto sparse=Encoded(extra);
        const auto restored=PrepareNativeControllerRestore(sparse,cloudgenshin::NativeConfigRestoreMode::Exact);
        assert(restored.runtime_value && *restored.runtime_value==sparse);
    }
    // Future fields, keyboard entries and input-device choices must not be
    // silently stripped. Game data is not a launcher remapping profile.
    const auto opaque=*EncodeNativeConfigUri(R"({ "GENERAL_DATA" : "{\"future\":7,\"enableInputHotSwap\":false}", "USD_test" : "  saved  " })");
    const auto opaque_restored=PrepareNativeControllerRestore(opaque,cloudgenshin::NativeConfigRestoreMode::Exact);
    assert(opaque_restored.runtime_value && *opaque_restored.runtime_value==opaque);
    for(const auto* invalid:{"%GG", "%7B%7D", "%7B%22GENERAL_DATA%22%3A123%7D"})
        assert(!PrepareNativeControllerRestore(invalid,cloudgenshin::NativeConfigRestoreMode::Exact).runtime_value);

    // Full is the legacy A/B path only. It still returns the whole config and
    // forces hot-swap on, which is exactly the 0.2.116/0.2.117 failure.
    const auto full=PrepareNativeControllerRestore(original,cloudgenshin::NativeConfigRestoreMode::Full);
    assert(full.map_count==2 && full.hot_swap==false && full.runtime_value);
    auto full_actual=General(*full.runtime_value);
    assert(full_actual["enableInputHotSwap"]==true);
    full_actual["enableInputHotSwap"]=false;
    assert(full_actual==general); // every map string and unrelated setting survives
    assert(General(original)==general); // stored original never changes
    const auto full_envelope=Json::parse(*DecodeNativeConfigUri(*full.runtime_value));
    assert(full_envelope["other"]=="keep + % 中文");
    // Full refuses an already-enabled or missing/mistyped hot-swap field.
    Json no_hot=general;no_hot.erase("enableInputHotSwap");
    assert(!PrepareNativeControllerRestore(Encoded(no_hot),
        cloudgenshin::NativeConfigRestoreMode::Full).runtime_value);
    Json text_hot=general;text_hot["enableInputHotSwap"]="false";
    assert(!PrepareNativeControllerRestore(Encoded(text_hot),
        cloudgenshin::NativeConfigRestoreMode::Full).runtime_value);

    // 0.2.120 M0 keybind mode: reply contains ONLY the positively identified
    // controller maps. No switch is invented, no unrelated field is echoed.
    Json keybind_general{
        {"_overrideControllerMapKeyList",Json::array({
            "stable-xbox-c","stable-keyboard-c","stable-ps5-f","stable-mouse-m","stable-custom-z",
            "stable-xbox-empty"})},
        {"_overrideControllerMapValueList",Json::array({
            "<CJoystickMap>controller-c</CJoystickMap>",
            "<KeyboardMap>keyboard-c</KeyboardMap>",
            "<FJoystickMap>controller-f</FJoystickMap>",
            "<MouseMap>mouse-m</MouseMap>",
            "<GuitarMap>custom-z</GuitarMap>",
            ""})},
        {"enableInputHotSwap",true},
        {"rewiredEnableKeyboard",true},
        {"rewiredDisableKeyboard",false},
        {"inputData",R"({"joypadSenseIndex":4,"joypadInvertCameraY":true})"},
        {"extraSetting","must not leak into M0"},
        {"unknown",Json{{"large",std::uint64_t{18446744073709551615ULL}},
            {"nestedFlag",Json{{"enableInputHotSwap",true}}}}}};
    const auto keybind_original=Encoded(keybind_general);
    // An unrecognised root name must still be restored (real saves may use
    // custom-controller roots), but it is counted for diagnostics.
    const auto keybind_custom=PrepareNativeControllerRestore(
        keybind_original,cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(keybind_custom.map_count==3);
    assert(keybind_custom.unknown_map_count==1);    // GuitarMap root is unrecognised
    assert(keybind_custom.keyboard_map_count==2);
    assert(keybind_custom.reason==NativeBindReason::Okay);
    assert(keybind_custom.runtime_value);
    // With the unknown entry removed, the same save restores normally.
    auto keybind_known=keybind_general;
    keybind_known["_overrideControllerMapKeyList"]=Json::array({
        "stable-xbox-c","stable-keyboard-c","stable-ps5-f","stable-mouse-m","stable-xbox-empty"});
    keybind_known["_overrideControllerMapValueList"]=Json::array({
        "<CJoystickMap>controller-c</CJoystickMap>",
        "<KeyboardMap>keyboard-c</KeyboardMap>",
        "<FJoystickMap>controller-f</FJoystickMap>",
        "<MouseMap>mouse-m</MouseMap>",
        ""});
    const auto keybind_original_known=Encoded(keybind_known);
    const auto keybind=PrepareNativeControllerRestore(
        keybind_original_known,cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(keybind.map_count==2);
    assert(keybind.keyboard_map_count==2);   // KeyboardMap + MouseMap
    assert(keybind.unknown_map_count==0);
    assert(keybind.dropped_empty_count==1);  // empty value, never a total failure
    assert(keybind.reason==NativeBindReason::Okay);
    assert(keybind.runtime_value);
    auto keybind_actual=General(*keybind.runtime_value);
    // Keybinds: the two map arrays plus input-mode fields passed through exactly
    // as the game saved them. No switch is forced and no keyboard map leaks.
    // Only the two map arrays plus the input-mode switch. `inputData` and the
    // Rewired/vibration fields are not echoed: returning a stale local device
    // description stopped the pod accepting input in 0.2.121 testing.
    assert(keybind_actual.size()==3);
    assert(keybind_actual.contains("_overrideControllerMapKeyList"));
    assert(keybind_actual.contains("_overrideControllerMapValueList"));
    assert(keybind_actual.contains("enableInputHotSwap"));
    assert(!keybind_actual.contains("inputData"));
    assert(!keybind_actual.contains("rewiredEnableKeyboard"));
    assert(!keybind_actual.contains("rewiredDisableKeyboard"));
    assert(keybind_actual["_overrideControllerMapKeyList"]==
        Json::array({"stable-xbox-c","stable-ps5-f"}));
    assert(keybind_actual["_overrideControllerMapValueList"]==Json::array({
        "<CJoystickMap>controller-c</CJoystickMap>",
        "<FJoystickMap>controller-f</FJoystickMap>"}));
    // Passed through unchanged, never rewritten.
    assert(keybind_actual["enableInputHotSwap"]==true);
    // Unrelated settings are not echoed.
    assert(!keybind_actual.contains("extraSetting"));
    assert(!keybind_actual.contains("unknown"));
    assert(keybind_actual.dump().find("KeyboardMap")==std::string::npos);
    assert(keybind_actual.dump().find("MouseMap")==std::string::npos);
    assert(General(keybind_original_known)==keybind_known); // stored original unchanged
    // Idempotence: the game writes back what it received; re-filtering it must
    // produce the same payload, not drift or empty.
    const auto refilter=PrepareNativeControllerRestore(
        *keybind.runtime_value,cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(refilter.runtime_value && *refilter.runtime_value==*keybind.runtime_value);
    assert(refilter.map_count==2 && refilter.keyboard_map_count==0);

    const auto inspection=InspectNativeConfig(keybind_original_known);
    assert(inspection.valid && inspection.map_count==5 && inspection.keyboard_map_count==2);
    assert(inspection.unknown_map_count==1); // the empty entry counts as unusable
    assert(inspection.hot_swap && inspection.keyboard_enabled && !inspection.keyboard_disabled);
    // Adoption evidence compares only controller maps, never payload bytes.
    assert(CompareNativeConfigMaps(keybind_original_known,*keybind.runtime_value));
    assert(!CompareNativeConfigMaps(Encoded(general),*keybind.runtime_value));

    // All-keyboard config: nothing is returned at all.
    Json keyboard_only=keybind_general;
    keyboard_only["_overrideControllerMapKeyList"]=Json::array({"stable-keyboard-c"});
    keyboard_only["_overrideControllerMapValueList"]=Json::array({"<KeyboardMap>only</KeyboardMap>"});
    const auto no_controller=PrepareNativeControllerRestore(
        Encoded(keyboard_only),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(no_controller.map_count==0 && no_controller.keyboard_map_count==1 &&
        !no_controller.runtime_value && no_controller.reason==NativeBindReason::NoControllerMap);
    // Unknown-but-plausible map format is kept and counted: real saves may use
    // controller roots we have not seen yet, and dropping them loses the keys.
    Json unknown_only=keybind_general;
    unknown_only["_overrideControllerMapKeyList"]=Json::array({"stable-unknown"});
    unknown_only["_overrideControllerMapValueList"]=Json::array({"<SomethingMap>x</SomethingMap>"});
    const auto unknown_result=PrepareNativeControllerRestore(
        Encoded(unknown_only),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(unknown_result.runtime_value && unknown_result.map_count==1);
    assert(unknown_result.unknown_map_count==1);
    assert(unknown_result.reason==NativeBindReason::Okay);
    // A value that is not a serialised map at all is dropped, not guessed.
    Json not_a_map=keybind_general;
    not_a_map["_overrideControllerMapKeyList"]=Json::array({"stable-plain"});
    not_a_map["_overrideControllerMapValueList"]=Json::array({"plain text, not a map"});
    const auto not_a_map_result=PrepareNativeControllerRestore(
        Encoded(not_a_map),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(!not_a_map_result.runtime_value && not_a_map_result.unknown_map_count==1);
    assert(not_a_map_result.reason==NativeBindReason::NoControllerMap);
    // Missing / mismatched lists are not guessed.
    Json missing=keybind_general;missing.erase("_overrideControllerMapKeyList");
    const auto missing_result=PrepareNativeControllerRestore(
        Encoded(missing),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(!missing_result.runtime_value && missing_result.reason==NativeBindReason::ListsMissing);
    Json mismatched=keybind_general;
    mismatched["_overrideControllerMapValueList"]=Json::array({"<CJoystickMap>only</CJoystickMap>"});
    const auto mismatched_result=PrepareNativeControllerRestore(
        Encoded(mismatched),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(!mismatched_result.runtime_value &&
        mismatched_result.reason==NativeBindReason::ListsMismatched);
    // Empty lists: nothing to restore.
    Json empty=keybind_general;
    empty["_overrideControllerMapKeyList"]=Json::array();
    empty["_overrideControllerMapValueList"]=Json::array();
    assert(!PrepareNativeControllerRestore(Encoded(empty),
        cloudgenshin::NativeConfigRestoreMode::Keybinds).runtime_value);
    // Over the cap after filtering: refuse rather than send a partial layout.
    Json many=keybind_general;
    Json many_keys=Json::array(),many_maps=Json::array();
    for(std::size_t i=0;i<kMaximumNativeMapEntries+1;++i) {
        many_keys.push_back("stable-xbox-"+std::to_string(i));
        many_maps.push_back("<CJoystickMap>m"+std::to_string(i)+"</CJoystickMap>");
    }
    many["_overrideControllerMapKeyList"]=many_keys;
    many["_overrideControllerMapValueList"]=many_maps;
    const auto many_result=PrepareNativeControllerRestore(
        Encoded(many),cloudgenshin::NativeConfigRestoreMode::Keybinds);
    assert(!many_result.runtime_value && many_result.reason==NativeBindReason::TooManyEntries);
    // Off mode never builds a reply.
    const auto off=PrepareNativeControllerRestore(
        keybind_original,cloudgenshin::NativeConfigRestoreMode::Off);
    assert(!off.runtime_value && off.reason==NativeBindReason::NoControllerMap);

    // Structure report: names/types/counts/roots only, never map contents.
    const auto report=BuildNativeConfigStructureReport(keybind_original_known);
    assert(report.find("envelope_field=GENERAL_DATA")!=std::string::npos);
    assert(report.find("map_entries=5")!=std::string::npos);
    assert(report.find("root=CJoystickMap")!=std::string::npos);
    assert(report.find("root=KeyboardMap")!=std::string::npos);
    assert(report.find("keyboard=1")!=std::string::npos);
    assert(report.find("controller-c")==std::string::npos);
    assert(report.find("keyboard-c")==std::string::npos);
    assert(report.find("must not leak")==std::string::npos);
    assert(BuildNativeConfigStructureReport("%GG").find("decode=failed")!=std::string::npos);
    assert(BuildNativeConfigStructureReport("garbage").find("envelope=not_object")!=std::string::npos);

    // Malformed input has an explicit reason, never an unsanitized fallback.
    for(const auto* invalid:{"", "%", "%0", "%GG", "%FF", "hello", "%7B%7D", "%7B%22GENERAL_DATA%22%3A123%7D"}) {
        const auto bad=PrepareNativeControllerRestore(invalid,
            cloudgenshin::NativeConfigRestoreMode::Keybinds);
        assert(!bad.runtime_value && bad.reason!=NativeBindReason::Okay);
    }
    assert(*DecodeNativeConfigUri("a+b%20c%2b%25")=="a+b c+%");
    assert(*DecodeNativeConfigUri(*EncodeNativeConfigUri("\"!~*'()中文\n"))=="\"!~*'()中文\n");
    assert(!DecodeNativeConfigUri(std::string(kSdkJsonMaximumBytes+1,'x')));
    assert(!EncodeNativeConfigUri(std::string(kSdkJsonMaximumBytes,' ')));
    assert(!PrepareNativeControllerRestore(std::string(kSdkJsonMaximumBytes+1,'x'),
        cloudgenshin::NativeConfigRestoreMode::Keybinds).runtime_value);

    const auto toggle=std::filesystem::temp_directory_path()/
        ("cg-native-config-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".txt");
    // Missing marker = Keybinds: return the saved maps plus the game's own
    // input-mode values. An empty marker is an explicit opt-out to save-only.
    using cloudgenshin::NativeConfigRestoreMode;
    using cloudgenshin::NativeConfigRestoreModeFor;
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Keybinds);
    assert(!cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    {std::ofstream out(toggle,std::ios::binary);out<<"1\n";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Off);
    assert(cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Off);
    assert(!cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_CONFIG_RESTORE_OFF_V1\n";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Off);
    assert(!cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    for(const auto* ending:{"", "\n", "\r\n"}) {
        {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_CONFIG_EXACT_V1"<<ending;}
        assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Exact);
        assert(!cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    }
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_CONFIG_RESTORE_V1\n";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Full);
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_CONFIG_RESTORE_V1\r\n";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Full);
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_CONFIG_RESTORE_V1";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Full);
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_KEYBINDS_RESTORE_V1\n";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Keybinds);
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_KEYBINDS_RESTORE_V1";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Keybinds);
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"CGNX_NATIVE_KEYBINDS_RESTORE_V1\n";}
    {std::ofstream out(toggle,std::ios::binary|std::ios::app);out<<"x";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Off);
    assert(cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    {std::ofstream out(toggle,std::ios::binary|std::ios::trunc);out<<"\xEF\xBB\xBF" "CGNX_NATIVE_KEYBINDS_RESTORE_V1\n";}
    assert(NativeConfigRestoreModeFor(toggle.string())==NativeConfigRestoreMode::Off);
    assert(cloudgenshin::NativeConfigMarkerUnknown(toggle.string()));
    std::filesystem::remove(toggle);
    std::cout<<"PASS native config M0 keybind payload, keyboard/mouse exclusion, unknown-root keep, reasons, idempotence and Off default\n";
}
