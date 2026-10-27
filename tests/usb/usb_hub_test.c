/* USB 2 external hub control, power, reset, status bitmap and TT route model. */
#include "UsbHost.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t device_descriptor[18] = {
    18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1
};
static const uint8_t configuration_single_tt[25] = {
    9,2,25,0,1,1,0,0x80,50,
    9,4,0,0,1,9,0,0,0,
    7,5,0x81,3,16,0,12
};
static const uint8_t configuration_multi_tt[41] = {
    9,2,41,0,1,1,0,0x80,50,
    9,4,0,0,1,9,0,1,0,
    7,5,0x81,3,16,0,12,
    9,4,0,1,1,9,0,2,0,
    7,5,0x81,3,16,0,12
};

struct mock_controller {
    uint8_t descriptor[39];
    uint8_t descriptor_length, port_count;
    uint16_t port_status[USB_HOST_HUB_MAX_PORTS];
    uint16_t port_change[USB_HOST_HUB_MAX_PORTS];
    uint16_t hub_status, hub_change;
    unsigned route_depth, controls, configured, interrupts;
    unsigned power_on, power_off, resets, clears, disables;
    unsigned fail_power_on_at, fail_power_off_at;
    unsigned events[3], attach_attempts, fail_attach_count;
    uint32_t current_ms;
    uint32_t power_request_advance_ms, reset_request_advance_ms;
    uint8_t device_protocol, interface_protocol;
    enum usb_host_speed device_speed, event_speed;
    uint16_t reset_speed_bits;
    uint8_t bitmap[USB_HOST_HUB_MAX_BITMAP_BYTES];
    uint32_t reset_complete_at;
    bool multi_tt, bad_descriptor, short_bitmap, overlong_bitmap;
    bool reset_stuck, fail_detach_once, fail_overcurrent_once;
    bool fail_power_on_applied;
};

static void put_le16(uint8_t *p, uint16_t value)
{ p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }

