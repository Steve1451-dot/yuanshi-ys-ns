#pragma once

#include "WebSocketClient.hpp"
#include "mihoyo/signaling_frame.hpp"
#include "mihoyo/rtc_signal.hpp"

#include <chrono>
#include <atomic>
#include <thread>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cloudgenshin {

// Transport-only adapter. Authentication, dispatch and WebRTC SDP belong to
// separate stages; constructing this class never starts a game session.
class SignalingClient {
public:
    using FrameCallback = std::function<void(const signaling::Frame&)>;
    using RtcSignalCallback = std::function<void(const rtc::SignalMessage&)>;
    using ConnectAttemptCallback=std::function<void(unsigned,const opennow::websocket::ConnectDiagnostics&)>;

    explicit SignalingClient(const std::string& wss_url,std::vector<std::string> alternatives={});
    ~SignalingClient();

    bool connect(bool send_hello = true, const std::atomic_bool* cancelled = nullptr,
                 long budget_ms = 20000);
    enum class ReconnectResult {Idle,Pending,Connected,Failed};
    // Only the reconnect worker touches the socket until poll_reconnect joins
    // it. Media continues on its existing owner; no concurrent curl access.
    bool begin_reconnect();
    ReconnectResult poll_reconnect();
    void cancel_reconnect();
    bool reconnecting() const {return reconnect_running_;}
    bool recoverable_disconnect() const;
    bool send_client_hello();
    void disconnect();
    void poll();
    bool send(signaling::MessageType type, const std::vector<std::uint8_t>& payload);
    bool send_answer(const std::string& sdp);
    bool send_candidate(const std::string& candidate, const std::string& sdp_mid,
                        int sdp_mline_index);
    void set_on_frame(FrameCallback callback) { on_frame_ = std::move(callback); }
    void set_on_rtc_signal(RtcSignalCallback callback) { on_rtc_signal_ = std::move(callback); }
    void set_on_connect_attempt(ConnectAttemptCallback callback) { on_connect_attempt_=std::move(callback); }
    const opennow::websocket::ConnectDiagnostics& connect_diagnostics() const { return connect_diagnostics_; }
    bool is_connected() const { return !reconnect_running_ && local_error_.empty() && socket_.is_connected(); }
    std::string last_error() const;
    // Ephemeral routing only; never log or persist this endpoint.
    const std::string& connected_endpoint() const {return connected_endpoint_;}

private:
    WebSocketClient socket_;
    std::vector<std::string> endpoints_;
    FrameCallback on_frame_;
    RtcSignalCallback on_rtc_signal_;
    ConnectAttemptCallback on_connect_attempt_;
    opennow::websocket::ConnectDiagnostics connect_diagnostics_;
    std::string local_error_;
    std::string connected_endpoint_;
    std::chrono::steady_clock::time_point next_keepalive_{};
    std::thread reconnect_worker_;
    std::atomic_bool reconnect_cancelled_{false},reconnect_done_{false};
    bool reconnect_running_=false,reconnect_result_=false;
};

} // namespace cloudgenshin
