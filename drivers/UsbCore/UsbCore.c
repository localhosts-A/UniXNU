/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbCore.h"

static void zero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static bool power2(size_t v) { return v && !(v & (v - 1)); }

int usb_mmio_read(const struct usb_mmio *m, uint32_t offset, uint32_t *value)
{
    if (!m || !value || !m->read32 || (offset & 3) || m->size < 4 || offset > m->size - 4) return USB_RANGE;
    return m->read32(m->context, offset, value);
}
int usb_mmio_write(const struct usb_mmio *m, uint32_t offset, uint32_t value)
{
    if (!m || !m->write32 || !m->barrier || (offset & 3) || m->size < 4 || offset > m->size - 4) return USB_RANGE;
    m->barrier(m->context);
    int result = m->write32(m->context, offset, value);
    m->barrier(m->context);
    return result;
}
int usb_mmio_poll(const struct usb_mmio *m, uint32_t offset, uint32_t mask, uint32_t value,
                  uint32_t timeout, uint32_t interval)
{
    if (!m || !m->delay_us || !interval || timeout > 1000000 || (value & ~mask)) return USB_INVALID;
    uint32_t elapsed = 0;
    for (;;) {
        uint32_t current;
        int result = usb_mmio_read(m, offset, &current);
        if (result) return result;
        if ((current & mask) == value) return USB_OK;
        if (elapsed == timeout) return USB_TIMEOUT;
        uint32_t delay = interval < timeout - elapsed ? interval : timeout - elapsed;
        m->delay_us(m->context, delay);
        elapsed += delay;
    }
}

static bool valid_dma(const struct usb_dma_buffer *b, size_t size, size_t alignment)
{
    return b->cpu && b->size >= size && size && size <= UINT32_MAX &&
        !((uintptr_t)b->cpu & (alignment - 1)) && !(b->address & (alignment - 1)) &&
        b->address <= UINT32_MAX && size - 1 <= UINT32_MAX - b->address;
}
int usb_dma_alloc32(const struct usb_dma_ops *ops, size_t size, size_t alignment, struct usb_dma_buffer *out)
{
    if (!out) return USB_INVALID;
    zero(out, sizeof(*out));
    if (!ops || !ops->qualified || !ops->allocate || !ops->release || !ops->sync) return USB_UNSUPPORTED;
    if (!size || size > UINT32_MAX || !power2(alignment) || alignment > UINT32_MAX) return USB_RANGE;
    int result = ops->allocate(ops->context, size, alignment, UINT32_MAX, out);
    if (result || !valid_dma(out, size, alignment)) {
        if (out->cpu) ops->release(ops->context, out);
        zero(out, sizeof(*out));
        return result ? result : USB_RANGE;
    }
    /* Expose only the requested extent, even if the backend reserved more. */
    out->size = size;
    zero(out->cpu, size);
    return USB_OK;
}
int usb_dma_sync(const struct usb_dma_ops *ops, struct usb_dma_buffer *b, size_t offset,
                 size_t size, enum usb_dma_direction direction, bool for_device)
{
    if (!ops || !ops->qualified || !ops->sync || !b || !b->cpu || !b->size ||
        b->address > UINT32_MAX || b->size - 1 > UINT32_MAX - b->address ||
        offset > b->size || size > b->size - offset || direction > USB_DMA_BIDIRECTIONAL || direction < 0) return USB_RANGE;
    return size ? ops->sync(ops->context, b, offset, size, direction, for_device) : USB_OK;
}
void usb_dma_free(const struct usb_dma_ops *ops, struct usb_dma_buffer *b)
{
    if (b && b->cpu && (!ops || !ops->release)) return;
    if (ops && ops->release && b && b->cpu) ops->release(ops->context, b);
    if (b) zero(b, sizeof(*b));
}