static int control(void *context, const struct usb_host_route *route, uint8_t address,
                   const struct usb_setup *setup, void *data, size_t capacity,
                   size_t *actual, uint32_t timeout_ms)
{
    struct mock_controller *m = context;
    assert(route && route->root_port == 2 && route->depth == m->route_depth);
    assert(setup && actual && timeout_ms && setup->length <= capacity);
    assert(!setup->length || data);
    ++m->controls;
    *actual = 0;
    if (setup->request == 6 && setup->type == 0x80) {
        uint8_t kind = (uint8_t)(setup->value >> 8);
        if (kind == 1 && address == 0 && setup->length == 8) {
            memcpy(data, device_descriptor, 8); *actual = 8; return USB_OK;
        }
        if (kind == 1 && address == 7 && setup->length == sizeof(device_descriptor)) {
            uint8_t bytes[sizeof(device_descriptor)];
            memcpy(bytes, device_descriptor, sizeof(bytes));
            bytes[4] = 9; bytes[6] = m->device_protocol;
            memcpy(data, bytes, sizeof(bytes));
            *actual = sizeof(device_descriptor); return USB_OK;
        }
        if (kind == 2 && address == 7 && setup->length == 9) {
            const uint8_t *bytes = m->multi_tt ? configuration_multi_tt : configuration_single_tt;
            memcpy(data, bytes, 9); *actual = 9; return USB_OK;
        }
        size_t config_length = m->multi_tt ? sizeof(configuration_multi_tt) : sizeof(configuration_single_tt);
        if (kind == 2 && address == 7 && setup->length == config_length) {
            uint8_t bytes[sizeof(configuration_multi_tt)];
            const uint8_t *source = m->multi_tt ? configuration_multi_tt : configuration_single_tt;
            memcpy(bytes, source, config_length);
            bytes[16] = m->interface_protocol;
            memcpy(data, bytes, config_length);
            *actual = config_length; return USB_OK;
        }
        return USB_STALL;
    }
    if (setup->type == 0 && setup->request == 9 && address == 7 && !setup->length)
        return setup->value == 1 ? USB_OK : USB_INVALID;
    if (setup->type == 0x01 && setup->request == 11 && address == 7 &&
        setup->value == 1 && setup->index == 0 && !setup->length) return USB_OK;
    if (setup->type == 0xa0 && setup->request == 6 && address == 7 &&
        setup->value == 0x2900 && setup->index == 0) {
        if (setup->length == 7) {
            memcpy(data, m->descriptor, 7); *actual = 7;
            if (m->bad_descriptor) ((uint8_t *)data)[0] = 10;
            return USB_OK;
        }
        if (setup->length == m->descriptor_length) {
            memcpy(data, m->descriptor, m->descriptor_length);
            *actual = m->descriptor_length; return USB_OK;
        }
        return USB_STALL;
    }
    if (setup->type == 0xa3 && setup->request == 0 && address == 7 &&
        setup->length == 4 && setup->index > 0 && setup->index <= m->port_count) {
        uint8_t port = (uint8_t)(setup->index - 1u);
        if (m->reset_complete_at && m->current_ms >= m->reset_complete_at &&
            (m->port_status[port] & 0x0010u)) {
            m->port_status[port] &= (uint16_t)~0x0610u;
            m->port_status[port] |= (uint16_t)(0x0003u | m->reset_speed_bits);
            m->port_change[port] |= 0x0010u;
            m->reset_complete_at = 0;
        }
        put_le16(data, m->port_status[port]);
        put_le16((uint8_t *)data + 2, m->port_change[port]);
        *actual = 4; return USB_OK;
    }
    if (setup->type == 0xa0 && setup->request == 0 && address == 7 &&
        setup->length == 4 && setup->index == 0) {
        put_le16(data, m->hub_status); put_le16((uint8_t *)data + 2, m->hub_change);
        *actual = 4; return USB_OK;
    }
    if ((setup->type == 0x23 || setup->type == 0x20) && address == 7 && !setup->length) {
        bool set = setup->request == 3;
        bool clear = setup->request == 1;
        if (!set && !clear) return USB_STALL;
        if (setup->type == 0x20 && setup->index == 0 && clear && setup->value <= 1) {
            m->hub_change &= (uint16_t)~(1u << setup->value);
            ++m->clears; return USB_OK;
        }
        if (setup->type != 0x23) return USB_STALL;
        if (!setup->index || setup->index > m->port_count) return USB_INVALID;
        uint8_t port = (uint8_t)(setup->index - 1u);
        if (setup->value == 8 && set) {
            ++m->power_on;
            if (m->fail_power_on_at == m->power_on) {
                if (m->fail_power_on_applied) m->port_status[port] |= 0x0100u;
                return USB_TIMEOUT;
            }
            m->port_status[port] |= 0x0100u;
            m->current_ms += m->power_request_advance_ms;
            return USB_OK;
        }
        if (setup->value == 8 && clear) {
            ++m->power_off;
            if (m->fail_power_off_at == m->power_off) return USB_TIMEOUT;
            m->port_status[port] &= (uint16_t)~0x0100u; return USB_OK;
        }
        if (setup->value == 4 && set) {
            ++m->resets;
            if (m->reset_stuck || m->reset_complete_at) m->port_status[port] |= 0x0010u;
            else {
                m->port_status[port] &= (uint16_t)~0x0610u;
                m->port_status[port] |= (uint16_t)(0x0003u | m->reset_speed_bits);
                m->port_change[port] |= 0x0010u;
            }
            m->current_ms += m->reset_request_advance_ms;
            return USB_OK;
        }
        if (clear && setup->value >= 16 && setup->value <= 20) {
            m->port_change[port] &= (uint16_t)~(1u << (setup->value - 16u));
            ++m->clears; return USB_OK;
        }
    }
    assert(0 && "unexpected hub control request");
    return USB_IO;
}

static int address_device(void *context, const struct usb_host_route *route,
                          enum usb_host_speed speed, uint8_t max_packet,
                          uint8_t preferred, uint8_t *assigned)
{
    struct mock_controller *m = context;
    assert(route && route->depth == m->route_depth && route->root_port == 2);
    assert(speed == m->device_speed && max_packet == 64 && preferred == 7);
    *assigned = preferred; return USB_OK;
}

