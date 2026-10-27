/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define _POSIX_C_SOURCE 200112L
#include "UsbCore.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void set32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void setbe32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * (3 - i))); }
static uint32_t getbe32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

struct registers { uint32_t value, reads, writes, barriers, delays; bool ready, fail; };
static int mmio_read(void *ctx, uint32_t offset, uint32_t *out) {
    struct registers *r = ctx; assert(offset == 0); ++r->reads;
    if (r->fail) return USB_IO;
    *out = r->ready && r->reads >= 3 ? r->value : 0; return USB_OK;
}
static int mmio_write(void *ctx, uint32_t offset, uint32_t value) {
    struct registers *r = ctx; assert(!offset); ++r->writes; r->value = value; return USB_OK;
}
static void barrier(void *ctx) { ++((struct registers *)ctx)->barriers; }
static void delay(void *ctx, uint32_t us) { ((struct registers *)ctx)->delays += us; }
static void test_mmio(void) {
    struct registers r = {.value = 0x81, .ready = true};
    struct usb_mmio m = {&r, 4, mmio_read, mmio_write, barrier, delay}; uint32_t value;
    assert(usb_mmio_poll(&m, 0, 0x81, 0x81, 10, 3) == USB_OK && r.delays == 6);
    assert(usb_mmio_write(&m, 0, 7) == USB_OK && r.barriers == 2 && r.writes == 1);
    assert(usb_mmio_read(&m, UINT32_MAX - 3, &value) == USB_RANGE);
    assert(usb_mmio_write(&m, 1, 4) == USB_RANGE && r.writes == 1);
    r.ready = false; r.delays = 0; r.reads = 0;
    assert(usb_mmio_poll(&m, 0, 1, 1, 10, 3) == USB_TIMEOUT && r.delays == 10 && r.reads == 5);
    assert(usb_mmio_poll(&m, 0, 1, 1, 0, 1) == USB_TIMEOUT);
    assert(usb_mmio_poll(&m, 0, 1, 2, 10, 1) == USB_INVALID);
    assert(usb_mmio_poll(&m, 0, 1, 1, 10, 0) == USB_INVALID);
    r.fail = true; assert(usb_mmio_poll(&m, 0, 1, 1, 10, 1) == USB_IO);
}

struct allocator { int allocated, freed, syncs; uint64_t address; };
static int allocate(void *ctx, size_t size, size_t alignment, uint64_t limit, struct usb_dma_buffer *b) {
    struct allocator *a = ctx; assert(limit == UINT32_MAX); ++a->allocated;
    if (posix_memalign(&b->cpu, alignment < sizeof(void *) ? sizeof(void *) : alignment, size)) return USB_NO_MEMORY;
    b->address = a->address; b->size = size; return USB_OK;
}
static void release(void *ctx, struct usb_dma_buffer *b) { ++((struct allocator *)ctx)->freed; free(b->cpu); }
static int sync_buffer(void *ctx, struct usb_dma_buffer *b, size_t offset, size_t n, enum usb_dma_direction d, bool device) {
    (void)d; (void)device; assert(offset + n <= b->size); ++((struct allocator *)ctx)->syncs; return USB_OK;
}
static void test_dma(void) {
    struct allocator a = {.address = 0x80001000};
    struct usb_dma_ops ops = {&a, false, allocate, release, sync_buffer}; struct usb_dma_buffer b;
    assert(usb_dma_alloc32(&ops, 4096, 64, &b) == USB_UNSUPPORTED && !a.allocated);
    ops.qualified = true;
    assert(usb_dma_alloc32(&ops, 4096, 3, &b) == USB_RANGE && !a.allocated);
    assert(usb_dma_alloc32(&ops, 4096, 64, &b) == USB_OK && !((uint8_t *)b.cpu)[2047]);
    assert(usb_dma_sync(&ops, &b, 0, 4096, USB_DMA_BIDIRECTIONAL, true) == USB_OK);
    assert(usb_dma_sync(&ops, &b, SIZE_MAX, 8, USB_DMA_TO_DEVICE, true) == USB_RANGE);
    assert(usb_dma_sync(&ops, &b, 4090, 7, USB_DMA_FROM_DEVICE, false) == USB_RANGE && a.syncs == 1);
    usb_dma_free(NULL, &b); assert(b.cpu && !a.freed);
    usb_dma_free(&ops, &b); assert(!b.cpu && a.freed == 1);
    a.address = 0xffffffc0;
    assert(usb_dma_alloc32(&ops, 128, 64, &b) == USB_RANGE && !b.cpu && a.freed == 2);
    a.address = 0x100000000ULL;
    assert(usb_dma_alloc32(&ops, 64, 64, &b) == USB_RANGE && a.freed == 3);
}