int usb_parse_configuration(const uint8_t *bytes, size_t length, struct usb_configuration *out)
{
    if (!bytes || !out || length < 9 || bytes[0] != 9 || bytes[1] != 2 || !bytes[5] ||
        le16(bytes + 2) < 9 || le16(bytes + 2) > length || bytes[4] > USB_MAX_INTERFACES) return USB_INVALID;
    struct usb_configuration value;
    zero(&value, sizeof(value));
    value.value = bytes[5];
    uint16_t used_in = 0, used_out = 0;
    int active = -1;
    bool seen_interface = false;
    size_t total = le16(bytes + 2);
    for (size_t at = 9; at < total;) {
        if (total - at < 2 || bytes[at] < 2 || bytes[at] > total - at) return USB_INVALID;
        uint8_t n = bytes[at], kind = bytes[at + 1];
        if (kind == 4) {
            if (n < 9) return USB_INVALID;
            seen_interface = true;
            active = -1;
            if (bytes[at + 3] == 0) {
                if (value.interface_count == USB_MAX_INTERFACES) return USB_RANGE;
                for (uint8_t i = 0; i < value.interface_count; ++i)
                    if (value.interfaces[i].number == bytes[at + 2]) return USB_INVALID;
                active = value.interface_count++;
                struct usb_interface *iface = &value.interfaces[active];
                iface->number = bytes[at + 2]; iface->klass = bytes[at + 5];
                iface->subclass = bytes[at + 6]; iface->protocol = bytes[at + 7];
                iface->endpoint_count = bytes[at + 4];
            }
        } else if (kind == 5) {
            if (n < 7 || !seen_interface) return USB_INVALID;
            if (active >= 0) {
                uint8_t address = bytes[at + 2], number = address & 15, type = bytes[at + 3] & 3;
                uint16_t encoded_packet = le16(bytes + at + 4);
                uint16_t packet = encoded_packet & 0x7ff;
                uint8_t additional_transactions = (uint8_t)((encoded_packet >> 11) & 3);
                if (!number || (address & 0x70) || !type || !packet || packet > 1024 || (encoded_packet & 0xe000) ||
                    additional_transactions == 3 || (type == 2 && (additional_transactions || packet > 512)) ||
                    (type == 1 && (!bytes[at + 6] || bytes[at + 6] > 16)) ||
                    (type == 3 && !bytes[at + 6]) ||
                    value.endpoint_count == USB_MAX_ENDPOINTS) return USB_INVALID;
                uint16_t *used = address & 0x80 ? &used_in : &used_out;
                if (*used & (1u << number)) return USB_INVALID;
                *used |= (uint16_t)(1u << number);
                struct usb_endpoint *ep = &value.endpoints[value.endpoint_count++];
                ep->address = address; ep->type = type; ep->max_packet = packet;
                ep->interval = bytes[at + 6]; ep->interface_number = value.interfaces[active].number;
                ep->transactions = (uint8_t)(additional_transactions + 1);
            }
        } else if (kind == 2) return USB_INVALID;
        at += n;
    }
    if (value.interface_count != bytes[4]) return USB_INVALID;
    for (uint8_t i = 0; i < value.interface_count; ++i) {
        uint8_t count = 0;
        for (uint8_t j = 0; j < value.endpoint_count; ++j)
            if (value.endpoints[j].interface_number == value.interfaces[i].number) ++count;
        if (count != value.interfaces[i].endpoint_count) return USB_INVALID;
    }
    *out = value;
    return USB_OK;
}

void usb_log_init(struct usb_log_ring *ring) { if (ring) zero(ring, sizeof(*ring)); }
size_t usb_log_write(struct usb_log_ring *ring, const void *bytes, size_t length)
{
    if (!ring || (!bytes && length)) return 0;
    if (__atomic_test_and_set(&ring->locked, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&ring->dropped, length, __ATOMIC_RELAXED);
        return 0;
    }
    const uint8_t *input = bytes;
    size_t count = length < USB_LOG_BYTES - ring->count ? length : USB_LOG_BYTES - ring->count;
    for (size_t i = 0; i < count; ++i) {
        ring->data[ring->write] = input[i];
        ring->write = (ring->write + 1) % USB_LOG_BYTES;
    }
    ring->count += count;
    __atomic_fetch_add(&ring->dropped, length - count, __ATOMIC_RELAXED);
    __atomic_clear(&ring->locked, __ATOMIC_RELEASE);
    return count;
}
size_t usb_log_take(struct usb_log_ring *ring, void *bytes, size_t length)
{
    if (!ring || (!bytes && length) || __atomic_test_and_set(&ring->locked, __ATOMIC_ACQUIRE)) return 0;
    uint8_t *output = bytes;
    size_t count = length < ring->count ? length : ring->count;
    for (size_t i = 0; i < count; ++i) {
        output[i] = ring->data[ring->read]; ring->read = (ring->read + 1) % USB_LOG_BYTES;
    }
    ring->count -= count;
    __atomic_clear(&ring->locked, __ATOMIC_RELEASE);
    return count;
}
uint64_t usb_log_dropped(const struct usb_log_ring *ring)
{ return ring ? __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED) : 0; }

