#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "address.h"
#include "config.h"
#include "dtls_srtp.h"
#if CONFIG_MBEDTLS_DEBUG
#include "mbedtls/debug.h"
#endif
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "ports.h"
#ifdef __SWITCH__
#include <switch.h>
static int dtls_switch_entropy(void* context, unsigned char* output, size_t length, size_t* written) {
  (void)context;
  randomGet(output, length);
  *written = length;
  return 0;
}
#endif
#include "socket.h"
#include "utils.h"

int dtls_srtp_udp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  UdpSocket* udp_socket = (UdpSocket*)dtls_srtp->user_data;

  int ret = udp_socket_sendto(udp_socket, dtls_srtp->remote_addr, buf, len);

  LOGD("dtls_srtp_udp_send (%d)", ret);

  return ret;
}

int dtls_srtp_udp_recv(void* ctx, uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  UdpSocket* udp_socket = (UdpSocket*)dtls_srtp->user_data;

  int ret;

  while ((ret = udp_socket_recvfrom(udp_socket, &udp_socket->bind_addr, buf, len)) <= 0) {
    ports_sleep_ms(1);
  }

  LOGD("dtls_srtp_udp_recv (%d)", ret);

  return ret;
}

static void dtls_srtp_x509_digest(const mbedtls_x509_crt* crt, char* buf) {
  int i;
  unsigned char digest[32];

  mbedtls_sha256_context sha256_ctx;
  mbedtls_sha256_init(&sha256_ctx);
  mbedtls_sha256_starts(&sha256_ctx, 0);
  mbedtls_sha256_update(&sha256_ctx, crt->raw.p, crt->raw.len);
  mbedtls_sha256_finish(&sha256_ctx, (unsigned char*)digest);
  mbedtls_sha256_free(&sha256_ctx);

  for (i = 0; i < 32; i++) {
    snprintf(buf, 4, "%.2X:", digest[i]);
    buf += 3;
  }

  *(--buf) = '\0';
}

// Do not verify CA
static int dtls_srtp_cert_verify(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
  *flags &= ~(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH | MBEDTLS_X509_BADCERT_BAD_KEY);
  return 0;
}

static int dtls_srtp_selfsign_cert(DtlsSrtp* dtls_srtp) {
  int ret;

  mbedtls_x509write_cert crt;

  unsigned char* cert_buf = NULL;
#if CONFIG_MBEDTLS_2_X
  mbedtls_mpi serial;
#else
  const char* serial = "peer";
#endif
  const char* pers = "dtls_srtp";

  cert_buf = (unsigned char*)malloc(RSA_KEY_LENGTH * 2);
  if (cert_buf == NULL) {
    LOGE("malloc failed");
    return -1;
  }

  ret = mbedtls_ctr_drbg_seed(&dtls_srtp->ctr_drbg, mbedtls_entropy_func, &dtls_srtp->entropy, (const unsigned char*)pers, strlen(pers));
  if (ret != 0) { free(cert_buf); return ret; }

#if CONFIG_DTLS_USE_ECDSA
  ret = mbedtls_pk_setup(&dtls_srtp->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret == 0) ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(dtls_srtp->pkey), mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);
#else
  ret = mbedtls_pk_setup(&dtls_srtp->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret == 0) ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(dtls_srtp->pkey), mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg, RSA_KEY_LENGTH, 65537);
#endif
  if (ret != 0) { free(cert_buf); return ret; }

  mbedtls_x509write_crt_init(&crt);

  mbedtls_x509write_crt_set_subject_key(&crt, &dtls_srtp->pkey);

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

  mbedtls_x509write_crt_set_subject_key(&crt, &dtls_srtp->pkey);

  mbedtls_x509write_crt_set_issuer_key(&crt, &dtls_srtp->pkey);

  mbedtls_x509write_crt_set_subject_name(&crt, "CN=dtls_srtp");

  mbedtls_x509write_crt_set_issuer_name(&crt, "CN=dtls_srtp");

#if CONFIG_MBEDTLS_2_X
  mbedtls_mpi_init(&serial);
  mbedtls_mpi_fill_random(&serial, 16, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);
  ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
  if (ret < 0) {
    LOGE("mbedtls_x509write_crt_set_serial failed -0x%.4x", (unsigned int)-ret);
  }
#else
  mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char*)serial, strlen(serial));
#endif

  mbedtls_x509write_crt_set_validity(&crt, "20180101000000", "20280101000000");

  ret = mbedtls_x509write_crt_pem(&crt, cert_buf, 2 * RSA_KEY_LENGTH, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);

  if (ret < 0) {
    LOGE("mbedtls_x509write_crt_pem failed -0x%.4x", (unsigned int)-ret);
  }

  if (ret == 0) ret = mbedtls_x509_crt_parse(&dtls_srtp->cert, cert_buf, strlen((const char*)cert_buf) + 1);

  mbedtls_x509write_crt_free(&crt);

  free(cert_buf);

  return ret;
}

