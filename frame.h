#ifndef WS_FRAME_PARSING_H
#define WS_FRAME_PARSING_H



#include <stdint.h>
#include <stddef.h>

#define FIN_MORE 0
#define FIN_DONE 1

#define OP_CONT 0x0
#define OP_TXT 0x1
#define OP_BIN 0x2

#define OP_PING 0x9
#define OP_PONG 0xA



static inline uint8_t frame_get_fin(const char *buf){
    return (buf[0] >> 7) & 0x01;
}

static inline uint8_t frame_get_opcode(const char *buf){
    return buf[0] & 0x0F;
}





#endif /* WS_FRAME_PARSING_H */
