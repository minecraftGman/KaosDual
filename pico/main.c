/*
 * KAOS Pico — main.c  (clean rewrite)
 *
 * Protocol based on confirmed-working RPCS3/Dolphin implementation.
 *
 * Key facts:
 *   - Commands arrive via HID SET_REPORT (control EP0) on PS3
 *   - Responses go via interrupt IN endpoint (0x81)
 *   - Status packet: 'S' [b0][b1][b2][b3] [seq] [active] 0x00...
 *     - 4 status bytes = 32-bit array, 2 bits per slot (up to 16 slots)
 *     - bits [2i+1:2i]: 00=absent, 01=present, 11=arrived, 10=removed
 *   - R response: {R, 0x02, 0x18} — Traptanium ID, works with all games
 *   - A response: {A, activation, 0xFF, 0x00} — wireless portal format
 *   - Status bits 0-1 = slot 0 (index 0x20), bits 2-3 = slot 1 (index 0x21)
 *   - Query/Write index 0x20 = slot 0, 0x21 = slot 1 (Traptanium mapping)
 *
 * Core 0: TinyUSB + HID + response queue drain
 * Core 1: UART RX from ESP32
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/uart.h"
#include "tusb.h"

#include "skylander_slots.h"
#include "usb_descriptors.h"
#include "kaos_protocol.h"

/* -----------------------------------------------------------------------
 * UART config
 * ----------------------------------------------------------------------- */
#define KAOS_UART    uart1
#define KAOS_UART_TX 4
#define KAOS_UART_RX 5


/* -----------------------------------------------------------------------
 * Response queue — core 0 drains this into USB IN reports
 * ----------------------------------------------------------------------- */
#define RESP_QUEUE_LEN 16
#define REPORT_LEN     32

static uint8_t  q_buf[RESP_QUEUE_LEN][REPORT_LEN];
static uint8_t  q_head = 0, q_tail = 0;
static spin_lock_t *q_lock;

static bool q_push(const uint8_t *data) {
    uint32_t save = spin_lock_blocking(q_lock);
    uint8_t next = (q_tail + 1) % RESP_QUEUE_LEN;
    bool ok = (next != q_head);
    if (ok) { memcpy(q_buf[q_tail], data, REPORT_LEN); q_tail = next; }
    spin_unlock(q_lock, save);
    return ok;
}

static bool q_pop(uint8_t *out) {
    uint32_t save = spin_lock_blocking(q_lock);
    bool ok = (q_head != q_tail);
    if (ok) { memcpy(out, q_buf[q_head], REPORT_LEN); q_head = (q_head + 1) % RESP_QUEUE_LEN; }
    spin_unlock(q_lock, save);
    return ok;
}

static bool q_empty(void) {
    uint32_t save = spin_lock_blocking(q_lock);
    bool e = (q_head == q_tail);
    spin_unlock(q_lock, save);
    return e;
}

/* -----------------------------------------------------------------------
 * Slot spinlock (core 0 reads, core 1 writes)
 * ----------------------------------------------------------------------- */
static spin_lock_t *s_slot_lock;

/* -----------------------------------------------------------------------
 * Status report
 *
 * Format (32 bytes):
 *   [0]  'S' (0x53)
 *   [1]  status bits 7-0
 *   [2]  status bits 15-8
 *   [3]  status bits 23-16
 *   [4]  status bits 31-24
 *   [5]  sequence counter (auto-increment)
 *   [6]  portal active flag (0x01 = active)
 *   [7..31] 0x00
 *
 * 2 bits per slot:
 *   00 = not present
 *   01 = present
 *   11 = just arrived (one-shot)
 *   10 = just removed (one-shot)
 * ----------------------------------------------------------------------- */
static uint8_t  g_status_seq = 0;
static bool     g_was_loaded[MAX_SLOTS]       = {false};
static bool     g_arrival_pending[MAX_SLOTS]  = {false};
static bool     g_removal_pending[MAX_SLOTS]  = {false};