#if CONFIG_MBEDTLS_DEBUG
static void dtls_srtp_debug(void* ctx, int level, const char* file, int line, const char* str) {
  LOGD("%s:%04d: %s", file, line, str);
}
#endif

static void dtls_srtp_clear_variants(DtlsSrtp* dtls_srtp) {
  for (int i = 0; i < DTLS_SRTP_VARIANT_COUNT; i++) {
    if (dtls_srtp->srtp_in_variant[i]) {
      srtp_dealloc(dtls_srtp->srtp_in_variant[i]);
      dtls_srtp->srtp_in_variant[i] = NULL;
    }
    dtls_srtp->srtp_variant_successes[i] = 0;
  }
  dtls_srtp->srtp_in_variant_preferred = 0;
}

static void dtls_srtp_clear_primary(DtlsSrtp* dtls_srtp) {
  if (dtls_srtp->srtp_in) {
    srtp_dealloc(dtls_srtp->srtp_in);
    dtls_srtp->srtp_in = NULL;
  }
  if (dtls_srtp->srtp_out) {
    srtp_dealloc(dtls_srtp->srtp_out);
    dtls_srtp->srtp_out = NULL;
  }
}

static void dtls_srtp_clear_sdes(DtlsSrtp* dtls_srtp) {
  for (int i = 0; i < DTLS_SRTP_SDES_MEDIA_COUNT; i++) {
    if (dtls_srtp->srtp_in_alt[i]) {
      srtp_dealloc(dtls_srtp->srtp_in_alt[i]);
      dtls_srtp->srtp_in_alt[i] = NULL;
    }
    dtls_srtp->sdes_key_len[i] = 0;
    dtls_srtp->sdes_profile[i] = 0;
  }
  if (dtls_srtp->srtp_out_sdes) {
    srtp_dealloc(dtls_srtp->srtp_out_sdes);
    dtls_srtp->srtp_out_sdes = NULL;
  }
  dtls_srtp->srtp_in_alt_count = 0;
  dtls_srtp->srtp_in_preferred = 0;
  dtls_srtp->sdes_outbound = 0;
  dtls_srtp_clear_variants(dtls_srtp);
}

int dtls_srtp_init(DtlsSrtp* dtls_srtp, DtlsSrtpRole role, void* user_data) {
  static const mbedtls_ssl_srtp_profile default_profiles[] = {
      MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
      MBEDTLS_TLS_SRTP_UNSET};

  dtls_srtp->role = role;
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
  dtls_srtp->negotiated_profile = MBEDTLS_TLS_SRTP_UNSET;
  dtls_srtp->user_data = user_data;
  dtls_srtp->udp_send = dtls_srtp_udp_send;
  dtls_srtp->udp_recv = dtls_srtp_udp_recv;
  dtls_srtp->plaintext_outbound = 0;
  dtls_srtp_clear_sdes(dtls_srtp);

  mbedtls_ssl_config_init(&dtls_srtp->conf);
  mbedtls_ssl_init(&dtls_srtp->ssl);

  mbedtls_x509_crt_init(&dtls_srtp->cert);
  mbedtls_pk_init(&dtls_srtp->pkey);
  mbedtls_entropy_init(&dtls_srtp->entropy);
  mbedtls_ctr_drbg_init(&dtls_srtp->ctr_drbg);
  mbedtls_ssl_cookie_init(&dtls_srtp->cookie_ctx);
  memset(&dtls_srtp->handshake_timer, 0, sizeof(dtls_srtp->handshake_timer));
  // Defaults first, then explicit certificate/authentication policy.
  int ret = mbedtls_ssl_config_defaults(&dtls_srtp->conf,
      role == DTLS_SRTP_ROLE_SERVER ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) goto failed;
#ifdef __SWITCH__
  ret = mbedtls_entropy_add_source(&dtls_srtp->entropy, dtls_switch_entropy, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (ret != 0) goto failed;
#endif
#if CONFIG_MBEDTLS_DEBUG
  mbedtls_debug_set_threshold(3);
  mbedtls_ssl_conf_dbg(&dtls_srtp->conf, dtls_srtp_debug, NULL);
#endif
  ret = dtls_srtp_selfsign_cert(dtls_srtp);
  if (ret != 0) goto failed;

  mbedtls_ssl_conf_verify(&dtls_srtp->conf, dtls_srtp_cert_verify, NULL);

  mbedtls_ssl_conf_authmode(&dtls_srtp->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

  mbedtls_ssl_conf_ca_chain(&dtls_srtp->conf, &dtls_srtp->cert, NULL);

  ret = mbedtls_ssl_conf_own_cert(&dtls_srtp->conf, &dtls_srtp->cert, &dtls_srtp->pkey);
  if (ret != 0) goto failed;

  mbedtls_ssl_conf_rng(&dtls_srtp->conf, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);

  mbedtls_ssl_conf_read_timeout(&dtls_srtp->conf, 1000);

  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    ret = mbedtls_ssl_cookie_setup(&dtls_srtp->cookie_ctx, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);
    if (ret != 0) goto failed;

    mbedtls_ssl_conf_dtls_cookies(&dtls_srtp->conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &dtls_srtp->cookie_ctx);

  }

  dtls_srtp_x509_digest(&dtls_srtp->cert, dtls_srtp->local_fingerprint);

  LOGD("local fingerprint: %s", dtls_srtp->local_fingerprint);

  ret = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&dtls_srtp->conf, default_profiles);
  if (ret != 0) goto failed;

  mbedtls_ssl_conf_srtp_mki_value_supported(&dtls_srtp->conf, MBEDTLS_SSL_DTLS_SRTP_MKI_UNSUPPORTED);

  mbedtls_ssl_conf_cert_req_ca_list(&dtls_srtp->conf, MBEDTLS_SSL_CERT_REQ_CA_LIST_DISABLED);

  ret = mbedtls_ssl_setup(&dtls_srtp->ssl, &dtls_srtp->conf);
  if (ret != 0) goto failed;
  // Register once: set_timer_cb cancels the timer. Repeating this on every
  // nonblocking poll prevents a lost DTLS flight from ever being retransmitted.
  mbedtls_ssl_set_timer_cb(&dtls_srtp->ssl, &dtls_srtp->handshake_timer,
                          mbedtls_timing_set_delay, mbedtls_timing_get_delay);
  dtls_srtp->initialized = 1;

  return 0;
failed:
  dtls_srtp_deinit(dtls_srtp);
  return ret;
}

