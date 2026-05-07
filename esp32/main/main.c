/*
 * KAOS ESP32 — main.c
 *
 * Storage: SD card via SPI if present, falls back to SPIFFS on internal flash.
 * Files uploaded via web UI browser.
 *
 * SD card wiring (SPI):
 *   ESP32 GPIO23 (MOSI) ──→ SD MOSI
 *   ESP32 GPIO19 (MISO) ←── SD MISO
 *   ESP32 GPIO18 (CLK)  ──→ SD CLK
 *   ESP32 GPIO5  (CS)   ──→ SD CS
 *
 * UART wiring:
 *   ESP32 GPIO17 (TX2) ──→ Pico GPIO5 (UART1 RX)
 *   ESP32 GPIO16 (RX2) ←── Pico GPIO4 (UART1 TX)
 *   ESP32 GND          ─── Pico GND
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/i2c.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_spiffs.h"

#include "Skylander.h"
#include "web_ui.h"
#include "pico_bridge.h"

static const char *TAG = "KAOS-ESP32";

/* -----------------------------------------------------------------------
 * Config — edit these
 * ----------------------------------------------------------------------- */
#define WIFI_AP_SSID     "KAOS-Portal"
#define WIFI_AP_PASSWORD "skylands1"
#define WIFI_AP_CHANNEL  6
#define PORTAL_IP        "192.168.4.1"
#define SPIFFS_MOUNT     "/spiffs"
#define SD_MOUNT         "/sdcard"

/* SD card SPI pins */
#define PIN_SD_MOSI      23
#define PIN_SD_MISO      19
#define PIN_SD_CLK       18
#define PIN_SD_CS        5

#define I2C_PORT         I2C_NUM_0
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define LCD_I2C_ADDR     0x27

/* -----------------------------------------------------------------------
 * Globals (shared with web_ui.c and pico_bridge.c)
 * ----------------------------------------------------------------------- */
SemaphoreHandle_t g_sky_mutex;
int               g_file_count = 0;
char              g_file_list[64][64];
const char       *g_storage_root = SPIFFS_MOUNT; /* set at boot, SD takes priority */

/* -----------------------------------------------------------------------
 * LCD1602 via PCF8574 I2C backpack
 * ----------------------------------------------------------------------- */
#define LCD_BL (1<<3)
#define LCD_EN (1<<2)
#define LCD_RS (1<<0)

static void lcd_i2c(uint8_t v) {
    uint8_t b = v | LCD_BL;
    i2c_master_write_to_device(I2C_PORT, LCD_I2C_ADDR, &b, 1, pdMS_TO_TICKS(10));
}
static void lcd_pulse(uint8_t d) {
    lcd_i2c(d|LCD_EN); vTaskDelay(1); lcd_i2c(d&~LCD_EN); vTaskDelay(1);
}
static void lcd_nibble(uint8_t n, bool rs) {
    lcd_pulse((n<<4)|LCD_BL|(rs?LCD_RS:0));
}
static void lcd_byte(uint8_t v, bool rs) {
    lcd_nibble(v>>4,rs); lcd_nibble(v&0xf,rs);
}
static void lcd_cmd(uint8_t c)  { lcd_byte(c,false); vTaskDelay(2); }
static void lcd_char(char c)    { lcd_byte((uint8_t)c,true); }
static void lcd_init(void) {
    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_nibble(0x3,false); vTaskDelay(pdMS_TO_TICKS(5));
    lcd_nibble(0x3,false); vTaskDelay(pdMS_TO_TICKS(2));
    lcd_nibble(0x3,false); vTaskDelay(pdMS_TO_TICKS(2));
    lcd_nibble(0x2,false);
    lcd_cmd(0x28); lcd_cmd(0x0C); lcd_cmd(0x06); lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
}
static void lcd_line(uint8_t row, const char *s) {
    char buf[17]; memset(buf,' ',16); buf[16]=0;
    int l=strlen(s); if(l>16)l=16; memcpy(buf,s,l);
    lcd_byte(0x80|(row?0x40:0x00), false);
    vTaskDelay(2);
    for(int i=0;i<16;i++) lcd_char(buf[i]);
}

/* -----------------------------------------------------------------------
 * SPIFFS
 * ----------------------------------------------------------------------- */
static sdmmc_card_t *s_sd_card = NULL;

