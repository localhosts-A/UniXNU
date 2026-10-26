/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbHost.h"

static void clear_bytes(void *ptr, size_t count)
{
    uint8_t *out = ptr;
    while (count--) *out++ = 0;
}

static uint16_t get_le16(const uint8_t *bytes)
{ return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8)); }

static int host_control(struct usb_host_device *device, uint8_t address,
                        uint8_t type, uint8_t request, uint16_t value,
                        uint16_t index, void *data, size_t capacity,
                        uint16_t length, size_t *actual)
{
    if (!device || !device->ops.control || !actual || capacity < length || (!data && length))
        return USB_INVALID;
    struct usb_setup setup = {type, request, value, index, length};
    *actual = 0;
    int result = device->ops.control(device->ops.context, &device->route, address,
                                     &setup, data, capacity, actual, 1000);
    if (!result && (*actual > capacity || *actual > length)) return USB_PHASE_ERROR;
    return result;
}

static int host_descriptor(struct usb_host_device *device, uint8_t address,
                           uint8_t type, uint8_t index, void *data,
                           size_t capacity, uint16_t length, size_t *actual)
{
    return host_control(device, address, 0x80, 6,
                        (uint16_t)(((uint16_t)type << 8) | index), 0,
                        data, capacity, length, actual);
}

static bool valid_route(const struct usb_host_route *route)
{
    if (!route || !route->root_port || route->depth > USB_HOST_MAX_ROUTE_DEPTH) return false;
    for (uint8_t i = 0; i < route->depth; ++i) {
        if (!route->hub_address[i] || route->hub_address[i] > 127 || !route->hub_port[i]) return false;
        for (uint8_t j = 0; j < i; ++j)
            if (route->hub_address[i] == route->hub_address[j]) return false;
    }
    if ((route->tt_hub_address == 0) != (route->tt_port == 0)) return false;
    if (route->multi_tt && (!route->tt_hub_address || !route->depth)) return false;
    if (route->tt_hub_address) {
        bool found_tt = false;
        for (uint8_t i = 0; i < route->depth; ++i)
            if (route->hub_address[i] == route->tt_hub_address && route->hub_port[i] == route->tt_port)
                found_tt = true;
        if (!found_tt) return false;
    }
    return true;
}

static bool valid_ep0_packet(enum usb_host_speed speed, uint8_t packet)
{
    if (speed == USB_HOST_LOW_SPEED) return packet == 8;
    if (speed == USB_HOST_HIGH_SPEED) return packet == 64;
    return speed == USB_HOST_FULL_SPEED &&
        (packet == 8 || packet == 16 || packet == 32 || packet == 64);
}

static bool valid_endpoint_for_speed(enum usb_host_speed speed,
                                     const struct usb_endpoint *endpoint)
{
    if (endpoint->transactions > 1 && (speed != USB_HOST_HIGH_SPEED ||
        (endpoint->type != 1 && endpoint->type != 3))) return false;
    if (endpoint->type == 2) {
        if (speed == USB_HOST_LOW_SPEED) return false;
        if (speed == USB_HOST_HIGH_SPEED) return endpoint->max_packet == 512;
        return endpoint->max_packet == 8 || endpoint->max_packet == 16 ||
               endpoint->max_packet == 32 || endpoint->max_packet == 64;
    }
    if (endpoint->type == 3) {
        if (speed == USB_HOST_LOW_SPEED) return endpoint->max_packet <= 8;
        if (speed == USB_HOST_FULL_SPEED) return endpoint->max_packet <= 64;
        return endpoint->max_packet <= 1024 && endpoint->interval <= 16;
    }
    if (endpoint->type == 1) {
        return speed != USB_HOST_LOW_SPEED && endpoint->max_packet <= 1023 && endpoint->interval <= 16;
    }
    return false;
}

static struct usb_host_hid_interface *new_hid(struct usb_host_device *device,
                                               const struct usb_interface *interface,
                                               enum usb_host_hid_kind kind)
{
    if (device->hid_count == USB_HOST_MAX_HID_INTERFACES) return NULL;
    struct usb_host_hid_interface *binding = &device->hid[device->hid_count++];
    clear_bytes(binding, sizeof(*binding));
    binding->kind = kind;
    binding->interface_number = interface->number;
    return binding;
}

static int discover_classes(struct usb_host_device *device)
{
    const struct usb_configuration *configuration = &device->parsed_configuration;
    for (uint8_t i = 0; i < configuration->interface_count; ++i) {
        const struct usb_interface *interface = &configuration->interfaces[i];
        if (interface->klass == 3 && interface->subclass == 1 &&
            (interface->protocol == 1 || interface->protocol == 2)) {
            enum usb_host_hid_kind kind = interface->protocol == 1 ?
                USB_HOST_HID_KEYBOARD : USB_HOST_HID_MOUSE;
            bool already = false;
            for (uint8_t h = 0; h < device->hid_count; ++h)
                already |= device->hid[h].kind == kind;
            if (already) continue;
            struct usb_host_hid_interface *binding = new_hid(device, interface, kind);
            if (!binding) continue;
            for (uint8_t e = 0; e < configuration->endpoint_count; ++e) {
                const struct usb_endpoint *endpoint = &configuration->endpoints[e];
                if (endpoint->interface_number != interface->number ||
                    endpoint->type != 3 || !(endpoint->address & 0x80)) continue;
                if ((kind == USB_HOST_HID_KEYBOARD && endpoint->max_packet < 8) ||
                    (kind == USB_HOST_HID_MOUSE && endpoint->max_packet < 3)) continue;
                binding->endpoint = endpoint->address;
                binding->interval = endpoint->interval;
                binding->max_packet = endpoint->max_packet;
                break;
            }
            if (!binding->endpoint) --device->hid_count;
        } else if (interface->klass == 9 && interface->subclass == 0 &&
                   interface->protocol <= 1 && !device->hub_interface.present) {
            for (uint8_t e = 0; e < configuration->endpoint_count; ++e) {
                const struct usb_endpoint *endpoint = &configuration->endpoints[e];
                if (endpoint->interface_number != interface->number ||
                    endpoint->type != 3 || !(endpoint->address & 0x80) ||
                    !endpoint->max_packet) continue;
                device->hub_interface.interface_number = interface->number;
                device->hub_interface.endpoint = endpoint->address;
                device->hub_interface.interval = endpoint->interval;
                device->hub_interface.protocol = interface->protocol;
                device->hub_interface.max_packet = endpoint->max_packet;
                device->hub_interface.present = true;
                break;
            }
        } else if (interface->klass == 8 && interface->subclass == 6 &&
                   interface->protocol == 0x50 && !device->has_bot) {
            uint8_t input = 0, output = 0;
            for (uint8_t e = 0; e < configuration->endpoint_count; ++e) {
                const struct usb_endpoint *endpoint = &configuration->endpoints[e];
                if (endpoint->interface_number != interface->number || endpoint->type != 2) continue;
                if (endpoint->address & 0x80) { if (!input) input = endpoint->address; }
                else if (!output) output = endpoint->address;
            }
            if (input && output) {
                device->bot_transport.context = device;
                device->bot_transport.control = NULL; /* Filled after endpoint setup. */
                device->bot_transport.bulk = NULL;
                device->bot_transport.clear_halt = NULL;
                device->bot.interface_number = interface->number;
                device->bot.in_endpoint = input;
                device->bot.out_endpoint = output;
                device->has_bot = true;
            }
        }
    }
    return USB_OK;
}