int usb_setup_decode(const void *data, size_t size, struct usb_setup *out)
{
    if (!data || !out || size != 8) return USB_INVALID;
    const uint8_t *p = data;
    out->type = p[0]; out->request = p[1]; out->value = le16(p + 2);
    out->index = le16(p + 4); out->length = le16(p + 6);
    return USB_OK;
}
int usb_cdc_init(struct usb_cdc *cdc, uint16_t vendor, uint16_t product, uint16_t revision,
                 bool high_speed, const char *serial)
{
    if (!cdc || !vendor || !product || !serial) return USB_INVALID;
    size_t length = 0;
    while (length < 33 && serial[length]) ++length;
    if (!length || length > 32) return USB_INVALID;
    for (size_t i = 0; i < length; ++i) if ((uint8_t)serial[i] < 32 || (uint8_t)serial[i] > 126) return USB_INVALID;
    zero(cdc, sizeof(*cdc));
    cdc->vendor = vendor; cdc->product = product; cdc->revision = revision;
    copy(cdc->serial, serial, length);
    usb_cdc_bus_reset(cdc, high_speed);
    return USB_OK;
}
void usb_cdc_bus_reset(struct usb_cdc *cdc, bool high_speed)
{
    if (!cdc) return;
    cdc->high_speed = high_speed; cdc->address = 0; cdc->configuration = 0; cdc->control_lines = 0;
    cdc->halted_in = cdc->halted_out = 0;
    const uint8_t coding[7] = {0x00, 0xc2, 0x01, 0x00, 0, 0, 8};
    copy(cdc->line_coding, coding, sizeof(coding));
}
static size_t configuration(uint8_t *out, bool high, uint8_t kind)
{
    const uint8_t config[75] = {
        9,2,75,0,2,1,0,0xc0,1,
        8,11,0,2,2,2,1,0,
        9,4,0,0,1,2,2,1,0,
        5,0x24,0,0x10,1, 5,0x24,1,0,1, 4,0x24,2,2, 5,0x24,6,0,1,
        7,5,0x81,3,16,0,9,
        9,4,1,0,2,0x0a,0,0,0,
        7,5,0x02,2,0,2,0, 7,5,0x82,2,0,2,0
    };
    copy(out, config, sizeof(config)); out[1] = kind;
    put16(out + 65, high ? 512 : 64); put16(out + 72, high ? 512 : 64);
    return sizeof(config);
}
static size_t string_descriptor(uint8_t *out, const char *string)
{
    size_t n = 0;
    while (string[n] && n < 63) { out[2 + 2 * n] = (uint8_t)string[n]; out[3 + 2 * n] = 0; ++n; }
    out[0] = (uint8_t)(2 + 2 * n); out[1] = 3; return out[0];
}
static bool cdc_endpoint(const struct usb_cdc *cdc, uint16_t endpoint)
{
    return endpoint == 0 || endpoint == 0x80 || (cdc->configuration &&
        (endpoint == 0x81 || endpoint == 0x02 || endpoint == 0x82));
}
int usb_cdc_control(struct usb_cdc *cdc, const struct usb_setup *s, const void *out_data,
                    size_t out_size, void *buffer, size_t capacity, struct usb_control_reply *reply)
{
    if (!cdc || !s || !reply || (!buffer && capacity) || (!out_data && out_size)) return USB_INVALID;
    zero(reply, sizeof(*reply));
    if (((s->type & 0x80) && out_size) || (!(s->type & 0x80) && s->length != out_size)) return USB_INVALID;
    uint8_t bytes[128];
    size_t count = 0;
    if (s->type == 0x80 && s->request == 6) {
        uint8_t kind = (uint8_t)(s->value >> 8), index = (uint8_t)s->value;
        if (kind != 3 && (s->index || index)) return USB_STALL;
        if (kind == 1) {
            const uint8_t device[18] = {18,1,0,2,0xef,2,1,64,0,0,0,0,0,0,1,2,3,1};
            copy(bytes, device, sizeof(device));
            put16(bytes + 8, cdc->vendor); put16(bytes + 10, cdc->product); put16(bytes + 12, cdc->revision);
            count = sizeof(device);
        } else if (kind == 2 || kind == 7) count = configuration(bytes, kind == 2 ? cdc->high_speed : !cdc->high_speed, kind);
        else if (kind == 6) {
            const uint8_t qualifier[10] = {10,6,0,2,0xef,2,1,64,1,0};
            copy(bytes, qualifier, sizeof(qualifier)); count = sizeof(qualifier);
        } else if (kind == 3) {
            if (index && s->index != 0x0409) return USB_STALL;
            if (!index) { const uint8_t language[4] = {4,3,9,4}; copy(bytes, language, 4); count = 4; }
            else if (index == 1) count = string_descriptor(bytes, "UniXNU");
            else if (index == 2) count = string_descriptor(bytes, "Dali USB diagnostics");
            else if (index == 3) count = string_descriptor(bytes, cdc->serial);
            else return USB_STALL;
        } else return USB_STALL;
    } else if (s->type == 0 && s->request == 5 && !s->index && !s->length && s->value <= 127 && !cdc->configuration) {
        reply->effect = USB_CONTROL_ADDRESS; reply->argument = (uint8_t)s->value;
    } else if (s->type == 0 && s->request == 9 && !s->index && !s->length && s->value <= 1 && cdc->address) {
        reply->effect = USB_CONTROL_CONFIGURATION; reply->argument = (uint8_t)s->value;
    } else if (s->type == 0x80 && s->request == 8 && !s->value && !s->index && s->length == 1) {
        bytes[0] = cdc->configuration; count = 1;
    } else if (s->type == 0x80 && s->request == 0 && !s->value && !s->index && s->length == 2) {
        bytes[0] = 1; bytes[1] = 0; count = 2;
    } else if (s->type == 0x81 && s->request == 0 && !s->value && s->index <= 1 && s->length == 2 && cdc->configuration) {
        bytes[0] = bytes[1] = 0; count = 2;
    } else if (s->type == 0x82 && s->request == 0 && !s->value && s->length == 2 && cdc_endpoint(cdc, s->index)) {
        uint16_t halted = s->index & 0x80 ? cdc->halted_in : cdc->halted_out;
        bytes[0] = (uint8_t)((halted >> (s->index & 15)) & 1); bytes[1] = 0; count = 2;
    } else if (s->type == 2 && (s->request == 1 || s->request == 3) && !s->value && !s->length &&
               cdc->configuration && (s->index & 15) && cdc_endpoint(cdc, s->index)) {
        reply->effect = s->request == 1 ? USB_CONTROL_CLEAR_HALT : USB_CONTROL_SET_HALT;
        reply->argument = (uint8_t)s->index;
    } else if (s->type == 0xa1 && s->request == 0x21 && !s->value && !s->index && s->length == 7 && cdc->configuration) {
        copy(bytes, cdc->line_coding, 7); count = 7;
    } else if (s->type == 0x21 && s->request == 0x20 && !s->value && !s->index && s->length == 7 && cdc->configuration) {
        if (out_size != 7) return USB_STALL;
        const uint8_t *p = out_data;
        if (!le32(p) || p[4] > 2 || p[5] > 4 || !((p[6] >= 5 && p[6] <= 8) || p[6] == 16)) return USB_STALL;
        reply->effect = USB_CONTROL_LINE_CODING;
        copy(reply->line_coding, p, 7);
    } else if (s->type == 0x21 && s->request == 0x22 && !s->index && !s->length && !(s->value & ~3u) && cdc->configuration) {
        reply->effect = USB_CONTROL_LINES; reply->argument = (uint8_t)s->value;
    } else if (s->type == 0x21 && s->request == 0x23 && !s->index && !s->length && cdc->configuration) {
        /* Logging has no UART break line; an ACM SEND_BREAK is acknowledged. */
    } else if (s->type == 0x81 && s->request == 10 && !s->value && s->index <= 1 && s->length == 1 && cdc->configuration) {
        bytes[0] = 0; count = 1;
    } else if (s->type == 1 && s->request == 11 && !s->value && s->index <= 1 && !s->length && cdc->configuration) {
        /* Both interfaces expose alternate setting zero only. */
    } else return USB_STALL;
    if (count > s->length) count = s->length;
    if (count > capacity) return USB_RANGE;
    if (count) copy(buffer, bytes, count);
    reply->length = count;
    return USB_OK;
}
void usb_cdc_commit_control(struct usb_cdc *cdc, const struct usb_control_reply *reply)
{
    if (!cdc || !reply) return;
    if (reply->effect == USB_CONTROL_ADDRESS) cdc->address = reply->argument;
    else if (reply->effect == USB_CONTROL_CONFIGURATION) {
        cdc->configuration = reply->argument;
        cdc->control_lines = 0; cdc->halted_in = cdc->halted_out = 0;
    } else if (reply->effect == USB_CONTROL_LINE_CODING) {
        copy(cdc->line_coding, reply->line_coding, 7);
    } else if (reply->effect == USB_CONTROL_LINES) cdc->control_lines = reply->argument;
    else if (reply->effect == USB_CONTROL_CLEAR_HALT || reply->effect == USB_CONTROL_SET_HALT) {
        uint16_t *halted = reply->argument & 0x80 ? &cdc->halted_in : &cdc->halted_out;
        uint16_t mask = (uint16_t)(1u << (reply->argument & 15));
        if (reply->effect == USB_CONTROL_CLEAR_HALT) *halted &= (uint16_t)~mask;
        else *halted |= mask;
    }
}

