#pragma once
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

/* Device-mode only */
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_PICO

/* HID */
#define CFG_TUD_HID             1
#define CFG_TUD_HID_EP_BUFSIZE  64

/* Disable unused classes */
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#endif
