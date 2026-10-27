/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UNIXNU_USB_HOST_H
#define UNIXNU_USB_HOST_H
#include "UsbCore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_HOST_MAX_ROUTE_DEPTH 5
#define USB_HOST_MAX_CONFIGURATION 2048
#define USB_HOST_MAX_HID_INTERFACES 4
#define USB_HOST_HUB_MAX_PORTS 127
#define USB_HOST_HUB_MAX_BITMAP_BYTES 16

enum usb_host_speed { USB_HOST_LOW_SPEED, USB_HOST_FULL_SPEED, USB_HOST_HIGH_SPEED };
enum usb_host_state {
    USB_HOST_EMPTY, USB_HOST_ADDRESSED, USB_HOST_CONFIGURED, USB_HOST_FAULTED,
    USB_HOST_ENUMERATING
};
enum usb_host_hid_kind { USB_HOST_HID_KEYBOARD, USB_HOST_HID_MOUSE };

/* A route is copied into each device so controller backends can encode xHCI
 * route strings and USB2 TT information without consulting mutable hub state. */
struct usb_host_route {
    uint8_t root_port;
    uint8_t depth;
    uint8_t hub_address[USB_HOST_MAX_ROUTE_DEPTH];
    uint8_t hub_port[USB_HOST_MAX_ROUTE_DEPTH];
    uint8_t tt_hub_address;
    uint8_t tt_port;
    uint16_t tt_think_time_ns;
    bool multi_tt;
};

struct usb_host_ops {
    void *context;
    /* On success actual is bounded by capacity. Control may return STALL.
     * Without prepare_device, an address-zero control must not leave any
     * slot/DMA ownership behind on success or failure. */
    int (*control)(void *, const struct usb_host_route *, uint8_t address,
                   const struct usb_setup *, void *, size_t capacity,
                   size_t *actual, uint32_t timeout_ms);
    /* xHCI Address Device (or equivalent) owns address assignment and EP0 setup.
     * Without prepare_device, failure must leave no live slot or DMA ownership.
     * With prepare_device, failure retains the prepared slot for disable_device.
     * A successful assignment must return a USB address in 1..127. */
    int (*address_device)(void *, const struct usb_host_route *, enum usb_host_speed,
                         uint8_t ep0_max_packet, uint8_t preferred_address,
                         uint8_t *assigned_address);
    int (*configure_endpoint)(void *, const struct usb_host_route *, uint8_t address,
                              enum usb_host_speed, const struct usb_endpoint *);
    int (*interrupt)(void *, const struct usb_host_route *, uint8_t address,
                     uint8_t endpoint, void *, size_t capacity, size_t *actual,
                     uint32_t timeout_ms);
    int (*bulk)(void *, const struct usb_host_route *, uint8_t address,
                uint8_t endpoint, void *, size_t, size_t *, uint32_t timeout_ms);
    /* Reset the controller-side endpoint state after CLEAR_FEATURE succeeds. */
    int (*reset_endpoint)(void *, const struct usb_host_route *, uint8_t address,
                          uint8_t endpoint);
    /* Must halt/cancel every endpoint and release slot/DMA only after a fresh
     * stop proof. Failure keeps the device object intact for retry. If a
     * successful address_device returned an invalid address, use the route and
     * independently retained slot ownership to clean up; never treat that
     * invalid address alone as a slot identifier or proof. */
    int (*disable_device)(void *, const struct usb_host_route *, uint8_t address);
    /* Monotonic millisecond clock used by the nonblocking hub power/reset state machine. */
    uint32_t (*now_ms)(void *);
    /* Optional default-address setup, called before the first EP0 request.
     * Set *cleanup_required before any action that can acquire a slot or DMA,
     * including actions whose failure may have taken effect. Success requires
     * it to be true. If true, disable_device(route, 0) must identify the owned
     * slot independently of the address and prove its cleanup, including when
     * a failed address command already changed the hardware address and after
     * an applied-but-error cleanup. A failure with false must have no ownership.
     * The owner serializes enumeration with all other use of the route. */
    int (*prepare_device)(void *, const struct usb_host_route *, enum usb_host_speed,
                          bool *cleanup_required);
};

struct usb_host_hid_interface {
    enum usb_host_hid_kind kind;
    uint8_t interface_number;
    uint8_t endpoint;
    uint8_t interval;
    uint16_t max_packet;
    bool active;
    union { struct usb_keyboard keyboard; struct usb_mouse mouse; } state;
};

struct usb_host_hub_interface {
    uint8_t interface_number;
    uint8_t endpoint;
    uint8_t interval;
    uint8_t protocol;
    uint16_t max_packet;
    uint8_t multi_tt_endpoint;
    uint8_t multi_tt_interval;
    uint16_t multi_tt_max_packet;
    bool multi_tt_available;
    bool use_multi_tt;
    bool present;
};

struct usb_host_device {
    enum usb_host_state state;
    struct usb_host_ops ops;
    struct usb_host_route route;
    enum usb_host_speed speed;
    uint8_t address;
    uint8_t configuration;
    uint8_t device_descriptor[18];
    struct usb_configuration parsed_configuration;
    struct usb_host_hid_interface hid[USB_HOST_MAX_HID_INTERFACES];
    uint8_t hid_count;
    struct usb_host_hub_interface hub_interface;
    struct usb_bot bot;
    struct usb_transport bot_transport;
    bool has_bot;
    bool stop_proven;
    bool lifecycle_busy;
    bool stop_invalid;
    bool invalid_address_cleanup;
    bool default_address_cleanup;
};

