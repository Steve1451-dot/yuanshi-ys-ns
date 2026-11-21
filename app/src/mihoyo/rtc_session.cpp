#include "mihoyo/rtc_session.hpp"
#include "mihoyo/stream_quality_policy.hpp"
#include "webrtc/media_ingress_diagnostics.hpp"
#include "webrtc/peer_poll_policy.hpp"
#include "mihoyo/peer_recovery_policy.hpp"
#include "mihoyo/ice_candidate_policy.hpp"
#include "mihoyo/sdp_answer_policy.hpp"
#include "mihoyo/sdp_answer.hpp"
#include "mihoyo/autotest_policy.hpp"
#include "app_paths.hpp"

#include <utility>
#include <climits>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace cloudgenshin::rtc {
namespace {

constexpr std::size_t kMaximumPendingCandidates = 64;
constexpr std::size_t kSdkTraceLimitBytes = 512 * 1024;
// The official web client answers every further offer on the same
// RTCPeerConnection. Bound bursts rather than the lifetime of a healthy stream.
constexpr std::uint32_t kMaximumRenegotiations = 32;
constexpr auto kRenegotiationWindow = std::chrono::minutes(1);
std::int64_t MonotonicMs(std::chrono::steady_clock::time_point now) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}
// Value of the first SDP attribute line with `prefix` (which includes the
// trailing colon). Bounded; used only to classify a re-offer as a media
// update or a transport change, never logged.
std::string SdpAttributeValue(std::string_view sdp, std::string_view prefix) {
    std::size_t at=0;
    while((at=sdp.find(prefix,at))!=std::string_view::npos) {
        if(at==0 || sdp[at-1]=='\n') {
            const auto begin=at+prefix.size();
            auto end=sdp.find_first_of("\r\n",begin);
            if(end==std::string_view::npos) end=sdp.size();
            if(end-begin>256) return {};
            return std::string(sdp.substr(begin,end-begin));
        }
        at+=prefix.size();
    }
    return {};
}
const char* ControlEvent(ControlChannelPolicy::State state) {
    using State=ControlChannelPolicy::State;
    switch(state) {
    case State::WaitingAssociation:return "RTC_CONTROL_WAITING_ASSOCIATION";
    case State::WaitingChannel:return "RTC_CONTROL_WAITING_CHANNEL";
    case State::Ready:return "RTC_CONTROL_READY";
    case State::Recovering:return "RTC_CONTROL_RECOVERING";
    case State::InitFailed:return "RTC_SCTP_INIT_FAILED";
    case State::AssociationTimeout:return "RTC_SCTP_ASSOCIATION_TIMEOUT";
    case State::ChannelTimeout:return "RTC_DATA_CHANNEL_TIMEOUT";
    case State::Lost:return "RTC_CONTROL_CHANNEL_LOST";
    default:return nullptr;
    }
}

// Minimal protobuf reader for the autotest data-channel trace: returns the
// first varint value of `field`, skipping every other wire type safely.
bool ReadVarintField(std::span<const std::uint8_t> body, std::uint32_t field,
                     std::uint64_t& value) {
    const auto read=[](std::span<const std::uint8_t> data,std::size_t& at,std::uint64_t& out) {
        out=0; unsigned shift=0;
        while (at<data.size() && shift<64) {
            const auto byte=data[at++];
            out|=static_cast<std::uint64_t>(byte&0x7f)<<shift;
            if (!(byte&0x80)) return true;
            shift+=7;
        }
        return false;
    };
    std::size_t i=0;
    while (i<body.size()) {
        std::uint64_t key=0;
        if (!read(body,i,key)) return false;
        const auto number=static_cast<std::uint32_t>(key>>3);
        const auto wire=static_cast<std::uint32_t>(key&7);
        if (wire==0) {
            std::uint64_t v=0;
            if (!read(body,i,v)) return false;
            if (number==field) { value=v; return true; }
        } else if (wire==2) {
            std::uint64_t length=0;
            if (!read(body,i,length) || length>body.size()-i) return false;
            i+=static_cast<std::size_t>(length);
        } else if (wire==1) {
            if (body.size()-i<8) return false;
            i+=8;
        } else if (wire==5) {
            if (body.size()-i<4) return false;
            i+=4;
        } else {
            return false;
        }
    }
    return false;
}

// First length-delimited field `field` of a protobuf message (slices the
// buffer; no copies). Used for the RtcDataChannel feedback submessage.
bool ReadBytesField(std::span<const std::uint8_t> body, std::uint32_t field,
                    std::span<const std::uint8_t>& value) {
    std::size_t i=0;
    while (i<body.size()) {
        std::uint64_t key=0; unsigned shift=0;
        while (i<body.size() && shift<64) {
            const auto byte=body[i++];
            key|=static_cast<std::uint64_t>(byte&0x7f)<<shift;
            if (!(byte&0x80)) break;
            shift+=7;
        }
        const auto number=static_cast<std::uint32_t>(key>>3);
        const auto wire=static_cast<std::uint32_t>(key&7);
        if (wire==0) {
            while (i<body.size() && (body[i++]&0x80)) {}
        } else if (wire==1) {
            if (body.size()-i<8) return false;
            i+=8;
        } else if (wire==5) {
            if (body.size()-i<4) return false;
            i+=4;
        } else if (wire==2) {
            std::uint64_t length=0; shift=0;
            while (i<body.size() && shift<64) {
                const auto byte=body[i++];
                length|=static_cast<std::uint64_t>(byte&0x7f)<<shift;
                if (!(byte&0x80)) break;
                shift+=7;
            }
            if (length>body.size()-i) return false;
            if (number==field) { value=body.subspan(i,static_cast<std::size_t>(length)); return true; }
            i+=static_cast<std::size_t>(length);
        } else {
            return false;
        }
    }
    return false;
}

std::string Num(double value) {
    std::ostringstream out; out<<value; return out.str();
}

std::string HexHead(const std::uint8_t* bytes,std::size_t size,std::size_t max_bytes=12) {
    static const char kHex[]="0123456789ABCDEF";
    const auto n=std::min(size,max_bytes);
    std::string out; out.reserve(n*3);
    for(std::size_t i=0;i<n;++i) {
        if(i) out.push_back(' ');
        out.push_back(kHex[bytes[i]>>4]);
        out.push_back(kHex[bytes[i]&0xf]);
    }
    if(size>n) out+=" ...";
    return out;
}

// Compact scalar view of a protobuf message for the autotest trace: varint and
// fixed fields by number; length-delimited fields only by length (cursor image
// bytes are never copied into a log).
std::string ProtoScalarSummary(std::span<const std::uint8_t> body) {
    std::ostringstream out; int shown=0;
    std::size_t i=0;
    while (i<body.size() && shown<20) {
        std::uint64_t key=0; unsigned shift=0;
        while (i<body.size() && shift<64) {
            const auto byte=body[i++];
            key|=static_cast<std::uint64_t>(byte&0x7f)<<shift;
            if (!(byte&0x80)) break;
            shift+=7;
        }
        const auto number=static_cast<std::uint32_t>(key>>3);
        const auto wire=static_cast<std::uint32_t>(key&7);
        if(shown++) out<<' ';
        if (wire==0) {
            std::uint64_t v=0; shift=0;
            while (i<body.size() && shift<64) {
                const auto byte=body[i++];
                v|=static_cast<std::uint64_t>(byte&0x7f)<<shift;
                if (!(byte&0x80)) break;
                shift+=7;
            }
            out<<number<<'='<<v;
        } else if (wire==1) {
            if (body.size()-i<8) break;
            std::uint64_t raw=0;
            for(int b=0;b<8;++b) raw|=static_cast<std::uint64_t>(body[i+b])<<(8*b);
            double v=0; std::memcpy(&v,&raw,sizeof(v));
            out<<number<<'='<<Num(v);
            i+=8;
        } else if (wire==5) {
            if (body.size()-i<4) break;
            std::uint32_t raw=0;
            for(int b=0;b<4;++b) raw|=static_cast<std::uint32_t>(body[i+b])<<(8*b);
            float v=0; std::memcpy(&v,&raw,sizeof(v));
            out<<number<<'='<<v;
            i+=4;
        } else if (wire==2) {
            std::uint64_t length=0; shift=0;
            while (i<body.size() && shift<64) {
                const auto byte=body[i++];
                length|=static_cast<std::uint64_t>(byte&0x7f)<<shift;
                if (!(byte&0x80)) break;
                shift+=7;
            }
            if (length>body.size()-i) break;
            out<<number<<"=bytes("<<length<<')';
            i+=static_cast<std::size_t>(length);
        } else {
            break;
        }
    }
    return out.str();
}

} // namespace

Session::Session(std::string wss_url, control::StartGame authorization,
                 std::vector<IceServerConfig> ice_servers,
                 Callbacks callbacks,std::vector<std::string> signaling_alternatives,std::vector<DispatchRoute> routes,Clock clock,
                 std::vector<WebControlRoute> web_control_routes,sdk::BridgeContext sdk_bridge)
    : signaling_(wss_url,std::move(signaling_alternatives)), ice_servers_(std::move(ice_servers)),
      callbacks_(std::move(callbacks)), authorization_(std::move(authorization)),routes_(std::move(routes)),clock_(std::move(clock)),
      web_control_([this](const std::uint8_t* bytes,std::size_t size) {
          if(!running_) return;
          if(consume_sdk_packet(bytes,size)) return;
          if(callbacks_.data) callbacks_.data(bytes,size);
      }),web_control_routes_(std::move(web_control_routes)),sdk_context_(std::move(sdk_bridge)) {
    // Bridge diagnostics flow through the bounded candidate-event sink; the
    // context itself never carries credentials into any log.
    sdk_context_.event=[this](const char* event,int code){if(callbacks_.candidate_event) callbacks_.candidate_event(event,code);};
    if (std::ifstream mode_file(opennow::AppHomePath()+"/cloudgenshin-input-channel.txt");
        mode_file.is_open()) {
        std::string mode; mode_file >> mode;
        if (mode == "both") input_channel_mode_ = 1;
        else if (mode == "legacy") input_channel_mode_ = 2;
    }
    // Autotest-only protocol trace: message kinds, function identifiers and
    // reply decisions. Payload values never reach this file.
    if (AutoTestEnabled(opennow::AppHomePath()+"/cloudgenshin-autotest.txt")) {
        sdk_trace_start_=std::chrono::steady_clock::now();
        sdk_trace_log_.open(opennow::AppHomePath()+"/sdk-bridge.log",
                            std::ios::binary|std::ios::trunc);
        if (sdk_trace_log_.is_open()) {
            sdk_context_.trace=[this](const std::string& line) {
                if (!sdk_trace_log_.is_open() || sdk_trace_bytes_>=kSdkTraceLimitBytes) return;
                const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now()-sdk_trace_start_).count();
                std::string entry="[+"+std::to_string(elapsed)+"ms] "+line+'\n';
                sdk_trace_log_ << entry;
                sdk_trace_log_.flush();
                sdk_trace_bytes_ += entry.size();
            };
        }
    }
    signaling_.set_on_rtc_signal([this](const SignalMessage& message) {
        handle_signal(message);
    });
    signaling_.set_on_frame([this](const signaling::Frame& frame) { handle_frame(frame); });
    signaling_.set_on_connect_attempt(callbacks_.connect_attempt);
}