static void build_status(uint8_t out[REPORT_LEN]) {
    memset(out, 0, REPORT_LEN);
    out[0] = 'S';

    uint32_t save = spin_lock_blocking(s_slot_lock);
    uint32_t bits = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {
        bool loaded = g_slots[i].loaded && g_slots[i].active;

        /* Detect transitions — set pending on load, keep it until game queries */
        if (loaded && !g_was_loaded[i])  g_arrival_pending[i] = true;
        if (!loaded && g_was_loaded[i])  g_removal_pending[i] = true;
        g_was_loaded[i] = loaded;

        uint32_t slot_bits = 0;
        if (g_arrival_pending[i]) {
            slot_bits = 0x3;            /* 11 = arrived — keep firing until cleared by Q */
            /* NOTE: do NOT clear here — cleared in Q handler when game queries */
        } else if (g_removal_pending[i]) {
            slot_bits = 0x2;            /* 10 = removed (one-shot) */
            g_removal_pending[i] = false;
        } else if (loaded) {
            slot_bits = 0x1;            /* 01 = present */
        }

        /* Status bit layout — 2 bits per slot starting at bit 0.
         * The game maps bit position N*2 to query index 0x20+N.
         * No shift needed — slot 0 at bits 0-1 = index 0x20, slot 1 at bits 2-3 = index 0x21 */
        bits |= (slot_bits << (i * 2));
    }
    spin_unlock(s_slot_lock, save);

    out[1] = (bits >>  0) & 0xFF;
    out[2] = (bits >>  8) & 0xFF;
    out[3] = (bits >> 16) & 0xFF;
    out[4] = (bits >> 24) & 0xFF;
    out[5] = g_status_seq++;
    out[6] = 0x01;  /* always active */
}

/* -----------------------------------------------------------------------
 * Debug helper — sends a short string back to ESP32 via UART
 * ----------------------------------------------------------------------- */
static void pico_debug(const char *msg) {
    uint16_t len = (uint16_t)strlen(msg);
    if (len > 40) len = 40;
    static uint8_t frame[56];
    int n = kaos_build_frame(frame, MSG_DEBUG, (const uint8_t*)msg, len);
    uart_write_blocking(KAOS_UART, frame, n);
}

/* Write debounce — updated by handle_command (core 0), read by main loop (core 0) */
static volatile uint32_t g_last_write_ms[MAX_SLOTS] = {0};

/* -----------------------------------------------------------------------
 * Handle incoming HID command (from SET_REPORT or interrupt OUT)
 * Builds a response and pushes it to the queue, or returns false
 * if no response needed.
 * ----------------------------------------------------------------------- */