static bool contains(const uint8_t keys[6], uint8_t usage)
{ for (size_t i = 0; i < 6; ++i) if (keys[i] == usage) return true; return false; }
int usb_keyboard_report(struct usb_keyboard *state, const uint8_t *report, size_t size, usb_key_event event, void *ctx)
{
    if (!state || !report || !event || size != 8 || report[1]) return USB_INVALID;
    for (size_t i = 2; i < 8; ++i) {
        if (report[i] && report[i] <= 3) return USB_AGAIN;
        for (size_t j = 2; report[i] && j < i; ++j) if (report[j] == report[i]) return USB_INVALID;
    }
    /* Releases precede presses to avoid retaining modifiers/keys on transitions. */
    for (size_t i = 0; i < 6; ++i) if (state->keys[i] && !contains(report + 2, state->keys[i])) event(ctx, state->keys[i], false);
    for (unsigned i = 0; i < 8; ++i) if ((state->modifiers & (1u << i)) && !(report[0] & (1u << i))) event(ctx, (uint16_t)(0xe0 + i), false);
    for (unsigned i = 0; i < 8; ++i) if (!(state->modifiers & (1u << i)) && (report[0] & (1u << i))) event(ctx, (uint16_t)(0xe0 + i), true);
    for (size_t i = 2; i < 8; ++i) if (report[i] && !contains(state->keys, report[i])) event(ctx, report[i], true);
    state->modifiers = report[0]; copy(state->keys, report + 2, 6);
    return USB_OK;
}
void usb_keyboard_disconnect(struct usb_keyboard *state, usb_key_event event, void *ctx)
{ const uint8_t empty[8] = {0}; (void)usb_keyboard_report(state, empty, sizeof(empty), event, ctx); }
int usb_mouse_report(struct usb_mouse *state, const uint8_t *report, size_t size, usb_pointer_event event, void *ctx)
{
    if (!state || !report || !event || (size != 3 && size != 4) || (report[0] & 0xe0)) return USB_INVALID;
    state->buttons = report[0];
    event(ctx, state->buttons, (int8_t)report[1], (int8_t)report[2], size == 4 ? (int8_t)report[3] : 0);
    return USB_OK;
}
void usb_mouse_disconnect(struct usb_mouse *state, usb_pointer_event event, void *ctx)
{ if (state && event) { state->buttons = 0; event(ctx, 0, 0, 0, 0); } }

