/*
 * KAOS Pico — main.c
 *
 * Responsibilities:
 *   - Appear as a Skylander Portal of Power over USB HID
 *   - Receive Skylander dumps from ESP32 over UART1 (MSG_LOAD / MSG_UNLOAD)
 *   - Handle all HID commands from the game (read/write blocks, status)
 *   - Send MSG_WRITE_BACK to ESP32 when the game writes to a tag
 *
 * Core 0: TinyUSB task + HID processing
 * Core 1: UART RX from ESP32
 *
 * Wiring:
 *   Pico GPIO4 (UART1 TX) ──→ ESP32 GPIO16 (RX2)
 *   Pico GPIO5 (UART1 RX) ←── ESP32 GPIO17 (TX2)
 *   Pico GND              ─── ESP32 GND
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
#define KAOS_UART       uart1
#define KAOS_UART_TX    4
#define KAOS_UART_RX    5

/* -----------------------------------------------------------------------
 * Portal HID commands
 * ----------------------------------------------------------------------- */
#define CMD_ACTIVATE    'A'
#define CMD_READY_QUERY 'Q'
#define CMD_STATUS      'S'
#define CMD_RESET       'R'
#define CMD_MUSIC       'M'
#define CMD_LIGHT       'L'
#define CMD_TAG_READ    'b'
#define CMD_TAG_WRITE   'w'
#define CMD_QUERY_CHAR  'J'
#define CMD_TAG_ID      'I'
#define RESP_READY      'A'
#define RESP_STATUS     'S'
#define RESP_TAG_READ   'b'
#define RESP_TAG_WRITE  'w'
#define RESP_TAG_ID     'i'
#define RESP_NEXT_TAG   'j'

static bool g_portal_active = false;

/* Spinlock protecting g_slots from concurrent core 0 / core 1 access */
static spin_lock_t *s_slot_lock;

/* -----------------------------------------------------------------------
 * UART send helpers
 * ----------------------------------------------------------------------- */
static void uart_send_frame(kaos_msg_t type, const uint8_t *payload, uint16_t len) {
    static uint8_t frame_buf[SKYLANDER_DUMP_SIZE + 8];
    /* kaos_build_frame handles len==0 safely; pass empty array if NULL */
    static const uint8_t empty[1] = {0};
    int n = kaos_build_frame(frame_buf, type,
                             (payload && len) ? payload : empty,
                             len);
    uart_write_blocking(KAOS_UART, frame_buf, n);
}

/* -----------------------------------------------------------------------
 * UART RX core (core 1) — receives MSG_LOAD / MSG_UNLOAD from ESP32
 * ----------------------------------------------------------------------- */
static void core1_uart_rx(void) {
    kaos_parser_t parser;
    kaos_parser_init(&parser);

    kaos_msg_t   type;
    uint8_t     *payload;
    uint16_t     len;

    while (true) {
        if (uart_is_readable(KAOS_UART)) {
            uint8_t b = uart_getc(KAOS_UART);
            if (kaos_parser_feed(&parser, b, &type, &payload, &len)) {
                uint32_t save = spin_lock_blocking(s_slot_lock);
                switch (type) {
                    case MSG_LOAD:
                        if (len >= 1 + SKYLANDER_DUMP_SIZE)
                            slots_load(payload[0], payload + 1);
                        break;
                    case MSG_UNLOAD:
                        if (len >= 1) slots_unload(payload[0]);
                        break;
                    default:
                        break;
                }
                spin_unlock(s_slot_lock, save);
            }
        }
        tight_loop_contents();
    }
}

/* -----------------------------------------------------------------------
 * HID processing (core 0)
 * ----------------------------------------------------------------------- */
static void build_status_report(uint8_t r[PORTAL_HID_REPORT_LEN]) {
    memset(r, 0, PORTAL_HID_REPORT_LEN);
    r[0] = RESP_STATUS;
    r[1] = slots_portal_status();
}