static void handle_command(const uint8_t *cmd) {
    uint8_t resp[REPORT_LEN];
    memset(resp, 0, REPORT_LEN);
    bool has_resp = true;

    /* Debug: log commands received from the game */
    if (cmd[0]=='R' || cmd[0]=='A' || cmd[0]=='Q' || cmd[0]=='S') {
        char d[6] = "CMD:X";
        d[4] = cmd[0]; d[5] = '\0';
        pico_debug(d);
    }

    switch (cmd[0]) {

    case 'R':
        /* Ready — Traptanium ID, universal across all games */
        resp[0] = 'R';
        resp[1] = 0x02;
        resp[2] = 0x18;
        {
            char d[16] = "R:";
            d[2] = "0123456789ABCDEF"[cmd[1]>>4];
            d[3] = "0123456789ABCDEF"[cmd[1]&0xF];
            d[4] = ',';
            d[5] = "0123456789ABCDEF"[cmd[2]>>4];
            d[6] = "0123456789ABCDEF"[cmd[2]&0xF];
            d[7] = '\0';
            pico_debug(d);
        }
        break;

    case 'A':
        /* Activate/deactivate portal.
         * Wireless format: {A, activation, 0xFF, 0x00}
         * cmd[2] may contain LED color/brightness for Trap Team portal lights — ignored. */
        resp[0] = 0x41;
        resp[1] = cmd[1];
        resp[2] = 0xFF;
        resp[3] = 0x00;
        {
            char d[16] = "A:";
            d[2] = "0123456789ABCDEF"[cmd[1]>>4];
            d[3] = "0123456789ABCDEF"[cmd[1]&0xF];
            d[4] = ',';
            d[5] = "0123456789ABCDEF"[cmd[2]>>4];
            d[6] = "0123456789ABCDEF"[cmd[2]&0xF];
            d[7] = '\0';
            pico_debug(d);
        }
        break;

    case 'S':
        /* Explicit status request */
        build_status(resp);
        break;

    case 'Q':
        {
            uint8_t raw_idx = cmd[1];
            uint8_t blk     = cmd[2];
            /* Slot mapping:
             * Older games (Imaginators etc): 0x10=slot0, 0x11=slot1
             * Trap Team PS3:                 0x20=slot0, 0x21=slot1
             * Accept both — map to internal slot 0/1 */
            uint8_t slot;
            if      (raw_idx >= 0x20) slot = raw_idx - 0x20;
            else if (raw_idx >= 0x10) slot = raw_idx - 0x10;
            else                      slot = raw_idx;

            if (blk == 0) {
                char d[12] = "Q:0x";
                d[4] = "0123456789ABCDEF"[raw_idx>>4];
                d[5] = "0123456789ABCDEF"[raw_idx&0xF];
                d[6] = '>'; d[7] = 's';
                d[8] = slot < 10 ? '0'+slot : 'A'+(slot-10);
                d[9] = '\0';
                pico_debug(d);
                if (slot < MAX_SLOTS) g_arrival_pending[slot] = false;
            }
            resp[0] = 'Q';
            resp[2] = blk;
            uint32_t save = spin_lock_blocking(s_slot_lock);
            uint8_t *bd   = slots_get_block(slot, blk);
            if (bd && slot < MAX_SLOTS) {
                resp[1] = raw_idx;   /* echo back the exact index game sent */
                memcpy(&resp[3], bd, 16);
            } else {
                resp[1] = 0x01;      /* error */
            }
            spin_unlock(s_slot_lock, save);
        }
        break;

    case 'W':
        {
            uint8_t raw_idx = cmd[1];
            uint8_t blk     = cmd[2];
            uint8_t slot;
            if      (raw_idx >= 0x20) slot = raw_idx - 0x20;
            else if (raw_idx >= 0x10) slot = raw_idx - 0x10;
            else                      slot = raw_idx;

            resp[0] = 'W';
            resp[1] = 0x00;
            resp[2] = blk;

            if (slot < MAX_SLOTS) {
                uint32_t save = spin_lock_blocking(s_slot_lock);
                slots_write_block(slot, blk, &cmd[3]);
                g_slots[slot].dirty = true;
                spin_unlock(s_slot_lock, save);
                g_last_write_ms[slot] = to_ms_since_boot(get_absolute_time());
            }
        }
        break;

    case 'J': /* Trap Team fade light — echo cmd char + side byte */
        resp[0] = 'J';
        resp[1] = cmd[1];
        break;

    case 'C': /* Color — no LED, no response needed */
    case 'L': /* Light — no response needed */
    case 'M': /* Music — no response needed */
    case 'V': /* Unknown, seen on startup — ignore */
        has_resp = false;
        break;

    default:
        has_resp = false;
        break;
    }

    if (has_resp) q_push(resp);
}

/* -----------------------------------------------------------------------
 * TinyUSB HID callbacks
 * ----------------------------------------------------------------------- */

/* SET_REPORT (control) — PS3 sends commands this way */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buf, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    if (bufsize >= 1) handle_command(buf);
}

/* GET_REPORT — return current status */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buf, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    uint8_t resp[REPORT_LEN];
    if (!q_pop(resp)) build_status(resp);
    uint16_t n = (reqlen < REPORT_LEN) ? reqlen : REPORT_LEN;
    memcpy(buf, resp, n);
    return n;
}

/* -----------------------------------------------------------------------
 * UART send helper
 * ----------------------------------------------------------------------- */
static void uart_send(kaos_msg_t type, const uint8_t *payload, uint16_t len) {
    static uint8_t frame[SKYLANDER_DUMP_SIZE + 8];
    static const uint8_t empty[1] = {0};
    int n = kaos_build_frame(frame, type,
                             (payload && len) ? payload : empty, len);
    uart_write_blocking(KAOS_UART, frame, n);
}

/* -----------------------------------------------------------------------
 * Core 1 — UART RX from ESP32
 * ----------------------------------------------------------------------- */
