#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "config.h"
#include "dtls_srtp.h"
#include "ice_candidate_pair_policy.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"

// Remote m-line protocol tokens. Native pods sometimes advertise SDES media
// (RTP/SAVPF + a=crypto) even while the same transport completes DTLS.
#define PEER_MEDIA_PROTO_UNKNOWN 0
#define PEER_MEDIA_PROTO_SDES 1
#define PEER_MEDIA_PROTO_DTLS_SRTP 2
#define PEER_MEDIA_PROTO_PLAIN 3
#define PEER_REMOTE_SETUP_UNKNOWN 0
#define PEER_REMOTE_SETUP_ACTIVE 1
#define PEER_REMOTE_SETUP_PASSIVE 2
#define PEER_REMOTE_SETUP_ACTPASS 3

// Small dependency-free base64 decoder for a=crypto inline keys. The DTLS
// library is linked anyway, but keeping this local lets the host test compile
// peer_connection.c without an mbedtls base64 stub.
static int peer_connection_base64_decode(const char* in, int in_len,
                                         unsigned char* out, int out_cap) {
  int value = 0, bits = 0, out_len = 0;
  for (int i = 0; i < in_len; i++) {
    const char c = in[i];
    int digit;
    if (c >= 'A' && c <= 'Z') digit = c - 'A';
    else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
    else if (c >= '0' && c <= '9') digit = c - '0' + 52;
    else if (c == '+') digit = 62;
    else if (c == '/') digit = 63;
    else if (c == '=') break;
    else return -1;
    value = (value << 6) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out_len >= out_cap) return -1;
      out[out_len++] = (unsigned char)((value >> bits) & 0xFF);
    }
  }
  return out_len;
}

static int peer_connection_media_protocol(const char* line) {
  // m=video 9 UDP/TLS/RTP/SAVPF 96
  const char* token = strchr(line, ' ');
  if (!token) return PEER_MEDIA_PROTO_UNKNOWN;
  token = strchr(token + 1, ' ');
  if (!token) return PEER_MEDIA_PROTO_UNKNOWN;
  token++;
  const char* end = strchr(token, ' ');
  const size_t len = end ? (size_t)(end - token) : strlen(token);
  if (len == strlen("UDP/TLS/RTP/SAVPF") &&
      strncmp(token, "UDP/TLS/RTP/SAVPF", len) == 0)
    return PEER_MEDIA_PROTO_DTLS_SRTP;
  if ((len == strlen("RTP/SAVPF") && strncmp(token, "RTP/SAVPF", len) == 0) ||
      (len == strlen("RTP/SAVP") && strncmp(token, "RTP/SAVP", len) == 0))
    return PEER_MEDIA_PROTO_SDES;
  if (len == strlen("RTP/AVPF") && strncmp(token, "RTP/AVPF", len) == 0)
    return PEER_MEDIA_PROTO_PLAIN;
  return PEER_MEDIA_PROTO_UNKNOWN;
}

static uint16_t peer_connection_crypto_suite(const char* line) {
  // a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:...
  const char* p = strchr(line, ' ');
  if (!p) return MBEDTLS_TLS_SRTP_UNSET;
  p++;
  if (strncmp(p, "AES_CM_128_HMAC_SHA1_80", 23) == 0 &&
      (p[23] == ' ' || p[23] == ';' || p[23] == '\0' || p[23] == '\r'))
    return MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;
  if (strncmp(p, "AES_CM_128_HMAC_SHA1_32", 23) == 0 &&
      (p[23] == ' ' || p[23] == ';' || p[23] == '\0' || p[23] == '\r'))
    return MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32;
  return MBEDTLS_TLS_SRTP_UNSET;
}

static int peer_connection_crypto_key(const char* line, unsigned char* out,
                                      int out_cap) {
  const char* p = strstr(line, "inline:");
  if (!p) return -1;
  p += strlen("inline:");
  const char* end = p;
  while (*end && *end != ' ' && *end != '|' && *end != ';' && *end != '\r' &&
         *end != '\n')
    end++;
  return peer_connection_base64_decode(p, (int)(end - p), out, out_cap);
}

#define STATE_CHANGED(pc, curr_state)                                  \
  if (pc->state != curr_state) {                                       \
    peer_connection_diag_log("state_change pc=%p %d->%d", (void*)pc, (int)pc->state,    \
                             (int)curr_state);                         \
    pc->state = curr_state;                                            \
    if (pc->oniceconnectionstatechange) {                              \
      pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    }                                                                 \
  }

struct PeerConnection {
  int has_negotiated_payload_types, receive_video_payload, receive_audio_payload;
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;

  char sdp[CONFIG_SDP_BUFFER_SIZE];

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* user_data);

  // mbedtls_ssl_read may return a prefix when its destination is too small.
  // SCTP needs the entire plaintext packet (including its original checksum),
  // not two reads treated as separate packets. Match the incoming datagram
  // capacity, independently of the conservative outgoing CONFIG_MTU.
  uint8_t temp_buf[CONFIG_DATAGRAM_BUFFER_SIZE];
  uint8_t agent_buf[CONFIG_DATAGRAM_BUFFER_SIZE];
  int agent_ret;
  int dtls_pending;
  int b_local_description_created;
  int remote_ice_lite;
  DtlsSrtpRole answer_dtls_role;
  int offer_dtls_client;

  // Remote SDP media-security state. SDES keys are copied out of a=crypto and
  // applied after dtls_srtp_init so the DTLS-derived contexts stay primary.
  int remote_crypto_count;
  uint16_t remote_crypto_suite;
  int remote_video_proto;
  int remote_audio_proto;
  int remote_setup;
  unsigned char remote_sdes_key[DTLS_SRTP_SDES_MEDIA_COUNT][DTLS_SRTP_SDES_KEY_LENGTH];
  int remote_sdes_key_len[DTLS_SRTP_SDES_MEDIA_COUNT];
  uint16_t remote_sdes_profile[DTLS_SRTP_SDES_MEDIA_COUNT];

  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  RtpDecoder artp_decoder;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;
  RtcpReceiverStats video_receiver_stats;

  int dtls_handshake_attempts;
  int dtls_last_result;
  uint32_t dtls_datagrams_sent;
  uint32_t dtls_datagrams_received;
  uint32_t dtls_handshake_started_ms;
  int sctp_create_attempted;
  int sctp_create_result;
  int completed_udp_packets;
  int completed_rtcp_packets;
  int completed_dtls_packets;
  int completed_rtp_packets;
  int completed_unknown_packets;
  int rtp_decrypt_failures;
  int rtcp_decrypt_failures;

  // Plaintext media fallback. Some native pods run DTLS for the data channel
  // and then send media without SRTP despite advertising UDP/TLS/RTP/SAVPF.
  // Promotion requires a run of structurally valid packets so a wrong SRTP key
  // (random bytes) can never be mistaken for plaintext.
  int plaintext_media;
  uint32_t plaintext_rtp_streak, plaintext_rtcp_streak;
  uint32_t plaintext_rtp_promotions, plaintext_rtcp_promotions;
  uint16_t plaintext_last_rtp_seq;
  int plaintext_has_last_seq;
  int plaintext_video_shape_seen;
  uint32_t plaintext_miss_streak;
  uint32_t rtp_video_decrypt_failures, rtp_audio_decrypt_failures;
  uint32_t rtp_video_decrypt_successes, rtp_audio_decrypt_successes;

  uint32_t last_rtp_payload_type;
  uint32_t rtp_payload_rejected, rtp_ssrc_rejected;
  uint32_t rtp_audio_routed, rtp_video_routed;
  int rtp_last_decrypt_error, rtcp_last_decrypt_error;
  int sctp_read_events;
};

static void peer_connection_apply_remote_sdes(PeerConnection* pc) {
  if (!pc->dtls_srtp.initialized) return;
  for (int media = 0; media < DTLS_SRTP_SDES_MEDIA_COUNT; media++) {
    if (pc->remote_sdes_key_len[media] != DTLS_SRTP_SDES_KEY_LENGTH) continue;
    dtls_srtp_set_sdes_key(&pc->dtls_srtp, media, pc->remote_sdes_profile[media],
                           pc->remote_sdes_key[media],
                           pc->remote_sdes_key_len[media]);
  }
  // Only trust the SDES key for outbound when the remote m-line itself
  // advertised SDES. DTLS-protocol answers keep the DTLS outbound context.
  if (pc->remote_video_proto == PEER_MEDIA_PROTO_SDES)
    dtls_srtp_set_sdes_outbound(&pc->dtls_srtp, 1);
}

