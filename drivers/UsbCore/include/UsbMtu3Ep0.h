/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIXNU_USB_MTU3_EP0_H
#define UNIXNU_USB_MTU3_EP0_H
#include "UsbCore.h"

#ifdef __cplusplus
extern "C" {
#endif

enum usb_ep0_phase { USB_EP0_PREPARE, USB_EP0_APPLY, USB_EP0_ABORT };
struct usb_mtu3_ep0_ops {
    void *context;
    /* Native 8/16-bit FIFO writes; widening a tail write emits extra bytes. */
    int (*write_tail)(void *, uint32_t offset, unsigned width, uint32_t value);
    /* Required for configuration and endpoint-halt effects. PREPARE reserves or
     * quiesces endpoints before status; APPLY enables them after ACK. ABORT rolls
     * back a prepared transaction. All calls are serialized and must be bounded.
     * The callback must not re-enter EP0 or alter the CDC software state. */
    int (*effect)(void *, const struct usb_control_reply *, enum usb_ep0_phase);
};
enum usb_ep0_state {
    USB_EP0_OFF, USB_EP0_IDLE, USB_EP0_IN, USB_EP0_IN_LAST, USB_EP0_OUT,
    USB_EP0_STATUS, USB_EP0_STALLED, USB_EP0_FAULTED
};
struct usb_mtu3_ep0 {
    struct usb_mmio mac;
    struct usb_mtu3_ep0_ops ops;
    struct usb_cdc *cdc;
    struct usb_setup setup;
    struct usb_control_reply pending;
    uint8_t data[128];
    size_t length, position;
    uint64_t started_us, last_us;
    enum usb_ep0_state state;
    bool prepared, zlp;
};

/* MAC/PHY/clocks and exclusive ownership must already be established. This
 * module never forces VBUS or connects a pullup, and never touches DMA queues.
 * The port owner must stop/cancel nonzero endpoints BEFORE bus_reset/stop.
 * Only full/high speed (EP0 MPS 64) is supported. No heap allocations. */
int usb_mtu3_ep0_init(struct usb_mtu3_ep0 *, const struct usb_mmio *,
                      const struct usb_mtu3_ep0_ops *, struct usb_cdc *, bool mac_owned);
int usb_mtu3_ep0_start(struct usb_mtu3_ep0 *);
/* Poll from an IRQ worker or timer. now_us is monotonic; each transaction has a
 * one-second deadline. Poll even without an IRQ to observe status ACK/timeout. */
int usb_mtu3_ep0_poll(struct usb_mtu3_ep0 *, uint64_t now_us);
int usb_mtu3_ep0_bus_reset(struct usb_mtu3_ep0 *, bool high_speed);
int usb_mtu3_ep0_stop(struct usb_mtu3_ep0 *);

#ifdef __cplusplus
}
#endif
#endif
