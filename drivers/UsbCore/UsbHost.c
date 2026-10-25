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

static bool selected_interface(const struct usb_host_device *device, uint8_t number)
{
    for (uint8_t h = 0; h < device->hid_count; ++h)
        if (device->hid[h].interface_number == number) return true;
    return device->has_bot && device->bot.interface_number == number;
}

static bool selected_endpoint(const struct usb_host_device *device,
                              const struct usb_endpoint *endpoint)
{ return selected_interface(device, endpoint->interface_number); }

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
    if (device->address && device->ops.disable_device) {
        int stopped = device->ops.disable_device(device->ops.context, &device->route,
                                                 device->address);
        if (stopped) {
            device->state = USB_HOST_FAULTED;
            return stopped;
        }
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
        if (ops->disable_device(ops->context, route, address)) {
            device->address = address;
            device->state = USB_HOST_FAULTED;
            return USB_RANGE;
        }
        return abort_enumeration(device, USB_RANGE);
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

    actual = 0;
    result = host_control(device, address, 0, 9, device->configuration, 0,
                          NULL, 0, 0, &actual);
    if (result || actual) return abort_enumeration(device, result ? result : USB_PHASE_ERROR);

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

int usb_host_disconnect(struct usb_host_device *device, usb_key_event key_event,
                        usb_pointer_event pointer_event, void *event_context)
{
    if (!device) return USB_INVALID;
    if (device->state == USB_HOST_EMPTY) return USB_OK;
    if (!device->ops.disable_device ||
        (!device->address && device->state != USB_HOST_FAULTED)) return USB_IO;
    for (uint8_t h = 0; h < device->hid_count; ++h) {
        const struct usb_host_hid_interface *binding = &device->hid[h];
        if (binding->kind == USB_HOST_HID_KEYBOARD) {
            bool held = binding->state.keyboard.modifiers != 0;
            for (uint8_t k = 0; k < 6; ++k) held |= binding->state.keyboard.keys[k] != 0;
            if (held && !key_event) return USB_INVALID;
        } else if (binding->state.mouse.buttons && !pointer_event) return USB_INVALID;
    }
    int result = device->ops.disable_device(device->ops.context, &device->route,
                                            device->address);
    if (result) {
        device->state = USB_HOST_FAULTED;
        return result;
    }
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
