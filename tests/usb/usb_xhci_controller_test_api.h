/* Test-only API for reusing the production xHCI controller register/DMA model. */
#ifndef UNIXNU_USB_XHCI_CONTROLLER_TEST_API_H
#define UNIXNU_USB_XHCI_CONTROLLER_TEST_API_H

#include "UsbXhciHost.h"

#ifdef __cplusplus
extern "C" {
#endif

struct usb_xhci_test_host;

struct usb_xhci_test_host_counters {
    unsigned host_control_requests;
    unsigned hid_protocol_selections;
    unsigned hid_interrupts;
    unsigned bot_capacity_commands;
    unsigned bot_read_only_commands;
    unsigned bot_read_commands;
    unsigned bot_sync_commands;
    unsigned bot_write_commands;
};

/* The caller owns the RAM medium and DMA ops through stop/destroy. */
struct usb_xhci_test_host *usb_xhci_test_host_create(
    uint8_t *media, size_t media_size, const uint8_t keyboard_report[8],
    const struct usb_dma_ops *dma_ops);
int usb_xhci_test_host_get_ops(struct usb_xhci_test_host *, struct usb_host_ops *);
int usb_xhci_test_host_get_counters(struct usb_xhci_test_host *,
                                    struct usb_xhci_test_host_counters *);
/* Stop is accepted only after the host owner has disabled its last device slot. */
int usb_xhci_test_host_stop(struct usb_xhci_test_host *);
int usb_xhci_test_host_destroy(struct usb_xhci_test_host *);

#ifdef __cplusplus
}
#endif
#endif