int usb_bot_init(struct usb_bot *bot, const struct usb_transport *transport, uint8_t iface,
                 uint8_t in, uint8_t out, uint8_t lun)
{
    if (!bot || !transport || !transport->bulk || !transport->control || !transport->clear_halt ||
        !(in & 0x80) || (out & 0x80) || (in & 0x70) || (out & 0x70) || !(in & 15) || !(out & 15) || lun > 15) return USB_INVALID;
    zero(bot, sizeof(*bot)); bot->transport = *transport;
    bot->interface_number = iface; bot->in_endpoint = in; bot->out_endpoint = out; bot->lun = lun; bot->present = true;
    return USB_OK;
}
static int bot_connection(struct usb_bot *bot, int result)
{
    if (result == USB_DISCONNECTED) usb_bot_detach(bot);
    return result;
}
int usb_bot_reset(struct usb_bot *bot)
{
    if (!bot || !bot->present) return USB_DISCONNECTED;
    bot->needs_reset = true;
    struct usb_setup reset = {0x21, 0xff, 0, bot->interface_number, 0};
    size_t actual = 0;
    int result = bot->transport.control(bot->transport.context, &reset, 0, 0, &actual, 1000);
    if (result) return bot_connection(bot, result);
    if (actual) return USB_PHASE_ERROR;
    result = bot->transport.clear_halt(bot->transport.context, bot->in_endpoint);
    if (result) return bot_connection(bot, result);
    result = bot->transport.clear_halt(bot->transport.context, bot->out_endpoint);
    if (!result) bot->needs_reset = false;
    return bot_connection(bot, result);
}
static int bot_failure(struct usb_bot *bot, int failure)
{
    if (failure == USB_DISCONNECTED) usb_bot_detach(bot);
    else (void)usb_bot_reset(bot);
    return failure;
}
int usb_bot_command(struct usb_bot *bot, const uint8_t *cdb, size_t cdb_size,
                    bool input, void *data, size_t size, size_t *completed)
{
    if (completed) *completed = 0;
    if (!bot || !cdb || !completed || !cdb_size || cdb_size > 16 || size > UINT32_MAX || (!data && size)) return USB_INVALID;
    if (!bot->present) return USB_DISCONNECTED;
    if (bot->needs_reset) return USB_PHASE_ERROR;
    uint8_t cbw[31] = {0}, csw[13] = {0};
    if (!++bot->tag) ++bot->tag;
    put32(cbw, 0x43425355); put32(cbw + 4, bot->tag); put32(cbw + 8, (uint32_t)size);
    cbw[12] = input ? 0x80 : 0; cbw[13] = bot->lun; cbw[14] = (uint8_t)cdb_size; copy(cbw + 15, cdb, cdb_size);
    size_t actual = 0;
    int result = bot->transport.bulk(bot->transport.context, bot->out_endpoint, cbw, sizeof(cbw), &actual, 1000);
    if (result || actual != sizeof(cbw)) return bot_failure(bot, result ? result : USB_PHASE_ERROR);
    size_t transferred = 0;
    if (size) {
        result = bot->transport.bulk(bot->transport.context, input ? bot->in_endpoint : bot->out_endpoint, data, size, &transferred, 10000);
        if (!input && (result || transferred != size)) { bot->write_uncertain = true; return bot_failure(bot, result ? result : USB_PHASE_ERROR); }
        if (transferred > size) return bot_failure(bot, USB_PHASE_ERROR);
        if (result == USB_STALL && input) {
            result = bot->transport.clear_halt(bot->transport.context, bot->in_endpoint);
            if (result) return bot_failure(bot, result);
        } else if (result) return bot_failure(bot, result);
    }
    actual = 0;
    result = bot->transport.bulk(bot->transport.context, bot->in_endpoint, csw, sizeof(csw), &actual, 1000);
    if (result == USB_STALL) {
        result = bot->transport.clear_halt(bot->transport.context, bot->in_endpoint);
        if (!result) { actual = 0; result = bot->transport.bulk(bot->transport.context, bot->in_endpoint, csw, sizeof(csw), &actual, 1000); }
    }
    if (result || actual != sizeof(csw) || le32(csw) != 0x53425355 || le32(csw + 4) != bot->tag ||
        le32(csw + 8) > size || csw[12] > 2 || csw[12] == 2 || size - le32(csw + 8) != transferred) {
        if (!input && size) bot->write_uncertain = true;
        return bot_failure(bot, result ? result : USB_PHASE_ERROR);
    }
    *completed = transferred;
    if (!input && size && csw[12]) bot->write_uncertain = true;
    return csw[12] ? USB_COMMAND_FAILED : USB_OK;
}
int usb_bot_capacity(struct usb_bot *bot)
{
    if (!bot) return USB_INVALID;
    bot->blocks = 0; bot->block_size = 0;
    uint8_t command[16] = {0x25}, bytes[32] = {0};
    size_t actual;
    int result = usb_bot_command(bot, command, 10, true, bytes, 8, &actual);
    if (result) return result;
    if (actual != 8) return USB_PHASE_ERROR;
    uint64_t last = be32(bytes);
    uint32_t size = be32(bytes + 4);
    if (last == UINT32_MAX) {
        zero(command, sizeof(command)); command[0] = 0x9e; command[1] = 0x10; command[13] = 32;
        result = usb_bot_command(bot, command, 16, true, bytes, sizeof(bytes), &actual);
        if (result) return result;
        if (actual != sizeof(bytes)) return USB_PHASE_ERROR;
        last = ((uint64_t)be32(bytes) << 32) | be32(bytes + 4); size = be32(bytes + 8);
    }
    if (last == UINT64_MAX || size < 512 || size > 4096 || !power2(size) || last + 1 > UINT64_MAX / size) return USB_RANGE;
    bot->blocks = last + 1; bot->block_size = size;
    return USB_OK;
}
int usb_bot_read_only(struct usb_bot *bot, bool *read_only)
{
    if (!bot || !read_only || !bot->present) return USB_INVALID;
    uint8_t command[6] = {0x1a, 0, 0x3f, 0, 4, 0};
    uint8_t header[4] = {0};
    size_t actual = 0;
    int result = usb_bot_command(bot, command, sizeof(command), true,
                                 header, sizeof(header), &actual);
    if (result == USB_COMMAND_FAILED) {
        /* Some devices implement page 0 but reject the all-pages selector. */
        command[2] = 0;
        zero(header, sizeof(header));
        actual = 0;
        result = usb_bot_command(bot, command, sizeof(command), true,
                                 header, sizeof(header), &actual);
        if (result == USB_COMMAND_FAILED) {
            /* Match the conservative SCSI disk policy: failed WP probing is
             * treated as writable; actual writes still return BOT status.
             */
            *read_only = false;
            return USB_OK;
        }
    }
    if (result) return result;
    if (actual < 3) return USB_PHASE_ERROR;
    *read_only = (header[2] & 0x80) != 0;
    return USB_OK;
}
static int bot_rw(struct usb_bot *bot, bool write, uint64_t lba, uint32_t blocks, void *data, size_t size)
{
    if (!bot || !bot->present) return USB_DISCONNECTED;
    if (!bot->blocks || !bot->block_size || !blocks || lba >= bot->blocks || blocks > bot->blocks - lba ||
        blocks > SIZE_MAX / bot->block_size || size != (size_t)blocks * bot->block_size || size > UINT32_MAX || !data) return USB_RANGE;
    uint8_t command[16] = {0}; size_t command_size;
    if (lba <= UINT32_MAX && blocks <= UINT16_MAX && blocks - 1 <= UINT32_MAX - lba) {
        command[0] = write ? 0x2a : 0x28; put_be32(command + 2, (uint32_t)lba);
        command[7] = (uint8_t)(blocks >> 8); command[8] = (uint8_t)blocks; command_size = 10;
    } else {
        command[0] = write ? 0x8a : 0x88;
        put_be32(command + 2, (uint32_t)(lba >> 32)); put_be32(command + 6, (uint32_t)lba);
        put_be32(command + 10, blocks); command_size = 16;
    }
    size_t actual;
    int result = usb_bot_command(bot, command, command_size, !write, data, size, &actual);
    if (write && (result || actual != size)) bot->write_uncertain = true;
    return result ? result : actual == size ? USB_OK : USB_PHASE_ERROR;
}
int usb_bot_read(struct usb_bot *b, uint64_t lba, uint32_t blocks, void *data, size_t size)
{ return bot_rw(b, false, lba, blocks, data, size); }
int usb_bot_write(struct usb_bot *b, uint64_t lba, uint32_t blocks, const void *data, size_t size)
{ return bot_rw(b, true, lba, blocks, (void *)data, size); }
int usb_bot_sync(struct usb_bot *bot)
{
    const uint8_t command[10] = {0x35}; size_t actual;
    int result = usb_bot_command(bot, command, sizeof(command), false, 0, 0, &actual);
    /* A successful flush cannot undo uncertainty from an earlier partial write. */
    return result ? result : bot->write_uncertain ? USB_IO : USB_OK;
}
void usb_bot_detach(struct usb_bot *bot)
{ if (bot) { bot->present = false; bot->blocks = 0; bot->block_size = 0; } }