Session::~Session() {
    stop();
}

bool Session::start(const std::atomic_bool* cancelled) {
    stop();
    receive_budget_hits_=0;receive_batch_maximum_=0;
    peer_call_peak_us_=0;peer_slow_calls_=0;
    signal_poll_timings_.reset();peer_drain_timings_.reset();session_tasks_timings_.reset();
    sdk_poll_timings_.reset();control_poll_timings_.reset();
    error_.clear();
    reconnect_.reset();ever_completed_=false;preserve_remote_on_stop_=false;
    session_end_received_ = false;
    // Official web SDK sets startGamingTimeMS = startConnectingTimeMS = now
    // immediately before initRtcConnection(); the elapsed values are written
    // into StartGameReq only once the signaling transport is up.
    const auto connecting_started = std::chrono::steady_clock::now();
    if (ice_servers_.size() > 5) {
        error_ = "Too many ICE servers";
        return false;
    }
    const auto auth_packet = control::EncodeStartGame(authorization_);
    if (auth_packet.empty()) {
        error_ = "Missing game authorization fields";
        return false;
    }
    if (!signaling_.connect(false,cancelled)) {
        error_ = "Could not connect cloud signaling";
        return false;
    }
    running_ = true;
    cancelled_=cancelled;
    sdk_bridge_.emplace(sdk_context_,[this](std::uint16_t command,const std::vector<std::uint8_t>& body) {
        return send_sdk_proxy(command,body);
    });
    for(const auto& route:web_control_routes_) if(route.signaling_url==signaling_.connected_endpoint()) {
        selected_control_urls_.push_back(route.control_url);break;
    }
    // Field diagnosis for the control transport: the dispatch may offer IPv4
    // and IPv6 control endpoints for the same pod. Log every candidate (host
    // and path only, never credentials) so a failing endpoint can be told
    // apart from a missing one.
    if(sdk_context_.trace) {
        const std::string selected=signaling_.connected_endpoint();
        std::size_t logged=0;
        for(const auto& route:web_control_routes_) {
            if(logged++>=8) {
                sdk_context_.trace("control candidate list truncated total="+
                                   std::to_string(web_control_routes_.size()));
                break;
            }
            sdk_context_.trace(std::string("control candidate")+
                (route.signaling_url==selected?" selected":"")+
                " sig="+route.signaling_url.substr(0,160)+
                " ctrl="+route.control_url.substr(0,160));
        }
    }
    for(const auto& route:routes_) if(route.signaling_url==signaling_.connected_endpoint()) {
        selected_route_=route;break;
    }
    if(callbacks_.candidate_event) callbacks_.candidate_event(selected_route_ ?
        "RTC_DISPATCH_ROUTE_READY" : "RTC_DISPATCH_ROUTE_UNAVAILABLE",0);
    // Official StartGameReq timing fields, measured from the same instant:
    // 9 since_start_sec = ceil(elapsed/1000), 10 link_tasks_ms = elapsed.
    {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connecting_started).count();
        const auto since_sec = (std::max<std::int64_t>(elapsed_ms, 0) + 999) / 1000;
        authorization_.has_rtc_connected = false;
        authorization_.since_start_sec = static_cast<std::uint32_t>(since_sec);
        authorization_.link_tasks_ms = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(elapsed_ms, 0, 0xffffffffLL));
    }
    const auto timed_auth_packet = control::EncodeStartGame(authorization_);
    if (timed_auth_packet.empty()) {
        error_ = "Missing game authorization fields";
        return false;
    }
    // The official client authenticates over proxy BEFORE RTC client hello.
    if (!signaling_.send(signaling::MessageType::Proxy, timed_auth_packet)) {
        error_ = "Could not send game authorization";
        stop(); return false;
    }
    auth_sent_ = true;
    deadline_ = clock_() + std::chrono::seconds(20);
    return true;
}

bool Session::initialize_peer(bool send_hello) {
    try { runtime_ = std::make_unique<opennow::webrtc::PeerRuntime>(); }
    catch (...) { error_ = "Could not initialize streaming security runtime"; return false; }
    PeerConfiguration config{};
    config.video_codec = CODEC_H264;
    config.video_reorder_hold_ms = kVideoRecoveryHoldMs;
    config.video_reorder_initial_packets = kVideoRecoveryInitialPackets;
    config.video_reorder_max_packets = kVideoRecoveryMaximumPackets;
    config.audio_codec = CODEC_OPUS;
    // Preserve the negotiated media/SDP layout; types 1/2 route control over
    // GCC instead and must not depend on an SCTP association becoming usable.
    config.datachannel = DATA_CHANNEL_BINARY;
    config.onvideopacket = &Session::OnVideo;
    config.onvideodamage = &Session::OnVideoDamage;
    config.onaudiopacket = &Session::OnAudio;
    config.user_data = this;
    for (std::size_t i = 0; i < ice_servers_.size(); ++i) {
        config.ice_servers[i].urls = ice_servers_[i].url.c_str();
        config.ice_servers[i].username = ice_servers_[i].username.empty()
            ? nullptr : ice_servers_[i].username.c_str();
        config.ice_servers[i].credential = ice_servers_[i].credential.empty()
            ? nullptr : ice_servers_[i].credential.c_str();
    }
    peer_ = peer_connection_create(&config);
    if (!peer_) {
        error_ = "Could not initialize WebRTC peer";
        return false;
    }
    peer_connection_ondatachannel(peer_, &Session::OnData, nullptr, nullptr);
    peer_connection_on_remote_datachannel(peer_, &Session::OnRemoteChannel);
    peer_connection_oniceconnectionstatechange(peer_, &Session::OnPeerState);
    // Verbose SCTP/DTLS file diagnostics are field-analysis only; enable them
    // exactly when the debug autotest marker is present so normal players never
    // pay the file I/O. Diagnostics use only the current application's home.
    const bool diagnostics=AutoTestEnabled(opennow::AppHomePath()+"/cloudgenshin-autotest.txt");
    peer_connection_set_diagnostics_enabled(diagnostics ? 1 : 0);
    if (diagnostics) {
#ifdef _WIN32
        (void)::_mkdir(opennow::AppHomePath().c_str());
#else
        (void)::mkdir(opennow::AppHomePath().c_str(),0777);
#endif
    }
    if (send_hello && !signaling_.send_client_hello()) {
        error_ = "Could not send authenticated RTC hello";
        return false;
    }
    return true;
}