static int find_multi_tt_endpoint(const uint8_t *bytes, size_t length,
                                  uint8_t interface_number,
                                  struct usb_endpoint *endpoint, bool *found)
{
    if (!bytes || !endpoint || !found || length < 9) return USB_INVALID;
    *found = false;
    size_t total = get_le16(bytes + 2);
    bool candidate = false, seen = false;
    uint8_t expected_endpoints = 0;
    size_t found_endpoints = 0;
    struct usb_endpoint selected = {0};
    for (size_t at = 9; at < total;) {
        uint8_t descriptor_length = bytes[at], kind = bytes[at + 1];
        if (kind == 4) {
            candidate = false;
            if (descriptor_length < 9) return USB_INVALID;
            if (bytes[at + 2] == interface_number && bytes[at + 3] == 1 &&
                bytes[at + 5] == 9 && bytes[at + 6] == 0) {
                if (seen || bytes[at + 7] != 2) return USB_PHASE_ERROR;
                seen = true;
                candidate = true;
                expected_endpoints = bytes[at + 4];
            }
        } else if (kind == 5 && candidate) {
            if (descriptor_length < 7) return USB_INVALID;
            ++found_endpoints;
            uint8_t address = bytes[at + 2];
            uint8_t type = bytes[at + 3] & 3u;
            uint16_t encoded_packet = get_le16(bytes + at + 4);
            uint8_t additional = (uint8_t)((encoded_packet >> 11) & 3u);
            uint16_t packet = encoded_packet & 0x7ffu;
            if (!(address & 0x80u) || !(address & 0x0fu) || (address & 0x70u) ||
                type != 3 || !packet || packet > 1024 || (encoded_packet & 0xe000u) ||
                additional == 3 || !bytes[at + 6]) return USB_PHASE_ERROR;
            selected.address = address;
            selected.type = type;
            selected.max_packet = packet;
            selected.interval = bytes[at + 6];
            selected.interface_number = interface_number;
            selected.transactions = (uint8_t)(additional + 1u);
        }
        at += descriptor_length;
    }
    if (!seen) return USB_OK;
    if (expected_endpoints != 1 || found_endpoints != 1) return USB_PHASE_ERROR;
    *endpoint = selected;
    *found = true;
    return USB_OK;
}

static bool selected_interface(const struct usb_host_device *device, uint8_t number)
{
    for (uint8_t h = 0; h < device->hid_count; ++h)
        if (device->hid[h].interface_number == number) return true;
    if (device->hub_interface.present &&
        device->hub_interface.interface_number == number) return true;
    return device->has_bot && device->bot.interface_number == number;
}

static bool selected_endpoint(const struct usb_host_device *device,
                              const struct usb_endpoint *endpoint)
{
    if (device->hub_interface.present &&
        device->hub_interface.interface_number == endpoint->interface_number) {
        return !device->hub_interface.use_multi_tt &&
               endpoint->address == device->hub_interface.endpoint;
    }
    return selected_interface(device, endpoint->interface_number);
}

static int host_bot_control(void *context, const struct usb_setup *setup,
                            void *data, size_t capacity, size_t *actual,
                            uint32_t timeout_ms)
{
    struct usb_host_device *device = context;
    if (!device || device->state != USB_HOST_CONFIGURED || !actual || !setup ||
        !timeout_ms || setup->length > capacity || (setup->length && !data)) return USB_INVALID;
    *actual = 0;
    int result = device->ops.control(device->ops.context, &device->route, device->address,
                                     setup, data, capacity, actual, timeout_ms);
    if (!result && (*actual > capacity || *actual > setup->length)) return USB_PHASE_ERROR;
    return result;
}

static int host_bot_bulk(void *context, uint8_t endpoint, void *data, size_t size,
                         size_t *actual, uint32_t timeout_ms)
{
    struct usb_host_device *device = context;
    if (!device || device->state != USB_HOST_CONFIGURED || !device->ops.bulk ||
        !actual || !timeout_ms || (!data && size) || size > UINT32_MAX) return USB_INVALID;
    *actual = 0;
    int result = device->ops.bulk(device->ops.context, &device->route, device->address,
                                  endpoint, data, size, actual, timeout_ms);
    if (!result && *actual > size) return USB_PHASE_ERROR;
    return result;
}

static int host_bot_clear_halt(void *context, uint8_t endpoint)
{
    struct usb_host_device *device = context;
    if (!device || device->state != USB_HOST_CONFIGURED || !endpoint ||
        (endpoint & 0x70) || !(endpoint & 15)) return USB_INVALID;
    size_t actual = 0;
    int result = host_control(device, device->address, 0x02, 1, 0, endpoint,
                              NULL, 0, 0, &actual);
    if (!result && actual) return USB_PHASE_ERROR;
    if (result) return result;
    if (!device->ops.reset_endpoint) return USB_UNSUPPORTED;
    return device->ops.reset_endpoint(device->ops.context, &device->route,
                                      device->address, endpoint);
}

