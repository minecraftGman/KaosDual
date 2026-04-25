/*
 * pico_bridge.c
 * ESP32 side of the UART link to the Pi Pico.
 *
 * TX: GPIO17 (UART2)  →  Pico GPIO5 (UART1 RX)
 * RX: GPIO16 (UART2)  ←  Pico GPIO4 (UART1 TX)
 */
#include "pico_bridge.h"
#include "Skylander.h"
#include "esp_log.h"
#include "driver/uart.h"
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
 * RX task — listens for MSG_WRITE_BACK, MSG_PICO_READY, MSG_DEBUG
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
                ESP_LOGI(TAG, "Pico ready");
                s_pico_ready = true;
                break;

            case MSG_WRITE_BACK: {
                if (len < 1 + SKYLANDER_DUMP_SIZE) break;
                uint8_t slot = payload[0];

                xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
                if (slot < 2 && g_skylanders[slot].loaded) {
                    FILE *f = fopen(g_skylanders[slot].filename, "wb");
                    if (f) {
                        fwrite(payload + 1, 1, SKYLANDER_DUMP_SIZE, f);
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

            case MSG_DEBUG: {
                char msg[48] = {0};
                int dlen = (len < 47) ? len : 47;
                memcpy(msg, payload, dlen);
                ESP_LOGI(TAG, "DBG: %s", msg);
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
    uart_driver_install(BRIDGE_UART, 2048, 2048, 0, NULL, 0);
    uart_param_config(BRIDGE_UART, &cfg);
    uart_set_pin(BRIDGE_UART, PIN_TX, PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(rx_task, "pico_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "Bridge UART2 ready (TX=%d RX=%d @ %d baud)",
             PIN_TX, PIN_RX, KAOS_BAUD);

    vTaskDelay(pdMS_TO_TICKS(500));
    send_frame(MSG_ESP_READY, NULL, 0);
    ESP_LOGI(TAG, "Sent ESP_READY to Pico");
}

void pico_bridge_load(uint8_t slot, const uint8_t *raw_dump) {
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

bool pico_bridge_is_ready(void) {
    return s_pico_ready;
}
