#include "mihoyo/genshin_controller_lock.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
using namespace cloudgenshin::sdk;
using Json=nlohmann::json;
namespace {
constexpr auto key="cloud_local_data_123";
Json Data(bool controller,int action,int progress=1) {
    const auto map="<JoystickMap><actionId>"+std::to_string(action)+"</actionId></JoystickMap>";
    Json general{{"_overrideControllerMapKeyList",Json::array({"stable-xbox"})},
        {"_overrideControllerMapValueList",Json::array({map})},{"enableInputHotSwap",false},
        {"newGraphicsSetting",progress}};
    Json user{{"isUsingJoypad",controller},{"checkedDefaultUsingJoypad",controller},
        {"joypadGUID",controller?"test-controller-guid":""},{"progress",progress},{"future",Json{{"flag",true}}}};
    const auto value=EncodeNativeConfigUri(Json{{"GENERAL_DATA",general.dump()},{"USD_0",R"({"isUsingJoypad":false})"},
        {"USD_456",user.dump()},{"other","fresh-"+std::to_string(progress)}}.dump());assert(value);
    return Json{{key,*value}};
}
Json Envelope(const Json& data) {return Json::parse(*DecodeNativeConfigUri(data.at(key).get<std::string>()));}
Json General(const Json& data) {return Json::parse(Envelope(data).at("GENERAL_DATA").get<std::string>());}
Json User(const Json& data) {return Json::parse(Envelope(data).at("USD_456").get<std::string>());}
Json Encoded(Json envelope) {return Json{{key,*EncodeNativeConfigUri(envelope.dump())}};}
}
int main(int argc,char** argv) {
    // Optional local, read-only verification against game-only snapshots.
    // Output is counts only: no account keys, GUIDs or map contents.
    if(argc==2) {
        std::ifstream file(argv[1],std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file),{}};
        assert(!bytes.empty() && bytes.size()<=300*1024);
        const auto snapshot=ParseSdkJson(bytes);assert(snapshot.is_object() && snapshot.contains("data"));
        NativeControllerLock actual(snapshot["data"]);assert(actual.locked()>0);
        const auto fixed=actual.normalize(snapshot["data"],snapshot["data"]);
        unsigned users=0;
        for(auto it=fixed.begin();it!=fixed.end();++it)
            if(it.value().is_string()) users+=InspectNativeConfig(it.value().get_ref<const std::string&>()).joypad_user_count;
        assert(users>0);
        std::cout<<"PASS real game-only snapshot fixed_profiles="<<actual.locked()<<" joypad_user_count="<<users<<'\n';
        return 0;
    }
    const auto good=Data(true,43),touch=Data(false,99,8);
    NativeControllerLock locked(good);assert(locked.locked()==1);
    const auto fixed=locked.normalize(good,touch);
    assert(User(fixed)["isUsingJoypad"]==true && User(fixed)["checkedDefaultUsingJoypad"]==true);
    assert(User(fixed)["joypadGUID"]=="test-controller-guid");
    assert(General(fixed)["_overrideControllerMapValueList"]==General(good)["_overrideControllerMapValueList"]);
    assert(General(fixed)["newGraphicsSetting"]==8 && User(fixed)["progress"]==8);
    assert(User(fixed)["future"]==User(touch)["future"] && Envelope(fixed)["other"]=="fresh-8");
    assert(Envelope(fixed)["USD_0"]==Envelope(touch)["USD_0"]);
    assert(locked.normalize(fixed,fixed)==fixed); // No drift or repeated rewrite.
    // Later valid controller writes must not silently change the fixed buttons.
    const auto changed=locked.normalize(fixed,Data(true,88,9));
    assert(General(changed)["_overrideControllerMapValueList"]==General(good)["_overrideControllerMapValueList"]);
    assert(User(changed)["progress"]==9);
    // Restart reconstructs the same fixed profile from the encrypted-store data.
    NativeControllerLock restart(changed);assert(restart.normalize(changed,touch)==fixed);
    // Never learn a default/touch profile; learn exactly once from a later
    // explicit controller save, and preserve that profile thereafter.
    NativeControllerLock fresh(touch);assert(!fresh.locked());
    assert(fresh.normalize(touch,touch)==touch && !fresh.locked());
    const auto first=fresh.normalize(touch,good);assert(fresh.locked()==1);
    assert(General(fresh.normalize(first,touch))["_overrideControllerMapValueList"]==General(good)["_overrideControllerMapValueList"]);
    // Recover only matching game keys, only from a controller-confirmed seed.
    NativeControllerLock recovered(touch,{Data(false,21),good});
    assert(recovered.locked()==1 && recovered.recovered()==1);
    assert(User(recovered.normalize(touch,touch))["isUsingJoypad"]==true);
    auto other=good;other["cloud_local_data_999"]=other[key];other.erase(key);
    NativeControllerLock isolated(touch,{other});assert(!isolated.locked());
    // Do not trust truthy strings, missing flags, empty GUIDs or UID 0.
    for(const auto field:{"isUsingJoypad","checkedDefaultUsingJoypad","joypadGUID"}) {
        auto envelope=Envelope(good);auto user=User(good);user.erase(field);envelope["USD_456"]=user.dump();
        NativeControllerLock invalid(Encoded(envelope));assert(!invalid.locked());
    }
    {
        auto envelope=Envelope(good);auto user=User(good);user["isUsingJoypad"]="true";envelope["USD_456"]=user.dump();
        NativeControllerLock invalid(Encoded(envelope));assert(!invalid.locked());
        envelope=Envelope(good);envelope.erase("USD_456");
        NativeControllerLock default_user(Encoded(envelope));assert(!default_user.locked());
        // Already matching data is returned byte-for-byte, avoiding full
        // re-encoding on every ordinary save or fresh-progress write.
        assert(locked.normalize(good,good)==good);
    }
    auto missing=Envelope(touch);missing.erase("USD_456");
    const auto merged=locked.normalize(changed,Encoded(missing));
    assert(User(merged)["progress"]==9); // Missing subtree uses latest, not first-session data.
    assert(User(merged)["isUsingJoypad"]==true);
    assert(locked.normalize(good,Json{{key,"%GG"}})==good && locked.rejected()==1);
    assert(locked.normalize(good,Json::object()).empty());
    // Unrelated SDK keys never become controller presets or get rewritten.
    assert(locked.normalize(good,Json{{"other","value"}})==Json({{"other","value"}}));
    std::cout<<"PASS fixed native controls: no touch overwrite/drift, fresh data preserved, recovery scoped, restart, malformed saves\n";
}