static int configure_endpoint(void *context, const struct usb_host_route *route,
                              uint8_t address, enum usb_host_speed speed,
                              const struct usb_endpoint *endpoint)
{
    struct mock_controller *m = context;
    assert(route && route->depth == m->route_depth && address == 7);
    assert(speed == m->device_speed && endpoint->address == 0x81 && endpoint->type == 3);
    ++m->configured; return USB_OK;
}

static int interrupt_transfer(void *context, const struct usb_host_route *route,
                              uint8_t address, uint8_t endpoint, void *data,
                              size_t capacity, size_t *actual, uint32_t timeout_ms)
{
    struct mock_controller *m = context;
    assert(route && route->depth == m->route_depth && address == 7);
    assert(endpoint == 0x81 && capacity == (m->port_count + 8u) / 8u && actual && timeout_ms);
    ++m->interrupts; *actual = 0;
    if (m->short_bitmap) return USB_OK;
    if (m->overlong_bitmap) { *actual = capacity + 1u; return USB_OK; }
    memcpy(data, m->bitmap, capacity); *actual = capacity;
    memset(m->bitmap, 0, sizeof(m->bitmap)); return USB_OK;
}

static int bulk_transfer(void *context, const struct usb_host_route *route,
                         uint8_t address, uint8_t endpoint, void *data,
                         size_t size, size_t *actual, uint32_t timeout_ms)
{
    (void)context; (void)route; (void)address; (void)endpoint;
    (void)data; (void)size; (void)timeout_ms;
    *actual = 0; return USB_UNSUPPORTED;
}

static int disable_device(void *context, const struct usb_host_route *route, uint8_t address)
{
    struct mock_controller *m = context;
    assert(route && route->depth == m->route_depth && address == 7);
    ++m->disables; return USB_OK;
}

static uint32_t clock_ms(void *context)
{ return ((struct mock_controller *)context)->current_ms; }

static int on_event(void *context, enum usb_host_hub_event_kind kind, uint8_t port,
                    const struct usb_host_route *route, enum usb_host_speed speed)
{
    struct mock_controller *m = context;
    assert(route && route->root_port == 2 && route->depth == m->route_depth + 1);
    assert(route->hub_address[m->route_depth] == 7 && route->hub_port[m->route_depth] == port);
    ++m->events[kind];
    if (kind == USB_HOST_HUB_ATTACH) {
        ++m->attach_attempts;
        if (m->fail_attach_count) { --m->fail_attach_count; return USB_TIMEOUT; }
        assert(speed == m->event_speed);
    }
    if (kind == USB_HOST_HUB_DETACH && m->fail_detach_once) {
        m->fail_detach_once = false; return USB_TIMEOUT;
    }
    if (kind == USB_HOST_HUB_OVERCURRENT && m->fail_overcurrent_once) {
        m->fail_overcurrent_once = false; return USB_TIMEOUT;
    }
    return USB_OK;
}

static struct usb_host_ops make_ops(struct mock_controller *m)
{
    return (struct usb_host_ops){m, control, address_device, configure_endpoint,
        interrupt_transfer, bulk_transfer, NULL, disable_device, clock_ms, NULL};
}

static struct usb_host_route make_route(unsigned depth)
{
    struct usb_host_route route = {0}; route.root_port = 2;
    route.depth = (uint8_t)depth;
    for (uint8_t i = 0; i < route.depth; ++i) {
        route.hub_address[i] = (uint8_t)(i + 1u);
        route.hub_port[i] = (uint8_t)(i + 1u);
    }
    return route;
}

static void set_port_count(struct mock_controller *m, uint8_t port_count)
{
    m->port_count = port_count;
    uint8_t bitmap_bytes = (uint8_t)((port_count + 8u) / 8u);
    m->descriptor_length = (uint8_t)(7u + 2u * bitmap_bytes);
    memset(m->descriptor, 0, sizeof(m->descriptor));
    m->descriptor[0] = m->descriptor_length; m->descriptor[1] = 0x29;
    m->descriptor[2] = port_count;
    m->descriptor[3] = 0x21; /* Individual power switching, 16-bit-time TT think time. */
    m->descriptor[5] = 10;
    memset(m->descriptor + 7u + bitmap_bytes, 0xff, bitmap_bytes);
}