static int request(struct usb_cdc *cdc, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                   uint16_t length, const void *out, size_t out_size, void *bytes, size_t capacity,
                   struct usb_control_reply *reply) {
    struct usb_setup s = {type, req, value, index, length};
    return usb_cdc_control(cdc, &s, out, out_size, bytes, capacity, reply);
}
static void configure(struct usb_cdc *cdc) {
    struct usb_control_reply reply;
    assert(request(cdc, 0, 5, 7, 0, 0, NULL, 0, NULL, 0, &reply) == USB_OK);
    assert(!cdc->address && reply.effect == USB_CONTROL_ADDRESS);
    usb_cdc_commit_control(cdc, &reply); assert(cdc->address == 7);
    assert(request(cdc, 0, 9, 1, 0, 0, NULL, 0, NULL, 0, &reply) == USB_OK);
    assert(!cdc->configuration); usb_cdc_commit_control(cdc, &reply); assert(cdc->configuration == 1);
}
static void test_cdc(void) {
    struct usb_cdc cdc; uint8_t data[128], original[75]; struct usb_control_reply reply;
    assert(usb_cdc_init(&cdc, 0x1234, 0x5678, 0x100, true, "0123456789abcdef") == USB_OK);
    assert(request(&cdc, 0x80, 6, 0x100, 0, 8, NULL, 0, data, sizeof(data), &reply) == USB_OK && reply.length == 8 && data[7] == 64);
    assert(request(&cdc, 0x80, 6, 0x200, 0, 128, NULL, 0, data, sizeof(data), &reply) == USB_OK && reply.length == 75);
    memcpy(original, data, 75); struct usb_configuration config;
    assert(usb_parse_configuration(data, reply.length, &config) == USB_OK);
    assert(config.interface_count == 2 && config.endpoint_count == 3 && config.endpoints[1].max_packet == 512);
    data[61] = 0; assert(usb_parse_configuration(data, 75, &config) == USB_INVALID);
    memcpy(data, original, 75); data[70] = 0x02; assert(usb_parse_configuration(data, 75, &config) == USB_INVALID);
    memcpy(data, original, 75); data[63] = 0x80; assert(usb_parse_configuration(data, 75, &config) == USB_INVALID);
    memcpy(data, original, 75); data[2] = 76; assert(usb_parse_configuration(data, 75, &config) == USB_INVALID);
    for (size_t n = 0; n < 75; ++n) assert(usb_parse_configuration(original, n, &config) != USB_OK);
    uint32_t seed = 37;
    for (unsigned i = 0; i < 5000; ++i) {
        memcpy(data, original, 75); seed = seed * 1664525u + 1013904223u;
        data[seed % 75] ^= (uint8_t)(seed >> 24); (void)usb_parse_configuration(data, 75, &config);
    }
    assert(request(&cdc, 0, 9, 1, 0, 0, NULL, 0, NULL, 0, &reply) == USB_STALL);
    configure(&cdc);
    uint8_t coding[7] = {0,0xc2,1,0,0,0,8};
    assert(request(&cdc, 0x21, 0x20, 0, 0, 7, coding, 7, NULL, 0, &reply) == USB_OK);
    usb_cdc_commit_control(&cdc, &reply);
    assert(request(&cdc, 0xa1, 0x21, 0, 0, 7, NULL, 0, data, sizeof(data), &reply) == USB_OK && !memcmp(data, coding, 7));
    coding[6] = 9;
    assert(request(&cdc, 0x21, 0x20, 0, 0, 7, coding, 7, NULL, 0, &reply) == USB_STALL && cdc.line_coding[6] == 8);
    assert(request(&cdc, 0x21, 0x22, 3, 0, 0, coding, 1, NULL, 0, &reply) == USB_INVALID && !cdc.control_lines);
    assert(request(&cdc, 0x21, 0x22, 3, 0, 0, NULL, 0, NULL, 0, &reply) == USB_OK && !cdc.control_lines);
    usb_cdc_commit_control(&cdc, &reply); assert(cdc.control_lines == 3);
    assert(request(&cdc, 2, 3, 0, 0x82, 0, NULL, 0, NULL, 0, &reply) == USB_OK);
    usb_cdc_commit_control(&cdc, &reply);
    assert(request(&cdc, 0x82, 0, 0, 0x82, 2, NULL, 0, data, sizeof(data), &reply) == USB_OK && data[0] == 1);
    assert(request(&cdc, 2, 1, 0, 0x82, 0, NULL, 0, NULL, 0, &reply) == USB_OK);
    usb_cdc_commit_control(&cdc, &reply);
    assert(request(&cdc, 0x82, 0, 0, 0x82, 2, NULL, 0, data, sizeof(data), &reply) == USB_OK && !data[0]);
    assert(request(&cdc, 2, 3, 0, 0x80, 0, NULL, 0, NULL, 0, &reply) == USB_STALL);
    usb_cdc_bus_reset(&cdc, false); assert(!cdc.configuration && !cdc.control_lines && !cdc.address);
    assert(request(&cdc, 0x80, 6, 0x200, 0, 128, NULL, 0, data, sizeof(data), &reply) == USB_OK);
    assert(usb_parse_configuration(data, 75, &config) == USB_OK && config.endpoints[2].max_packet == 64);
    assert(request(&cdc, 0x80, 6, 0x700, 0, 128, NULL, 0, data, sizeof(data), &reply) == USB_OK);
    data[1] = 2; assert(usb_parse_configuration(data, 75, &config) == USB_OK && config.endpoints[2].max_packet == 512);
    uint8_t setup[8] = {0x21,0x22,3,0,1,0,0,0}; struct usb_setup s;
    assert(usb_setup_decode(setup, 8, &s) == USB_OK && s.index == 1 && s.value == 3);
    assert(usb_setup_decode(setup, 7, &s) == USB_INVALID);
    assert(usb_cdc_init(&cdc, 0, 1, 0, true, "x") == USB_INVALID);
    char unterminated[33]; memset(unterminated, 'a', sizeof(unterminated));
    assert(usb_cdc_init(&cdc, 1, 1, 0, true, unterminated) == USB_INVALID);

    const uint8_t periodic[] = {
        9,2,25,0,1,1,0,0x80,50,
        9,4,0,0,1,3,1,1,0,
        7,5,0x81,3,8,8,10 /* 8-byte payload, two transactions per HS microframe */
    };
    assert(usb_parse_configuration(periodic, sizeof(periodic), &config) == USB_OK);
    assert(config.endpoint_count == 1 && config.endpoints[0].max_packet == 8 &&
           config.endpoints[0].transactions == 2);
    uint8_t reserved_transaction_bits[sizeof(periodic)];
    memcpy(reserved_transaction_bits, periodic, sizeof(periodic));
    reserved_transaction_bits[23] = 0x18;
    assert(usb_parse_configuration(reserved_transaction_bits, sizeof(reserved_transaction_bits), &config) == USB_INVALID);
}