static int peer_connection_send_rtcp_nack(PeerConnection* pc, uint32_t ssrc,
                                          uint16_t packet_id, uint16_t bitmask);

static int peer_connection_send_video_nack(void* userdata,uint16_t pid,uint16_t mask) {
  PeerConnection* pc=userdata;
  return pc->remote_vssrc && peer_connection_send_rtcp_nack(pc,pc->remote_vssrc,pid,mask)>0;
}

static void peer_connection_configure_recovery(PeerConnection* pc) {
  rtp_decoder_set_reorder_hold_ms(&pc->vrtp_decoder,pc->config.video_reorder_hold_ms);
  if (pc->config.video_reorder_initial_packets &&
      rtp_decoder_set_recovery_window(&pc->vrtp_decoder,pc->config.video_reorder_initial_packets,
                                     pc->config.video_reorder_max_packets)!=0)
    ++pc->vrtp_decoder.reorder_growth_failures; // Keep the working small window on OOM.
}

static int peer_connection_diagnostics_enabled;

void peer_connection_set_diagnostics_enabled(int enabled) {
  peer_connection_diagnostics_enabled = enabled != 0;
  sctp_set_diagnostics_enabled(enabled);
}

static void peer_connection_diag_log(const char* fmt, ...) {
  if (!peer_connection_diagnostics_enabled)
    return;
  FILE* file = fopen("sdmc:/switch/yuanshi-launcher/signaling.log", "a");
  FILE* trace = fopen("sdmc:/switch/yuanshi-launcher/stream_trace.log", "a");
  const int input_relevant = strstr(fmt, "sctp") != NULL ||
                             strstr(fmt, "dtls") != NULL ||
                             strstr(fmt, "transport_completed") != NULL;
  FILE* input = input_relevant ? fopen("sdmc:/switch/yuanshi-launcher/input.log", "a") : NULL;

  if (file) {
    fputs("LIBPEER ", file);
  }
  if (trace) {
    fputs("LIBPEER ", trace);
  }
  if (input) {
    fputs("LIBPEER ", input);
  }

  va_list args;
  va_start(args, fmt);
  if (file) {
    va_list copy;
    va_copy(copy, args);
    vfprintf(file, fmt, copy);
    va_end(copy);
  }
  if (trace) {
    va_list copy;
    va_copy(copy, args);
    vfprintf(trace, fmt, copy);
    va_end(copy);
  }
  if (input) {
    vfprintf(input, fmt, args);
  }
  va_end(args);

  if (file) {
    fputc('\n', file);
    fclose(file);
  }
  if (trace) {
    fputc('\n', trace);
    fclose(trace);
  }
  if (input) {
    fputc('\n', input);
    fclose(input);
  }
}


static void peer_connection_hex_preview(const uint8_t* data, int len, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!data || len <= 0) {
    return;
  }

  int count = len < 12 ? len : 12;
  size_t pos = 0;
  for (int i = 0; i < count && pos + 4 < out_len; ++i) {
    pos += snprintf(out + pos, out_len - pos, "%s%02x", i == 0 ? "" : " ", data[i]);
  }
  if (len > count && pos + 5 < out_len) {
    snprintf(out + pos, out_len - pos, " ...");
  }
}

#include "plaintext_media.h"

static const char* peer_connection_ice_state_name(IceCandidateState state) {
  switch (state) {
    case ICE_CANDIDATE_STATE_FROZEN:
      return "frozen";
    case ICE_CANDIDATE_STATE_WAITING:
      return "waiting";
    case ICE_CANDIDATE_STATE_INPROGRESS:
      return "inprogress";
    case ICE_CANDIDATE_STATE_SUCCEEDED:
      return "succeeded";
    case ICE_CANDIDATE_STATE_FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

static const char* peer_connection_ice_type_name(IceCandidateType type) {
  switch (type) {
    case ICE_CANDIDATE_TYPE_HOST:
      return "host";
    case ICE_CANDIDATE_TYPE_SRFLX:
      return "srflx";
    case ICE_CANDIDATE_TYPE_PRFLX:
      return "prflx";
    case ICE_CANDIDATE_TYPE_RELAY:
      return "relay";
    default:
      return "unknown";
  }
}

static void peer_connection_log_candidate_pairs(PeerConnection* pc, const char* reason) {
  if (!pc) {
    return;
  }

  peer_connection_diag_log("ice_pairs reason=%s total=%d local=%d remote=%d mode=%d iceLite=%d",
                           reason ? reason : "unknown",
                           pc->agent.candidate_pairs_num,
                           pc->agent.local_candidates_count,
                           pc->agent.remote_candidates_count,
                           pc->agent.mode,
                           pc->remote_ice_lite);

  for (int i = 0; i < pc->agent.candidate_pairs_num && i < 12; ++i) {
    IceCandidatePair* pair = &pc->agent.candidate_pairs[i];
    char local[ADDRSTRLEN] = {0};
    char remote[ADDRSTRLEN] = {0};
    if (pair->local) {
      addr_to_string(&pair->local->addr, local, sizeof(local));
    }
    if (pair->remote) {
      addr_to_string(&pair->remote->addr, remote, sizeof(remote));
    }
    peer_connection_diag_log("ice_pair[%d] state=%s selected=%d local=%s/%s:%u remote=%s/%s:%u priority=%" PRIu64,
                             i,
                             peer_connection_ice_state_name(pair->state),
                             pair == pc->agent.selected_pair,
                             pair->local
                                 ? peer_connection_ice_type_name(pair->local->type)
                                 : "none",
                             local,
                             pair->local ? pair->local->addr.port : 0,
                             pair->remote
                                 ? peer_connection_ice_type_name(pair->remote->type)
                                 : "none",
                             remote,
                             pair->remote ? pair->remote->addr.port : 0,
                             pair->priority);
  }
}

static void peer_connection_outgoing_rtp_packet(uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, data, (int*)&size);
  agent_send(&pc->agent, data, size);
}

static int peer_connection_dtls_srtp_recv(void* ctx, unsigned char* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  if (pc->agent_ret > 0) {
    const int bytes = pc->agent_ret;
    pc->agent_ret = -1;
    pc->dtls_pending = 0;
    if ((size_t)bytes > len)
      return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    memcpy(buf, pc->agent_buf, bytes);
    return bytes;
  }

  if (pc->state == PEER_CONNECTION_COMPLETED)
    return MBEDTLS_ERR_SSL_WANT_READ;

  const int ret = agent_recv_nonblocking(&pc->agent, buf, len);
  if (ret > 0) {
    pc->dtls_datagrams_received++;
    return ret;
  }

  return MBEDTLS_ERR_SSL_WANT_READ;
}

static int peer_connection_dtls_srtp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len);
  const int sent = agent_send(&pc->agent, buf, len);
  if (sent > 0) pc->dtls_datagrams_sent++;
  return sent;
}