void dtls_srtp_deinit(DtlsSrtp* dtls_srtp) {
  mbedtls_ssl_free(&dtls_srtp->ssl);
  mbedtls_ssl_config_free(&dtls_srtp->conf);

  mbedtls_x509_crt_free(&dtls_srtp->cert);
  mbedtls_pk_free(&dtls_srtp->pkey);
  mbedtls_entropy_free(&dtls_srtp->entropy);
  mbedtls_ctr_drbg_free(&dtls_srtp->ctr_drbg);

  mbedtls_ssl_cookie_free(&dtls_srtp->cookie_ctx);

  dtls_srtp_clear_primary(dtls_srtp);
  dtls_srtp_clear_sdes(dtls_srtp);
  dtls_srtp->initialized = 0;
}

static void dtls_srtp_apply_negotiated_profile(DtlsSrtp* dtls_srtp, srtp_policy_t* policy) {
  srtp_crypto_policy_set_rtp_default(&policy->rtp);
  srtp_crypto_policy_set_rtcp_default(&policy->rtcp);
  switch (dtls_srtp->negotiated_profile) {
    case MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32:
      // RFC 4568 short-tag profile: 4-byte RTP auth tag. SRTCP keeps the
      // 10-byte default tag (the 32-bit profile is not valid for SRTCP).
      srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy->rtp);
      break;
    case MBEDTLS_TLS_SRTP_NULL_HMAC_SHA1_80:
      srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy->rtp);
      srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy->rtcp);
      break;
    default:
      break;
  }
}

// Build one inbound probe context from a key+salt pair. The primary context
// keeps the negotiated profile; probes only differ in direction, auth-tag
// length or confidentiality mode so a successful tag check identifies the
// pod's actual layout.
static srtp_t dtls_srtp_create_inbound_probe(DtlsSrtp* dtls_srtp, int index,
                                             const uint8_t* key,
                                             const uint8_t* salt,
                                             int sha1_32, int auth_only) {
  srtp_policy_t policy;
  memset(&policy, 0, sizeof(policy));
  srtp_crypto_policy_set_rtp_default(&policy.rtp);
  srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
  if (sha1_32) {
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
  }
  if (auth_only) {
    policy.rtp.sec_serv = sec_serv_auth;
    policy.rtcp.sec_serv = sec_serv_auth;
  }
  memcpy(dtls_srtp->srtp_variant_key[index], key, SRTP_MASTER_KEY_LENGTH);
  memcpy(dtls_srtp->srtp_variant_key[index] + SRTP_MASTER_KEY_LENGTH, salt,
         SRTP_MASTER_SALT_LENGTH);
  policy.ssrc.type = ssrc_any_inbound;
  policy.key = dtls_srtp->srtp_variant_key[index];
  policy.next = NULL;
  srtp_t context = NULL;
  if (srtp_create(&context, &policy) != srtp_err_status_ok) {
    LOGD("Error creating inbound SRTP probe %d", index);
    return NULL;
  }
  return context;
}