void Session::poll() {
    if (!running_) return;
    if(reconnect_.active() && error_.empty() && !session_end_received_) poll_recovery();
    if(!running_) return;
    receive_backlog_hint_=false;
    const auto drain_media=[this](bool completed_only) {
        if(!running_ || !peer_ || !error_.empty() || session_end_received_ ||
           (completed_only && peer_connection_get_state(peer_)!=PEER_CONNECTION_COMPLETED)) return;
        const auto started=clock_();
        const auto drained=opennow::webrtc::DrainPeerPacketsBudgeted([this] {
            if(!running_ || !peer_ || !error_.empty() || session_end_received_) return 0;
            return peer_connection_loop(peer_);
        },[this] {return std::chrono::duration_cast<std::chrono::microseconds>(clock_().time_since_epoch()).count();});
        receive_backlog_hint_|=drained.exhausted;
        if(drained.exhausted) ++receive_budget_hits_;
        receive_batch_maximum_=std::max(receive_batch_maximum_,drained.packets);
        peer_call_peak_us_=std::max(peer_call_peak_us_,drained.call_max_us);
        peer_slow_calls_+=drained.slow_calls;
        peer_drain_timings_.observe(std::chrono::duration_cast<std::chrono::microseconds>(clock_()-started).count());
    };
    // Catch up established media before control work. A handshake still gets
    // only its normal post-signaling step, not two connectivity checks.
    drain_media(true);
    const auto signal_started=clock_();
    signaling_.poll();
    signal_poll_timings_.observe(std::chrono::duration_cast<std::chrono::microseconds>(clock_()-signal_started).count());
    // Preserve the old handshake-before-deadline ordering. In particular a
    // DTLS completion at the deadline can clear it before timeout evaluation.
    if(peer_ && peer_connection_get_state(peer_)!=PEER_CONNECTION_COMPLETED) drain_media(false);
    const auto tasks_started=clock_();
    if (!sctp_up_reported_ && peer_ && peer_connection_sctp_connected(peer_)) {
        sctp_up_reported_ = true;
        if (callbacks_.candidate_event) callbacks_.candidate_event("RTC_SCTP_ASSOCIATION_UP",1);
    }
    const auto now = clock_();
    if(sdk_bridge_) {
        const auto started=clock_();sdk_bridge_->Poll(MonotonicMs(now));
        sdk_poll_timings_.observe(std::chrono::duration_cast<std::chrono::microseconds>(clock_()-started).count());
    }
    if(authenticated_ && UsesWebControl(web_control_channel_type_) && error_.empty() &&
       !signaling_.reconnecting() && (!reconnect_.active() || recovery_phase_==RecoveryPhase::Verifying)) {
        const auto started=clock_();poll_web_control(now);
        control_poll_timings_.observe(std::chrono::duration_cast<std::chrono::microseconds>(clock_()-started).count());
    }
    if(peer_ && peer_connection_get_state(peer_)==PEER_CONNECTION_COMPLETED && error_.empty()) {
        PeerTransportStats stats{};
        peer_connection_get_transport_stats(peer_,&stats);
        const auto previous=control_.state();
        const auto state=control_.update(MonotonicMs(now),stats.sctp_create_attempted && stats.sctp_create_result<0,
                                         peer_connection_sctp_connected(peer_)!=0,
                                         data_sid_.has_value() || extra_sid_.has_value());
        // WEBCTRL sessions report GCC health through the CONTROL_WSS_* events;
        // the SCTP control policy stays a silent fallback tracker there so a
        // missing association never overwrites the visible status.
        if(state!=previous && !UsesWebControl(web_control_channel_type_) && callbacks_.candidate_event)
            callbacks_.candidate_event(ControlEvent(state),stats.sctp_create_result);
        // The data channel is only a fallback for WEBCTRL sessions: there a
        // missing association degrades control, it never ends the stream.
        if(control_.failed() && !UsesWebControl(web_control_channel_type_) && !reconnect_.active()) error_=ControlEvent(state);
    }
    const auto transport=can_send_input() ?
        (control_via_wss() ? InputTransport::WebControl : InputTransport::Rtc) : InputTransport::Unavailable;
    if(input_transport_.observe(transport) && callbacks_.candidate_event) {
        callbacks_.candidate_event(transport==InputTransport::Rtc ? "INPUT_ROUTE_RTC_SELECTED" :
            transport==InputTransport::WebControl ? "INPUT_ROUTE_WSS_SELECTED" : "INPUT_ROUTE_UNAVAILABLE",0);
    }
    if(transport==InputTransport::Rtc && web_control_.ready() && !native_rtc_route_reported_ &&
       PreferNativeGamepadRtc(sdk_context_.app_id,pad_vendor_id_,pad_product_id_,server_pad_enabled_)) {
        native_rtc_route_reported_=true;
        if(callbacks_.candidate_event) callbacks_.candidate_event("NATIVE_GAMEPAD_RTC_PREFERRED",0);
    }
    if (can_send_input() && now >= next_heartbeat_) {
        const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool first_heartbeat = heartbeat_id_ == 0;
        auto heartbeat = control::EncodeHeartbeat(heartbeat_id_+1, unix_ms);
        if (!heartbeat.empty()) {
            if(send_control_packet(heartbeat)) {
                ++heartbeat_id_;
                last_heartbeat_unix_ms_=unix_ms;
                if (sdk_context_.trace && hb_trace_out_<3) {
                    ++hb_trace_out_;
                    sdk_context_.trace("out rtc hb id="+std::to_string(heartbeat_id_)+
                                       " ts="+std::to_string(unix_ms));
                }
                if (first_heartbeat && callbacks_.candidate_event) callbacks_.candidate_event("CONTROL_HEARTBEAT_STARTED",0);
            }
        }
        next_heartbeat_ = now + std::chrono::seconds(1);
    }
    if (error_.empty() && !signaling_.is_connected() && !reconnect_.active())
        error_ = "Cloud signaling disconnected";
    if (error_.empty() && !reconnect_.active() && deadline_ != std::chrono::steady_clock::time_point{} &&
        now >= deadline_)
        error_ = authenticated_ ? "WebRTC connection timed out" : "Game authorization timed out";
    session_tasks_timings_.observe(std::chrono::duration_cast<std::chrono::microseconds>(clock_()-tasks_started).count());
    if (!error_.empty()) {
        if(begin_recovery()) return;
        if(reconnect_.active() && !session_end_received_) preserve_remote_on_stop_=true;
        report_transport();stop();return;
    }
    // Another bounded turn after SDK/GCC/heartbeat work. Input still gets a
    // turn outside poll(); no additional media owner or unbounded drain loop.
    drain_media(true);
    if (!error_.empty()) {
        if(begin_recovery()) return;
        if(reconnect_.active() && !session_end_received_) preserve_remote_on_stop_=true;
        report_transport();stop();
    }
}

bool Session::begin_recovery() {
    // Never retry authorization denial, malformed protocol, certificate
    // errors, game logout or a user cancellation. A transport must have been
    // established once; initial login/queue failures retain their own UI.
    if(!ever_completed_ || session_end_received_ || (cancelled_ && *cancelled_) ||
       (error_=="Cloud signaling disconnected" && !signaling_.recoverable_disconnect()) ||
       (error_!="Cloud signaling disconnected" && error_!="WebRTC connection lost" &&
        error_!="RTC_CONTROL_CHANNEL_LOST")) return false;
    if(reconnect_.active()) {error_.clear();retry_recovery();return true;}
    const int reason=error_=="Cloud signaling disconnected" ? 1 : error_=="WebRTC connection lost" ? 2 : 3;
    if(!reconnect_.begin(MonotonicMs(clock_()))) {fail_recovery();return false;}
    error_.clear();resume_auth_pending_=false;recovery_media_received_=false;
    recovery_replace_peer_=reason==3 || !peer_ || peer_connection_get_state(peer_)!=PEER_CONNECTION_COMPLETED;
    // Stop stale GCC state without StopGameReq. The existing peer and SDK
    // reliable queue are retained until the pod tells us whether to replace
    // the media transport. A resumed input route is re-announced by the view.
    web_control_.stop();web_control_started_=web_control_reported_=web_control_failed_=false;
    control_session_.reset();control_handshake_received_=false;
    idle_gamepad_.invalidate();recovery_phase_=RecoveryPhase::Waiting;deadline_={};
    if(callbacks_.candidate_event) {
        callbacks_.candidate_event("STREAM_RECONNECT_CAUSE",reason);
        callbacks_.candidate_event("STREAM_RECONNECTING",reason);
    }
    return true;
}

void Session::retry_recovery() {
    signaling_.disconnect();resume_auth_pending_=false;recovery_media_received_=false;
    web_control_.stop();web_control_started_=web_control_reported_=web_control_failed_=false;
    control_session_.reset();control_handshake_received_=false;
    recovery_replace_peer_|=recovery_phase_==RecoveryPhase::Verifying || !peer_ ||
        peer_connection_get_state(peer_)!=PEER_CONNECTION_COMPLETED;
    recovery_phase_=RecoveryPhase::Waiting;reconnect_.retry(MonotonicMs(clock_()));
    if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECT_RETRY",reconnect_.attempts());
}
void Session::fail_recovery() {
    // Preserve the allocation for a later user retry, rather than turning a
    // local timeout into a server-side game exit.
    error_="Automatic stream recovery exhausted";preserve_remote_on_stop_=true;
    if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECT_EXHAUSTED",reconnect_.attempts());
}
void Session::reset_recovery_peer() {
    peer_resetting_=true;
    if(peer_) {peer_connection_close(peer_);peer_connection_destroy(peer_);peer_=nullptr;}
    runtime_.reset();peer_resetting_=false;
    data_sid_.reset();extra_sid_.reset();control_.reset();sctp_up_reported_=false;
    pending_candidates_.clear();seen_candidates_.clear();answer_sent_=false;
    local_answer_sdp_.clear();remote_ice_ufrag_.clear();remote_fingerprint_.clear();renegotiations_.clear();
    if(callbacks_.video_damage) {PeerVideoDamage damage{};callbacks_.video_damage(damage);}
}
void Session::poll_recovery() {
    const auto now=clock_();const auto ms=MonotonicMs(now);
    if(cancelled_ && *cancelled_) return;
    if(reconnect_.timed_out(ms)) {fail_recovery();report_transport();stop();return;}
    if(recovery_phase_==RecoveryPhase::Waiting) {
        if(reconnect_.no_attempts()) {fail_recovery();report_transport();stop();return;}
        if(!reconnect_.due(ms)) return;
        reconnect_.attempted();
        recovery_attempt_started_=now;
        if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECT_ATTEMPT",reconnect_.attempts());
        if(!signaling_.begin_reconnect()) {retry_recovery();return;}
        recovery_phase_=RecoveryPhase::Connecting;
    }
    if(recovery_phase_==RecoveryPhase::Connecting) {
        const auto result=signaling_.poll_reconnect();
        if(result==SignalingClient::ReconnectResult::Pending) return;
        if(result!=SignalingClient::ReconnectResult::Connected) {
            // HTTP refusal / TLS failure is not a reason to bypass verification
            // or keep retrying the same rejected endpoint.
            const auto& diagnostic=signaling_.connect_diagnostics();
            if(!diagnostic.may_try_alternative() || diagnostic.curl_code==CURLE_PEER_FAILED_VERIFICATION ||
               diagnostic.curl_code==CURLE_SSL_CACERT_BADFILE || diagnostic.curl_code==CURLE_SSL_PINNEDPUBKEYNOTMATCH) {
                fail_recovery();report_transport();stop();return;
            }
            retry_recovery();return;
        }
        selected_control_urls_.clear();selected_route_.reset();
        for(const auto& route:web_control_routes_) if(route.signaling_url==signaling_.connected_endpoint()) {
            selected_control_urls_.push_back(route.control_url);break;
        }
        for(const auto& route:routes_) if(route.signaling_url==signaling_.connected_endpoint()) {selected_route_=route;break;}
        auto resume=authorization_;
        resume.has_rtc_connected=!recovery_replace_peer_ && peer_ &&
            peer_connection_get_state(peer_)==PEER_CONNECTION_COMPLETED;
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-recovery_attempt_started_).count();
        resume.link_tasks_ms=static_cast<std::uint32_t>(std::clamp<std::int64_t>(elapsed,0,0xffffffffLL));
        resume.since_start_sec=static_cast<std::uint32_t>((static_cast<std::uint64_t>(resume.link_tasks_ms)+999)/1000);
        if(!signaling_.send(signaling::MessageType::Proxy,control::EncodeStartGame(resume))) {retry_recovery();return;}
        resume_auth_pending_=true;recovery_phase_=RecoveryPhase::Authorizing;
        recovery_step_deadline_=now+std::chrono::seconds(5);
        if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECT_TRANSPORT_READY",reconnect_.attempts());
    }
    if(recovery_phase_==RecoveryPhase::Authorizing || recovery_phase_==RecoveryPhase::Verifying) {
        if(!signaling_.is_connected()) {
            if(!signaling_.recoverable_disconnect()) {
                error_="Invalid cloud signaling during recovery";preserve_remote_on_stop_=true;
                report_transport();stop();return;
            }
            retry_recovery();return;
        }
        if(now>=recovery_step_deadline_) {retry_recovery();return;}
        if(recovery_phase_==RecoveryPhase::Verifying && recovery_media_received_ &&
           (!callbacks_.decoded_frames || callbacks_.decoded_frames()>recovery_decoded_baseline_) && peer_ &&
           peer_connection_get_state(peer_)==PEER_CONNECTION_COMPLETED &&
           (control_via_wss() || rtc_input_ready())) {
            reconnect_.recovered(ms);idle_gamepad_.invalidate();deadline_={};
            if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECTED",reconnect_.attempts());
            (void)request_keyframe();
        }
    }
}