static int abort_enumeration(struct usb_host_device *device, int failure)
{
    if (device->address) {
        device->state = USB_HOST_FAULTED;
        if (!device->ops.disable_device) return USB_IO;
        device->lifecycle_busy = true;
        int stopped = device->ops.disable_device(device->ops.context, &device->route,
                                                 device->address);
        device->lifecycle_busy = false;
        if (stopped) return stopped;
    }
    clear_bytes(device, sizeof(*device));
    return failure;
}

int usb_host_enumerate(struct usb_host_device *device, const struct usb_host_ops *ops,
                       const struct usb_host_route *route, enum usb_host_speed speed,
                       uint8_t preferred_address)
{
    if (!device || !ops || !ops->control || !ops->address_device ||
        !ops->configure_endpoint || !ops->disable_device || !ops->bulk ||
        speed < USB_HOST_LOW_SPEED || speed > USB_HOST_HIGH_SPEED ||
        !preferred_address || preferred_address > 127 || !valid_route(route)) return USB_INVALID;
    if (device->state != USB_HOST_EMPTY) return USB_BUSY;
    clear_bytes(device, sizeof(*device));
    device->ops = *ops;
    device->route = *route;
    device->speed = speed;

    uint8_t descriptor[18] = {0};
    size_t actual = 0;
    int result = host_descriptor(device, 0, 1, 0, descriptor, sizeof(descriptor), 8, &actual);
    if (result || actual != 8 || descriptor[0] != sizeof(descriptor) || descriptor[1] != 1 ||
        !valid_ep0_packet(speed, descriptor[7]))
        return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
    for (size_t i = 0; i < 8; ++i) device->device_descriptor[i] = descriptor[i];

    uint8_t address = 0;
    result = ops->address_device(ops->context, route, speed, descriptor[7],
                                 preferred_address, &address);
    if (result) return abort_enumeration(device, result);
    if (!address || address > 127) {
        device->address = address;
        device->state = USB_HOST_FAULTED;
        device->invalid_address_cleanup = true;
        device->lifecycle_busy = true;
        int stopped = ops->disable_device(ops->context, route, address);
        device->lifecycle_busy = false;
        if (stopped) return USB_RANGE;
        clear_bytes(device, sizeof(*device));
        return USB_RANGE;
    }
    device->address = address;
    device->state = USB_HOST_ADDRESSED;

    result = host_descriptor(device, address, 1, 0, descriptor, sizeof(descriptor),
                             sizeof(descriptor), &actual);
    if (result || actual != sizeof(descriptor) || descriptor[0] != sizeof(descriptor) ||
        descriptor[1] != 1 || descriptor[7] != device->device_descriptor[7] || !descriptor[17])
        return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
    for (size_t i = 0; i < sizeof(descriptor); ++i) device->device_descriptor[i] = descriptor[i];

    uint8_t header[9] = {0};
    result = host_descriptor(device, address, 2, 0, header, sizeof(header), sizeof(header), &actual);
    if (result || actual != sizeof(header) || header[0] < sizeof(header) || header[1] != 2)
        return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
    uint16_t total = get_le16(header + 2);
    if (total < sizeof(header) || total > USB_HOST_MAX_CONFIGURATION || !header[5] ||
        header[4] > USB_MAX_INTERFACES)
        return abort_enumeration(device, USB_RANGE);
    uint8_t bytes[USB_HOST_MAX_CONFIGURATION];
    clear_bytes(bytes, total);
    result = host_descriptor(device, address, 2, 0, bytes, total, total, &actual);
    if (result || actual != total || bytes[0] < sizeof(header) || bytes[1] != 2 ||
        get_le16(bytes + 2) != total || bytes[5] != header[5] || bytes[4] != header[4])
        return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
    result = usb_parse_configuration(bytes, total, &device->parsed_configuration);
    if (result) return abort_enumeration(device, result);
    device->configuration = device->parsed_configuration.value;
    (void)discover_classes(device);
    struct usb_endpoint multi_tt_endpoint = {0};
    bool use_multi_tt = false;
    if (device->hub_interface.present) {
        if (speed == USB_HOST_HIGH_SPEED) {
            uint8_t device_protocol = device->device_descriptor[6];
            if (device_protocol == 1 && device->hub_interface.protocol == 0) {
                /* High-speed single-TT hubs use interface protocol zero. */
            } else if (device_protocol == 2 && device->hub_interface.protocol == 1) {
                bool found = false;
                result = find_multi_tt_endpoint(bytes, total,
                                                 device->hub_interface.interface_number,
                                                 &multi_tt_endpoint, &found);
                if (result || !found)
                    return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
                if (!valid_endpoint_for_speed(speed, &multi_tt_endpoint))
                    return abort_enumeration(device, USB_UNSUPPORTED);
                device->hub_interface.multi_tt_endpoint = multi_tt_endpoint.address;
                device->hub_interface.multi_tt_interval = multi_tt_endpoint.interval;
                device->hub_interface.multi_tt_max_packet = multi_tt_endpoint.max_packet;
                device->hub_interface.multi_tt_available = true;
                device->hub_interface.use_multi_tt = true;
                use_multi_tt = true;
            } else {
                return abort_enumeration(device, USB_UNSUPPORTED);
            }
        } else if (speed == USB_HOST_FULL_SPEED &&
                   device->device_descriptor[6] == 0 &&
                   device->hub_interface.protocol == 0) {
            /* Full-speed hubs have no transaction translator interface. */
        } else {
            return abort_enumeration(device, USB_UNSUPPORTED);
        }
    }

    actual = 0;
    result = host_control(device, address, 0, 9, device->configuration, 0,
                          NULL, 0, 0, &actual);
    if (result || actual) return abort_enumeration(device, result ? result : USB_PHASE_ERROR);

    if (use_multi_tt) {
        actual = 0;
        result = host_control(device, address, 0x01, 11, 1,
                              device->hub_interface.interface_number,
                              NULL, 0, 0, &actual);
        if (result || actual)
            return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
        result = ops->configure_endpoint(ops->context, route, address, speed,
                                         &multi_tt_endpoint);
        if (result) return abort_enumeration(device, result);
        device->hub_interface.endpoint = multi_tt_endpoint.address;
        device->hub_interface.interval = multi_tt_endpoint.interval;
        device->hub_interface.max_packet = multi_tt_endpoint.max_packet;
        device->hub_interface.protocol = 2;
    }

    for (uint8_t e = 0; e < device->parsed_configuration.endpoint_count; ++e) {
        const struct usb_endpoint *endpoint = &device->parsed_configuration.endpoints[e];
        if (!selected_endpoint(device, endpoint)) continue;
        if (!valid_endpoint_for_speed(speed, endpoint))
            return abort_enumeration(device, USB_UNSUPPORTED);
        result = ops->configure_endpoint(ops->context, route, address, speed, endpoint);
        if (result) return abort_enumeration(device, result);
    }
    device->state = USB_HOST_CONFIGURED;

    uint8_t old_hid_count = device->hid_count, usable_hid = 0;
    for (uint8_t h = 0; h < old_hid_count; ++h) {
        struct usb_host_hid_interface *binding = &device->hid[h];
        actual = 0;
        result = host_control(device, address, 0x21, 0x0b, 0,
                              binding->interface_number, NULL, 0, 0, &actual);
        if (result == USB_STALL) continue;
        if (result || actual) return abort_enumeration(device, result ? result : USB_PHASE_ERROR);
        binding->active = true;
        if (usable_hid != h) device->hid[usable_hid] = *binding;
        ++usable_hid;
    }
    for (uint8_t h = usable_hid; h < old_hid_count; ++h)
        clear_bytes(&device->hid[h], sizeof(device->hid[h]));
    device->hid_count = usable_hid;
    if (device->has_bot) {
        device->bot_transport.control = host_bot_control;
        device->bot_transport.bulk = host_bot_bulk;
        device->bot_transport.clear_halt = host_bot_clear_halt;
        result = usb_bot_init(&device->bot, &device->bot_transport,
                              device->bot.interface_number, device->bot.in_endpoint,
                              device->bot.out_endpoint, 0);
        if (result) return abort_enumeration(device, result);
    }
    return USB_OK;
}

