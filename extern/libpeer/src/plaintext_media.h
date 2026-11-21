/**
 * @file plaintext_media.h
 * @brief Structural checks for pods that send media without SRTP.
 *
 * Some native cloud pods finish DTLS for the data channel and then send RTP and
 * RTCP in the clear even though their SDP advertises UDP/TLS/RTP/SAVPF. The RTP
 * header is never protected, so only the payload distinguishes clear media from
 * a wrong SRTP key: a failed decrypt leaves random bytes where the codec's
 * framing must be.
 *
 * These predicates are pure and side-effect free so the promotion rule can be
 * unit tested without a live transport. They are deliberately strict: a
 * false positive would route unauthenticated bytes into the decoder.
 */
#ifndef PLAINTEXT_MEDIA_H_
#define PLAINTEXT_MEDIA_H_

#include <stdint.h>

// True when payload/len holds one H.264 RTP payload of a legal shape:
// a single NAL unit, an in-range FU-A/FU-B fragment, or a STAP-A whose inner
// length-prefixed units exactly fill the payload.
static inline int plaintext_media_h264_payload(const uint8_t* payload, int len) {
  if (!payload || len < 1) return 0;
  const int first = payload[0];
  if (first & 0x80) return 0;  // forbidden_zero_bit must be clear
  const int nal = first & 0x1f;
  if (nal >= 1 && nal <= 23) {
    // Single NAL unit: needs at least one body byte to be a real slice.
    return len >= 2 ? 1 : 0;
  }
  if (nal == 24) {  // STAP-A
    if (len < 3) return 0;
    int offset = 1;
    while (offset + 2 <= len) {
      const int unit = (payload[offset] << 8) | payload[offset + 1];
      if (unit < 2 || offset + 2 + unit > len) return 0;
      const int inner = payload[offset + 2];
      if (inner & 0x80) return 0;
      const int inner_type = inner & 0x1f;
      if (inner_type < 1 || inner_type > 23) return 0;
      offset += 2 + unit;
    }
    return offset == len ? 1 : 0;
  }
  if (nal == 28 || nal == 29) {  // FU-A / FU-B
    if (len < 3) return 0;
    const int header = payload[1];
    const int fu_type = header & 0x1f;
    if (fu_type < 1 || fu_type > 23) return 0;
    // A start fragment must begin a coded slice, not a parameter set.
    if (header & 0x80) {
      if (fu_type != 1 && fu_type != 2 && fu_type != 5) return 0;
    }
    return 1;
  }
  return 0;
}

// True when the datagram is a plaintext RTP packet whose payload has a legal
// shape for the media it claims to be. Audio codecs (opus/red) have no rigid
// framing, so audio only needs a well-formed header with a non-empty payload.
static inline int plaintext_media_rtp_valid(const uint8_t* packet, int len,
                                            int is_video) {
  if (!packet || len < 13) return 0;
  if ((packet[0] >> 6) != 2) return 0;
  int offset = 12 + ((packet[0] & 0x0f) * 4);
  if (packet[0] & 0x10) {
    if (offset + 4 > len) return 0;
    const int ext_words = (packet[offset + 2] << 8) | packet[offset + 3];
    offset += 4 + ext_words * 4;
  }
  if (offset >= len) return 0;
  if (!is_video) return 1;
  return plaintext_media_h264_payload(packet + offset, len - offset);
}

// True when the datagram is a plaintext RTCP compound: every block declares a
// length that lands on the next block and the last one ends exactly at the
// datagram boundary. SRTCP encrypts those lengths, so this cannot match by
// accident.
static inline int plaintext_media_rtcp_valid(const uint8_t* packet, int len) {
  if (!packet || len < 8) return 0;
  int offset = 0;
  int blocks = 0;
  while (offset + 4 <= len) {
    const int version = packet[offset] >> 6;
    const int type = packet[offset + 1];
    if (version != 2) return 0;
    if (type < 192 || type > 208) return 0;
    const int block_len = (((packet[offset + 2] << 8) | packet[offset + 3]) * 4) + 4;
    if (block_len < 4 || offset + block_len > len) return 0;
    offset += block_len;
    if (++blocks > 16) return 0;
  }
  return offset == len ? 1 : 0;
}

#endif  // PLAINTEXT_MEDIA_H_
