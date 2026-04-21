/*
 * usb_descriptors.c  —  Pi Pico / TinyUSB
 * Skylander Portal of Power  VID=0x1430  PID=0x0150
 *
 * Portal type is read from portal_get_type() in main.c:
 *   0 = Spyro's Adventure / Giants    — bcdDevice 0x0001
 *   1 = Swap Force                    — bcdDevice 0x0001
 *   2 = Trap Team (Traptanium)        — bcdDevice 0x0200
 *   3 = Imaginators / SuperChargers   — bcdDevice 0x0200 (default)
 *
 * All types share VID/PID 0x1430/0x0150. The game differentiates by
 * the bcdDevice version and product string. Trap Team also checks that
 * the portal responds to the 'J' (fade light) command correctly.
 */
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

/* Declared in main.c */
extern uint8_t portal_get_type(void);

/* ---- Device descriptor — bcdDevice varies by portal type ---- */
uint8_t const *tud_descriptor_device_cb(void) {
    static tusb_desc_device_t desc;
    desc.bLength            = sizeof(tusb_desc_device_t);
    desc.bDescriptorType    = TUSB_DESC_DEVICE;
    desc.bcdUSB             = 0x0200;
    desc.bDeviceClass       = 0x00;
    desc.bDeviceSubClass    = 0x00;
    desc.bDeviceProtocol    = 0x00;
    desc.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE;
    desc.idVendor           = PORTAL_USB_VID;
    desc.idProduct          = PORTAL_USB_PID;
    /* Trap Team checks for bcdDevice >= 0x0200 to allow the Traptanium portal.
     * SSA/Giants/SwapForce used 0x0001. */
    desc.bcdDevice          = (portal_get_type() >= 2) ? 0x0200 : 0x0001;
    desc.iManufacturer      = 0x01;
    desc.iProduct           = 0x02;
    desc.iSerialNumber      = 0x03;
    desc.bNumConfigurations = 0x01;
    return (uint8_t const*)&desc;
}

/* ---- HID report descriptor — same for all types ---- */
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
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report),
                       PORTAL_EP_IN,
                       PORTAL_HID_REPORT_LEN,
                       PORTAL_EP_POLL_MS),
};

/* ---- String descriptors — product name varies by type ---- */
static const char *s_product_names[] = {
    "Spyro Portals",          /* 0: SSA / Giants */
    "Spyro Portals",          /* 1: Swap Force */
    "Traptanium Portal",      /* 2: Trap Team */
    "Spyro Portals",          /* 3: Imaginators */
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t i)    { (void)i; return desc_hid_report; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t i) { (void)i; return desc_config; }

uint16_t const *tud_descriptor_string_cb(uint8_t idx, uint16_t langid) {
    (void)langid;
    static uint16_t buf[32];
    uint8_t n;

    const char *strs[] = {
        (const char[]){0x09,0x04},
        "Activision",
        s_product_names[portal_get_type() & 0x03],
        "00000001",
    };

    if (idx == 0) { memcpy(&buf[1], strs[0], 2); n=1; }
    else {
        if (idx >= sizeof(strs)/sizeof(strs[0])) return NULL;
        const char *str = strs[idx];
        n = strlen(str); if(n>31) n=31;
        for(uint8_t i=0;i<n;i++) buf[1+i]=str[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING<<8)|(2*n+2));
    return buf;
}