int usb_host_start_storage(struct usb_host_device *device)
{
    if (!device || device->state != USB_HOST_CONFIGURED) return USB_DISCONNECTED;
    if (!device->has_bot || !device->bot.present) return USB_UNSUPPORTED;
    return usb_bot_capacity(&device->bot);
}

int usb_host_boot_report_descriptor(enum usb_host_hid_kind kind,
                                    const uint8_t **descriptor, size_t *length)
{
    static const uint8_t keyboard[] = {
        0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x05,0x07,
        0x19,0xe0, 0x29,0xe7, 0x15,0x00, 0x25,0x01,
        0x75,0x01, 0x95,0x08, 0x81,0x02,
        0x75,0x08, 0x95,0x01, 0x81,0x01,
        0x15,0x00, 0x26,0xff,0x00, 0x19,0x00, 0x2a,0xff,0x00,
        0x75,0x08, 0x95,0x06, 0x81,0x00, 0xc0
    };
    static const uint8_t mouse[] = {
        0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x09,0x01, 0xa1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
        0x95,0x05, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x03, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x80, 0x25,0x7f,
        0x75,0x08, 0x95,0x02, 0x81,0x06, 0xc0,0xc0
    };
    if (!descriptor || !length) return USB_INVALID;
    if (kind == USB_HOST_HID_KEYBOARD) {
        *descriptor = keyboard; *length = sizeof(keyboard);
    } else if (kind == USB_HOST_HID_MOUSE) {
        *descriptor = mouse; *length = sizeof(mouse);
    } else return USB_UNSUPPORTED;
    return USB_OK;
}

int usb_host_read_boot_report(struct usb_host_device *device, uint8_t hid_index,
                              uint8_t *buffer, size_t capacity, size_t *actual,
                              uint32_t timeout_ms)
{
    if (!actual) return USB_INVALID;
    *actual = 0;
    if (!device || !buffer || !timeout_ms || timeout_ms > 10000 ||
        hid_index >= device->hid_count || hid_index >= USB_HOST_MAX_HID_INTERFACES ||
        !device->ops.interrupt) return USB_INVALID;
    if (device->state != USB_HOST_CONFIGURED) return USB_DISCONNECTED;
    const struct usb_host_hid_interface *binding = &device->hid[hid_index];
    size_t required;
    if (binding->kind == USB_HOST_HID_KEYBOARD) required = 8;
    else if (binding->kind == USB_HOST_HID_MOUSE) required = 3;
    else return USB_UNSUPPORTED;
    if (!binding->active || capacity < required || binding->max_packet < required ||
        binding->max_packet > 1024) return USB_INVALID;
    uint8_t report[1024];
    size_t received = 0;
    int result = device->ops.interrupt(device->ops.context, &device->route,
                                      device->address, binding->endpoint,
                                      report, binding->max_packet, &received, timeout_ms);
    if (result) return result;
    if (received > binding->max_packet ||
        (binding->kind == USB_HOST_HID_KEYBOARD && (received != 8 || report[1])) ||
        (binding->kind == USB_HOST_HID_MOUSE &&
         ((received != 3 && received != 4) || (report[0] & 0xe0))))
        return USB_PHASE_ERROR;
    for (size_t i = 0; i < required; ++i) buffer[i] = report[i];
    *actual = required;
    return USB_OK;
}

