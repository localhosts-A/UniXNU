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
#define USB_XHCI_MAX_EVENT_RING_ENTRIES 4096u
#define USB_XHCI_PORT_BITMAP_BYTES 32u
#define USB_XHCI_MAX_ROUTE_DEPTH 5u
#define USB_XHCI_PENDING_EVENTS 32u
#define USB_XHCI_MAX_NONCONTROL_ENDPOINTS 30u

#define USB_XHCI_TRB_TYPE_SHIFT 10u
#define USB_XHCI_TRB_TYPE_MASK 0x3fu
#define USB_XHCI_TRB_CYCLE 0x00000001u
#define USB_XHCI_TRB_TOGGLE_CYCLE 0x00000002u
#define USB_XHCI_TRB_EVENT_DATA 0x00000004u
#define USB_XHCI_TRB_ISP 0x00000004u
#define USB_XHCI_TRB_CHAIN 0x00000010u
#define USB_XHCI_TRB_IOC 0x00000020u

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
    USB_XHCI_TRB_BANDWIDTH_EVENT = 35,
    USB_XHCI_TRB_DOORBELL_EVENT = 36,
    USB_XHCI_TRB_HOST_CONTROLLER_EVENT = 37
};

struct usb_xhci_trb { uint8_t bytes[USB_XHCI_TRB_BYTES]; };

enum usb_xhci_completion_code {
    USB_XHCI_CC_INVALID = 0,
    USB_XHCI_CC_SUCCESS = 1,
    USB_XHCI_CC_STALL_ERROR = 6,
    USB_XHCI_CC_SHORT_PACKET = 13,
    USB_XHCI_CC_RING_UNDERRUN = 14,
    USB_XHCI_CC_RING_OVERRUN = 15,
    USB_XHCI_CC_VF_EVENT_RING_FULL = 16,
    USB_XHCI_CC_EVENT_RING_FULL = 21,
    USB_XHCI_CC_COMMAND_RING_STOPPED = 24,
    USB_XHCI_CC_COMMAND_ABORTED = 25,
    USB_XHCI_CC_STOPPED = 26,
    USB_XHCI_CC_STOPPED_LENGTH_INVALID = 27,
    USB_XHCI_CC_STOPPED_SHORT_PACKET = 28
};

enum usb_xhci_event_kind {
    USB_XHCI_EVENT_RAW = 0,
    USB_XHCI_EVENT_TRANSFER,
    USB_XHCI_EVENT_COMMAND_COMPLETION,
    USB_XHCI_EVENT_PORT_STATUS_CHANGE,
    USB_XHCI_EVENT_HOST_CONTROLLER
};

struct usb_xhci_event {
    struct usb_xhci_trb raw;
    enum usb_xhci_event_kind kind;
    uint8_t type;
    uint8_t cycle;
    uint8_t completion_code;
    union {
        struct {
            uint64_t pointer_or_data;
            uint32_t residual_bytes;
            uint8_t endpoint_id;
            uint8_t slot_id;
            bool event_data;
            /* False for Event Data and completion codes with no TRB pointer. */
            bool trb_pointer_valid;
        } transfer;
        struct {
            uint64_t command_trb_pointer;
            uint32_t completion_parameter;
            uint8_t vf_id;
            uint8_t slot_id;
            /* False for Invalid and Command Ring Stopped completions. */
            bool command_trb_pointer_valid;
        } command;
        struct { uint8_t port_id; } port_status;
    } data;
};

/* Decode little-endian Event TRB fields without normalizing completion codes.
 * Unhandled, optional and vendor event types are returned as RAW with bytes
 * preserved in raw. Event-specific pointer-valid flags encode only the xHCI
 * cases that must not be treated as completed TRB addresses. */
int usb_xhci_event_decode(const struct usb_xhci_trb *, struct usb_xhci_event *);

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

/* USB2 speed encodings are the values used by the xHCI Slot Context. */
enum usb_xhci_speed { USB_XHCI_SPEED_FULL = 1, USB_XHCI_SPEED_LOW = 2,
                      USB_XHCI_SPEED_HIGH = 3 };

/* Normalized sticky change flags returned by the guarded root-port snapshot.
 * Pass only these flags to usb_xhci_root_port_ack_changes(). */
#define USB_XHCI_ROOT_PORT_CHANGE_CONNECT       (1u << 0)
#define USB_XHCI_ROOT_PORT_CHANGE_ENABLE        (1u << 1)
#define USB_XHCI_ROOT_PORT_CHANGE_WARM_RESET    (1u << 2)
#define USB_XHCI_ROOT_PORT_CHANGE_OVERCURRENT   (1u << 3)
#define USB_XHCI_ROOT_PORT_CHANGE_RESET         (1u << 4)
#define USB_XHCI_ROOT_PORT_CHANGE_LINK_STATE    (1u << 5)
#define USB_XHCI_ROOT_PORT_CHANGE_CONFIG_ERROR  (1u << 6)
#define USB_XHCI_ROOT_PORT_CHANGE_MASK          0x7fu