static void dtls_srtp_create_inbound_probes(DtlsSrtp* dtls_srtp,
                                            const uint8_t* client_key,
                                            const uint8_t* server_key,
                                            const uint8_t* client_salt,
                                            const uint8_t* server_salt) {
  const uint8_t* local_key = dtls_srtp->role == DTLS_SRTP_ROLE_SERVER
      ? server_key : client_key;
  const uint8_t* local_salt = dtls_srtp->role == DTLS_SRTP_ROLE_SERVER
      ? server_salt : client_salt;
  const uint8_t* remote_key = dtls_srtp->role == DTLS_SRTP_ROLE_SERVER
      ? client_key : server_key;
  const uint8_t* remote_salt = dtls_srtp->role == DTLS_SRTP_ROLE_SERVER
      ? client_salt : server_salt;

  dtls_srtp_clear_variants(dtls_srtp);
  dtls_srtp->srtp_in_variant[0] = dtls_srtp_create_inbound_probe(
      dtls_srtp, 0, remote_key, remote_salt, 1, 0);
  dtls_srtp->srtp_in_variant[1] = dtls_srtp_create_inbound_probe(
      dtls_srtp, 1, local_key, local_salt, 0, 0);
  dtls_srtp->srtp_in_variant[2] = dtls_srtp_create_inbound_probe(
      dtls_srtp, 2, local_key, local_salt, 1, 0);
  // RTCP-only: the remote may mark SRTCP as authenticated-but-not-encrypted.
  dtls_srtp->srtp_in_variant[DTLS_SRTP_VARIANT_AUTH_ONLY] =
      dtls_srtp_create_inbound_probe(
          dtls_srtp, DTLS_SRTP_VARIANT_AUTH_ONLY, remote_key, remote_salt, 0, 1);
  dtls_srtp->srtp_in_variant[4] = dtls_srtp_create_inbound_probe(
      dtls_srtp, 4, remote_key, client_salt, 0, 0);
  dtls_srtp->srtp_in_variant[5] = dtls_srtp_create_inbound_probe(
      dtls_srtp, 5, local_key, server_salt, 0, 0);
}

static int dtls_srtp_key_derivation(DtlsSrtp* dtls_srtp, const unsigned char* master_secret, size_t secret_len, const unsigned char* randbytes, size_t randbytes_len, mbedtls_tls_prf_types tls_prf_type) {
  int ret;
  const char* dtls_srtp_label = "EXTRACTOR-dtls_srtp";
  uint8_t key_material[DTLS_SRTP_KEY_MATERIAL_LENGTH];
  // The use_srtp profile is selected in ServerHello, before the master secret
  // is derived, so it is available here. Recording it also lets the client
  // diagnose pods that answer with a different profile than the default.
  mbedtls_dtls_srtp_info srtp_info;
  memset(&srtp_info, 0, sizeof(srtp_info));
  mbedtls_ssl_get_dtls_srtp_negotiation_result(&dtls_srtp->ssl, &srtp_info);
  dtls_srtp->negotiated_profile = srtp_info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile);
  LOGI("DTLS-SRTP keys role=%d profile=%u prf=%d",
       (int)dtls_srtp->role, (unsigned)dtls_srtp->negotiated_profile,
       (int)tls_prf_type);
  // The export callback can run for both the handshake and application
  // transforms; replace the previous contexts instead of leaking them.
  dtls_srtp_clear_primary(dtls_srtp);
  // Export keying material
  if ((ret = mbedtls_ssl_tls_prf(tls_prf_type, master_secret, secret_len, dtls_srtp_label,
                                 randbytes, randbytes_len, key_material, sizeof(key_material))) != 0) {
    LOGE("mbedtls_ssl_tls_prf failed(%d)", ret);
    return ret;
  }

#if 0
  int i, j;
  printf("    DTLS-SRTP key material is:");
  for (j = 0; j < sizeof(key_material); j++) {
    if (j % 8 == 0) {
      printf("\n    ");
    }
    printf("%02x ", key_material[j]);
  }
  printf("\n");

  /* produce a less readable output used to perform automatic checks
   * - compare client and server output
   * - interop test with openssl which client produces this kind of output
   */
  printf("    Keying material: ");
  for (j = 0; j < sizeof(key_material); j++) {
    printf("%02X", key_material[j]);
  }
  printf("\n");