static void init_mock(struct mock_controller *m)
{
    memset(m, 0, sizeof(*m));
    m->multi_tt = true; m->device_protocol = 2; m->interface_protocol = 1;
    m->device_speed = USB_HOST_HIGH_SPEED; m->event_speed = USB_HOST_FULL_SPEED;
    set_port_count(m, 4);
}

static void set_port_bitmap(struct mock_controller *m, uint8_t port)
{ m->bitmap[port / 8u] |= (uint8_t)(1u << (port % 8u)); }

static int start_hub_at(struct mock_controller *m, struct usb_host_device *device,
                        struct usb_host_hub *hub, uint32_t now_ms)
{
    m->current_ms = now_ms;
    return usb_host_hub_start(device, hub);
}

static int poll_hub_at(struct mock_controller *m, struct usb_host_device *device,
                       struct usb_host_hub *hub, uint32_t now_ms,
                       usb_host_hub_event callback)
{
    m->current_ms = now_ms;
    return usb_host_hub_poll(device, hub, 10, callback, m);
}

static void enumerate_hub_at_speed(struct mock_controller *m, struct usb_host_device *device,
                                   unsigned depth, enum usb_host_speed speed)
{
    m->route_depth = depth;
    struct usb_host_ops host = make_ops(m);
    struct usb_host_route route = make_route(depth);
    assert(usb_host_enumerate(device, &host, &route, speed, 7) == USB_OK);
    assert(device->state == USB_HOST_CONFIGURED && device->hub_interface.present);
    assert(device->hub_interface.endpoint == 0x81 && m->configured == 1);
}

static void enumerate_hub(struct mock_controller *m, struct usb_host_device *device,
                          unsigned depth)
{ enumerate_hub_at_speed(m, device, depth, USB_HOST_HIGH_SPEED); }