void Session::web_control_fail(const char* reason) {
    if(!web_control_failed_) {
        web_control_failed_=true;
        if(callbacks_.candidate_event) {
            callbacks_.candidate_event(reason && *reason ? reason : "CONTROL_WSS_CONNECT_FAILED",0);
            callbacks_.candidate_event("CONTROL_WSS_FALLBACK_DATA_CHANNEL",0);
        }
        // The official web client calls resetInput() from onDisConnected, which
        // only runs once the game control channel had been alive (its KCP sync
        // completed). A channel that never connected is never reset, so the pod
        // only sees ResetInputsReq after a real drop. Same message over the
        // signaling proxy channel.
        if(running_ && authenticated_ && web_control_reported_ && !reset_inputs_sent_ &&
           input_transport_.kind()==InputTransport::WebControl) {
            const auto packet=control::EncodeResetInputs(authorization_.game_token);
            if(!packet.empty() && signaling_.send(signaling::MessageType::Proxy,packet)) {
                reset_inputs_sent_=true;
                if(callbacks_.candidate_event)
                    callbacks_.candidate_event("CONTROL_WSS_RESET_INPUTS_SENT",0);
            }
        }
    }
    web_control_.stop();
}

void Session::poll_web_control(std::chrono::steady_clock::time_point now) {
    if(web_control_failed_) return;
    if(!web_control_started_) {
        if(!control_session_) {
            if(control_handshake_received_) {web_control_fail("CONTROL_WSS_HANDSHAKE_INVALID");return;}
            if(now>=control_handshake_deadline_) {web_control_fail("CONTROL_WSS_HANDSHAKE_TIMEOUT");return;}
            return;
        }
        web_control_started_=true;
        if(callbacks_.candidate_event) callbacks_.candidate_event("CONTROL_WSS_CONNECTING",0);
        const bool connected=web_control_.start(selected_control_urls_,*control_session_,now,cancelled_);
        if(callbacks_.candidate_event) {
            const auto& diagnostic=web_control_.connect_diagnostics();
            callbacks_.candidate_event("CONTROL_WSS_ENDPOINT_PORT",diagnostic.endpoint_port);
            callbacks_.candidate_event("CONTROL_WSS_HTTP_STATUS",static_cast<int>(diagnostic.http_status));
            callbacks_.candidate_event("CONTROL_WSS_TRANSPORT_RESULT",diagnostic.curl_code);
        }
        // Opt-in autotest trace: endpoint (query stripped) and the bounded
        // transport detail, so a failed TLS handshake can be diagnosed. The
        // session id, tokens and response bodies never reach this line.
        if(sdk_context_.trace) {
            const auto& diagnostic=web_control_.connect_diagnostics();
            std::string line="control endpoint="+web_control_.attempted_url();
            if(!diagnostic.endpoint_host.empty()) line+=" host="+diagnostic.endpoint_host;
            if(!diagnostic.detail.empty()) line+=" detail="+diagnostic.detail;
            sdk_context_.trace(line);
        }
        if(!connected) {web_control_fail(web_control_.error());return;}
        if(callbacks_.candidate_event) callbacks_.candidate_event("CONTROL_WSS_WAITING_SYNC",0);
    }
    web_control_.poll(now);
    if(web_control_.error()) {web_control_fail(web_control_.error());return;}
    if(web_control_.ready() && !web_control_reported_) {
        web_control_reported_=true;
        if(callbacks_.candidate_event) callbacks_.candidate_event("CONTROL_WSS_READY",0);
    }
}

void Session::stop() {
    signaling_.cancel_reconnect();
    running_ = false;
    receive_backlog_hint_=false;
    // Do not echo a StopGameReq after the pod already ended the session with
    // StopGameRsp (20005); that is a server-initiated end, not a local stop.
    if (auth_sent_ && signaling_.is_connected() && !session_end_received_ && !preserve_remote_on_stop_) {
        signaling_.send(signaling::MessageType::Proxy,
                         control::EncodeStopGame(authorization_.game_token));
        signaling_.poll(); // bounded flush; no blocking wait for StopGameRsp
    }
    auth_sent_ = false;
    reconnect_.reset();resume_auth_pending_=false;recovery_media_received_=false;
    web_control_.stop();
    control_session_.reset();
    selected_control_urls_.clear();
    control_handshake_received_=web_control_started_=web_control_reported_=web_control_failed_=false;
    reset_inputs_sent_=false;
    control_handshake_deadline_={};cancelled_=nullptr;
    signaling_.disconnect();
    if (peer_) {
        peer_connection_close(peer_);
        peer_connection_destroy(peer_);
        peer_ = nullptr;
        peer_connection_set_diagnostics_enabled(0);
    }
    runtime_.reset();
    data_sid_.reset();
    extra_sid_.reset();
    sctp_up_reported_ = false;
    control_.reset();
    input_transport_.reset();server_pad_enabled_=false;native_rtc_route_reported_=false;
    pending_candidates_.clear();
    seen_candidates_.clear();
    seen_proxy_commands_.clear();
    sdk_bridge_.reset();
    had_gamepad_activity_ = false;
    idle_gamepad_.invalidate();idle_gamepad_suppression_reported_=false;
    native_pad_trace_=native_touch_trace_=0;
    traced_gamepad_={};
    motion_={};
    orientation_sent_=0;
    motion_feedback_received_=motion_feedback_invalid_=motion_feedback_before_auth_=0;
    motion_activation_received_=0;
    server_control_mode_ = -1;
    selected_route_.reset();
    answer_sent_ = false;
    local_answer_sdp_.clear();
    remote_ice_ufrag_.clear();
    remote_fingerprint_.clear();
    renegotiations_.clear();
    authenticated_ = false;
    xbox_enabled_ = false;
    ps_enabled_ = false;
    gr0006_enabled_ = false;
    web_control_channel_type_ = 0;
    deadline_ = {};
    next_heartbeat_ = {};
    heartbeat_id_ = 0;
    last_heartbeat_unix_ms_ = 0;
    hb_trace_out_ = hb_trace_in_ = in_trace_count_ = 0;
    hb_in_count_ = hb_in_match_ = hb_in_last_id_ = 0;
    hb_summary_next_ms_ = 0;
    out_input_trace_ = 0;
    out_pad_trace_ = 0;gameplay_input_trace_=false;
}

void Session::handle_frame(const signaling::Frame& frame) {
    if (!running_ || !error_.empty()) return;
    if(frame.type==signaling::MessageType::Handshake) {
        const auto id=ParseControlSession(frame.payload);
        // A regenerated id replaces control for a newer session; keeping the
        // original one can only fail the GCC, which is optional anyway.
        if(control_session_ && id!=control_session_) return;
        control_handshake_received_=true;control_session_=id;
        return;
    }
    if(frame.type!=signaling::MessageType::Proxy) return;
    const auto packet = control::DecodePacket(frame.payload);
    if (!packet) { error_ = "Invalid cloud control packet"; return; }
    // The official proxy channel also carries the in-game SDK bridge traffic
    // (cmd 1900 reliable queue and the 20014 idle tip); hand it to the bridge
    // before the generic dispatch. Server-initiated StopGameRsp is handled
    // first so the SDK bridge cannot hide an explicit session end.
    if (packet->command == control::kStopGameRsp) {
        error_ = "StopGameRsp received";
        session_end_received_ = true;
        if (callbacks_.candidate_event)
            callbacks_.candidate_event("GAME_SDK_SESSION_ENDED",0);
        return;
    }
    if (consume_motion_feedback(*packet)) return;
    if (sdk_bridge_ && sdk_bridge_->HandleProxyPacket(packet->command,packet->body,MonotonicMs(clock_())))
        return;
    // The CA reports the pod's active input adapter through SyncGameControlMode
    // (server->client, CmdId 20037). The official native client logs
    // "sync game control mode: %d" and forwards {"control_mode":"gamepad"} to
    // its UI; here the value becomes a live receipt of whether the pod accepted
    // controller input.
    if (packet->command == control::kSyncGameControlMode) {
        const auto mode = control::DecodeSyncGameControlMode(packet->body);
        if (!mode) {
            if (sdk_context_.trace) sdk_context_.trace("in rtc sync game control mode malformed");
            return;
        }
        if (*mode != server_control_mode_) {
            server_control_mode_ = *mode;
            if (callbacks_.candidate_event)
                callbacks_.candidate_event("SERVER_GAME_CONTROL_MODE", *mode);
        }
        if (sdk_context_.trace) sdk_context_.trace("in rtc control mode=" + std::to_string(*mode));
        return;
    }
    if (packet->command != control::kStartGameRsp) {
        // A stalled game UI may be waiting on an unimplemented game/SDK
        // exchange. Record only bounded unique command numbers, never bodies.
        if(seen_proxy_commands_.size()<16 &&
           std::find(seen_proxy_commands_.begin(),seen_proxy_commands_.end(),packet->command)==seen_proxy_commands_.end()) {
            seen_proxy_commands_.push_back(packet->command);
            if(callbacks_.candidate_event) callbacks_.candidate_event("GAME_CONTROL_MESSAGE_RECEIVED",packet->command);
        }
        return;
    }
    if (authenticated_ && !resume_auth_pending_) return; // duplicate response must not recreate the peer
    const auto result = control::DecodeStartResult(packet->body);
    if (!result) { error_ = "Invalid game authorization response"; return; }
    if (!result->accepted()) {
        if (callbacks_.auth_rejected) callbacks_.auth_rejected(result->retcode);
        error_ = "Game authorization rejected (code " + std::to_string(result->retcode) + ")";
        return;
    }
    if(resume_auth_pending_) {
        resume_auth_pending_=false;
        web_control_channel_type_=result->control_channel_type;
        control_handshake_deadline_=clock_()+std::chrono::seconds(10);
        recovery_phase_=RecoveryPhase::Verifying;recovery_media_received_=false;
        recovery_decoded_baseline_=callbacks_.decoded_frames ? callbacks_.decoded_frames() : 0;
        recovery_step_deadline_=clock_()+std::chrono::seconds(9);
        if(recovery_replace_peer_) {reset_recovery_peer();if(!initialize_peer()) return;}
        else if(!signaling_.send_client_hello()) {retry_recovery();return;}
        // This only clears held state; it is not a gameplay remap or an input
        // mode switch. The view sends device/list/neutral again when ready.
        (void)signaling_.send(signaling::MessageType::Proxy,control::EncodeResetInputs(authorization_.game_token));
        (void)request_keyframe();
        if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECT_AUTHENTICATED",reconnect_.attempts());
        return;
    }
    authenticated_ = true;
    // Force the Xbox virtual pad on regardless of CaGameConfig. The official
    // client only uses gamepad_enable_xbox/ps as a local gate: ClientCore's
    // startGamepad() starts the Java GamepadManager and no client-to-pod
    // message carries those flags. The pod builds its virtual pad from the
    // RtcGamepadList announcement, so the forced route sends that list and
    // type-17 state frames anyway. Whether the pod's input adapter accepts
    // them is server-side and unverified; GAMEPAD_CHANNEL_SEND_OK_NOT_DELIVERY_PROOF
    // and GAMEPAD_SERVER_MODE_NOT_REPORTED_20S record what actually happened.
    xbox_enabled_ = true;
    ps_enabled_ = false;
    gr0006_enabled_ = false;
    server_pad_enabled_=result->xbox_enabled || result->ps_enabled;
    web_control_channel_type_ = result->control_channel_type;
    control_handshake_deadline_=clock_()+std::chrono::seconds(15);
    deadline_ = clock_() + std::chrono::seconds(30);
    if (initialize_peer() && callbacks_.authenticated) callbacks_.authenticated(*result);
}

