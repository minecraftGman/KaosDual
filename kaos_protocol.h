/*
 * kaos_protocol.h
 * Shared UART protocol between ESP32 (manager) and Pi Pico (USB HID portal).
 *
 * Copy this file into both firmware projects.
 *
 * Physical wiring (3 wires):
 *   ESP32 GPIO17 (TX2) ──→ Pico GPIO5  (RX1, UART1)
 *   ESP32 GPIO16 (RX2) ←── Pico GPIO4  (TX1, UART1)
 *   ESP32 GND          ─── Pico GND
 *
 * Baud rate: 921600
 *
 * Frame format:
 *   [0xAB][LEN:1][TYPE:1][PAYLOAD: LEN bytes][XOR:1]
 *
 *   XOR = LEN ^ TYPE ^ all payload bytes
 *
 * ── ESP32 → Pico ──────────────────────────────────────────
 *   MSG_LOAD    slot(1) + raw_dump(1024)   Load a Skylander into a slot
 *   MSG_UNLOAD  slot(1)                    Unload a slot
 *
 * ── Pico → ESP32 ──────────────────────────────────────────
 *   MSG_WRITE_BACK  slot(1) + raw_dump(1024)  Game wrote to tag; save to SD
 *   MSG_PICO_READY  (no payload)              Pico booted and is ready
 */

#pragma once
#ifndef KAOS_PROTOCOL_H
#define KAOS_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define KAOS_SOF            0xAB
#define KAOS_BAUD           115200
#define SKYLANDER_DUMP_SIZE 1024

typedef enum {
    MSG_LOAD        = 0x01,
    MSG_UNLOAD      = 0x02,
    MSG_SET_PORTAL_TYPE = 0x03,
    MSG_ESP_READY   = 0x04,   /* ESP32 → Pico: ESP32 just booted, please re-send PICO_READY */
    MSG_WRITE_BACK  = 0x10,
    MSG_PICO_READY  = 0x11,
    MSG_DEBUG       = 0x20,
} kaos_msg_t;

/* Build a frame into out_buf (must be >= payload_len + 4).
 * Returns total bytes written. */
static inline int kaos_build_frame(uint8_t *out, kaos_msg_t type,
                                   const uint8_t *payload, uint16_t len) {
    /* We only support payloads up to 255 bytes in the 1-byte LEN field,
     * EXCEPT for MSG_LOAD / MSG_WRITE_BACK which carry 1025 bytes.
     * For those we use a 2-byte length extension: LEN=0xFF signals 16-bit. */
    if (len <= 0xFE) {
        out[0] = KAOS_SOF;
        out[1] = (uint8_t)len;
        out[2] = (uint8_t)type;
        uint8_t xor = (uint8_t)len ^ (uint8_t)type;
        for (int i = 0; i < len; i++) { out[3+i] = payload[i]; xor ^= payload[i]; }
        out[3+len] = xor;
        return 4 + len;
    } else {
        /* Extended length: [SOF][0xFF][LEN_HI][LEN_LO][TYPE][PAYLOAD][XOR] */
        out[0] = KAOS_SOF;
        out[1] = 0xFF;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)(len & 0xFF);
        out[4] = (uint8_t)type;
        uint8_t xor = 0xFF ^ out[2] ^ out[3] ^ (uint8_t)type;
        for (int i = 0; i < len; i++) { out[5+i] = payload[i]; xor ^= payload[i]; }
        out[5+len] = xor;
        return 6 + len;
    }
}

/* -----------------------------------------------------------------------
 * Incremental frame parser — feed one byte at a time.
 * Returns 1 when a complete valid frame is ready, 0 otherwise.
 * Call kaos_parser_init() once before use.
 * ----------------------------------------------------------------------- */
typedef struct {
    enum { KP_SOF, KP_LEN, KP_LEN_HI, KP_LEN_LO, KP_TYPE, KP_PAYLOAD, KP_XOR } state;
    uint16_t len;
    uint8_t  type;
    uint8_t  xor;
    uint16_t pos;
    uint8_t  buf[SKYLANDER_DUMP_SIZE + 4];
    int      extended;
} kaos_parser_t;

static inline void kaos_parser_init(kaos_parser_t *p) {
    memset(p, 0, sizeof(*p));
}

static inline int kaos_parser_feed(kaos_parser_t *p, uint8_t b,
                                    kaos_msg_t *out_type,
                                    uint8_t **out_payload,
                                    uint16_t *out_len) {
    switch (p->state) {
        case KP_SOF:
            if (b == KAOS_SOF) p->state = KP_LEN;
            break;
        case KP_LEN:
            if (b == 0xFF) { p->extended = 1; p->xor = 0xFF; p->state = KP_LEN_HI; }
            else           { p->extended = 0; p->len = b; p->xor = b; p->pos = 0;
                             p->state = KP_TYPE; }
            break;
        case KP_LEN_HI:
            p->len = (uint16_t)(b << 8); p->xor ^= b; p->state = KP_LEN_LO; break;
        case KP_LEN_LO:
            p->len |= b; p->xor ^= b; p->pos = 0; p->state = KP_TYPE; break;
        case KP_TYPE:
            p->type = b; p->xor ^= b;
            p->state = (p->len == 0) ? KP_XOR : KP_PAYLOAD;
            break;
        case KP_PAYLOAD:
            p->buf[p->pos++] = b; p->xor ^= b;
            if (p->pos >= p->len) p->state = KP_XOR;
            break;
        case KP_XOR:
            p->state = KP_SOF;
            if (b == p->xor) {
                *out_type    = (kaos_msg_t)p->type;
                *out_payload = p->buf;
                *out_len     = p->len;
                return 1;
            }
            break;
    }
    return 0;
}

#endif /* KAOS_PROTOCOL_H */
