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
    unsigned operation_order, reset_order, tt_clear_order, clear_feature_order, tt_clear_calls;
    uint16_t tt_clear_value, tt_clear_index;
    uint8_t tt_clear_hub_address, tt_clear_route_depth;
    unsigned keyboard_reads, mouse_reads, fail_endpoint_at;
    uint8_t ep_in[4], ep_out[4];
    bool bad_ep0, stall_keyboard_protocol, bad_tag, fail_disable, fail_first_descriptor;
    bool short_first_descriptor, overlong_first_descriptor, oversize_config, short_config, disconnected;
    bool high_bandwidth;
    bool invalid_high_interval;
    bool fail_keyboard_protocol, clear_disable_on_endpoint;
    size_t raw_length;
    int raw_error;
    bool keyboard_reserved, mouse_reserved;
    bool invalid_assignment, slot_owned, slot_live, applied_disable_error;
    bool prepared_backend, address_assigned, fail_address, address_applied_error, prepare_bad_success;
    unsigned prepare_failure, prepare_calls;
    uint8_t returned_address;
    struct usb_host_device *reenter_device;
    int reenter_stop, reenter_release, reenter_disconnect;
    int reenter_read;
    size_t reenter_actual;
};
struct input_events {
    unsigned key_count, pointer_count; uint16_t usages[8]; bool down[8];
    uint8_t buttons; int16_t x,y,wheel;
    struct usb_host_device *reenter_device;
    int reenter_stop, reenter_release, reenter_disconnect;
};
struct test_lock { bool held; };

