#ifndef WS_FRAME_PARSING_H
#define WS_FRAME_PARSING_H

#include <stddef.h>
#include <stdint.h>

#define FIN_MORE 0
#define FIN_DONE 1

#define OP_CONT 0x0
#define OP_TXT 0x1
#define OP_BIN 0x2

#define OP_PING 0x9
#define OP_PONG 0xA

#define PAYLOAD_LEN_16 126
#define PAYLOAD_LEN_64 127

static inline uint8_t frame_get_fin(const unsigned char *buf) {
  return (buf[0] >> 7) & 0x01;
}

static inline uint8_t frame_get_opcode(const unsigned char  *buf) {
  return buf[0] & 0x0F;
}

static inline size_t frame_payload_get_len126(const unsigned char  *buf) {
  return (buf[2] << 8) | buf[3];
}

static inline size_t frame_payload_get_len127(const unsigned char *buf) {
  return ((uint64_t)buf[2] << 56) | ((uint64_t)buf[3] << 48) |
         ((uint64_t)buf[4] << 40) | ((uint64_t)buf[5] << 32) |
         ((uint64_t)buf[6] << 24) | ((uint64_t)buf[7] << 16) |
         ((uint64_t)buf[8] << 8) | (uint64_t)buf[9];
}

static inline size_t frame_payload_get_len(const unsigned char *buf) {
  return buf[1] & 0X7F;
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline int frame_get_mask_offset(const unsigned char *buf, size_t n) {
    return 2 + ((n > 125) * 2) + ((n > 0xFFFF) * 6);
}

static void frame_payload_unmask(const unsigned char *src, unsigned char *dst, uint8_t *mask,
                                 size_t len) {
  size_t mask_idx = 0;
  for (size_t i = 0; i < len; ++i) {
    dst[i] = src[i] ^ mask[mask_idx];
    mask_idx = (mask_idx + 1) & 3;
  }
}

#endif /* WS_FRAME_PARSING_H */