static void *producer(void *ctx) {
    struct usb_log_ring *ring = ctx; uint8_t bytes[17]; memset(bytes, 0x5a, sizeof(bytes));
    for (unsigned i = 0; i < 10000; ++i) (void)usb_log_write(ring, bytes, sizeof(bytes));
    return NULL;
}
static void test_ring(void) {
    struct usb_log_ring ring; usb_log_init(&ring); uint8_t data[USB_LOG_BYTES];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    assert(usb_log_write(&ring, data, sizeof(data)) == sizeof(data));
    assert(usb_log_write(&ring, data, 1) == 0 && usb_log_dropped(&ring) == 1);
    uint8_t out[128]; assert(usb_log_take(&ring, out, sizeof(out)) == sizeof(out) && !memcmp(data, out, sizeof(out)));
    assert(usb_log_write(&ring, out, sizeof(out)) == sizeof(out));
    assert(usb_log_take(&ring, data, sizeof(data)) == sizeof(data));
    assert(!memcmp(data + sizeof(data) - sizeof(out), out, sizeof(out)));
    usb_log_init(&ring); ring.locked = 1;
    assert(!usb_log_write(&ring, data, 7) && usb_log_dropped(&ring) == 7);
    assert(!usb_log_take(&ring, data, 7)); ring.locked = 0;
    usb_log_init(&ring); pthread_t a, b;
    assert(!pthread_create(&a, NULL, producer, &ring) && !pthread_create(&b, NULL, producer, &ring));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    size_t got = usb_log_take(&ring, data, sizeof(data));
    assert(got + usb_log_dropped(&ring) == 2u * 10000u * 17u);
}