struct usb_xhci_root_port_status {
    bool connected;
    bool enabled;
    bool overcurrent;
    bool resetting;
    bool powered;
    uint8_t speed_id; /* Raw xHCI speed ID; interpret only when connected. */
    uint32_t changes; /* Sticky normalized change flags; snapshot does not clear them. */
};

/* hub_port stores the actual downstream port; hub_port_count preserves the
 * parent hub's bNbrPorts for Route String encoding. For low/full-speed paths,
 * the TT fields identify an ancestor high-speed hub and its actual port. */
struct usb_xhci_route {
    uint8_t root_port;
    uint8_t depth;
    uint8_t hub_slot[USB_XHCI_MAX_ROUTE_DEPTH];
    uint8_t hub_port[USB_XHCI_MAX_ROUTE_DEPTH];
    uint8_t hub_port_count[USB_XHCI_MAX_ROUTE_DEPTH];
    uint8_t tt_hub_slot;
    uint8_t tt_port;
    bool multi_tt;
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
    uint8_t producer_cycle, consumer_cycle;
    bool controller_active;
    bool faulted;
};

/* One-segment ring with an explicit Link TRB. Descriptor memory must be DMA32,
 * hardware coherent and entirely within one 64 KiB region. Caller serializes submit/completion
 * processing, consumes events in event-ring order, and rings the appropriate
 * xHCI doorbell only after submit returns. A multi-TRB submission is handed
 * to the controller by publishing its first TRB last, even if the controller
 * is still polling after an earlier doorbell. */
int usb_xhci_transfer_ring_init(struct usb_xhci_transfer_ring *, const struct usb_dma_ops *, uint32_t entries);
int usb_xhci_transfer_ring_mark_active(struct usb_xhci_transfer_ring *);
int usb_xhci_transfer_ring_enqueue(struct usb_xhci_transfer_ring *, const struct usb_xhci_trb *,
                                   size_t count, uint64_t *first_trb_address);
int usb_xhci_transfer_ring_complete_through(struct usb_xhci_transfer_ring *, uint64_t trb_address,
                                            uint32_t *completed_trbs);
