/* USB host enumeration and class routing through a fault-injected controller. */
#define _POSIX_C_SOURCE 200112L
#include "UsbHost.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t device_descriptor[18] = {
    18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1
};
static const uint8_t configuration_descriptor[64] = {
    9,2,64,0,3,1,0,0x80,50,
    9,4,0,0,1,3,1,1,0, 7,5,0x81,3,8,0,10,
    9,4,1,0,1,3,1,2,0, 7,5,0x82,3,4,0,5,
    9,4,2,0,2,8,6,0x50,0, 7,5,0x83,2,64,0,0, 7,5,0x03,2,64,0,0
};

struct mock_controller {
    uint8_t media[4096], cdb[16];
    enum usb_host_speed speed;
    uint32_t tag, transfer_size;
    size_t residue;
    unsigned phase, address_calls, endpoint_calls, controls, interrupts;
    unsigned bulk_calls, reset_commands, clear_features, endpoint_resets, disables;
    unsigned keyboard_reads, mouse_reads, fail_endpoint_at;
    uint8_t ep_in[4], ep_out[4];
    bool bad_ep0, stall_keyboard_protocol, bad_tag, fail_disable, fail_first_descriptor;
    bool short_first_descriptor, overlong_first_descriptor, oversize_config, short_config, disconnected;
    bool high_bandwidth;
    bool invalid_high_interval;
};
struct input_events { unsigned key_count, pointer_count; uint16_t usages[8]; bool down[8]; uint8_t buttons; int16_t x,y,wheel; };

static uint32_t get_le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void set_le32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get_be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void set_be32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

static int control(void *context, const struct usb_host_route *route, uint8_t address,
                   const struct usb_setup *setup, void *data, size_t capacity,
                   size_t *actual, uint32_t timeout_ms)
{
    struct mock_controller *m = context;
    assert(route && route->root_port == 2 && route->depth == 1 && route->hub_address[0] == 7 && route->hub_port[0] == 3);
    assert(setup && actual && timeout_ms && setup->length <= capacity && (!setup->length || data));
    ++m->controls; *actual = 0;
    if (setup->request == 6 && setup->type == 0x80) {
        uint8_t kind = (uint8_t)(setup->value >> 8);
        uint8_t index = (uint8_t)setup->value;
        if (index) return USB_STALL;
        if (kind == 1 && address == 0 && setup->length == 8) {
            if (m->fail_first_descriptor) return USB_TIMEOUT;
            memcpy(data, device_descriptor, 8); *actual = 8;
            if (m->bad_ep0) ((uint8_t *)data)[7] = 32;
            if (m->short_first_descriptor) *actual = 7;
            if (m->overlong_first_descriptor) *actual = 9;
            return USB_OK;
        }
        if (kind == 1 && address == 5 && setup->length == 18) {
            memcpy(data, device_descriptor, 18); *actual = 18; return USB_OK;
        }
        if (kind == 2 && address == 5 && setup->length == 9) {
            memcpy(data, configuration_descriptor, 9); *actual = 9;
            if (m->oversize_config) { ((uint8_t *)data)[2] = 1; ((uint8_t *)data)[3] = 8; }
            return USB_OK;
        }
        if (kind == 2 && address == 5 && setup->length == sizeof(configuration_descriptor)) {
            uint8_t bytes[sizeof(configuration_descriptor)];
            memcpy(bytes, configuration_descriptor, sizeof(configuration_descriptor));
            if (m->speed == USB_HOST_HIGH_SPEED) {
                bytes[54] = 0; bytes[55] = 2;
                bytes[61] = 0; bytes[62] = 2;
            }
            if (m->high_bandwidth) bytes[23] = 8;
            if (m->invalid_high_interval) bytes[24] = 17;
            memcpy(data, bytes, sizeof(bytes));
            *actual = sizeof(configuration_descriptor) - (m->short_config ? 1u : 0u); return USB_OK;
        }
        return USB_STALL;
    }
    if (setup->type == 0 && setup->request == 9 && address == 5 && setup->value == 1 && !setup->length)
        return USB_OK;
    if (setup->type == 0x21 && setup->request == 0x0b && address == 5 && !setup->length) {
        if (setup->index == 0 && m->stall_keyboard_protocol) return USB_STALL;
        assert(setup->index <= 1 && setup->value == 0); return USB_OK;
    }
    if (setup->type == 0x21 && setup->request == 0xff && address == 5 && setup->index == 2 && !setup->length) {
        ++m->reset_commands; m->phase = 0; return USB_OK;
    }
    if (setup->type == 0x02 && setup->request == 1 && address == 5 && !setup->length && !setup->value) {
        assert(setup->index == 0x83 || setup->index == 0x03); ++m->clear_features; return USB_OK;
    }
    assert(0 && "unexpected control request"); return USB_IO;
}