static int block_lock(void *context)
{
    struct test_lock *lock = context;
    assert(lock && !lock->held); lock->held = true; return USB_OK;
}
static void block_unlock(void *context)
{
    struct test_lock *lock = context;
    assert(lock && lock->held); lock->held = false;
}

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
    assert(setup && actual && timeout_ms && setup->length <= capacity && (!setup->length || data));
    ++m->controls; *actual = 0;
    if (setup->type == 0x23 && setup->request == 8 && !setup->length) {
        assert(route && route->root_port == 2 && route->depth == 0 && address == 7);
        ++m->tt_clear_calls; m->tt_clear_value = setup->value;
        m->tt_clear_index = setup->index; m->tt_clear_hub_address = address;
        m->tt_clear_route_depth = route->depth;
        m->tt_clear_order = ++m->operation_order;
        return USB_OK;
    }
    assert(route && route->root_port == 2 && route->depth == 1 &&
           route->hub_address[0] == 7 && route->hub_port[0] == 3);
    if (setup->request == 6 && setup->type == 0x80) {
        uint8_t kind = (uint8_t)(setup->value >> 8);
        uint8_t index = (uint8_t)setup->value;
        if (index) return USB_STALL;
        if (kind == 1 && address == 0 && setup->length == 8) {
            if (m->prepared_backend) assert(m->slot_owned && m->slot_live);
            if (m->prepared_backend && m->reenter_device) {
                assert(usb_host_stop_device(m->reenter_device) == USB_BUSY);
                assert(usb_host_disconnect(m->reenter_device, NULL, NULL, NULL) == USB_BUSY);
            }
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
        if (setup->index == 0 && m->fail_keyboard_protocol) return USB_TIMEOUT;
        if (setup->index == 0 && m->stall_keyboard_protocol) return USB_STALL;
        assert(setup->index <= 1 && setup->value == 0); return USB_OK;
    }
    if (setup->type == 0x21 && setup->request == 0xff && address == 5 && setup->index == 2 && !setup->length) {
        ++m->reset_commands; m->phase = 0; return USB_OK;
    }
    if (setup->type == 0x02 && setup->request == 1 && address == 5 && !setup->length && !setup->value) {
        assert(setup->index == 0x83 || setup->index == 0x03); ++m->clear_features;
        m->clear_feature_order = ++m->operation_order; return USB_OK;
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
    ++m->address_calls; m->speed = speed;
    if (m->prepared_backend) assert(m->slot_owned && m->slot_live);
    if (m->prepared_backend && m->reenter_device) {
        assert(usb_host_stop_device(m->reenter_device) == USB_BUSY);
        assert(usb_host_release_stopped_device(m->reenter_device, NULL, NULL, NULL) == USB_BUSY);
    }
    if (m->fail_address) {
        if (m->address_applied_error) m->address_assigned = true;
        return USB_TIMEOUT;
    }
    m->slot_owned = m->slot_live = true;
    m->address_assigned = true;
    *assigned = m->invalid_assignment ? m->returned_address : preferred;
    return USB_OK;
}
static int configure_endpoint(void *context, const struct usb_host_route *route,
                              uint8_t address, enum usb_host_speed speed,
                              const struct usb_endpoint *endpoint)
{
    struct mock_controller *m = context;
    assert(route && address == 5 && (speed == USB_HOST_HIGH_SPEED || speed == USB_HOST_FULL_SPEED) && endpoint);
    if (++m->endpoint_calls == m->fail_endpoint_at) {
        if (m->clear_disable_on_endpoint) {
            assert(m->reenter_device);
            m->reenter_device->ops.disable_device = NULL;
        }
        return USB_TIMEOUT;
    }
    assert(m->endpoint_calls <= 4);
    if (endpoint->address & 0x80) m->ep_in[m->endpoint_calls-1] = endpoint->address;
    else m->ep_out[m->endpoint_calls-1] = endpoint->address;
    return USB_OK;
}
static int reset_endpoint(void *context, const struct usb_host_route *route,
                          uint8_t address, uint8_t endpoint)
{
    struct mock_controller *m = context; assert(route && address == 5);
    assert(endpoint == 0x83 || endpoint == 0x03); ++m->endpoint_resets;
    m->reset_order = ++m->operation_order; return USB_OK;
}
static int disable_device(void *context, const struct usb_host_route *route, uint8_t address)
{
    struct mock_controller *m = context;
    uint8_t expected = m->prepared_backend && (!m->address_assigned || m->fail_address) ? 0 :
        (m->invalid_assignment ? m->returned_address : 5);
    assert(route && m->slot_owned && address == expected);
    ++m->disables;
    if (m->reenter_device) {
        m->reenter_stop = usb_host_stop_device(m->reenter_device);
        m->reenter_release = usb_host_release_stopped_device(m->reenter_device, NULL, NULL, NULL);
        m->reenter_disconnect = usb_host_disconnect(m->reenter_device, NULL, NULL, NULL);
        uint8_t report[8]; m->reenter_actual = 99;
        m->reenter_read = usb_host_read_boot_report(m->reenter_device, 0, report,
                                                    sizeof(report), &m->reenter_actual, 1);
    }
    if (m->fail_disable) {
        if (m->applied_disable_error) m->slot_live = false;
        return USB_TIMEOUT;
    }
    m->slot_live = m->slot_owned = false;
    m->phase = 0; return USB_OK;
}
static int interrupt_transfer(void *context, const struct usb_host_route *route,
                              uint8_t address, uint8_t endpoint, void *data,
                              size_t capacity, size_t *actual, uint32_t timeout_ms)
{
    struct mock_controller *m = context; assert(route && address == 5 && actual && timeout_ms);
    ++m->interrupts; *actual = 0;
    if (m->disconnected) return USB_DISCONNECTED;
    if (m->raw_error) return m->raw_error;
    if (endpoint == 0x81) {
        assert(capacity == 8);
        uint8_t pressed[8] = {0,0,4,0,0,0,0,0};
        if (!m->keyboard_reads++) { memcpy(data, pressed, sizeof(pressed)); *actual = sizeof(pressed); }
        else { memset(data, 0, sizeof(pressed)); *actual = sizeof(pressed); }
        if (m->raw_length) *actual = m->raw_length;
        if (m->keyboard_reserved) ((uint8_t *)data)[1] = 1;
        return USB_OK;
    }
    assert(endpoint == 0x82 && capacity == 4);
    uint8_t pointer[4] = {1,2,0xff,1}; memcpy(data, pointer, sizeof(pointer)); *actual = sizeof(pointer);
    if (m->raw_length) *actual = m->raw_length;
    if (m->mouse_reserved) ((uint8_t *)data)[0] = 0xe0;
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
    if (!down && events->reenter_device) {
        events->reenter_stop = usb_host_stop_device(events->reenter_device);
        events->reenter_release = usb_host_release_stopped_device(events->reenter_device, NULL, NULL, NULL);
        events->reenter_disconnect = usb_host_disconnect(events->reenter_device, NULL, NULL, NULL);
    }
}
static void pointer_event(void *context, uint8_t buttons, int16_t x, int16_t y, int16_t wheel)
{
    struct input_events *events = context; ++events->pointer_count;
    events->buttons = buttons; events->x = x; events->y = y; events->wheel = wheel;
}
static struct usb_host_ops ops(struct mock_controller *m)
{
    return (struct usb_host_ops){m, control, address_device, configure_endpoint,
        interrupt_transfer, bulk_transfer, reset_endpoint, disable_device, NULL, NULL, NULL, NULL};
}
static struct usb_host_route route(void)
{
    struct usb_host_route value = {0}; value.root_port = 2; value.depth = 1;
    value.hub_address[0] = 7; value.hub_port[0] = 3; value.tt_hub_address = 7;
    value.tt_port = 3; value.tt_think_time_ns = 800; return value;
}

static int prepare_device(void *context, const struct usb_host_route *path,
                          enum usb_host_speed speed, bool *cleanup_required)
{
    struct mock_controller *m = context;
    assert(path && path->root_port == 2 && speed == USB_HOST_HIGH_SPEED);
    assert(cleanup_required && !*cleanup_required && !m->slot_owned);
    ++m->prepare_calls;
    if (m->reenter_device) {
        m->reenter_stop = usb_host_stop_device(m->reenter_device);
        m->reenter_release = usb_host_release_stopped_device(m->reenter_device, NULL, NULL, NULL);
        m->reenter_disconnect = usb_host_disconnect(m->reenter_device, NULL, NULL, NULL);
        struct usb_host_ops nested = ops(m);
        assert(usb_host_enumerate(m->reenter_device, &nested, path, speed, 5) == USB_BUSY);
    }
    if (m->prepare_failure == 1) return USB_TIMEOUT; /* no side effect */
    *cleanup_required = true; /* before the simulated slot operation */
    m->slot_owned = m->slot_live = true;
    if (m->prepare_failure == 2) return USB_TIMEOUT; /* applied but error */
    if (m->prepare_bad_success) *cleanup_required = false;
    return USB_OK;
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

static void test_tt_bot_halt_recovery(void)
{
    struct mock_controller m = {0}; struct usb_host_device device = {0};
    enumerate(&m, &device);
    device.route.tt_hub_address = 7;
    device.route.tt_port = 3;
    device.route.multi_tt = true;
    assert(device.bot_transport.clear_halt(device.bot_transport.context, 0x83) == USB_OK);
    assert(m.tt_clear_calls == 1u && m.tt_clear_hub_address == 7u &&
           m.tt_clear_route_depth == 0u && m.tt_clear_value == 0x9053u &&
           m.tt_clear_index == 3u && m.reset_order < m.tt_clear_order &&
           m.tt_clear_order < m.clear_feature_order);

    unsigned previous_reset_order = m.reset_order;
    device.route.multi_tt = false;
    assert(device.bot_transport.clear_halt(device.bot_transport.context, 0x03) == USB_OK);
    assert(m.tt_clear_calls == 2u && m.tt_clear_value == 0x1053u &&
           m.tt_clear_index == 1u && m.reset_order > previous_reset_order &&
           m.reset_order < m.tt_clear_order && m.tt_clear_order < m.clear_feature_order);
    device.route.tt_hub_address = 0;
    device.route.tt_port = 0;
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_two_phase_stop_and_release(void)
{
    struct mock_controller m = {0}; struct usb_host_device device = {0};
    struct input_events events = {0};
    struct usb_block_io block = {0}; struct test_lock lock = {0}; uint8_t bounce[512];
    enumerate(&m, &device);
    assert(usb_host_start_storage(&device) == USB_OK && device.bot.present);
    assert(usb_block_io_init(&block, &device.bot, bounce, sizeof(bounce),
                             &lock, block_lock, block_unlock) == USB_OK);
    assert(usb_host_poll_input(&device, key_event, pointer_event, &events, 20) == USB_OK);
    assert(events.key_count == 1 && events.down[0] && events.buttons == 1);
    assert(usb_block_io_quiesce(&block) == USB_OK && block.quiesced && !block.accepting);
    assert(usb_host_disconnect(&device, NULL, pointer_event, &events) == USB_INVALID);
    assert(device.state == USB_HOST_CONFIGURED && device.bot.present && !m.disables);

    /* Software state cannot be released until the backend has stopped DMA. */
    assert(usb_host_release_stopped_device(&device, key_event, pointer_event,
                                           &events) == USB_BUSY);
    assert(device.state == USB_HOST_CONFIGURED && device.bot.present && !m.disables);
    assert(usb_block_io_detach(&block, false) == USB_BUSY && !block.detached);

    m.reenter_device = &device;
    m.fail_disable = true;
    assert(usb_host_stop_device(&device) == USB_TIMEOUT);
    assert(device.state == USB_HOST_FAULTED && !device.stop_proven && device.bot.present);
    assert(m.reenter_stop == USB_BUSY && m.reenter_release == USB_BUSY &&
           m.reenter_disconnect == USB_BUSY);
    assert(usb_host_release_stopped_device(&device, key_event, pointer_event,
                                           &events) == USB_BUSY);
    assert(events.key_count == 1 && events.buttons == 1 && m.disables == 1);
    uint8_t report[8]; size_t actual = 99;
    assert(usb_host_read_boot_report(&device, 0, report, sizeof(report), &actual, 20) ==
           USB_DISCONNECTED);
    assert(!actual && device.bot.present);

    m.fail_disable = false;
    assert(usb_host_stop_device(&device) == USB_OK);
    assert(device.state == USB_HOST_FAULTED && device.stop_proven && m.disables == 2);
    assert(usb_host_stop_device(&device) == USB_OK && m.disables == 2);
    /* Held input requires a neutral event callback; refusal retains BOT state. */
    assert(usb_host_release_stopped_device(&device, NULL, pointer_event,
                                           &events) == USB_INVALID);
    assert(device.stop_proven && device.bot.present && events.key_count == 1);
    /* The storage child relinquishes its BOT pointer after stop proof. */
    assert(usb_block_io_detach(&block, device.stop_proven) == USB_OK);
    assert(block.detached && !device.bot.present && block.bot == &device.bot);
    block.bot = NULL; /* Model the Dali storage child's successful detach. */
    events.reenter_device = &device;
    assert(usb_host_release_stopped_device(&device, key_event, pointer_event,
                                           &events) == USB_OK);
    assert(device.state == USB_HOST_EMPTY && !device.stop_proven && !device.bot.present);
    assert(events.key_count == 2 && !events.down[1] && events.buttons == 0);
    assert(events.reenter_stop == USB_BUSY && events.reenter_release == USB_BUSY &&
           events.reenter_disconnect == USB_BUSY && !lock.held);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK && m.disables == 2);
}

static void test_stop_invalid_preconditions(void)
{
    struct mock_controller m = {0}; struct usb_host_device device = {0};
    enumerate(&m, &device);
    device.ops.disable_device = NULL;
    assert(usb_host_stop_device(&device) == USB_IO);
    assert(device.state == USB_HOST_FAULTED && !device.stop_proven && !m.disables);
    assert(usb_host_confirm_controller_stopped(&device) == USB_UNSUPPORTED);
    assert(device.state == USB_HOST_FAULTED && !device.stop_proven);
    assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_BUSY);
    device.ops.disable_device = disable_device;
    assert(usb_host_stop_device(&device) == USB_OK && m.disables == 1);
    assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_OK);

    m = (struct mock_controller){0}; device = (struct usb_host_device){0};
    enumerate(&m, &device);
    device.ops.disable_device = NULL;
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_IO);
    assert(device.state == USB_HOST_FAULTED && !device.stop_proven && !m.disables);
    device.ops.disable_device = disable_device;
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK && m.disables == 1);

    const uint8_t invalid_addresses[] = {0, 128, 255};
    for (size_t i = 0; i < sizeof(invalid_addresses); ++i) {
        m = (struct mock_controller){0}; device = (struct usb_host_device){0};
        enumerate(&m, &device);
        device.address = invalid_addresses[i]; /* No retained cleanup provenance. */
        assert(usb_host_stop_device(&device) == USB_IO);
        assert(device.state == USB_HOST_FAULTED && device.stop_invalid && !device.stop_proven);
        assert(usb_host_stop_device(&device) == USB_IO && !m.disables && m.slot_owned);
        assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_IO && !m.disables);
        assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_BUSY);
    }
}