static bool sd_init(void) {
    spi_bus_config_t bus = {
        .mosi_io_num   = PIN_SD_MOSI,
        .miso_io_num   = PIN_SD_MISO,
        .sclk_io_num   = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) {
        ESP_LOGW(TAG, "SD SPI bus init failed");
        return false;
    }

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 20,
        .allocation_unit_size   = 16 * 1024,
    };
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs   = PIN_SD_CS;
    slot.host_id   = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mcfg, &s_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) — using SPIFFS", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted at %s (%llu MB)",
             SD_MOUNT, ((uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size) >> 20);
    return true;
}

static void spiffs_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_MOUNT,
        .partition_label        = "spiffs",
        .max_files              = 20,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS failed: %s", esp_err_to_name(ret));
        lcd_line(0, "SPIFFS ERROR");
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %u KB total, %u KB used",
             (unsigned)(total/1024), (unsigned)(used/1024));
}

static void storage_init(void) {
    if (sd_init()) {
        g_storage_root = SD_MOUNT;
        lcd_line(0, "KAOS Portal");
        lcd_line(1, "SD card OK");
    } else {
        spiffs_init();
        g_storage_root = SPIFFS_MOUNT;
        lcd_line(0, "KAOS Portal");
        lcd_line(1, "SPIFFS storage");
    }
}

/* -----------------------------------------------------------------------
 * File scan
 * ----------------------------------------------------------------------- */
static bool is_sky_file(const char *n) {
    int l = strlen(n);
    return l > 4 && (
        strcasecmp(n+l-4, ".bin")  == 0 ||
        strcasecmp(n+l-4, ".dmp")  == 0 ||
        strcasecmp(n+l-4, ".sky")  == 0 ||
        (l > 5 && strcasecmp(n+l-5, ".dump") == 0));
}

void scan_files(void) {
    g_file_count = 0;
    DIR *dp = opendir(g_storage_root);
    if (!dp) { ESP_LOGE(TAG, "Cannot open %s", g_storage_root); return; }
    struct dirent *e;
    while ((e = readdir(dp)) && g_file_count < 64) {
        if (is_sky_file(e->d_name)) {
            strncpy(g_file_list[g_file_count], e->d_name, 63);
            g_file_list[g_file_count][63] = '\0';
            g_file_count++;
        }
    }
    closedir(dp);

    for (int a = 0; a < g_file_count - 1; a++)
        for (int b = a + 1; b < g_file_count; b++)
            if (strcmp(g_file_list[a], g_file_list[b]) > 0) {
                char tmp[64];
                strncpy(tmp, g_file_list[a], 64);
                strncpy(g_file_list[a], g_file_list[b], 64);
                strncpy(g_file_list[b], tmp, 64);
            }

    for (int i = 0; i < g_file_count; i++)
        ESP_LOGI(TAG, "  [%d] %s", i, g_file_list[i]);
    ESP_LOGI(TAG, "%d file(s) on %s", g_file_count, g_storage_root);
}

void spiffs_full_path(const char *basename, char *out, size_t out_len) {
    snprintf(out, out_len, "%s/%s", g_storage_root, basename);
}

/* -----------------------------------------------------------------------
 * WiFi Access Point
 * ----------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_STACONNECTED)
        ESP_LOGI(TAG, "Client connected");
    else if (id == WIFI_EVENT_AP_STADISCONNECTED)
        ESP_LOGI(TAG, "Client disconnected");
}

static void wifi_ap_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    wifi_config_t wcfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = WIFI_AP_PASSWORD,
            .max_connection = 4,
            .authmode       = strlen(WIFI_AP_PASSWORD) ?
                              WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP '%s' ready at %s", WIFI_AP_SSID, PORTAL_IP);
}

/* -----------------------------------------------------------------------
 * app_main
 * ----------------------------------------------------------------------- */
void app_main(void) {
    ESP_LOGI(TAG, "=== KAOS Portal ===");

    /* NVS */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* I2C + LCD */
    i2c_config_t ic = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_PORT, &ic);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    lcd_init();
    lcd_line(0, "KAOS Portal");
    lcd_line(1, "Starting...");

    /* Mutex */
    g_sky_mutex = xSemaphoreCreateMutex();

    /* Storage — SD card if present, SPIFFS fallback */
    storage_init();
    scan_files();

    /* Pico bridge (UART2) */
    pico_bridge_init();

    /* WiFi AP */
    wifi_ap_init();

    /* Web UI */
    web_ui_start();

    /* LCD: show AP name + password */
    lcd_line(0, WIFI_AP_SSID);
    lcd_line(1, "pw:" WIFI_AP_PASSWORD);

    ESP_LOGI(TAG, "Ready. WiFi: '%s'  URL: http://%s",
             WIFI_AP_SSID, PORTAL_IP);

    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