#endif

  const uint8_t* client_key = key_material;
  const uint8_t* server_key = client_key + SRTP_MASTER_KEY_LENGTH;
  const uint8_t* client_salt = server_key + SRTP_MASTER_KEY_LENGTH;
  const uint8_t* server_salt = client_salt + SRTP_MASTER_SALT_LENGTH;
  const uint8_t *local_key, *remote_key, *local_salt, *remote_salt;
  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    local_key = server_key;
    local_salt = server_salt;
    remote_key = client_key;
    remote_salt = client_salt;
  } else {
    local_key = client_key;
    local_salt = client_salt;
    remote_key = server_key;
    remote_salt = server_salt;
  }
  // derive inbounds keys

  memset(&dtls_srtp->remote_policy, 0, sizeof(dtls_srtp->remote_policy));

  dtls_srtp_apply_negotiated_profile(dtls_srtp, &dtls_srtp->remote_policy);

  memcpy(dtls_srtp->remote_policy_key, remote_key, SRTP_MASTER_KEY_LENGTH);
  memcpy(dtls_srtp->remote_policy_key + SRTP_MASTER_KEY_LENGTH, remote_salt, SRTP_MASTER_SALT_LENGTH);

  dtls_srtp->remote_policy.ssrc.type = ssrc_any_inbound;
  dtls_srtp->remote_policy.key = dtls_srtp->remote_policy_key;
  dtls_srtp->remote_policy.next = NULL;

  if (srtp_create(&dtls_srtp->srtp_in, &dtls_srtp->remote_policy) != srtp_err_status_ok) {
    LOGD("Error creating inbound SRTP session for component");
    return -1;
  }

  LOGI("Created inbound SRTP session");

  // derive outbounds keys
  memset(&dtls_srtp->local_policy, 0, sizeof(dtls_srtp->local_policy));

  dtls_srtp_apply_negotiated_profile(dtls_srtp, &dtls_srtp->local_policy);

  memcpy(dtls_srtp->local_policy_key, local_key, SRTP_MASTER_KEY_LENGTH);
  memcpy(dtls_srtp->local_policy_key + SRTP_MASTER_KEY_LENGTH, local_salt, SRTP_MASTER_SALT_LENGTH);

  dtls_srtp->local_policy.ssrc.type = ssrc_any_outbound;
  dtls_srtp->local_policy.key = dtls_srtp->local_policy_key;
  dtls_srtp->local_policy.next = NULL;

  if (srtp_create(&dtls_srtp->srtp_out, &dtls_srtp->local_policy) != srtp_err_status_ok) {
    LOGE("Error creating outbound SRTP session");
    return -1;
  }

  LOGI("Created outbound SRTP session");
  dtls_srtp_create_inbound_probes(dtls_srtp, client_key, server_key,
                                  client_salt, server_salt);
  dtls_srtp->state = DTLS_SRTP_STATE_CONNECTED;
  return 0;
}

#if CONFIG_MBEDTLS_2_X
static int dtls_srtp_key_derivation_cb(void* context,
                                       const unsigned char* ms,
                                       const unsigned char* kb,
                                       size_t maclen,
                                       size_t keylen,
                                       size_t ivlen,
                                       const unsigned char client_random[32],
                                       const unsigned char server_random[32],
                                       mbedtls_tls_prf_types tls_prf_type) {
#else
static void dtls_srtp_key_derivation_cb(void* context,
                                        mbedtls_ssl_key_export_type secret_type,
                                        const unsigned char* secret,
                                        size_t secret_len,
                                        const unsigned char client_random[32],
                                        const unsigned char server_random[32],
                                        mbedtls_tls_prf_types tls_prf_type) {
#endif
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)context;

  unsigned char master_secret[48];
  unsigned char randbytes[64];

  memcpy(randbytes, client_random, 32);
  memcpy(randbytes + 32, server_random, 32);

#if CONFIG_MBEDTLS_2_X
  memcpy(master_secret, ms, sizeof(master_secret));
  return dtls_srtp_key_derivation(dtls_srtp, master_secret, sizeof(master_secret), randbytes, sizeof(randbytes), tls_prf_type);
#else
  memcpy(master_secret, secret, sizeof(master_secret));
  dtls_srtp_key_derivation(dtls_srtp, master_secret, sizeof(master_secret), randbytes, sizeof(randbytes), tls_prf_type);
#endif
}