static void peer_connection_incoming_rtcp(PeerConnection* pc, uint8_t* buf, size_t len) {
  if (!rtcp_validate_compound(buf, len))
    return;
  size_t pos = 0;

  while (pos < len) {
    RtcpPacketView view;
    if (rtcp_parse_packet(buf + pos, len - pos, &view) != 0)
      return;

    switch (view.type) {
      case RTCP_SR:
        {
          uint32_t sender_ssrc_net, ntp_seconds_net, ntp_fraction_net, rtp_timestamp_net;
          memcpy(&sender_ssrc_net, buf + pos + 4, 4);
          memcpy(&ntp_seconds_net, buf + pos + 8, 4);
          memcpy(&ntp_fraction_net, buf + pos + 12, 4);
          memcpy(&rtp_timestamp_net, buf + pos + 16, 4);
          const uint32_t sender_ssrc = ntohl(sender_ssrc_net);
          const uint32_t ntp_seconds = ntohl(ntp_seconds_net);
          const uint32_t ntp_fraction = ntohl(ntp_fraction_net);
          const uint32_t rtp_timestamp = ntohl(rtp_timestamp_net);
          const uint64_t ntp_us = (uint64_t)ntp_seconds * 1000000ULL +
                                  (((uint64_t)ntp_fraction * 1000000ULL) >> 32);
          if (sender_ssrc == pc->remote_vssrc)
            rtcp_receiver_record_sr(&pc->video_receiver_stats, &view, ports_get_monotonic_time());
          if (pc->config.onrtpsenderreport)
            pc->config.onrtpsenderreport(sender_ssrc, ntp_us, rtp_timestamp, pc->config.user_data);
        }
        break;
      case RTCP_RR:
        LOGD("RTCP_PR");
        if (view.count > 0) {
// TODO: REMB, GCC ...etc
#if 0
          RtcpRr rtcp_rr = rtcp_parse_rr(buf);
          uint32_t fraction = ntohl(rtcp_rr.report_block[0].flcnpl) >> 24;
          uint32_t total = ntohl(rtcp_rr.report_block[0].flcnpl) & 0x00FFFFFF;
          if(pc->on_receiver_packet_loss && fraction > 0) {

            pc->on_receiver_packet_loss((float)fraction/256.0, total, pc->config.user_data);
          }
#endif
        }
        break;
      case RTCP_PSFB: {
        int fmt = view.count;
        LOGD("RTCP_PSFB %d", fmt);
        // PLI and FIR
        if ((fmt == 1 || fmt == 4) && pc->config.on_request_keyframe) {
          pc->config.on_request_keyframe(pc->config.user_data);
        }
      }
      default:
        break;
    }

    pos += view.size;
  }
}

static void peer_connection_maybe_send_receiver_report(PeerConnection* pc) {
  const uint32_t now_ms = ports_get_monotonic_time();
  if (pc->state != PEER_CONNECTION_COMPLETED || pc->remote_vssrc == 0 ||
      !rtcp_receiver_report_due(&pc->video_receiver_stats, now_ms))
    return;
  uint8_t packet[128];
  int size = rtcp_get_receiver_report(packet, sizeof(packet) - 32,
      pc->vrtp_encoder.ssrc, &pc->video_receiver_stats, now_ms, "webrtc-h264");
  int success = 0;
  if (size > 0 && dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, packet, &size) == 0)
    success = agent_send(&pc->agent, packet, size) == size;
  rtcp_receiver_report_sent(&pc->video_receiver_stats, now_ms, success);
}

const char* peer_connection_state_to_string(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_COMPLETED:
      return "completed";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(PeerConnection* pc) {
  return pc->state;
}

void* peer_connection_get_sctp(PeerConnection* pc) {
  return &pc->sctp;
}

int peer_connection_sctp_connected(PeerConnection* pc) {
  return pc ? sctp_is_connected(&pc->sctp) : 0;
}

PeerConnection* peer_connection_create(PeerConfiguration* config) {
  PeerConnection* pc = calloc(1, sizeof(PeerConnection));
  peer_connection_diag_log("peer_create pc=%p", (void*)pc);
  if (!pc) {
    return NULL;
  }

  memcpy(&pc->config, config, sizeof(PeerConfiguration));
  pc->state = PEER_CONNECTION_NEW;

  agent_create(&pc->agent);

  memset(&pc->sctp, 0, sizeof(pc->sctp));

  if (pc->config.audio_codec) {
    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);
    rtp_decoder_set_audio_callback(&pc->artp_decoder, pc->config.onaudiopacket);
  }

  if (pc->config.video_codec) {
    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);
    peer_connection_configure_recovery(pc);
    rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
    rtp_decoder_set_video_damage_callback(&pc->vrtp_decoder, pc->config.onvideodamage);
  }

  return pc;
}

void peer_connection_destroy(PeerConnection* pc) {
  if (pc) {
    peer_connection_diag_log("peer_destroy pc=%p", (void*)pc);
    rtp_decoder_cleanup(&pc->artp_decoder);
    rtp_decoder_cleanup(&pc->vrtp_decoder);
    sctp_destroy_association(&pc->sctp);
    dtls_srtp_deinit(&pc->dtls_srtp);
    agent_destroy(&pc->agent);
    free(pc);
    pc = NULL;
  }
}

void peer_connection_close(PeerConnection* pc) {
  peer_connection_diag_log("peer_close pc=%p state=%d", (void*)pc, (int)pc->state);
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_datachannel_send_binary_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol) {
  return peer_connection_create_datachannel_sid(pc, channel_type, priority, reliability_parameter, label, protocol, 0);
}

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid) {
  int rtrn = -1;

  if (!sctp_is_connected(&pc->sctp)) {
#if !CONFIG_USE_USRSCTP
    peer_connection_diag_log("sctp_not_connected waiting_handshake sid=%u label=%s", sid, label ? label : "");
#endif
    LOGE("sctp not connected");
    return rtrn;
  }

  //  0                   1                   2                   3
  //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |  Message Type |  Channel Type |            Priority           |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                    Reliability Parameter                      |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |         Label Length          |       Protocol Length         |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                             Label                             |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                            Protocol                           |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  int msg_size = 12 + strlen(label) + strlen(protocol);
  uint16_t priority_big_endian = htons(priority);
  uint32_t reliability_big_endian = htonl(reliability_parameter);
  uint16_t label_length = htons(strlen(label));
  uint16_t protocol_length = htons(strlen(protocol));
  char* msg = calloc(1, msg_size);
  if (!msg) {
    return rtrn;
  }

  msg[0] = DATA_CHANNEL_OPEN;
  msg[1] = (char)channel_type;
  memcpy(msg + 2, &priority_big_endian, sizeof(uint16_t));
  memcpy(msg + 4, &reliability_big_endian, sizeof(uint32_t));
  memcpy(msg + 8, &label_length, sizeof(uint16_t));
  memcpy(msg + 10, &protocol_length, sizeof(uint16_t));
  memcpy(msg + 12, label, strlen(label));
  memcpy(msg + 12 + strlen(label), protocol, strlen(protocol));

  rtrn = sctp_outgoing_data(&pc->sctp, msg, msg_size, PPID_CONTROL, sid);
  if (rtrn >= 0) {
    sctp_add_stream_mapping(&pc->sctp, label, sid);
  }
  free(msg);
  return rtrn;
}

static char* peer_connection_dtls_role_setup_value(DtlsSrtpRole d) {
  return d == DTLS_SRTP_ROLE_SERVER ? "a=setup:passive" : "a=setup:active";
}

static int peer_connection_read_dtls(PeerConnection* pc) {
  int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
  LOGD("Got DTLS data %d", ret);

  if (ret > 0) {
    pc->sctp_read_events++;
    if (peer_connection_diagnostics_enabled &&
        (pc->sctp_read_events <= 10 || pc->sctp_read_events % 600 == 0)) {
      char sctp_preview[64];
      peer_connection_hex_preview(pc->temp_buf, ret, sctp_preview, sizeof(sctp_preview));
      peer_connection_diag_log("dtls_read_appdata ret=%d sctpEvents=%d preview=%s",
                               ret, pc->sctp_read_events, sctp_preview);
    }
    sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
  } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
    peer_connection_diag_log("dtls_peer_close_notify packets=%d sctpConnected=%d",
                             pc->completed_dtls_packets, sctp_is_connected(&pc->sctp));
    STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
  } else if (ret < 0 && (pc->completed_dtls_packets <= 10 || pc->completed_dtls_packets % 600 == 0)) {
    peer_connection_diag_log("dtls_read_no_appdata ret=%d dtlsPackets=%d",
                             ret, pc->completed_dtls_packets);
  }
  if (ret <= 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
    pc->dtls_pending = 0;
    pc->agent_ret = -1;
  }
  return ret;
}