struct input_events { unsigned n; uint16_t usage[32]; bool down[32]; int16_t x,y,w; uint8_t buttons; };
static void key(void *ctx, uint16_t usage, bool down) {
    struct input_events *e = ctx; assert(e->n < 32); e->usage[e->n] = usage; e->down[e->n++] = down;
}
static void mouse(void *ctx, uint8_t buttons, int16_t x, int16_t y, int16_t wheel) {
    struct input_events *e = ctx; ++e->n; e->buttons = buttons; e->x = x; e->y = y; e->w = wheel;
}
static void test_hid(void) {
    struct usb_keyboard keyboard = {0}; struct input_events events = {0};
    uint8_t report[8] = {2,0,4,5};
    assert(usb_keyboard_report(&keyboard, report, sizeof(report), key, &events) == USB_OK && events.n == 3);
    assert(events.usage[0] == 0xe1 && events.down[0] && events.usage[1] == 4);
    assert(usb_keyboard_report(&keyboard, report, sizeof(report), key, &events) == USB_OK && events.n == 3);
    report[2] = 1; assert(usb_keyboard_report(&keyboard, report, 8, key, &events) == USB_AGAIN && events.n == 3);
    report[2] = 5; assert(usb_keyboard_report(&keyboard, report, 8, key, &events) == USB_INVALID && events.n == 3);
    usb_keyboard_disconnect(&keyboard, key, &events); assert(events.n == 6 && !keyboard.modifiers);
    for (unsigned i = 3; i < 6; ++i) assert(!events.down[i]);
    struct usb_mouse state = {0}; uint8_t pointer[4] = {3,0xff,0x80,1};
    assert(usb_mouse_report(&state, pointer, sizeof(pointer), mouse, &events) == USB_OK);
    assert(events.buttons == 3 && events.x == -1 && events.y == -128 && events.w == 1);
    usb_mouse_disconnect(&state, mouse, &events); assert(!events.buttons && !state.buttons);
    assert(usb_mouse_report(&state, pointer, 2, mouse, &events) == USB_INVALID);
}