int usb_host_poll_input(struct usb_host_device *device, usb_key_event key_event,
                        usb_pointer_event pointer_event, void *event_context,
                        uint32_t timeout_ms)
{
    if (!device || device->state != USB_HOST_CONFIGURED || !device->ops.interrupt ||
        !timeout_ms || timeout_ms > 10000) return USB_INVALID;
    for (uint8_t h = 0; h < device->hid_count; ++h)
        if ((device->hid[h].kind == USB_HOST_HID_KEYBOARD && !key_event) ||
            (device->hid[h].kind == USB_HOST_HID_MOUSE && !pointer_event)) return USB_INVALID;
    uint8_t report[1024];
    int first_error = USB_OK;
    for (uint8_t h = 0; h < device->hid_count; ++h) {
        struct usb_host_hid_interface *binding = &device->hid[h];
        if (!binding->active) continue;
        size_t actual = 0;
        int result = device->ops.interrupt(device->ops.context, &device->route,
                                           device->address, binding->endpoint,
                                           report, binding->max_packet, &actual,
                                           timeout_ms);
        if (result == USB_AGAIN || result == USB_TIMEOUT) continue;
        if (result) {
            if (result == USB_DISCONNECTED) {
                int stopped = usb_host_disconnect(device, key_event, pointer_event, event_context);
                return stopped ? stopped : USB_DISCONNECTED;
            }
            if (!first_error) first_error = result;
            continue;
        }
        if (actual > binding->max_packet) {
            if (!first_error) first_error = USB_PHASE_ERROR;
            continue;
        }
        if (binding->kind == USB_HOST_HID_KEYBOARD) {
            if (key_event) result = usb_keyboard_report(&binding->state.keyboard,
                                                        report, actual, key_event, event_context);
            else result = USB_INVALID;
        } else {
            if (pointer_event) result = usb_mouse_report(&binding->state.mouse,
                                                         report, actual, pointer_event, event_context);
            else result = USB_INVALID;
        }
        if (result && !first_error) first_error = result;
    }
    return first_error;
}

static int validate_disconnect_events(const struct usb_host_device *device,
                                      usb_key_event key_event,
                                      usb_pointer_event pointer_event)
{
    for (uint8_t h = 0; h < device->hid_count; ++h) {
        const struct usb_host_hid_interface *binding = &device->hid[h];
        if (binding->kind == USB_HOST_HID_KEYBOARD) {
            bool held = binding->state.keyboard.modifiers != 0;
            for (uint8_t k = 0; k < 6; ++k) held |= binding->state.keyboard.keys[k] != 0;
            if (held && !key_event) return USB_INVALID;
        } else if (binding->state.mouse.buttons && !pointer_event) return USB_INVALID;
    }
    return USB_OK;
}

int usb_host_stop_device(struct usb_host_device *device)
{
    if (!device) return USB_INVALID;
    if (device->lifecycle_busy) return USB_BUSY;
    if (device->state == USB_HOST_EMPTY || device->stop_proven) return USB_OK;
    if (device->stop_invalid) return USB_IO;
    bool invalid_address = (!device->address || device->address > 127) &&
                           !device->invalid_address_cleanup;
    /* Close host admission even if the backend is missing or stop needs retry. */
    device->state = USB_HOST_FAULTED;
    if (invalid_address) {
        /* A malformed live address without cleanup provenance cannot prove stop. */
        device->stop_invalid = true;
        return USB_IO;
    }
    if (!device->ops.disable_device) return USB_IO;
    device->lifecycle_busy = true;
    int result = device->ops.disable_device(device->ops.context, &device->route,
                                            device->address);
    if (!result) device->stop_proven = true;
    device->lifecycle_busy = false;
    return result;
}

int usb_host_release_stopped_device(struct usb_host_device *device,
                                    usb_key_event key_event,
                                    usb_pointer_event pointer_event,
                                    void *event_context)
{
    if (!device) return USB_INVALID;
    if (device->lifecycle_busy) return USB_BUSY;
    if (device->state == USB_HOST_EMPTY) return USB_OK;
    if (!device->stop_proven) return USB_BUSY;
    int result = validate_disconnect_events(device, key_event, pointer_event);
    if (result) return result;
    device->lifecycle_busy = true;
    for (uint8_t h = 0; h < device->hid_count; ++h) {
        struct usb_host_hid_interface *binding = &device->hid[h];
        if (binding->kind == USB_HOST_HID_KEYBOARD && key_event)
            usb_keyboard_disconnect(&binding->state.keyboard, key_event, event_context);
        else if (binding->kind == USB_HOST_HID_MOUSE && pointer_event)
            usb_mouse_disconnect(&binding->state.mouse, pointer_event, event_context);
    }
    if (device->has_bot) usb_bot_detach(&device->bot);
    clear_bytes(device, sizeof(*device));
    return USB_OK;
}

int usb_host_disconnect(struct usb_host_device *device, usb_key_event key_event,
                        usb_pointer_event pointer_event, void *event_context)
{
    if (!device) return USB_INVALID;
    if (device->lifecycle_busy) return USB_BUSY;
    if (device->state == USB_HOST_EMPTY) return USB_OK;
    if (!device->stop_proven &&
        (device->stop_invalid || !device->ops.disable_device ||
         ((!device->address || device->address > 127) &&
          !device->invalid_address_cleanup)))
        return usb_host_stop_device(device);
    int result = validate_disconnect_events(device, key_event, pointer_event);
    if (result) return result;
    result = usb_host_stop_device(device);
    if (result) return result;
    return usb_host_release_stopped_device(device, key_event, pointer_event,
                                           event_context);
}

#define USB_HUB_DESCRIPTOR_TYPE 0x29u
#define USB_HUB_PORT_CONNECTION 0x0001u
#define USB_HUB_PORT_ENABLE 0x0002u
#define USB_HUB_PORT_OVERCURRENT 0x0008u
#define USB_HUB_PORT_RESET 0x0010u
#define USB_HUB_PORT_POWER 0x0100u
#define USB_HUB_PORT_LOW_SPEED 0x0200u
#define USB_HUB_PORT_HIGH_SPEED 0x0400u
#define USB_HUB_PORT_C_CONNECTION 0x0001u
#define USB_HUB_PORT_C_ENABLE 0x0002u
#define USB_HUB_PORT_C_SUSPEND 0x0004u
#define USB_HUB_PORT_C_OVERCURRENT 0x0008u
#define USB_HUB_PORT_C_RESET 0x0010u
#define USB_HUB_PORT_FEATURE_RESET 4u
#define USB_HUB_PORT_FEATURE_POWER 8u
#define USB_HUB_HUB_C_LOCAL_POWER 0x0001u
#define USB_HUB_HUB_C_OVERCURRENT 0x0002u
#define USB_HUB_FEATURE_C_LOCAL_POWER 0u
#define USB_HUB_FEATURE_C_OVERCURRENT 1u
#define USB_HUB_POWER_NO_SWITCHING 2u
#define USB_HUB_RESET_MIN_MS 50u
#define USB_HUB_RESET_TIMEOUT_MS 1000u

