/*
 * KAOS Pico — main.c
 *
 * Core 0: TinyUSB task + HID processing
 * Core 1: UART RX from ESP32
 *
 * Portal type is set by ESP32 via MSG_SET_PORTAL_TYPE and stored in a
 * simple global — takes effect after next power cycle of the Pico since
 * USB descriptors are fixed at enumeration time.
 *
 * Wiring:
 *   Pico GPIO4 (UART1 TX) ──→ ESP32 GPIO16 (RX2)
 *   Pico GPIO5 (UART1 RX) ←── ESP32 GPIO17 (TX2)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
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
 * Portal type — set by ESP32, read by usb_descriptors.c at init time.
 * Stored in the last page of flash so it survives power cycles.
 *
 * We use a simple 256-byte flash page at the very end of flash.
 * Layout: [0] = magic 0xAB, [1] = portal type (0-3)
 * ----------------------------------------------------------------------- */
#define FLASH_PORTAL_TYPE_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_PORTAL_TYPE_MAGIC   0xAB

static uint8_t g_portal_type = 3; /* default: Imaginators */

static void load_portal_type_from_flash(void) {
    const uint8_t *p = (const uint8_t *)(XIP_BASE + FLASH_PORTAL_TYPE_OFFSET);
    if (p[0] == FLASH_PORTAL_TYPE_MAGIC && p[1] <= 3) {
        g_portal_type = p[1];
    }
}

static void save_portal_type_to_flash(uint8_t type) {
    /* Read the sector first, modify our 2 bytes, erase, write back */
    static uint8_t sector_buf[FLASH_SECTOR_SIZE];
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + FLASH_PORTAL_TYPE_OFFSET);
    memcpy(sector_buf, flash_ptr, FLASH_SECTOR_SIZE);
    sector_buf[0] = FLASH_PORTAL_TYPE_MAGIC;
    sector_buf[1] = type;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_PORTAL_TYPE_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_PORTAL_TYPE_OFFSET, sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

uint8_t portal_get_type(void) { return g_portal_type; }

/* -----------------------------------------------------------------------
 * Portal HID commands (from game/console)
 * ----------------------------------------------------------------------- */
#define CMD_ACTIVATE    'A'
#define CMD_READY_QUERY 'Q'
#define CMD_STATUS      'S'
#define CMD_RESET       'R'
#define CMD_MUSIC       'M'
#define CMD_LIGHT       'L'
#define CMD_J           'J'   /* Trap Team fade light — respond with 'J' */
#define CMD_TAG_READ    'b'
#define CMD_TAG_WRITE   'w'
#define RESP_STATUS     'S'
#define RESP_TAG_READ   'b'
#define RESP_TAG_WRITE  'w'

static bool g_portal_active = false;
static spin_lock_t *s_slot_lock;

/* -----------------------------------------------------------------------
 * UART send
 * ----------------------------------------------------------------------- */
static void uart_send_frame(kaos_msg_t type, const uint8_t *payload, uint16_t len) {
    static uint8_t frame_buf[SKYLANDER_DUMP_SIZE + 8];
    static const uint8_t empty[1] = {0};
    int n = kaos_build_frame(frame_buf, type,
                             (payload && len) ? payload : empty, len);
    uart_write_blocking(KAOS_UART, frame_buf, n);
}

/* -----------------------------------------------------------------------
 * UART RX — core 1
 * ----------------------------------------------------------------------- */
