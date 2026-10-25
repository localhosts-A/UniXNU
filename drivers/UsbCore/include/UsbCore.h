/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UNIXNU_USB_CORE_H
#define UNIXNU_USB_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum usb_result {
    USB_OK = 0, USB_INVALID = -1, USB_RANGE = -2, USB_TIMEOUT = -3,
    USB_IO = -4, USB_STALL = -5, USB_DISCONNECTED = -6, USB_NO_MEMORY = -7,
    USB_UNSUPPORTED = -8, USB_BUSY = -9, USB_COMMAND_FAILED = -10,
    USB_PHASE_ERROR = -11, USB_AGAIN = -12
};

/* Backends own serialization. Register access never extends this aperture. */
struct usb_mmio {
    void *context;
    uint32_t size;
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*delay_us)(void *, uint32_t);
};
int usb_mmio_read(const struct usb_mmio *, uint32_t offset, uint32_t *);
int usb_mmio_write(const struct usb_mmio *, uint32_t offset, uint32_t value);
int usb_mmio_poll(const struct usb_mmio *, uint32_t offset, uint32_t mask,
                  uint32_t value, uint32_t timeout_us, uint32_t interval_us);

enum usb_dma_direction { USB_DMA_TO_DEVICE, USB_DMA_FROM_DEVICE, USB_DMA_BIDIRECTIONAL };
struct usb_dma_buffer {
    void *cpu; uint64_t address; size_t size; void *cookie;
    /* Uncached or hardware coherent: simultaneous CPU/device descriptor access
     * must not invalidate dirty adjacent descriptors in the same cache line. */
    bool coherent;
};
struct usb_dma_ops {
    void *context;
    /* Must prove exclusive device ownership and the actual mapping policy. */
    bool qualified;
    int (*allocate)(void *, size_t size, size_t alignment, uint64_t limit, struct usb_dma_buffer *);
    void (*release)(void *, struct usb_dma_buffer *);
    int (*sync)(void *, struct usb_dma_buffer *, size_t offset, size_t length,
                enum usb_dma_direction, bool for_device);
};
int usb_dma_alloc32(const struct usb_dma_ops *, size_t size, size_t alignment, struct usb_dma_buffer *);
int usb_dma_sync(const struct usb_dma_ops *, struct usb_dma_buffer *, size_t offset, size_t length,
                 enum usb_dma_direction, bool for_device);
void usb_dma_free(const struct usb_dma_ops *, struct usb_dma_buffer *);

#define USB_MAX_INTERFACES 16
#define USB_MAX_ENDPOINTS 32
struct usb_endpoint { uint8_t address, type, interval; uint16_t max_packet; uint8_t interface_number; };
struct usb_interface { uint8_t number, klass, subclass, protocol, endpoint_count; };
struct usb_configuration {
    uint8_t value, interface_count, endpoint_count;
    struct usb_interface interfaces[USB_MAX_INTERFACES];
    struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
};
int usb_parse_configuration(const uint8_t *, size_t, struct usb_configuration *);

#define USB_LOG_BYTES 4096
/* Try-lock writers may be called from diagnostics/IRQ context: never wait on USB. */
struct usb_log_ring {
    uint8_t data[USB_LOG_BYTES];
    size_t read, write, count;
    uint64_t dropped;
    uint8_t locked;
};
void usb_log_init(struct usb_log_ring *);
size_t usb_log_write(struct usb_log_ring *, const void *, size_t);
size_t usb_log_take(struct usb_log_ring *, void *, size_t);
uint64_t usb_log_dropped(const struct usb_log_ring *);

struct usb_setup { uint8_t type, request; uint16_t value, index, length; };
int usb_setup_decode(const void *, size_t, struct usb_setup *);
enum usb_control_effect {
    USB_CONTROL_NONE, USB_CONTROL_ADDRESS, USB_CONTROL_CONFIGURATION,
    USB_CONTROL_CLEAR_HALT, USB_CONTROL_SET_HALT
};
struct usb_control_reply { size_t length; enum usb_control_effect effect; uint8_t argument; };
struct usb_cdc {
    uint16_t vendor, product, revision;
    bool high_speed;
    uint8_t address, configuration, line_coding[7];
    uint16_t halted_in, halted_out;
    uint16_t control_lines;
    char serial[33];
    struct usb_log_ring log;
};
int usb_cdc_init(struct usb_cdc *, uint16_t vendor, uint16_t product, uint16_t revision,
                 bool high_speed, const char *serial);
