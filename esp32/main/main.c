/*
 * KAOS ESP32 — main.c
 *
 * This chip's only jobs:
 *   1. Host a WiFi Access Point
 *   2. Serve the web UI for picking/loading Skylanders
 *   3. Read Skylander files from SD card
 *   4. Push raw dumps to the Pi Pico over UART2
 *   5. Receive write-backs from the Pico and save them to SD
 *   6. Show the AP name and IP on the LCD
 *
 * No Python. No proxy. No USB.
 * The Pico handles all USB HID portal duties.
 *
 * Wiring:
 *   ESP32 GPIO17 (TX2) ──→ Pico GPIO5 (UART1 RX)
 *   ESP32 GPIO16 (RX2) ←── Pico GPIO4 (UART1 TX)
 *   ESP32 GND          ─── Pico GND
 *
 *   SD  MOSI=13  MISO=12  CLK=14  CS=15
 *   LCD SDA=21   SCL=22   addr=0x27
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
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "Skylander.h"
#include "web_ui.h"
#include "pico_bridge.h"

static const char *TAG = "KAOS-ESP32";

/* -----------------------------------------------------------------------
 * Config — change pins here if needed
 * ----------------------------------------------------------------------- */
#define WIFI_AP_SSID     "KAOS-Portal"
#define WIFI_AP_PASSWORD "skylands1"      /* shown on LCD — change this to whatever you want */
#define WIFI_AP_CHANNEL  6
#define PORTAL_IP        "192.168.4.1"

#define SD_SPI_HOST      HSPI_HOST
#define PIN_SD_MOSI      13
#define PIN_SD_MISO      19
#define PIN_SD_CLK       14
#define PIN_SD_CS        15
#define SD_MOUNT_POINT   "/sdcard"

#define I2C_PORT         I2C_NUM_0
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define LCD_I2C_ADDR     0x27           /* try 0x3F if blank */

/* -----------------------------------------------------------------------
 * Globals (shared with web_ui.c and pico_bridge.c)
 * ----------------------------------------------------------------------- */
SemaphoreHandle_t g_sky_mutex;
int               g_file_count = 0;
char              g_file_list[64][300];

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
 * SD card
 * ----------------------------------------------------------------------- */
static sdmmc_card_t *g_sd_card = NULL;

static void sd_init(void) {
    ESP_LOGI(TAG,"SD init: MOSI=%d MISO=%d CLK=%d CS=%d",
             PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CLK, PIN_SD_CS);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    spi_bus_config_t bus = {
        .mosi_io_num=PIN_SD_MOSI,.miso_io_num=PIN_SD_MISO,
        .sclk_io_num=PIN_SD_CLK,.quadwp_io_num=-1,.quadhd_io_num=-1,
        .max_transfer_sz=4096,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
    ESP_LOGI(TAG,"SPI bus init: %s", esp_err_to_name(ret));
    if (ret != ESP_OK) { lcd_line(0,"SPI BUS FAIL"); return; }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS; slot.host_id = SD_SPI_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed=false,.max_files=8,.allocation_unit_size=16*1024
    };
    esp_err_t mret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT,&host,&slot,&mnt,&g_sd_card);
    ESP_LOGI(TAG,"SD mount result: %s", esp_err_to_name(mret));
    if (mret != ESP_OK) {
        ESP_LOGE(TAG,"SD mount failed: %s", esp_err_to_name(mret));
        lcd_line(0,"SD CARD ERROR");
        lcd_line(1,"Check card!");
        return;
    }
    ESP_LOGI(TAG,"SD mounted OK");
    sdmmc_card_print_info(stdout, g_sd_card);
}

static void scan_files(void) {
    g_file_count = 0;
    ESP_LOGI(TAG,"Scanning %s ...", SD_MOUNT_POINT);
    DIR *dp = opendir(SD_MOUNT_POINT);
    if (!dp) {
        ESP_LOGE(TAG,"Cannot open SD root directory!");
        return;
    }
    struct dirent *e;
    while ((e=readdir(dp)) && g_file_count<64) {
        ESP_LOGI(TAG,"  found: '%s' type=%d", e->d_name, e->d_type);
        int l = strlen(e->d_name);
        if (e->d_type==DT_REG && l>4 &&
            (strcasecmp(e->d_name+l-4,".bin")==0 ||
             strcasecmp(e->d_name+l-4,".dmp")==0 ||
             strcasecmp(e->d_name+l-4,".sky")==0 ||
             (l>5&&strcasecmp(e->d_name+l-5,".dump")==0))) {
            snprintf(g_file_list[g_file_count],300,"%s/%s",SD_MOUNT_POINT,e->d_name);
            ESP_LOGI(TAG,"  -> loaded as slot [%d]",g_file_count);
            g_file_count++;
        } else {
            ESP_LOGW(TAG,"  -> skipped (wrong type/extension)");
        }
    }
    closedir(dp);
    ESP_LOGI(TAG,"Scan done: %d Skylander file(s) found", g_file_count);
}

/* -----------------------------------------------------------------------
 * WiFi Access Point
 * ----------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_STACONNECTED)
        ESP_LOGI(TAG,"Device connected to AP");
    else if (id == WIFI_EVENT_AP_STADISCONNECTED)
        ESP_LOGI(TAG,"Device disconnected from AP");
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
            .authmode       = strlen(WIFI_AP_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG,"AP '%s' started, IP: %s", WIFI_AP_SSID, PORTAL_IP);
}

/* -----------------------------------------------------------------------
 * app_main
 * ----------------------------------------------------------------------- */
void app_main(void) {
    ESP_LOGI(TAG,"=== KAOS Dual-Board Portal ===");

    /* NVS */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    /* I2C + LCD */
    i2c_config_t ic = {
        .mode=I2C_MODE_MASTER,
        .sda_io_num=PIN_I2C_SDA,.scl_io_num=PIN_I2C_SCL,
        .sda_pullup_en=GPIO_PULLUP_ENABLE,.scl_pullup_en=GPIO_PULLUP_ENABLE,
        .master.clk_speed=100000,
    };
    i2c_param_config(I2C_PORT,&ic);
    i2c_driver_install(I2C_PORT,I2C_MODE_MASTER,0,0,0);
    lcd_init();
    lcd_line(0,"KAOS Portal");
    lcd_line(1,"Starting...");

    /* Mutex */
    g_sky_mutex = xSemaphoreCreateMutex();

    /* SD */
    sd_init();
    if (g_sd_card) {
        scan_files();
        char msg[17];
        snprintf(msg,sizeof(msg),"%d file%s found",
                 g_file_count, g_file_count==1?"":"s");
        lcd_line(1,msg);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Pico bridge (UART2) */
    pico_bridge_init();

    /* WiFi AP */
    wifi_ap_init();

    /* HTTP server */
    web_ui_start();

    /* LCD: show AP name and password so only physical access reveals it */
    lcd_line(0, WIFI_AP_SSID);
    lcd_line(1, "pw:" WIFI_AP_PASSWORD);

    ESP_LOGI(TAG,"Ready.");
    ESP_LOGI(TAG,"  WiFi: '%s'", WIFI_AP_SSID);
    ESP_LOGI(TAG,"  Web UI: http://%s", PORTAL_IP);
    ESP_LOGI(TAG,"  Pico: UART2 TX=GPIO%d RX=GPIO%d", 17, 16);

    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