int peer_connection_loop(PeerConnection* pc) {
  uint32_t ssrc = 0;
  int packet_processed = 0;
  if (!pc->dtls_pending) {
    memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
    pc->agent_ret = -1;
  }

  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      break;

    case PEER_CONNECTION_CHECKING:
      if (pc->agent.candidate_pairs_num == 0) {
        break;
      }
      if (agent_select_candidate_pair(&pc->agent) < 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
      } else if (agent_connectivity_check(&pc->agent) == 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
      }
      break;

    case PEER_CONNECTION_CONNECTED:
      if (pc->dtls_handshake_attempts == 0) {
        pc->dtls_handshake_started_ms = ports_get_epoch_time();
        peer_connection_diag_log("dtls_start pc=%p datachannel=%d candidatePairs=%d",
                                 (void*)pc,
                                 pc->config.datachannel,
                                 pc->agent.candidate_pairs_num);
        peer_connection_log_candidate_pairs(pc, "dtls_start");
      }

      pc->dtls_handshake_attempts++;

      int dtls_ret = dtls_srtp_handshake(&pc->dtls_srtp, NULL);
      pc->dtls_last_result = dtls_ret;
      if (dtls_ret == 0) {
        LOGD("DTLS-SRTP handshake done");
        peer_connection_diag_log("dtls_done attempts=%d datachannel=%d version=%s cipher=%s",
                                 pc->dtls_handshake_attempts,
                                 pc->config.datachannel,
                                 mbedtls_ssl_get_version(&pc->dtls_srtp.ssl),
                                 mbedtls_ssl_get_ciphersuite(&pc->dtls_srtp.ssl));

        if (pc->config.datachannel && !pc->sctp_create_attempted) {
          LOGI("SCTP create socket");
#if CONFIG_USE_USRSCTP
          peer_connection_diag_log("sctp_backend=usrsctp");
#else
          peer_connection_diag_log("sctp_backend=internal");
#endif
          peer_connection_diag_log("sctp_create_start pc=%p", (void*)pc);
          pc->sctp_create_attempted = 1;
          int sctp_ret = sctp_create_association(&pc->sctp, &pc->dtls_srtp);
          pc->sctp_create_result = sctp_ret;
          peer_connection_diag_log("sctp_create_done ret=%d", sctp_ret);
          if (sctp_ret == 0) {
            pc->sctp.userdata = pc->config.user_data;
          } else {
            LOGE("SCTP create socket failed");
          }
        }

        peer_connection_log_candidate_pairs(pc, "transport_completed");
        peer_connection_diag_log("transport_completed");
        STATE_CHANGED(pc, PEER_CONNECTION_COMPLETED);
      } else if ((uint32_t)(ports_get_epoch_time() - pc->dtls_handshake_started_ms) >= 3500) {
        int failed_pairs = agent_fail_nominated_remote(&pc->agent);
        peer_connection_diag_log("dtls_endpoint_timeout attempts=%d failedPairs=%d; trying next remote",
                                 pc->dtls_handshake_attempts,
                                 failed_pairs);
        dtls_srtp_reset_session(&pc->dtls_srtp);
        pc->dtls_handshake_attempts = 0;
        pc->dtls_handshake_started_ms = 0;
        if (failed_pairs > 0) {
          STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
        } else {
          STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
        }
      } else if (pc->dtls_handshake_attempts == 1 || pc->dtls_handshake_attempts % 600 == 0) {
        peer_connection_diag_log("dtls_pending ret=%d attempts=%d",
                                 dtls_ret,
                                 pc->dtls_handshake_attempts);
      }
      break;
    case PEER_CONNECTION_COMPLETED:
      sctp_tick(&pc->sctp);
      if (pc->dtls_pending || mbedtls_ssl_check_pending(&pc->dtls_srtp.ssl)) {
        packet_processed = peer_connection_read_dtls(pc) > 0;
        if (packet_processed || pc->dtls_pending || pc->state != PEER_CONNECTION_COMPLETED)
          break;
      }
      if ((pc->agent_ret = agent_recv_nonblocking(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf))) > 0) {
        packet_processed = 1;
        LOGD("agent_recv %d", pc->agent_ret);
        pc->completed_udp_packets++;
        if (dtls_srtp_probe(pc->agent_buf)) {
          pc->dtls_pending = 1;
          pc->completed_dtls_packets++;
          if (peer_connection_diagnostics_enabled &&
              (pc->completed_dtls_packets <= 10 || pc->completed_dtls_packets % 600 == 0)) {
            char preview[64];
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_diag_log("udp_class=dtls count=%d bytes=%d preview=%s",
                                     pc->completed_dtls_packets,
                                     pc->agent_ret,
                                     preview);
          }
          peer_connection_read_dtls(pc);

        } else if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTCP packet");
          pc->completed_rtcp_packets++;
          if (peer_connection_diagnostics_enabled &&
              (pc->completed_rtcp_packets <= 5 || pc->completed_rtcp_packets % 600 == 0)) {
            char preview[64];
            char tail[64];
            const int len = pc->agent_ret;
            const int tail_len = len > 16 ? 16 : len;
            const int e14 = len >= 14 ? (pc->agent_buf[len - 14] & 0x80) != 0 : -1;
            const int e8 = len >= 8 ? (pc->agent_buf[len - 8] & 0x80) != 0 : -1;
            peer_connection_hex_preview(pc->agent_buf, len, preview, sizeof(preview));
            peer_connection_hex_preview(pc->agent_buf + len - tail_len, tail_len,
                                        tail, sizeof(tail));
            peer_connection_diag_log(
                "udp_class=rtcp count=%d bytes=%d e14=%d e8=%d preview=%s tail=%s",
                                     pc->completed_rtcp_packets, len, e14, e8, preview, tail);
          }
          // Plaintext RTCP compound headers parse exactly; SRTCP cannot, since
          // its block lengths are encrypted. RTCP alone never promotes: the
          // pod's media must also have shown valid H.264 framing, so a stray
          // unauthenticated datagram cannot flip the session.
          if (plaintext_media_rtcp_valid(pc->agent_buf, pc->agent_ret)) {
            ++pc->plaintext_rtcp_streak;
          } else {
            pc->plaintext_rtcp_streak = 0;
          }
          if (!pc->plaintext_media && pc->plaintext_rtcp_streak >= 2 &&
              pc->plaintext_video_shape_seen >= 3) {
            pc->plaintext_media = 1;
            pc->dtls_srtp.plaintext_outbound = 1;
            ++pc->plaintext_rtcp_promotions;
            peer_connection_diag_log(
                "plaintext_media_promoted source=rtcp streak=%u bytes=%d",
                                     pc->plaintext_rtcp_streak, pc->agent_ret);
          }if (!pc->plaintext_media) {const int unprotect_result = dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
          if (unprotect_result != 0) {
            pc->rtcp_decrypt_failures++;
            pc->rtcp_last_decrypt_error = unprotect_result;
            peer_connection_diag_log("rtcp_decrypt_failed failures=%d bytesAfter=%d plainShape=%u",
                                     pc->rtcp_decrypt_failures,
                                     pc->agent_ret,                                       pc->plaintext_rtcp_streak);
            break;}}
          peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);

        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTP packet");
          pc->completed_rtp_packets++;
          // Cache while the buffer really contains an RTP-shaped datagram.
          // A later DTLS/RTCP receive overwrites agent_buf; stats must not read
          // an unrelated byte as the last payload. This header is not yet
          // authenticated and is diagnostic only, never a routing decision.
          pc->last_rtp_payload_type = ((RtpPacket*)pc->agent_buf)->header.type;

          if (peer_connection_diagnostics_enabled &&
              (pc->completed_rtp_packets <= 10 || pc->completed_rtp_packets % 1200 == 0)) {
            char preview[64];
            char payload[40];
            const int payload_len = pc->agent_ret > 12 ? pc->agent_ret - 12 : 0;
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_hex_preview(pc->agent_buf + 12,
                                        payload_len > 12 ? 12 : payload_len,
                                        payload, sizeof(payload));
            peer_connection_diag_log(
                "udp_class=rtp count=%d bytes=%d pt=%u ssrc=%08x preview=%s payload=%s",
                                     pc->completed_rtp_packets,
                                     pc->agent_ret,
                                     (unsigned)((RtpPacket*)pc->agent_buf)->header.type,
                                     (unsigned)rtp_get_ssrc(pc->agent_buf),
                                     preview, payload);
          }

          const uint32_t pre_ssrc = rtp_get_ssrc(pc->agent_buf);
          const uint32_t pre_pt = ((RtpPacket*)pc->agent_buf)->header.type;
          // Video candidacy must not depend on having learned the remote SSRC:
          // the negotiated payload type is authoritative and is known from SDP
          // before the first packet arrives.
          const int pre_is_video = (pc->remote_vssrc && pre_ssrc == pc->remote_vssrc) ||
              (pc->has_negotiated_payload_types && pre_pt == pc->receive_video_payload);
          const int pre_is_audio = pc->remote_assrc && pre_ssrc == pc->remote_assrc;
          // Detect clear media before touching the buffer. The RTP header is
          // always plaintext, so a valid H.264 payload plus a continuous
          // sequence number cannot come from a wrongly keyed decrypt.
          const int plain_shape = plaintext_media_rtp_valid(
              pc->agent_buf, pc->agent_ret, pre_is_video);
          if (pre_is_video && plain_shape) {
            const uint16_t seq = (uint16_t)((pc->agent_buf[2] << 8) | pc->agent_buf[3]);
            const int continuous = pc->plaintext_has_last_seq &&
                (uint16_t)(seq - pc->plaintext_last_rtp_seq) >= 1 &&
                (uint16_t)(seq - pc->plaintext_last_rtp_seq) <= 8;
            pc->plaintext_rtp_streak =
                (pc->plaintext_rtp_streak == 0 || continuous) ? pc->plaintext_rtp_streak + 1 : 1;
            pc->plaintext_last_rtp_seq = seq;
            pc->plaintext_has_last_seq = 1;
          } else if (pre_is_video) {
            pc->plaintext_rtp_streak = 0;
            pc->plaintext_has_last_seq = 0;
          }
          if (pre_is_video && plain_shape && !pc->plaintext_media)
            ++pc->plaintext_video_shape_seen;
          // Eight consecutive well-formed packets is far beyond any chance
          // alignment; promote once and stay there for the session.
          if (!pc->plaintext_media && pre_is_video && pc->plaintext_rtp_streak >= 8) {
            pc->plaintext_media = 1;
            pc->dtls_srtp.plaintext_outbound = 1;
            pc->plaintext_rtp_promotions++;
            peer_connection_diag_log(
                "plaintext_media_promoted source=rtp streak=%u bytes=%d pt=%u",
                pc->plaintext_rtp_streak, pc->agent_ret,
                (unsigned)((RtpPacket*)pc->agent_buf)->header.type);
          }

          int want_plaintext = pc->plaintext_media;
          if (pc->plaintext_media) {
            if (pre_is_video && !plain_shape) {
              // A promoted session must keep seeing H.264 framing. A long run
              // of opaque video payloads means the pod switched that media to
              // SRTP; drop the promotion so the normal path re-examines it.
              if (++pc->plaintext_miss_streak >= 30) {
                pc->plaintext_media = 0;
                pc->dtls_srtp.plaintext_outbound = 0;
                pc->plaintext_miss_streak = 0;
                pc->plaintext_rtp_streak = 0;
                pc->plaintext_has_last_seq = 0;
                want_plaintext = 0;
                peer_connection_diag_log("plaintext_media_demoted");
              } else {
                ++pc->rtp_payload_rejected;
                break;
              }
            } else {
              pc->plaintext_miss_streak = 0;
            }
          }
          if (!want_plaintext) {
            const int unprotect_result = dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
          if (unprotect_result != 0) {
            pc->rtp_decrypt_failures++;
            if (pre_is_video) pc->rtp_video_decrypt_failures++;
            if (pre_is_audio) pc->rtp_audio_decrypt_failures++;
            pc->rtp_last_decrypt_error = unprotect_result;
            peer_connection_diag_log(
                "rtp_decrypt_failed failures=%d bytesAfter=%d video=%d audio=%d plainShape=%d streak=%u",
                                     pc->rtp_decrypt_failures,
                                     pc->agent_ret,
                                       pc->rtp_video_decrypt_failures,
                                       pc->rtp_audio_decrypt_failures,
                                       plain_shape,
                                       pc->plaintext_rtp_streak);
            break;            }            if (pre_is_video) pc->rtp_video_decrypt_successes++;            if (pre_is_audio) pc->rtp_audio_decrypt_successes++;
          }

          ssrc = rtp_get_ssrc(pc->agent_buf);
          RtpPacket* rtp = (RtpPacket*)pc->agent_buf;
          if (pc->has_negotiated_payload_types && rtp->header.type != pc->receive_video_payload &&
              rtp->header.type != pc->receive_audio_payload) {
            pc->rtp_payload_rejected++;
            break;
          }
          const int is_audio_payload = pc->has_negotiated_payload_types
              ? rtp->header.type == pc->receive_audio_payload : rtp->header.type == PT_PCMU ||
                                       rtp->header.type == PT_PCMA ||
                                       rtp->header.type == PT_OPUS ||
                                       rtp->header.type == 63;
          if (ssrc == pc->remote_assrc || (pc->remote_assrc == 0 && is_audio_payload)) {
            if (pc->remote_assrc == 0) {
              pc->remote_assrc = ssrc;
              LOGI("Learned remote audio SSRC from RTP: %" PRIu32, pc->remote_assrc);
              peer_connection_diag_log("learned_audio_ssrc=%" PRIu32 " pt=%u",
                                       pc->remote_assrc, rtp->header.type);
            }
            pc->rtp_audio_routed++;
            rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
          } else if (ssrc == pc->remote_vssrc ||
                     (pc->remote_vssrc == 0 && (pc->has_negotiated_payload_types
                      ? rtp->header.type == pc->receive_video_payload :
                      rtp->header.type != PT_PCMU &&
                      rtp->header.type != PT_PCMA &&
                      rtp->header.type != PT_OPUS &&
                      rtp->header.type != 63))) {
            if (pc->remote_vssrc == 0) {
              pc->remote_vssrc = ssrc;
              LOGI("Learned remote video SSRC from RTP: %" PRIu32, pc->remote_vssrc);
            }
            pc->rtp_video_routed++;
            rtcp_receiver_record_rtp(&pc->video_receiver_stats,
                                     pc->agent_buf, pc->agent_ret, ports_get_monotonic_time());
            rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
            if (pc->completed_rtp_packets % 6000 == 0) {
              peer_connection_diag_log(
                  "rtp_h264 packets=%u gaps=%u reordered=%u late=%u forced=%u buffered=%u nalus=%u auOk=%u auDrop=%u",
                  pc->vrtp_decoder.packets_received,
                  pc->vrtp_decoder.sequence_gaps,
                  pc->vrtp_decoder.reordered_packets,
                  pc->vrtp_decoder.late_packets_dropped,
                  pc->vrtp_decoder.forced_sequence_skips,
                  pc->vrtp_decoder.reorder_buffered_packets,
                  pc->vrtp_decoder.nal_units_completed,
                  pc->vrtp_decoder.access_units_completed,
                  pc->vrtp_decoder.access_units_dropped);
            }
          } else {
            pc->rtp_ssrc_rejected++;
          }

        } else {
          pc->completed_unknown_packets++;
          if (peer_connection_diagnostics_enabled &&
              (pc->completed_unknown_packets <= 10 || pc->completed_unknown_packets % 600 == 0)) {
            char preview[64];
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_diag_log("udp_class=unknown count=%d bytes=%d preview=%s totals udp/rtcp/dtls/rtp/unknown=%d/%d/%d/%d/%d",
                                     pc->completed_unknown_packets,
                                     pc->agent_ret,
                                     preview,
                                     pc->completed_udp_packets,
                                     pc->completed_rtcp_packets,
                                     pc->completed_dtls_packets,
                                     pc->completed_rtp_packets,
                                     pc->completed_unknown_packets);
          }
          LOGW("Unknown data");
        }
      }

      if (CONFIG_KEEPALIVE_TIMEOUT > 0 &&
          pc->agent.binding_request_time > 0 &&
          (ports_get_epoch_time() - pc->agent.binding_request_time) > CONFIG_KEEPALIVE_TIMEOUT) {
        LOGI("binding request timeout; keeping completed transport alive");
        pc->agent.binding_request_time = ports_get_epoch_time();
      }
      rtp_decoder_poll_after_receive(&pc->vrtp_decoder,packet_processed);
      // Drive repair even when no new RTP arrives (tail loss / short silence).
      // Retire expired holes before requesting them; all transport work stays
      // on the peer owner and the recovery module bounds feedback frequency.
      // ICE's cached RTT is owner-local. Do not query SCTP_STATUS (which
      // takes the global SCTP mutex) in the high-frequency media loop.
      rtp_decoder_poll_nacks(&pc->vrtp_decoder,agent_get_rtt_ms(&pc->agent),
                             peer_connection_send_video_nack,pc);
      peer_connection_maybe_send_receiver_report(pc);

      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  return packet_processed;
}

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType type) {
  char* start = (char*)sdp;
  peer_connection_diag_log("set_remote_desc pc=%p type=%d state=%d", (void*)pc, (int)type,
                           (int)pc->state);
  char* line = NULL;
  char buf[1024];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  pc->answer_dtls_role = DTLS_SRTP_ROLE_CLIENT;
  int is_update = 0;
  int video_ssrc_seen = 0;
  int audio_ssrc_seen = 0;int current_media = -1;Agent* agent = &pc->agent;
  // Identity belongs to the REMOTE description. Clear when replacing it, not
  // when generating a local answer after this offer has already been parsed.
  // Missing fingerprints must still fail closed instead of inheriting a pin.
  memset(pc->dtls_srtp.remote_fingerprint, 0, sizeof(pc->dtls_srtp.remote_fingerprint));
  // Media-security attributes also belong to the new remote description.
  pc->remote_crypto_count = 0;
  pc->remote_crypto_suite = MBEDTLS_TLS_SRTP_UNSET;
  pc->remote_video_proto = PEER_MEDIA_PROTO_UNKNOWN;
  pc->remote_audio_proto = PEER_MEDIA_PROTO_UNKNOWN;
  pc->remote_setup = PEER_REMOTE_SETUP_UNKNOWN;
  memset(pc->remote_sdes_key_len, 0, sizeof(pc->remote_sdes_key_len));
  memset(pc->remote_sdes_profile, 0, sizeof(pc->remote_sdes_profile));
  // A new remote description can be a different pod session with different
  // media protection. Forget any plaintext promotion; it is re-proven from the
  // packet shapes of the new session before being used again.
  pc->plaintext_media = 0;
  pc->plaintext_rtp_streak = 0;
  pc->plaintext_rtcp_streak = 0;
  pc->plaintext_has_last_seq = 0;
  pc->plaintext_video_shape_seen = 0;
  pc->plaintext_miss_streak = 0;
  pc->dtls_srtp.plaintext_outbound = 0;

  while ((line = strstr(start, "\r\n"))) {
    line = strstr(start, "\r\n");
    size_t line_len = (size_t)(line - start);
    if (line_len >= sizeof(buf))
      line_len = sizeof(buf) - 1;
    memcpy(buf, start, line_len);
    buf[line_len] = '\0';

    // Media-shape diagnostics only: codecs, payload types, SSRC groups and
    // the setup role. Never log candidates, ICE credentials, fingerprints or
    // a=crypto inline keys.
    if (strncmp(buf, "m=video ", 8) == 0 || strncmp(buf, "m=audio ", 8) == 0 ||
        strncmp(buf, "m=application ", 14) == 0 ||
        strncmp(buf, "a=rtpmap:", 9) == 0 || strncmp(buf, "a=fmtp:", 7) == 0 ||
        strncmp(buf, "a=rtcp-fb:", 10) == 0 ||
        strncmp(buf, "a=ssrc-group:", 13) == 0 ||
        strncmp(buf, "a=ssrc:", 7) == 0 || strncmp(buf, "a=mid:", 6) == 0 ||
        strncmp(buf, "a=setup:", 8) == 0)
      peer_connection_diag_log("remote_sdp %s", buf);

    if (strcmp(buf, "a=setup:active") == 0) {
      pc->answer_dtls_role = DTLS_SRTP_ROLE_SERVER;
      pc->remote_setup = PEER_REMOTE_SETUP_ACTIVE;
    } else if (strcmp(buf, "a=setup:passive") == 0) {
      pc->answer_dtls_role = DTLS_SRTP_ROLE_CLIENT;
      pc->remote_setup = PEER_REMOTE_SETUP_PASSIVE;
    } else if (strcmp(buf, "a=setup:actpass") == 0) {
      pc->answer_dtls_role = DTLS_SRTP_ROLE_CLIENT;
      pc->remote_setup = PEER_REMOTE_SETUP_ACTPASS;
    }
    // "a=fingerprint:sha-256 AA:BB:..." -- the hash value starts after the last
    // colon-space separator, i.e. at offset 23. Two things had to be fixed
    // here: the value must start at the digest (not at "sha-256 "), and a
    // fingerprint may itself contain colons, so only the FIRST ':' (the one
    // after "a=fingerprint") can be the separator. Using strchr looked right
    // but picked the colon inside "sha-256:" ... which is the same character,
    // so the algorithm prefix leaked into the digest and every real handshake
    // failed with "Remote certificate fingerprint mismatch".
    if (strstr(buf, "a=fingerprint")) {
      const char* value = strchr(buf, ':');        // after "a=fingerprint"
      if (value) {
        value++;                                    // now at the algorithm name
        const char* space = strchr(value, ' ');     // separator before digest
        if (space) {
          value = space + 1;
          strncpy(pc->dtls_srtp.remote_fingerprint, value, DTLS_SRTP_FINGERPRINT_LENGTH - 1);
          pc->dtls_srtp.remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH - 1] = '\0';
        }
      }
    }

    if (strstr(buf, "a=ice-ufrag") &&
        strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag, strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }

    if (strncmp(buf, "m=video ", 8) == 0) {
      current_media = 0;
      pc->remote_video_proto = peer_connection_media_protocol(buf);
      ssrc = &pc->remote_vssrc;
    } else if (strncmp(buf, "m=audio ", 8) == 0) {
      current_media = 1;
      pc->remote_audio_proto = peer_connection_media_protocol(buf);
      ssrc = &pc->remote_assrc;
    } else if (strncmp(buf, "m=application ", 14) == 0) {
      current_media = -1;
      ssrc = NULL;
    }

    if (strncmp(buf, "a=crypto:", 9) == 0) {
      ++pc->remote_crypto_count;
      const uint16_t suite = peer_connection_crypto_suite(buf);
      if (suite != MBEDTLS_TLS_SRTP_UNSET &&
          pc->remote_crypto_suite == MBEDTLS_TLS_SRTP_UNSET)
        pc->remote_crypto_suite = suite;
      if (current_media >= 0 && current_media < DTLS_SRTP_SDES_MEDIA_COUNT &&
          suite != MBEDTLS_TLS_SRTP_UNSET) {
        unsigned char key[DTLS_SRTP_SDES_KEY_LENGTH];
        const int key_len = peer_connection_crypto_key(buf, key, sizeof(key));
        if (key_len == DTLS_SRTP_SDES_KEY_LENGTH) {
          memcpy(pc->remote_sdes_key[current_media], key,
                 DTLS_SRTP_SDES_KEY_LENGTH);
          pc->remote_sdes_key_len[current_media] = key_len;
          pc->remote_sdes_profile[current_media] = suite;
          peer_connection_diag_log("remote_sdes_key media=%d profile=%u",
                                   current_media, (unsigned)suite);
        }
      }
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      int* ssrc_seen = ssrc == &pc->remote_vssrc
          ? &video_ssrc_seen : &audio_ssrc_seen;
      if (*ssrc_seen) {
        start = line + 2;
        continue;
      }
      *ssrc_seen = 1;
      uint32_t parsed_ssrc = strtoul(val_start + 7, NULL, 10);
      if (*ssrc == 0) {
        *ssrc = parsed_ssrc;
        LOGD("SSRC: %" PRIu32, *ssrc);
        peer_connection_diag_log("remote_ssrc_selected media=%s ssrc=%" PRIu32,
                                 ssrc == &pc->remote_vssrc ? "video" : "audio",
                                 *ssrc);
      } else if (*ssrc != parsed_ssrc) {
        const int video = ssrc == &pc->remote_vssrc;
        peer_connection_diag_log("remote_ssrc_changed media=%s previous=%" PRIu32 " current=%" PRIu32,
                                 video ? "video" : "audio", *ssrc, parsed_ssrc);
        *ssrc = parsed_ssrc;
        if (video) {
          rtp_decoder_cleanup(&pc->vrtp_decoder);
          rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                           pc->config.onvideotrack, pc->config.user_data);
          peer_connection_configure_recovery(pc);
          rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
          rtp_decoder_set_video_damage_callback(&pc->vrtp_decoder, pc->config.onvideodamage);
          memset(&pc->video_receiver_stats, 0, sizeof(pc->video_receiver_stats));
        } else {
          rtp_decoder_cleanup(&pc->artp_decoder);
          rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                           pc->config.onaudiotrack, pc->config.user_data);
          rtp_decoder_set_audio_callback(&pc->artp_decoder, pc->config.onaudiopacket);
        }
      }
    }

    start = line + 2;
  }

  if (is_update) {
    return;
  }

  pc->remote_ice_lite = strstr(sdp, "a=ice-lite") != NULL;
  agent_set_remote_description(&pc->agent, (char*)sdp);
  // An ANSWER arrives after the local offer initialized DTLS; register its
  // SDES keys immediately. For an OFFER this is a no-op and create_answer
  // applies them after dtls_srtp_init.
  peer_connection_apply_remote_sdes(pc);
  if (type == SDP_TYPE_ANSWER) {
    agent_update_candidate_pairs(&pc->agent);
    STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  }
}

