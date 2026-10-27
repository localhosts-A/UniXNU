/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbXhciHost.h"

static void zero_bytes(void *memory, size_t size)
{
    uint8_t *bytes = memory;
    while (size--) *bytes++ = 0;
}

static bool same_route_path(const struct usb_host_route *left,
                            const struct usb_host_route *right, uint8_t depth)
{
    if (!left || !right || left->root_port != right->root_port || left->depth != depth ||
        right->depth < depth) return false;
    for (uint8_t i = 0; i < depth; ++i)
        if (left->hub_address[i] != right->hub_address[i] ||
            left->hub_port[i] != right->hub_port[i]) return false;
    return true;
}

static bool same_route(const struct usb_host_route *left, const struct usb_host_route *right)
{
    if (!left || !right || left->root_port != right->root_port ||
        left->depth != right->depth || left->tt_hub_address != right->tt_hub_address ||
        left->tt_port != right->tt_port || left->tt_think_time_ns != right->tt_think_time_ns ||
        left->multi_tt != right->multi_tt) return false;
    return same_route_path(left, right, left->depth);
}

static bool valid_host_route(const struct usb_host_route *route)
{
    if (!route || !route->root_port || route->depth > USB_HOST_MAX_ROUTE_DEPTH) return false;
    for (uint8_t i = 0; i < route->depth; ++i) {
        if (!route->hub_address[i] || route->hub_address[i] > 127u || !route->hub_port[i])
            return false;
        for (uint8_t j = 0; j < i; ++j)
            if (route->hub_address[i] == route->hub_address[j]) return false;
    }
    if ((route->tt_hub_address == 0u) != (route->tt_port == 0u)) return false;
    if (route->multi_tt && (!route->tt_hub_address || !route->depth)) return false;
    if (route->tt_hub_address) {
        bool found = false;
        for (uint8_t i = 0; i < route->depth; ++i)
            if (route->hub_address[i] == route->tt_hub_address &&
                route->hub_port[i] == route->tt_port) found = true;
        if (!found) return false;
    }
    return true;
}

static int host_speed_to_xhci(enum usb_host_speed speed, enum usb_xhci_speed *out)
{
    if (!out) return USB_INVALID;
    switch (speed) {
    case USB_HOST_LOW_SPEED: *out = USB_XHCI_SPEED_LOW; return USB_OK;
    case USB_HOST_FULL_SPEED: *out = USB_XHCI_SPEED_FULL; return USB_OK;
    case USB_HOST_HIGH_SPEED: *out = USB_XHCI_SPEED_HIGH; return USB_OK;
    default: return USB_INVALID;
    }
}

static int xhci_speed_to_host(enum usb_xhci_speed speed, enum usb_host_speed *out)
{
    if (!out) return USB_INVALID;
    switch (speed) {
    case USB_XHCI_SPEED_LOW: *out = USB_HOST_LOW_SPEED; return USB_OK;
    case USB_XHCI_SPEED_FULL: *out = USB_HOST_FULL_SPEED; return USB_OK;
    case USB_XHCI_SPEED_HIGH: *out = USB_HOST_HIGH_SPEED; return USB_OK;
    default: return USB_PHASE_ERROR;
    }
}

static int adapter_acquire(struct usb_xhci_host_adapter *adapter)
{
    if (!adapter || !adapter->controller || !adapter->lock || !adapter->unlock)
        return USB_INVALID;
    return adapter->lock(adapter->lock_context);
}

static void adapter_unlock(struct usb_xhci_host_adapter *adapter)
{
    adapter->unlock(adapter->lock_context);
}

static int adapter_lock(struct usb_xhci_host_adapter *adapter)
{
    int result = adapter_acquire(adapter);
    if (result) return result;
    if (adapter->controller->state != USB_XHCI_CONTROLLER_RUNNING) {
        adapter_unlock(adapter);
        return USB_IO;
    }
    return USB_OK;
}

static struct usb_xhci_host_device *find_route(struct usb_xhci_host_adapter *adapter,
                                               const struct usb_host_route *route)
{
    struct usb_xhci_host_device *found = NULL;
    for (size_t i = 0; i < adapter->device_capacity; ++i) {
        struct usb_xhci_host_device *device = &adapter->devices[i];
        if (!device->owned || !same_route(&device->route, route)) continue;
        if (found) return NULL;
        found = device;
    }
    return found;
}

