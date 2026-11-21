#pragma once
#include "mihoyo/sdk_wire_limits.hpp"
#include "mihoyo/genshin_native_config_policy.hpp"
#include "mihoyo/genshin_controller_lock.hpp"
#include <borealis/extern/nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cloudgenshin::sdk {
// The official cloud client talks to the in-game SDK through the reliable
// message queue channel (cmd 1900): an acknowledged protobuf queue carrying
// AES-encrypted JSON "SDK" messages. Values below come from the published
// official web bundles (cg-sdk / web.b / combo-web), never from account data.
inline constexpr std::uint16_t kRtcNotPlayingTipsCmd = 20014;
inline constexpr std::uint16_t kRtcKeepPlayingCmd = 20015;

// proto.common.RmqMessageType.
inline constexpr std::uint32_t kRmqMessageNormal = 1;
inline constexpr std::uint32_t kRmqMessageReTransmit = 2;
inline constexpr std::uint32_t kRmqMessageRebuilt = 3;
inline constexpr std::uint32_t kRmqMessageAck = 4;

// proto.common.ReliableMessageQueueData.
struct ReliableMessageQueueData {
    std::uint32_t msg_id = 0;
    std::uint32_t msg_type = 0;
    std::uint32_t seq_id = 0;
    std::uint32_t seq_cnt = 0;
    std::uint32_t total_len = 0;
    std::uint32_t data_len = 0;
    std::vector<std::uint8_t> data;
    std::string tag;
};
std::optional<ReliableMessageQueueData> DecodeReliableMessageQueueData(
    std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> EncodeReliableMessageQueueData(
    const ReliableMessageQueueData& message);
// ACK may also carry seqId (field 2) and unknown protobuf extension fields.
std::optional<std::uint32_t> DecodeReliableMessageQueueAck(std::span<const std::uint8_t> bytes);

// proto.common.SdkGameDataMessage { name = 1, data = 2 }.
struct SdkGameDataMessage {
    std::string name;
    std::vector<std::uint8_t> data;
};
std::optional<SdkGameDataMessage> DecodeSdkGameDataMessage(
    std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> EncodeSdkGameDataMessage(const SdkGameDataMessage& message);

// The official web client's SDK payload crypto: AES-256-ECB/PKCS7 with the
// published web key, Base64 text on the wire (CryptoJS output without a salt).
std::optional<std::string> EncryptGameDataPayload(std::string_view json);
std::optional<std::string> DecryptGameDataPayload(std::string_view base64_text);

struct BridgeContext {
    std::string game_token;      // proto.msg.RtcKeepPlaying.gameToken
    std::string account_id;      // cloud_get_data("account_id")
    std::string device_id;       // login_login result fields
    std::string app_id;
    std::string channel_id;
    std::string channel_token;   // the passport token the official client reports
    std::string open_id;
    std::string combo_token;
    std::string sdk_login_data{};
    // Optional values from the official box/user config. Without a known
    // duration, perform only the immediate refresh, never an endless timer.
    std::optional<double> keep_alive_duration_minutes{};
    double idle_timeout_minutes = 0;
    // info_get_cps reply. The official web page composes it as
    // "<platform>_<utm_source||mihoyo>"; the native holder uses the app's
    // channel string with "mihoyo" as fallback. WebPC/WebKeyboard -> pc.
    std::string cps = "pc_mihoyo";
    // login_logout / login_switch_role end the official cloud session.
    std::function<void()> on_session_end;
    // Fixed-tag diagnostics; never carries message payloads.
    std::function<void(const char*, int)> event;
    // Protocol-metadata trace sink: invoker kind, function identifier, index and
    // reply decision only, never parameter values or credentials. The session
    // points it at a bounded file solely for the field-diagnostics autotest.
    std::function<void(const std::string&)> trace;
    // Official FP service response bound to this device, never a local hash.
    // Not logged or saved in plaintext. Empty means unavailable, not verified.
    std::string device_fingerprint{};
    // Game-owned SDK storage restored before the peer starts. The change sink
    // only queues work; it must never perform filesystem I/O in a peer callback.
    nlohmann::json initial_cloud_data = nlohmann::json::object();
    std::function<void(const nlohmann::json&)> on_cloud_data_changed;
    bool merge_cloud_data_updates = false; // Official Android: independent files per key.
    // false: keep initial_cloud_data only as a merge base for saving and return
    // "" from cloud_get_data instead of the saved game configuration.
    bool serve_saved_cloud_data = true;
    // hk4e only. Keybinds filters maps/fields; Exact preserves the original
    // string; Full is the legacy forced-hot-swap A/B path. The view combines
    // Off with serve_saved_cloud_data=false for save-only operation.
    NativeConfigRestoreMode native_config_restore_mode = NativeConfigRestoreMode::Off;
    // ys Keybinds only: keep a confirmed native pad profile fixed, while all
    // unrelated game data continues updating. Recovery candidates are already
    // scoped by the local game-data store, never account/login snapshots.
    bool lock_native_controller = false;
    std::vector<nlohmann::json> native_controller_recovery;
};

// Replicates the official web client's SDK game-data bridge: queue reception
// with acknowledgements, payload crypto, the Init/login reply contract and the
// cloud_* custom invocations, including the game-directed keep-alive.
class Bridge {
public:
    using SendProxy =
        std::function<bool(std::uint16_t command, const std::vector<std::uint8_t>& body)>;
    Bridge(BridgeContext context, SendProxy send_proxy);

    // Returns true when the command belongs to the game SDK bridge.
    bool HandleProxyPacket(std::uint16_t command, std::span<const std::uint8_t> body,
                           std::int64_t monotonic_ms);
    // A local input message was sent; keep-playing follows onAnyAction.
    void OnLocalInput(std::int64_t monotonic_ms);
    // Drives the game-requested cloud keep-alive interval.
    void Poll(std::int64_t monotonic_ms);

private:
    using Json = nlohmann::json;
    void Emit(const char* tag, int code = 0);
    void EmitOnce(std::uint32_t bit, const char* tag);
    void EmitCustomOnce(std::uint32_t bit, const char* tag);
    void Trace(const std::string& line);
    void ReceiveQueueMessage(const ReliableMessageQueueData& message, std::int64_t monotonic_ms);
    void DeliverMessage(std::span<const std::uint8_t> bytes, std::int64_t monotonic_ms);
    void HandleGamePayload(std::span<const std::uint8_t> bytes, std::int64_t monotonic_ms);
    void SendAck(std::uint32_t acked_msg_id);
    bool SendKeepPlaying(std::int64_t monotonic_ms);
    void SendEncryptedReply(const std::string& wire_json);
    void SendBoxConfig();
    void ReportControllerLock();
    std::optional<std::string> DispatchCustom(const std::string& function, const Json& params,
                                              bool index_present, std::int64_t index,
                                              std::int64_t monotonic_ms);
    std::optional<std::string> DispatchInvoke(const std::string& function, const Json& params,
                                              bool index_present, std::int64_t index);
    std::optional<std::string> DispatchInvokeReturn(const std::string& function,
                                                    bool index_present, std::int64_t index);
    std::optional<std::string> DispatchWebview(const std::string& function, const Json& params,
                                               bool index_present, std::int64_t index);
    static std::string BuildReply(const std::string& type, bool index_present,
                                  std::int64_t index, bool has_data, const Json& data);

    BridgeContext context_;
    SendProxy send_;
    // Receiver/sender counters both start at 1 in the official classes and are
    // independent of each other.
    std::uint32_t receiver_message_id_ = 1;
    std::uint32_t sender_message_id_ = 1;
    std::optional<std::uint32_t> large_reply_id_;
    bool large_reply_acknowledged_ = false;
    bool large_reply_failed_ = false;
    std::vector<std::uint8_t> receive_buffer_;
    std::uint32_t receive_total_length_ = 0;
    bool retransmitting_ = false;
    // onAnyAction-style keep-playing.
    std::int64_t last_keep_playing_ms_ = -1;
    bool no_action_tip_ = false;
    // Game-requested keep-alive timer (cloud_keep_alive custom call).
    bool keep_alive_enabled_ = false;
    std::uint32_t keep_alive_ticks_left_ = 0;
    std::int64_t next_keep_alive_ms_ = 0;
    // cloud_set_data storage (the official CLOUD_DATA_STORAGE_KEY map).
    Json cloud_data_ = Json::object();
    std::optional<NativeControllerLock> native_controller_lock_;
    bool native_lock_ready_reported_=false,native_lock_protected_reported_=false,native_lock_rejected_reported_=false;
    // sdkWebview global UA (the official runtime config key "ua"). Empty is a
    // valid initial value; it is never replaced by a fabricated platform UA.
    std::string webview_user_agent_;
    // The official page pushes on_set_box_config once after the game's Init.
    bool box_config_sent_ = false;
    // One-shot diagnostics per category.
    std::uint32_t emitted_ = 0;
    // Per-function custom-call evidence. Separate from emitted_ because these
    // must stay distinguishable and must survive log trimming.
    std::uint32_t custom_emitted_ = 0;
    bool native_reason_reported_ = false;
    bool native_write_summary_reported_ = false;
    std::string last_keybind_reply_;
    unsigned report_diagnostics_count_ = 0;
    bool star_rail_network_error_reported_ = false;
    bool star_rail_nonzero_retcode_reported_ = false;
    unsigned cloud_schema_count_ = 0;
    bool cloud_data_returned_ = false;
    bool cloud_data_rejected_ = false;
    bool cloud_restore_skipped_ = false;
    bool cloud_save_only_reported_ = false;
    bool native_restore_reported_ = false;
};
} // namespace cloudgenshin::sdk