static void test_invalid_assignment_cleanup(void)
{
    const uint8_t invalid_addresses[] = {0, 128, 255};
    for (size_t i = 0; i < sizeof(invalid_addresses); ++i) {
        for (unsigned applied = 0; applied < 2; ++applied) {
            struct usb_host_device device = {0};
            struct mock_controller m = {.invalid_assignment=true,
                .returned_address=invalid_addresses[i], .fail_disable=true,
                .applied_disable_error=applied != 0, .reenter_device=&device};
            struct usb_host_ops host = ops(&m); struct usb_host_route path = route();
            assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_RANGE);
            assert(device.state == USB_HOST_FAULTED && device.invalid_address_cleanup &&
                   !device.stop_invalid && !device.stop_proven && m.slot_owned);
            assert(m.slot_live == (applied == 0) && m.disables == 1);
            assert(m.reenter_stop == USB_BUSY && m.reenter_release == USB_BUSY &&
                   m.reenter_disconnect == USB_BUSY);
            assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_BUSY);
            m.fail_disable = false;
            assert(usb_host_stop_device(&device) == USB_OK && device.stop_proven);
            assert(!m.slot_owned && !m.slot_live && m.disables == 2);
            assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_OK);
            assert(device.state == USB_HOST_EMPTY);
        }
        struct usb_host_device device = {0};
        struct mock_controller m = {.invalid_assignment=true, .returned_address=invalid_addresses[i]};
        struct usb_host_ops host = ops(&m); struct usb_host_route path = route();
        assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_RANGE);
        assert(device.state == USB_HOST_EMPTY && !m.slot_owned && m.disables == 1);
    }
}