static void test_connected_reset_tt_disconnect_and_overcurrent(void)
{
    struct mock_controller m; init_mock(&m);
    struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    enumerate_hub(&m, &device, 0);
    assert(start_hub_at(&m, &device, &hub, 100) == USB_OK);
    assert(hub.initialized && hub.port_count == 4 && hub.multi_tt &&
           hub.tt_think_time_ns == 1332 && hub.power_good_ms == 20 && m.power_on == 4);
    m.port_status[0] = 0x0001u;
    m.port_change[0] = 0x0001u;
    assert(poll_hub_at(&m, &device, &hub, 119, on_event) == USB_AGAIN);
    assert(!m.interrupts && !m.resets);
    assert(poll_hub_at(&m, &device, &hub, 120, on_event) == USB_AGAIN);
    assert(m.resets == 1 && m.clears >= 1 && m.events[USB_HOST_HUB_ATTACH] == 0);
    m.fail_attach_count = 2;
    assert(poll_hub_at(&m, &device, &hub, 169, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 170, on_event) == USB_TIMEOUT);
    assert(m.resets == 1 && m.attach_attempts == 1 && !hub.ports[0].attached);
    assert(poll_hub_at(&m, &device, &hub, 1121, on_event) == USB_TIMEOUT);
    assert(m.resets == 1 && m.attach_attempts == 2 && !hub.ports[0].attached);
    assert(poll_hub_at(&m, &device, &hub, 1122, on_event) == USB_OK);
    assert(m.attach_attempts == 3 && m.events[USB_HOST_HUB_ATTACH] == 3 && hub.ports[0].attached);
    assert(hub.ports[0].speed == USB_HOST_FULL_SPEED);
    struct usb_host_route child = {0};
    assert(usb_host_hub_child_route(&device, &hub, 1, USB_HOST_FULL_SPEED, &child) == USB_OK);
    assert(child.depth == 1 && child.hub_address[0] == 7 && child.hub_port[0] == 1);
    assert(child.tt_hub_address == 7 && child.tt_port == 1 &&
           child.tt_think_time_ns == 1332 && child.multi_tt);

    m.port_status[0] = 0x0100u;
    m.port_change[0] = 0x0001u; set_port_bitmap(&m, 1);
    m.fail_detach_once = true;
    assert(poll_hub_at(&m, &device, &hub, 172, on_event) == USB_TIMEOUT);
    assert(hub.ports[0].attached && m.events[USB_HOST_HUB_DETACH] == 1);
    set_port_bitmap(&m, 1);
    assert(poll_hub_at(&m, &device, &hub, 173, on_event) == USB_OK);
    assert(!hub.ports[0].attached && m.events[USB_HOST_HUB_DETACH] == 2);

    m.port_status[1] = 0x0108u; m.port_change[1] = 0x0008u; set_port_bitmap(&m, 2);
    m.fail_overcurrent_once = true;
    assert(poll_hub_at(&m, &device, &hub, 174, on_event) == USB_TIMEOUT);
    assert(!hub.ports[1].overcurrent_latched && hub.ports[1].powered && !m.power_off);
    set_port_bitmap(&m, 2);
    assert(poll_hub_at(&m, &device, &hub, 175, on_event) == USB_IO);
    assert(m.events[USB_HOST_HUB_OVERCURRENT] == 2 && m.power_off == 1);
    assert(hub.ports[1].overcurrent_latched && !hub.ports[1].powered);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(!hub.initialized && m.power_off == 4);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_malformed_descriptor_short_bitmap_global_fault_and_depth(void)
{
    struct mock_controller m; struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    init_mock(&m); m.bad_descriptor = true; enumerate_hub(&m, &device, 0);
    assert(start_hub_at(&m, &device, &hub, 0) == USB_PHASE_ERROR);
    assert(!hub.initialized && !m.power_on);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m);
    device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    m.descriptor[3] |= 0x80u; /* D7 advertises port indicators; it is defined, not reserved. */
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    assert(poll_hub_at(&m, &device, &hub, 20, on_event) == USB_OK);
    m.short_bitmap = true;
    assert(poll_hub_at(&m, &device, &hub, 21, on_event) == USB_PHASE_ERROR);
    m.short_bitmap = false; m.overlong_bitmap = true;
    assert(poll_hub_at(&m, &device, &hub, 22, on_event) == USB_PHASE_ERROR);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m);
    device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    m.hub_status = 2; m.hub_change = 2; m.bitmap[0] = 1;
    assert(poll_hub_at(&m, &device, &hub, 20, on_event) == USB_IO);
    assert(hub.faulted && poll_hub_at(&m, &device, &hub, 21, on_event) == USB_IO);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, USB_HOST_MAX_ROUTE_DEPTH);
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    struct usb_host_route output = {0}; output.root_port = 99;
    assert(usb_host_hub_child_route(&device, &hub, 1, USB_HOST_FULL_SPEED, &output) == USB_RANGE);
    assert(output.root_port == 99);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); m.multi_tt = false; m.device_protocol = 0; m.interface_protocol = 0;
    m.device_speed = USB_HOST_FULL_SPEED; m.event_speed = USB_HOST_LOW_SPEED;
    m.reset_speed_bits = 0x0200u;
    device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub_at_speed(&m, &device, 0, USB_HOST_FULL_SPEED);
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    m.port_status[0] = 0x0001u; m.port_change[0] = 1;
    assert(poll_hub_at(&m, &device, &hub, 20, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 70, on_event) == USB_OK);
    assert(hub.ports[0].attached && hub.ports[0].speed == USB_HOST_LOW_SPEED);
    struct usb_host_route child = {0};
    assert(usb_host_hub_child_route(&device, &hub, 1, USB_HOST_LOW_SPEED, &child) == USB_OK);
    assert(!child.tt_hub_address && !child.tt_port && !child.multi_tt);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_bitmap_edges_max_ports_and_clock_wrap(void)
{
    struct mock_controller m; init_mock(&m); set_port_count(&m, 9);
    struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    enumerate_hub(&m, &device, 0);
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    assert(hub.port_count == 9 && hub.bitmap_bytes == 2);
    m.port_status[7] = 0x0001u; m.port_change[7] = 1;
    m.port_status[8] = 0x0001u; m.port_change[8] = 1;
    assert(poll_hub_at(&m, &device, &hub, 20, on_event) == USB_AGAIN);
    assert(m.resets == 2);
    assert(poll_hub_at(&m, &device, &hub, 70, on_event) == USB_OK);
    assert(m.events[USB_HOST_HUB_ATTACH] == 2 &&
           hub.ports[7].attached && hub.ports[8].attached);
    m.port_status[8] = 0x0100u; m.port_change[8] = 1; set_port_bitmap(&m, 9);
    assert(poll_hub_at(&m, &device, &hub, 71, on_event) == USB_OK);
    assert(!hub.ports[8].attached && m.events[USB_HOST_HUB_DETACH] == 1);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(m.events[USB_HOST_HUB_DETACH] == 2);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); set_port_count(&m, USB_HOST_HUB_MAX_PORTS);
    device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    m.descriptor[3] = 0x22; m.descriptor[5] = 0;
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    assert(hub.port_count == 127 && hub.bitmap_bytes == 16 && m.descriptor_length == 39);
    m.port_status[126] = 0x0001u; m.port_change[126] = 1;
    assert(poll_hub_at(&m, &device, &hub, 0, on_event) == USB_AGAIN);
    assert(m.resets == 1);
    assert(poll_hub_at(&m, &device, &hub, 50, on_event) == USB_OK);
    assert(hub.ports[126].attached && m.events[USB_HOST_HUB_ATTACH] == 1);
    m.port_status[126] = 0; m.port_change[126] = 1; set_port_bitmap(&m, 127);
    assert(poll_hub_at(&m, &device, &hub, 51, on_event) == USB_OK);
    assert(!hub.ports[126].attached && m.events[USB_HOST_HUB_DETACH] == 1);
    struct usb_host_route child = {0};
    assert(usb_host_hub_child_route(&device, &hub, 127, USB_HOST_LOW_SPEED, &child) == USB_OK);
    assert(child.hub_port[0] == 127 && child.tt_port == 127);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    assert(start_hub_at(&m, &device, &hub, UINT32_C(0xfffffff5)) == USB_OK);
    assert(poll_hub_at(&m, &device, &hub, 4, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 9, on_event) == USB_OK);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_power_failure_retains_cleanup_state(void)
{
    struct mock_controller m; init_mock(&m);
    struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    enumerate_hub(&m, &device, 0);
    m.fail_power_on_at = 2; m.fail_power_on_applied = true;
    assert(start_hub_at(&m, &device, &hub, 0) == USB_TIMEOUT);
    assert(hub.initialized && hub.faulted && hub.ports[0].powered && hub.ports[1].powered);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(!hub.initialized && m.power_off == 2);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    m.fail_power_off_at = 1;
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_TIMEOUT);
    assert(hub.initialized && hub.ports[0].powered);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(!hub.initialized && m.power_off == 5);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_attach_retry_cleanup_on_unplug(void)
{
    struct mock_controller m; init_mock(&m);
    struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    enumerate_hub(&m, &device, 0);
    m.descriptor[3] = 0x22; m.descriptor[5] = 0;
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    m.port_status[0] = 0x0101u; m.port_change[0] = 1;
    assert(poll_hub_at(&m, &device, &hub, 0, on_event) == USB_AGAIN);
    m.fail_attach_count = 2;
    assert(poll_hub_at(&m, &device, &hub, 50, on_event) == USB_TIMEOUT);
    assert(hub.ports[0].attach_pending && !hub.ports[0].attached);

    m.port_status[0] = 0x0100u; m.port_change[0] = 1; set_port_bitmap(&m, 1);
    assert(poll_hub_at(&m, &device, &hub, 51, on_event) == USB_OK);
    assert(!hub.ports[0].attach_pending && !hub.ports[0].attached);
    assert(m.events[USB_HOST_HUB_ATTACH] == 1 && m.events[USB_HOST_HUB_DETACH] == 1);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_control_request_clock_accounting_and_restart(void)
{
    struct mock_controller m; init_mock(&m);
    struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    enumerate_hub(&m, &device, 0);
    m.power_request_advance_ms = 5;
    m.reset_request_advance_ms = 100;
    m.port_status[0] = 0x0001u; m.port_change[0] = 1;
    assert(start_hub_at(&m, &device, &hub, 100) == USB_OK);
    assert(m.current_ms == 120 && hub.power_ready_ms == 140);
    assert(poll_hub_at(&m, &device, &hub, 139, on_event) == USB_AGAIN);
    assert(!m.interrupts && !m.resets);
    assert(poll_hub_at(&m, &device, &hub, 140, on_event) == USB_AGAIN);
    assert(m.current_ms == 240 && m.resets == 1);
    assert(hub.ports[0].reset_ready_ms == 290 && hub.ports[0].reset_deadline_ms == 1240);
    assert(poll_hub_at(&m, &device, &hub, 289, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 290, on_event) == USB_OK);
    assert(hub.ports[0].attached && m.events[USB_HOST_HUB_ATTACH] == 1);

    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(!hub.initialized && m.events[USB_HOST_HUB_DETACH] == 1);
    m.port_status[0] = 0; m.port_change[0] = 0;
    assert(start_hub_at(&m, &device, &hub, 300) == USB_OK);
    assert(hub.initialized && hub.power_ready_ms == 340);
    assert(poll_hub_at(&m, &device, &hub, 340, on_event) == USB_OK);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_reset_timeout_and_single_tt_route(void)
{
    struct mock_controller m; init_mock(&m);
    struct usb_host_device device = {0}; struct usb_host_hub hub = {0};
    enumerate_hub(&m, &device, 0);
    /* The descriptor's power mode is no-switching, so startup is immediately ready. */
    m.descriptor[3] = 0x22; m.descriptor[5] = 0;
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    m.port_status[0] = 0x0001u; m.port_change[0] = 1; m.reset_stuck = true;
    assert(poll_hub_at(&m, &device, &hub, 0, on_event) == USB_AGAIN);
    assert(m.resets == 1);
    assert(poll_hub_at(&m, &device, &hub, 50, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 1000, on_event) == USB_TIMEOUT);
    assert(hub.ports[0].reset_failed && m.resets == 1 && !m.events[USB_HOST_HUB_ATTACH]);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    m.descriptor[3] = 0x22; m.descriptor[5] = 0;
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    m.port_status[0] = 0x0001u; m.port_change[0] = 1; m.reset_stuck = true;
    m.reset_complete_at = 1001;
    assert(poll_hub_at(&m, &device, &hub, 0, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 50, on_event) == USB_AGAIN);
    assert(poll_hub_at(&m, &device, &hub, 1001, on_event) == USB_TIMEOUT);
    assert(hub.ports[0].reset_failed && !m.events[USB_HOST_HUB_ATTACH]);
    m.reset_stuck = false; m.port_status[0] = 0x0101u; m.port_change[0] = 1;
    set_port_bitmap(&m, 1);
    assert(poll_hub_at(&m, &device, &hub, 1002, on_event) == USB_AGAIN);
    assert(m.resets == 2 && !hub.ports[0].reset_failed);
    assert(poll_hub_at(&m, &device, &hub, 1052, on_event) == USB_OK);
    assert(hub.ports[0].attached && m.events[USB_HOST_HUB_ATTACH] == 1);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    init_mock(&m); m.multi_tt = false; m.device_protocol = 1; m.interface_protocol = 0;
    device = (struct usb_host_device){0}; hub = (struct usb_host_hub){0};
    enumerate_hub(&m, &device, 0);
    m.descriptor[3] = 0x01; /* Single-TT hub, 8-bit-time think time. */
    assert(start_hub_at(&m, &device, &hub, 0) == USB_OK);
    struct usb_host_route child = {0};
    assert(usb_host_hub_child_route(&device, &hub, 3, USB_HOST_LOW_SPEED, &child) == USB_OK);
    assert(child.tt_hub_address == 7 && child.tt_port == 3 &&
           child.tt_think_time_ns == 666 && !child.multi_tt);
    assert(usb_host_hub_stop(&device, &hub, on_event, &m) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

int main(void)
{
    test_connected_reset_tt_disconnect_and_overcurrent();
    test_malformed_descriptor_short_bitmap_global_fault_and_depth();
    test_bitmap_edges_max_ports_and_clock_wrap();
    test_power_failure_retains_cleanup_state();
    test_attach_retry_cleanup_on_unplug();
    test_control_request_clock_accounting_and_restart();
    test_reset_timeout_and_single_tt_route();
    puts("PASS: USB production hub descriptor, power, bitmap, reset, fault and TT routing");
    return 0;
}