static bool host_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{ return (uint32_t)(now_ms - deadline_ms) < 0x80000000u; }

static int hub_feature(struct usb_host_device *device, uint8_t request_type,
                      uint8_t request, uint16_t feature, uint16_t port)
{
    size_t actual = 0;
    int result = host_control(device, device->address, request_type, request, feature,
                              port, NULL, 0, 0, &actual);
    if (!result && actual) return USB_PHASE_ERROR;
    return result;
}

static int hub_get_port_status(struct usb_host_device *device, uint8_t port,
                               uint16_t *status, uint16_t *change)
{
    uint8_t bytes[4] = {0};
    size_t actual = 0;
    if (!status || !change) return USB_INVALID;
    int result = host_control(device, device->address, 0xa3, 0, 0, port,
                              bytes, sizeof(bytes), sizeof(bytes), &actual);
    if (result) return result;
    if (actual != sizeof(bytes)) return USB_PHASE_ERROR;
    *status = get_le16(bytes);
    *change = get_le16(bytes + 2);
    return USB_OK;
}

static int hub_get_status(struct usb_host_device *device,
                          uint16_t *status, uint16_t *change)
{
    uint8_t bytes[4] = {0};
    size_t actual = 0;
    if (!status || !change) return USB_INVALID;
    int result = host_control(device, device->address, 0xa0, 0, 0, 0,
                              bytes, sizeof(bytes), sizeof(bytes), &actual);
    if (result) return result;
    if (actual != sizeof(bytes)) return USB_PHASE_ERROR;
    *status = get_le16(bytes);
    *change = get_le16(bytes + 2);
    return USB_OK;
}

static int hub_clear_port_changes(struct usb_host_device *device, uint8_t port,
                                  uint16_t change)
{
    static const struct { uint16_t mask, feature; } features[] = {
        {USB_HUB_PORT_C_CONNECTION, 16}, {USB_HUB_PORT_C_ENABLE, 17},
        {USB_HUB_PORT_C_SUSPEND, 18}, {USB_HUB_PORT_C_OVERCURRENT, 19},
        {USB_HUB_PORT_C_RESET, 20}
    };
    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (!(change & features[i].mask)) continue;
        int result = hub_feature(device, 0x23, 1, features[i].feature, port);
        if (result) return result;
    }
    return USB_OK;
}

static int hub_clear_status_changes(struct usb_host_device *device, uint16_t change)
{
    if (change & USB_HUB_HUB_C_LOCAL_POWER) {
        int result = hub_feature(device, 0x20, 1, USB_HUB_FEATURE_C_LOCAL_POWER, 0);
        if (result) return result;
    }
    if (change & USB_HUB_HUB_C_OVERCURRENT)
        return hub_feature(device, 0x20, 1, USB_HUB_FEATURE_C_OVERCURRENT, 0);
    return USB_OK;
}

static int hub_set_port_power(struct usb_host_device *device,
                              struct usb_host_hub *hub, uint8_t port,
                              bool enabled)
{
    struct usb_host_hub_port *state = &hub->ports[port - 1];
    if (state->powered == enabled) return USB_OK;
    uint16_t power_mode = hub->characteristics & 3u;
    if (power_mode == USB_HUB_POWER_NO_SWITCHING) {
        state->powered = true;
        return enabled ? USB_OK : USB_UNSUPPORTED;
    }
    if (enabled) state->powered = true; /* Retain uncertain SET_PORT_POWER for stop retry. */
    int result = hub_feature(device, 0x23, enabled ? 3 : 1,
                             USB_HUB_PORT_FEATURE_POWER, port);
    if (!result) state->powered = enabled;
    return result;
}

static int hub_speed_from_status(uint16_t status, enum usb_host_speed *speed)
{
    if (!speed) return USB_INVALID;
    bool low = (status & USB_HUB_PORT_LOW_SPEED) != 0;
    bool high = (status & USB_HUB_PORT_HIGH_SPEED) != 0;
    if (low && high) return USB_PHASE_ERROR;
    *speed = high ? USB_HOST_HIGH_SPEED : low ? USB_HOST_LOW_SPEED : USB_HOST_FULL_SPEED;
    return USB_OK;
}

int usb_host_hub_child_route(const struct usb_host_device *device,
                             const struct usb_host_hub *hub, uint8_t port,
                             enum usb_host_speed speed, struct usb_host_route *route)
{
    if (!device || !hub || !route || !hub->initialized || !device->address ||
        device->state != USB_HOST_CONFIGURED || port == 0 || port > hub->port_count ||
        speed < USB_HOST_LOW_SPEED || speed > USB_HOST_HIGH_SPEED ||
        !valid_route(&device->route)) return USB_INVALID;
    if (device->route.depth >= USB_HOST_MAX_ROUTE_DEPTH) return USB_RANGE;
    struct usb_host_route child = device->route;
    child.hub_address[child.depth] = device->address;
    child.hub_port[child.depth] = port;
    ++child.depth;
    if (device->speed == USB_HOST_HIGH_SPEED && speed != USB_HOST_HIGH_SPEED) {
        child.tt_hub_address = device->address;
        child.tt_port = port;
        child.tt_think_time_ns = hub->tt_think_time_ns;
        child.multi_tt = hub->multi_tt;
    }
    if (!valid_route(&child)) return USB_RANGE;
    *route = child;
    return USB_OK;
}

