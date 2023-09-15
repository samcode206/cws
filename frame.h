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

static inline uint8_t frame_get_fin(const char *buf) {
  return (buf[0] >> 7) & 0x01;
}

static inline uint8_t frame_get_opcode(const char *buf) {
  return buf[0] & 0x0F;
}

static inline uint32_t frame_payload_get_len(const char *buf) {
  return buf[1] & 0X7F;
}

static inline uint32_t frame_get_mask(const char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static void frame_payload_unmask(const char *src, char *dst, uint8_t *mask,
                                 size_t len) {
  size_t mask_idx = 0;
  for (size_t i = 0; i < len; ++i) {
    dst[i] = src[i] ^ mask[mask_idx];
    mask_idx = (mask_idx + 1) & 3;
  }
}

#endif /* WS_FRAME_PARSING_H */
