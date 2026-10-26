/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UNIXNU_USB_XHCI_H
#define UNIXNU_USB_XHCI_H
#include "UsbCore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_XHCI_TRB_BYTES 16u
#define USB_XHCI_MAX_TRANSFER_RING_ENTRIES 4096u
#define USB_XHCI_MIN_TRANSFER_RING_ENTRIES 4u
#define USB_XHCI_MIN_EVENT_RING_ENTRIES 16u
#define USB_XHCI_MAX_EVENT_RING_ENTRIES 65535u
#define USB_XHCI_PORT_BITMAP_BYTES 32u

#define USB_XHCI_TRB_TYPE_SHIFT 10u
#define USB_XHCI_TRB_TYPE_MASK 0x3fu
#define USB_XHCI_TRB_CYCLE 0x00000001u
#define USB_XHCI_TRB_TOGGLE_CYCLE 0x00000002u

enum usb_xhci_trb_type {
    USB_XHCI_TRB_NORMAL = 1,
    USB_XHCI_TRB_SETUP_STAGE = 2,
    USB_XHCI_TRB_DATA_STAGE = 3,
    USB_XHCI_TRB_STATUS_STAGE = 4,
    USB_XHCI_TRB_ISOCH = 5,
    USB_XHCI_TRB_LINK = 6,
    USB_XHCI_TRB_ENABLE_SLOT = 9,
    USB_XHCI_TRB_DISABLE_SLOT = 10,
    USB_XHCI_TRB_ADDRESS_DEVICE = 11,
    USB_XHCI_TRB_CONFIGURE_ENDPOINT = 12,
    USB_XHCI_TRB_EVALUATE_CONTEXT = 13,
    USB_XHCI_TRB_RESET_ENDPOINT = 14,
    USB_XHCI_TRB_STOP_ENDPOINT = 15,
    USB_XHCI_TRB_SET_TR_DEQUEUE = 16,
    USB_XHCI_TRB_RESET_DEVICE = 17,
    USB_XHCI_TRB_NOOP_COMMAND = 23,
    USB_XHCI_TRB_TRANSFER_EVENT = 32,
    USB_XHCI_TRB_COMMAND_EVENT = 33,
    USB_XHCI_TRB_PORT_STATUS_EVENT = 34,
    USB_XHCI_TRB_HOST_CONTROLLER_EVENT = 37
};

struct usb_xhci_trb { uint8_t bytes[USB_XHCI_TRB_BYTES]; };

struct usb_xhci_capabilities {
    uint16_t version;
    uint16_t max_interrupts;
    uint8_t max_slots;
    uint8_t max_ports;
    uint8_t context_bytes;
    bool address_64bit;
    uint32_t operational_offset;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint32_t page_size_mask;
    uint8_t usb2_port_count;
    uint8_t usb2_ports[USB_XHCI_PORT_BITMAP_BYTES];
};

/* Read-only capability discovery. The result is committed only after every
 * register window and USB2 Supported Protocol range has been validated. */
int usb_xhci_read_capabilities(const struct usb_mmio *, struct usb_xhci_capabilities *);

struct usb_xhci_transfer_ring {
    struct usb_dma_ops dma;
    struct usb_dma_buffer trbs;
    uint32_t entries;          /* Includes the controller-visible Link TRB. */
    uint32_t usable_entries;    /* Transfer/command TRBs, excluding Link. */
    uint32_t producer_index;
    uint32_t consumer_index;
    uint32_t outstanding;
    uint8_t producer_cycle;
    bool controller_active;
    bool faulted;
};

/* One-segment ring with an explicit Link TRB. Descriptor memory must be DMA32,
 * 64-byte aligned and hardware coherent. Caller serializes submit/completion
 * processing, consumes events in event-ring order, and rings the appropriate
 * xHCI doorbell only after submit returns. */
int usb_xhci_transfer_ring_init(struct usb_xhci_transfer_ring *, const struct usb_dma_ops *, uint32_t entries);
int usb_xhci_transfer_ring_mark_active(struct usb_xhci_transfer_ring *);
int usb_xhci_transfer_ring_enqueue(struct usb_xhci_transfer_ring *, const struct usb_xhci_trb *,
                                   size_t count, uint64_t *first_trb_address);
int usb_xhci_transfer_ring_complete_through(struct usb_xhci_transfer_ring *, uint64_t trb_address,
                                            uint32_t *completed_trbs);
/* Call only after a fresh controller/endpoint stop proof. */
int usb_xhci_transfer_ring_halted(struct usb_xhci_transfer_ring *);
int usb_xhci_transfer_ring_destroy(struct usb_xhci_transfer_ring *);

struct usb_xhci_event_ring {
    struct usb_dma_ops dma;
    struct usb_dma_buffer erst;
    struct usb_dma_buffer trbs;
    uint32_t entries;
    uint32_t dequeue_index;
    uint8_t consumer_cycle;
    bool controller_active;
    bool faulted;
};

/* The event ring is a single ERST segment. Its event/ERST memory is DMA32,
 * 64-byte aligned and hardware coherent. The owner writes ERSTBA/ERSTSZ and
 * ERDP through its own MMIO backend and then marks the ring active. */
int usb_xhci_event_ring_init(struct usb_xhci_event_ring *, const struct usb_dma_ops *, uint32_t entries);
int usb_xhci_event_ring_mark_active(struct usb_xhci_event_ring *);
int usb_xhci_event_ring_read(struct usb_xhci_event_ring *, struct usb_xhci_trb *);
int usb_xhci_event_ring_dequeue_address(const struct usb_xhci_event_ring *, uint64_t *);
int usb_xhci_event_ring_erst_address(const struct usb_xhci_event_ring *, uint64_t *);
/* Call only after a fresh controller halt proof. */
int usb_xhci_event_ring_halted(struct usb_xhci_event_ring *);
int usb_xhci_event_ring_destroy(struct usb_xhci_event_ring *);

#ifdef __cplusplus
}
#endif
#endif