static bool valid_roles(const struct usb_role_ops *ops)
{
    return ops && ops->stop_submissions && ops->sync_storage && ops->cancel_requests &&
        ops->stop_dma && ops->vbus && ops->select_role && ops->start;
}
static int stop_role(struct usb_role_state *state, const struct usb_role_ops *ops)
{
    /* Even when sync fails, cancel and stop must run before power can change. */
    int first = ops->stop_submissions(ops->context);
    int result = ops->sync_storage(ops->context); if (!first) first = result;
    result = ops->cancel_requests(ops->context); if (!first) first = result;
    int stopped = ops->stop_dma(ops->context); if (!first) first = stopped;
    state->role = USB_ROLE_STOPPED;
    if (!stopped) { result = ops->vbus(ops->context, false); if (!first) first = result; }
    if (first) state->faulted = true;
    return first;
}
int usb_role_switch(struct usb_role_state *state, const struct usb_role_ops *ops, enum usb_role role)
{
    if (!state || !valid_roles(ops) || role < USB_ROLE_STOPPED || role > USB_ROLE_HOST) return USB_INVALID;
    if (state->switching) return USB_BUSY;
    if (state->faulted) return USB_IO;
    if (state->role == role) return USB_OK;
    state->switching = true;
    int result = USB_OK;
    if (state->role != USB_ROLE_STOPPED) result = stop_role(state, ops);
    if (!result && role != USB_ROLE_STOPPED) {
        result = ops->select_role(ops->context, role);
        if (!result) result = ops->vbus(ops->context, role == USB_ROLE_HOST);
        if (!result) result = ops->start(ops->context, role);
        if (result) {
            /* A failed start may already have enabled queues. Quiesce them. */
            (void)stop_role(state, ops); state->faulted = true;
        } else state->role = role;
    }
    state->switching = false;
    return result;
}
int usb_role_quiesce(struct usb_role_state *state, const struct usb_role_ops *ops)
{
    if (!state || !valid_roles(ops)) return USB_INVALID;
    if (state->switching) return USB_BUSY;
    if (state->role == USB_ROLE_STOPPED) return state->faulted ? USB_IO : USB_OK;
    state->switching = true;
    int result = stop_role(state, ops);
    state->switching = false;
    return result;
}