enum usb_host_hub_event_kind {
    USB_HOST_HUB_ATTACH,
    USB_HOST_HUB_DETACH,
    USB_HOST_HUB_OVERCURRENT
};

/* ATTACH starts child enumeration; DETACH and OVERCURRENT must quiesce and
 * release the child before returning. A failed ATTACH is retried independently
 * of the hardware reset deadline; unplug/stop sends DETACH to clean partial
 * child state. Other callback errors are retried while the port state remains
 * owned. Port/hub overcurrent is fail-stopped until the owner successfully
 * stops and restarts the hub. */
typedef int (*usb_host_hub_event)(void *, enum usb_host_hub_event_kind,
                                  uint8_t port, const struct usb_host_route *,
                                  enum usb_host_speed);

struct usb_host_hub_port {
    bool powered;
    bool attached;
    bool reset_pending;
    bool attach_pending;
    bool reset_failed;
    bool overcurrent_latched;
    enum usb_host_speed speed;
    uint32_t reset_ready_ms;
    uint32_t reset_deadline_ms;
};

struct usb_host_hub {
    bool initialized;
    bool faulted;
    bool scan_all_ports;
    uint8_t interface_number;
    uint8_t endpoint;
    uint8_t interval;
    uint8_t protocol;
    uint8_t port_count;
    uint8_t bitmap_bytes;
    uint16_t power_good_ms;
    uint16_t characteristics;
    uint16_t tt_think_time_ns;
    bool multi_tt;
    uint32_t power_ready_ms;
    struct usb_host_hub_port ports[USB_HOST_HUB_MAX_PORTS];
};

/* The caller zero-initializes device before first use. Enumeration uses the
 * controller's address_device operation because xHCI
 * assigns USB addresses as part of Address Device; it does not send SET_ADDRESS
 * as a separate control transfer. The caller must reset/power the route first.
 * A backend needing a slot before the first descriptor read must implement
 * prepare_device. Failed cleanup retains a faulted device for stop retry. */
int usb_host_enumerate(struct usb_host_device *, const struct usb_host_ops *,
                       const struct usb_host_route *, enum usb_host_speed,
                       uint8_t preferred_address);
int usb_host_start_storage(struct usb_host_device *);
/* These descriptors describe SET_PROTOCOL(BOOT), not the device's Report
 * protocol descriptor. No Report ID; keyboard input is 8 bytes, mouse 3.
 * Mouse buttons 1-5 are retained; a fourth vendor wheel byte is discarded by
 * read_boot_report and remains available only through the legacy translator.
 * The descriptor pointer has static lifetime. */
int usb_host_boot_report_descriptor(enum usb_host_hid_kind, const uint8_t **, size_t *);
/* Synchronous copied report for an OS HID consumer. The owner serializes this
 * call with poll/disconnect and keeps the device alive until it returns.
 * AGAIN/TIMEOUT/DISCONNECTED do not stop or release the device: the owner must
 * close HID admission, stop endpoints, then detach child services. Failed
 * transfers leave the caller buffer unchanged and set actual to zero. */
int usb_host_read_boot_report(struct usb_host_device *, uint8_t hid_index,
                              uint8_t *, size_t capacity, size_t *actual,
                              uint32_t timeout_ms);
/* Legacy translated-input path. A disconnect automatically stops and clears
 * host state; do not use while any external child retains that state or BOT.
 * Its event callbacks must not reenter host operations. */
int usb_host_poll_input(struct usb_host_device *, usb_key_event, usb_pointer_event,
                        void *event_context, uint32_t timeout_ms);
/* The owner serializes host calls and quiesces child services before stop.
 * This closes host admission and asks the backend to stop every endpoint. A
 * failed stop leaves the device faulted and all host/BOT state retained for a
 * retry. Reentrant lifecycle calls from stop/release callbacks return BUSY.
 * The owner retains exclusive backend ownership and must not restart or reuse
 * this device while the stop proof is held. */
int usb_host_stop_device(struct usb_host_device *);
/* Requires a successful usb_host_stop_device(). Call after child services have
 * detached; legacy translated-HID releases and BOT cleanup happen before host
 * state clears. The raw-report XNU HID child sends its own neutral report
 * during its earlier quiesce. */
int usb_host_release_stopped_device(struct usb_host_device *, usb_key_event,
                                    usb_pointer_event, void *event_context);
/* Convenience stop-and-release path for devices without retained OS children. */
int usb_host_disconnect(struct usb_host_device *, usb_key_event, usb_pointer_event,
                        void *event_context);

/* The caller zero-initializes hub state before start. USB 2 hub service requires
 * the controller clock callback, which must be monotonic in ms;
 * wrap is supported for bounded power/reset deadlines. The owner must stop
 * child services before disconnecting the hub device. */
int usb_host_hub_start(struct usb_host_device *, struct usb_host_hub *);
int usb_host_hub_poll(struct usb_host_device *, struct usb_host_hub *,
                      uint32_t timeout_ms,
                      usb_host_hub_event, void *event_context);
int usb_host_hub_child_route(const struct usb_host_device *,
                             const struct usb_host_hub *, uint8_t port,
                             enum usb_host_speed, struct usb_host_route *);
int usb_host_hub_stop(struct usb_host_device *, struct usb_host_hub *,
                      usb_host_hub_event, void *event_context);

#ifdef __cplusplus
}
#endif
#endif