static int address_device(void *context, const struct usb_host_route *route,
                          enum usb_host_speed speed, uint8_t max_packet,
                          uint8_t preferred, uint8_t *assigned)
{
    struct mock_controller *m = context;
    assert(route->root_port == 2 && (speed == USB_HOST_HIGH_SPEED || speed == USB_HOST_FULL_SPEED) &&
           max_packet == 64 && preferred == 5);
    ++m->address_calls; m->speed = speed; *assigned = preferred; return USB_OK;
}
static int configure_endpoint(void *context, const struct usb_host_route *route,
                              uint8_t address, enum usb_host_speed speed,
                              const struct usb_endpoint *endpoint)
{
    struct mock_controller *m = context;
    assert(route && address == 5 && (speed == USB_HOST_HIGH_SPEED || speed == USB_HOST_FULL_SPEED) && endpoint);
    if (++m->endpoint_calls == m->fail_endpoint_at) return USB_TIMEOUT;
    assert(m->endpoint_calls <= 4);
    if (endpoint->address & 0x80) m->ep_in[m->endpoint_calls-1] = endpoint->address;
    else m->ep_out[m->endpoint_calls-1] = endpoint->address;
    return USB_OK;
}
static int reset_endpoint(void *context, const struct usb_host_route *route,
                          uint8_t address, uint8_t endpoint)
{
    struct mock_controller *m = context; assert(route && address == 5);
    assert(endpoint == 0x83 || endpoint == 0x03); ++m->endpoint_resets; return USB_OK;
}
static int disable_device(void *context, const struct usb_host_route *route, uint8_t address)
{
    struct mock_controller *m = context; assert(route && address == 5); ++m->disables;
    if (m->fail_disable) return USB_TIMEOUT;
    m->phase = 0; return USB_OK;
}
static int interrupt_transfer(void *context, const struct usb_host_route *route,
                              uint8_t address, uint8_t endpoint, void *data,
                              size_t capacity, size_t *actual, uint32_t timeout_ms)
{
    struct mock_controller *m = context; assert(route && address == 5 && actual && timeout_ms);
    ++m->interrupts; *actual = 0;
    if (m->disconnected) return USB_DISCONNECTED;
    if (endpoint == 0x81) {
        assert(capacity == 8);
        uint8_t pressed[8] = {0,0,4,0,0,0,0,0};
        if (!m->keyboard_reads++) { memcpy(data, pressed, sizeof(pressed)); *actual = sizeof(pressed); }
        else { memset(data, 0, sizeof(pressed)); *actual = sizeof(pressed); }
        return USB_OK;
    }
    assert(endpoint == 0x82 && capacity == 4);
    uint8_t pointer[4] = {1,2,0xff,1}; memcpy(data, pointer, sizeof(pointer)); *actual = sizeof(pointer);
    ++m->mouse_reads; return USB_OK;
}
static int bulk_transfer(void *context, const struct usb_host_route *route,
                         uint8_t address, uint8_t endpoint, void *data,
                         size_t size, size_t *actual, uint32_t timeout_ms)
{
    struct mock_controller *m = context; uint8_t *bytes = data;
    assert(route && address == 5 && actual && timeout_ms); ++m->bulk_calls; *actual = 0;
    if (!m->phase) {
        assert(endpoint == 0x03 && size == 31 && get_le32(bytes) == 0x43425355);
        m->tag = get_le32(bytes + 4); m->transfer_size = get_le32(bytes + 8);
        memcpy(m->cdb, bytes + 15, sizeof(m->cdb)); m->phase = m->transfer_size ? 1 : 2;
        *actual = size; return USB_OK;
    }
    if (m->phase == 1) {
        if (m->cdb[0] == 0x25) {
            assert(endpoint == 0x83 && size == 8); set_be32(bytes, 7); set_be32(bytes + 4, 512);
        } else if (m->cdb[0] == 0x28 || m->cdb[0] == 0x2a) {
            uint32_t lba = get_be32(m->cdb + 2); size_t offset = (size_t)(lba % 8) * 512;
            assert(size == m->transfer_size && offset <= sizeof(m->media) && size <= sizeof(m->media)-offset);
            if (m->cdb[0] == 0x28) { assert(endpoint == 0x83); memcpy(bytes, m->media + offset, size); }
            else { assert(endpoint == 0x03); memcpy(m->media + offset, bytes, size); }
        } else assert(0 && "unsupported SCSI command");
        *actual = size; m->phase = 2; return USB_OK;
    }
    assert(m->phase == 2 && endpoint == 0x83 && size == 13);
    memset(bytes, 0, size); set_le32(bytes, 0x53425355);
    set_le32(bytes + 4, m->bad_tag ? m->tag + 1 : m->tag);
    set_le32(bytes + 8, (uint32_t)m->residue); *actual = size; m->phase = 0; return USB_OK;
}

