// Reuse the deterministic fake curl/WebSocket transport, without running its
// standalone main. No network, TLS bypass or libpeer stubs enter the NRO.
#define main WebSocketTransportSelfTest
#include "websocket_client_test.cpp"
#undef main
#include "mihoyo/rtc_session.hpp"
#include "mihoyo/genshin_input_experiment.hpp"
#include "mihoyo/gyro_stick_assist.hpp"
#include "mihoyo/launcher_input_policy.hpp"
#include "mihoyo/controller_capture_policy.hpp"
#include "mihoyo/stream_quality_policy.hpp"
#include "mihoyo_sdp_answer_fixture.hpp"
#include <iostream>
#include <borealis/extern/nlohmann/json.hpp>

struct PeerConnection {
    PeerConfiguration config{};
    PeerConnectionState state=PEER_CONNECTION_NEW;
    void (*on_state)(PeerConnectionState,void*)=nullptr;
    void (*on_channel)(void*,uint16_t,const char*,uint32_t)=nullptr;
    void (*on_data)(char*,size_t,void*,uint16_t)=nullptr;
};
namespace {
PeerConnection* live_peer=nullptr;
unsigned creates=0,initializes=0,deinitializes=0;
unsigned accepted_channel_writes=0;
int candidate_result=0;
unsigned candidate_calls=0;
unsigned remote_descriptions=0,answer_creates=0;
bool local_candidate_available=true;
bool association_connected=true,association_init_failed=false;
int channel_send_result=0;
int measured_rtt=15;
int rtc_dtls_result=0;
std::function<int(PeerConnection*)> poll_hook;
std::string last_candidate;
std::vector<std::uint8_t> sent_data;
void InjectRaw(FakeTransport& fake,const std::vector<std::uint8_t>& bytes) {
    assert(bytes.size()<65536);
    if(bytes.size()<126) fake.input.insert(fake.input.end(),{0x82,static_cast<std::uint8_t>(bytes.size())});
    else fake.input.insert(fake.input.end(),{0x82,126,static_cast<std::uint8_t>(bytes.size()>>8),static_cast<std::uint8_t>(bytes.size())});
    fake.input.insert(fake.input.end(),bytes.begin(),bytes.end());
}
void Inject(cloudgenshin::signaling::MessageType type,const std::vector<std::uint8_t>& payload,FakeTransport* target=nullptr) {
    InjectRaw(target ? *target : latest(),cloudgenshin::signaling::EncodeFrame(type,payload));
}
}
namespace opennow {
const std::string& AppHomePath(){static const std::string path="test-only-missing-app-directory";return path;}
}
extern "C" {
int peer_init(){++initializes;return 0;}
void peer_deinit(){++deinitializes;}
PeerConnection* peer_connection_create(PeerConfiguration* config) {
    assert(!live_peer);++creates;live_peer=new PeerConnection;live_peer->config=*config;return live_peer;
}
void peer_connection_close(PeerConnection*){}
void peer_connection_destroy(PeerConnection* peer){assert(peer==live_peer);delete peer;live_peer=nullptr;}
int peer_connection_loop(PeerConnection* peer){return poll_hook ? poll_hook(peer):0;}
void peer_connection_ondatachannel(PeerConnection* peer,void(*message)(char*,size_t,void*,uint16_t),void(*)(void*),void(*)(void*)){peer->on_data=message;}
void peer_connection_on_remote_datachannel(PeerConnection* peer,void(*open)(void*,uint16_t,const char*,uint32_t)){peer->on_channel=open;}
void peer_connection_oniceconnectionstatechange(PeerConnection* peer,void(*state)(PeerConnectionState,void*)){peer->on_state=state;}
PeerConnectionState peer_connection_get_state(PeerConnection* peer){return peer->state;}
int peer_connection_get_rtt_ms(PeerConnection*){return measured_rtt;}
int peer_connection_datachannel_send_binary_sid(PeerConnection*,char* bytes,size_t size,uint16_t sid){assert(sid==3);if(channel_send_result>=0) {sent_data.assign(bytes,bytes+size);++accepted_channel_writes;}return channel_send_result;}
int peer_connection_sctp_connected(PeerConnection*){return association_connected;}
void peer_connection_set_diagnostics_enabled(int enabled){assert(enabled==0);}
int peer_connection_request_video_keyframe(PeerConnection*){return 0;}
void peer_connection_set_remote_description(PeerConnection*,const char*,SdpType type){assert(type==SDP_TYPE_OFFER);++remote_descriptions;}
const char* peer_connection_create_answer(PeerConnection*){++answer_creates;return sdp_fixture::local;}
int peer_connection_add_ice_candidate(PeerConnection*,char* text){++candidate_calls;last_candidate=text;return candidate_result;}
int peer_connection_get_transport_stats(PeerConnection*,PeerTransportStats* stats){*stats={};stats->dtls_result=rtc_dtls_result;stats->ice_local_candidates=local_candidate_available;stats->sctp_create_attempted=1;stats->sctp_create_result=association_init_failed ? -1 : 0;return 0;}
int peer_connection_set_receive_payload_types(PeerConnection* peer,int video,int audio){
    assert(video==100 && audio==109);
    return peer->state==PEER_CONNECTION_NEW || peer->state==PEER_CONNECTION_CHECKING ||
        peer->state==PEER_CONNECTION_COMPLETED ? 0 : -1;
}
}
int main() {
    using namespace cloudgenshin;
    unsigned authenticated=0;
    rtc::Session::Callbacks callbacks;
    callbacks.authenticated=[&](const control::StartResult& result){assert(result.accepted());++authenticated;};
    rtc::Session session("wss://example.invalid/cloud",{"t","a","v","s","p",10},{},callbacks,{},
        {{"wss://example.invalid/cloud/rtc-sdk","198.51.100.5",5678}});
    assert(session.start());session.poll();
    assert(session.network_rtt_ms()==-1);
    assert(!session.is_authenticated() && !live_peer && creates==0);
    assert(!session.set_graphics_mode(0) && !session.set_fps(60));
    assert(!session.set_bitrate_range(3000,4000));
    auto sent=frames(latest());assert(sent.size()==1 && sent[0].opcode==2);
    const auto initial=signaling::DecodeFrame({sent[0].payload.begin(),sent[0].payload.end()});
    assert(initial && initial->type==signaling::MessageType::Proxy && control::DecodePacket(initial->payload)->command==control::kStartGameReq);
    assert(!session.send_gamepad({},1));
    Inject(signaling::MessageType::Proxy,control::EncodePacket({20010,{}, {8,23,122,2,8,1}}));
    session.poll();session.poll();
    assert(!session.motion_requested() && session.motion_feedback_received()==1 && session.motion_feedback_before_auth()==1);
    assert(session.motion_activation_received()==0);
    // CaGameConfig enables the Xbox virtual pad here. A live pod with both
    // flags off is covered by the forced-pad policy: the session declares the
    // pad itself, so frames are still forwarded (see rtc_session.cpp).
    const auto success=control::EncodePacket({control::kStartGameRsp,{}, {8,1,16,4,26,2,8,1}});
    Inject(signaling::MessageType::Proxy,success);session.poll();session.poll();
    assert(session.is_authenticated() && session.gamepad_enabled() && authenticated==1 && live_peer && creates==1);
    sent=frames(latest());assert(sent.size()==2);
    const auto hello=signaling::DecodeFrame({sent[1].payload.begin(),sent[1].payload.end()});
    assert(hello && hello->type==signaling::MessageType::Handshake);
    Inject(signaling::MessageType::Proxy,success);session.poll();session.poll();assert(creates==1);
    const auto offer=nlohmann::json{{"type","offer"},{"sdp",sdp_fixture::offer}}.dump();
    Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});session.poll();session.poll();
    assert(session.has_answered());
    assert(!session.request_keyframe()); // Official request requires connected transport.
    const auto send_candidate=[&](const std::string& value) {
        const std::string message="{\"type\":\"candidates\",\"candidate\":\""+value+"\"}";
        Inject(signaling::MessageType::Signaling,{message.begin(),message.end()});session.poll();session.poll();
        assert(session.is_running());
    };
    send_candidate("candidate:1 1 tcp 120 192.0.2.1 9 typ host");
    assert(candidate_calls==0);
    candidate_result=-1;
    send_candidate("candidate:2 1 udp 120 192.0.2.1 8000 typ host");
    assert(candidate_calls==1);
    candidate_result=0;
    send_candidate("candidate:3 1 udp 120 192.0.2.2 8001 typ host");
    assert(candidate_calls==2);
    assert(last_candidate=="a=candidate:3 1 udp 120 198.51.100.5 5678 typ host");
    send_candidate("candidate:3 1 udp 120 192.0.2.2 8001 typ host");
    assert(candidate_calls==2);
    live_peer->on_channel(live_peer->config.user_data,3,"data",50);
    live_peer->state=PEER_CONNECTION_COMPLETED;
    live_peer->on_state(live_peer->state,live_peer->config.user_data);
    assert(session.network_rtt_ms()==15);
    measured_rtt=-1;assert(session.network_rtt_ms()==-1);measured_rtt=15;
    session.poll();assert(control::DecodePacket(sent_data)->command==20029);
    assert(session.request_keyframe());session.poll();
    const auto keyframe_frame=frames(latest()).back();
    const auto keyframe_signal=signaling::DecodeFrame({keyframe_frame.payload.begin(),keyframe_frame.payload.end()});
    assert(keyframe_signal && keyframe_signal->type==signaling::MessageType::Proxy &&
        control::DecodePacket(keyframe_signal->payload)->command==control::kKeyFrameReq);
    for(int tier=0;tier<3;++tier) {
        const auto before=frames(latest()).size();
        assert(session.set_graphics_mode(tier));session.poll();
        const auto outgoing=frames(latest());assert(outgoing.size()==before+2);
        const auto graphics=signaling::DecodeFrame({outgoing[before].payload.begin(),outgoing[before].payload.end()});
        const auto bitrate=signaling::DecodeFrame({outgoing[before+1].payload.begin(),outgoing[before+1].payload.end()});
        const auto range=*StreamQualityBitrate(tier);
        assert(graphics && graphics->type==signaling::MessageType::Proxy &&
            graphics->payload==control::EncodeSetGraphicsMode(tier));
        assert(bitrate && bitrate->type==signaling::MessageType::Proxy &&
            bitrate->payload==control::EncodeSetBitrate(MihoyoBitrate(range.minimum),MihoyoBitrate(range.maximum)));
    }
    const auto before_invalid=frames(latest()).size();
    assert(!session.set_graphics_mode(-1) && !session.set_graphics_mode(3));session.poll();
    assert(!session.set_bitrate_range(2999,4000) && !session.set_bitrate_range(4000,3000) &&
        !session.set_bitrate_range(3000,12001));session.poll();
    assert(frames(latest()).size()==before_invalid);
    assert(session.set_bitrate_range(3000,3300));session.poll();
    const auto limited=frames(latest()).back();
    const auto limited_signal=signaling::DecodeFrame({limited.payload.begin(),limited.payload.end()});
    assert(limited_signal && limited_signal->payload==control::EncodeSetBitrate(MihoyoBitrate(3000),MihoyoBitrate(3300)));
    for(const auto fps:{30,60}) {
        assert(session.set_fps(fps));session.poll();
        const auto outgoing=frames(latest()).back();
        const auto signal=signaling::DecodeFrame({outgoing.payload.begin(),outgoing.payload.end()});
        assert(signal && signal->payload==control::EncodeSetFps(fps));
    }
    assert(session.send_gamepad({},1) && control::DecodePacket(sent_data)->command==20010);
    input::MotionSample motion;
    motion.valid=true;motion.source=0;motion.sequence=1;motion.sampled_ms=100;motion.gravity={0,0,1};
    motion.orientation=input::MotionQuaternion{0.5f,0.5f,0.5f,0.5f};
    assert(!session.motion_requested() && !session.send_motion(motion,100));
    const auto imu_on=control::EncodePacket({20010,{}, {8,23,122,2,8,1}});
    Inject(signaling::MessageType::Proxy,imu_on);session.poll();session.poll();
    assert(session.motion_requested() && session.send_motion(motion,100));
    assert(session.motion_feedback_received()==2 && session.motion_feedback_before_auth()==1);
    assert(session.motion_activation_received()==1);
    Inject(signaling::MessageType::Proxy,control::EncodePacket({20010,{}, {8,23,122,2,8,2}}));
    session.poll();session.poll();
    assert(session.motion_feedback_invalid()==1 && session.motion_requested());
    assert(sent_data==input::EncodeInputPacket(input::EncodeImuChannel(motion)));
    assert(session.orientation_sent()==1);
    assert(!session.send_motion(motion,116) && session.motion_sent()==1);
    ++motion.sequence;motion.sampled_ms=116;channel_send_result=-1;
    assert(!session.send_motion(motion,116) && session.motion_failures()==1);
    assert(session.orientation_sent()==1); // Rejected transport write is not a pose sent.
    channel_send_result=0;
    assert(!session.send_motion(motion,132)); // Failure cannot queue stale motion.
    auto imu_off=control::EncodePacket({20010,{}, {8,23,122,0}});
    live_peer->on_data(reinterpret_cast<char*>(imu_off.data()),imu_off.size(),live_peer->config.user_data,3);
    assert(!session.motion_requested());
    assert(session.motion_feedback_received()==3);
    assert(session.motion_activation_received()==1); // A brief activation is not lost in a 5s snapshot.
    live_peer->on_state(PEER_CONNECTION_DISCONNECTED,live_peer->config.user_data);
    session.poll();assert(session.is_running() && session.is_recovering());
    assert(!session.input_ready());session.stop();
    assert(!live_peer && initializes==deinitializes);
    assert(session.network_rtt_ms()==-1);
    assert(!session.motion_requested() && !session.send_motion(motion,132));
    assert(session.orientation_sent()==0);
    assert(session.motion_activation_received()==0);
    {
        rtc::Session rejected("wss://example.invalid/cloud",{"t","a","v","s","p",10},{});
        assert(rejected.start());
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kStartGameRsp,{}, {8,0,16,4,26,0}}));
        rejected.poll();rejected.poll();assert(!rejected.is_running() && !rejected.is_authenticated() && creates==1);
        assert(rejected.last_error().find("code 0")!=std::string::npos);
        assert(std::string(rejected.last_error_event())=="GAME_AUTH_REJECTED");
    }
    std::atomic_bool cancelled{true};
    rtc::Session stopped("wss://example.invalid/cloud",{"t","a","v","s","p",10},{});
    assert(!stopped.start(&cancelled));
    assert(std::string(stopped.last_error_event())=="SIGNALING_CONNECT_CANCELLED");
    rtc::Session missing("wss://example.invalid/cloud",{},{});
    assert(!missing.start());
    assert(std::string(missing.last_error_event())=="GAME_AUTH_FIELDS_MISSING");
    unsigned attempts=0;
    auto fallback_callbacks=callbacks;
    fallback_callbacks.connect_attempt=[&](unsigned,const auto&){++attempts;};
    next_perform=CURLE_COULDNT_CONNECT;
    rtc::Session fallback("wss://example.invalid/primary",{"t","a","v","s","p",10},{},
        fallback_callbacks,{"wss://example.invalid/backup"},
        {{"wss://example.invalid/primary/rtc-sdk","192.0.2.1",1234},
         {"wss://example.invalid/backup/rtc-sdk","198.51.100.2",5678}});
    assert(fallback.start() && attempts==2);
    fallback.poll();
    const auto fallback_sent=frames(latest());
    assert(fallback_sent.size()==1);
    const auto fallback_auth=signaling::DecodeFrame({fallback_sent[0].payload.begin(),fallback_sent[0].payload.end()});
    assert(fallback_auth && fallback_auth->type==signaling::MessageType::Proxy);
    assert(!fallback.is_authenticated()); // no peer/hello created before game auth
    Inject(signaling::MessageType::Proxy,success);fallback.poll();fallback.poll();
    Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});fallback.poll();fallback.poll();
    const auto routed_candidate=rtc::EncodeCandidate("candidate:1 1 udp 120 10.0.0.1 1234 typ host","0",0);
    assert(routed_candidate);
    Inject(signaling::MessageType::Signaling,*routed_candidate);fallback.poll();fallback.poll();
    assert(last_candidate=="a=candidate:1 1 udp 120 198.51.100.2 5678 typ host");
    fallback.stop();
    {
        auto now=std::chrono::steady_clock::time_point{};
        std::vector<std::string> events;
        auto tracked=callbacks;tracked.candidate_event=[&](const char* event,int){events.emplace_back(event);};
        const auto begin=[&](const std::vector<std::uint8_t>& auth) {
            sdk::BridgeContext bridge; bridge.game_token="synthetic-activity-token";
            auto result=std::make_unique<rtc::Session>("wss://example.invalid/cloud",control::StartGame{"t","a","v","s","p",10},
                std::vector<rtc::IceServerConfig>{},tracked,std::vector<std::string>{},std::vector<rtc::DispatchRoute>{},[&]{return now;},
                std::vector<rtc::WebControlRoute>{},bridge);
            assert(result->start());
            Inject(signaling::MessageType::Proxy,auth);result->poll();result->poll();
            assert(result->is_authenticated() && live_peer);
            Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});result->poll();result->poll();
            assert(result->has_answered() && live_peer);
            live_peer->state=PEER_CONNECTION_COMPLETED;live_peer->on_state(live_peer->state,live_peer->config.user_data);
            return result;
        };
        association_connected=false;
        auto waiting=begin(success);waiting->poll();assert(!waiting->input_ready() && !waiting->send_keyboard(32,true));
        now+=std::chrono::seconds(15);waiting->poll();
        assert(!waiting->is_running() && std::string(waiting->last_error_event())=="RTC_SCTP_ASSOCIATION_TIMEOUT");
        association_connected=true;
        auto no_channel=begin(success);no_channel->poll();assert(!no_channel->has_remote_data_channel());
        now+=std::chrono::seconds(15);no_channel->poll();
        assert(!no_channel->is_running() && std::string(no_channel->last_error_event())=="RTC_DATA_CHANNEL_TIMEOUT");
        association_init_failed=true;
        auto failed=begin(success);failed->poll();
        assert(!failed->is_running() && std::string(failed->last_error_event())=="RTC_SCTP_INIT_FAILED");
        association_init_failed=false;events.clear();
        auto ready=begin(success);live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        channel_send_result=-1;ready->poll();assert(ready->input_ready());
        assert(std::count(events.begin(),events.end(),"CONTROL_HEARTBEAT_STARTED")==0);
        channel_send_result=0;now+=std::chrono::seconds(1);ready->poll();
        assert(std::count(events.begin(),events.end(),"CONTROL_HEARTBEAT_STARTED")==1);
        assert(ready->gamepad_enabled());
        assert(ready->send_gamepad({},1) && ready->send_gamepad({},2));
        assert(ready->send_gamepad_list(0x045e,0x028e));
        // A locally accepted device/state is not a cloud gamepad-mode receipt.
        assert(ready->server_control_mode()==-1);
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kSyncGameControlMode,{}, {8,3}}));
        ready->poll();ready->poll();assert(ready->server_control_mode()==3);
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kSyncGameControlMode,{}, {8}}));
        ready->poll();ready->poll();assert(ready->server_control_mode()==3); // Malformed reply cannot change the badge.
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kSyncGameControlMode,{}, {8,1}}));
        ready->poll();ready->poll();assert(ready->server_control_mode()==1); // A later server mode clears confirmation.
        assert(std::count(events.begin(),events.end(),"GAME_SDK_KEEP_PLAYING_SENT")==0);
        input::SwitchControllerState active; active.a=true;
        assert(ready->send_gamepad(active,3));
        assert(std::count(events.begin(),events.end(),"GAME_SDK_KEEP_PLAYING_SENT")==1);
        assert(ready->send_keyboard(32,true));
        association_connected=false;ready->poll();
        assert(ready->is_running() && !ready->input_ready() && !ready->send_keyboard(32,true));
        now+=std::chrono::seconds(4);ready->poll();assert(ready->is_running());
        association_connected=true;ready->poll();
        assert(ready->is_running() && ready->input_ready() && ready->send_keyboard(32,true));
        association_connected=false;ready->poll();
        now+=std::chrono::seconds(5);ready->poll();
        assert(ready->is_running() && ready->is_recovering());ready->stop();
        association_connected=true;
        // Recovery must never hide an explicit server stop or a closed
        // signaling connection, and must not requeue or recreate the peer.
        for(const bool server_stop:{true,false}) {
            auto recovering=begin(success);
            live_peer->on_channel(live_peer->config.user_data,3,"data",50);
            recovering->poll();assert(recovering->input_ready());
            association_connected=false;recovering->poll();
            assert(recovering->is_running() && !recovering->input_ready());
            if(server_stop) {
                Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kStopGameRsp,{}, {}}));
                recovering->poll();recovering->poll();
                assert(recovering->session_end_received());
                assert(std::string(recovering->last_error_event())=="GAME_SDK_SESSION_ENDED");
            } else {
                latest().receive_result=CURLE_RECV_ERROR;recovering->poll();
                assert(recovering->is_running() && recovering->is_recovering());recovering->stop();
            }
            assert(!recovering->is_running() && !live_peer);
            association_connected=true;
        }
    }
    // Server-directed renegotiation: the official web client answers every
    // later offer on the same RTCPeerConnection. A same-transport media update
    // reuses the established ICE/DTLS association and the first answer.
    {
        auto now=std::chrono::steady_clock::time_point{};
        std::vector<std::string> events;
        auto tracked=callbacks;
        tracked.candidate_event=[&](const char* event,int){events.emplace_back(event);};
        remote_descriptions=0;answer_creates=0;
        rtc::Session renegotiate("wss://example.invalid/cloud",{"t","a","v","s","p",10},{},tracked,{}, {},[&]{return now;});
        assert(renegotiate.start());
        Inject(signaling::MessageType::Proxy,success);renegotiate.poll();renegotiate.poll();
        auto* signal=&latest();
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);
        renegotiate.poll();renegotiate.poll();
        assert(renegotiate.has_answered() && remote_descriptions==1 && answer_creates==1);
        const auto first_frames=frames(*signal).size();
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);
        renegotiate.poll();renegotiate.poll();
        assert(renegotiate.is_running() && renegotiate.has_answered());
        assert(remote_descriptions==2 && answer_creates==1);
        const auto after_update=frames(*signal);
        assert(after_update.size()==first_frames+1);
        const auto update_answer=signaling::DecodeFrame(
            {after_update.back().payload.begin(),after_update.back().payload.end()});
        assert(update_answer && update_answer->type==signaling::MessageType::Signaling);
        const auto update_message=rtc::ParseSignalMessage(update_answer->payload);
        assert(update_message && update_message->type==rtc::SignalType::Answer);
        assert(std::count(events.begin(),events.end(),"RTC_RENEGOTIATION_ACCEPTED")==1);
        live_peer->state=PEER_CONNECTION_COMPLETED;
        live_peer->on_state(live_peer->state,live_peer->config.user_data);
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        // A long session can receive more than 32 legitimate media updates.
        // Only a burst is limited, and no update may recreate the transport.
        for(int i=0;i<40;++i) {
            now+=std::chrono::seconds(60);
            Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);
            renegotiate.poll();renegotiate.poll();
            assert(renegotiate.is_running() && renegotiate.input_ready());
        }
        // The rolling-minute cap still bounds a malfunctioning pod.
        for(std::uint32_t i=1;i<32;++i) {
            Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);
            renegotiate.poll();renegotiate.poll();
            assert(renegotiate.is_running());
        }
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);
        renegotiate.poll();renegotiate.poll();
        assert(!renegotiate.is_running());
        assert(std::string(renegotiate.last_error_event())=="RTC_RENEGOTIATION_LIMIT_EXCEEDED");
        assert(remote_descriptions==73 && answer_creates==1);
    }
    // A changed remote ICE ufrag is an ICE restart. libpeer's answer builder
    // rotates ICE/DTLS credentials of the live association, which only a
    // restart offer may request; the fixed unsupported error stays.
    {
        rtc::Session restart("wss://example.invalid/cloud",{"t","a","v","s","p",10},{},callbacks);
        assert(restart.start());
        Inject(signaling::MessageType::Proxy,success);restart.poll();restart.poll();
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});restart.poll();restart.poll();
        assert(restart.has_answered());
        std::string changed=sdp_fixture::offer;
        const auto at=changed.find("a=ice-ufrag:remote");
        assert(at!=std::string::npos);
        changed.replace(at+sizeof("a=ice-ufrag:")-1,sizeof("remote")-1,"remote-restart");
        const auto changed_offer=nlohmann::json{{"type","offer"},{"sdp",changed}}.dump();
        Inject(signaling::MessageType::Signaling,{changed_offer.begin(),changed_offer.end()});
        restart.poll();restart.poll();
        assert(!restart.is_running());
        assert(std::string(restart.last_error_event())=="RTC_RENEGOTIATION_UNSUPPORTED");
    }
    {
        local_candidate_available=false;
        rtc::Session no_network("wss://example.invalid/cloud",{"t","a","v","s","p",10},{});
        assert(no_network.start());
        Inject(signaling::MessageType::Proxy,success);no_network.poll();no_network.poll();
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});no_network.poll();no_network.poll();
        assert(!no_network.is_running() && std::string(no_network.last_error_event())=="RTC_LOCAL_CANDIDATE_UNAVAILABLE");
        local_candidate_available=true;
    }
    // Type 1/2 uses the official separate control WebSocket, even when SCTP
    // never connects. Exercise both handshake ordering and backup dispatch.
    for(std::uint8_t type:{1,2}) {
        auto now=std::chrono::steady_clock::time_point{};
        std::vector<std::string> events;
        std::vector<std::uint8_t> received;
        unsigned game_data_events=0;
        auto tracked=callbacks;
        tracked.candidate_event=[&](const char* event,int code){
            events.emplace_back(event);
            if(std::string_view(event)=="GAME_CONTROL_MESSAGE_RECEIVED") {assert(code==20013);++game_data_events;}
        };
        tracked.data=[&](const std::uint8_t* data,std::size_t size){received.assign(data,data+size);};
        association_connected=false;association_init_failed=true;sent_data.clear();
        rtc::Session web("wss://example.invalid/primary",{"t","a","v","s","p",10},{},tracked,
            {"wss://example.invalid/backup"},{},[&]{return now;},
            {{"wss://example.invalid/primary/rtc-sdk","https://primary.invalid/gcc"},
             {"wss://example.invalid/backup/rtc-sdk","https://backup.invalid/gcc"}});
        if(type==2) next_perform=CURLE_COULDNT_CONNECT;
        assert(web.start());auto* signal=&latest();
        const auto flush=[&] {web.poll();web.poll();};
        const std::string handshake="{\"session_id\":42}";
        const auto inject_handshake=[&] {Inject(signaling::MessageType::Handshake,{handshake.begin(),handshake.end()},signal);flush();};
        if(type==1) inject_handshake();
        assert(!web.is_authenticated());
        // CaGameConfig enables the Xbox virtual pad and selects web control
        // type 1/2, so type-17 frames may ride the control WebSocket.
        const auto web_auth=control::EncodePacket({control::kStartGameRsp,{}, {8,1,16,4,26,4,8,1,64,type}});
        Inject(signaling::MessageType::Proxy,web_auth,signal);flush();
        assert(web.is_authenticated() && web.gamepad_enabled());
        if(type==2) inject_handshake();
        auto* gcc=&latest();assert(gcc!=signal);
        assert(gcc->request.starts_with("GET /gcc?sessionId=42 HTTP/1.1\r\n"));
        assert(gcc->request.find(type==2 ? "Host: backup.invalid" : "Host: primary.invalid")!=std::string::npos);
        assert(!web.input_ready() && !web.send_keyboard(32,true));
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);flush();
        assert(web.has_answered());
        if(type==1) {
            InjectRaw(*gcc,rtc::EncodeControlSync(1251,42));flush();
            assert(web.has_remote_data_channel() && !web.input_ready() && !web.send_gamepad_list(1,2));
        }
        live_peer->state=PEER_CONNECTION_COMPLETED;live_peer->on_state(live_peer->state,live_peer->config.user_data);
        flush();assert(web.is_running());
        if(type==2) {
            assert(!web.has_remote_data_channel());
            InjectRaw(*gcc,rtc::EncodeControlSync(1251,42));flush();
        }
        assert(web.has_remote_data_channel() && web.input_ready() && web.send_keyboard(32,true));
        assert(web.send_mouse_move(1,2) && web.send_mouse_button(1,true,0.0,0.0) && web.send_gamepad({},1));
        assert(web.request_keyframe());
        flush();
        const auto keyframe=frames(*signal).back();
        const auto keyframe_message=signaling::DecodeFrame({keyframe.payload.begin(),keyframe.payload.end()});
        assert(keyframe_message && keyframe_message->type==signaling::MessageType::Proxy &&
            control::DecodePacket(keyframe_message->payload)->command==control::kKeyFrameReq);
        for(int i=0;i<12;++i) {
            now+=std::chrono::seconds(2);InjectRaw(*gcc,rtc::EncodeControlSync(1254,42));flush();
            assert(web.is_running() && web.input_ready());
        }
        assert(sent_data.empty()); // Neither input nor heartbeat used the nonexistent SCTP association.
        unsigned input_count=0,heartbeat_count=0,sync_count=0,ping_count=0;
        for(const auto& frame:frames(*gcc)) {
            assert(frame.opcode==2 && frame.payload.size()>1);
            const auto packet=control::DecodePacket({reinterpret_cast<const std::uint8_t*>(frame.payload.data()+1),frame.payload.size()-1});
            assert(packet);
            if(packet->command==1250 || packet->command==1252) {assert(frame.payload[0]==0);++sync_count;}
            if(packet->command==1253) {assert(frame.payload[0]==0);++ping_count;}
            if(packet->command==20010) {assert(frame.payload[0]==1);++input_count;}
            if(packet->command==20029) {assert(frame.payload[0]==1);++heartbeat_count;}
        }
        assert(sync_count==2 && ping_count>=10 && input_count==4 && heartbeat_count>=10);
        assert(std::count(events.begin(),events.end(),"CONTROL_WSS_READY")==1);
        assert(std::count(events.begin(),events.end(),"RTC_CONTROL_WAITING_ASSOCIATION")==0);
        assert(std::count(events.begin(),events.end(),"CONTROL_HEARTBEAT_STARTED")==1);
        auto motion_feedback=imu_on;motion_feedback.insert(motion_feedback.begin(),1);
        InjectRaw(*gcc,motion_feedback);flush();assert(web.motion_requested());
        input::MotionSample web_motion;
        web_motion.source=0;web_motion.sequence=1;web_motion.valid=true;web_motion.gravity={0,0,1};
        web_motion.orientation=input::MotionQuaternion{0.5f,0.5f,-0.5f,-0.5f};
        const auto motion_ms=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        web_motion.sampled_ms=motion_ms;
        assert(web.send_motion(web_motion,motion_ms));flush();
        const auto motion_frame=frames(*gcc).back();
        assert(motion_frame.payload[0]==1);
        const auto sent_motion=control::DecodePacket({reinterpret_cast<const std::uint8_t*>(motion_frame.payload.data()+1),motion_frame.payload.size()-1});
        assert(sent_motion && sent_motion->command==20010 && sent_motion->body==input::EncodeImuChannel(web_motion));
        assert(web.orientation_sent()==1);
        motion_feedback=imu_off;motion_feedback.insert(motion_feedback.begin(),1);
        InjectRaw(*gcc,motion_feedback);flush();assert(!web.motion_requested());
        auto data=control::EncodePacket({20030,{}, {8,1}});data.insert(data.begin(),1);
        InjectRaw(*gcc,data);flush();assert(received==std::vector<std::uint8_t>(data.begin()+1,data.end()));
        for(int i=0;i<2;++i) {
            Inject(signaling::MessageType::Proxy,control::EncodePacket({20013,{}, {8,1}}),signal);flush();
        }
        assert(game_data_events==1); // No bodies or per-packet logging.
        now+=std::chrono::seconds(6);web.poll();
        // Official graceful degradation: when the independent GameControlChannel
        // goes silent the client keeps the session and falls back to the RTC
        // data channel; the stream must not end because of the control socket.
        assert(std::count(events.begin(),events.end(),"CONTROL_WSS_PONG_TIMEOUT")==1);
        assert(std::count(events.begin(),events.end(),"CONTROL_WSS_FALLBACK_DATA_CHANNEL")==1);
        assert(web.is_running() && !web.input_ready()); // no SCTP fallback here
        assert(gcc->closed && !signal->closed);
        // Reusing the session must not reuse a stale control session id.
        assert(web.start());
        Inject(signaling::MessageType::Proxy,web_auth);flush();
        assert(web.is_authenticated());
        live_peer->state=PEER_CONNECTION_COMPLETED;live_peer->on_state(live_peer->state,live_peer->config.user_data);
        now+=std::chrono::seconds(15);web.poll();
        assert(web.is_running());
        assert(std::count(events.begin(),events.end(),"CONTROL_WSS_HANDSHAKE_TIMEOUT")==1);
        assert(std::count(events.begin(),events.end(),"CONTROL_WSS_FALLBACK_DATA_CHANNEL")==2);
        association_connected=true;association_init_failed=false;
    }
    for(const auto invalid:{false,true}) {
        std::vector<std::string> events;
        auto tracked=callbacks;
        tracked.candidate_event=[&](const char* event,int){events.emplace_back(event);};
        rtc::Session web("wss://example.invalid/cloud",{"t","a","v","s","p",10},{},tracked);
        assert(web.start());
        const auto auth=control::EncodePacket({control::kStartGameRsp,{}, {8,1,16,4,26,2,64,2}});
        Inject(signaling::MessageType::Proxy,auth);web.poll();web.poll();
        const std::string handshake=invalid ? "{\"session_id\":-1}" : "{\"session_id\":42}";
        Inject(signaling::MessageType::Handshake,{handshake.begin(),handshake.end()});web.poll();web.poll();
        // Control-channel setup failures degrade to the data channel; they do
        // not tear the session down.
        const std::string expected=invalid ? "CONTROL_WSS_HANDSHAKE_INVALID" : "CONTROL_WSS_ENDPOINT_MISSING";
        assert(web.is_running());
        assert(std::count(events.begin(),events.end(),expected)==1);
        assert(std::count(events.begin(),events.end(),"CONTROL_WSS_FALLBACK_DATA_CHANNEL")==1);
    }
    {
        auto now=std::chrono::steady_clock::time_point{};
        rtc::Session burst("wss://example.invalid/burst",{"t","a","v","s","p",10},{},callbacks,{}, {},[&]{return now;});
        assert(burst.start());
        Inject(signaling::MessageType::Proxy,success);burst.poll();burst.poll();
        unsigned handshake_calls=0;
        poll_hook=[&](PeerConnection*) {++handshake_calls;return 0;};
        burst.poll();assert(handshake_calls==1); // No duplicate connectivity/DTLS steps.
        poll_hook={};
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        live_peer->state=PEER_CONNECTION_COMPLETED;
        live_peer->on_state(live_peer->state,live_peer->config.user_data);
        burst.poll();assert(burst.input_ready());
        unsigned calls=0,remaining=31;
        poll_hook=[&](PeerConnection*) {++calls;if(remaining) {--remaining;return 1;}return 0;};
        burst.poll();
        assert(remaining==0 && calls==33 && !burst.receive_backlog_hint());
        assert(burst.receive_batch_maximum()==31 && burst.receive_budget_hits()==0);

        remaining=10000;calls=0;now+=std::chrono::seconds(2);sent_data.clear();
        burst.poll();
        assert(calls==128 && remaining==9872 && burst.receive_backlog_hint());
        assert(burst.receive_budget_hits()==2 && burst.receive_batch_maximum()==64);
        assert(control::DecodePacket(sent_data)->command==20029); // Heartbeat not starved by a full burst.
        assert(burst.send_gamepad({},2000)); // The caller's input still gets a turn.
        calls=0;remaining=0;burst.poll();
        assert(calls==2 && !burst.receive_backlog_hint()); // No idle spin, no sticky backlog flag.

        calls=0;
        poll_hook=[&](PeerConnection*) {++calls;now+=std::chrono::microseconds(500);return 1;};
        burst.poll();
        assert(calls==8 && burst.receive_backlog_hint()); // Two 2 ms cooperative batches.
        assert(burst.peer_drain_peak_us()==2000 && burst.receive_budget_hits()==4);
        assert(burst.peer_call_peak_us()==500 && burst.peer_slow_calls()==0);
        poll_hook=[&](PeerConnection* peer) {
            ++calls;peer->state=PEER_CONNECTION_FAILED;
            peer->on_state(peer->state,peer->config.user_data);return 1;
        };
        calls=0;burst.poll();
        assert(calls==1 && burst.is_running() && burst.is_recovering() && live_peer && !burst.receive_backlog_hint());
        assert(burst.receive_batch_maximum()==64 && burst.receive_budget_hits()==4); // Keep final diagnostics.
        poll_hook={};
        burst.stop();
        assert(burst.start());
        assert(burst.receive_budget_hits()==0 && burst.receive_batch_maximum()==0 && burst.peer_drain_peak_us()==0);
        assert(burst.peer_call_peak_us()==0 && burst.peer_slow_calls()==0);
        Inject(signaling::MessageType::Proxy,success);burst.poll();burst.poll();
        unsigned handshake_completions=0;
        poll_hook=[&](PeerConnection* peer) {
            if(peer->state!=PEER_CONNECTION_COMPLETED) {
                ++handshake_completions;peer->state=PEER_CONNECTION_COMPLETED;
                peer->on_state(peer->state,peer->config.user_data);
            }
            return 0;
        };
        now+=std::chrono::seconds(30);burst.poll();
        assert(handshake_completions==1 && burst.is_running()); // Complete handshake before evaluating its old deadline.
        calls=0;
        poll_hook=[&](PeerConnection* peer) {
            if(++calls==1) return 0; // Pre-control drain is idle.
            peer->state=PEER_CONNECTION_FAILED;
            peer->on_state(peer->state,peer->config.user_data);return 1;
        };
        burst.poll();
        assert(calls==2 && burst.is_running() && burst.is_recovering() && live_peer);
        burst.stop(); // Post-control failure enters recovery in the same poll.
        poll_hook={};
        burst.stop();
        std::cout<<"PASS production Session burst bounds, control fairness, deadline ordering and callback failure\n";
    }
    for(const auto mode:{input::GenshinInputMode::AndroidPs5,input::GenshinInputMode::WebPs5,input::GenshinInputMode::CurrentPs5})
    for(const unsigned original_terminal:{3U,6U,7U,10U}) {
        // Real Session implementation, synthetic transport only. Verify the
        // experiment changes StartGame + SdkDeviceInfo and BOTH pad messages,
        // not just the UI text; IMU still waits for a legitimate server event.
        const auto identity=input::ResolveSessionInputIdentity(mode,{original_terminal,0x045e,0x028e});
        if(mode==input::GenshinInputMode::CurrentPs5) assert(identity.terminal==original_terminal);
        sdk::BridgeContext bridge;
        bridge.cps=identity.terminal==3 || identity.terminal==7?"android_mihoyo":
            identity.terminal==6?"ios_mihoyo":"pc_mihoyo";
        rtc::Session android("wss://example.invalid/cloud",{"t","a","v","s","p",identity.terminal},{},
            {},{}, {},std::chrono::steady_clock::now,{},bridge);
        android.set_pad_identity(identity.vendor,identity.product);
        assert(android.start());android.poll();
        const auto request=frames(latest()).front();
        const auto signal=signaling::DecodeFrame({request.payload.begin(),request.payload.end()});
        assert(signal && signal->type==signaling::MessageType::Proxy);
        const auto start=control::DecodePacket(signal->payload);
        assert(start && start->command==control::kStartGameReq && start->body.size()>=11 &&
            start->body[9]==0x20 && start->body[10]==identity.terminal); // field 4, isolated terminal.
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kStartGameRsp,{}, {8,1,16,4,26,2,16,1}}));
        android.poll();android.poll();assert(android.is_authenticated() && live_peer);
        assert(!android.has_answered() && !android.input_ready()); // No media/input prerequisite for device info.
        assert(android.send_device_info(1280,720));android.poll();
        const auto device_frame=frames(latest()).back();
        const auto device_signal=signaling::DecodeFrame({device_frame.payload.begin(),device_frame.payload.end()});
        control::DeviceInfo info;
        info.game_token="t";info.sdk_version="v";info.terminal_type=identity.terminal;
        info.device_name="Nintendo Switch";info.device_gpu="NVIDIA Tegra X1";info.device_system="Horizon OS";
        info.network_type="wifi";info.transport_protocol="udp";info.screen_width=1280;info.screen_height=720;
        assert(device_signal && device_signal->payload==control::EncodeSdkDeviceInfo(info));
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        live_peer->state=PEER_CONNECTION_COMPLETED;
        live_peer->on_state(live_peer->state,live_peer->config.user_data);android.poll();
        assert(android.send_gamepad_list(android.pad_vendor_id(),android.pad_product_id()));
        assert(sent_data==input::EncodeInputPacket(input::EncodeGamepadListChannel(0x054c,0x0ce6)));
        input::SwitchControllerState pad;pad.a=true;pad.right_x=10000;
        assert(android.send_gamepad(pad,100));
        assert(sent_data==input::EncodeGamepadPacket(input::ToGamepadState(pad,100,0x054c,0x0ce6)));
        input::MotionSample sample;sample.valid=true;sample.source=0;sample.sequence=1;sample.sampled_ms=100;
        sample.gyro={0,1,0};sample.gravity={0,0,1};sample.orientation=input::MotionQuaternion{1,0,0,0};
        assert(!android.send_motion(sample,100) && android.motion_sent()==0);
        Inject(signaling::MessageType::Proxy,imu_on);android.poll();android.poll();
        assert(android.motion_requested() && android.send_motion(sample,100));
        assert(sent_data==input::EncodeInputPacket(input::EncodeImuChannel(sample)));
        assert(android.motion_sent()==1 && android.orientation_sent()==1);
        sample.sequence=2;sample.sampled_ms=116;channel_send_result=-1;
        assert(!android.send_motion(sample,116) && android.motion_sent()==1 && android.motion_failures()==1);
        channel_send_result=0;
        Inject(signaling::MessageType::Proxy,imu_off);android.poll();android.poll();
        sample.sequence=3;sample.sampled_ms=132;
        assert(!android.motion_requested() && !android.send_motion(sample,132));
        android.stop();assert(!live_peer);
        std::cout<<"PASS PS5 production Session terminal="<<identity.terminal<<" early device info, packets + IMU gate (offline transport)\n";
    }
    // Production Session + real input encoder, synthetic transport. Local gyro
    // is delivered as Xbox right-stick input with NO cloud IMU activation.
    for(const unsigned terminal:{3U,6U,7U,10U}) {
        sdk::BridgeContext context;context.app_id="4";
        rtc::Session xbox("wss://example.invalid/local-aim",{"t","a","v","s","p",terminal},{},
            {},{},{},std::chrono::steady_clock::now,{},context);
        xbox.set_pad_identity(input::kXboxVendorId,input::kXboxProductId);
        assert(xbox.start());xbox.poll();
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kStartGameRsp,{}, {8,1,16,4,26,2,16,1}}));
        xbox.poll();xbox.poll();
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        live_peer->state=PEER_CONNECTION_COMPLETED;
        live_peer->on_state(live_peer->state,live_peer->config.user_data);xbox.poll();
        assert(!xbox.motion_requested() && xbox.motion_activation_received()==0);
        const input::GyroSettings config;
        input::SwitchControllerState pad;pad.zl=true;
        input::MotionSample sample;sample.valid=true;sample.source=0;sample.sequence=1;sample.sampled_ms=100;
        sample.gyro={0,-0.2f,-0.3f};
        input::GyroStickAssist assist;
        sample.sampled_ms=0;
        const auto immediate=assist.apply(config,pad,sample,0,true,pad.zl);
        assert(assist.active() && immediate.zl && immediate.right_x<0 && immediate.right_y>0);
        sample.sampled_ms=100;
        const auto aim=assist.apply(config,pad,sample,100,true,pad.zl);
        assert(assist.active() && aim.right_x< -6000 && aim.right_y>6000);
        assert(xbox.send_gamepad_list(xbox.pad_vendor_id(),xbox.pad_product_id()));
        assert(sent_data==input::EncodeInputPacket(input::EncodeGamepadListChannel(input::kXboxVendorId,input::kXboxProductId)));
        std::uint64_t trace=0;
        const auto send_report=[&](const input::SwitchControllerState& report,std::int64_t now) {
            assert(xbox.send_gamepad(report,now));
            assert(sent_data==input::EncodeGamepadPacket(input::ToGamepadState(report,now),terminal==3?++trace:0));
        };
        send_report(aim,100);
        assert(!xbox.send_motion(sample,100) && xbox.motion_sent()==0);
        for(const auto face:{&input::SwitchControllerState::a,&input::SwitchControllerState::y}) {
            auto chord=pad;chord.l=true;chord.*face=true;
            const auto composed=assist.apply(config,chord,sample,100,true,chord.zl);
            assert(!assist.active() && composed.right_x==0 && composed.right_y==0);
            assert(input::ToGamepadState(composed,100).buttons==
                (face==&input::SwitchControllerState::a?0x2100u:0x4100u));
            send_report(composed,100);
        }
        pad.zl=false;
        const auto released=assist.apply(config,pad,sample,100,true,false);
        assert(!assist.active() && released.right_x==0 && released.right_y==0);
        send_report(released,116);
        // Native positions reach the real packet sender without the removed
        // combat preset or any custom local button transformation.
        const auto send_position=[&](const input::SwitchControllerState& raw,std::uint32_t expected) {
            const auto packet=input::ToGamepadState(raw,200);
            assert(packet.buttons==expected);
            send_report(raw,200);
        };
        pad={};pad.r=true;send_position(pad,0x0200); // Right shoulder -> Xbox RB, no combat swap.
        pad={};pad.a=true;send_position(pad,0x2000); // Right face -> Xbox B, no combat swap.
        pad={};pad.b=true;send_position(pad,0x1000); // Bottom face -> jump A.
        pad={};pad.x=true;send_position(pad,0x8000); // Top face -> burst Y.
        pad={};pad.y=true;send_position(pad,0x4000); // Left face -> interact X.
        pad={};pad.minus=true;send_position(pad,0x0020); // Minus -> game Back / map, never local diagnostics.
        send_position({},0); // Real Back release packet.
        pad={};pad.l=true;pad.r=true;
        send_position(pad,0x0300); // LB+RB; no context-dependent remapping.
        pad={};pad.l=true;pad.a=true;send_position(pad,0x2100); // Gadget on right-face chord.
        pad={};pad.r=true;pad.zl=true;send_position(pad,0x0200);
        assert(assist.apply(config,pad,sample,100,false,pad.zl).right_x==0); // UI capture, not another input mode.
        send_position({},0); // Explicit neutral: no sticky input across UI capture.
        xbox.stop();assert(!live_peer);
    }
    std::cout<<"PASS local physical-ZL gyro -> Xbox packets without native IMU activation, with neutral release\n";
    for(const auto app:{"4","8"}) for(const unsigned terminal:{3U,10U}) {
        sdk::BridgeContext context;context.app_id=app;
        std::vector<std::string> input_trace;
        context.trace=[&](const std::string& line){input_trace.push_back(line);};
        rtc::Session idle("wss://example.invalid/idle",{"t","a","v","s","p",terminal},{},callbacks,{}, {},
            std::chrono::steady_clock::now,{},context);
        assert(idle.start());idle.poll();
        Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kStartGameRsp,{}, {8,1,16,4,26,2,16,1}}));
        idle.poll();idle.poll();
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        live_peer->state=PEER_CONNECTION_COMPLETED;
        live_peer->on_state(live_peer->state,live_peer->config.user_data);idle.poll();
        const bool change_only=app==std::string("4") && terminal==3;
        auto writes=accepted_channel_writes;
        assert(idle.send_gamepad({},100));
        assert(sent_data==input::EncodeGamepadPacket(input::ToGamepadState({},100),change_only?1:0));
        for(int frame=0;frame<300;++frame) assert(idle.send_gamepad({},116+frame*16));
        assert(accepted_channel_writes==writes+(change_only?1:301));
        // Exercise the same fixed tap owner as the stream worker against the
        // production packet/session code (only the transport is fake).
        // Neutral pad refreshes must not replace a held touch with synthetic A/B.
        input::LauncherPointerTap tap;
        using TapAction=input::LauncherPointerTap::Action;
        const auto touch_at=[&](std::uint32_t action) {
            return std::vector<input::TouchEvent>{{0,action,tap.kX,tap.kY,true}};
        };
        writes=accepted_channel_writes;
        assert(tap.next(5000,true)==TapAction::Down);
        assert(idle.send_touch(touch_at(input::kTouchActionDown)));
        tap.sent(TapAction::Down,true,5000);
        assert(sent_data==input::EncodeInputPacket(input::EncodeTouchEventListChannel(
            touch_at(input::kTouchActionDown)),change_only?1:0));
        for(int frame=0;frame<8;++frame) {
            assert(tap.next(5000+frame*16,true)==TapAction::None);
            assert(idle.send_gamepad({},5000+frame*16));
        }
        assert(tap.next(5120,false)==TapAction::Up);
        assert(idle.send_touch(touch_at(input::kTouchActionUp)));
        tap.sent(TapAction::Up,true,5120);
        assert(!tap.held());
        assert(sent_data==input::EncodeInputPacket(input::EncodeTouchEventListChannel(
            touch_at(input::kTouchActionUp)),change_only?2:0));
        assert(accepted_channel_writes==writes+(change_only?2:10));
        input::SwitchControllerState held;held.b=true;
        writes=accepted_channel_writes;
        assert(idle.send_gamepad(held,5200) && idle.send_gamepad(held,5216));
        assert(accepted_channel_writes==writes+2);
        channel_send_result=-1;assert(!idle.send_gamepad({},5232));
        channel_send_result=0;assert(idle.send_gamepad({},5248));
        assert(sent_data==input::EncodeGamepadPacket(input::ToGamepadState({},5248),change_only?4:0));
        assert(std::count_if(input_trace.begin(),input_trace.end(),[](const auto& line){
            return line.starts_with("out rtc pad ");})==2); // Held edge + release, not held refresh spam.
        writes=accepted_channel_writes;
        assert(idle.send_gamepad({},5264,true)); // Explicit UI neutral must reach wire.
        assert(accepted_channel_writes==writes+1);
        writes=accepted_channel_writes;
        for(int frame=0;frame<10;++frame) assert(idle.send_gamepad({},5264+frame*16,true));
        assert(accepted_channel_writes==writes+10); // Gameplay keeps the old refresh cadence.
        assert(idle.reset_game_inputs(input::kInputControlGamepad|input::kInputControlTouchScreen));
        writes=accepted_channel_writes;
        assert(idle.send_gamepad({},5280) && accepted_channel_writes==writes+1);
        assert(idle.send_gamepad_list(idle.pad_vendor_id(),idle.pad_product_id()));
        writes=accepted_channel_writes;
        assert(idle.send_gamepad({},5296) && accepted_channel_writes==writes+1);
        association_connected=false;idle.poll();
        assert(!idle.send_gamepad({},5312)); // A cached idle is not a live channel.
        association_connected=true;idle.poll();
        writes=accepted_channel_writes;
        assert(idle.send_gamepad({},5328) && accepted_channel_writes==writes+1);
        for(int edge=0;edge<100;++edge) assert(idle.send_gamepad(edge%2?input::SwitchControllerState{}:held,5400+edge*16,true));
        const auto pad_traces=[&] {return std::count_if(input_trace.begin(),input_trace.end(),
            [](const auto& line){return line.starts_with("out rtc pad ");});};
        assert(pad_traces()==32); // Title attempts cannot create unbounded logs.
        idle.begin_gameplay_input_trace();
        input::SwitchControllerState camera;camera.right_x=20000;
        assert(idle.send_gamepad(camera,8000,true));
        assert(pad_traces()==33);
        assert(input_trace.back().find(" rx=20000 ")!=std::string::npos);
        for(int edge=0;edge<100;++edge) assert(idle.send_gamepad(edge%2?input::SwitchControllerState{}:camera,8016+edge*16,true));
        assert(pad_traces()==96);
        idle.begin_gameplay_input_trace(); // Duplicate will_enter/enter is not another budget.
        assert(idle.send_gamepad(held,10000,true) && pad_traces()==96);
        assert(input_trace.size()<=128); // Title 32 + gameplay 64 + other 32.
        // The test identity must be identical in device lists and reports,
        // including cached-neutral invalidation. No accidental mixed-PID pad.
        for(const auto product:{0x028eU,input::kXboxProductId}) {
            idle.set_pad_identity(input::kXboxVendorId,product);
            assert(idle.send_gamepad_list(idle.pad_vendor_id(),idle.pad_product_id()));
            assert(sent_data==input::EncodeInputPacket(input::EncodeGamepadListChannel(input::kXboxVendorId,product)));
            assert(idle.send_gamepad({},10016));
            const auto initial=control::DecodePacket(sent_data);
            assert(initial && initial->command==20010);
            const auto expected=input::EncodeGamepadChannel(input::ToGamepadState({},10016,input::kXboxVendorId,product));
            assert(initial->body.size()>=expected.size() &&
                std::equal(expected.begin(),expected.end(),initial->body.begin()));
        }
        // All physical Switch positions through the same release/system-key
        // filters, composition and production Session encoder. Golden masks
        // are independent of ToGamepadState; no action-level local remapping.
        struct ButtonCase {bool input::SwitchControllerState::*member;std::uint32_t mask,left,right;};
        const std::array<ButtonCase,16> button_cases{{
            {&input::SwitchControllerState::dpad_up,1,0,0},
            {&input::SwitchControllerState::dpad_down,2,0,0},
            {&input::SwitchControllerState::dpad_left,4,0,0},
            {&input::SwitchControllerState::dpad_right,8,0,0},
            {&input::SwitchControllerState::plus,16,0,0},
            {&input::SwitchControllerState::minus,32,0,0},
            {&input::SwitchControllerState::stick_left,64,0,0},
            {&input::SwitchControllerState::stick_right,128,0,0},
            {&input::SwitchControllerState::l,256,0,0},
            {&input::SwitchControllerState::r,512,0,0},
            {&input::SwitchControllerState::b,4096,0,0},
            {&input::SwitchControllerState::a,8192,0,0},
            {&input::SwitchControllerState::y,16384,0,0},
            {&input::SwitchControllerState::x,32768,0,0},
            {&input::SwitchControllerState::zl,0,255,0},
            {&input::SwitchControllerState::zr,0,0,255}}};
        input::ControllerSystemButtons system_buttons;
        input::InputReleaseGate release_gate;
        assert(!release_gate.suppress({},20000));
        for(const auto& key:button_cases) {
            input::SwitchControllerState physical;
            physical.*key.member=true;
            auto final=input::ComposeGamepadFrame(physical,false,false);
            system_buttons.observe(physical);
            if(release_gate.suppress(physical,20000)) final={};
            system_buttons.filter(final);
            input::GamepadState expected{input::kXboxVendorId,input::kXboxProductId,key.mask,
                key.left,key.right,0,0,0,0,20000};
            assert(idle.send_gamepad(final,20000,true));
            const auto packet=control::DecodePacket(sent_data);assert(packet && packet->command==20010);
            const auto prefix=input::EncodeGamepadChannel(expected);
            assert(packet->body.size()>=prefix.size() && std::equal(prefix.begin(),prefix.end(),packet->body.begin()));
            system_buttons.observe({});
            assert(idle.send_gamepad({},20016,true)); // Release every button independently.
        }
        const std::array<std::int32_t input::SwitchControllerState::*,4> axes{{
            &input::SwitchControllerState::left_x,&input::SwitchControllerState::left_y,
            &input::SwitchControllerState::right_x,&input::SwitchControllerState::right_y}};
        for(unsigned axis=0;axis<axes.size();++axis) for(const auto value:{-32767,32767}) {
            input::SwitchControllerState physical;physical.*axes[axis]=value;
            input::GamepadState expected;expected.vendor_id=input::kXboxVendorId;
            expected.product_id=input::kXboxProductId;expected.rtt_trace_send_time=21000;
            if(axis==0) expected.left_thumb_x=value;
            else if(axis==1) expected.left_thumb_y=value;
            else if(axis==2) expected.right_thumb_x=value;
            else expected.right_thumb_y=value;
            assert(idle.send_gamepad(physical,21000,true));
            const auto packet=control::DecodePacket(sent_data);assert(packet);
            const auto prefix=input::EncodeGamepadChannel(expected);
            assert(packet->body.size()>=prefix.size() && std::equal(prefix.begin(),prefix.end(),packet->body.begin()));
            assert(idle.send_gamepad({},21016,true));
        }
        idle.stop();assert(!live_peer);
    }
    std::cout<<"PASS production Session Android ys idle dedup, uninterrupted touch, held/release, failure and recovery; other routes unchanged\n";
    for(const auto app:{"4","8"}) for(const bool server_pad:{false,true}) for(const bool restored:{false,true}) {
        auto now=std::chrono::steady_clock::time_point{};
        std::vector<std::string> events;
        auto tracked=callbacks;
        tracked.candidate_event=[&](const char* tag,int){events.emplace_back(tag);};
        sdk::BridgeContext context;context.app_id=app;
        if(restored) context.initial_cloud_data=nlohmann::json{{"cloud_local_data_123",std::string(87834,'x')}};
        association_connected=false;association_init_failed=false;sent_data.clear();
        rtc::Session native("wss://example.invalid/native-pad",{"t","a","v","s","p",10},{},tracked,
            {},{},[&]{return now;},{{"wss://example.invalid/native-pad/rtc-sdk","https://native.invalid/gcc"}},context);
        assert(native.start());auto* signal=&latest();
        const auto flush=[&]{native.poll();native.poll();};
        const auto auth=control::EncodePacket({control::kStartGameRsp,{},
            server_pad ? std::vector<std::uint8_t>{8,1,16,4,26,4,8,1,64,2} : std::vector<std::uint8_t>{8,1,16,4,26,2,64,2}});
        Inject(signaling::MessageType::Proxy,auth,signal);flush();
        const std::string handshake="{\"session_id\":42}";
        Inject(signaling::MessageType::Handshake,{handshake.begin(),handshake.end()},signal);flush();
        auto* gcc=&latest();assert(gcc!=signal);
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()},signal);flush();
        live_peer->state=PEER_CONNECTION_COMPLETED;live_peer->on_state(live_peer->state,live_peer->config.user_data);
        InjectRaw(*gcc,rtc::EncodeControlSync(1251,42));flush();
        assert(native.input_ready() && native.input_transport_generation()==1);
        input::SwitchControllerState held;held.a=true;held.left_x=12000;
        assert(native.send_gamepad(held,100));flush();
        assert(sent_data.empty()); // GCC remains usable until RTC is actually ready.
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);
        association_connected=true;flush();
        const bool prefer=std::string_view(app)=="4" && !server_pad;
        assert(native.input_ready() && native.input_transport_generation()==(prefer?2:1));
        const auto before=frames(*gcc).size();sent_data.clear();
        assert(native.send_gamepad_list(input::kXboxVendorId,input::kXboxProductId));
        if(prefer) assert(sent_data==input::EncodeInputPacket(input::EncodeGamepadListChannel(input::kXboxVendorId,input::kXboxProductId)));
        assert(native.reset_game_inputs(input::kInputControlGamepad));
        if(prefer) assert(sent_data==input::EncodeInputPacket(input::EncodeResetGameInputControlChannel(input::kInputControlGamepad)));
        assert(native.send_gamepad({},101)); // Neutral after source declaration.
        if(prefer) assert(sent_data==input::EncodeGamepadPacket(input::ToGamepadState({},101)));
        assert(native.send_gamepad(held,102));flush();
        if(prefer) {
            assert(sent_data==input::EncodeGamepadPacket(input::ToGamepadState(held,102)));
            assert(frames(*gcc).size()==before); // Never double-send a press on both routes.
            assert(std::count(events.begin(),events.end(),"NATIVE_GAMEPAD_RTC_PREFERRED")==1);
            association_connected=false;flush(); // RTC loss -> GCC, ready stays true.
            assert(native.input_ready() && native.input_transport_generation()==3);
            const auto fallback_before=frames(*gcc).size();
            assert(native.send_gamepad({},103));flush();assert(frames(*gcc).size()==fallback_before+1);
            association_connected=true;flush();
            assert(native.input_ready() && native.input_transport_generation()==4);
            // Failure of the now-unused GCC must not reset the active RTC pad.
            now+=std::chrono::seconds(6);flush();
            assert(native.is_running() && native.input_ready());
            assert(std::count(events.begin(),events.end(),"CONTROL_WSS_RESET_INPUTS_SENT")==0);
        } else {
            assert(sent_data.empty() && frames(*gcc).size()==before+4);
            assert(std::count(events.begin(),events.end(),"NATIVE_GAMEPAD_RTC_PREFERRED")==0);
        }
        native.stop();assert(!live_peer);association_connected=true;
    }
    std::cout<<"PASS native ys forced Xbox stable RTC route, preserved GCC fallback, epochs, config-independent identity and neutral declaration\n";
    {
        // Restored config crosses the production Session, signaling client and
        // WebSocket encoder. Previously a >64 KiB body became an empty proxy
        // and was incorrectly reported as sent even though no RMQ data left.
        const std::string key="cloud_local_data_123",blob(87834,'x');
        sdk::BridgeContext restored;restored.initial_cloud_data=nlohmann::json{{key,blob}};
        rtc::Session large_config("wss://example.invalid/large-config",{"t","a","v","s","p",10},{},
            {},{}, {},std::chrono::steady_clock::now,{},restored);
        assert(large_config.start());large_config.poll();
        Inject(signaling::MessageType::Proxy,success);large_config.poll();large_config.poll();
        const auto request=nlohmann::json{{"f","cloud_get_data"},{"p",key},{"i",29}}.dump();
        const auto encrypted=sdk::EncryptGameDataPayload(request);assert(encrypted);
        sdk::ReliableMessageQueueData queue;
        queue.msg_id=17;queue.msg_type=sdk::kRmqMessageNormal;queue.seq_id=queue.seq_cnt=1;
        queue.data=sdk::EncodeSdkGameDataMessage({"SDK",{encrypted->begin(),encrypted->end()}});
        queue.total_len=queue.data_len=queue.data.size();
        Inject(signaling::MessageType::Proxy,control::EncodePacket({sdk::kReliableMessageQueueCmd,{},
            sdk::EncodeReliableMessageQueueData(queue)}));
        large_config.poll();large_config.poll();
        unsigned returned=0;
        for(const auto& frame:frames(latest())) {
            if(frame.opcode!=2) continue;
            const auto signal=signaling::DecodeFrame({frame.payload.begin(),frame.payload.end()});assert(signal);
            if(signal->type!=signaling::MessageType::Proxy) continue;
            const auto packet=control::DecodePacket(signal->payload);assert(packet); // No empty proxy.
            if(packet->command!=sdk::kReliableMessageQueueCmd) continue;
            const auto response=sdk::DecodeReliableMessageQueueData(packet->body);assert(response);
            if(response->msg_type!=sdk::kRmqMessageNormal) continue;
            const auto data=sdk::DecodeSdkGameDataMessage(response->data);assert(data);
            const auto plain=sdk::DecryptGameDataPayload(std::string(data->data.begin(),data->data.end()));assert(plain);
            const auto message=nlohmann::json::parse(*plain);
            const auto params=nlohmann::json::parse(message["p"].get<std::string>());
            assert(params["index"]==29 && params["data"]==blob);++returned;
        }
        assert(returned==1);large_config.stop();assert(!live_peer);
        std::cout<<"PASS restored 88 KiB config over production Session/WebSocket without empty proxy\n";
    }
    // Production Session + WebSocket failure injection. A disconnect keeps
    // the game allocation, SDK instance and last frame alive while reconnect
    // uses a separate socket owner. Only decoded video + control proves repair.
    for(int scenario=0;scenario<13;++scenario) {
        auto now=std::chrono::steady_clock::time_point{};
        std::vector<std::string> events;
        std::uint64_t decoded=10;
        rtc::Session::Callbacks cb;
        cb.candidate_event=[&](const char* tag,int){events.emplace_back(tag);};
        cb.decoded_frames=[&]{return decoded;};
        rtc::Session repaired("wss://example.invalid/recovery",{"t","a","v","s","p",3},{},cb,{}, {},[&]{return now;});
        assert(repaired.start());
        Inject(signaling::MessageType::Proxy,success);repaired.poll();repaired.poll();
        Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});repaired.poll();repaired.poll();
        live_peer->state=PEER_CONNECTION_COMPLETED;
        live_peer->on_state(live_peer->state,live_peer->config.user_data);
        live_peer->on_channel(live_peer->config.user_data,3,"data",50);repaired.poll();
        assert(repaired.input_ready());
        const auto initial_peer=live_peer;const auto initial_creates=creates;
        auto* original_socket=&latest();
        const auto count=[&](const char* event){return std::count(events.begin(),events.end(),event);};
        const auto wait_event=[&](const char* event,int target) {
            for(int step=0;step<2000 && count(event)<target && repaired.is_running();++step) {
                repaired.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(count(event)>=target);
        };
        if(scenario==8 || scenario==9) {
            if(scenario==8) {
                rtc_dtls_result=-9984;live_peer->state=PEER_CONNECTION_FAILED;
                live_peer->on_state(live_peer->state,live_peer->config.user_data);
            } else InjectRaw(latest(),{0xff});
            repaired.poll();repaired.poll();
            assert(!repaired.is_running() && count("STREAM_RECONNECTING")==0);
            if(scenario==8) assert(std::string(repaired.last_error_event())=="RTC_DTLS_CERTIFICATE_VERIFY_FAILED");
            rtc_dtls_result=0;continue;
        }
        if(scenario==11) {
            association_connected=false;repaired.poll();now+=std::chrono::seconds(5);
        } else if(scenario==1) {
            live_peer->state=PEER_CONNECTION_FAILED;live_peer->on_state(live_peer->state,live_peer->config.user_data);
        } else original_socket->receive_result=CURLE_RECV_ERROR;
        repaired.poll();assert(repaired.is_running() && repaired.is_recovering() && !repaired.input_ready());
        assert(count("STREAM_RECONNECTING")==1 && live_peer==initial_peer);
        if(scenario==3) {
            const auto end=control::EncodePacket({control::kStopGameRsp,{}, {}});
            live_peer->on_data(reinterpret_cast<char*>(const_cast<std::uint8_t*>(end.data())),end.size(),live_peer->config.user_data,3);
            repaired.poll();assert(!repaired.is_running() && repaired.session_end_received());
            assert(count("STREAM_RECONNECT_ATTEMPT")==0);continue;
        }
        if(scenario==4) {
            for(int attempt=1;attempt<=3;++attempt) {
                next_perform=CURLE_COULDNT_CONNECT;now+=std::chrono::seconds(2);repaired.poll();
                wait_event("STREAM_RECONNECT_RETRY",attempt);
            }
            repaired.poll();assert(!repaired.is_running());
            assert(count("STREAM_RECONNECT_ATTEMPT")==3 && count("STREAM_RECONNECT_EXHAUSTED")==1);
            assert(std::string(repaired.last_error_event())=="STREAM_RECONNECT_EXHAUSTED");
        } else {
            if(scenario==5) next_perform=CURLE_PEER_FAILED_VERIFICATION;
            now+=std::chrono::milliseconds(250);repaired.poll();
            if(scenario==6) {
                // User cancellation joins the connector; no late callback can
                // resurrect the view or leave a background socket owner alive.
                repaired.stop();assert(!repaired.is_running() && !live_peer);continue;
            }
            if(scenario==5) {
                wait_event("STREAM_RECONNECT_EXHAUSTED",1);
                assert(!repaired.is_running() && count("STREAM_RECONNECT_ATTEMPT")==1);
            } else {
                wait_event("STREAM_RECONNECT_TRANSPORT_READY",1);
                assert(live_peer==initial_peer && repaired.is_recovering());
                // No StopGameReq may precede this re-authentication.
                bool auth_seen=false;
                for(const auto& frame:frames(latest())) if(frame.opcode==2) {
                    const auto sig=signaling::DecodeFrame({frame.payload.begin(),frame.payload.end()});assert(sig);
                    if(sig->type!=signaling::MessageType::Proxy) continue;
                    const auto packet=control::DecodePacket(sig->payload);assert(packet);
                    assert(packet->command!=control::kStopGameReq);
                    if(packet->command==control::kStartGameReq) {
                        control::StartGame expected{"t","a","v","s","p",3};expected.has_rtc_connected=scenario!=1 && scenario!=11;
                        assert(sig->payload==control::EncodeStartGame(expected));auth_seen=true;
                    }
                }
                assert(auth_seen);
                if(scenario==12) {
                    InjectRaw(latest(),{0xff});repaired.poll();repaired.poll();repaired.poll();
                    assert(!repaired.is_running() && count("STREAM_RECONNECT_ATTEMPT")==1);
                    assert(std::string(repaired.last_error_event())=="SIGNALING_PROTOCOL_INVALID");continue;
                }
                if(scenario==2) {
                    Inject(signaling::MessageType::Proxy,control::EncodePacket({control::kStartGameRsp,{}, {8,0,16,4,26,0}}));
                    repaired.poll();repaired.poll();assert(!repaired.is_running());
                    assert(std::string(repaired.last_error_event())=="GAME_AUTH_REJECTED");continue;
                }
                Inject(signaling::MessageType::Proxy,success);repaired.poll();repaired.poll();
                assert(count("STREAM_RECONNECT_AUTHENTICATED")==1 && repaired.is_recovering());
                if(scenario==10) {
                    // An auth ACK without new decoded video must time out,
                    // then rebuild the otherwise nominally completed peer.
                    now+=std::chrono::seconds(10);repaired.poll();
                    assert(repaired.is_recovering() && count("STREAM_RECONNECT_RETRY")==1);
                    now+=std::chrono::seconds(2);repaired.poll();
                    wait_event("STREAM_RECONNECT_TRANSPORT_READY",2);
                    Inject(signaling::MessageType::Proxy,success);repaired.poll();repaired.poll();
                    assert(creates==initial_creates+1);
                }
                if(scenario==7) {
                    std::string sdp=sdp_fixture::offer;const auto at=sdp.find("a=ice-ufrag:");assert(at!=sdp.npos);
                    const auto end=sdp.find_first_of("\r\n",at);assert(end!=sdp.npos);
                    sdp.replace(at,end-at,"a=ice-ufrag:recoveryfresh");
                    const auto rotated=nlohmann::json{{"type","offer"},{"sdp",sdp}}.dump();
                    Inject(signaling::MessageType::Signaling,{rotated.begin(),rotated.end()});repaired.poll();repaired.poll();
                    assert(creates==initial_creates+1 && count("STREAM_RECONNECT_PEER_REBUILT")==1);
                }
                if(scenario==1 || scenario==7 || scenario==10 || scenario==11) {
                    assert(creates==initial_creates+1);
                    if(scenario!=7) {Inject(signaling::MessageType::Signaling,{offer.begin(),offer.end()});repaired.poll();repaired.poll();}
                    association_connected=true;
                    live_peer->state=PEER_CONNECTION_COMPLETED;
                    live_peer->on_state(live_peer->state,live_peer->config.user_data);
                    live_peer->on_channel(live_peer->config.user_data,3,"data",50);repaired.poll();
                } else assert(creates==initial_creates && live_peer==initial_peer);
                std::uint8_t byte=1;PeerVideoPacket packet{&byte,1,0,0};
                live_peer->config.onvideopacket(&packet,live_peer->config.user_data);
                repaired.poll();assert(repaired.is_recovering()); // RTP alone is not decoded video.
                ++decoded;repaired.poll();assert(!repaired.is_recovering() && repaired.input_ready());
                assert(count("STREAM_RECONNECTED")==1 && repaired.pad_vendor_id()==input::kXboxVendorId);
                assert(repaired.send_gamepad({},1000,true));repaired.stop();
                std::cout<<"PASS automatic same-allocation recovery scenario="<<scenario<<"\n";
            }
        }
        assert(!live_peer);
        // Exhaustion never sends a server StopGameReq, including a retained
        // original socket. A later manual connection may still resume the pod.
        for(const auto& frame:frames(*original_socket)) if(frame.opcode==2) {
            const auto sig=signaling::DecodeFrame({frame.payload.begin(),frame.payload.end()});assert(sig);
            if(sig->type==signaling::MessageType::Proxy) assert(control::DecodePacket(sig->payload)->command!=control::kStopGameReq);
        }
    }
    next_perform=CURLE_PEER_FAILED_VERIFICATION;
    rtc::Session tls_failure("wss://example.invalid/primary",{"t","a","v","s","p",10},{});
    assert(!tls_failure.start());
    assert(std::string(tls_failure.last_error_event())=="SIGNALING_TCP_TLS_FAILED");
}