static int dtls_srtp_do_handshake(DtlsSrtp* dtls_srtp) {
#if CONFIG_MBEDTLS_2_X
  mbedtls_ssl_conf_export_keys_ext_cb(&dtls_srtp->conf, dtls_srtp_key_derivation_cb, dtls_srtp);
#else
  mbedtls_ssl_set_export_keys_cb(&dtls_srtp->ssl, dtls_srtp_key_derivation_cb, dtls_srtp);
#endif

  mbedtls_ssl_set_bio(&dtls_srtp->ssl, dtls_srtp, dtls_srtp->udp_send, dtls_srtp->udp_recv, NULL);

  // Do exactly one non-blocking handshake step. The peer connection loop calls
  // us again after polling the UDP socket, preserving MbedTLS' DTLS state.
  return mbedtls_ssl_handshake(&dtls_srtp->ssl);
}

static int dtls_srtp_handshake_server(DtlsSrtp* dtls_srtp) {
  if (dtls_srtp->state == DTLS_SRTP_STATE_INIT) {
    unsigned char client_ip[] = "test";
    mbedtls_ssl_set_client_transport_id(&dtls_srtp->ssl, client_ip, sizeof(client_ip));
    dtls_srtp->state = DTLS_SRTP_STATE_HANDSHAKE;
  }
  int ret = dtls_srtp_do_handshake(dtls_srtp);
  if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
    ret = mbedtls_ssl_session_reset(&dtls_srtp->ssl);
    dtls_srtp->state = DTLS_SRTP_STATE_INIT;
    return ret != 0 ? ret : MBEDTLS_ERR_SSL_WANT_READ;
  }
  return ret;
}

static int dtls_srtp_handshake_client(DtlsSrtp* dtls_srtp) {
  int ret;

  ret = dtls_srtp_do_handshake(dtls_srtp);
  if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
    LOGE("failed! mbedtls_ssl_handshake returned -0x%.4x\n\n", (unsigned int)-ret);
  }

  if (ret == 0)
    LOGD("DTLS client handshake done");

  return ret;
}

int dtls_srtp_handshake(DtlsSrtp* dtls_srtp, Address* addr) {
  int ret;
  dtls_srtp->remote_addr = addr;

  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    ret = dtls_srtp_handshake_server(dtls_srtp);
  } else {
    ret = dtls_srtp_handshake_client(dtls_srtp);
  }

  // A peer certificate exists only once the DTLS handshake completes. Treat
  // WANT_READ/WANT_WRITE as normal progress instead of a fingerprint failure.
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    return ret;
  if (ret != 0)
    return ret;

  const mbedtls_x509_crt* remote_crt;
  if ((remote_crt = mbedtls_ssl_get_peer_cert(&dtls_srtp->ssl)) != NULL) {
    dtls_srtp_x509_digest(remote_crt, dtls_srtp->actual_remote_fingerprint);

    if (strncmp(dtls_srtp->remote_fingerprint, dtls_srtp->actual_remote_fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH) != 0) {
      LOGE("Remote certificate fingerprint mismatch");
      return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

  } else {
    LOGE("no remote fingerprint");
    return MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE;
  }

  mbedtls_dtls_srtp_info dtls_srtp_negotiation_result;
  mbedtls_ssl_get_dtls_srtp_negotiation_result(&dtls_srtp->ssl, &dtls_srtp_negotiation_result);

  return ret;
}

void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp) {
  // A new description may carry different SDES keys. Drop the old contexts
  // even when the DTLS session was never fully initialized.
  dtls_srtp_clear_sdes(dtls_srtp);
  if (!dtls_srtp->initialized) {
    dtls_srtp->state = DTLS_SRTP_STATE_INIT;
    return;
  }

  dtls_srtp_clear_primary(dtls_srtp);
  dtls_srtp->plaintext_outbound = 0;

  mbedtls_ssl_session_reset(&dtls_srtp->ssl);
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
}

int dtls_srtp_write(DtlsSrtp* dtls_srtp, const unsigned char* buf, size_t len) {
  int ret;

  do {
    ret = mbedtls_ssl_write(&dtls_srtp->ssl, buf, len);

  } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
  return ret;
}

int dtls_srtp_read(DtlsSrtp* dtls_srtp, unsigned char* buf, size_t len) {
  memset(buf, 0, len);
  return mbedtls_ssl_read(&dtls_srtp->ssl, buf, len);
}

int dtls_srtp_probe(uint8_t* buf) {
  if (buf == NULL)
    return 0;

  LOGD("DTLS content type: %d", buf[0]);
  // Handle change-cipher-spec, alert, handshake and application-data records.
  return (buf[0] >= 0x14 && buf[0] <= 0x17);
}