static void core1_uart_rx(void) {
    kaos_parser_t parser;
    kaos_parser_init(&parser);

    kaos_msg_t  type;
    uint8_t    *payload;
    uint16_t    len;

    while (true) {
        if (!uart_is_readable(KAOS_UART)) {
            tight_loop_contents();
            continue;
        }
        uint8_t b = uart_getc(KAOS_UART);
        if (!kaos_parser_feed(&parser, b, &type, &payload, &len)) continue;

        switch (type) {
            case MSG_LOAD:
                if (len >= 1 + SKYLANDER_DUMP_SIZE) {
                    uint32_t s = spin_lock_blocking(s_slot_lock);
                    slots_load(payload[0], payload + 1);
                    spin_unlock(s_slot_lock, s);
                }
                break;

            case MSG_UNLOAD: {
                if (len >= 1) {
                    uint32_t s = spin_lock_blocking(s_slot_lock);
                    slots_unload(payload[0]);
                    spin_unlock(s_slot_lock, s);
                }
                break;
            }

            case MSG_SET_PORTAL_TYPE:
                /* Save to flash — takes effect on next power cycle */
                if (len >= 1 && payload[0] <= 3 && payload[0] != g_portal_type) {
                    save_portal_type_to_flash(payload[0]);
                    gpio_put(PICO_DEFAULT_LED_PIN, 1);
                    sleep_ms(300);
                    gpio_put(PICO_DEFAULT_LED_PIN, 0);
                }
                break;

            default:
                break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Status report
 *
 * Format: S [stat0][stat1][stat2][stat3] [seq] [active] 0x00...
 *
 * 4 status bytes = 32-bit little-endian bitfield.
 * Each slot uses 2 bits: 00=empty 01=present 11=arrived 10=removed
 * ----------------------------------------------------------------------- */
static uint8_t g_status_seq = 0;
static bool    g_slot_was_loaded[MAX_SLOTS] = {false};
static bool    g_arrival_pending[MAX_SLOTS] = {false};
static bool    g_removal_pending[MAX_SLOTS] = {false};

static void build_status_report(uint8_t r[PORTAL_HID_REPORT_LEN]) {
    memset(r, 0, PORTAL_HID_REPORT_LEN);
    r[0] = RESP_STATUS;

    uint32_t s = spin_lock_blocking(s_slot_lock);

    uint32_t bits = 0;
    for (int i = 0; i < MAX_SLOTS && i < 16; i++) {
        bool loaded = g_slots[i].loaded && g_slots[i].active;

        if (loaded && !g_slot_was_loaded[i]) {
            g_arrival_pending[i] = true;
            g_removal_pending[i] = false;
        } else if (!loaded && g_slot_was_loaded[i]) {
            g_removal_pending[i] = true;
            g_arrival_pending[i] = false;
        }
        g_slot_was_loaded[i] = loaded;

        uint32_t slot_bits = 0;
        if (g_arrival_pending[i]) {
            slot_bits = 0x3; /* arrived */
            g_arrival_pending[i] = false;
        } else if (g_removal_pending[i]) {
            slot_bits = 0x2; /* removed */
            g_removal_pending[i] = false;
        } else if (loaded) {
            slot_bits = 0x1; /* present */
        }
        bits |= (slot_bits << (i * 2));
    }

    spin_unlock(s_slot_lock, s);

    r[1] = (bits >>  0) & 0xFF;
    r[2] = (bits >>  8) & 0xFF;
    r[3] = (bits >> 16) & 0xFF;
    r[4] = (bits >> 24) & 0xFF;
    r[5] = g_status_seq++;
    r[6] = g_portal_active ? 0x01 : 0x00;
}

/* -----------------------------------------------------------------------
 * HID response buffer (written by tud_hid_set_report_cb, read by main loop)
 * ----------------------------------------------------------------------- */
static uint8_t g_hid_response[PORTAL_HID_REPORT_LEN];
static bool    g_response_ready = false;

/* -----------------------------------------------------------------------
 * HID command processing
 * ----------------------------------------------------------------------- */
static void process_hid_command(const uint8_t *in, uint8_t r[PORTAL_HID_REPORT_LEN]) {
    memset(r, 0, PORTAL_HID_REPORT_LEN);

    switch (in[0]) {
        case CMD_RESET:
            /* 'R' — first command from game, identifies portal type.
             * Bytes 1+2 tell the game what portal this is.
             * Trap Team rejects anything that isn't 0x02 0x18.
             *
             * Known IDs:
             *   0x01 0x3D = SSA / Giants / Swap Force
             *   0x02 0x18 = Traptanium (Trap Team)
             *   0x02 0x0A = SuperChargers / Imaginators
             */
            g_portal_active = true;
            r[0] = 'R';
            switch (g_portal_type) {
                case 2:  /* Trap Team */
                    r[1] = 0x02; r[2] = 0x18;
                    break;
                case 3:  /* Imaginators / SuperChargers */
                    r[1] = 0x02; r[2] = 0x0A;
                    break;
                case 0:  /* SSA / Giants */
                case 1:  /* Swap Force */
                default:
                    r[1] = 0x01; r[2] = 0x3D;
                    break;
            }
            break;

        case CMD_ACTIVATE:
            /* 'A' — 0x01 = activate, 0x00 = deactivate */
            g_portal_active = (in[1] == 0x01);
            r[0] = 'A'; r[1] = in[1]; r[2] = 0xFF; r[3] = 0x77;
            break;

        case CMD_READY_QUERY:
            r[0] = 'A'; r[1] = 0x00; r[2] = 0xFF; r[3] = 0x77;
            break;

        case CMD_STATUS:
            build_status_report(r);
            break;

        case CMD_J:
            /* Trap Team fade light — just echo back the command */
            r[0] = 'J'; r[1] = in[1];
            break;

        case CMD_MUSIC:
        case CMD_LIGHT:
        case 'C': /* Color command */
            /* No LEDs — silently ignore */
            break;

        case CMD_TAG_READ: {
            /* Game sends index 0x10 for slot 0, 0x11 for slot 1 */
            uint8_t idx  = in[1];
            uint8_t slot = (idx >= 0x10) ? (idx - 0x10) : idx;
            uint8_t blk  = in[2];
            r[0] = RESP_TAG_READ;
            r[1] = in[1];
            r[2] = in[2];
            uint32_t sv = spin_lock_blocking(s_slot_lock);
            uint8_t *bd = slots_get_block(slot, blk);
            if (bd) { r[3] = 0x00; memcpy(&r[4], bd, 16); }
            else      r[3] = 0xFF;
            spin_unlock(s_slot_lock, sv);
            break;
        }

        case CMD_TAG_WRITE: {
            uint8_t idx  = in[1];
            uint8_t slot = (idx >= 0x10) ? (idx - 0x10) : idx;
            uint8_t blk  = in[2];
            r[0] = RESP_TAG_WRITE;
            r[1] = in[1];
            r[2] = in[2];
            r[3] = 0x00;

            uint32_t sv = spin_lock_blocking(s_slot_lock);
            slots_write_block(slot, blk, &in[3]);
            bool should_wb = (blk == 8) && (slot < MAX_SLOTS) && g_slots[slot].dirty;
            static uint8_t wb_payload[1 + SKYLANDER_DUMP_SIZE];
            if (should_wb) {
                wb_payload[0] = slot;
                memcpy(wb_payload + 1, g_slots[slot].data, SKYLANDER_DUMP_SIZE);
                g_slots[slot].dirty = false;
            }
            spin_unlock(s_slot_lock, sv);

            if (should_wb)
                uart_send_frame(MSG_WRITE_BACK, wb_payload, 1 + SKYLANDER_DUMP_SIZE);
            break;
        }

        default:
            break;
    }
}

/* -----------------------------------------------------------------------
 * TinyUSB callbacks
 * ----------------------------------------------------------------------- */

/* Commands arrive via HID SET_REPORT (PS3 wireless portal protocol) */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    if (bufsize < 1) return;

    /* Strip leading zero report-ID byte if present */
    const uint8_t *cmd = buffer;
    if (buffer[0] == 0x00 && bufsize > 1) { cmd = buffer + 1; }

    process_hid_command(cmd, g_hid_response);
    g_response_ready = true;
}

/* IN endpoint callback — game polling for data */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    uint16_t len = (reqlen < PORTAL_HID_REPORT_LEN) ? reqlen : PORTAL_HID_REPORT_LEN;
    if (g_response_ready) {
        memcpy(buffer, g_hid_response, len);
        g_response_ready = false;
    } else {
        build_status_report(buffer);
    }
    return len;
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void) {
    int lock_num = spin_lock_claim_unused(true);
    s_slot_lock  = spin_lock_instance(lock_num);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    /* Load portal type from flash (saved by previous MSG_SET_PORTAL_TYPE) */
    load_portal_type_from_flash();

    /* UART to ESP32 */
    uart_init(KAOS_UART, KAOS_BAUD);
    gpio_set_function(KAOS_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(KAOS_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(KAOS_UART, false, false);
    uart_set_format(KAOS_UART, 8, 1, UART_PARITY_NONE);

    /* TinyUSB — portal type already set, descriptor will use it */
    tusb_init();

    /* Blink LED to show ready */
    for (int i = 0; i < 3; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1); sleep_ms(80);
        gpio_put(PICO_DEFAULT_LED_PIN, 0); sleep_ms(80);
    }

    /* Tell ESP32 we're up */
    sleep_ms(200);
    uart_send_frame(MSG_PICO_READY, NULL, 0);

    /* Core 1: UART RX */
    multicore_launch_core1(core1_uart_rx);

    /* Core 0: USB HID main loop */
    uint32_t last_status_ms = 0;
    while (true) {
        tud_task();

        /* Send queued HID response */
        if (tud_hid_ready() && g_response_ready) {
            tud_hid_report(0, g_hid_response, PORTAL_HID_REPORT_LEN);
            g_response_ready = false;
            last_status_ms = to_ms_since_boot(get_absolute_time());
        }

        /* Periodic status heartbeat — keeps game from timing out */
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