static const char* peer_connection_create_sdp(PeerConnection* pc, SdpType sdp_type) {
  char* description = (char*)pc->temp_buf;

  peer_connection_diag_log("sdp_create pc=%p type=%d state=%d", (void*)pc, (int)sdp_type,
                           (int)pc->state);
  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;

  pc->sctp.connected = 0;

  switch (sdp_type) {
    case SDP_TYPE_OFFER:
      role = pc->offer_dtls_client ? DTLS_SRTP_ROLE_CLIENT : DTLS_SRTP_ROLE_SERVER;
      agent_clear_candidates(&pc->agent);
      pc->agent.mode = AGENT_MODE_CONTROLLING;
      break;
    case SDP_TYPE_ANSWER:
      role = pc->answer_dtls_role;
      pc->agent.mode = pc->remote_ice_lite ? AGENT_MODE_CONTROLLING : AGENT_MODE_CONTROLLED;
      break;
    default:
      break;
  }

  dtls_srtp_reset_session(&pc->dtls_srtp);
  if (pc->dtls_srtp.initialized) dtls_srtp_deinit(&pc->dtls_srtp);
  pc->dtls_handshake_attempts = 0;
  pc->dtls_handshake_started_ms = 0;
  pc->dtls_last_result = dtls_srtp_init(&pc->dtls_srtp, role, pc);
  if (pc->dtls_last_result != 0) return NULL;
  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;
  // dtls_srtp_init() just cleared the previous SDES contexts; re-register the
  // keys parsed from the current remote description.
  peer_connection_apply_remote_sdes(pc);

  memset(pc->sdp, 0, sizeof(pc->sdp));
  // A new offer has no peer identity yet. An ANSWER must preserve the pin
  // parsed from the remote offer above; clearing here breaks all answerers.
  if (sdp_type == SDP_TYPE_OFFER)
    memset(pc->dtls_srtp.remote_fingerprint, 0, sizeof(pc->dtls_srtp.remote_fingerprint));
  // TODO: check if we have video or audio codecs
  sdp_create(pc->sdp,
             pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE,
             pc->config.datachannel);

  agent_create_ice_credential(&pc->agent);
  sdp_append(pc->sdp, "a=ice-ufrag:%s", pc->agent.local_ufrag);
  sdp_append(pc->sdp, "a=ice-pwd:%s", pc->agent.local_upwd);
  sdp_append(pc->sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
  sdp_append(pc->sdp, peer_connection_dtls_role_setup_value(role));

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(pc->sdp);
  }

  switch (pc->config.audio_codec) {
    case CODEC_PCMA:
      sdp_append_pcma(pc->sdp);
      break;
    case CODEC_PCMU:
      sdp_append_pcmu(pc->sdp);
      break;
    case CODEC_OPUS:
      sdp_append_opus(pc->sdp);
    default:
      break;
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(pc->sdp);
  }

  pc->b_local_description_created = 1;

  agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address
  for (int i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
    if (pc->config.ice_servers[i].urls) {
      LOGI("ice server: %s", pc->config.ice_servers[i].urls);
      agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls, pc->config.ice_servers[i].username, pc->config.ice_servers[i].credential);
    }
  }

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));
  sdp_append(pc->sdp, description);

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->sdp, pc->config.user_data);
  }

  return pc->sdp;
}