static struct usb_xhci_host_device *find_device(struct usb_xhci_host_adapter *adapter,
                                                const struct usb_host_route *route,
                                                uint8_t address)
{
    struct usb_xhci_host_device *device = find_route(adapter, route);
    if (!device) return NULL;
    if (!address) return device->slot.state == USB_XHCI_SLOT_DEFAULT ? device : NULL;
    return device->slot.address == address && device->slot.state == USB_XHCI_SLOT_ADDRESSED ?
           device : NULL;
}

static struct usb_xhci_host_device *find_parent_hub(struct usb_xhci_host_adapter *adapter,
                                                    const struct usb_host_route *route,
                                                    uint8_t depth, uint8_t address)
{
    struct usb_xhci_host_device *found = NULL;
    for (size_t i = 0; i < adapter->device_capacity; ++i) {
        struct usb_xhci_host_device *device = &adapter->devices[i];
        if (!device->owned || device->slot.address != address ||
            device->slot.state != USB_XHCI_SLOT_ADDRESSED || !device->slot.is_hub ||
            !device->slot.hub_port_count ||
            !same_route_path(&device->route, route, depth)) continue;
        if (found) return NULL;
        found = device;
    }
    return found;
}

static int convert_route(struct usb_xhci_host_adapter *adapter,
                         const struct usb_host_route *route,
                         struct usb_xhci_route *xhci_route)
{
    if (!valid_host_route(route) || !xhci_route) return USB_INVALID;
    struct usb_xhci_route converted = {0};
    converted.root_port = route->root_port;
    converted.depth = route->depth;
    for (uint8_t i = 0; i < route->depth; ++i) {
        struct usb_xhci_host_device *hub = find_parent_hub(adapter, route, i,
                                                          route->hub_address[i]);
        if (!hub) return USB_DISCONNECTED;
        converted.hub_slot[i] = hub->slot.slot_id;
        converted.hub_port[i] = route->hub_port[i];
        converted.hub_port_count[i] = hub->slot.hub_port_count;
    }
    if (route->tt_hub_address) {
        bool found = false;
        for (uint8_t i = 0; i < route->depth; ++i) {
            if (route->hub_address[i] != route->tt_hub_address) continue;
            converted.tt_hub_slot = converted.hub_slot[i];
            found = true;
            break;
        }
        if (!found) return USB_INVALID;
        converted.tt_port = route->tt_port;
        converted.multi_tt = route->multi_tt;
    }
    *xhci_route = converted;
    return USB_OK;
}

static struct usb_xhci_host_device *free_device(struct usb_xhci_host_adapter *adapter)
{
    for (size_t i = 0; i < adapter->device_capacity; ++i)
        if (!adapter->devices[i].owned &&
            adapter->devices[i].slot.state == USB_XHCI_SLOT_EMPTY)
            return &adapter->devices[i];
    return NULL;
}

static int adapter_control(void *context, const struct usb_host_route *route, uint8_t address,
                           const struct usb_setup *setup, void *buffer, size_t capacity,
                           size_t *actual, uint32_t timeout_ms)
{
    struct usb_xhci_host_adapter *adapter = context;
    int result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_device(adapter, route, address);
    result = device ? usb_xhci_device_control(&device->slot, address, setup, buffer, capacity,
                                               actual, timeout_ms) : USB_DISCONNECTED;
    adapter_unlock(adapter);
    return result;
}

static int adapter_address_device(void *context, const struct usb_host_route *route,
                                  enum usb_host_speed speed, uint8_t ep0_max_packet,
                                  uint8_t preferred_address, uint8_t *assigned_address)
{
    struct usb_xhci_host_adapter *adapter = context;
    enum usb_xhci_speed xhci_speed;
    int result = host_speed_to_xhci(speed, &xhci_speed);
    if (result) return result;
    result = adapter_lock(adapter);
    if (result) return result;
    (void)preferred_address; /* The xHC assigns the address in the output Slot Context. */
    struct usb_xhci_host_device *device = find_route(adapter, route);
    if (!device || device->slot.speed != xhci_speed ||
        device->slot.state != USB_XHCI_SLOT_DEFAULT) result = USB_DISCONNECTED;
    else result = usb_xhci_device_address(&device->slot, ep0_max_packet, assigned_address);
    adapter_unlock(adapter);
    return result;
}