int dtls_srtp_set_sdes_key(DtlsSrtp* dtls_srtp, int media, uint16_t profile,
                           const unsigned char* key, int key_len) {
  if (!dtls_srtp || media < 0 || media >= DTLS_SRTP_SDES_MEDIA_COUNT || !key ||
      key_len != DTLS_SRTP_SDES_KEY_LENGTH)
    return -1;
  if (profile != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80 &&
      profile != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32)
    return -1;

  if (dtls_srtp->srtp_in_alt[media]) {
    srtp_dealloc(dtls_srtp->srtp_in_alt[media]);
    dtls_srtp->srtp_in_alt[media] = NULL;
  }
  // The outbound SDES context may have been built from this slot's old key.
  // Drop it; peer_connection_apply_remote_sdes() re-enables it after all keys
  // have been registered.
  if (dtls_srtp->srtp_out_sdes) {
    srtp_dealloc(dtls_srtp->srtp_out_sdes);
    dtls_srtp->srtp_out_sdes = NULL;
    dtls_srtp->sdes_outbound = 0;
  }
  memcpy(dtls_srtp->sdes_key[media], key, DTLS_SRTP_SDES_KEY_LENGTH);
  dtls_srtp->sdes_key_len[media] = key_len;
  dtls_srtp->sdes_profile[media] = profile;

  srtp_policy_t policy;
  memset(&policy, 0, sizeof(policy));
  srtp_crypto_policy_set_rtp_default(&policy.rtp);
  srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
  if (profile == MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32)
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
  policy.ssrc.type = ssrc_any_inbound;
  policy.key = dtls_srtp->sdes_key[media];
  policy.next = NULL;

  if (srtp_create(&dtls_srtp->srtp_in_alt[media], &policy) != srtp_err_status_ok) {
    LOGD("Error creating SDES inbound SRTP session media=%d", media);
    dtls_srtp->srtp_in_alt[media] = NULL;
    dtls_srtp->sdes_key_len[media] = 0;
    dtls_srtp->sdes_profile[media] = 0;
    return -1;
  }
  if (dtls_srtp->srtp_in_alt_count < media + 1)
    dtls_srtp->srtp_in_alt_count = media + 1;
  LOGI("Created SDES inbound SRTP session media=%d profile=%u", media,
       (unsigned)profile);
  return 0;
}

void dtls_srtp_set_sdes_outbound(DtlsSrtp* dtls_srtp, int enabled) {
  if (!dtls_srtp) return;
  if (!enabled) {
    if (dtls_srtp->srtp_out_sdes) {
      srtp_dealloc(dtls_srtp->srtp_out_sdes);
      dtls_srtp->srtp_out_sdes = NULL;
    }
    dtls_srtp->sdes_outbound = 0;
    return;
  }

  int media = dtls_srtp->sdes_key_len[0] > 0
      ? 0 : (dtls_srtp->sdes_key_len[1] > 0 ? 1 : -1);
  if (media < 0) {
    dtls_srtp->sdes_outbound = 0;
    return;
  }
  if (!dtls_srtp->srtp_out_sdes) {
    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    if (dtls_srtp->sdes_profile[media] == MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32)
      srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
    policy.ssrc.type = ssrc_any_outbound;
    policy.key = dtls_srtp->sdes_key[media];
    policy.next = NULL;
    if (srtp_create(&dtls_srtp->srtp_out_sdes, &policy) != srtp_err_status_ok) {
      LOGD("Error creating SDES outbound SRTP session media=%d", media);
      dtls_srtp->srtp_out_sdes = NULL;
      dtls_srtp->sdes_outbound = 0;
      return;
    }
    LOGI("Created SDES outbound SRTP session media=%d", media);
  }
  dtls_srtp->sdes_outbound = 1;
}

typedef struct DtlsSrtpTry {
  int kind;
  int index;
} DtlsSrtpTry;