bool Session::request_keyframe() {
    if(!running_ || !peer_ || !authenticated_ || peer_connection_get_state(peer_)!=PEER_CONNECTION_COMPLETED)
        return false;
    // The official client requests IDR through KeyFrameReq (20032), not just
    // RTCP PLI. Keep PLI as best effort, but report success only for the proxy.
    const bool sent=signaling_.send(signaling::MessageType::Proxy,control::EncodeKeyframeRequest());
    (void)peer_connection_request_video_keyframe(peer_);
    return sent;
}

std::string Session::last_error() const {
    if (!error_.empty()) return error_;
    const auto signaling_err = signaling_.last_error();
    // Signal: "StopGameRsp received" is a server-initiated session end.
    if (signaling_err == "StopGameRsp received") return "StopGameRsp received";
    return signaling_err.empty() ? std::string{} : signaling_err;
}

const char* Session::last_error_event() const {
    for(const char* event:{"CONTROL_WSS_HANDSHAKE_INVALID","CONTROL_WSS_HANDSHAKE_TIMEOUT",
        "CONTROL_WSS_ENDPOINT_MISSING","CONTROL_WSS_CONNECT_FAILED","CONTROL_WSS_SEND_FAILED",
        "CONTROL_WSS_PROTOCOL_INVALID","CONTROL_WSS_SESSION_MISMATCH","CONTROL_WSS_DISCONNECTED",
        "CONTROL_WSS_SYNC_TIMEOUT","CONTROL_WSS_PONG_TIMEOUT"}) if(error_==event) return event;
    for(const auto state:{ControlChannelPolicy::State::InitFailed,ControlChannelPolicy::State::AssociationTimeout,
                         ControlChannelPolicy::State::ChannelTimeout,ControlChannelPolicy::State::Lost}) {
        const auto* event=ControlEvent(state);if(error_==event) return event;
    }
    if (error_=="StopGameRsp received") return "GAME_SDK_SESSION_ENDED";
    if (error_=="Could not connect cloud signaling") return signaling_.connect_diagnostics().event();
    if (error_=="Could not send game authorization") return "GAME_AUTH_SEND_FAILED";
    if (error_=="Game authorization timed out") return "GAME_AUTH_TIMEOUT";
    if (error_.rfind("Game authorization rejected (code ",0)==0) return "GAME_AUTH_REJECTED";
    if (error_=="Invalid cloud control packet") return "GAME_CONTROL_PACKET_INVALID";
    if (error_=="Invalid game authorization response") return "GAME_AUTH_RESPONSE_INVALID";
    if (error_=="Cloud signaling disconnected") return "RTC_SIGNALING_DISCONNECTED";
    if (error_=="Automatic stream recovery exhausted") return "STREAM_RECONNECT_EXHAUSTED";
    if (error_=="Invalid cloud signaling during recovery") return "SIGNALING_PROTOCOL_INVALID";
    if (error_=="WebRTC connection timed out") return "RTC_CONNECT_TIMEOUT";
    if (error_=="WebRTC connection lost") return "RTC_CONNECTION_LOST";
    if (error_=="WebRTC certificate verification failed") return "RTC_DTLS_CERTIFICATE_VERIFY_FAILED";
    if (error_=="Could not initialize streaming security runtime") return "RTC_SECURITY_INIT_FAILED";
    if (error_=="Could not initialize WebRTC peer") return "RTC_PEER_INIT_FAILED";
    if (error_=="Could not send authenticated RTC hello") return "RTC_HELLO_SEND_FAILED";
    if (error_=="Could not send WebRTC answer") return "RTC_ANSWER_SEND_FAILED";
    if (error_=="Unsupported cloud media offer") return "RTC_SDP_UNSUPPORTED";
    if (error_=="No local ICE candidate") return "RTC_LOCAL_CANDIDATE_UNAVAILABLE";
    if (error_=="Cloud renegotiation is not supported") return "RTC_RENEGOTIATION_UNSUPPORTED";
    if (error_=="Too many cloud renegotiations") return "RTC_RENEGOTIATION_LIMIT_EXCEEDED";
    if (error_=="Too many early ICE candidates") return "RTC_CANDIDATE_LIMIT_EXCEEDED";
    if (error_=="Invalid remote ICE candidate") return "RTC_CANDIDATE_INVALID";
    if (error_=="Remote ICE candidate rejected") return "RTC_CANDIDATE_REJECTED";
    if (error_=="Too many ICE servers") return "RTC_ICE_SERVER_LIMIT_EXCEEDED";
    if (error_=="Missing game authorization fields") return "GAME_AUTH_FIELDS_MISSING";
    return "RTC_TRANSPORT_OR_RUNTIME_FAILED";
}

bool Session::rtc_input_ready() const {
    return peer_ && (data_sid_ || extra_sid_) && control_.ready() && peer_connection_sctp_connected(peer_);
}

bool Session::control_via_wss() const {
    return SelectInputTransport(UsesWebControl(web_control_channel_type_) && web_control_.ready(),
        rtc_input_ready(),PreferNativeGamepadRtc(sdk_context_.app_id,pad_vendor_id_,pad_product_id_,server_pad_enabled_))
        ==InputTransport::WebControl;
}

bool Session::can_send_input() const {
    if(!running_ || reconnect_.active() || !error_.empty() || !authenticated_ || !peer_ ||
       peer_connection_get_state(peer_)!=PEER_CONNECTION_COMPLETED) return false;
    // Official GCC preference stays intact except for ys's forced Xbox route,
    // which uses its proven RTC path once ready. GCC remains a fallback; TLS
    // verification and all input packet formats are unchanged.
    if(control_via_wss()) return true;
    return rtc_input_ready();
}

bool Session::send_packet_on_sid(std::vector<std::uint8_t>& packet, std::uint16_t sid) {
    return peer_connection_datachannel_send_binary_sid(peer_,
        reinterpret_cast<char*>(packet.data()),packet.size(),sid)>=0;
}

bool Session::send_control_packet(std::vector<std::uint8_t>& packet) {
    if(!can_send_input() || packet.empty()) return false;
    if(control_via_wss()) return web_control_.send(packet);
    if(!data_sid_ && input_channel_mode_!=2) return false;
    if(input_channel_mode_==2 && extra_sid_) return send_packet_on_sid(packet,*extra_sid_);
    if(!data_sid_) return false;
    return send_packet_on_sid(packet,*data_sid_);
}

bool Session::send_channel_message(const std::vector<std::uint8_t>& channel_message, bool user_activity,
                                   std::uint64_t message_trace_id) {
    if (!can_send_input()) return false;
    auto packet = input::EncodeInputPacket(channel_message,message_trace_id);
    if (packet.empty()) return false;
    bool sent = false;
    if (control_via_wss()) {
        sent = web_control_.send(packet);
    } else if (input_channel_mode_ == 2 && extra_sid_) {
        sent = send_packet_on_sid(packet, *extra_sid_);
    } else if (data_sid_) {
        sent = send_packet_on_sid(packet, *data_sid_);
        // Diagnostic comparison: the CA's legacy-PPID channel received the
        // same official frame, so one run shows which channel it consumes.
        if (input_channel_mode_ == 1 && extra_sid_ && *extra_sid_ != *data_sid_)
            sent = send_packet_on_sid(packet, *extra_sid_) || sent;
    }
    if (!sent) return false;
    // A delivered local input is the page's onAnyAction: it answers a pending
    // idle tip and arms the throttled keep-playing.
    if (user_activity && sdk_bridge_) sdk_bridge_->OnLocalInput(MonotonicMs(clock_()));
    return true;
}

bool Session::send_sdk_proxy(std::uint16_t command, const std::vector<std::uint8_t>& body) {
    // The official bridge replies with sendTcpMessage (= signaling sendProxy);
    // it never rides the input data channel.
    if (!running_) return false;
    control::Packet packet;packet.command=command;packet.body=body;
    const auto encoded = control::EncodePacket(packet);
    // Never report an SDK reply as sent when packet validation rejected it.
    return !encoded.empty() && signaling_.send(signaling::MessageType::Proxy,encoded);
}

bool Session::consume_sdk_packet(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || !size) return false;
    const auto packet = control::DecodePacket(std::span(bytes,size));
    if (!packet) return false;
    if (consume_motion_feedback(*packet)) return true;
    // Some pods deliver StopGameRsp on the RTC/web-control data channel rather
    // than the signaling proxy. Handle it before the SDK bridge so it cannot be
    // mistaken for an unknown bridge message.
    if (packet->command == control::kStopGameRsp) {
        error_ = "StopGameRsp received";
        session_end_received_ = true;
        if (callbacks_.candidate_event)
            callbacks_.candidate_event("GAME_SDK_SESSION_ENDED",0);
        return true;
    }
    if (!sdk_bridge_) return false;
    return sdk_bridge_->HandleProxyPacket(packet->command,packet->body,MonotonicMs(clock_()));
}

