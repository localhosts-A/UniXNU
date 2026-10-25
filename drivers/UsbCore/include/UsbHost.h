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

enum usb_host_speed { USB_HOST_LOW_SPEED, USB_HOST_FULL_SPEED, USB_HOST_HIGH_SPEED };
enum usb_host_state { USB_HOST_EMPTY, USB_HOST_ADDRESSED, USB_HOST_CONFIGURED, USB_HOST_FAULTED };
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
    /* On success actual is bounded by capacity. Control may return STALL. */
    int (*control)(void *, const struct usb_host_route *, uint8_t address,
                   const struct usb_setup *, void *, size_t capacity,
                   size_t *actual, uint32_t timeout_ms);
    /* xHCI Address Device (or equivalent) owns address assignment and EP0 setup.
     * If it returns failure it must leave no live slot or DMA ownership behind. */
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
     * stop proof. Failure keeps the device object intact for retry. */
    int (*disable_device)(void *, const struct usb_host_route *, uint8_t address);
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
    struct usb_bot bot;
    struct usb_transport bot_transport;
    bool has_bot;
};

/* The caller zero-initializes device before first use. Enumeration uses the
 * controller's address_device operation because xHCI
 * assigns USB addresses as part of Address Device; it does not send SET_ADDRESS
 * as a separate control transfer. The caller must reset/power the route first. */
int usb_host_enumerate(struct usb_host_device *, const struct usb_host_ops *,
                       const struct usb_host_route *, enum usb_host_speed,
                       uint8_t preferred_address);
int usb_host_start_storage(struct usb_host_device *);
int usb_host_poll_input(struct usb_host_device *, usb_key_event, usb_pointer_event,
                        void *event_context, uint32_t timeout_ms);
int usb_host_disconnect(struct usb_host_device *, usb_key_event, usb_pointer_event,
                        void *event_context);

#ifdef __cplusplus
}
#endif
#endif
