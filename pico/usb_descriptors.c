/*
 * usb_descriptors.c  —  Pi Pico / TinyUSB
 * Skylander Portal of Power  VID=0x1430  PID=0x0150
 */
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

/* ---- Device descriptor ---- */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = PORTAL_USB_VID,
    .idProduct          = PORTAL_USB_PID,
    .bcdDevice          = 0x0200,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* ---- HID report descriptor (vendor 32-byte IN+OUT) ---- */
static const uint8_t desc_hid_report[] = {
    0x06,0x00,0xFF,
    0x09,0x01,
    0xA1,0x01,
    0x19,0x01,0x29,0x40,0x15,0x00,0x26,0xFF,0x00,
    0x75,0x08,0x95,0x20,0x81,0x00,
    0x19,0x01,0x29,0x40,0x15,0x00,0x26,0xFF,0x00,
    0x75,0x08,0x95,0x20,0x91,0x00,
    0xC0,
};

/* ---- Configuration descriptor ---- */
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1,1,0,CONFIG_TOTAL_LEN,TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,100),
    TUD_HID_INOUT_DESCRIPTOR(0,0,HID_ITF_PROTOCOL_NONE,
        sizeof(desc_hid_report),
        PORTAL_EP_OUT, PORTAL_EP_IN,
        PORTAL_HID_REPORT_LEN, PORTAL_EP_POLL_MS),
};

static const char *s_str[] = {
    (const char[]){0x09,0x04},
    "Activision",
    "Spyro Portals",
    "00000001",
};

uint8_t const *tud_descriptor_device_cb(void)              { return (uint8_t const*)&desc_device; }
uint8_t const *tud_hid_descriptor_report_cb(uint8_t i)    { (void)i; return desc_hid_report; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t i) { (void)i; return desc_config; }

uint16_t const *tud_descriptor_string_cb(uint8_t idx, uint16_t langid) {
    (void)langid;
    static uint16_t buf[32];
    uint8_t n;
    if (idx == 0) { memcpy(&buf[1], s_str[0], 2); n=1; }
    else {
        if (idx >= sizeof(s_str)/sizeof(s_str[0])) return NULL;
        const char *str = s_str[idx];
        n = strlen(str); if(n>31) n=31;
        for(uint8_t i=0;i<n;i++) buf[1+i]=str[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING<<8)|(2*n+2));
    return buf;
}
