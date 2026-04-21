#pragma once
#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#define PORTAL_USB_VID        0x1430
#define PORTAL_USB_PID        0x0150
#define PORTAL_HID_REPORT_LEN 32
#define PORTAL_EP_IN          0x81   /* interrupt IN — portal → host */
#define PORTAL_EP_POLL_MS     1      /* 1ms polling interval */

/* No PORTAL_EP_OUT — PS3 wireless protocol sends commands
 * via HID SET_REPORT control requests on EP0, not an interrupt OUT */

#endif