const char* peer_connection_create_offer(PeerConnection* pc) {
  return peer_connection_create_sdp(pc, SDP_TYPE_OFFER);
}

int peer_connection_set_offer_dtls_client(PeerConnection* pc, int enabled) {
  if (!pc || pc->b_local_description_created || (enabled != 0 && enabled != 1))
    return -1;
  pc->offer_dtls_client = enabled;
  return 0;
}

const char* peer_connection_create_answer(PeerConnection* pc) {
  const char* sdp = peer_connection_create_sdp(pc, SDP_TYPE_ANSWER);
  if (!sdp) return NULL;
  agent_update_candidate_pairs(&pc->agent);
  STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  return sdp;
}

int peer_connection_get_transport_stats(PeerConnection* pc, PeerTransportStats* stats) {
  if (!pc || !stats) return -1;
  stats->dtls_role = pc->dtls_srtp.role;
  stats->dtls_result = pc->dtls_last_result;
  stats->dtls_remote_pin_present = pc->dtls_srtp.remote_fingerprint[0] != 0;
  stats->dtls_attempts = pc->dtls_handshake_attempts;
  stats->dtls_srtp_profile = pc->dtls_srtp.negotiated_profile;
  stats->sdp_remote_crypto = pc->remote_crypto_count;
  stats->sdp_remote_crypto_suite = pc->remote_crypto_suite;
  stats->sdp_remote_video_proto = pc->remote_video_proto;
  stats->sdp_remote_audio_proto = pc->remote_audio_proto;
  stats->sdp_remote_setup = pc->remote_setup;
  stats->sdp_sdes_video = pc->remote_sdes_key_len[0] > 0;
  stats->sdp_sdes_audio = pc->remote_sdes_key_len[1] > 0;
  stats->srtp_alt_successes = pc->dtls_srtp.srtp_in_alt_successes;
  stats->srtp_primary_failures = pc->dtls_srtp.srtp_in_primary_failures;
  for (int i = 0; i < 6; i++)
    stats->srtp_variant_successes[i] = pc->dtls_srtp.srtp_variant_successes[i];
  stats->rtp_video_decrypt_failures = pc->rtp_video_decrypt_failures;
  stats->rtp_audio_decrypt_failures = pc->rtp_audio_decrypt_failures;
  stats->rtp_video_decrypt_successes = pc->rtp_video_decrypt_successes;
  stats->rtp_audio_decrypt_successes = pc->rtp_audio_decrypt_successes;
  stats->plaintext_media = pc->plaintext_media;
  stats->plaintext_rtp_streak = pc->plaintext_rtp_streak;
  stats->plaintext_rtp_promotions = pc->plaintext_rtp_promotions;
  stats->plaintext_rtcp_promotions = pc->plaintext_rtcp_promotions;
  stats->dtls_datagrams_sent = pc->dtls_datagrams_sent;
  stats->dtls_datagrams_received = pc->dtls_datagrams_received;
  stats->ice_local_candidates = pc->agent.local_candidates_count;
  stats->ice_remote_candidates = pc->agent.remote_candidates_count;
  stats->ice_candidate_pairs = pc->agent.candidate_pairs_num;
  stats->ice_requests_sent = pc->agent.binding_requests_sent;
  stats->ice_responses_received = pc->agent.binding_responses_received;
  stats->ice_responses_rejected = pc->agent.binding_responses_rejected;
  stats->udp_datagrams_received = pc->agent.udp_datagrams_received;
  stats->udp_send_errors = pc->agent.udp_send_errors;
  stats->udp_receive_errors = pc->agent.udp_receive_errors;
  stats->udp_select_errors = pc->agent.udp_select_errors;
  stats->udp_last_error = pc->agent.last_socket_error;
  stats->sctp_create_attempted = pc->sctp_create_attempted;
  stats->sctp_create_result = pc->sctp_create_result;
  stats->rtp_completed = (uint32_t)pc->completed_rtp_packets;
  stats->rtp_decrypt_failures = (uint32_t)pc->rtp_decrypt_failures;
  stats->rtcp_completed = (uint32_t)pc->completed_rtcp_packets;
  stats->rtcp_decrypt_failures = (uint32_t)pc->rtcp_decrypt_failures;
  stats->rtp_last_payload_type = pc->last_rtp_payload_type;
  stats->rtp_payload_rejected = pc->rtp_payload_rejected;
  stats->rtp_ssrc_rejected = pc->rtp_ssrc_rejected;
  stats->rtp_audio_routed = pc->rtp_audio_routed;
  stats->rtp_video_routed = pc->rtp_video_routed;
  stats->rtp_last_decrypt_error = pc->rtp_last_decrypt_error;
  stats->rtcp_last_decrypt_error = pc->rtcp_last_decrypt_error;
  return 0;
}