static void test_enumeration_abort_reentry(void)
{
    for (unsigned configured = 0; configured < 2; ++configured) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            struct usb_host_device device = {0};
            struct mock_controller m = {.fail_endpoint_at=configured ? 0u : 3u,
                .fail_keyboard_protocol=configured != 0, .fail_disable=failure != 0,
                .applied_disable_error=failure == 2, .reenter_device=&device};
            struct usb_host_ops host = ops(&m); struct usb_host_route path = route();
            assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_TIMEOUT);
            assert(m.reenter_stop == USB_BUSY && m.reenter_release == USB_BUSY &&
                   m.reenter_disconnect == USB_BUSY);
            assert(m.reenter_read == USB_DISCONNECTED && !m.reenter_actual && !m.interrupts);
            if (!failure) {
                assert(device.state == USB_HOST_EMPTY && !m.slot_owned && m.disables == 1);
                continue;
            }
            assert(device.state == USB_HOST_FAULTED && !device.stop_proven &&
                   !device.lifecycle_busy && m.slot_owned && m.disables == 1);
            assert(m.slot_live == (failure == 1));
            assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_BUSY);
            m.fail_disable = false;
            assert(usb_host_stop_device(&device) == USB_OK && device.stop_proven);
            assert(!m.slot_owned && !m.slot_live && m.disables == 2);
            assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_OK);
        }
    }
    struct usb_host_device device = {0};
    struct mock_controller m = {.fail_endpoint_at=3, .clear_disable_on_endpoint=true,
                                .reenter_device=&device};
    struct usb_host_ops host = ops(&m); struct usb_host_route path = route();
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_IO);
    assert(device.state == USB_HOST_FAULTED && m.slot_owned && !m.disables);
    assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_BUSY);
    device.ops.disable_device = disable_device;
    assert(usb_host_stop_device(&device) == USB_OK && !m.slot_owned);
    assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_OK);
}