static void process_hid_out(const uint8_t *in, uint8_t r[PORTAL_HID_REPORT_LEN]) {
    memset(r, 0, PORTAL_HID_REPORT_LEN);

    switch (in[0]) {
        case CMD_ACTIVATE:
            /* 'A' — activate portal. PS3 sends this after 'R'. */
            g_portal_active = (in[1] == 0x01);
            r[0]=RESP_READY; r[1]=0xFF; r[2]=0x77;
            break;

        case CMD_READY_QUERY:
            r[0]=RESP_READY; r[1]=0xFF; r[2]=0x77;
            break;

        case CMD_STATUS:
            build_status_report(r);
            break;

        case CMD_RESET:
            /* 'R' — PS3 sends this first to start the portal.
             * Respond with 'R' then immediately start sending status. */
            g_portal_active = true;
            r[0]='R';
            break;

        case CMD_MUSIC:
        case CMD_LIGHT:
            /* Ignore lighting/music — no LED on our portal */
            break;

        case CMD_TAG_READ: {
            uint8_t slot = (in[1] >> 4) & 0x0F;
            uint8_t blk  = in[2];
            r[0]=RESP_TAG_READ; r[1]=in[1]; r[2]=in[2];
            uint32_t save = spin_lock_blocking(s_slot_lock);
            uint8_t *bd = slots_get_block(slot, blk);
            if (bd) { r[3]=0x00; memcpy(&r[4], bd, 16); }
            else      r[3]=0xFF;
            spin_unlock(s_slot_lock, save);
            break;
        }

        case CMD_TAG_WRITE: {
            uint8_t slot = (in[1] >> 4) & 0x0F;
            uint8_t blk  = in[2];
            r[0]=RESP_TAG_WRITE; r[1]=in[1]; r[2]=in[2]; r[3]=0x00;

            uint32_t save = spin_lock_blocking(s_slot_lock);
            slots_write_block(slot, blk, &in[3]);
            bool should_writeback = (blk == 8) && g_slots[slot].dirty;
            /* Snapshot the data while we hold the lock */
            static uint8_t wb_payload[1 + SKYLANDER_DUMP_SIZE];
            if (should_writeback) {
                wb_payload[0] = slot;
                memcpy(wb_payload + 1, g_slots[slot].data, SKYLANDER_DUMP_SIZE);
                g_slots[slot].dirty = false;
            }
            spin_unlock(s_slot_lock, save);

            /* Send outside the lock — uart_write_blocking can take a while */
            if (should_writeback)
                uart_send_frame(MSG_WRITE_BACK, wb_payload,
                                1 + SKYLANDER_DUMP_SIZE);
            break;
        }

        case CMD_QUERY_CHAR: {
            uint8_t slot = in[1] & 0x0F;
            r[0]=RESP_NEXT_TAG;
            uint32_t save = spin_lock_blocking(s_slot_lock);
            r[1]=slot | (g_slots[slot].loaded ? 0x10 : 0x00);
            if (g_slots[slot].loaded) memcpy(&r[2], g_slots[slot].uid, 4);
            spin_unlock(s_slot_lock, save);
            break;
        }

        case CMD_TAG_ID: {
            uint8_t slot = in[1] & 0x0F;
            r[0]=RESP_TAG_ID; r[1]=slot;
            uint32_t save = spin_lock_blocking(s_slot_lock);
            if (g_slots[slot].loaded) memcpy(&r[2], g_slots[slot].uid, 4);
            spin_unlock(s_slot_lock, save);
            break;
        }

        default:
            break;
    }
}

/* -----------------------------------------------------------------------
 * TinyUSB HID callbacks
 * ----------------------------------------------------------------------- */
static uint8_t g_hid_response[PORTAL_HID_REPORT_LEN];
static bool    g_response_ready = false;

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    if (bufsize >= PORTAL_HID_REPORT_LEN) {
        process_hid_out(buffer, g_hid_response);
        g_response_ready = true;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    uint16_t len = (reqlen < PORTAL_HID_REPORT_LEN) ? reqlen : PORTAL_HID_REPORT_LEN;
    if (g_response_ready) {
        memcpy(buffer, g_hid_response, len);
        g_response_ready = false;
    } else {
        uint8_t status[PORTAL_HID_REPORT_LEN];
        build_status_report(status);
        memcpy(buffer, status, len);
    }
    return len;
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void) {
    /* Spinlock for cross-core slot access */
    int lock_num = spin_lock_claim_unused(true);
    s_slot_lock  = spin_lock_instance(lock_num);

    /* UART1 for ESP32 comms */
    uart_init(KAOS_UART, KAOS_BAUD);
    gpio_set_function(KAOS_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(KAOS_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(KAOS_UART, false, false);
    uart_set_format(KAOS_UART, 8, 1, UART_PARITY_NONE);

    /* TinyUSB */
    tusb_init();

    /* Signal to ESP32 that we're ready */
    sleep_ms(200);
    uart_send_frame(MSG_PICO_READY, NULL, 0);

    /* Core 1: UART RX */
    multicore_launch_core1(core1_uart_rx);

    /* Core 0: USB + HID
     * Also send periodic status reports while portal is active so the
     * game's status-polling doesn't time out. */
    uint32_t last_status_ms = 0;
    while (true) {
        tud_task();

        if (tud_hid_ready() && g_response_ready) {
            tud_hid_report(0, g_hid_response, PORTAL_HID_REPORT_LEN);
            g_response_ready = false;
            last_status_ms = to_ms_since_boot(get_absolute_time());
        }

        /* Send a status report every 100ms when active and no other traffic */
        if (g_portal_active && tud_hid_ready()) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - last_status_ms >= 100) {
                uint8_t status[PORTAL_HID_REPORT_LEN];
                build_status_report(status);
                tud_hid_report(0, status, PORTAL_HID_REPORT_LEN);
                last_status_ms = now;
            }
        }
    }
}