static int adapter_prepare_device(void *context, const struct usb_host_route *route,
                                 enum usb_host_speed speed, bool *cleanup_required)
{
    struct usb_xhci_host_adapter *adapter = context;
    if (!cleanup_required) return USB_INVALID;
    *cleanup_required = false;
    enum usb_xhci_speed xhci_speed;
    int result = host_speed_to_xhci(speed, &xhci_speed);
    if (result) return result;
    result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_route xhci_route;
    result = convert_route(adapter, route, &xhci_route);
    if (result) { adapter_unlock(adapter); return result; }
    if (find_route(adapter, route)) { adapter_unlock(adapter); return USB_BUSY; }
    struct usb_xhci_host_device *device = free_device(adapter);
    if (!device) { adapter_unlock(adapter); return USB_NO_MEMORY; }
    device->route = *route;
    device->owned = true;
    result = usb_xhci_device_prepare(&device->slot, adapter->controller, &xhci_route,
                                     xhci_speed, cleanup_required);
    if (result && !*cleanup_required) zero_bytes(device, sizeof(*device));
    adapter_unlock(adapter);
    return result;
}

static int adapter_configure_hub(void *context, const struct usb_host_route *route,
                                 uint8_t address, enum usb_host_speed speed,
                                 uint8_t port_count, uint16_t tt_think_time_ns,
                                 bool multi_tt)
{
    struct usb_xhci_host_adapter *adapter = context;
    enum usb_xhci_speed xhci_speed;
    int result = host_speed_to_xhci(speed, &xhci_speed);
    if (result) return result;
    result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_device(adapter, route, address);
    result = device && device->slot.speed == xhci_speed ?
        usb_xhci_device_configure_hub(&device->slot, port_count,
                                      tt_think_time_ns, multi_tt) : USB_DISCONNECTED;
    adapter_unlock(adapter);
    return result;
}

static int adapter_configure_endpoint(void *context, const struct usb_host_route *route,
                                      uint8_t address, enum usb_host_speed speed,
                                      const struct usb_endpoint *endpoint)
{
    struct usb_xhci_host_adapter *adapter = context;
    enum usb_xhci_speed xhci_speed;
    int result = host_speed_to_xhci(speed, &xhci_speed);
    if (result) return result;
    result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_device(adapter, route, address);
    result = device && device->slot.speed == xhci_speed ?
        usb_xhci_device_configure_endpoint(&device->slot, endpoint) : USB_DISCONNECTED;
    adapter_unlock(adapter);
    return result;
}

static int adapter_interrupt(void *context, const struct usb_host_route *route,
                             uint8_t address, uint8_t endpoint, void *buffer,
                             size_t capacity, size_t *actual, uint32_t timeout_ms)
{
    struct usb_xhci_host_adapter *adapter = context;
    int result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_device(adapter, route, address);
    result = device ? usb_xhci_device_interrupt(&device->slot, address, endpoint,
                                                 buffer, capacity, actual, timeout_ms) :
                      USB_DISCONNECTED;
    adapter_unlock(adapter);
    return result;
}

static int adapter_bulk(void *context, const struct usb_host_route *route,
                        uint8_t address, uint8_t endpoint, void *buffer,
                        size_t length, size_t *actual, uint32_t timeout_ms)
{
    struct usb_xhci_host_adapter *adapter = context;
    int result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_device(adapter, route, address);
    result = device ? usb_xhci_device_bulk(&device->slot, address, endpoint,
                                           buffer, length, actual, timeout_ms) :
                      USB_DISCONNECTED;
    adapter_unlock(adapter);
    return result;
}

static int adapter_reset_endpoint(void *context, const struct usb_host_route *route,
                                  uint8_t address, uint8_t endpoint)
{
    struct usb_xhci_host_adapter *adapter = context;
    int result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_device(adapter, route, address);
    result = device ? usb_xhci_device_reset_endpoint(&device->slot, address, endpoint,
                                                      adapter->command_timeout_ms) :
                      USB_DISCONNECTED;
    adapter_unlock(adapter);
    return result;
}

static int adapter_disable_device(void *context, const struct usb_host_route *route,
                                  uint8_t address)
{
    struct usb_xhci_host_adapter *adapter = context;
    int result = adapter_lock(adapter);
    if (result) return result;
    struct usb_xhci_host_device *device = find_route(adapter, route);
    if (!device) result = USB_DISCONNECTED;
    else {
        /* Route plus retained record identifies uncertain/default-address slots. */
        (void)address;
        result = usb_xhci_device_disable(&device->slot, adapter->command_timeout_ms);
        if (!result) zero_bytes(device, sizeof(*device));
    }
    adapter_unlock(adapter);
    return result;
}

static uint32_t adapter_now_ms(void *context)
{
    struct usb_xhci_host_adapter *adapter = context;
    return adapter->now_ms(adapter->clock_context);
}