static int dtls_srtp_unprotect_with_fallback(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                             int* bytes, int rtcp) {
  if (!dtls_srtp || !packet || !bytes || *bytes <= 0)
    return srtp_err_status_bad_param;

  const int original_len = *bytes;
  uint8_t stack_original[DTLS_SRTP_SDES_SCRATCH_LENGTH];
  uint8_t* original = original_len <= (int)sizeof(stack_original)
      ? stack_original
      : (uint8_t*)malloc((size_t)original_len);
  if (!original) return srtp_err_status_alloc_fail;
  memcpy(original, packet, (size_t)original_len);

  // libsrtp rewrites the buffer even when authentication fails. Every attempt
  // starts from the original bytes so a failed key cannot corrupt the packet
  // before the next candidate sees it.
  enum { SRTP_TRY_PRIMARY, SRTP_TRY_VARIANT, SRTP_TRY_SDES };
  DtlsSrtpTry order[1 + DTLS_SRTP_VARIANT_COUNT + DTLS_SRTP_SDES_MEDIA_COUNT];
  int order_count = 0;
  if (dtls_srtp->srtp_in_variant_preferred > 0 &&
      dtls_srtp->srtp_in_variant_preferred <= DTLS_SRTP_VARIANT_COUNT &&
      dtls_srtp->srtp_in_variant[dtls_srtp->srtp_in_variant_preferred - 1])
    order[order_count++] = (DtlsSrtpTry){SRTP_TRY_VARIANT,
        dtls_srtp->srtp_in_variant_preferred - 1};
  if (dtls_srtp->srtp_in_preferred > 0 &&
      dtls_srtp->srtp_in_preferred <= dtls_srtp->srtp_in_alt_count &&
      dtls_srtp->srtp_in_alt[dtls_srtp->srtp_in_preferred - 1])
    order[order_count++] = (DtlsSrtpTry){SRTP_TRY_SDES,
        dtls_srtp->srtp_in_preferred - 1};
  if (dtls_srtp->srtp_in)
    order[order_count++] = (DtlsSrtpTry){SRTP_TRY_PRIMARY, 0};
  for (int i = 0; i < DTLS_SRTP_VARIANT_COUNT; i++) {
    if (!dtls_srtp->srtp_in_variant[i] ||
        i == dtls_srtp->srtp_in_variant_preferred - 1)
      continue;
    // Variant 3 skips decryption, so it is only safe for RTCP (where it
    // probes the E=0 authenticated-only mode) and never for RTP payloads.
    if (!rtcp && i == DTLS_SRTP_VARIANT_AUTH_ONLY)
      continue;
    order[order_count++] = (DtlsSrtpTry){SRTP_TRY_VARIANT, i};
  }
  for (int i = 0; i < dtls_srtp->srtp_in_alt_count; i++) {
    if (!dtls_srtp->srtp_in_alt[i] || i == dtls_srtp->srtp_in_preferred - 1)
      continue;
    order[order_count++] = (DtlsSrtpTry){SRTP_TRY_SDES, i};
  }
  if (order_count == 0) {
    if (original != stack_original) free(original);
    return srtp_err_status_no_ctx;
  }

  int ret = srtp_err_status_auth_fail;
  for (int k = 0; k < order_count; k++) {
    const int kind = order[k].kind;
    const int index = order[k].index;
    srtp_t context = kind == SRTP_TRY_PRIMARY
        ? dtls_srtp->srtp_in
        : (kind == SRTP_TRY_VARIANT ? dtls_srtp->srtp_in_variant[index]
                                    : dtls_srtp->srtp_in_alt[index]);
    int len = original_len;
    memcpy(packet, original, (size_t)original_len);
    ret = rtcp ? srtp_unprotect_rtcp(context, packet, &len)
               : srtp_unprotect(context, packet, &len);
    if (ret == srtp_err_status_ok) {
      *bytes = len;
      if (kind == SRTP_TRY_PRIMARY) {
        dtls_srtp->srtp_in_preferred = 0;
        dtls_srtp->srtp_in_variant_preferred = 0;
      } else if (kind == SRTP_TRY_VARIANT) {
        dtls_srtp->srtp_in_variant_preferred = index + 1;
        dtls_srtp->srtp_in_preferred = 0;
        ++dtls_srtp->srtp_variant_successes[index];
      } else {
        dtls_srtp->srtp_in_preferred = index + 1;
        dtls_srtp->srtp_in_variant_preferred = 0;
        ++dtls_srtp->srtp_in_alt_successes;
      }
      if (original != stack_original) free(original);
      return ret;
    }
    if (kind == SRTP_TRY_PRIMARY) ++dtls_srtp->srtp_in_primary_failures;
  }

  memcpy(packet, original, (size_t)original_len);
  *bytes = original_len;
  if (original != stack_original) free(original);
  return ret;
}

int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return dtls_srtp_unprotect_with_fallback(dtls_srtp, packet, bytes, 0);
}

int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return dtls_srtp_unprotect_with_fallback(dtls_srtp, packet, bytes, 1);
}

void dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  if (dtls_srtp->plaintext_outbound) return;
  srtp_t context = (dtls_srtp->sdes_outbound && dtls_srtp->srtp_out_sdes)
      ? dtls_srtp->srtp_out_sdes : dtls_srtp->srtp_out;
  if (context) srtp_protect(context, packet, bytes);
}

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  if (dtls_srtp->plaintext_outbound) return 0;
  srtp_t context = (dtls_srtp->sdes_outbound && dtls_srtp->srtp_out_sdes)
      ? dtls_srtp->srtp_out_sdes : dtls_srtp->srtp_out;
  return context ? srtp_protect_rtcp(context, packet, bytes)
                 : srtp_err_status_no_ctx;
}