int peer_connection_set_receive_payload_types(PeerConnection* pc, int video, int audio) {
  if (!pc || video < 0 || video > 127 || audio < 0 || audio > 127 || video == audio ||
      (pc->state != PEER_CONNECTION_NEW && pc->state != PEER_CONNECTION_CHECKING &&
       pc->state != PEER_CONNECTION_COMPLETED)) return -1;
  pc->receive_video_payload = video; pc->receive_audio_payload = audio;
  pc->has_negotiated_payload_types = 1;
  return 0;
}

int peer_connection_send_rtcp_pil(PeerConnection* pc, uint32_t ssrc) {
  uint8_t plibuf[128];
  int size = 12;

  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || ssrc == 0)
    return -1;

  if (rtcp_get_pli(plibuf, size, pc->vrtp_encoder.ssrc, ssrc) != size)
    return -1;

  if (dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, plibuf, &size) != 0)
    return -1;
  const int ret = agent_send(&pc->agent, plibuf, size);
  peer_connection_diag_log("rtcp_pli sent=%d mediaSsrc=%" PRIu32 " bytes=%d",
                           ret,
                           ssrc,
                           size);
  return ret == size ? ret : -1;
}

static int peer_connection_send_rtcp_nack(PeerConnection* pc, uint32_t ssrc,
                                          uint16_t packet_id, uint16_t bitmask) {
  uint8_t nackbuf[64];
  int size = 16;

  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || ssrc == 0)
    return -1;
  if (rtcp_get_nack(nackbuf, size, pc->vrtp_encoder.ssrc, ssrc,
                    packet_id, bitmask) != size)
    return -1;

  if (dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, nackbuf, &size)!=0) return -1;
  const int written=agent_send(&pc->agent,nackbuf,size);
  return written==size ? written : -1;
}