bool Session::consume_motion_feedback(const control::Packet& packet) {
    if(packet.command!=input::kRtcDataChannelCommand) return false;
    const auto enabled=control::DecodeImuFeedback(packet.body);
    if(!enabled) {
        std::uint64_t type=0;
        if(ReadVarintField(packet.body,1,type) && type==23) ++motion_feedback_invalid_;
        return false;
    }
    ++motion_feedback_received_;
    if(!authenticated_ || !running_ || session_end_received_) {++motion_feedback_before_auth_;return true;}
    if(*enabled) ++motion_activation_received_;
    const bool changed=motion_.enabled()!=*enabled;
    motion_.enable(*enabled);
    if(changed && callbacks_.candidate_event)
        callbacks_.candidate_event("MOTION_SERVER_REQUESTED",*enabled?1:0);
    return true;
}

bool Session::send_motion(const input::MotionSample& sample,std::int64_t monotonic_ms) {
    if(!can_send_input() || !motion_.admit(sample,monotonic_ms,16)) return false;
    const bool sent=send_channel_message(input::EncodeImuChannel(sample),false);
    if(sent) {
        motion_.sent(sample);
        if(sample.orientation) ++orientation_sent_;
    } else motion_.failed();
    return sent;
}

void Session::trace_incoming_data(const std::uint8_t* bytes, std::size_t size) {
    if (!sdk_context_.trace || !bytes || !size) return;
    constexpr int kInTraceCap = 48;
    const auto packet = control::DecodePacket(std::span(bytes,size));
    if (!packet) {
        // A message the framing layer could not decode is the one case where
        // the wrapper bytes matter: keep a bounded head so the autotest can
        // tell a foreign frame from a truncated one.
        if (in_trace_count_>=kInTraceCap) return;
        ++in_trace_count_;
        sdk_context_.trace("in rtc undecoded len="+std::to_string(size)+
                           " head="+HexHead(bytes,size));
        return;
    }
    if (packet->command == 20029) {
        std::uint64_t id=0, ts=0;
        const bool has_id=ReadVarintField(packet->body,1,id);
        const bool has_ts=ReadVarintField(packet->body,2,ts);
        const bool match=has_id && has_ts && id==heartbeat_id_ &&
            ts==static_cast<std::uint64_t>(last_heartbeat_unix_ms_);
        ++hb_in_count_;
        if (match) ++hb_in_match_;
        hb_in_last_id_=id;
        // Show the first few echoes in full, then a 30s summary: enough to
        // prove the channel still carries app data after a DTLS restart
        // without flooding the bounded trace.
        if (hb_trace_in_<6) {
            ++hb_trace_in_;
            hb_summary_next_ms_=MonotonicMs(std::chrono::steady_clock::now())+30000;
            sdk_context_.trace("in rtc hb id="+std::to_string(id)+" ts="+std::to_string(ts)+
                               " sent_id="+std::to_string(heartbeat_id_)+
                               " sent_ts="+std::to_string(last_heartbeat_unix_ms_)+
                               " match="+(match?"1":"0"));
            return;
        }
        const auto now_ms=MonotonicMs(std::chrono::steady_clock::now());
        if (now_ms>=hb_summary_next_ms_) {
            hb_summary_next_ms_=now_ms+30000;
            const auto unix_now=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            sdk_context_.trace("in rtc hb sum n="+std::to_string(hb_in_count_)+
                               " match="+std::to_string(hb_in_match_)+
                               " last_id="+std::to_string(id)+
                               " age_ms="+std::to_string(unix_now-static_cast<std::int64_t>(ts))+
                               " cur_id="+std::to_string(heartbeat_id_));
        }
        return;
    }
    if (in_trace_count_>=kInTraceCap) return;
    ++in_trace_count_;
    if (packet->command == 20010) {
        std::uint64_t type=0;
        ReadVarintField(packet->body,1,type);
        std::string extra;
        if (type==11) {
            // RtcMouseFeedback (official web DataType.MOUSE_FEEDBACK) sits in
            // field 4 of RtcDataChannel (field 1 type, 2 keyboard, 3 mouse,
            // 4 mouse_feedback). Scalar fields only; the cursor image is
            // reported by length, never copied into the trace.
            std::span<const std::uint8_t> feedback;
            if (ReadBytesField(packet->body,4,feedback))
                extra=" fb{"+ProtoScalarSummary(feedback)+"}";
        }
        sdk_context_.trace("in rtc data type="+std::to_string(type)+
                           " len="+std::to_string(size)+
                           " head="+HexHead(bytes,size)+extra);
    } else {
        sdk_context_.trace("in rtc cmd="+std::to_string(packet->command)+
                           " len="+std::to_string(size)+
                           " head="+HexHead(bytes,size));
    }
}

bool Session::send_gamepad(const input::SwitchControllerState& state,
                            std::int64_t monotonic_ms,bool force) {
    // The official client's gate is local: CaGameConfig gamepad_enable_* only
    // decides whether the Java GamepadManager starts, and the pod never
    // receives those flags. This forced route therefore sends type-17 frames
    // regardless. The launcher is pointer-only by design, so only in-game
    // movement proves the pod created and honoured the virtual pad.
    if (!gamepad_enabled() || !can_send_input()) return false;
    const auto gamepad = input::ToGamepadState(state, monotonic_ms,
                                               pad_vendor_id_, pad_product_id_);
    const bool active = input::HasGamepadActivity(gamepad);
    const bool idle_change_only=input::SuppressIdleGamepadReports(sdk_context_.app_id,authorization_.terminal_type);
    const auto epoch=input_transport_.generation();
    if(idle_change_only && !idle_gamepad_.needs_report(active,epoch,force)) {
        if(!idle_gamepad_suppression_reported_) {
            idle_gamepad_suppression_reported_=true;
            if(callbacks_.candidate_event) callbacks_.candidate_event("GAMEPAD_IDLE_REPORT_SUPPRESSION_ACTIVE",0);
        }
        return true; // Not a new delivery; the same neutral is already queued.
    }
    // A release is an action too, but repeated neutral reports are not.
    // APK onGamepadPressed sets field 18 using its gamepad metric counter.
    const auto trace=idle_change_only ? (native_pad_trace_==UINT64_MAX ? 1 : native_pad_trace_+1) : 0;
    const bool sent = send_channel_message(input::EncodeGamepadChannel(gamepad),
                                           active || had_gamepad_activity_,trace);
    if (sent) {
        // Diagnostic-only and edge-bounded, not a per-frame gameplay log.
        // Observe the final encoded state, after UI/neutral/gyro composition.
        const std::array<std::int64_t,7> signature{gamepad.buttons,gamepad.left_trigger,gamepad.right_trigger,
            gamepad.left_thumb_x,gamepad.left_thumb_y,gamepad.right_thumb_x,gamepad.right_thumb_y};
        const int trace_cap=gameplay_input_trace_?64:32;
        if(sdk_context_.trace && out_pad_trace_<trace_cap && signature!=traced_gamepad_ &&
            (active || had_gamepad_activity_)) {
            ++out_pad_trace_;
            sdk_context_.trace("out rtc pad buttons="+std::to_string(gamepad.buttons)+
                " lx="+std::to_string(gamepad.left_thumb_x)+" ly="+std::to_string(gamepad.left_thumb_y)+
                " rx="+std::to_string(gamepad.right_thumb_x)+" ry="+std::to_string(gamepad.right_thumb_y)+
                " trace="+std::to_string(trace));
        }
        traced_gamepad_=signature;
        had_gamepad_activity_ = active;idle_gamepad_.delivered(active,epoch);native_pad_trace_=trace;
    }
    return sent;
}

bool Session::send_touch(const std::vector<input::TouchEvent>& events) {
    if (events.empty() || events.size() > input::kTouchEventListLimit) return false;
    // The pod's launcher only understands pointer input; the touch list itself
    // carries the mode (the official client sets no input_control_type here).
    const bool native=sdk_context_.app_id=="4" && authorization_.terminal_type==3;
    const auto trace=native ? (native_touch_trace_==UINT64_MAX ? 1 : native_touch_trace_+1) : 0;
    const bool sent = send_channel_message(input::EncodeTouchEventListChannel(events),true,trace);
    if(sent) native_touch_trace_=trace;
    if (sent && sdk_context_.trace && out_input_trace_ < 32) {
        ++out_input_trace_;
        sdk_context_.trace("out rtc touch n=" + std::to_string(events.size()) +
                           " f0=" + std::to_string(events.front().finger_id) +
                           " a0=" + std::to_string(events.front().action)+
                           " trace="+std::to_string(trace));
    }
    return sent;
}

bool Session::send_keyboard(std::uint32_t key_code, bool down) {
    if (!key_code) return false;
    const bool sent=send_channel_message(input::EncodeKeyboardChannel(key_code, down));
    if (sent && sdk_context_.trace && out_input_trace_<32) {
        ++out_input_trace_;
        sdk_context_.trace("out rtc key code="+std::to_string(key_code)+
                           " down="+(down?"1":"0"));
    }
    return sent;
}

bool Session::send_mouse_move(double delta_x, double delta_y) {
    return send_channel_message(input::EncodeMouseMoveChannel(delta_x, delta_y));
}

bool Session::send_mouse_move(double delta_x, double delta_y,
                              double cursor_x, double cursor_y) {
    const bool sent=send_channel_message(input::EncodeMouseMoveChannel(delta_x, delta_y,
                                                                       cursor_x, cursor_y));
    if (sent && sdk_context_.trace && out_input_trace_<32) {
        ++out_input_trace_;
        sdk_context_.trace("out rtc move dx="+Num(delta_x)+" dy="+Num(delta_y)+
                           " x="+Num(cursor_x)+" y="+Num(cursor_y));
    }
    return sent;
}

bool Session::send_mouse_button(std::uint32_t button, bool down,
                                double cursor_x, double cursor_y) {
    if (!button) return false;
    const bool sent=send_channel_message(input::EncodeMouseButtonChannel(button, down,
                                                                         cursor_x, cursor_y));
    if (sent && sdk_context_.trace && out_input_trace_<32) {
        ++out_input_trace_;
        sdk_context_.trace("out rtc btn b="+std::to_string(button)+
                           " down="+(down?"1":"0")+
                           " x="+Num(cursor_x)+" y="+Num(cursor_y));
    }
    return sent;
}

