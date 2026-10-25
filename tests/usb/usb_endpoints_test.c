#include "UsbMtu3Endpoints.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct registers {
    uint32_t regs[0x1000 / 4], resets;
    unsigned reads, writes, fail_write;
};
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    struct registers *r = context; ++r->reads; *value = r->regs[offset / 4]; return USB_OK;
}
static int write32(void *context, uint32_t offset, uint32_t value)
{
    struct registers *r = context;
    if (++r->writes == r->fail_write) return USB_IO;
    if (offset == 0x708) r->regs[0x704 / 4] |= value;
    else if (offset == 0x70c) r->regs[0x704 / 4] &= ~value;
    else if (offset == 0x110 || offset == 0x120 || offset == 0x220) {
        uint32_t w1c = (1u << 22) | (offset == 0x220 ? 1u << 16 : 0);
        r->regs[offset / 4] = (r->regs[offset / 4] & w1c & ~value) | (value & ~w1c);
    } else r->regs[offset / 4] = value;
    if (offset == 0x804) r->resets |= value;
    return USB_OK;
}
static void barrier(void *context) { (void)context; }
static void init_regs(struct registers *mac, struct registers *ippc, bool separate, bool enhanced)
{
    memset(mac, 0, sizeof(*mac)); memset(ippc, 0, sizeof(*ippc));
    ippc->regs[0xa4 / 4] = (enhanced ? 0x1003u : 0x1002u) << 16;
    ippc->regs[0x28 / 4] = separate ? 1 : 0;
    mac->regs[0xc10 / 4] = 0x202;
    mac->regs[0xc08 / 4] = 4096; mac->regs[0xc0c / 4] = 2048;
    mac->regs[0x828 / 4] = 1; mac->regs[0x860 / 4] = 1u << 7;
}
static int probe(struct usb_mtu3_endpoints *ep, struct registers *mac, struct registers *ippc, bool override)
{
    struct usb_mmio m = {mac, sizeof(mac->regs), read32, write32, barrier, NULL};
    struct usb_mmio p = {ippc, 0x100, read32, write32, barrier, NULL};
    return usb_mtu3_endpoints_init(ep, &m, &p, true, override);
}
static void test_layouts(void)
{
    for (unsigned separate = 0; separate < 2; ++separate) {
        for (unsigned enhanced = 0; enhanced < 2; ++enhanced) {
            for (unsigned high = 0; high < 2; ++high) {
                struct registers m, p; struct usb_mtu3_endpoints ep;
                init_regs(&m, &p, separate != 0, enhanced != 0);
                assert(probe(&ep, &m, &p, false) == USB_OK && ep.enhanced == (enhanced != 0));
                assert(!m.writes && !p.writes);
                uint32_t tx = separate ? 0 : 64, rx = separate ? 0 : 2112;
                assert(ep.tx_base == tx && ep.rx_base == rx);
                assert(usb_mtu3_endpoints_configure(&ep, high != 0) == USB_OK && ep.configured);
                assert(m.regs[0x110 / 4] == ((1u << 29) | 16));
                assert(m.regs[0x120 / 4] == ((1u << 29) | (high ? 512u : 64u)));
                assert(m.regs[0x220 / 4] == m.regs[0x120 / 4]);
                assert(m.regs[0x114 / 4] == 0x110 && m.regs[0x124 / 4] == 0x100 && m.regs[0x224 / 4] == 0x100);
                assert(m.regs[0x118 / 4] == ((9u << 24) | (9u << 16) | (tx >> 4)));
                assert(m.regs[0x128 / 4] == ((9u << 16) | ((tx + 1024) >> 4)));
                assert(m.regs[0x228 / 4] == ((9u << 16) | (rx >> 4)));
                assert(m.regs[0x704 / 4] == 0x40006 && m.regs[0x828 / 4] == 0x40007 && m.regs[0x860 / 4] == 0x84);
                assert(m.resets == 0x60004 && m.regs[0x804 / 4] == 0);
                assert(usb_mtu3_endpoints_configure(&ep, high != 0) == USB_BUSY);
                assert(usb_mtu3_endpoints_deconfigure(&ep) == USB_OK && !ep.configured && !ep.faulted);
                assert(!m.regs[0x110 / 4] && !m.regs[0x120 / 4] && !m.regs[0x220 / 4]);
                assert(!m.regs[0x704 / 4] && m.regs[0x828 / 4] == 1 && m.regs[0x860 / 4] == 0x80);
            }
        }
    }
}
static void test_rejections(void)
{
    struct registers m, p; struct usb_mtu3_endpoints ep;
    init_regs(&m, &p, true, true);
    assert(probe(&ep, &m, &p, true) == USB_OK && !ep.enhanced);
    for (unsigned i = 0; i < 6; ++i) {
        init_regs(&m, &p, true, true);
        if (i == 0) p.regs[0xa4 / 4] = 0;
        if (i == 1) p.regs[0xa4 / 4] = UINT32_MAX;
        if (i == 2) m.regs[0xc10 / 4] = 0x102;
        if (i == 3) m.regs[0xc08 / 4] = 1024;
        if (i == 4) m.regs[0xc0c / 4] = 512;
        if (i == 5) m.regs[0xc08 / 4] = UINT32_MAX;
        assert(probe(&ep, &m, &p, false) != USB_OK && !m.writes);
        assert(usb_mtu3_endpoints_configure(&ep, true) == USB_INVALID);
    }
    const uint32_t queues[3] = {0x510, 0x520, 0x620};
    for (unsigned i = 0; i < 3; ++i) {
        init_regs(&m, &p, false, false); assert(probe(&ep, &m, &p, false) == USB_OK);
        m.regs[queues[i] / 4] = 1u << 15;
        assert(usb_mtu3_endpoints_configure(&ep, true) == USB_BUSY && !m.writes);
        m.regs[queues[i] / 4] = 0;
        assert(usb_mtu3_endpoints_configure(&ep, true) == USB_OK);
        unsigned before = m.writes;
        m.regs[queues[i] / 4] = 1u << 15;
        assert(usb_mtu3_endpoints_deconfigure(&ep) == USB_BUSY && ep.configured && m.writes == before);
    }
}
static void test_halt(void)
{
    struct registers m, p; struct usb_mtu3_endpoints ep;
    init_regs(&m, &p, false, false); assert(probe(&ep, &m, &p, false) == USB_OK);
    assert(usb_mtu3_endpoints_halt(&ep, 0x82, true) == USB_INVALID);
    assert(usb_mtu3_endpoints_configure(&ep, true) == USB_OK);
    assert(usb_mtu3_endpoints_halt(&ep, 0x80, true) == USB_INVALID);
    assert(usb_mtu3_endpoints_halt(&ep, 0x83, true) == USB_INVALID);
    const uint8_t addresses[3] = {0x81, 0x82, 0x02};
    const uint32_t csrs[3] = {0x110, 0x120, 0x220};
    const uint32_t queues[3] = {0x510, 0x520, 0x620};
    for (unsigned i = 0; i < 3; ++i) {
        m.regs[queues[i] / 4] = 1u << 15; unsigned before = m.writes;
        assert(usb_mtu3_endpoints_halt(&ep, addresses[i], true) == USB_BUSY && m.writes == before);
        m.regs[queues[i] / 4] = 0;
        uint32_t flags = (1u << 22) | (i == 2 ? 1u << 16 : 0);
        m.regs[csrs[i] / 4] |= flags;
        assert(usb_mtu3_endpoints_halt(&ep, addresses[i], true) == USB_OK);
        assert((m.regs[csrs[i] / 4] & flags) == flags && (m.regs[csrs[i] / 4] & (1u << 21)));
        uint16_t bitmap = i == 2 ? ep.halted_out : ep.halted_in;
        assert(bitmap & (1u << (addresses[i] & 15)));
        m.resets = 0;
        assert(usb_mtu3_endpoints_halt(&ep, addresses[i], false) == USB_OK);
        assert(!(m.regs[csrs[i] / 4] & ((1u << 22) | (1u << 21))));
        assert(m.resets == (1u << ((i == 2 ? 0 : 16) + (addresses[i] & 15))));
    }
}
static void test_faults(void)
{
    for (unsigned step = 1; step < 19; ++step) {
        struct registers m, p; struct usb_mtu3_endpoints ep;
        init_regs(&m, &p, true, true); assert(probe(&ep, &m, &p, false) == USB_OK);
        m.fail_write = step;
        assert(usb_mtu3_endpoints_configure(&ep, true) == USB_IO && ep.faulted);
        unsigned before = m.writes;
        assert(usb_mtu3_endpoints_configure(&ep, true) == USB_IO && m.writes == before);
        m.fail_write = 0;
        assert(usb_mtu3_endpoints_deconfigure(&ep) == USB_OK && !ep.configured && !ep.faulted);
        assert(usb_mtu3_endpoints_configure(&ep, false) == USB_OK);
    }
}
int main(void)
{
    test_layouts(); test_rejections(); test_halt(); test_faults();
    puts("PASS: MTU3 endpoint production FIFO/capability/CSR/halt/active-DMA/fault tests; model only");
    return 0;
}