/* Complete through a decoded transfer event only when it names a real TRB. */
int usb_xhci_transfer_ring_complete_event(struct usb_xhci_transfer_ring *, const struct usb_xhci_event *,
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
 * 64-byte aligned and hardware coherent; the event segment is padded to a
 * 64-byte extent and does not cross a 64 KiB boundary. The owner writes ERSTBA/ERSTSZ and
 * ERDP through its own MMIO backend and then marks the ring active. */
int usb_xhci_event_ring_init(struct usb_xhci_event_ring *, const struct usb_dma_ops *, uint32_t entries);
int usb_xhci_event_ring_mark_active(struct usb_xhci_event_ring *);
int usb_xhci_event_ring_read(struct usb_xhci_event_ring *, struct usb_xhci_trb *);
int usb_xhci_event_ring_dequeue_address(const struct usb_xhci_event_ring *, uint64_t *);
int usb_xhci_event_ring_erst_address(const struct usb_xhci_event_ring *, uint64_t *);
/* Call only after a fresh controller halt proof. */
int usb_xhci_event_ring_halted(struct usb_xhci_event_ring *);
int usb_xhci_event_ring_destroy(struct usb_xhci_event_ring *);

/* Controller schedule owner. The platform guard must prove continuous exclusive
 * controller/MMIO/power ownership and exclude other register/DMA users for the
 * entire lifecycle. Calls are serialized by the owner. This polling-mode
 * controller does not establish IRQ routing, USB ports or device services.
 * Zero-initialize before first start. A failed start after reset is attempted
 * retains state and DMA; call stop and keep the platform lease until it succeeds. */
#define USB_XHCI_MAX_SCRATCHPADS 1023u
enum usb_xhci_controller_state {
    USB_XHCI_CONTROLLER_EMPTY, USB_XHCI_CONTROLLER_FAULTED, USB_XHCI_CONTROLLER_RUNNING
};
struct usb_xhci_controller {
    struct usb_mmio mmio;
    struct usb_dma_ops dma;
    struct usb_xhci_capabilities caps;
    int (*guard)(void *);
    void *guard_context;
    struct usb_dma_buffer dcbaa;
    struct usb_dma_buffer scratch_array;
    struct usb_dma_buffer scratch_pages[USB_XHCI_MAX_SCRATCHPADS];
    struct usb_xhci_transfer_ring commands;
    struct usb_xhci_event_ring events;
    uint32_t page_bytes;
    uint16_t scratch_count;
    uint16_t scratch_allocated;
    enum usb_xhci_controller_state state;
    uint64_t pending_command;
    uint64_t acknowledged_event;
    struct usb_xhci_event pending_events[USB_XHCI_PENDING_EVENTS];
    uint8_t slot_limit;
    uint8_t pending_event_head, pending_event_count;
    bool command_pending;
    bool reset_proven;
};
int usb_xhci_controller_start(struct usb_xhci_controller *, const struct usb_mmio *,
                              const struct usb_dma_ops *, int (*guard)(void *),
                              void *guard_context, uint8_t slots,
                              uint32_t command_entries, uint32_t event_entries);
/* Submit one command TRB and ring DB0. Caller owns command parameters and any
 * referenced DMA until its completion event or a successful controller stop.
 * A failed publication/doorbell may have taken effect: stop before release.
 * The owner serializes calls, consumes every event and applies a bounded timeout;
 * on timeout it must stop the controller, never reuse the pending command. */
int usb_xhci_controller_submit_command(struct usb_xhci_controller *,
                                       const struct usb_xhci_trb *);
/* Submit one command and wait with a bounded timeout. Unrelated events are
 * retained for next_event. A timeout faults the controller and retains DMA. */
int usb_xhci_controller_command(struct usb_xhci_controller *, const struct usb_xhci_trb *,
                                uint8_t expected_slot, uint32_t timeout_ms,
                                struct usb_xhci_event *completion);
/* Returns each event in ring order, including unrelated/unknown events. A
 * matching command completion retires the pending command; its completion code
 * is returned unmodified in the event. AGAIN means no event. An invalid or
 * unexpected or aborted command completion, DMA/MMIO error or controller error
 * faults the controller: retain buffers and stop. Output is unchanged on failure. Each
 * event advances ERDP at most to the last handled entry; an empty-ring poll
 * advances to the empty slot. */
int usb_xhci_controller_next_event(struct usb_xhci_controller *, struct usb_xhci_event *);

/* Reset an already powered USB2 root port under the controller's exclusive
 * platform guard. Earlier events are returned to the owner first (USB_BUSY);
 * completion requires both PRC/CCS/PED and a matching port-status event.
 * The caller supplies a bounded timeout. This does not power the port or
 * qualify the platform's power/connector policy. */
int usb_xhci_root_port_reset(struct usb_xhci_controller *, uint8_t root_port,
                             uint32_t timeout_ms, enum usb_xhci_speed *speed);
/* Read USB2 PORTSC state under the controller guard. On failure, output is
 * unchanged. The returned change bits are a snapshot and remain latched until
 * explicitly acknowledged. */
int usb_xhci_root_port_get_status(struct usb_xhci_controller *, uint8_t root_port,
                                  struct usb_xhci_root_port_status *);
/* Acknowledge only selected sticky change bits using safe RW1C writes. Other
 * PORTSC status/control bits are preserved. Use flags from a fresh snapshot. */
int usb_xhci_root_port_ack_changes(struct usb_xhci_controller *, uint8_t root_port,
                                   uint32_t changes);

/* One EP0 control TD on a slot/context and active endpoint ring established by
 * the owner, in the same controller DMA address space. The caller serializes
 * all ring use and routes every event to its
 * owner. It must retain this object and its slot until finish or fresh stop.
 * Zero-initialize the TD before first use. No more than one TD may be
 * outstanding on this ring. The private DMA32 data
 * buffer never crosses a 64 KiB boundary. Short IN data waits for status. */
struct usb_xhci_control_td {
    struct usb_xhci_controller *controller;
    struct usb_xhci_transfer_ring *ring;
    struct usb_dma_ops dma;
    struct usb_dma_buffer data;
    uint64_t data_trb, status_trb;
    size_t requested, actual;
    uint8_t slot, trb_count;
    bool input, pending, complete, short_seen, faulted;
};
int usb_xhci_control_submit(struct usb_xhci_control_td *, struct usb_xhci_controller *,
                            struct usb_xhci_transfer_ring *, uint8_t slot,
                            const struct usb_setup *, const void *out_data, size_t capacity);
/* AGAIN also means an unrelated event or a short data stage; it does not
 * retire the TD. A matching error faults the controller and retains DMA. */
int usb_xhci_control_event(struct usb_xhci_control_td *, const struct usb_xhci_event *);
/* Copies IN data only after the status completion; output is unchanged on
 * failure. For OUT/no-data transfers, buffer may be NULL. */
int usb_xhci_control_finish(struct usb_xhci_control_td *, void *buffer, size_t capacity, size_t *actual);
/* Only after controller/endpoint stop proof and ring_halted has succeeded. */
int usb_xhci_control_release_stopped(struct usb_xhci_control_td *);

/* USB2 bulk/interrupt endpoint state. One synchronous TD may be active per
 * endpoint. Buffers and rings remain owned until a completion event or a fresh
 * controller/slot stop proof. */
struct usb_xhci_device_endpoint {
    struct usb_endpoint descriptor;
    struct usb_xhci_transfer_ring ring;
    struct usb_dma_buffer transfer_data;
    uint64_t transfer_trb;
    uint32_t transfer_length;
    uint8_t dci;
    bool configured, transfer_pending, halted, needs_dequeue, faulted, input;
};

enum usb_xhci_device_slot_state {
    USB_XHCI_SLOT_EMPTY, USB_XHCI_SLOT_ENABLED, USB_XHCI_SLOT_DEFAULT,
    USB_XHCI_SLOT_ADDRESSED, USB_XHCI_SLOT_FAULTED
};
struct usb_xhci_device_slot {
    struct usb_xhci_controller *controller;
    struct usb_dma_ops dma;
    struct usb_xhci_route route;
    struct usb_dma_buffer output_context, input_context;
    struct usb_xhci_transfer_ring ep0;
    struct usb_xhci_control_td control;
    struct usb_xhci_device_endpoint endpoints[USB_XHCI_MAX_NONCONTROL_ENDPOINTS];
    enum usb_xhci_device_slot_state state;
    enum usb_xhci_speed speed;
    uint8_t slot_id, address, ep0_max_packet;
    uint8_t hub_port_count;
    bool is_hub;
    bool cleanup_required;
};
/* Prepare a route that has already been reset and powered. cleanup_required is
 * set before Enable Slot because the command may take effect despite an error.
 * Success performs Enable Slot and Address Device with BSR=1. */
int usb_xhci_device_prepare(struct usb_xhci_device_slot *, struct usb_xhci_controller *,
                            const struct usb_xhci_route *, enum usb_xhci_speed,
                            bool *cleanup_required);
/* After reading the first eight device descriptor bytes at address zero, update
 * EP0 MPS and run Address Device with BSR=0. */
int usb_xhci_device_address(struct usb_xhci_device_slot *, uint8_t ep0_max_packet,
                            uint8_t *assigned_address);
/* Publish parsed USB2 hub metadata with Configure Endpoint before child slots
 * are addressed. TT think time is in nanoseconds from the hub descriptor. */
int usb_xhci_device_configure_hub(struct usb_xhci_device_slot *, uint8_t port_count,
                                  uint16_t tt_think_time_ns, bool multi_tt);
int usb_xhci_device_control(struct usb_xhci_device_slot *, uint8_t address,
                            const struct usb_setup *, void *, size_t capacity,
                            size_t *actual, uint32_t timeout_ms);
/* Configure USB2 bulk/interrupt endpoints after SET_CONFIGURATION. Endpoint
 * addresses and descriptors are validated against the addressed slot speed. */
int usb_xhci_device_configure_endpoint(struct usb_xhci_device_slot *,
                                       const struct usb_endpoint *);
int usb_xhci_device_interrupt(struct usb_xhci_device_slot *, uint8_t address,
                              uint8_t endpoint, void *, size_t capacity,
                              size_t *actual, uint32_t timeout_ms);
int usb_xhci_device_bulk(struct usb_xhci_device_slot *, uint8_t address,
                         uint8_t endpoint, void *, size_t length,
                         size_t *actual, uint32_t timeout_ms);
/* After a STALL, issue Reset Endpoint before the caller clears the device-side
 * halt. The next transfer programs Set TR Dequeue Pointer before ringing. */
int usb_xhci_device_reset_endpoint(struct usb_xhci_device_slot *, uint8_t address,
                                   uint8_t endpoint, uint32_t timeout_ms);
/* Disable Slot is the stop proof while the controller runs. On controller stop,
 * call release_stopped to reclaim retained slot/context/ring DMA after reset. */
int usb_xhci_device_disable(struct usb_xhci_device_slot *, uint32_t timeout_ms);
int usb_xhci_device_release_stopped(struct usb_xhci_device_slot *);
/* Stop submissions and children first. Stop requires fresh halt and reset
 * proof before releasing any controller-visible memory; errors are retryable. */
int usb_xhci_controller_stop(struct usb_xhci_controller *);

#ifdef __cplusplus
}
#endif
#endif