struct disk {
    uint8_t media[4096], cdb[16]; uint32_t tag, size, residue; uint64_t lba;
    unsigned phase, commands, writes, resets, clears, syncs;
    bool input, partial_write, data_in_stall, bad_tag, reset_fail, csw_stall, disconnected, capacity_bad;
    size_t data_in_stall_actual;
    bool read_only, reject_mode_all, reject_mode_page0, short_mode_sense;
    uint8_t status;
};
static int control(void *ctx, const struct usb_setup *setup, void *data, size_t size, size_t *actual, uint32_t timeout) {
    struct disk *d = ctx; (void)data; assert(!size && timeout && setup->type == 0x21 && setup->request == 0xff && setup->index == 2);
    ++d->resets; *actual = 0;
    if (d->disconnected) return USB_DISCONNECTED;
    if (d->reset_fail) return USB_TIMEOUT;
    d->phase = 0; return USB_OK;
}
static int clear(void *ctx, uint8_t ep) { struct disk *d = ctx; assert(ep == 0x81 || ep == 0x02); ++d->clears; return USB_OK; }
static int bulk(void *ctx, uint8_t ep, void *buffer, size_t size, size_t *actual, uint32_t timeout) {
    struct disk *d = ctx; uint8_t *p = buffer; *actual = 0; assert(timeout);
    if (d->disconnected) return USB_DISCONNECTED;
    if (!d->phase) {
        assert(ep == 2 && size == 31 && get32(p) == 0x43425355 && p[14] <= 16 && p[13] == 0);
        ++d->commands; d->tag = get32(p + 4); d->size = get32(p + 8); d->input = p[12] == 0x80;
        memcpy(d->cdb, p + 15, 16); d->residue = 0; d->status = 0; d->phase = d->size ? 1 : 2; *actual = size;
        if (d->cdb[0] == 0x35) ++d->syncs;
        return USB_OK;
    }
    if (d->phase == 1) {
        assert(size == d->size && ep == (d->input ? 0x81 : 2));
        if (d->cdb[0] == 0x25) { assert(size == 8); setbe32(p, 7); setbe32(p + 4, d->capacity_bad ? 768 : 512); }
        else if (d->cdb[0] == 0x1a) {
            assert(d->input && size == 4 && d->cdb[1] == 0 && d->cdb[3] == 0 &&
                   d->cdb[4] == 4 && d->cdb[5] == 0);
            bool reject = d->cdb[2] == 0x3f ? d->reject_mode_all :
                          d->cdb[2] == 0 ? d->reject_mode_page0 : true;
            if (reject) { d->residue = 4; d->status = 1; }
            else { memset(p, 0, 4); p[2] = d->read_only ? 0x80 : 0; }
            *actual = reject ? 0 : d->short_mode_sense ? 2 : 4;
            d->phase = 2;
            return USB_OK;
        }
        else if (d->cdb[0] == 0x28 || d->cdb[0] == 0x2a || d->cdb[0] == 0x88 || d->cdb[0] == 0x8a) {
            bool cdb16 = d->cdb[0] == 0x88 || d->cdb[0] == 0x8a;
            d->lba = cdb16 ? ((uint64_t)getbe32(d->cdb + 2) << 32) | getbe32(d->cdb + 6) : getbe32(d->cdb + 2);
            size_t offset = (size_t)(d->lba % 8) * 512;
            assert(size <= sizeof(d->media) - offset);
            if (d->input) memcpy(p, d->media + offset, size);
            else { ++d->writes; memcpy(d->media + offset, p, d->partial_write ? size / 2 : size); }
            if (d->input && d->data_in_stall) {
                assert(d->data_in_stall_actual <= size);
                d->residue = (uint32_t)(size - d->data_in_stall_actual);
                d->data_in_stall = false; d->phase = 2;
                *actual = d->data_in_stall_actual;
                return USB_STALL;
            }
        } else assert(0);
        d->phase = 2;
        if (d->partial_write && !d->input) { *actual = size / 2; return USB_TIMEOUT; }
        *actual = size; return USB_OK;
    }
    assert(d->phase == 2 && ep == 0x81 && size == 13);
    if (d->csw_stall) { d->csw_stall = false; return USB_STALL; }
    memset(p, 0, size); set32(p, 0x53425355); set32(p + 4, d->bad_tag ? d->tag + 1 : d->tag); set32(p + 8, d->residue);
    p[12] = d->status;
    d->status = 0;
    d->phase = 0; *actual = size; return USB_OK;
}
static void test_bot(void) {
    struct disk disk = {0}; struct usb_bot bot; struct usb_transport transport = {&disk, control, bulk, clear};
    assert(usb_bot_init(&bot, &transport, 2, 0x81, 2, 0) == USB_OK);
    assert(usb_bot_capacity(&bot) == USB_OK && bot.blocks == 8 && bot.block_size == 512);
    bool read_only = false;
    disk.read_only = true;
    assert(usb_bot_read_only(&bot, &read_only) == USB_OK && read_only);
    disk.read_only = false;
    assert(usb_bot_read_only(&bot, &read_only) == USB_OK && !read_only);
    disk.reject_mode_all = true; disk.read_only = true;
    assert(usb_bot_read_only(&bot, &read_only) == USB_OK && read_only);
    disk.reject_mode_page0 = true; read_only = true;
    assert(usb_bot_read_only(&bot, &read_only) == USB_OK && !read_only);
    disk.reject_mode_all = disk.reject_mode_page0 = false;
    disk.short_mode_sense = true; read_only = true;
    assert(usb_bot_read_only(&bot, &read_only) == USB_PHASE_ERROR && read_only);
    disk.short_mode_sense = false;
    assert(usb_bot_read_only(NULL, &read_only) == USB_INVALID && read_only);
    disk.capacity_bad = true;
    assert(usb_bot_capacity(&bot) == USB_RANGE && !bot.blocks && !bot.block_size);
    disk.capacity_bad = false;
    assert(usb_bot_capacity(&bot) == USB_OK && bot.blocks == 8 && bot.block_size == 512);
    uint8_t data[512], readback[512]; for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i ^ 0xa5);
    assert(usb_bot_write(&bot, 2, 1, data, sizeof(data)) == USB_OK);
    assert(usb_bot_sync(&bot) == USB_OK && disk.syncs == 1);
    assert(usb_bot_read(&bot, 2, 1, readback, sizeof(readback)) == USB_OK && !memcmp(data, readback, sizeof(data)));
    unsigned commands = disk.commands;
    assert(usb_bot_write(&bot, 8, 1, data, sizeof(data)) == USB_RANGE && disk.commands == commands);
    assert(usb_bot_read(&bot, UINT64_MAX, 1, data, sizeof(data)) == USB_RANGE && disk.commands == commands);
    assert(usb_bot_write(&bot, 0, 2, data, sizeof(data)) == USB_RANGE && disk.commands == commands);
    unsigned reset_base = disk.resets, clear_base = disk.clears;
    disk.csw_stall = true;
    assert(usb_bot_read(&bot, 2, 1, readback, 512) == USB_OK && disk.clears == clear_base + 1);
    uint8_t read_cdb[10] = {0x28}; size_t completed = 0;
    clear_base = disk.clears; disk.data_in_stall = true; disk.data_in_stall_actual = 256u;
    assert(usb_bot_command(&bot, read_cdb, sizeof(read_cdb), true, readback,
                           sizeof(readback), &completed) == USB_OK);
    assert(completed == 256u && disk.clears == clear_base + 1u);
    disk.bad_tag = true;
    assert(usb_bot_read(&bot, 2, 1, readback, 512) == USB_PHASE_ERROR &&
           disk.resets == reset_base + 1 && disk.clears == clear_base + 3);
    disk.bad_tag = false; disk.partial_write = true; unsigned writes = disk.writes;
    assert(usb_bot_write(&bot, 2, 1, data, sizeof(data)) == USB_TIMEOUT);
    assert(disk.writes == writes + 1 && bot.write_uncertain && disk.resets == reset_base + 2); /* Never replay. */
    disk.partial_write = false; assert(usb_bot_sync(&bot) == USB_IO && disk.syncs == 2);
    disk.bad_tag = true; disk.reset_fail = true;
    assert(usb_bot_read(&bot, 2, 1, readback, 512) == USB_PHASE_ERROR && bot.needs_reset);
    commands = disk.commands;
    assert(usb_bot_read(&bot, 2, 1, readback, 512) == USB_PHASE_ERROR && disk.commands == commands);
    disk.bad_tag = false; disk.reset_fail = false;
    assert(usb_bot_reset(&bot) == USB_OK && !bot.needs_reset);
    bot.blocks = (uint64_t)UINT32_MAX + 100;
    assert(usb_bot_read(&bot, (uint64_t)UINT32_MAX + 1, 1, readback, 512) == USB_OK && disk.cdb[0] == 0x88 && disk.lba == (uint64_t)UINT32_MAX + 1);
    disk.disconnected = true;
    assert(usb_bot_read(&bot, 0, 1, readback, 512) == USB_DISCONNECTED && !bot.present);
    usb_bot_detach(&bot); assert(!bot.blocks && !bot.block_size);
    assert(usb_bot_init(&bot, &transport, 2, 0x81, 2, 0) == USB_OK);
    bot.blocks = 8; bot.block_size = 512;
    assert(usb_bot_reset(&bot) == USB_DISCONNECTED && !bot.present && !bot.blocks && !bot.block_size);
}

