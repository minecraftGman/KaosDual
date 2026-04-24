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
 *   - R response identifies portal type to the game
 *   - A response: 0x41, activation_byte, 0xFF, 0x77
 *   - Query/Write use index 0x10 for slot 0, 0x11 for slot 1, etc.
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
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
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
 * Portal type — stored in watchdog scratch across reboots
 * scratch[4] = magic, scratch[5] = type
 * ----------------------------------------------------------------------- */
#define PORTAL_TYPE_MAGIC 0xCA05CA05u

static uint8_t load_portal_type(void) {
    if (watchdog_hw->scratch[4] == PORTAL_TYPE_MAGIC) {
        uint8_t t = (uint8_t)watchdog_hw->scratch[5];
        if (t <= 3) return t;
    }
    return 3; /* default: Imaginators */
}

static void save_and_reboot(uint8_t type) {
    watchdog_hw->scratch[4] = PORTAL_TYPE_MAGIC;
    watchdog_hw->scratch[5] = type;
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
    watchdog_reboot(0, 0, 10);
    while (1) tight_loop_contents();
}

volatile uint8_t g_portal_type = 3;
uint8_t portal_get_type(void) { return g_portal_type; }

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
static bool     g_portal_active               = false;

static void build_status(uint8_t out[REPORT_LEN]) {
    memset(out, 0, REPORT_LEN);
    out[0] = 'S';

    uint32_t save = spin_lock_blocking(s_slot_lock);
    uint32_t bits = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {
        bool loaded = g_slots[i].loaded && g_slots[i].active;

        /* Detect transitions */
        if (loaded && !g_was_loaded[i])  g_arrival_pending[i] = true;
        if (!loaded && g_was_loaded[i])  g_removal_pending[i] = true;
        g_was_loaded[i] = loaded;

        uint32_t slot_bits = 0;
        if (g_arrival_pending[i]) {
            slot_bits = 0x3;            /* 11 = arrived */
            g_arrival_pending[i] = false;
        } else if (g_removal_pending[i]) {
            slot_bits = 0x2;            /* 10 = removed */
            g_removal_pending[i] = false;
        } else if (loaded) {
            slot_bits = 0x1;            /* 01 = present */
        }

        bits |= (slot_bits << (i * 2));
    }
    spin_unlock(s_slot_lock, save);

    out[1] = (bits >>  0) & 0xFF;
    out[2] = (bits >>  8) & 0xFF;
    out[3] = (bits >> 16) & 0xFF;
    out[4] = (bits >> 24) & 0xFF;
    out[5] = g_status_seq++;
    out[6] = g_portal_active ? 0x01 : 0x00;
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
        /* Ready — identifies portal type to game.
         * Exact IDs from Texthead1/Skylanders-Portal-IDs (verified against FCC):
         *   SSA PS3/Wii wireless: 0x01 0x29
         *   Giants PS3/Wii:       0x01 0x3D
         *   Swap Force:           0x02 0x03
         *   Traptanium (TT):      0x02 0x18
         *   Imaginators:          0x02 0x0A 0x05 0x08
         * Newer portals are backwards compatible so Imaginators ID works for all
         * older games too — but SSA specifically checks for its own ID.
         */
        g_portal_active = true;
        resp[0] = 'R';
        switch (g_portal_type) {
            case 0:  /* SSA */
                resp[1]=0x01; resp[2]=0x29;
                break;
            case 1:  /* Giants / Swap Force */
                resp[1]=0x01; resp[2]=0x3D;
                break;
            case 2:  /* Trap Team */
                resp[1]=0x02; resp[2]=0x18;
                break;
            case 3:  /* Imaginators (default) */
            default:
                resp[1]=0x02; resp[2]=0x0A; resp[3]=0x05; resp[4]=0x08;
                break;
        }
        break;

    case 'A':
        /* Activate / deactivate */
        g_portal_active = (cmd[1] == 0x01);
        resp[0] = 0x41;      /* 'A' */
        resp[1] = cmd[1];
        resp[2] = 0xFF;
        resp[3] = 0x77;
        break;

    case 'S':
        /* Explicit status request */
        build_status(resp);
        break;

    case 'Q':
        {
            uint8_t raw_idx = cmd[1];
            uint8_t slot    = (raw_idx >= 0x10) ? (raw_idx - 0x10) : raw_idx;
            uint8_t blk     = cmd[2];
            /* Debug first block query per figure */
            if (blk == 0) {
                char d[12] = "Q:0x";
                d[4] = "0123456789ABCDEF"[raw_idx>>4];
                d[5] = "0123456789ABCDEF"[raw_idx&0xF];
                d[6] = '>'; d[7] = 's'; d[8] = '0'+slot; d[9] = '\0';
                pico_debug(d);
            }
            resp[0] = 'Q';
            resp[2] = blk;
            uint32_t save = spin_lock_blocking(s_slot_lock);
            uint8_t *bd   = slots_get_block(slot, blk);
            if (bd) {
                resp[1] = 0x10 + slot;
                memcpy(&resp[3], bd, 16);
            } else {
                resp[1] = 0x01;
            }
            spin_unlock(s_slot_lock, save);
        }
        break;

    case 'W':
        /* Write block */
        {
            uint8_t raw_idx = cmd[1];
            uint8_t slot    = (raw_idx >= 0x10) ? (raw_idx - 0x10) : raw_idx;
            uint8_t blk     = cmd[2];
            resp[0] = 'W';
            resp[1] = 0x00;   /* always 0 in write response */
            resp[2] = blk;    /* echo block number */

            uint32_t save = spin_lock_blocking(s_slot_lock);
            slots_write_block(slot, blk, &cmd[3]);
            /* Trigger write-back on every write — the ESP32 will save the
             * updated encrypted dump to SPIFFS each time */
            static uint8_t wb_buf[1 + SKYLANDER_DUMP_SIZE];
            wb_buf[0] = slot;
            memcpy(wb_buf + 1, g_slots[slot].data, SKYLANDER_DUMP_SIZE);
            spin_unlock(s_slot_lock, save);

            /* Send write-back to ESP32 */
            static uint8_t frame[SKYLANDER_DUMP_SIZE + 8];
            int n = kaos_build_frame(frame, MSG_WRITE_BACK, wb_buf, 1 + SKYLANDER_DUMP_SIZE);
            uart_write_blocking(KAOS_UART, frame, n);
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
static volatile bool     g_type_pending   = false;
static volatile uint8_t  g_pending_type   = 3;

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

                /* Push arrival status packets.
                 * Must include ALL currently loaded slots in each packet,
                 * not just the new one — otherwise the game thinks other
                 * figures disappeared when the new one arrived. */
                uint32_t all_present = 0;
                uint32_t save2 = spin_lock_blocking(s_slot_lock);
                for (int si = 0; si < MAX_SLOTS; si++) {
                    if (g_slots[si].loaded && g_slots[si].active)
                        all_present |= (0x1u << (si * 2));
                }
                spin_unlock(s_slot_lock, save2);

                /* New slot gets arrival bits (11), others stay present (01) */
                uint32_t arrival_bits = all_present | (0x2u << (slot * 2)); /* set upper bit = 11 */
                uint32_t present_bits = all_present;

                uint8_t pkt[REPORT_LEN];
                /* One arrival packet, then steady-state present packets */
                memset(pkt, 0, REPORT_LEN);
                pkt[0] = 'S';
                pkt[1] = (arrival_bits >> 0) & 0xFF;
                pkt[2] = (arrival_bits >> 8) & 0xFF;
                pkt[3] = (arrival_bits >> 16) & 0xFF;
                pkt[4] = (arrival_bits >> 24) & 0xFF;
                pkt[5] = 0;
                pkt[6] = 0x01;
                q_push(pkt);

                /* Follow with present-only packets */
                for (int i = 0; i < 3; i++) {
                    memset(pkt, 0, REPORT_LEN);
                    pkt[0] = 'S';
                    pkt[1] = (present_bits >> 0) & 0xFF;
                    pkt[2] = (present_bits >> 8) & 0xFF;
                    pkt[3] = (present_bits >> 16) & 0xFF;
                    pkt[4] = (present_bits >> 24) & 0xFF;
                    pkt[5] = 0;
                    pkt[6] = 0x01;
                    q_push(pkt);
                }
            } else {
                pico_debug("LOAD:BAD_LEN");
            }
            break;

        case MSG_UNLOAD:
            if (len >= 1) {
                uint32_t save = spin_lock_blocking(s_slot_lock);
                slots_unload(payload[0]);
                spin_unlock(s_slot_lock, save);
            }
            break;

        case MSG_ESP_READY:
            /* ESP32 just booted — unload all slots and re-announce ourselves */
            {
                uint32_t save = spin_lock_blocking(s_slot_lock);
                for (int si = 0; si < MAX_SLOTS; si++) slots_unload(si);
                spin_unlock(s_slot_lock, save);
            }
            uart_send(MSG_PICO_READY, NULL, 0);
            pico_debug("ESP_READY:ack");
            break;
            if (len >= 1 && payload[0] != g_portal_type && payload[0] <= 3) {
                g_pending_type = payload[0];
                g_type_pending = true;
            }
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

    /* Load portal type from watchdog scratch */
    g_portal_type = load_portal_type();

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

        /* Portal type change — save and reboot */
        if (g_type_pending) {
            g_type_pending = false;
#ifdef PICO_DEFAULT_LED_PIN
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
            sleep_ms(50);
            save_and_reboot(g_pending_type);
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

            /* Send periodic status when portal is active and queue is empty */
            if (!sent && g_portal_active) {
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (now - last_status_ms >= 20) {  /* 50 Hz, matching real portal */
                    build_status(resp);
                    tud_hid_report(0, resp, REPORT_LEN);
                    last_status_ms = now;
                }
            }
        }
    }
}