void usb_cdc_bus_reset(struct usb_cdc *, bool high_speed);
/* SET_ADDRESS is returned as an effect; hardware applies it AFTER the status ACK.
 * Call usb_cdc_commit_control only after successful status completion. */
int usb_cdc_control(struct usb_cdc *, const struct usb_setup *, const void *out_data,
                    size_t out_size, void *reply, size_t capacity, struct usb_control_reply *);
void usb_cdc_commit_control(struct usb_cdc *, const struct usb_control_reply *);

struct usb_keyboard { uint8_t modifiers, keys[6]; };
typedef void (*usb_key_event)(void *, uint16_t usage, bool down);
int usb_keyboard_report(struct usb_keyboard *, const uint8_t *, size_t, usb_key_event, void *);
void usb_keyboard_disconnect(struct usb_keyboard *, usb_key_event, void *);
struct usb_mouse { uint8_t buttons; };
typedef void (*usb_pointer_event)(void *, uint8_t buttons, int16_t dx, int16_t dy, int16_t wheel);
int usb_mouse_report(struct usb_mouse *, const uint8_t *, size_t, usb_pointer_event, void *);
void usb_mouse_disconnect(struct usb_mouse *, usb_pointer_event, void *);

struct usb_transport {
    void *context;
    int (*control)(void *, const struct usb_setup *, void *, size_t, size_t *, uint32_t timeout_ms);
    int (*bulk)(void *, uint8_t endpoint, void *, size_t, size_t *, uint32_t timeout_ms);
    int (*clear_halt)(void *, uint8_t endpoint);
};
struct usb_bot {
    struct usb_transport transport;
    uint8_t interface_number, in_endpoint, out_endpoint, lun;
    uint32_t tag;
    uint64_t blocks;
    uint32_t block_size;
    bool present, write_uncertain, needs_reset;
};
int usb_bot_init(struct usb_bot *, const struct usb_transport *, uint8_t interface_number,
                 uint8_t in_endpoint, uint8_t out_endpoint, uint8_t lun);
int usb_bot_reset(struct usb_bot *);
int usb_bot_command(struct usb_bot *, const uint8_t *cdb, size_t cdb_size,
                    bool input, void *data, size_t size, size_t *completed);
int usb_bot_capacity(struct usb_bot *);
int usb_bot_read(struct usb_bot *, uint64_t lba, uint32_t blocks, void *, size_t);
int usb_bot_write(struct usb_bot *, uint64_t lba, uint32_t blocks, const void *, size_t);
int usb_bot_sync(struct usb_bot *);
void usb_bot_detach(struct usb_bot *);

/* Switching is synchronous in a serialized worker. Each callback has its own
 * bounded deadline. An error leaves STOPPED; starting the opposite role is never
 * attempted after failed sync/cancel/stop or uncertain VBUS state. */
enum usb_role { USB_ROLE_STOPPED, USB_ROLE_DEVICE, USB_ROLE_HOST };
struct usb_role_ops {
    void *context;
    int (*stop_submissions)(void *);
    int (*sync_storage)(void *);
    int (*cancel_requests)(void *);
    int (*stop_dma)(void *);
    int (*vbus)(void *, bool source);
    int (*select_role)(void *, enum usb_role);
    int (*start)(void *, enum usb_role);
};
struct usb_role_state { enum usb_role role; bool switching; bool faulted; };
int usb_role_switch(struct usb_role_state *, const struct usb_role_ops *, enum usb_role);
/* Quiesce is idempotent; a successful call may be repeated for EBS retry. */
int usb_role_quiesce(struct usb_role_state *, const struct usb_role_ops *);

#ifdef __cplusplus
}
#endif
#endif