bool Session::send_gamepad_list(std::uint32_t vendor_id, std::uint32_t product_id) {
    if (!vendor_id || !product_id) return false;
    const bool sent=send_channel_message(input::EncodeGamepadListChannel(vendor_id, product_id), false);
    if(sent) idle_gamepad_.invalidate();
    return sent;
}

bool Session::send_device_info(std::uint32_t screen_width, std::uint32_t screen_height) {
    if (!running_ || !authenticated_ || error_.size()) return false;
    control::DeviceInfo info;
    info.game_token=authorization_.game_token;
    info.sdk_version=authorization_.sdk_version;
    info.terminal_type=authorization_.terminal_type;
    // Identity of the actual client, not a browser fingerprint: the pod only
    // stores it for its own statistics. The web SDK fills the same record from
    // userPlatformFeatures (deviceId, WebGL renderer) and the dispatch address
    // of the server the session is connected to.
    info.device_id=sdk_context_.device_id;
    info.device_name="Nintendo Switch";
    info.device_gpu="NVIDIA Tegra X1";
    info.device_system="Horizon OS";
    info.network_type="wifi";
    info.server_ip=selected_route_ ? selected_route_->ip : std::string{};
    info.transport_protocol="udp";
    info.screen_width=screen_width;
    info.screen_height=screen_height;
    const auto packet=control::EncodeSdkDeviceInfo(info);
    if (packet.empty()) return false;
    if (!signaling_.send(signaling::MessageType::Proxy, packet)) return false;
    if (sdk_context_.trace && !device_info_traced_) {
        device_info_traced_=true;
        sdk_context_.trace("out sdk device info w="+std::to_string(screen_width)+
                           " h="+std::to_string(screen_height));
    }
    return true;
}

bool Session::set_graphics_mode(std::int32_t mode) {
    if (!running_ || !authenticated_) return false;
    const auto range=StreamQualityBitrate(mode);
    if(!range) return false;
    const auto packet = control::EncodeSetGraphicsMode(mode);
    if (packet.empty()) return false;
    // The graphics enum alone does not replace the stream's bitrate limits.
    // Apply the explicit range afterwards, like the official floating menu.
    if(!signaling_.send(signaling::MessageType::Proxy,packet)) return false;
    return set_bitrate_range(range->minimum,range->maximum);
}

bool Session::set_bitrate_range(int minimum,int maximum) {
    if(!running_ || !authenticated_ || minimum<kStreamQualityBitrates[2].minimum ||
       maximum<minimum || maximum>kStreamQualityBitrates[0].maximum) return false;
    return signaling_.send(signaling::MessageType::Proxy,
        control::EncodeSetBitrate(MihoyoBitrate(minimum),MihoyoBitrate(maximum)));
}

bool Session::set_fps(std::uint32_t fps) {
    if (!running_ || !authenticated_) return false;
    const auto packet = control::EncodeSetFps(fps);
    if (packet.empty()) return false;
    return signaling_.send(signaling::MessageType::Proxy, packet);
}

bool Session::reset_game_inputs(std::uint32_t input_types) {
    if (!running_ || !authenticated_) return false;
    const auto channel = input::EncodeResetGameInputControlChannel(input_types);
    if (channel.empty()) return false;
    const bool sent = send_channel_message(channel, false);
    if(sent && (input_types & input::kInputControlGamepad)) idle_gamepad_.invalidate();
    if (sent && callbacks_.candidate_event)
        callbacks_.candidate_event("RESET_GAME_INPUT_CONTROL_SENT", static_cast<int>(input_types));
    return sent;
}

void Session::handle_signal(const SignalMessage& message) {
    if (!running_ || !authenticated_ || !peer_ || !error_.empty()) return;
    if(reconnect_.active() && recovery_phase_!=RecoveryPhase::Verifying) return;
    if (message.type == SignalType::Offer) {
        // A new authenticated signaling connection may offer fresh ICE/DTLS
        // credentials. Replace only the peer, not the game SDK/login/cache.
        // Outside recovery the existing transport-rotation guard still holds.
        if(answer_sent_ && reconnect_.active() && recovery_phase_==RecoveryPhase::Verifying &&
           (SdpAttributeValue(message.sdp,"a=ice-ufrag:")!=remote_ice_ufrag_ ||
            SdpAttributeValue(message.sdp,"a=fingerprint:")!=remote_fingerprint_)) {
            if(!PeerOfferWithinBounds(message.sdp)) {error_="Unsupported cloud media offer";return;}
            reset_recovery_peer();
            if(!initialize_peer(false)) return;
            if(callbacks_.candidate_event) callbacks_.candidate_event("STREAM_RECONNECT_PEER_REBUILT",0);
        }
        if (answer_sent_) {
            // The official web client keeps one RTCPeerConnection and answers
            // every later server offer. A same-transport media update (same
            // remote ICE ufrag and fingerprint) keeps the established
            // ICE/DTLS/SCTP association: let libpeer apply new SSRCs, then
            // answer with the unchanged local description that was already
            // sent. Building a fresh answer would rotate our ICE credentials
            // and DTLS certificate, which only an ICE restart may do; a
            // transport change therefore keeps the fixed unsupported error.
            const auto ufrag=SdpAttributeValue(message.sdp,"a=ice-ufrag:");
            const auto fingerprint=SdpAttributeValue(message.sdp,"a=fingerprint:");
            if(local_answer_sdp_.empty() || ufrag.empty() || ufrag!=remote_ice_ufrag_ ||
               fingerprint.empty() || fingerprint!=remote_fingerprint_) {
                error_ = "Cloud renegotiation is not supported";
                return;
            }
            const auto now=clock_();
            renegotiations_.erase(std::remove_if(renegotiations_.begin(),renegotiations_.end(),
                [&](const auto& accepted){return now-accepted>=kRenegotiationWindow;}),renegotiations_.end());
            if (renegotiations_.size() >= kMaximumRenegotiations) {
                error_ = "Too many cloud renegotiations";
                return;
            }
            if(!PeerOfferWithinBounds(message.sdp)) {error_="Unsupported cloud media offer";return;}
            peer_connection_set_remote_description(peer_, message.sdp.c_str(), SDP_TYPE_OFFER);
            AnswerIssue issue{};
            const auto negotiated=NegotiateAnswer(message.sdp,local_answer_sdp_,&issue);
            if(!negotiated && callbacks_.candidate_event)
                callbacks_.candidate_event("RTC_SDP_UNSUPPORTED_REASON",static_cast<int>(issue));
            if(!negotiated || peer_connection_set_receive_payload_types(
                   peer_,negotiated->video_payload,negotiated->audio_payload)!=0) {
                error_="Unsupported cloud media offer";return;
            }
            if(callbacks_.candidate_event) callbacks_.candidate_event(
                "RTC_SDP_NEGOTIATED_ANSWER_MISMATCH_MASK",SdpAnswerMismatchMask(message.sdp,negotiated->sdp));
            if(callbacks_.candidate_event) {
                callbacks_.candidate_event("RTC_NEGOTIATED_VIDEO_PAYLOAD",negotiated->video_payload);
                callbacks_.candidate_event("RTC_NEGOTIATED_AUDIO_PAYLOAD",negotiated->audio_payload);
            }
            if (!signaling_.send_answer(negotiated->sdp)) {
                error_ = "Could not send WebRTC answer";
                return;
            }
            renegotiations_.push_back(now);
            if(callbacks_.candidate_event)
                callbacks_.candidate_event("RTC_RENEGOTIATION_ACCEPTED",static_cast<int>(renegotiations_.size()));
            return;
        }
        if(!PeerOfferWithinBounds(message.sdp)) {error_="Unsupported cloud media offer";return;}
        peer_connection_set_remote_description(peer_, message.sdp.c_str(), SDP_TYPE_OFFER);
        const char* answer = peer_connection_create_answer(peer_);
        if(callbacks_.candidate_event && answer) {
            callbacks_.candidate_event("RTC_SDP_ANSWER_MISMATCH_MASK",SdpAnswerMismatchMask(message.sdp,answer));
            callbacks_.candidate_event("RTC_REMOTE_ICE_LITE",message.sdp.find("a=ice-lite")!=std::string::npos);
        }
        report_transport();
        if(!answer) { error_="Could not send WebRTC answer";return; }
        PeerTransportStats transport{};
        if(peer_connection_get_transport_stats(peer_,&transport)==0 && transport.ice_local_candidates==0) {
            error_="No local ICE candidate";return;
        }
        AnswerIssue issue{};
        const auto negotiated=NegotiateAnswer(message.sdp,answer,&issue);
        if(!negotiated && callbacks_.candidate_event)
            callbacks_.candidate_event("RTC_SDP_UNSUPPORTED_REASON",static_cast<int>(issue));
        if(!negotiated || peer_connection_set_receive_payload_types(peer_,negotiated->video_payload,negotiated->audio_payload)!=0) {
            error_="Unsupported cloud media offer";return;
        }
        if(callbacks_.candidate_event) callbacks_.candidate_event("RTC_SDP_NEGOTIATED_ANSWER_MISMATCH_MASK",SdpAnswerMismatchMask(message.sdp,negotiated->sdp));
        if(callbacks_.candidate_event) {
            callbacks_.candidate_event("RTC_NEGOTIATED_VIDEO_PAYLOAD",negotiated->video_payload);
            callbacks_.candidate_event("RTC_NEGOTIATED_AUDIO_PAYLOAD",negotiated->audio_payload);
        }
        if (!signaling_.send_answer(negotiated->sdp)) {
            error_ = "Could not send WebRTC answer";
            return;
        }
        answer_sent_ = true;
        local_answer_sdp_ = answer;
        remote_ice_ufrag_ = SdpAttributeValue(message.sdp,"a=ice-ufrag:");
        remote_fingerprint_ = SdpAttributeValue(message.sdp,"a=fingerprint:");
        for (const auto& candidate : pending_candidates_)
            add_remote_candidate(candidate);
        pending_candidates_.clear();
    } else if (message.type == SignalType::Candidate) {
        if (!answer_sent_) {
            if (pending_candidates_.size() == kMaximumPendingCandidates) {
                error_ = "Too many early ICE candidates";
                return;
            }
            pending_candidates_.push_back(message.candidate);
        } else {
            add_remote_candidate(message.candidate);
        }
    }
}

