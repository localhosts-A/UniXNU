/* Register/FIFO model executes the production EP0 engine; no device access. */
#include "UsbMtu3Ep0.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CSR 0x100u
#define RX (1u << 16)
#define SETUP (1u << 17)
#define TX (1u << 18)
#define END (1u << 19)
#define DPH (1u << 20)
#define SENT (1u << 22)
#define STALL (1u << 25)
struct model {
    uint32_t regs[0x900 / 4];
    uint8_t rx[128], fifo[128], sent[1024];
    size_t rx_size, rx_at, fifo_size, sent_size, packets[32], packet_count;
    unsigned tails[3], calls, fail_call, phases[3], reset_count;
    bool fail_tail, fail_prepare, fail_apply, fail_abort;
};
struct fixture { struct model m; struct usb_cdc cdc; struct usb_mtu3_ep0 ep; uint64_t now; };
static int access_result(struct model *m) { return ++m->calls == m->fail_call ? USB_IO : USB_OK; }
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    struct model *m = context;
    if (access_result(m)) return USB_IO;
    if (offset == 0x300) {
        *value = 0;
        for (unsigned i = 0; i < 4 && m->rx_at < m->rx_size; ++i)
            *value |= (uint32_t)m->rx[m->rx_at++] << (8u * i);
    } else *value = m->regs[offset / 4];
    return USB_OK;
}
static void fifo_put(struct model *m, uint32_t value, unsigned width)
{
    assert(m->fifo_size + width <= sizeof(m->fifo));
    for (unsigned i = 0; i < width; ++i) m->fifo[m->fifo_size++] = (uint8_t)(value >> (8u * i));
}
static int write32(void *context, uint32_t offset, uint32_t value)
{
    struct model *m = context;
    if (access_result(m)) return USB_IO;
    if (offset == 0x300) fifo_put(m, value, 4);
    else if (offset == 0x80) m->regs[offset / 4] &= ~value;
    else if (offset == 0x88) m->regs[0x84 / 4] |= value;
    else if (offset == 0x8c) m->regs[0x84 / 4] &= ~value;
    else if (offset == CSR) {
        uint32_t flags = m->regs[CSR / 4] & (RX | SETUP | SENT);
        flags &= ~value;
        m->regs[CSR / 4] = flags | (value & (0x3ffu | TX | END | DPH | STALL));
        if (value & TX) {
            assert(m->packet_count < 32 && m->sent_size + m->fifo_size <= sizeof(m->sent));
            m->packets[m->packet_count++] = m->fifo_size;
            memcpy(m->sent + m->sent_size, m->fifo, m->fifo_size);
            m->sent_size += m->fifo_size; m->fifo_size = 0;
        }
    } else {
        m->regs[offset / 4] = value;
        if (offset == 0x804 && (value & 1)) {
            ++m->reset_count; m->regs[CSR / 4] = 0;
            m->fifo_size = m->rx_size = m->rx_at = 0;
        }
    }
    return USB_OK;
}
static int tail(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    struct model *m = context;
    assert(offset == 0x300 && (width == 1 || width == 2));
    if (m->fail_tail) return USB_IO;
    ++m->tails[width]; fifo_put(m, value, width); return USB_OK;
}
static void barrier(void *context) { (void)context; }
static int effect(void *context, const struct usb_control_reply *reply, enum usb_ep0_phase phase)
{
    struct model *m = context;
    assert(reply->effect == USB_CONTROL_CONFIGURATION || reply->effect == USB_CONTROL_CLEAR_HALT || reply->effect == USB_CONTROL_SET_HALT);
    ++m->phases[phase];
    return (phase == USB_EP0_PREPARE && m->fail_prepare) ||
        (phase == USB_EP0_APPLY && m->fail_apply) || (phase == USB_EP0_ABORT && m->fail_abort) ? USB_IO : USB_OK;
}
static void init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    struct usb_mmio mac = {&f->m, sizeof(f->m.regs), read32, write32, barrier, NULL};
    struct usb_mtu3_ep0_ops ops = {&f->m, tail, effect};
    assert(usb_cdc_init(&f->cdc, 0x1234, 0x5678, 0x100, true, "0123456789012345678901234567890") == USB_OK);
    assert(usb_mtu3_ep0_init(&f->ep, &mac, &ops, &f->cdc, false) == USB_UNSUPPORTED);
    assert(usb_mtu3_ep0_init(&f->ep, &mac, &ops, &f->cdc, true) == USB_OK);
    assert(usb_mtu3_ep0_poll(&f->ep, 0) == USB_DISCONNECTED);
    assert(usb_mtu3_ep0_start(&f->ep) == USB_OK);
    assert(usb_mtu3_ep0_start(&f->ep) == USB_BUSY);
    assert(f->m.regs[0x84 / 4] == 0x10001 && f->m.regs[CSR / 4] == 64);
}
static int poll(struct fixture *f) { return usb_mtu3_ep0_poll(&f->ep, ++f->now); }
static void inject(struct fixture *f, const void *data, size_t size, uint32_t flags, uint32_t irq)
{
    assert(size <= sizeof(f->m.rx));
    memcpy(f->m.rx, data, size); f->m.rx_size = size; f->m.rx_at = 0;
    f->m.regs[0x108 / 4] = (uint32_t)size;
    f->m.regs[CSR / 4] |= flags; f->m.regs[0x80 / 4] |= irq;
}
static int setup(struct fixture *f, uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, bool aborting)
{
    uint8_t bytes[8] = {type, request, (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    if (aborting) { f->m.regs[CSR / 4] &= ~(TX | END); f->m.fifo_size = 0; }
    inject(f, bytes, sizeof(bytes), SETUP, aborting ? 0x10001 : 1);
    return poll(f);
}
static int ack_in(struct fixture *f)
{
    assert(f->m.regs[CSR / 4] & TX);
    f->m.regs[CSR / 4] &= ~TX; f->m.regs[0x80 / 4] |= 1;
    return poll(f);
}
static int ack_status(struct fixture *f)
{
    assert(f->m.regs[CSR / 4] & END);
    f->m.regs[CSR / 4] &= ~END; return poll(f);
}
static void configure(struct fixture *f)
{
    assert(setup(f, 0, 5, 9, 0, 0, false) == USB_OK);
    assert(!f->cdc.address && !(f->m.regs[0x800 / 4] & (0x7fu << 24)));
    assert(poll(f) == USB_AGAIN && !f->cdc.address); /* No premature ACK. */
    assert(ack_status(f) == USB_AGAIN && f->cdc.address == 9 && f->m.regs[0x800 / 4] == (9u << 24));
    assert(setup(f, 0, 9, 1, 0, 0, false) == USB_OK && !f->cdc.configuration);
    assert(f->m.phases[USB_EP0_PREPARE] == 1 && !f->m.phases[USB_EP0_APPLY]);
    assert(ack_status(f) == USB_AGAIN && f->cdc.configuration == 1 && f->m.phases[USB_EP0_APPLY] == 1);
}
static void test_enumeration(void)
{
    struct fixture f; init(&f);
    assert(setup(&f, 0x80, 6, 0x100, 0, 18, false) == USB_OK);
    assert(f.m.packet_count == 1 && f.m.packets[0] == 18 && f.m.sent[0] == 18 && f.m.tails[2] == 1);
    assert(ack_in(&f) == USB_OK && f.ep.state == USB_EP0_STATUS);
    assert(ack_status(&f) == USB_AGAIN && f.ep.state == USB_EP0_IDLE);
    f.m.packet_count = f.m.sent_size = 0;
    assert(setup(&f, 0x80, 6, 0x200, 0, 255, false) == USB_OK);
    assert(f.m.packets[0] == 64 && f.ep.state == USB_EP0_IN);
    assert(ack_in(&f) == USB_OK && f.m.packets[1] == 11 && f.ep.state == USB_EP0_IN_LAST);
    assert(f.m.sent_size == 75 && f.m.tails[1] == 1 && f.m.tails[2] == 2);
    assert(ack_in(&f) == USB_OK && ack_status(&f) == USB_AGAIN);
    f.m.packet_count = f.m.sent_size = 0;
    assert(setup(&f, 0x80, 6, 0x303, 0x409, 255, false) == USB_OK); /* 64-byte serial then ZLP. */
    assert(f.m.packets[0] == 64 && ack_in(&f) == USB_OK && f.m.packets[1] == 0);
    assert(ack_in(&f) == USB_OK && ack_status(&f) == USB_AGAIN);
    configure(&f);
}
static void test_class_and_abort(void)
{
    struct fixture f; init(&f); configure(&f);
    uint8_t coding[7] = {0x80,0x25,0,0,0,0,7};
    assert(setup(&f, 0x21, 0x20, 0, 0, 7, false) == USB_OK && f.ep.state == USB_EP0_OUT);
    inject(&f, coding, sizeof(coding), RX, 1);
    assert(poll(&f) == USB_OK && f.cdc.line_coding[6] == 8 && f.ep.state == USB_EP0_STATUS);
    assert(ack_status(&f) == USB_AGAIN && !memcmp(f.cdc.line_coding, coding, 7));
    assert(setup(&f, 0x21, 0x22, 3, 0, 0, false) == USB_OK && !f.cdc.control_lines);
    assert(setup(&f, 0x80, 8, 0, 0, 1, true) == USB_OK && !f.cdc.control_lines);
    assert(ack_in(&f) == USB_OK && ack_status(&f) == USB_AGAIN);
    assert(setup(&f, 0x21, 0x22, 3, 0, 0, false) == USB_OK && ack_status(&f) == USB_AGAIN && f.cdc.control_lines == 3);
    assert(setup(&f, 0, 9, 0, 0, 0, false) == USB_OK);
    assert(setup(&f, 0x80, 8, 0, 0, 1, true) == USB_OK && f.cdc.configuration == 1);
    assert(f.m.phases[USB_EP0_ABORT] == 1 && f.m.phases[USB_EP0_APPLY] == 1);
    assert(ack_in(&f) == USB_OK && ack_status(&f) == USB_AGAIN);
    assert(setup(&f, 2, 3, 0, 0x82, 0, false) == USB_OK && !(f.cdc.halted_in & 4));
    assert(ack_status(&f) == USB_AGAIN && (f.cdc.halted_in & 4));
    assert(setup(&f, 2, 1, 0, 0x82, 0, false) == USB_OK && ack_status(&f) == USB_AGAIN && !(f.cdc.halted_in & 4));
    assert(usb_log_write(&f.cdc.log, "retained", 8) == 8);
    assert(setup(&f, 0, 9, 0, 0, 0, false) == USB_OK);
    assert(usb_mtu3_ep0_bus_reset(&f.ep, false) == USB_OK && !f.cdc.configuration && !f.cdc.address && !f.cdc.high_speed);
    uint8_t data[8]; assert(usb_log_take(&f.cdc.log, data, sizeof(data)) == 8 && !memcmp(data, "retained", 8));
    assert(!f.cdc.control_lines && f.m.phases[USB_EP0_ABORT] == 2);
}
static void test_bad_packets_and_timeout(void)
{
    struct fixture f; init(&f); configure(&f);
    uint8_t bytes[65] = {0};
    assert(setup(&f, 0x21, 0x20, 0, 0, 7, false) == USB_OK);
    inject(&f, bytes, 6, RX, 1); assert(poll(&f) == USB_STALL && (f.m.regs[CSR / 4] & STALL));
    f.m.regs[CSR / 4] |= SENT;
    assert(setup(&f, 0x80, 8, 0, 0, 1, false) == USB_OK && !(f.m.regs[CSR / 4] & (SENT | STALL)));
    assert(ack_in(&f) == USB_OK && ack_status(&f) == USB_AGAIN);
    assert(setup(&f, 0x21, 0x20, 0, 0, 7, false) == USB_OK);
    inject(&f, bytes, 65, RX, 1); assert(poll(&f) == USB_STALL);
    assert(setup(&f, 0, 0xff, 0, 0, 0, true) == USB_STALL);
    assert(setup(&f, 0x80, 0xff, 0, 0, 4, true) == USB_STALL && (f.m.regs[CSR / 4] & DPH));
    assert(setup(&f, 0x21, 0x20, 0, 0, 129, true) == USB_STALL);
    inject(&f, bytes, 7, SETUP, 0x10001); assert(poll(&f) == USB_STALL);
    assert(setup(&f, 0, 9, 0, 0, 0, true) == USB_OK);
    unsigned aborted = f.m.phases[USB_EP0_ABORT];
    f.now += 1000000;
    assert(poll(&f) == USB_TIMEOUT && f.ep.state == USB_EP0_IDLE);
    assert(f.cdc.configuration == 1 && f.cdc.address == 9 && f.m.regs[0x800 / 4] == (9u << 24));
    assert(f.m.phases[USB_EP0_ABORT] == aborted + 1);
    assert(usb_mtu3_ep0_poll(&f.ep, 0) == USB_INVALID);
    assert(usb_mtu3_ep0_stop(&f.ep) == USB_OK && !f.cdc.address && !f.cdc.configuration);
    assert(usb_mtu3_ep0_stop(&f.ep) == USB_OK && !f.m.regs[0x84 / 4]);
    assert(poll(&f) == USB_DISCONNECTED);
}
static void test_failures(void)
{
    struct fixture f; init(&f); configure(&f);
    f.m.fail_prepare = true;
    assert(setup(&f, 0, 9, 0, 0, 0, false) == USB_STALL && f.cdc.configuration == 1 && f.m.phases[USB_EP0_ABORT] == 1);
    f.m.fail_prepare = false;
    assert(setup(&f, 0, 9, 0, 0, 0, true) == USB_OK);
    f.m.fail_apply = true;
    assert(ack_status(&f) == USB_IO && f.ep.state == USB_EP0_FAULTED && f.cdc.configuration == 1);
    assert(poll(&f) == USB_IO);
    init(&f); configure(&f);
    assert(setup(&f, 0, 9, 0, 0, 0, false) == USB_OK); f.m.fail_abort = true;
    assert(setup(&f, 0x80, 8, 0, 0, 1, true) == USB_IO && f.ep.state == USB_EP0_FAULTED);
    init(&f); f.m.fail_tail = true;
    assert(setup(&f, 0x80, 6, 0x100, 0, 18, false) == USB_IO && f.ep.state == USB_EP0_FAULTED);
    for (unsigned step = 1; step < 20; ++step) {
        init(&f); f.m.fail_call = f.m.calls + step;
        int result = setup(&f, 0x80, 6, 0x100, 0, 18, false);
        if (result == USB_OK) result = ack_in(&f);
        if (result == USB_OK) result = ack_status(&f);
        if (f.m.calls >= f.m.fail_call) assert(result == USB_IO && f.ep.state == USB_EP0_FAULTED);
    }
}
int main(void)
{
    test_enumeration(); test_class_and_abort(); test_bad_packets_and_timeout(); test_failures();
    puts("PASS: MTU3 EP0 production CDC register/FIFO/status/abort/timeout/fault tests; model only");
    return 0;
}