static void test_default_address_lifecycle(void)
{
    struct usb_host_route path = route();
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            struct usb_host_device device = {0};
            struct mock_controller m = {.prepared_backend=true, .reenter_device=&device,
                .prepare_failure=scenario == 0 ? 2u : 0u,
                .fail_first_descriptor=scenario == 1,
                .bad_ep0=scenario == 2, .fail_address=scenario == 3 || scenario == 5,
                .address_applied_error=scenario == 5, .prepare_bad_success=scenario == 4,
                .fail_disable=failure != 0, .applied_disable_error=failure == 2};
            struct usb_host_ops host = ops(&m);
            host.prepare_device = prepare_device;
            int expected = (scenario == 2 || scenario == 4) ? USB_PHASE_ERROR : USB_TIMEOUT;
            int result = usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5);
            assert(result == (failure ? USB_TIMEOUT : expected));
            assert(m.prepare_calls == 1 && m.disables == 1);
            assert(m.reenter_stop == USB_BUSY && m.reenter_release == USB_BUSY &&
                   m.reenter_disconnect == USB_BUSY);
            if (!failure) {
                assert(device.state == USB_HOST_EMPTY && !m.slot_owned);
                continue;
            }
            assert(device.state == USB_HOST_FAULTED && device.default_address_cleanup &&
                   !device.stop_proven && m.slot_owned && !device.lifecycle_busy);
            assert(m.slot_live == (failure == 1));
            assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_BUSY);
            m.fail_disable = false;
            assert(usb_host_stop_device(&device) == USB_OK && device.stop_proven);
            assert(!m.slot_owned && m.disables == 2);
            assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_OK);
        }
    }
    struct usb_host_device device = {0};
    struct mock_controller m = {.prepared_backend=true, .prepare_failure=1};
    struct usb_host_ops host = ops(&m); host.prepare_device = prepare_device;
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_TIMEOUT);
    assert(device.state == USB_HOST_EMPTY && !m.slot_owned && !m.disables && !m.controls);

    device = (struct usb_host_device){0};
    m = (struct mock_controller){.prepared_backend=true, .reenter_device=&device};
    host = ops(&m); host.prepare_device = prepare_device;
    assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_OK);
    assert(device.state == USB_HOST_CONFIGURED && !device.lifecycle_busy &&
           !device.default_address_cleanup && m.slot_owned && m.address_assigned);
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK && !m.slot_owned);

    const uint8_t invalid_addresses[] = {0, 128, 255};
    for (size_t i = 0; i < sizeof(invalid_addresses); ++i) {
        m = (struct mock_controller){.prepared_backend=true, .invalid_assignment=true,
                                     .returned_address=invalid_addresses[i], .fail_disable=true};
        device = (struct usb_host_device){0}; host = ops(&m); host.prepare_device = prepare_device;
        assert(usb_host_enumerate(&device, &host, &path, USB_HOST_HIGH_SPEED, 5) == USB_RANGE);
        assert(device.state == USB_HOST_FAULTED && device.invalid_address_cleanup && m.slot_owned);
        m.fail_disable = false;
        assert(usb_host_stop_device(&device) == USB_OK && !m.slot_owned);
        assert(usb_host_release_stopped_device(&device, NULL, NULL, NULL) == USB_OK);
    }
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

