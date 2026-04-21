/*
 * pico_bridge.c
 * ESP32 side of the UART link to the Pi Pico.
 *
 * TX: GPIO17 (UART2)  →  Pico GPIO5 (UART1 RX)
 * RX: GPIO16 (UART2)  ←  Pico GPIO4 (UART1 TX)
 */
#include "pico_bridge.h"
#include "Skylander.h"
#include "SkylanderCrypt.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "PicoBridge";

#define BRIDGE_UART     UART_NUM_2
#define PIN_TX          17
#define PIN_RX          16

extern SemaphoreHandle_t g_sky_mutex;

static bool s_pico_ready = false;

/* -----------------------------------------------------------------------
 * Send a framed message to the Pico
 * ----------------------------------------------------------------------- */
static void send_frame(kaos_msg_t type, const uint8_t *payload, uint16_t len) {
    static uint8_t buf[SKYLANDER_DUMP_SIZE + 8];
    int n = kaos_build_frame(buf, type, payload, len);
    uart_write_bytes(BRIDGE_UART, (const char *)buf, n);
}

/* -----------------------------------------------------------------------
 * RX task — listens for MSG_WRITE_BACK and MSG_PICO_READY
 * ----------------------------------------------------------------------- */
static void rx_task(void *arg) {
    kaos_parser_t parser;
    kaos_parser_init(&parser);

    kaos_msg_t  type;
    uint8_t    *payload;
    uint16_t    len;
    uint8_t     byte;

    while (1) {
        int r = uart_read_bytes(BRIDGE_UART, &byte, 1, pdMS_TO_TICKS(20));
        if (r != 1) continue;

        if (!kaos_parser_feed(&parser, byte, &type, &payload, &len)) continue;

        switch (type) {
            case MSG_PICO_READY:
                s_pico_ready = true;
                ESP_LOGI(TAG, "Pico is ready");
                break;

            case MSG_WRITE_BACK: {
                /* Pico sends back the updated decrypted dump after game writes.
                 * Re-encrypt it and save to SD, and update our in-memory copy. */
                if (len < 1 + SKYLANDER_DUMP_SIZE) break;
                uint8_t slot = payload[0];

                xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
                if (slot < 2 && g_skylanders[slot].loaded) {
                    /* Update ESP32's decrypted in-memory copy */
                    memcpy(g_skylanders[slot].data, payload + 1, SKYLANDER_DUMP_SIZE);

                    /* Re-encrypt a separate copy for saving to SD */
                    static uint8_t to_save[SKYLANDER_DUMP_SIZE];
                    memcpy(to_save, payload + 1, SKYLANDER_DUMP_SIZE);
                    encrypt_skylander(to_save, g_skylanders[slot].uid);

                    FILE *f = fopen(g_skylanders[slot].filename, "wb");
                    if (f) {
                        fwrite(to_save, 1, SKYLANDER_DUMP_SIZE, f);
                        fclose(f);
                        ESP_LOGI(TAG, "Saved slot %d → %s", slot,
                                 g_skylanders[slot].filename);
                    } else {
                        ESP_LOGE(TAG, "Cannot open for write: %s",
                                 g_skylanders[slot].filename);
                    }
                }
                xSemaphoreGive(g_sky_mutex);
                break;
            }

            default:
                break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void pico_bridge_init(void) {
    uart_config_t cfg = {
        .baud_rate  = KAOS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    /* Large RX buffer — we can receive a full 1025-byte write-back */
    uart_driver_install(BRIDGE_UART, 2048, 2048, 0, NULL, 0);
    uart_param_config(BRIDGE_UART, &cfg);
    uart_set_pin(BRIDGE_UART, PIN_TX, PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(rx_task, "pico_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "Bridge UART2 ready (TX=%d RX=%d @ %d baud)",
             PIN_TX, PIN_RX, KAOS_BAUD);
}

void pico_bridge_load(uint8_t slot, const uint8_t *raw_dump) {
    /* payload = [slot(1)][raw_dump(1024)] */
    static uint8_t payload[1 + SKYLANDER_DUMP_SIZE];
    payload[0] = slot;
    memcpy(payload + 1, raw_dump, SKYLANDER_DUMP_SIZE);
    send_frame(MSG_LOAD, payload, 1 + SKYLANDER_DUMP_SIZE);
    ESP_LOGI(TAG, "Sent LOAD slot %d to Pico", slot);
}

void pico_bridge_unload(uint8_t slot) {
    send_frame(MSG_UNLOAD, &slot, 1);
    ESP_LOGI(TAG, "Sent UNLOAD slot %d to Pico", slot);
}

void pico_bridge_set_portal_type(uint8_t type) {
    send_frame(MSG_SET_PORTAL_TYPE, &type, 1);
    ESP_LOGI(TAG, "Sent portal type %d to Pico", type);
}

bool pico_bridge_is_ready(void) {
    return s_pico_ready;
}