static int hub_emit(struct usb_host_device *device, struct usb_host_hub *hub,
                    uint8_t port, enum usb_host_hub_event_kind kind,
                    enum usb_host_speed speed, usb_host_hub_event callback,
                    void *event_context)
{
    if (!callback) return USB_INVALID;
    struct usb_host_route route;
    int result = usb_host_hub_child_route(device, hub, port, speed, &route);
    if (result) return result;
    return callback(event_context, kind, port, &route, speed);
}

int usb_host_hub_start(struct usb_host_device *device, struct usb_host_hub *hub)
{
    if (!device || !hub || device->state != USB_HOST_CONFIGURED ||
        !device->hub_interface.present || !device->address ||
        !device->ops.interrupt || !device->ops.now_ms || hub->initialized || hub->faulted)
        return USB_INVALID;
    uint8_t header[7] = {0};
    size_t actual = 0;
    int result = host_control(device, device->address, 0xa0, 6,
                              (uint16_t)(USB_HUB_DESCRIPTOR_TYPE << 8), 0,
                              header, sizeof(header), sizeof(header), &actual);
    if (result) return result;
    if (actual != sizeof(header) || header[0] < sizeof(header) ||
        header[1] != USB_HUB_DESCRIPTOR_TYPE || !header[2] ||
        header[2] > USB_HOST_HUB_MAX_PORTS) return USB_PHASE_ERROR;
    uint8_t bitmap_bytes = (uint8_t)((header[2] + 8u) / 8u);
    uint8_t expected = (uint8_t)(7u + 2u * bitmap_bytes);
    uint16_t characteristics = get_le16(header + 3);
    uint16_t power_mode = characteristics & 3u;
    if (header[0] != expected || (characteristics & 0xff00u) || power_mode == 3u)
        return USB_PHASE_ERROR;
    if (device->hub_interface.max_packet < bitmap_bytes) return USB_UNSUPPORTED;
    if ((device->speed == USB_HOST_HIGH_SPEED &&
         device->hub_interface.protocol != 0 && device->hub_interface.protocol != 2) ||
        (device->speed == USB_HOST_FULL_SPEED && device->hub_interface.protocol != 0))
        return USB_UNSUPPORTED;

    uint8_t descriptor[39] = {0};
    result = host_control(device, device->address, 0xa0, 6,
                          (uint16_t)(USB_HUB_DESCRIPTOR_TYPE << 8), 0,
                          descriptor, sizeof(descriptor), expected, &actual);
    if (result) return result;
    if (actual != expected || descriptor[0] != expected ||
        descriptor[1] != USB_HUB_DESCRIPTOR_TYPE || descriptor[2] != header[2] ||
        get_le16(descriptor + 3) != characteristics) return USB_PHASE_ERROR;

    clear_bytes(hub, sizeof(*hub));
    hub->initialized = true;
    hub->scan_all_ports = true;
    hub->interface_number = device->hub_interface.interface_number;
    hub->endpoint = device->hub_interface.endpoint;
    hub->interval = device->hub_interface.interval;
    hub->protocol = device->hub_interface.protocol;
    hub->port_count = header[2];
    hub->bitmap_bytes = bitmap_bytes;
    hub->power_good_ms = (uint16_t)descriptor[5] * 2u;
    hub->characteristics = characteristics;
    hub->multi_tt = device->speed == USB_HOST_HIGH_SPEED && hub->protocol == 2;
    if (device->speed == USB_HOST_HIGH_SPEED)
        hub->tt_think_time_ns = (uint16_t)((((characteristics >> 5) & 3u) + 1u) * 666u);
    uint16_t wait_ms = hub->power_good_ms;
    if (power_mode != USB_HUB_POWER_NO_SWITCHING && !wait_ms) wait_ms = 2;

    for (uint8_t port = 1; port <= hub->port_count; ++port) {
        struct usb_host_hub_port *state = &hub->ports[port - 1];
        if (power_mode == USB_HUB_POWER_NO_SWITCHING) {
            state->powered = true;
            continue;
        }
        result = hub_set_port_power(device, hub, port, true);
        if (result) {
            hub->faulted = true;
            return result;
        }
    }
    uint32_t power_complete_ms = device->ops.now_ms(device->ops.context);
    hub->power_ready_ms = power_complete_ms + wait_ms;
    return USB_OK;
}

static int hub_begin_reset(struct usb_host_device *device, struct usb_host_hub *hub,
                           uint8_t port)
{
    struct usb_host_hub_port *state = &hub->ports[port - 1];
    int result = hub_feature(device, 0x23, 3, USB_HUB_PORT_FEATURE_RESET, port);
    if (result) return result;
    uint32_t now_ms = device->ops.now_ms(device->ops.context);
    state->reset_pending = true;
    state->reset_ready_ms = now_ms + USB_HUB_RESET_MIN_MS;
    state->reset_deadline_ms = now_ms + USB_HUB_RESET_TIMEOUT_MS;
    return USB_AGAIN;
}