/* Decode HID short-item sizes/counts independently of the report reader so
 * descriptor edits cannot silently change the IOHID input buffer layout. */
static void check_boot_descriptor_layout(const uint8_t *bytes, size_t length,
                                         uint32_t expected_input_bits)
{
    uint32_t report_size = 0, report_count = 0, input_bits = 0;
    unsigned collections = 0;
    for (size_t at = 0; at < length;) {
        uint8_t prefix = bytes[at++];
        assert(prefix != 0xfe); /* no long items */
        size_t size = prefix & 3u; if (size == 3) size = 4;
        assert(size <= length - at);
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i) value |= (uint32_t)bytes[at+i] << (i*8);
        at += size;
        unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1 && tag == 7) report_size = value;
        else if (type == 1 && tag == 9) report_count = value;
        else if (type == 1 && tag == 8) assert(0 && "boot descriptors have no Report IDs");
        else if (type == 0 && tag == 8) {
            assert(report_size && report_size <= 8 && report_count <= 8);
            input_bits += report_size * report_count;
        } else if (type == 0 && tag == 10) ++collections;
        else if (type == 0 && tag == 12) { assert(collections); --collections; }
    }
    assert(!collections && input_bits == expected_input_bits);
}

static void test_copied_boot_reports(void)
{
    struct mock_controller m = {0}; struct usb_host_device device = {0};
    enumerate(&m, &device);
    uint8_t report[9]; memset(report, 0xa5, sizeof(report)); size_t actual = 99;
    assert(usb_host_read_boot_report(&device, 0, report, 7, &actual, 20) == USB_INVALID);
    assert(!actual && !m.interrupts && report[0] == 0xa5);
    assert(usb_host_read_boot_report(&device, 0, report, 8, &actual, 20) == USB_OK);
    const uint8_t pressed[8] = {0,0,4,0,0,0,0,0};
    assert(actual == 8 && !memcmp(report, pressed, 8) && report[8] == 0xa5);
    assert(usb_host_read_boot_report(&device, 0, report, 8, &actual, 20) == USB_OK);
    const uint8_t released[8] = {0};
    assert(actual == 8 && !memcmp(report, released, 8));
    memset(report, 0xa5, sizeof(report));
    assert(usb_host_read_boot_report(&device, 1, report, 3, &actual, 20) == USB_OK);
    assert(actual == 3 && report[0] == 1 && report[1] == 2 && report[2] == 0xff && report[3] == 0xa5);
    /* The legacy fourth wheel byte is excluded from the three-byte descriptor. */
    m.raw_length = 3;
    assert(usb_host_read_boot_report(&device, 1, report, 3, &actual, 20) == USB_OK && actual == 3);
    for (size_t length = 1; length <= 9; ++length) {
        if (length == 8) continue;
        m.raw_length = length; memset(report, 0xa5, sizeof(report));
        assert(usb_host_read_boot_report(&device, 0, report, 8, &actual, 20) == USB_PHASE_ERROR);
        assert(!actual && report[0] == 0xa5 && report[8] == 0xa5);
    }
    m.raw_length = 0; m.keyboard_reserved = true;
    assert(usb_host_read_boot_report(&device, 0, report, 8, &actual, 20) == USB_PHASE_ERROR && !actual);
    m.keyboard_reserved = false; m.mouse_reserved = true;
    assert(usb_host_read_boot_report(&device, 1, report, 3, &actual, 20) == USB_PHASE_ERROR && !actual);
    m.mouse_reserved = false;
    const int errors[] = {USB_AGAIN, USB_TIMEOUT, USB_IO, USB_DISCONNECTED};
    for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        m.raw_error = errors[i]; memset(report, 0xa5, sizeof(report));
        assert(usb_host_read_boot_report(&device, 0, report, 8, &actual, 20) == errors[i]);
        assert(!actual && report[0] == 0xa5 && device.state == USB_HOST_CONFIGURED && !m.disables);
    }
    m.raw_error = 0; m.fail_disable = true;
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_TIMEOUT);
    assert(usb_host_read_boot_report(&device, 0, report, 8, &actual, 20) == USB_DISCONNECTED && !actual);
    m.fail_disable = false;
    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);

    const uint8_t *descriptor = NULL; size_t length = 0;
    assert(usb_host_boot_report_descriptor(USB_HOST_HID_KEYBOARD, &descriptor, &length) == USB_OK);
    assert(descriptor && length && descriptor[length-1] == 0xc0);
    check_boot_descriptor_layout(descriptor, length, 64);
    const uint8_t *keyboard = descriptor; size_t keyboard_length = length;
    assert(usb_host_boot_report_descriptor(USB_HOST_HID_MOUSE, &descriptor, &length) == USB_OK);
    assert(descriptor && length && descriptor[length-1] == 0xc0 && descriptor != keyboard);
    check_boot_descriptor_layout(descriptor, length, 24);
    assert(usb_host_boot_report_descriptor((enum usb_host_hid_kind)9, &descriptor, &length) == USB_UNSUPPORTED);
    assert(usb_host_boot_report_descriptor(USB_HOST_HID_KEYBOARD, NULL, &length) == USB_INVALID);
    assert(usb_host_boot_report_descriptor(USB_HOST_HID_KEYBOARD, &descriptor, &length) == USB_OK);
    assert(descriptor == keyboard && length == keyboard_length);
}

int main(void)
{
    test_enum_hid_bot_and_disconnect();
    test_tt_bot_halt_recovery();
    test_two_phase_stop_and_release();
    test_stop_invalid_preconditions();
    test_invalid_assignment_cleanup();
    test_enumeration_abort_reentry();
    test_default_address_lifecycle();
    test_optional_class_and_errors();
    test_copied_boot_reports();
    puts("PASS: USB production host enumeration/HID/BOT class routing, TT recovery and cleanup retention");
    return 0;
}
