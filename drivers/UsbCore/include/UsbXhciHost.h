/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UNIXNU_USB_XHCI_HOST_H
#define UNIXNU_USB_XHCI_HOST_H
#include "UsbHost.h"
#include "UsbXhci.h"

#ifdef __cplusplus
extern "C" {
#endif

struct usb_xhci_host_device {
    struct usb_xhci_device_slot slot;
    struct usb_host_route route;
    bool owned;
};

/* Bridge storage is caller-owned so the platform can choose an appropriate
 * allocation context. All callbacks take the same controller-wide lock because
 * the portable controller and its event queue are synchronous and serialized. */
struct usb_xhci_host_adapter {
    struct usb_xhci_controller *controller;
    struct usb_xhci_host_device *devices;
    size_t device_capacity;
    int (*lock)(void *);
    void (*unlock)(void *);
    void *lock_context;
    uint32_t (*now_ms)(void *);
    void *clock_context;
    uint32_t command_timeout_ms;
};

/* The controller must already be running under the same exclusive platform
 * lease. Each device record must be zero-initialized and remains owned until
 * disable_device or release_stopped proves its DMA is safe to release. */
int usb_xhci_host_adapter_init(struct usb_xhci_host_adapter *,
                               struct usb_xhci_controller *,
                               struct usb_xhci_host_device *, size_t device_capacity,
                               int (*lock)(void *), void (*unlock)(void *), void *lock_context,
                               uint32_t (*now_ms)(void *), void *clock_context,
                               uint32_t command_timeout_ms);
int usb_xhci_host_adapter_get_ops(struct usb_xhci_host_adapter *, struct usb_host_ops *);

/* Reset an already powered USB2 root port. On USB_BUSY, drain events with
 * next_event and retry; power and connector policy remain platform-owned. */
int usb_xhci_host_adapter_reset_root_port(struct usb_xhci_host_adapter *, uint8_t root_port,
                                          uint32_t timeout_ms, enum usb_host_speed *speed);
int usb_xhci_host_adapter_root_port_get_status(struct usb_xhci_host_adapter *, uint8_t root_port,
                                              struct usb_xhci_root_port_status *);
int usb_xhci_host_adapter_root_port_ack_changes(struct usb_xhci_host_adapter *, uint8_t root_port,
                                                uint32_t changes);
int usb_xhci_host_adapter_next_event(struct usb_xhci_host_adapter *, struct usb_xhci_event *);

/* Acquires the adapter lock. Call only after every USB host/child user has
 * stopped and usb_xhci_controller_stop succeeded. */
int usb_xhci_host_adapter_release_stopped(struct usb_xhci_host_adapter *);

#ifdef __cplusplus
}
#endif
#endif
