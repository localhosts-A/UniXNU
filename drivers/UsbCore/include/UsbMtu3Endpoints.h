/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIXNU_USB_MTU3_ENDPOINTS_H
#define UNIXNU_USB_MTU3_ENDPOINTS_H
#include "UsbCore.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Fixed allocation for CDC notification IN1, bulk IN2 and bulk OUT2. USB2 only,
 * two 512-byte FIFO segments per endpoint, with independent DMA buffers/queues.
 * The owner serializes calls and stops/cancels QMU before changing this state. */
struct usb_mtu3_endpoints {
    struct usb_mmio mac;
    uint32_t tx_base, rx_base, tx_bytes, rx_bytes;
    uint16_t halted_in, halted_out;
    bool enhanced, separate_fifo, configured, faulted;
};
/* Read-only capability probe. MAC/PHY/clocks and exclusive ownership must be
 * established first. Hardware version plus the FDT gen2cp override select the
 * QMU format; no capability register is assumed from the board model name. */
int usb_mtu3_endpoints_init(struct usb_mtu3_endpoints *, const struct usb_mmio *mac,
                           const struct usb_mmio *ippc, bool mac_owned, bool gen2cp_unsupported);
int usb_mtu3_endpoints_configure(struct usb_mtu3_endpoints *, bool high_speed);
/* All three QMU queues must be inactive. A fault is latched on partial writes;
 * successful deconfiguration after external queue teardown clears that fault. */
int usb_mtu3_endpoints_deconfigure(struct usb_mtu3_endpoints *);
/* Only the selected queue must be inactive; clear-halt also resets data toggle.
 * Caller must retain/reset its QMU ring before resubmitting requests. */
int usb_mtu3_endpoints_halt(struct usb_mtu3_endpoints *, uint8_t address, bool halt);

#ifdef __cplusplus
}
#endif
#endif