struct role_trace { char trace[64]; size_t n; char fail; };
static int step(struct role_trace *t, char name) { assert(t->n + 1 < sizeof(t->trace)); t->trace[t->n++] = name; return t->fail == name ? USB_IO : USB_OK; }
static int stop_sub(void *p) { return step(p, 'S'); }
static int sync_media(void *p) { return step(p, 'F'); }
static int cancel(void *p) { return step(p, 'C'); }
static int stop_dma(void *p) { return step(p, 'D'); }
static int vbus(void *p, bool source) { return step(p, source ? '+' : '-'); }
static int select_role(void *p, enum usb_role role) { return step(p, role == USB_ROLE_HOST ? 'H' : 'G'); }
static int start(void *p, enum usb_role role) { return step(p, role == USB_ROLE_HOST ? 'h' : 'g'); }
static void test_roles(void) {
    struct role_trace trace = {0}; struct usb_role_ops ops = {&trace, stop_sub, sync_media, cancel, stop_dma, vbus, select_role, start};
    struct usb_role_state state = {USB_ROLE_DEVICE, false, false};
    assert(usb_role_switch(&state, &ops, USB_ROLE_HOST) == USB_OK && state.role == USB_ROLE_HOST);
    assert(!strcmp(trace.trace, "SFCD-H+h"));
    trace.n = 0; memset(trace.trace, 0, sizeof(trace.trace));
    assert(usb_role_quiesce(&state, &ops) == USB_OK && !strcmp(trace.trace, "SFCD-"));
    assert(usb_role_quiesce(&state, &ops) == USB_OK && trace.n == 5); /* EBS retry. */
    const char failures[] = {'S','F','C','D','-','H','+','h'};
    for (size_t i = 0; i < sizeof(failures); ++i) {
        memset(&trace, 0, sizeof(trace)); trace.fail = failures[i];
        state = (struct usb_role_state){USB_ROLE_DEVICE, false, false};
        assert(usb_role_switch(&state, &ops, USB_ROLE_HOST) == USB_IO);
        assert(state.role == USB_ROLE_STOPPED && state.faulted && !state.switching);
        size_t before = trace.n;
        assert(usb_role_switch(&state, &ops, USB_ROLE_DEVICE) == USB_IO && trace.n == before);
        if (failures[i] == 'D') assert(!strchr(trace.trace, '-')); /* Cannot alter power with live DMA. */
        if (i < 5) assert(!strchr(trace.trace, 'H') && !strchr(trace.trace, '+'));
    }
    state = (struct usb_role_state){USB_ROLE_DEVICE, true, false};
    assert(usb_role_switch(&state, &ops, USB_ROLE_HOST) == USB_BUSY);
}

int main(void) {
    test_mmio(); test_dma(); test_cdc(); test_ring(); test_hid(); test_bot(); test_roles();
    puts("PASS: USB production core MMIO/DMA/CDC/descriptors/HID/BOT/role tests; no hardware access");
    return 0;
}