int peer_connection_request_video_keyframe(PeerConnection* pc) {
  if (!pc)
    return -1;
  return peer_connection_send_rtcp_pil(pc, pc->remote_vssrc);
}

// callbacks
void peer_connection_on_connected(PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp, void* userdata)) {
  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata)) {
  if (pc) {
    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessage);
  }
}

void peer_connection_on_remote_datachannel(PeerConnection* pc,
    void (*onchannelopen)(void* userdata, uint16_t sid, const char* label, uint32_t ppid)) {
  if (pc)
    sctp_onchannelopen(&pc->sctp, onchannelopen);
}

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label, sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    LOGE("Remote ICE candidate table is full");
    return -1;
  }
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }
  LOGD("Add candidate: %s", candidate);
  IceCandidate* remote = &agent->remote_candidates[agent->remote_candidates_count];
  agent->remote_candidates_count++;

  for (int i = 0; i < agent->local_candidates_count; i++) {
    if (agent->local_candidates[i].addr.family != remote->addr.family)
      continue;
    if (agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
      LOGE("ICE candidate pair table is full");
      return -1;
    }

    IceCandidatePair* pair = &agent->candidate_pairs[agent->candidate_pairs_num++];
    pair->local = &agent->local_candidates[i];
    pair->remote = remote;
    pair->priority = ice_candidate_pair_priority(
        pair->local->priority,
        pair->remote->priority,
        agent->mode == AGENT_MODE_CONTROLLING);
    pair->state = ICE_CANDIDATE_STATE_FROZEN;
  }
  agent_sort_candidate_pairs(agent);

  return 0;
}

int peer_connection_get_ice_candidate_pair_stats(PeerConnection* pc,
                                                 int* total,
                                                 int* frozen,
                                                 int* inprogress,
                                                 int* succeeded,
                                                 int* failed) {
  if (!pc) {
    return -1;
  }

  AgentCandidatePairStats stats;
  agent_get_candidate_pair_stats(&pc->agent, &stats);
  if (total)
    *total = stats.total;
  if (frozen)
    *frozen = stats.frozen;
  if (inprogress)
    *inprogress = stats.inprogress;
  if (succeeded)
    *succeeded = stats.succeeded;
  if (failed)
    *failed = stats.failed;
  return 0;
}

int peer_connection_get_rtt_ms(PeerConnection* pc) {
  if (!pc)
    return -1;

  const int ice_rtt_ms = agent_get_rtt_ms(&pc->agent);
  return ice_rtt_ms >= 0 ? ice_rtt_ms : sctp_get_rtt_ms(&pc->sctp);
}

int peer_connection_get_video_rtp_stats(PeerConnection* pc, PeerVideoRtpStats* stats) {
  if (pc == NULL || stats == NULL)
    return -1;
  stats->packets_received = pc->vrtp_decoder.packets_received;
  stats->sequence_gaps = pc->vrtp_decoder.sequence_gaps;
  stats->reordered_packets = pc->vrtp_decoder.reordered_packets;
  stats->late_packets_dropped = pc->vrtp_decoder.late_packets_dropped;
  stats->forced_sequence_skips = pc->vrtp_decoder.forced_sequence_skips;
  stats->reorder_buffered_packets = pc->vrtp_decoder.reorder_buffered_packets;
  stats->access_units_completed = pc->vrtp_decoder.access_units_completed;
  stats->access_units_dropped = pc->vrtp_decoder.access_units_dropped;
  stats->nack_requests = pc->vrtp_decoder.nack_requests;
  stats->nack_packets_requested = pc->vrtp_decoder.nack_packets_requested;
  stats->nack_retry_packets = pc->vrtp_decoder.nack_retry_packets;
  stats->nack_recovered_packets = pc->vrtp_decoder.nack_recovered_packets;
  stats->nack_exhausted_packets = pc->vrtp_decoder.nack_exhausted_packets;
  stats->nack_send_failures = pc->vrtp_decoder.nack_send_failures;
  stats->nack_repair_max_ms = pc->vrtp_decoder.nack_repair_max_ms;
  stats->reorder_timeout_skips = pc->vrtp_decoder.reorder_timeout_skips;
  stats->reorder_capacity_skips = pc->vrtp_decoder.reorder_capacity_skips;
  stats->reorder_capacity = pc->vrtp_decoder.reorder_capacity;
  stats->reorder_peak_packets = pc->vrtp_decoder.reorder_peak_packets;
  stats->reorder_growths = pc->vrtp_decoder.reorder_growths;
  stats->reorder_growth_failures = pc->vrtp_decoder.reorder_growth_failures;
  return 0;
}