bool Session::add_remote_candidate(const std::string& candidate) {
    if (!peer_) return false;
    const auto report=[this](const char* event,int code) {
        if(callbacks_.candidate_event) callbacks_.candidate_event(event,code);
    };
    // Optional/unusable candidates must not abort other viable ICE paths.
    // Keep both work and diagnostic volume bounded per session.
    if(seen_candidates_.size()>=kMaximumPendingCandidates) return false;
    if(std::find(seen_candidates_.begin(),seen_candidates_.end(),candidate)!=seen_candidates_.end()) return true;
    seen_candidates_.push_back(candidate);
    const auto kind=ClassifyCandidate(candidate);
    if(kind!=CandidateKind::Udp) {
        report("RTC_CANDIDATE_SKIPPED",static_cast<int>(kind));
        return false;
    }
    std::string description;
    if (candidate.rfind("a=candidate:", 0) == 0)
        description = candidate;
    else if (candidate.rfind("candidate:", 0) == 0)
        description = "a=" + candidate;
    else {
        error_ = "Invalid remote ICE candidate";
        return false;
    }
    if(selected_route_) {
        if(const auto routed=RouteCandidate(description,*selected_route_)) {
            description=*routed;report("RTC_CANDIDATE_ROUTE_APPLIED",0);
        }
    }
    if (peer_connection_add_ice_candidate(peer_, description.data()) != 0) {
        report("RTC_CANDIDATE_LIBRARY_REJECTED",-1);
        return false;
    }
    report("RTC_CANDIDATE_ACCEPTED",0);
    return true;
}

void Session::OnVideo(const PeerVideoPacket* packet, void* userdata) {
    auto& session = *static_cast<Session*>(userdata);
    if(session.running_ && packet && packet->size && session.reconnect_.active() &&
       session.recovery_phase_==RecoveryPhase::Verifying) session.recovery_media_received_=true;
    if (session.running_ && packet && session.callbacks_.video) session.callbacks_.video(*packet);
}

void Session::OnVideoDamage(const PeerVideoDamage* damage, void* userdata) {
    auto& session = *static_cast<Session*>(userdata);
    if (session.running_ && damage && session.callbacks_.video_damage)
        session.callbacks_.video_damage(*damage);
}

void Session::OnAudio(const PeerAudioPacket* packet, void* userdata) {
    auto& session = *static_cast<Session*>(userdata);
    if (session.running_ && packet && session.callbacks_.audio) session.callbacks_.audio(*packet);
}

void Session::OnData(char* bytes, std::size_t size, void* userdata, std::uint16_t sid) {
    auto& session = *static_cast<Session*>(userdata);
    if (!session.running_ || !bytes) return;
    const bool known = (session.data_sid_ && *session.data_sid_ == sid) ||
                       (session.extra_sid_ && *session.extra_sid_ == sid);
    if (!known) return;
    session.trace_incoming_data(reinterpret_cast<const std::uint8_t*>(bytes), size);
    // While the independent control channel is valid it carries control; the
    // data channel is the official fallback and stays consumable otherwise.
    if (session.control_via_wss()) return;
    if (session.consume_sdk_packet(reinterpret_cast<const std::uint8_t*>(bytes), size)) return;
    if (session.callbacks_.data)
        session.callbacks_.data(reinterpret_cast<const std::uint8_t*>(bytes), size);
}

void Session::OnRemoteChannel(void* userdata, std::uint16_t sid, const char* label,
                              std::uint32_t ppid) {
    auto& session = *static_cast<Session*>(userdata);
    if (!session.running_) return;
    const std::string name = label ? std::string(label) : std::string();
    if (session.callbacks_.candidate_event) {
        session.callbacks_.candidate_event("RTC_REMOTE_DATA_CHANNEL_OPEN",
                                           static_cast<int>(sid));
        session.callbacks_.candidate_event("RTC_REMOTE_DATA_CHANNEL_PPID",
                                           static_cast<int>(ppid));
    }
    if (session.sdk_context_.trace)
        session.sdk_context_.trace("dc open sid=" + std::to_string(sid) +
                                   " ppid=" + std::to_string(ppid) + " label=" + name);
    if (ppid == 50) {
        if (!session.data_sid_) session.data_sid_ = sid;
    } else if (!session.extra_sid_) {
        session.extra_sid_ = sid;
    }
}

void Session::OnPeerState(PeerConnectionState state, void* userdata) {
    auto& session = *static_cast<Session*>(userdata);
    if (!session.running_ || session.peer_resetting_) return;
    if (state==PEER_CONNECTION_CONNECTED || state==PEER_CONNECTION_COMPLETED || state==PEER_CONNECTION_FAILED)
        session.report_transport();
    if (state == PEER_CONNECTION_COMPLETED) {
        session.ever_completed_=true;
        session.deadline_ = {};
        // WEBCTRL sessions prefer the independent GameControlChannel, but the
        // RTC data channel is the official fallback transport, so track it for
        // every type. GCC health is reported through the CONTROL_WSS_* events.
        session.control_.begin(MonotonicMs(session.clock_()));
        if(session.reconnect_.active()) (void)session.request_keyframe();
        if(!UsesWebControl(session.web_control_channel_type_) && session.callbacks_.candidate_event)
            session.callbacks_.candidate_event("RTC_CONTROL_WAITING_ASSOCIATION",0);
    }
    if (state == PEER_CONNECTION_FAILED || state == PEER_CONNECTION_DISCONNECTED) {
        PeerTransportStats transport{};
        const bool certificate_failed=session.peer_ &&
            peer_connection_get_transport_stats(session.peer_,&transport)==0 && transport.dtls_result==-9984;
        session.error_=certificate_failed ? "WebRTC certificate verification failed" : "WebRTC connection lost";
    }
    if (session.callbacks_.state) session.callbacks_.state(state);
}

void Session::report_transport() {
    PeerTransportStats stats{};
    if(!peer_ || !callbacks_.candidate_event || peer_connection_get_transport_stats(peer_,&stats)!=0) return;
    const auto& report=callbacks_.candidate_event;
    report("RTC_DTLS_LOCAL_ROLE",stats.dtls_role);
    report("RTC_DTLS_LAST_RESULT",stats.dtls_result);
    report("RTC_DTLS_REMOTE_PIN_PRESENT",stats.dtls_remote_pin_present);
    report("RTC_DTLS_DATAGRAMS_SENT",static_cast<int>(stats.dtls_datagrams_sent));
    report("RTC_DTLS_DATAGRAMS_RECEIVED",static_cast<int>(stats.dtls_datagrams_received));
    report("RTC_DTLS_SRTP_PROFILE",stats.dtls_srtp_profile);
    const auto count=[](std::uint32_t value) { return static_cast<int>(std::min(value,static_cast<std::uint32_t>(INT_MAX))); };
    report("RTC_SDP_REMOTE_CRYPTO",stats.sdp_remote_crypto);
    report("RTC_SDP_REMOTE_CRYPTO_SUITE",stats.sdp_remote_crypto_suite);
    report("RTC_SDP_REMOTE_VIDEO_PROTO",stats.sdp_remote_video_proto);
    report("RTC_SDP_REMOTE_AUDIO_PROTO",stats.sdp_remote_audio_proto);
    report("RTC_SDP_REMOTE_SETUP",stats.sdp_remote_setup);
    report("RTC_SDP_SDES_VIDEO",stats.sdp_sdes_video);
    report("RTC_SDP_SDES_AUDIO",stats.sdp_sdes_audio);
    report("RTC_SRTP_SDES_PACKETS",count(stats.srtp_alt_successes));
    report("RTC_SRTP_PRIMARY_FAILURES",count(stats.srtp_primary_failures));
    report("RTC_SRTP_PROBE_SHA1_32",count(stats.srtp_variant_successes[0]));
    report("RTC_SRTP_PROBE_SWAP_80",count(stats.srtp_variant_successes[1]));
    report("RTC_SRTP_PROBE_SWAP_32",count(stats.srtp_variant_successes[2]));
    report("RTC_SRTP_PROBE_AUTH_ONLY",count(stats.srtp_variant_successes[3]));
    report("RTC_SRTP_PROBE_MIX_KEY_SALT",count(stats.srtp_variant_successes[4]));
    report("RTC_SRTP_PROBE_MIX_SALT_KEY",count(stats.srtp_variant_successes[5]));
    report("RTC_RTP_VIDEO_DECRYPT_FAILURES",count(stats.rtp_video_decrypt_failures));
    report("RTC_RTP_AUDIO_DECRYPT_FAILURES",count(stats.rtp_audio_decrypt_failures));
    report("RTC_RTP_VIDEO_DECRYPT_SUCCESSES",count(stats.rtp_video_decrypt_successes));
    report("RTC_RTP_AUDIO_DECRYPT_SUCCESSES",count(stats.rtp_audio_decrypt_successes));
    report("RTC_MEDIA_PLAINTEXT_ACTIVE",stats.plaintext_media);
    report("RTC_MEDIA_PLAINTEXT_RTP_STREAK",count(stats.plaintext_rtp_streak));
    report("RTC_MEDIA_PLAINTEXT_RTP_PROMOTIONS",count(stats.plaintext_rtp_promotions));
    report("RTC_MEDIA_PLAINTEXT_RTCP_PROMOTIONS",count(stats.plaintext_rtcp_promotions));
    report("RTC_ICE_LOCAL_CANDIDATES",stats.ice_local_candidates);
    report("RTC_ICE_REMOTE_CANDIDATES",stats.ice_remote_candidates);
    report("RTC_ICE_CANDIDATE_PAIRS",stats.ice_candidate_pairs);
    report("RTC_ICE_REQUESTS_SENT",count(stats.ice_requests_sent));
    report("RTC_ICE_RESPONSES_RECEIVED",count(stats.ice_responses_received));
    report("RTC_ICE_RESPONSES_REJECTED",count(stats.ice_responses_rejected));
    report("RTC_UDP_DATAGRAMS_RECEIVED",count(stats.udp_datagrams_received));
    report("RTC_UDP_SEND_ERRORS",count(stats.udp_send_errors));
    report("RTC_UDP_RECEIVE_ERRORS",count(stats.udp_receive_errors));
    report("RTC_UDP_SELECT_ERRORS",count(stats.udp_select_errors));
    report("RTC_UDP_LAST_ERROR",stats.udp_last_error);
    report("RTC_SCTP_CREATE_ATTEMPTED",stats.sctp_create_attempted ? 1 : 0);
    report("RTC_SCTP_CREATE_RESULT",stats.sctp_create_result);
    opennow::webrtc::ReportMediaIngress(stats,[&report](const char* event,int value,long){report(event,value);});
}

} // namespace cloudgenshin::rtc