static void core1_uart_rx(void) {
    kaos_parser_t parser;
    kaos_parser_init(&parser);
    kaos_msg_t type;
    uint8_t   *payload;
    uint16_t   len;

    while (true) {
        if (!uart_is_readable(KAOS_UART)) { tight_loop_contents(); continue; }
        uint8_t b = uart_getc(KAOS_UART);
        if (!kaos_parser_feed(&parser, b, &type, &payload, &len)) continue;

        switch (type) {
        case MSG_LOAD:
            if (len >= 1 + SKYLANDER_DUMP_SIZE) {
                uint8_t slot = payload[0];

                uint32_t save = spin_lock_blocking(s_slot_lock);
                slots_load(slot, payload + 1);
                spin_unlock(s_slot_lock, save);

                char dbg[16] = "LOAD:s";
                dbg[6] = '0' + slot;
                dbg[7] = ',';
                uint8_t u = payload[1];
                dbg[8]  = "0123456789ABCDEF"[u>>4];
                dbg[9]  = "0123456789ABCDEF"[u&0xF];
                dbg[10] = '\0';
                pico_debug(dbg);
            } else {
                pico_debug("LOAD:BAD_LEN");
            }
            break;

        case MSG_UNLOAD:
            if (len >= 1) {
                uint8_t slot = payload[0];
                uint32_t save = spin_lock_blocking(s_slot_lock);
                slots_unload(slot);
                spin_unlock(s_slot_lock, save);
                if (slot < MAX_SLOTS) g_arrival_pending[slot] = false;
            }
            break;

        case MSG_ESP_READY:
            /* ESP32 just booted — unload all slots and re-announce ourselves */
            {
                uint32_t save = spin_lock_blocking(s_slot_lock);
                for (int si = 0; si < MAX_SLOTS; si++) {
                    slots_unload(si);
                    g_arrival_pending[si] = false;
                    g_removal_pending[si] = false;
                    g_was_loaded[si]      = false;
                }
                spin_unlock(s_slot_lock, save);
            }
            uart_send(MSG_PICO_READY, NULL, 0);
            pico_debug("ESP_READY:ack");
            break;

        default:
            break;
        }
    }
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void) {
    /* Claim spinlocks */
    q_lock      = spin_lock_instance(spin_lock_claim_unused(true));
    s_slot_lock = spin_lock_instance(spin_lock_claim_unused(true));

    /* LED — only on standard Pico (GP25). RP2040 Zero uses a NeoPixel
     * on GP16 which needs a different driver; skip it to keep things simple. */
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
#endif

    /* UART1 for ESP32 comms */
    uart_init(KAOS_UART, KAOS_BAUD);
    gpio_set_function(KAOS_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(KAOS_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(KAOS_UART, false, false);
    uart_set_format(KAOS_UART, 8, 1, UART_PARITY_NONE);

    /* USB */
    tusb_init();

    /* Tell ESP32 we're ready */
    sleep_ms(300);
    uart_send(MSG_PICO_READY, NULL, 0);

    /* Start core 1 UART RX */
    multicore_launch_core1(core1_uart_rx);

    /* Core 0 main loop */
    uint32_t last_status_ms = 0;

    while (true) {
        tud_task();

        /* Write-back debounce — send once after 500ms of write inactivity */
        {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            for (int si = 0; si < MAX_SLOTS; si++) {
                uint32_t save = spin_lock_blocking(s_slot_lock);
                bool dirty = g_slots[si].dirty;
                uint32_t last = g_last_write_ms[si];
                spin_unlock(s_slot_lock, save);

                if (dirty && last > 0 && (now - last) >= 500) {
                    /* Game stopped writing — send write-back now */
                    static uint8_t wb_buf[1 + SKYLANDER_DUMP_SIZE];
                    wb_buf[0] = si;
                    uint32_t s2 = spin_lock_blocking(s_slot_lock);
                    memcpy(wb_buf + 1, g_slots[si].data, SKYLANDER_DUMP_SIZE);
                    g_slots[si].dirty = false;
                    spin_unlock(s_slot_lock, s2);

                    static uint8_t frame[SKYLANDER_DUMP_SIZE + 8];
                    int n = kaos_build_frame(frame, MSG_WRITE_BACK, wb_buf,
                                             1 + SKYLANDER_DUMP_SIZE);
                    uart_write_blocking(KAOS_UART, frame, n);
                    pico_debug("WB:done");
                }
            }
        }

        /* Drain response queue → USB IN */
        if (tud_hid_ready()) {
            uint8_t resp[REPORT_LEN];
            bool sent = false;

            if (q_pop(resp)) {
                tud_hid_report(0, resp, REPORT_LEN);
                last_status_ms = to_ms_since_boot(get_absolute_time());
                sent = true;
            }

            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (!sent && (now - last_status_ms >= 20)) {
                /* Always send S status at 50Hz regardless of portal active state.
                 * The game relies on constant status packets from the moment of
                 * USB connection — stopping them confuses the portal detection. */
                build_status(resp);
                tud_hid_report(0, resp, REPORT_LEN);
                last_status_ms = now;
            }
        }
    }
}
