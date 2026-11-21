#pragma once
#include "mihoyo/genshin_native_config.hpp"
#include <map>
#include <vector>

namespace cloudgenshin::sdk {
// Lock the game's own confirmed controller configuration, NOT a local input
// remap and NOT an entire USD/player-data snapshot. Only maps, two known input
// switches and the three observed controller-selection fields are retained.
class NativeControllerLock {
    using Json=nlohmann::json;
    struct Profile {Json general=Json::object(),users=Json::object();};
    std::map<std::string,Profile> profiles_;
    std::size_t profile_bytes_=0;
    unsigned recovered_=0,applied_=0,rejected_=0;
    static bool GameKey(std::string_view key) {
        constexpr std::string_view prefix="cloud_local_data_";
        return key.starts_with(prefix) && Digits(key.substr(prefix.size()));
    }
    static bool Digits(std::string_view value) {
        return !value.empty() && value.size()<=32 &&
            std::all_of(value.begin(),value.end(),[](char c){return c>='0' && c<='9';});
    }
    static bool UserKey(std::string_view key) {
        return key.starts_with("USD_") && key!="USD_0" && Digits(key.substr(4));
    }
    static Json Object(const Json& value) {
        return value.is_string() ? ParseSdkJson(value.get_ref<const std::string&>()) : value;
    }
    static std::optional<Json> Envelope(const Json& value) {
        if(!value.is_string()) return {};
        const auto plain=DecodeNativeConfigUri(value.get_ref<const std::string&>());
        if(!plain) return {};
        auto envelope=ParseSdkJson(*plain);
        if(!envelope.is_object() || !envelope.contains("GENERAL_DATA") ||
           !envelope["GENERAL_DATA"].is_string()) return {};
        return envelope;
    }
    static std::optional<Profile> Extract(const Json& value) {
        if(!value.is_string()) return {};
        const auto prepared=PrepareNativeControllerRestore(value.get_ref<const std::string&>(),
            NativeConfigRestoreMode::Keybinds);
        if(!prepared.runtime_value || !prepared.map_count) return {};
        const auto envelope=Envelope(Json(*prepared.runtime_value));
        if(!envelope) return {};
        Profile result;result.general=Object((*envelope)["GENERAL_DATA"]);
        if(!result.general.is_object()) return {};
        for(auto it=envelope->begin();it!=envelope->end();++it) {
            if(!UserKey(it.key())) continue;
            const auto user=Object(it.value());
            // Never turn a touch/keyboard/default save into the fixed preset.
            // The game must have actually saved all three selection fields.
            if(!user.is_object() || !user.contains("isUsingJoypad") ||
               !user["isUsingJoypad"].is_boolean() || !user["isUsingJoypad"].get<bool>() ||
               !user.contains("checkedDefaultUsingJoypad") ||
               !user["checkedDefaultUsingJoypad"].is_boolean() || !user["checkedDefaultUsingJoypad"].get<bool>() ||
               !user.contains("joypadGUID") || !user["joypadGUID"].is_string()) continue;
            const auto& guid=user["joypadGUID"].get_ref<const std::string&>();
            if(guid.empty() || guid.size()>128) continue;
            result.users[it.key()]=Json{{"isUsingJoypad",true},{"checkedDefaultUsingJoypad",true},{"joypadGUID",guid}};
        }
        if(result.users.empty()) return {};
        return result;
    }
    void capture(const Json& data,bool recovery,const Json* scope=nullptr) {
        if(!data.is_object() || data.size()>32) return;
        for(auto it=data.begin();it!=data.end();++it) {
            if(profiles_.size()>=32) break;
            if(!GameKey(it.key()) || profiles_.contains(it.key()) ||
               (scope && !scope->contains(it.key()))) continue;
            if(auto profile=Extract(it.value())) {
                const auto bytes=profile->general.dump().size()+profile->users.dump().size()+it.key().size();
                if(bytes>kSdkJsonMaximumBytes-profile_bytes_) continue;
                profile_bytes_+=bytes;
                profiles_.emplace(it.key(),std::move(*profile));
                if(recovery) ++recovered_;
            }
        }
    }
    static std::optional<std::string> Apply(const Profile& profile,const Json& incoming,const Json& previous) {
        auto envelope=Envelope(incoming);if(!envelope) return {};
        auto general=Object((*envelope)["GENERAL_DATA"]);if(!general.is_object()) return {};
        bool changed=false,general_changed=false;
        for(const auto& field:{"_overrideControllerMapKeyList","_overrideControllerMapValueList",
                              "enableInputHotSwap","disableFallbackControllerType"})
            if(profile.general.contains(field) && (!general.contains(field) || general[field]!=profile.general[field])) {
                general[field]=profile.general[field];general_changed=true;changed=true;
            }
        if(general_changed) (*envelope)["GENERAL_DATA"]=general.dump();
        std::optional<Json> old;
        for(auto it=profile.users.begin();it!=profile.users.end();++it) {
            Json value;bool user_changed=false;
            if(envelope->contains(it.key())) value=(*envelope)[it.key()];
            else {
                if(!old) old=Envelope(previous); // Only parse a second blob when needed.
                if(!old || !old->contains(it.key())) return {};
                value=(*old)[it.key()];user_changed=true;
            }
            auto user=Object(value);if(!user.is_object()) return {};
            for(auto field=it.value().begin();field!=it.value().end();++field)
                if(!user.contains(field.key()) || user[field.key()]!=field.value()) {
                    user[field.key()]=field.value();user_changed=true;
                }
            // Preserve all fresh non-controller data and the game's value type.
            if(user_changed) {
                (*envelope)[it.key()]=value.is_string() ? Json(user.dump()) : user;changed=true;
            }
        }
        if(!changed) return incoming.get<std::string>(); // Preserve the exact original bytes on the common path.
        const auto plain=envelope->dump();
        return SdkJsonWithinBudget(plain) ? EncodeNativeConfigUri(plain) : std::nullopt;
    }
public:
    NativeControllerLock(const Json& initial,const std::vector<Json>& recovery={}) {
        capture(initial,false);
        for(const auto& candidate:recovery) capture(candidate,true,&initial);
    }
    Json normalize(const Json& previous,const Json& incoming) {
        if(!incoming.is_object()) return incoming;
        // A first-time installation can learn once, but only from a game write
        // explicitly confirming a real controller. Existing locks never drift.
        capture(incoming,false);
        Json result=incoming;
        for(const auto& [key,profile]:profiles_) {
            if(!result.contains(key)) continue;
            const Json old=previous.is_object() && previous.contains(key) ? previous[key] : Json();
            if(const auto fixed=Apply(profile,result[key],old)) {
                if(result[key]!=*fixed) ++applied_;
                result[key]=*fixed;
            } else {
                ++rejected_;
                if(old.is_string()) result[key]=old;
                else result.erase(key);
            }
        }
        return result;
    }
    unsigned locked() const {return static_cast<unsigned>(profiles_.size());}
    unsigned recovered() const {return recovered_;}
    unsigned applied() const {return applied_;}
    unsigned rejected() const {return rejected_;}
};
}