static void key_event(void *context, uint16_t usage, bool down)
{
    struct input_events *events = context; assert(events->key_count < 8);
    events->usages[events->key_count] = usage; events->down[events->key_count++] = down;
}
static void pointer_event(void *context, uint8_t buttons, int16_t x, int16_t y, int16_t wheel)
{
    struct input_events *events = context; ++events->pointer_count;
    events->buttons = buttons; events->x = x; events->y = y; events->wheel = wheel;
}
static struct usb_host_ops ops(struct mock_controller *m)
{
    return (struct usb_host_ops){m, control, address_device, configure_endpoint,
        interrupt_transfer, bulk_transfer, reset_endpoint, disable_device, NULL};
}
static struct usb_host_route route(void)
{
    struct usb_host_route value = {0}; value.root_port = 2; value.depth = 1;
    value.hub_address[0] = 7; value.hub_port[0] = 3; value.tt_hub_address = 7;
    value.tt_port = 3; value.tt_think_time_ns = 800; return value;
}
static void enumerate(struct mock_controller *m, struct usb_host_device *device)
{
    struct usb_host_ops host = ops(m); struct usb_host_route path = route();
    assert(usb_host_enumerate(device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_OK);
    assert(device->state == USB_HOST_CONFIGURED && device->address == 5 && device->configuration == 1);
}

static void test_enum_hid_bot_and_disconnect(void)
{
    struct mock_controller m = {0}; struct usb_host_device device = {0};
    enumerate(&m, &device);
    assert(m.address_calls == 1 && m.endpoint_calls == 4 && device.hid_count == 2 && device.has_bot);
    assert(device.hid[0].kind == USB_HOST_HID_KEYBOARD && device.hid[0].endpoint == 0x81);
    assert(device.hid[1].kind == USB_HOST_HID_MOUSE && device.hid[1].endpoint == 0x82);
    assert(usb_host_start_storage(&device) == USB_OK && device.bot.blocks == 8 && device.bot.block_size == 512);

    uint8_t contents[512], readback[512];
    for (size_t i=0;i<sizeof(contents);++i) contents[i]=(uint8_t)(i ^ 0xa7);
    assert(usb_bot_write(&device.bot, 2, 1, contents, sizeof(contents)) == USB_OK);
    assert(usb_bot_sync(&device.bot) == USB_OK);
    assert(usb_bot_read(&device.bot, 2, 1, readback, sizeof(readback)) == USB_OK);
    assert(!memcmp(contents, readback, sizeof(contents)));
    m.bad_tag = true;
    assert(usb_bot_read(&device.bot, 1, 1, readback, sizeof(readback)) == USB_PHASE_ERROR);
    assert(m.reset_commands == 1 && m.clear_features == 2 && m.endpoint_resets == 2);
    m.bad_tag = false;
    assert(usb_bot_read(&device.bot, 1, 1, readback, sizeof(readback)) == USB_OK);

    struct input_events events = {0};
    assert(usb_host_poll_input(&device, key_event, pointer_event, &events, 20) == USB_OK);
    assert(events.key_count == 1 && events.usages[0] == 4 && events.down[0]);
    assert(events.pointer_count == 1 && events.buttons == 1 && events.x == 2 && events.y == -1 && events.wheel == 1);
    assert(usb_host_poll_input(&device, key_event, pointer_event, &events, 20) == USB_OK);
    assert(events.key_count == 2 && events.usages[1] == 4 && !events.down[1]);
    /* Leave both inputs held, then verify failed stop retains the device and does not publish releases. */
    m.keyboard_reads = 0;
    assert(usb_host_poll_input(&device, key_event, pointer_event, &events, 20) == USB_OK);
    unsigned keys_before = events.key_count, pointers_before = events.pointer_count;
    m.fail_disable = true;
    assert(usb_host_disconnect(&device, key_event, pointer_event, &events) == USB_TIMEOUT);
    assert(device.state == USB_HOST_FAULTED && device.bot.present && events.key_count == keys_before && events.pointer_count == pointers_before);
    m.fail_disable = false;
    assert(usb_host_disconnect(&device, key_event, pointer_event, &events) == USB_OK);
    assert(device.state == USB_HOST_EMPTY && !device.bot.present && events.key_count == keys_before + 1);
    assert(!events.down[events.key_count-1] && events.buttons == 0 && events.pointer_count == pointers_before + 1);
}

static void test_optional_class_and_errors(void)
{
    struct mock_controller m = {.stall_keyboard_protocol=true};
    struct usb_host_device device = {0}; enumerate(&m, &device);
    assert(device.hid_count == 1 && device.hid[0].kind == USB_HOST_HID_MOUSE && device.has_bot);
    struct input_events events = {0};
    assert(usb_host_poll_input(&device, NULL, pointer_event, &events, 20) == USB_OK);
    assert(events.pointer_count == 1);
    assert(usb_host_disconnect(&device, NULL, pointer_event, &events) == USB_OK);

    m = (struct mock_controller){.bad_ep0=true}; device = (struct usb_host_device){0};
    struct usb_host_ops host = ops(&m); struct usb_host_route path = route();
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_PHASE_ERROR);
    assert(device.state == USB_HOST_EMPTY && !m.address_calls && !m.disables);

    m = (struct mock_controller){.short_first_descriptor=true}; device = (struct usb_host_device){0}; host = ops(&m);
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_PHASE_ERROR);
    assert(device.state == USB_HOST_EMPTY && !m.address_calls && !m.disables);

    m = (struct mock_controller){.overlong_first_descriptor=true}; device = (struct usb_host_device){0}; host = ops(&m);
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_PHASE_ERROR);
    assert(device.state == USB_HOST_EMPTY && !m.address_calls && !m.disables);

    m = (struct mock_controller){.oversize_config=true}; device = (struct usb_host_device){0}; host = ops(&m);
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_RANGE);
    assert(device.state == USB_HOST_EMPTY && m.disables == 1);

    m = (struct mock_controller){.short_config=true}; device = (struct usb_host_device){0}; host = ops(&m);
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_PHASE_ERROR);
    assert(device.state == USB_HOST_EMPTY && m.disables == 1);

    m = (struct mock_controller){.fail_endpoint_at=3}; device = (struct usb_host_device){0}; host = ops(&m);
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_TIMEOUT);
    assert(device.state == USB_HOST_EMPTY && m.disables == 1);

    m = (struct mock_controller){.fail_endpoint_at=3,.fail_disable=true}; device = (struct usb_host_device){0}; host = ops(&m);
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_TIMEOUT);
    assert(device.state == USB_HOST_FAULTED && device.address == 5);
    m.fail_disable = false;
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK && device.state == USB_HOST_EMPTY);

    m = (struct mock_controller){0}; device = (struct usb_host_device){0}; host = ops(&m);
    path.depth = USB_HOST_MAX_ROUTE_DEPTH + 1;
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_INVALID && !m.controls);
    path = route();
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_FULL_SPEED, 5) == USB_OK);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    m = (struct mock_controller){.high_bandwidth=true}; device = (struct usb_host_device){0};
    enumerate(&m, &device);
    assert(device.parsed_configuration.endpoints[0].max_packet == 8 &&
           device.parsed_configuration.endpoints[0].transactions == 2);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    m = (struct mock_controller){.high_bandwidth=true}; device = (struct usb_host_device){0}; host = ops(&m);
    path = route();
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_FULL_SPEED, 5) == USB_UNSUPPORTED);
    assert(device.state == USB_HOST_EMPTY && m.disables == 1 && !m.endpoint_calls);

    m = (struct mock_controller){.invalid_high_interval=true}; device = (struct usb_host_device){0}; host = ops(&m);
    path = route();
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_UNSUPPORTED);
    assert(device.state == USB_HOST_EMPTY && m.disables == 1 && !m.endpoint_calls);

    m = (struct mock_controller){0}; device = (struct usb_host_device){0};
    events = (struct input_events){0}; enumerate(&m, &device);
    assert(usb_host_poll_input(&device, key_event, pointer_event, &events, 10) == USB_OK);
    m.disconnected = true;
    assert(usb_host_poll_input(&device, key_event, pointer_event, &events, 10) == USB_DISCONNECTED);
    assert(device.state == USB_HOST_EMPTY && events.key_count == 2 && !events.down[1]);
}

int main(void)
{
    test_enum_hid_bot_and_disconnect();
    test_optional_class_and_errors();
    puts("PASS: USB production host enumeration/HID/BOT class routing, cleanup retention and fault cases");
    return 0;
}