int usb_xhci_host_adapter_init(struct usb_xhci_host_adapter *adapter,
                               struct usb_xhci_controller *controller,
                               struct usb_xhci_host_device *devices, size_t device_capacity,
                               int (*lock)(void *), void (*unlock)(void *), void *lock_context,
                               uint32_t (*now_ms)(void *), void *clock_context,
                               uint32_t command_timeout_ms)
{
    if (!adapter || !controller || !devices || !device_capacity || !lock || !unlock ||
        !command_timeout_ms || command_timeout_ms > 10000u ||
        controller->state != USB_XHCI_CONTROLLER_RUNNING ||
        device_capacity > controller->slot_limit) return USB_INVALID;
    for (size_t i = 0; i < device_capacity; ++i)
        if (devices[i].owned || devices[i].slot.state != USB_XHCI_SLOT_EMPTY)
            return USB_BUSY;
    struct usb_xhci_host_adapter candidate = {0};
    candidate.controller = controller;
    candidate.devices = devices;
    candidate.device_capacity = device_capacity;
    candidate.lock = lock;
    candidate.unlock = unlock;
    candidate.lock_context = lock_context;
    candidate.now_ms = now_ms;
    candidate.clock_context = clock_context;
    candidate.command_timeout_ms = command_timeout_ms;
    *adapter = candidate;
    return USB_OK;
}

int usb_xhci_host_adapter_get_ops(struct usb_xhci_host_adapter *adapter,
                                  struct usb_host_ops *ops)
{
    if (!adapter || !adapter->controller || !adapter->devices || !adapter->lock ||
        !adapter->unlock || !ops) return USB_INVALID;
    struct usb_host_ops candidate = {0};
    candidate.context = adapter;
    candidate.control = adapter_control;
    candidate.address_device = adapter_address_device;
    candidate.configure_endpoint = adapter_configure_endpoint;
    candidate.interrupt = adapter_interrupt;
    candidate.bulk = adapter_bulk;
    candidate.reset_endpoint = adapter_reset_endpoint;
    candidate.disable_device = adapter_disable_device;
    candidate.prepare_device = adapter_prepare_device;
    candidate.configure_hub = adapter_configure_hub;
    candidate.now_ms = adapter->now_ms ? adapter_now_ms : NULL;
    *ops = candidate;
    return USB_OK;
}

int usb_xhci_host_adapter_reset_root_port(struct usb_xhci_host_adapter *adapter,
                                          uint8_t root_port, uint32_t timeout_ms,
                                          enum usb_host_speed *speed)
{
    if (!speed) return USB_INVALID;
    int result = adapter_lock(adapter);
    if (result) return result;
    enum usb_xhci_speed xhci_speed;
    result = usb_xhci_root_port_reset(adapter->controller, root_port, timeout_ms, &xhci_speed);
    enum usb_host_speed host_speed;
    if (!result) result = xhci_speed_to_host(xhci_speed, &host_speed);
    if (!result) *speed = host_speed;
    adapter_unlock(adapter);
    return result;
}

int usb_xhci_host_adapter_next_event(struct usb_xhci_host_adapter *adapter,
                                     struct usb_xhci_event *event)
{
    if (!event) return USB_INVALID;
    int result = adapter_acquire(adapter);
    if (result) return result;
    if (adapter->controller->state == USB_XHCI_CONTROLLER_EMPTY) {
        adapter_unlock(adapter);
        return USB_IO;
    }
    result = usb_xhci_controller_next_event(adapter->controller, event);
    adapter_unlock(adapter);
    return result;
}

int usb_xhci_host_adapter_release_stopped(struct usb_xhci_host_adapter *adapter)
{
    if (!adapter || !adapter->controller || !adapter->devices || !adapter->lock ||
        !adapter->unlock) return USB_INVALID;
    int result = adapter_acquire(adapter);
    if (result) return result;
    if (adapter->controller->state != USB_XHCI_CONTROLLER_EMPTY) {
        adapter_unlock(adapter);
        return USB_BUSY;
    }
    for (size_t i = 0; i < adapter->device_capacity; ++i) {
        struct usb_xhci_host_device *device = &adapter->devices[i];
        if (!device->owned) continue;
        result = usb_xhci_device_release_stopped(&device->slot);
        if (result) { adapter_unlock(adapter); return result; }
        zero_bytes(device, sizeof(*device));
    }
    adapter_unlock(adapter);
    return USB_OK;
}