static int hub_process_port(struct usb_host_device *device, struct usb_host_hub *hub,
                            uint8_t port,
                            usb_host_hub_event callback, void *event_context)
{
    struct usb_host_hub_port *state = &hub->ports[port - 1];
    uint16_t status = 0, change = 0;
    int result = hub_get_port_status(device, port, &status, &change);
    if (result) return result;

    if (status & USB_HUB_PORT_OVERCURRENT) {
        if (!state->overcurrent_latched) {
            enum usb_host_speed speed = state->speed;
            if (!state->attached) {
                result = hub_speed_from_status(status, &speed);
                if (result) speed = USB_HOST_FULL_SPEED;
            }
            result = hub_emit(device, hub, port, USB_HOST_HUB_OVERCURRENT,
                              speed, callback, event_context);
            if (result) return result;
            state->attached = false;
            state->reset_pending = false;
            state->attach_pending = false;
            state->overcurrent_latched = true;
        }
        if ((hub->characteristics & 3u) != USB_HUB_POWER_NO_SWITCHING) {
            result = hub_set_port_power(device, hub, port, false);
            if (result) return result;
        }
        result = hub_clear_port_changes(device, port, change);
        return result ? result : USB_IO;
    }

    if (!(status & USB_HUB_PORT_CONNECTION)) {
        if (state->attached || state->attach_pending) {
            result = hub_emit(device, hub, port, USB_HOST_HUB_DETACH,
                              state->speed, callback, event_context);
            if (result) return result;
            state->attached = false;
            state->attach_pending = false;
        }
        state->reset_pending = false;
        state->attach_pending = false;
        state->reset_failed = false;
        result = hub_clear_port_changes(device, port, change);
        return result;
    }

    if (state->overcurrent_latched || !state->powered) return USB_IO;
    if (change & USB_HUB_PORT_C_CONNECTION) {
        if (state->attached || state->attach_pending) {
            result = hub_emit(device, hub, port, USB_HOST_HUB_DETACH,
                              state->speed, callback, event_context);
            if (result) return result;
            state->attached = false;
            state->attach_pending = false;
        }
        state->reset_failed = false;
        state->reset_pending = false;
        state->attach_pending = false;
    }
    result = hub_clear_port_changes(device, port, change);
    if (result) return result;
    if (state->attached) return USB_OK;
    if (state->reset_failed) return USB_TIMEOUT;
    if (state->attach_pending) {
        result = hub_emit(device, hub, port, USB_HOST_HUB_ATTACH,
                          state->speed, callback, event_context);
        if (result) return result;
        state->attached = true;
        state->attach_pending = false;
        return USB_OK;
    }
    if (!state->reset_pending) return hub_begin_reset(device, hub, port);
    uint32_t now_ms = device->ops.now_ms(device->ops.context);
    if (!host_time_reached(now_ms, state->reset_ready_ms)) return USB_AGAIN;
    if (host_time_reached(now_ms, state->reset_deadline_ms)) {
        state->reset_pending = false;
        state->reset_failed = true;
        return USB_TIMEOUT;
    }
    if (status & USB_HUB_PORT_RESET) {
        return USB_AGAIN;
    }
    state->reset_pending = false;
    if (!(status & USB_HUB_PORT_ENABLE)) {
        state->reset_failed = true;
        return USB_IO;
    }
    enum usb_host_speed speed;
    result = hub_speed_from_status(status, &speed);
    if (result) {
        state->reset_failed = true;
        return result;
    }
    state->speed = speed;
    state->attach_pending = true;
    result = hub_emit(device, hub, port, USB_HOST_HUB_ATTACH,
                      speed, callback, event_context);
    if (result) return result;
    state->attached = true;
    state->attach_pending = false;
    return USB_OK;
}

int usb_host_hub_poll(struct usb_host_device *device, struct usb_host_hub *hub,
                      uint32_t timeout_ms,
                      usb_host_hub_event callback, void *event_context)
{
    if (!device || !hub || !hub->initialized || device->state != USB_HOST_CONFIGURED ||
        device->address == 0 || !device->ops.now_ms || !timeout_ms || timeout_ms > 10000)
        return USB_INVALID;
    if (hub->faulted) return USB_IO;
    uint32_t now_ms = device->ops.now_ms(device->ops.context);
    if (!host_time_reached(now_ms, hub->power_ready_ms)) return USB_AGAIN;

    uint8_t bitmap[USB_HOST_HUB_MAX_BITMAP_BYTES] = {0};
    int interrupt_result = USB_OK;
    if (!hub->scan_all_ports) {
        size_t actual = 0;
        interrupt_result = device->ops.interrupt(device->ops.context, &device->route,
                                                  device->address, hub->endpoint,
                                                  bitmap, hub->bitmap_bytes,
                                                  &actual, timeout_ms);
        if (interrupt_result && interrupt_result != USB_AGAIN &&
            interrupt_result != USB_TIMEOUT) return interrupt_result;
        if (!interrupt_result && actual != hub->bitmap_bytes)
            return USB_PHASE_ERROR;
    }
    bool hub_status_changed = hub->scan_all_ports ||
        (!interrupt_result && (bitmap[0] & 1u) != 0);
    if (hub_status_changed) {
        uint16_t status = 0, change = 0;
        int result = hub_get_status(device, &status, &change);
        if (result) return result;
        result = hub_clear_status_changes(device, change);
        if (result) return result;
        if (status & USB_HUB_HUB_C_OVERCURRENT) {
            hub->faulted = true;
            return USB_IO;
        }
    }

    int first_error = USB_OK;
    bool pending_reset = false;
    for (uint8_t port = 1; port <= hub->port_count; ++port) {
        struct usb_host_hub_port *state = &hub->ports[port - 1];
        bool changed = hub->scan_all_ports ||
            (!interrupt_result && (bitmap[port / 8u] & (uint8_t)(1u << (port % 8u))) != 0);
        if (!changed && !state->reset_pending && !state->attach_pending) continue;
        int result = hub_process_port(device, hub, port, callback, event_context);
        if (result == USB_AGAIN) pending_reset = true;
        else if (result && !first_error) first_error = result;
    }
    hub->scan_all_ports = false;
    if (first_error) return first_error;
    if (pending_reset) return USB_AGAIN;
    if (interrupt_result) return interrupt_result;
    return USB_OK;
}

int usb_host_hub_stop(struct usb_host_device *device, struct usb_host_hub *hub,
                      usb_host_hub_event callback, void *event_context)
{
    if (!device || !hub) return USB_INVALID;
    if (!hub->initialized) return USB_OK;
    if (device->state != USB_HOST_CONFIGURED || !device->address) return USB_DISCONNECTED;
    for (uint8_t port = 1; port <= hub->port_count; ++port) {
        struct usb_host_hub_port *state = &hub->ports[port - 1];
        if (state->attached || state->attach_pending) {
            int result = hub_emit(device, hub, port, USB_HOST_HUB_DETACH,
                                  state->speed, callback, event_context);
            if (result) return result;
            state->attached = false;
            state->attach_pending = false;
        }
        state->attach_pending = false;
        if (state->powered && (hub->characteristics & 3u) != USB_HUB_POWER_NO_SWITCHING) {
            int result = hub_set_port_power(device, hub, port, false);
            if (result) return result;
        }
        state->reset_pending = false;
    }
    clear_bytes(hub, sizeof(*hub));
    return USB_OK;
}
