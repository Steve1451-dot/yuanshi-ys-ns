#ifndef DTLS_SRTP_H_
#define DTLS_SRTP_H_

#include <stdio.h>
#include <stdlib.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <srtp2/srtp.h>

#include "address.h"

#define SRTP_MASTER_KEY_LENGTH 16
#define SRTP_MASTER_SALT_LENGTH 14
#define DTLS_SRTP_KEY_MATERIAL_LENGTH 60
#define DTLS_SRTP_FINGERPRINT_LENGTH 160

// SDES (RFC 4568) fallback media slots. Some native cloud pods answer the
// DTLS handshake but still protect media with the key from their a=crypto
// line. Keep DTLS-SRTP as the primary path and use these only when the
// DTLS-derived key fails authentication.
#define DTLS_SRTP_SDES_MEDIA_COUNT 2  // 0 = video, 1 = audio
#define DTLS_SRTP_SDES_KEY_LENGTH (SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH)
#define DTLS_SRTP_SDES_SCRATCH_LENGTH 2048

// Inbound key-layout probes. RFC 5764 puts the client key/salt first, but a
// nonstandard pod may use the opposite direction or the 32-bit auth-tag
// profile despite negotiating SHA1_80. These contexts share the same DTLS
// master secret, so a successful authentication proves which layout the pod
// actually used; they never weaken the handshake or certificate pin.
#define DTLS_SRTP_VARIANT_COUNT 6
#define DTLS_SRTP_VARIANT_AUTH_ONLY 3  // RTCP E=0 probe; never used for RTP

typedef enum DtlsSrtpRole {

  DTLS_SRTP_ROLE_CLIENT,
  DTLS_SRTP_ROLE_SERVER

} DtlsSrtpRole;

typedef enum DtlsSrtpState {

  DTLS_SRTP_STATE_INIT,
  DTLS_SRTP_STATE_HANDSHAKE,
  DTLS_SRTP_STATE_CONNECTED

} DtlsSrtpState;

typedef struct DtlsSrtp {
  // MbedTLS
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ssl_cookie_ctx cookie_ctx;
  mbedtls_x509_crt cert;
  mbedtls_pk_context pkey;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;

  // SRTP
  srtp_policy_t remote_policy;
  srtp_policy_t local_policy;
  srtp_t srtp_in;
  srtp_t srtp_out;
  unsigned char remote_policy_key[SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH];
  unsigned char local_policy_key[SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH];

  // SDES fallback contexts, indexed by DTLS_SRTP_SDES_MEDIA_COUNT. The policy
  // key is copied from the remote a=crypto line before srtp_create().
  srtp_t srtp_in_alt[DTLS_SRTP_SDES_MEDIA_COUNT];
  srtp_t srtp_out_sdes;
  unsigned char sdes_key[DTLS_SRTP_SDES_MEDIA_COUNT][DTLS_SRTP_SDES_KEY_LENGTH];
  int sdes_key_len[DTLS_SRTP_SDES_MEDIA_COUNT];
  uint16_t sdes_profile[DTLS_SRTP_SDES_MEDIA_COUNT];
  int srtp_in_alt_count;
  // 0 = primary DTLS key, 1..count = alternate slot + 1. Once an alternate
  // authenticates a packet it is tried first, so steady-state cost stays low.
  int srtp_in_preferred;
  int sdes_outbound;
  uint32_t srtp_in_primary_failures;
  uint32_t srtp_in_alt_successes;

  // Key-layout probes derived from the same DTLS exporter output. Index 0/1/2
  // mirror the primary key with SHA1_32, the opposite direction with SHA1_80
  // and the opposite direction with SHA1_32; index 3 is an RTCP auth-only
  // probe; 4/5 mix key and salt halves to expose a layout bug. Each success
  // counter identifies the layout the remote actually used.
  srtp_t srtp_in_variant[DTLS_SRTP_VARIANT_COUNT];
  unsigned char srtp_variant_key[DTLS_SRTP_VARIANT_COUNT]
                                 [SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH];
  uint32_t srtp_variant_successes[DTLS_SRTP_VARIANT_COUNT];
  int srtp_in_variant_preferred;  // 0 = none, otherwise variant + 1

  // Some native pods complete DTLS for the data channel and then send media
  // without SRTP even though their SDP says UDP/TLS/RTP/SAVPF. The caller
  // promotes this only after several structurally valid plaintext packets; a
  // wrong SRTP key produces random bytes that cannot pass that check. When
  // set, outbound RTP/RTCP is sent in the clear too, because a pod that does
  // not protect media will not accept protected feedback either.
  int plaintext_outbound;

  int (*udp_send)(void* ctx, const unsigned char* buf, size_t len);
  int (*udp_recv)(void* ctx, unsigned char* buf, size_t len);

  Address* remote_addr;

  DtlsSrtpRole role;
  DtlsSrtpState state;
  int initialized;
  // use_srtp profile chosen by the handshake (0 = not negotiated). SRTP policy
  // must match it: AES128_CM_HMAC_SHA1_32 uses a 4-byte RTP auth tag, so the
  // default 10-byte policy fails authentication on pods that select it.
  uint16_t negotiated_profile;
  mbedtls_timing_delay_context handshake_timer;

  char local_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];
  char remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];
  char actual_remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];

  void* user_data;
} DtlsSrtp;

int dtls_srtp_init(DtlsSrtp* dtls_srtp, DtlsSrtpRole role, void* user_data);

// Register an inbound SDES fallback key. media: 0 = video, 1 = audio.
// profile: MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80 (or _32).
// key/key_len: 16-byte master key followed by the 14-byte salt.
// Returns 0 on success, -1 for unsupported slots/keys or srtp_create failure.
int dtls_srtp_set_sdes_key(DtlsSrtp* dtls_srtp, int media, uint16_t profile,
                           const unsigned char* key, int key_len);

// Use the SDES context for outbound media (RTP/RTCP) instead of the
// DTLS-derived context. Only enable when the remote media protocol is SDES.
void dtls_srtp_set_sdes_outbound(DtlsSrtp* dtls_srtp, int enabled);

void dtls_srtp_deinit(DtlsSrtp* dtls_srtp);

int dtls_srtp_create_cert(DtlsSrtp* dtls_srtp);

int dtls_srtp_handshake(DtlsSrtp* dtls_srtp, Address* addr);

void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp);

int dtls_srtp_write(DtlsSrtp* dtls_srtp, const uint8_t* buf, size_t len);

int dtls_srtp_read(DtlsSrtp* dtls_srtp, uint8_t* buf, size_t len);

void dtls_srtp_sctp_to_dtls(DtlsSrtp* dtls_srtp, uint8_t* packet, int bytes);

int dtls_srtp_probe(uint8_t* buf);

int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

void dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

#endif  // DTLS_SRTP_H_
