#include "mihoyo/sdk_bridge.hpp"
#include "mihoyo/sdk_cloud_data_store.hpp"
#include "mihoyo/game_control.hpp"
#include "mihoyo/signaling_frame.hpp"
#include "mihoyo/genshin_native_config.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace cloudgenshin;
using Json = nlohmann::json;
std::vector<std::uint8_t> Payload(const Json& json) {
    const auto encrypted = sdk::EncryptGameDataPayload(json.dump()); assert(encrypted);
    return sdk::EncodeSdkGameDataMessage({"SDK", {encrypted->begin(),encrypted->end()}});
}
void Receive(sdk::Bridge& bridge, const Json& json) {
    static std::uint32_t next_id = 17;
    sdk::ReliableMessageQueueData queue;
    queue.msg_id=next_id++;queue.msg_type=sdk::kRmqMessageNormal;queue.seq_id=queue.seq_cnt=1;
    queue.data=Payload(json);queue.data_len=queue.total_len=queue.data.size();
    assert(bridge.HandleProxyPacket(sdk::kReliableMessageQueueCmd,
        sdk::EncodeReliableMessageQueueData(queue),1000));
}
}
int main() {
    // Real SD store + AES/base64 SDK bridge + control packet + signaling frame.
    // No live user data, device, network or credentials are used in this test.
    for(const std::size_t size:{87834U,200000U}) {
        const auto home=std::filesystem::temp_directory_path()/
            ("yuanshi-large-config-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(home);
        const std::string key="cloud_local_data_123";
        std::string blob(size,'x');
        for(std::size_t i=0;i+4<blob.size();i+=257) {blob[i]='"';blob[i+1]='\\';blob[i+2]='\n';blob[i+3]='\0';}
        {sdk::CloudDataStore store(home.string(),"4","1","123");store.update(Json{{key,blob}});}
        {
            sdk::CloudDataStore store(home.string(),"4","1","123");
            assert(store.restored().at(key)==blob);
            sdk::BridgeContext context;context.initial_cloud_data=store.restored();
            context.merge_cloud_data_updates=true;
            std::vector<std::string> events;
            context.event=[&](const char* tag,int){events.emplace_back(tag);};
            unsigned replies=0,rejected_packets=0;
            std::uint32_t reply_id=0;
            std::string returned;
            sdk::Bridge bridge(context,[&](std::uint16_t command,const std::vector<std::uint8_t>& body) {
                const auto packet=control::EncodePacket({command,{},body});
                if(packet.empty()) {++rejected_packets;return false;}
                const auto frame=signaling::DecodeFrame(signaling::EncodeFrame(signaling::MessageType::Proxy,packet));
                assert(frame && frame->type==signaling::MessageType::Proxy);
                const auto decoded=control::DecodePacket(frame->payload);assert(decoded);
                const auto queue=sdk::DecodeReliableMessageQueueData(decoded->body);assert(queue);
                if(queue->msg_type==sdk::kRmqMessageNormal) {
                    reply_id=queue->msg_id;
                    assert(queue->seq_id==1 && queue->seq_cnt==1);
                    assert(queue->data_len==queue->total_len && queue->data.size()==queue->total_len);
                    const auto sdk_message=sdk::DecodeSdkGameDataMessage(queue->data);assert(sdk_message);
                    const auto plain=sdk::DecryptGameDataPayload(std::string(sdk_message->data.begin(),sdk_message->data.end()));assert(plain);
                    const auto reply=Json::parse(*plain);const auto params=Json::parse(reply["p"].get<std::string>());
                    assert(params["index"]==29);returned=params["data"].get<std::string>();++replies;
                }
                return true;
            });
            Receive(bridge,Json{{"f","cloud_get_data"},{"p",key},{"i",29}});
            if(rejected_packets || replies!=1 || returned!=blob) {
                std::cerr<<"FAIL restored large SDK configuration did not survive control framing, bytes="<<size<<'\n';
                return 1;
            }
            assert(reply_id>0 && reply_id<127);
            assert(std::count(events.begin(),events.end(),"GAME_SDK_LARGE_REPLY_SENT")==1);
            sdk::ReliableMessageQueueData ack;
            ack.msg_type=sdk::kRmqMessageAck;ack.msg_id=77; // Deliberately not the sender id.
            ack.data={8,static_cast<std::uint8_t>(reply_id+1)};
            bridge.HandleProxyPacket(sdk::kReliableMessageQueueCmd,sdk::EncodeReliableMessageQueueData(ack),1100);
            assert(std::count(events.begin(),events.end(),"GAME_SDK_LARGE_REPLY_ACKNOWLEDGED")==0);
            // Official proto ACK includes optional seqId and can carry extensions.
            // Earlier decoder incorrectly required field 1 to be the entire ACK.
            ack.data={16,1,8,static_cast<std::uint8_t>(reply_id),26,2,'o','k'};
            bridge.HandleProxyPacket(sdk::kReliableMessageQueueCmd,sdk::EncodeReliableMessageQueueData(ack),1200);
            bridge.HandleProxyPacket(sdk::kReliableMessageQueueCmd,sdk::EncodeReliableMessageQueueData(ack),1201);
            assert(std::count(events.begin(),events.end(),"GAME_SDK_LARGE_REPLY_ACKNOWLEDGED")==1);
            std::vector<std::string> failures;
            auto blocked_context=context;
            blocked_context.event=[&](const char* tag,int){failures.emplace_back(tag);};
            sdk::Bridge blocked(blocked_context,[](std::uint16_t,const std::vector<std::uint8_t>&){return false;});
            Receive(blocked,Json{{"f","cloud_get_data"},{"p",key},{"i",29}});
            assert(std::count(failures.begin(),failures.end(),"GAME_SDK_LARGE_REPLY_SEND_FAILED")==1);
            assert(std::count(failures.begin(),failures.end(),"GAME_SDK_LARGE_REPLY_SENT")==0);
            Json updated;
            context.on_cloud_data_changed=[&](const Json& data){updated=data;};
            sdk::Bridge incoming(context,[](std::uint16_t,const std::vector<std::uint8_t>&){return true;});
            Receive(incoming,Json{{"f","cloud_set_data"},{"p",Json{{key,blob}}.dump()}});
            assert(updated.at(key)==blob); // Encrypted wire size can exceed the plaintext budget.
        }
        std::filesystem::remove_all(home);
    }
    // Only the SDK queue gets a larger bounded packet; other commands keep 64 KiB.
    const std::vector<std::uint8_t> large(sdk::kMaximumSdkQueueBytes,0);
    const auto accepted=control::EncodePacket({sdk::kReliableMessageQueueCmd,{},large});
    assert(!accepted.empty() && control::DecodePacket(accepted));
    assert(control::EncodePacket({sdk::kReliableMessageQueueCmd,{},std::vector<std::uint8_t>(sdk::kMaximumSdkQueueBytes+1)}).empty());
    assert(control::EncodePacket({sdk::kRtcKeepPlayingCmd,{},large}).empty());
    auto wrong_command=accepted;wrong_command[2]=0;wrong_command[3]=1;
    assert(!control::DecodePacket(wrong_command));
    assert(sdk::DecodeReliableMessageQueueAck(std::vector<std::uint8_t>{8,7,16,1})==7);
    assert(sdk::DecodeReliableMessageQueueAck(std::vector<std::uint8_t>{16,1,8,7})==7);
    assert(!sdk::DecodeReliableMessageQueueAck(std::vector<std::uint8_t>{8,7,16}));
    assert(!sdk::DecodeReliableMessageQueueAck(std::vector<std::uint8_t>{8,0x80}));
    assert(!sdk::DecodeReliableMessageQueueAck(std::vector<std::uint8_t>{8,0xff,0xff,0xff,0xff,0x1f}));
    assert(!sdk::DecodeReliableMessageQueueAck(std::vector<std::uint8_t>(1025,0)));
    // Actual bridge: opt-in ys runtime reply requests native mode switching,
    // without modifying the stored map or affecting other games/default hosts.
    const Json general{{"_overrideControllerMapKeyList",Json::array({"xbox"})},
        {"_overrideControllerMapValueList",Json::array({"<CJoystickMap>native</CJoystickMap>"})},
        {"enableInputHotSwap",false},{"other",123}};
    const auto encoded=sdk::EncodeNativeConfigUri(Json{{"GENERAL_DATA",general.dump()}}.dump());assert(encoded);
    for(const std::string app:{"4","8"}) for(const bool enabled:{false,true}) {
        sdk::BridgeContext context;context.app_id=app;
        context.native_config_restore_mode=enabled
            ? cloudgenshin::NativeConfigRestoreMode::Full
            : cloudgenshin::NativeConfigRestoreMode::Off;
        context.serve_saved_cloud_data=true;
        context.initial_cloud_data=Json{{"cloud_local_data_123",*encoded}};
        std::string returned;unsigned writes=0;std::vector<std::string> events;
        context.on_cloud_data_changed=[&](const Json&){++writes;};
        context.event=[&](const char* name,int){events.emplace_back(name);};
        sdk::Bridge bridge(context,[&](std::uint16_t,const std::vector<std::uint8_t>& body) {
            const auto rmq=sdk::DecodeReliableMessageQueueData(body);assert(rmq);
            if(rmq->msg_type!=sdk::kRmqMessageNormal) return true;
            const auto message=sdk::DecodeSdkGameDataMessage(rmq->data);assert(message);
            const auto plain=sdk::DecryptGameDataPayload(std::string(message->data.begin(),message->data.end()));assert(plain);
            const auto reply=Json::parse(*plain);
            returned=Json::parse(reply["p"].get<std::string>())["data"].get<std::string>();return true;
        });
        Receive(bridge,Json{{"f","cloud_get_data"},{"p","cloud_local_data_123"},{"i",29}});
        const bool should_patch=app=="4" && enabled;
        assert((returned!=*encoded)==should_patch);
        auto restored=Json::parse(Json::parse(*sdk::DecodeNativeConfigUri(returned))["GENERAL_DATA"].get<std::string>());
        assert(restored["enableInputHotSwap"]==should_patch);
        restored["enableInputHotSwap"]=false;assert(restored==general);
        assert(writes==0 && context.initial_cloud_data["cloud_local_data_123"]==*encoded);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_INPUT_HOTSWAP_REQUESTED")==static_cast<int>(should_patch));
    }
    // 0.2.120 M0 keybind mode: reply contains only the controller maps. The
    // switches and unrelated settings are not echoed, and no hot-swap request is
    // emitted.
    {
        sdk::BridgeContext context;context.app_id="4";
        context.native_config_restore_mode=cloudgenshin::NativeConfigRestoreMode::Keybinds;
        context.serve_saved_cloud_data=true;
        context.initial_cloud_data=Json{{"cloud_local_data_123",*encoded}};
        std::string returned;std::vector<std::string> events;
        context.event=[&](const char* name,int){events.emplace_back(name);};
        sdk::Bridge bridge(context,[&](std::uint16_t,const std::vector<std::uint8_t>& body) {
            const auto rmq=sdk::DecodeReliableMessageQueueData(body);assert(rmq);
            if(rmq->msg_type!=sdk::kRmqMessageNormal) return true;
            const auto message=sdk::DecodeSdkGameDataMessage(rmq->data);assert(message);
            const auto plain=sdk::DecryptGameDataPayload(std::string(message->data.begin(),message->data.end()));assert(plain);
            const auto reply=Json::parse(*plain);
            const auto& envelope=reply["p"];
            if(!envelope.is_string()) return true; // cloud_set_data has no reply body
            const auto payload=Json::parse(envelope.get<std::string>());
            if(!payload.contains("data") || !payload["data"].is_string()) return true;
            returned=payload["data"].get<std::string>();return true;
        });
        Receive(bridge,Json{{"f","cloud_get_data"},{"p","cloud_local_data_123"},{"i",29}});
        const auto direct=sdk::PrepareNativeControllerRestore(
            *encoded,cloudgenshin::NativeConfigRestoreMode::Keybinds);
        assert(direct.runtime_value && returned==*direct.runtime_value);
        const auto restored=Json::parse(Json::parse(
            *sdk::DecodeNativeConfigUri(returned))["GENERAL_DATA"].get<std::string>());
        assert(restored.size()==3);
        assert(restored["_overrideControllerMapKeyList"]==Json::array({"xbox"}));
        assert(restored["_overrideControllerMapValueList"]==
            Json::array({"<CJoystickMap>native</CJoystickMap>"}));
        // Passed through unchanged from the saved config, never forced.
        assert(restored["enableInputHotSwap"]==false);
        assert(!restored.contains("other"));
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT")==1);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_INPUT_HOTSWAP_REQUESTED")==0);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_CUSTOM_GET_LOCAL_DATA")==1);
        // The game writes back its current state. Compare only controller maps;
        // matching maps are the real adoption evidence, the ACK is not.
        // cloud_set_data has no reply payload; ignore non-string data.
        Receive(bridge,Json{{"f","cloud_set_data"},{"p",Json{{"cloud_local_data_123",returned}}},{"i",30}});
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY")==1);
    }
    // Exact mode returns the complete original string, without Full's forced
    // hot-swap change. The real queue framing/crypto is exercised, not mocked.
    {
        sdk::BridgeContext context;context.app_id="4";
        context.native_config_restore_mode=cloudgenshin::NativeConfigRestoreMode::Exact;
        context.initial_cloud_data=Json{{"cloud_local_data_123",*encoded}};
        std::string returned;std::vector<std::string> events;
        context.event=[&](const char* name,int){events.emplace_back(name);};
        sdk::Bridge bridge(context,[&](std::uint16_t,const std::vector<std::uint8_t>& body) {
            const auto rmq=sdk::DecodeReliableMessageQueueData(body);assert(rmq);
            if(rmq->msg_type!=sdk::kRmqMessageNormal) return true;
            const auto message=sdk::DecodeSdkGameDataMessage(rmq->data);assert(message);
            const auto plain=sdk::DecryptGameDataPayload(std::string(message->data.begin(),message->data.end()));assert(plain);
            const auto reply=Json::parse(*plain);
            const auto payload=Json::parse(reply["p"].get<std::string>());
            returned=payload["data"].get<std::string>();return true;
        });
        Receive(bridge,Json{{"f","cloud_get_data"},{"p","cloud_local_data_123"},{"i",29}});
        assert(returned==*encoded);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_EXACT_CONFIG_PREPARED")==1);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_INPUT_HOTSWAP_REQUESTED")==0);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT")==0);
    }
    // Keybind mode must never fall back to the raw full config when parsing fails.
    {
        sdk::BridgeContext context;context.app_id="4";
        context.native_config_restore_mode=cloudgenshin::NativeConfigRestoreMode::Keybinds;
        context.serve_saved_cloud_data=true;
        context.initial_cloud_data=Json{{"cloud_local_data_123","not-a-native-config"}};
        std::string returned="unset";std::vector<std::string> events;
        context.event=[&](const char* name,int){events.emplace_back(name);};
        sdk::Bridge bridge(context,[&](std::uint16_t,const std::vector<std::uint8_t>& body) {
            const auto rmq=sdk::DecodeReliableMessageQueueData(body);assert(rmq);
            if(rmq->msg_type!=sdk::kRmqMessageNormal) return true;
            const auto message=sdk::DecodeSdkGameDataMessage(rmq->data);assert(message);
            const auto plain=sdk::DecryptGameDataPayload(std::string(message->data.begin(),message->data.end()));assert(plain);
            const auto reply=Json::parse(*plain);
            returned=Json::parse(reply["p"].get<std::string>())["data"].get<std::string>();return true;
        });
        Receive(bridge,Json{{"f","cloud_get_data"},{"p","cloud_local_data_123"},{"i",29}});
        assert(returned.empty());
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_KEYBIND_RESTORE_SKIPPED")==1);
        assert(std::count(events.begin(),events.end(),"GAME_SDK_NATIVE_BIND_REASON")==1);
    }
    // Three independent application lifetimes, real encrypted disk storage and
    // SDK wire framing. This proves restart persistence, not live-game input.
    {
        const auto home=std::filesystem::temp_directory_path()/
            ("yuanshi-binding-restart-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(home);
        const auto revised=*sdk::EncodeNativeConfigUri(Json{{"GENERAL_DATA",Json{
            {"_overrideControllerMapKeyList",Json::array({"xbox"})},
            {"_overrideControllerMapValueList",Json::array({"<CJoystickMap>changed-in-game</CJoystickMap>"})},
            {"enableInputHotSwap",true},{"inputData","opaque-device-choice"}}.dump()}}.dump());
        std::string expected;
        for(int launch=0;launch<3;++launch) {
            sdk::CloudDataStore disk(home.string(),"4","1","100001");
            sdk::BridgeContext context;context.app_id="4";
            context.native_config_restore_mode=cloudgenshin::NativeConfigRestoreMode::Exact;
            context.merge_cloud_data_updates=true;
            context.initial_cloud_data=disk.restored();
            context.on_cloud_data_changed=[&](const Json& update){disk.update(update);};
            std::string returned="not-received";
            sdk::Bridge bridge(context,[&](std::uint16_t,const std::vector<std::uint8_t>& body) {
                const auto rmq=sdk::DecodeReliableMessageQueueData(body);assert(rmq);
                if(rmq->msg_type!=sdk::kRmqMessageNormal) return true;
                const auto message=sdk::DecodeSdkGameDataMessage(rmq->data);assert(message);
                const auto plain=sdk::DecryptGameDataPayload(std::string(message->data.begin(),message->data.end()));assert(plain);
                const auto reply=Json::parse(*plain);
                if(!reply.contains("p") || !reply["p"].is_string()) return true;
                const auto payload=Json::parse(reply["p"].get<std::string>());
                if(payload.contains("data") && payload["data"].is_string()) returned=payload["data"].get<std::string>();
                return true;
            });
            Receive(bridge,Json{{"f","cloud_get_data"},{"p","cloud_local_data_123"},{"i",29}});
            assert(returned==expected);
            if(launch<2) {
                expected=launch==0 ? *encoded : revised;
                Receive(bridge,Json{{"f","cloud_set_data"},{"p",Json{{"cloud_local_data_123",expected}}.dump()},{"i",30}});
            }
            disk.stop();
        }
        std::filesystem::remove_all(home);
    }
    std::cout<<"PASS large SDK configuration, three-launch native binding persistence and command-specific size boundaries\n";
}
