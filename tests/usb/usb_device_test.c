/* Combined MTU3 MAC/FIFO/QMU/DMA model, driven as an independent USB host. */
#define _POSIX_C_SOURCE 200112L
#include "UsbMtu3Device.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_READY (1u << 16)
#define SETUP_READY (1u << 17)
#define TX_READY (1u << 18)
#define DATAEND (1u << 19)
#define ACTIVE (1u << 15)
struct allocation { uint64_t address; void *cpu; size_t size; };
struct model {
    uint32_t regs[0x2e00 / 4], ippc[0x100 / 4];
    struct allocation memory[6];
    uint8_t fifo_in[128], fifo_out[128], ep0_reply[512], log[4096], notification[10];
    size_t in_size, in_at, out_size, ep0_size, log_size;
    unsigned allocations, releases, alloc_attempts, fail_alloc, writes, fail_write, delay, resets, syncs, fail_sync;
    uint32_t stop_timeout;
    bool uncertain_start, noncoherent;
};
struct fixture { struct model m; struct usb_cdc cdc; struct usb_mtu3_device d; uint64_t now; };
static bool queue_csr(uint32_t offset) { return offset == 0x510 || offset == 0x520 || offset == 0x620; }
static bool any_active(struct model *m)
{ return ((m->regs[0x510 / 4] | m->regs[0x520 / 4] | m->regs[0x620 / 4]) & ACTIVE) != 0; }
static void *physical(struct model *m, uint64_t address, size_t size)
{
    for (unsigned i = 0; i < 6; ++i) {
        struct allocation *a = &m->memory[i];
        if (a->cpu && address >= a->address && address - a->address < a->size && size <= a->size - (address - a->address))
            return (uint8_t *)a->cpu + (address - a->address);
    }
    assert(0 && "unmapped DMA access"); return NULL;
}
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    struct model *m = context;
    if (offset == 0x300) {
        *value = 0;
        for (unsigned i = 0; i < 4 && m->in_at < m->in_size; ++i) *value |= (uint32_t)m->fifo_in[m->in_at++] << (8u * i);
    } else if (offset == 0x710) {
        *value = (m->regs[0x780 / 4] & 0xffff ? 1u << 1 : 0) |
            (m->regs[0x780 / 4] >> 16 ? 1u << 2 : 0) |
            (m->regs[0x7c0 / 4] & 0xffff ? 1u << 17 : 0) |
            (m->regs[0x7c0 / 4] >> 16 ? 1u << 18 : 0) |
            (m->regs[0x7d0 / 4] >> 16 ? 1u << 20 : 0);
    } else *value = m->regs[offset / 4];
    return USB_OK;
}
static int read_ippc(void *context, uint32_t offset, uint32_t *value)
{ *value = ((struct model *)context)->ippc[offset / 4]; return USB_OK; }
static void fifo_write(struct model *m, uint32_t value, unsigned width)
{
    assert(m->out_size + width <= sizeof(m->fifo_out));
    for (unsigned i = 0; i < width; ++i) m->fifo_out[m->out_size++] = (uint8_t)(value >> (8u * i));
}
static int write32(void *context, uint32_t offset, uint32_t value)
{
    struct model *m = context;
    if (++m->writes == m->fail_write) return USB_IO;
    if (queue_csr(offset)) {
        if (value == 1) {
            (void)physical(m, m->regs[(offset + 4) / 4], 16);
            m->regs[(offset + 8) / 4] = m->regs[(offset + 4) / 4]; m->regs[offset / 4] = ACTIVE;
            if (m->uncertain_start) { m->uncertain_start = false; return USB_IO; }
        } else if (value == 2) m->regs[offset / 4] = ACTIVE;
        else if (value == 4) { if (m->stop_timeout != offset) m->regs[offset / 4] = 0; }
        else assert(0);
    } else if (offset == 0x300) fifo_write(m, value, 4);
    else if (offset == 0x100) {
        uint32_t flags = m->regs[offset / 4] & (RX_READY | SETUP_READY | (1u << 22));
        m->regs[offset / 4] = (flags & ~value) | (value & ~(RX_READY | SETUP_READY | (1u << 22) | (1u << 23)));
        if (value & TX_READY) {
            assert(m->ep0_size + m->out_size <= sizeof(m->ep0_reply));
            memcpy(m->ep0_reply + m->ep0_size, m->fifo_out, m->out_size);
            m->ep0_size += m->out_size; m->out_size = 0;
        }
    } else if (offset == 0x80 || offset == 0x241c || offset == 0x854 || offset == 0x700 || offset == 0x780 || offset == 0x7c0 || offset == 0x7d0)
        m->regs[offset / 4] &= ~value;
    else if (offset == 0x8) m->regs[0x4 / 4] |= value;
    else if (offset == 0xc) m->regs[0x4 / 4] &= ~value;
    else if (offset == 0x88) m->regs[0x84 / 4] |= value;
    else if (offset == 0x8c) m->regs[0x84 / 4] &= ~value;
    else if (offset == 0x708) m->regs[0x704 / 4] |= value;
    else if (offset == 0x70c) m->regs[0x704 / 4] &= ~value;
    else if (offset == 0x718) m->regs[0x714 / 4] |= value;
    else if (offset == 0x71c) m->regs[0x714 / 4] &= ~value;
    else if (offset == 0x110 || offset == 0x120 || offset == 0x220) {
        uint32_t w1c = (1u << 22) | (offset == 0x220 ? RX_READY : 0);
        m->regs[offset / 4] = (m->regs[offset / 4] & w1c & ~value) | (value & ~w1c & ~(1u << 20));
    } else {
        m->regs[offset / 4] = value;
        if (offset == 0x804 && value) {
            ++m->resets;
            if (value & 1) { m->regs[0x100 / 4] = 0; m->in_at = m->in_size = m->out_size = 0; }
        }
    }
    return USB_OK;
}
static int tail(void *context, uint32_t offset, unsigned width, uint32_t value)
{ assert(offset == 0x300 && (width == 1 || width == 2)); fifo_write(context, value, width); return USB_OK; }
static void barrier(void *context) { (void)context; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static void delay(void *context, uint32_t us) { ((struct model *)context)->delay += us; }
static int allocate(void *context, size_t size, size_t alignment, uint64_t limit, struct usb_dma_buffer *out)
{
    struct model *m = context;
    assert(limit == UINT32_MAX && alignment == 64);
    if (++m->alloc_attempts == m->fail_alloc) return USB_NO_MEMORY;
    unsigned i;
    for (i = 0; i < 6; ++i) if (!m->memory[i].cpu) break;
    assert(i < 6);
    if (posix_memalign(&out->cpu, alignment, size)) return USB_NO_MEMORY;
    out->address = 0x81000000u + (uint64_t)i * 4096; out->size = size; out->coherent = !m->noncoherent;
    m->memory[i] = (struct allocation){out->address, out->cpu, size}; ++m->allocations; return USB_OK;
}
static void release(void *context, struct usb_dma_buffer *buffer)
{
    struct model *m = context; assert(!any_active(m));
    for (unsigned i = 0; i < 6; ++i) if (m->memory[i].cpu == buffer->cpu) {
        free(buffer->cpu); m->memory[i].cpu = NULL; ++m->releases; return;
    }
    assert(0 && "double free");
}
static int sync_memory(void *context, struct usb_dma_buffer *buffer, size_t at, size_t length, enum usb_dma_direction direction, bool device)
{
    struct model *m = context;
    (void)direction; (void)device;
    assert(physical(context, buffer->address + at, length) == (uint8_t *)buffer->cpu + at);
    return ++m->syncs == m->fail_sync ? USB_IO : USB_OK;
}
static void prepare(struct fixture *f, bool enhanced)
{
    memset(f, 0, sizeof(*f));
    f->m.ippc[0xa4 / 4] = (enhanced ? 0x1003u : 0x1002u) << 16;
    f->m.regs[0xc10 / 4] = 0x202; f->m.regs[0xc08 / 4] = 4096; f->m.regs[0xc0c / 4] = 4096;
    assert(usb_cdc_init(&f->cdc, 0x1234, 0x5678, 0x100, true, "dali-fixture") == USB_OK);
}
static int init(struct fixture *f, bool qualified)
{
    struct usb_mmio mac = {&f->m, sizeof(f->m.regs), read32, write32, barrier, delay};
    struct usb_mmio ippc = {&f->m, sizeof(f->m.ippc), read_ippc, NULL, NULL, NULL};
    struct usb_dma_ops dma = {&f->m, qualified, allocate, release, sync_memory};
    return usb_mtu3_device_init(&f->d, &mac, &ippc, &dma, &f->cdc, &f->m, tail, qualified, false);
}
static int poll(struct fixture *f) { return usb_mtu3_device_poll(&f->d, ++f->now); }
static void start(struct fixture *f)
{
    assert(init(f, true) == USB_OK && f->m.allocations == 6 && !f->m.writes);
    assert(usb_mtu3_device_start(&f->d) == USB_AGAIN && !f->m.writes);
    f->m.ippc[0x18 / 4] = 1u << 9;
    assert(usb_mtu3_device_start(&f->d) == USB_OK && f->d.started);
    assert(!f->m.regs[0x141c / 4] && !(f->m.regs[0xc84 / 4] & 3));
    assert(f->m.regs[0x428 / 4] == (f->d.endpoints.enhanced ? UINT32_MAX : 0));
    assert((f->m.regs[0x2404 / 4] & 0x61) == 0x61 && !f->m.regs[0x400 / 4]);
}
static void bus_reset(struct fixture *f, bool high)
{
    f->m.regs[0x800 / 4] = high ? 3 : 1; f->m.regs[0x241c / 4] |= 4;
    f->m.regs[0x854 / 4] |= 1;
    assert(poll(f) == USB_OK && f->d.connected && f->cdc.high_speed == high && !f->cdc.configuration);
}
static int setup(struct fixture *f, uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, bool aborting)
{
    uint8_t bytes[8] = {type, request, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    memcpy(f->m.fifo_in, bytes, sizeof(bytes)); f->m.in_at = 0; f->m.in_size = sizeof(bytes); f->m.regs[0x108 / 4] = 8;
    if (aborting) f->m.regs[0x100 / 4] &= ~(TX_READY | DATAEND);
    f->m.regs[0x100 / 4] |= SETUP_READY; f->m.regs[0x80 / 4] |= aborting ? 0x10001 : 1;
    return poll(f);
}
static void status_ack(struct fixture *f)
{
    assert(f->m.regs[0x100 / 4] & DATAEND); f->m.regs[0x100 / 4] &= ~DATAEND;
    assert(poll(f) == USB_OK);
}
static void in_ack(struct fixture *f)
{
    assert(f->m.regs[0x100 / 4] & TX_READY); f->m.regs[0x100 / 4] &= ~TX_READY;
    f->m.regs[0x80 / 4] |= 1; assert(poll(f) == USB_OK);
}
static void configure(struct fixture *f)
{
    assert(setup(f, 0, 5, 9, 0, 0, false) == USB_OK && !f->cdc.address);
    status_ack(f); assert(f->cdc.address == 9);
    assert(setup(f, 0, 9, 1, 0, 0, false) == USB_OK && !f->cdc.configuration && f->d.configuring);
    status_ack(f); assert(f->cdc.configuration == 1 && f->d.endpoints.configured && !f->d.configuring);
    assert(f->d.channels[0].submitted == 10 && f->d.channels[2].submitted == (f->cdc.high_speed ? 512u : 64u));
}
static void host_dma(struct fixture *f, uint8_t endpoint, const void *out, size_t out_size)
{
    struct model *m = &f->m;
    bool input = (endpoint & 0x80) != 0;
    uint32_t offset = (input ? 0x500u : 0x600u) + 16u * (uint32_t)(endpoint & 15);
    assert(m->regs[offset / 4] & ACTIVE);
    uint32_t *gpd = physical(m, m->regs[(offset + 8) / 4], 16);
    assert(gpd[0] & 1);
    size_t capacity = input ? gpd[3] & 0xfffffu : gpd[0] >> (m->regs[0x428 / 4] ? 12 : 16);
    uint8_t *data = physical(m, gpd[2], capacity);
    if (input && endpoint == 0x81) { assert(capacity == 10); memcpy(m->notification, data, 10); }
    else if (input) {
        assert(m->log_size + capacity <= sizeof(m->log));
        memcpy(m->log + m->log_size, data, capacity); m->log_size += capacity;
    } else {
        assert(out_size <= capacity); memcpy(data, out, out_size); gpd[3] = (uint32_t)out_size;
    }
    gpd[0] &= ~1u; m->regs[(offset + 8) / 4] = gpd[1];
    uint32_t *next = physical(m, gpd[1], 16);
    if (!(next[0] & 1)) m->regs[offset / 4] = 0; /* Empty queue requires RESUME. */
    m->regs[0x700 / 4] |= 1u << ((input ? 0u : 16u) + (unsigned)(endpoint & 15));
}
static void dtr(struct fixture *f, unsigned value)
{
    assert(setup(f, 0x21, 0x22, (uint16_t)value, 0, 0, false) == USB_OK);
    status_ack(f); assert(f->cdc.control_lines == value);
}
static void finish(struct fixture *f)
{
    assert(usb_mtu3_device_stop(&f->d) == USB_OK && f->d.quiesced && !any_active(&f->m));
    unsigned writes = f->m.writes;
    assert(usb_mtu3_device_stop(&f->d) == USB_OK && f->m.writes == writes);
    assert(usb_mtu3_device_destroy(&f->d) == USB_OK && f->m.releases == f->m.allocations);
}
static void test_cdc_data(void)
{
    for (unsigned enhanced = 0; enhanced < 2; ++enhanced) for (unsigned high = 0; high < 2; ++high) {
        struct fixture f; prepare(&f, enhanced != 0); start(&f); bus_reset(&f, high != 0); configure(&f);
        uint8_t bytes[600]; for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
        assert(usb_mtu3_device_log(&f.d, bytes, sizeof(bytes)) == sizeof(bytes));
        assert(poll(&f) == USB_OK && !f.d.channels[1].submitted); /* DTR low: keep ring. */
        host_dma(&f, 0x81, NULL, 0); assert(!f.m.notification[8]); assert(poll(&f) == USB_OK);
        dtr(&f, 3); assert(f.d.channels[1].submitted == 512);
        uint32_t *gpd = physical(&f.m, f.m.regs[0x528 / 4], 16);
        assert(enhanced ? (gpd[0] & (1u << 6)) : (gpd[3] & (1u << 29))); /* Flush host URB with a ZLP. */
        host_dma(&f, 0x81, NULL, 0); assert(f.m.notification[8] == 3 && f.m.notification[0] == 0xa1 && f.m.notification[1] == 0x20);
        host_dma(&f, 0x82, NULL, 0); assert(poll(&f) == USB_OK && f.d.tx_dma_completed == 512 && f.d.channels[1].submitted == 88);
        host_dma(&f, 0x82, NULL, 0); assert(poll(&f) == USB_OK && f.d.tx_dma_completed == 600 && !f.d.channels[1].submitted);
        assert(f.m.log_size == 600 && !memcmp(f.m.log, bytes, 600));
        host_dma(&f, 0x02, "discard", 7); assert(poll(&f) == USB_OK && f.d.rx_discarded == 7 && f.d.channels[2].submitted);
        f.m.regs[0x7d0 / 4] = 1u << 18;
        assert(poll(&f) == USB_OK && !f.d.faulted && !f.m.regs[0x7d0 / 4] && f.d.channels[2].submitted);
        assert(!usb_log_dropped(&f.cdc.log)); finish(&f);
    }
}
static void test_controls_and_link(void)
{
    struct fixture f; prepare(&f, true); start(&f); bus_reset(&f, true); configure(&f); dtr(&f, 1);
    assert(usb_mtu3_device_log(&f.d, "drop", 4) == 4 && poll(&f) == USB_OK);
    assert(setup(&f, 2, 3, 0, 0x82, 0, false) == USB_OK && !f.d.channels[1].submitted);
    status_ack(&f); assert((f.cdc.halted_in & 4) && (f.m.regs[0x120 / 4] & (1u << 21)) && usb_log_dropped(&f.cdc.log) == 4);
    assert(usb_mtu3_device_log(&f.d, "held", 4) == 4 && poll(&f) == USB_OK && !f.d.channels[1].submitted);
    unsigned resets = f.m.resets;
    assert(setup(&f, 0, 9, 0, 0, 0, false) == USB_OK && f.d.endpoints.configured && f.d.configuring && f.m.resets == resets);
    f.m.ep0_size = 0;
    assert(setup(&f, 0x80, 8, 0, 0, 1, true) == USB_OK && f.cdc.configuration == 1 && f.d.endpoints.configured);
    assert((f.d.endpoints.halted_in & 4) && !f.d.channels[1].submitted && f.m.ep0_reply[0] == 1 && f.m.resets == resets);
    in_ack(&f); status_ack(&f);
    assert(setup(&f, 2, 1, 0, 0x82, 0, false) == USB_OK); status_ack(&f);
    assert(!(f.cdc.halted_in & 4) && f.d.channels[1].submitted == 4);
    host_dma(&f, 0x82, NULL, 0); assert(poll(&f) == USB_OK && f.m.log_size == 4 && !memcmp(f.m.log, "held", 4));
    f.m.regs[0x241c / 4] = 1; assert(poll(&f) == USB_OK && f.d.suspended);
    assert(usb_mtu3_device_log(&f.d, "resume", 6) == 6 && poll(&f) == USB_OK && !f.d.channels[1].submitted);
    f.m.regs[0x241c / 4] = 2; assert(poll(&f) == USB_OK && !f.d.suspended && f.d.channels[1].submitted == 6);
    f.m.ippc[0x18 / 4] = 0; assert(poll(&f) == USB_DISCONNECTED && !f.d.connected && !f.cdc.configuration && !any_active(&f.m));
    assert(!f.m.releases && usb_log_dropped(&f.cdc.log) == 10);
    f.m.ippc[0x18 / 4] = 1u << 9; bus_reset(&f, false); configure(&f);
    assert(setup(&f, 0, 9, 0, 0, 0, false) == USB_OK); status_ack(&f);
    assert(!f.cdc.configuration && !f.d.endpoints.configured && !any_active(&f.m));
    finish(&f);
}
static void test_qualification_and_failures(void)
{
    struct fixture f; prepare(&f, true);
    assert(init(&f, false) == USB_UNSUPPORTED && !f.m.allocations && !f.m.writes);
    f.m.regs[0x520 / 4] = ACTIVE;
    assert(init(&f, true) == USB_BUSY && !f.m.allocations && !f.m.writes);
    for (unsigned fail = 1; fail <= 6; ++fail) {
        prepare(&f, true); f.m.fail_alloc = fail;
        assert(init(&f, true) == USB_NO_MEMORY && !f.d.initialized && f.m.allocations == f.m.releases && !f.m.writes);
    }
    prepare(&f, true); f.m.noncoherent = true;
    assert(init(&f, true) == USB_UNSUPPORTED && f.m.allocations == f.m.releases && !f.m.writes);
    for (unsigned index = 0; index < 3; ++index) {
        prepare(&f, true); start(&f); bus_reset(&f, true); configure(&f); dtr(&f, 1);
        assert(usb_mtu3_device_log(&f.d, "retained", 8) == 8 && poll(&f) == USB_OK);
        f.m.stop_timeout = index == 0 ? 0x510 : index == 1 ? 0x520 : 0x620;
        assert(usb_mtu3_device_stop(&f.d) == USB_TIMEOUT && f.d.faulted && !f.d.quiesced && !f.m.releases);
        assert(f.d.channels[index].queue.pending && f.d.channels[index].buffer.cpu && f.d.channels[index].queue.running);
        assert(usb_mtu3_device_destroy(&f.d) == USB_TIMEOUT && !f.m.releases);
        f.m.stop_timeout = 0;
        assert(usb_mtu3_device_stop(&f.d) == USB_OK && f.d.quiesced && !any_active(&f.m));
        assert(usb_mtu3_device_start(&f.d) == USB_IO);
        assert(usb_mtu3_device_destroy(&f.d) == USB_OK && f.m.allocations == f.m.releases);
    }
    prepare(&f, true); start(&f); bus_reset(&f, true);
    f.m.uncertain_start = true;
    assert(setup(&f, 0, 5, 9, 0, 0, false) == USB_OK); status_ack(&f);
    assert(setup(&f, 0, 9, 1, 0, 0, false) == USB_OK);
    f.m.regs[0x100 / 4] &= ~DATAEND;
    assert(poll(&f) == USB_IO && f.d.faulted && f.d.quiesced && !f.m.releases);
    assert(usb_mtu3_device_destroy(&f.d) == USB_OK && f.m.allocations == f.m.releases);
    prepare(&f, true); start(&f); bus_reset(&f, true); configure(&f);
    f.m.regs[0x780 / 4] = 1u << 2;
    assert(poll(&f) == USB_IO && f.d.faulted && f.d.quiesced);
    assert(usb_mtu3_device_destroy(&f.d) == USB_OK);
    prepare(&f, true); start(&f);
    unsigned start_writes = f.m.writes; finish(&f);
    for (unsigned step = 1; step <= start_writes; ++step) {
        prepare(&f, true); assert(init(&f, true) == USB_OK);
        f.m.ippc[0x18 / 4] = 1u << 9; f.m.fail_write = step;
        assert(usb_mtu3_device_start(&f.d) == USB_IO && f.d.faulted && !f.d.started && !f.m.releases);
        assert(usb_mtu3_device_destroy(&f.d) == USB_OK && f.m.allocations == f.m.releases);
    }
    prepare(&f, true); start(&f); bus_reset(&f, true); configure(&f);
    f.m.fail_sync = f.m.syncs + 1; /* Cancellation cannot complete its cache handoff. */
    assert(usb_mtu3_device_stop(&f.d) == USB_IO && f.d.faulted && !f.d.quiesced && !f.m.releases);
    assert(usb_mtu3_device_stop(&f.d) == USB_OK && f.d.quiesced);
    assert(usb_mtu3_device_destroy(&f.d) == USB_OK && f.m.allocations == f.m.releases);
}
int main(void)
{
    test_cdc_data(); test_controls_and_link(); test_qualification_and_failures();
    puts("PASS: MTU3 device production combined CDC/EP0/QMU/DMA/reset/disconnect/quiesce model; hardware_qualified=false");
    return 0;
}
