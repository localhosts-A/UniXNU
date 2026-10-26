/* xHCI capabilities, producer-ring and event-ring production-code tests. */
#define _POSIX_C_SOURCE 200112L
#include "UsbXhci.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mmio_model { uint32_t regs[0x1000 / 4]; unsigned reads, writes; int fail_read; };
struct sync_record {
    struct usb_dma_buffer *buffer;
    size_t offset, length;
    enum usb_dma_direction direction;
    bool for_device;
};
struct dma_model {
    uint64_t next_address;
    unsigned allocations, releases, syncs;
    unsigned fail_sync;
    bool noncoherent;
    struct sync_record sync_records[128];
};

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void set_reg(struct mmio_model *m, uint32_t offset, uint32_t value)
{ assert(!(offset & 3u) && offset < sizeof(m->regs)); m->regs[offset / 4u] = value; }
static int read32(void *context, uint32_t offset, uint32_t *out)
{
    struct mmio_model *m = context; assert(out && !(offset & 3u) && offset < sizeof(m->regs));
    ++m->reads;
    if (m->fail_read && m->reads == (unsigned)m->fail_read) return USB_IO;
    *out = m->regs[offset / 4u]; return USB_OK;
}
static int write32(void *context, uint32_t offset, uint32_t value)
{ struct mmio_model *m = context; (void)offset; (void)value; ++m->writes; return USB_IO; }
static void barrier(void *context) { (void)context; __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static void initialize_capabilities(struct mmio_model *m)
{
    memset(m, 0, sizeof(*m));
    set_reg(m, 0x00, 0x01000040u); /* xHCI 1.0, operational registers at 0x40. */
    set_reg(m, 0x04, 8u | (2u << 8) | (8u << 24));
    set_reg(m, 0x10, (0x800u / 4u << 16) | 5u); /* xECP 0x800, 64-byte contexts, AC64. */
    set_reg(m, 0x14, 0x600u);
    set_reg(m, 0x18, 0x500u);
    set_reg(m, 0x48, 1u); /* 4 KiB page support. */
    set_reg(m, 0x800, 2u | (6u << 8) | (2u << 24));
    set_reg(m, 0x804, 0x20425355u); /* "USB " */
    set_reg(m, 0x808, 1u | (4u << 8));
    set_reg(m, 0x818, 2u | (3u << 24));
    set_reg(m, 0x81c, 0x20425355u);
    set_reg(m, 0x820, 5u | (4u << 8));
}

static void test_capability_discovery(void)
{
    struct mmio_model m;
    initialize_capabilities(&m);
    struct usb_mmio mmio = {&m, sizeof(m.regs), read32, write32, barrier, NULL};
    struct usb_xhci_capabilities caps = {0};
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_OK);
    assert(caps.version == 0x0100 && caps.max_slots == 8 && caps.max_interrupts == 2 && caps.max_ports == 8);
    assert(caps.context_bytes == 64 && caps.address_64bit && caps.page_size_mask == 1);
    assert(caps.operational_offset == 0x40 && caps.doorbell_offset == 0x600 && caps.runtime_offset == 0x500);
    assert(caps.usb2_port_count == 4);
    assert(caps.usb2_ports[0] == 0x0f && caps.usb2_ports[1] == 0);
    assert(!m.writes);

    struct usb_xhci_capabilities before = caps;
    set_reg(&m, 0x808, 1u | (9u << 8)); /* Port range exceeds HCSPARAMS1. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_RANGE && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); set_reg(&m, 0x48, 2u); /* 4 KiB is required. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_UNSUPPORTED && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); set_reg(&m, 0x48, 0x00010001u); /* Reserved PAGESIZE bits. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_INVALID && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); set_reg(&m, 0x18, 0x521u); /* Reserved alignment bits. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_INVALID && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); set_reg(&m, 0x10, 0u); /* No Supported Protocol chain. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_UNSUPPORTED && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); set_reg(&m, 0x10, (0x0fffu << 16) | 5u); /* xECP escapes the BAR. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_RANGE && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); set_reg(&m, 0x818, 2u | (3u << 8) | (2u << 24));
    set_reg(&m, 0x820, 4u | (4u << 8)); /* Overlaps USB2 port 4. */
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_INVALID && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); mmio.size = 0x4c0;
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_RANGE && !memcmp(&caps, &before, sizeof(caps)));
    initialize_capabilities(&m); mmio.size = sizeof(m.regs); m.fail_read = 3; m.reads = 0;
    assert(usb_xhci_read_capabilities(&mmio, &caps) == USB_IO && !memcmp(&caps, &before, sizeof(caps)));
    puts("PASS: xHCI read-only capability validation");
}

static int allocate_dma(void *context, size_t size, size_t alignment, uint64_t limit,
                        struct usb_dma_buffer *out)
{
    struct dma_model *m = context;
    assert(out && limit == UINT32_MAX && alignment >= 64u);
    ++m->allocations;
    if (posix_memalign(&out->cpu, alignment, size)) return USB_NO_MEMORY;
    out->address = m->next_address;
    out->size = size;
    out->coherent = !m->noncoherent;
    m->next_address += ((uint64_t)size + 63u) & ~UINT64_C(63);
    return USB_OK;
}
static void release_dma(void *context, struct usb_dma_buffer *buffer)
{ ++((struct dma_model *)context)->releases; free(buffer->cpu); }
static int sync_dma(void *context, struct usb_dma_buffer *buffer, size_t offset, size_t size,
                    enum usb_dma_direction direction, bool for_device)
{
    struct dma_model *m = context;
    assert(buffer && offset <= buffer->size && size <= buffer->size - offset);
    assert(direction >= USB_DMA_TO_DEVICE && direction <= USB_DMA_BIDIRECTIONAL);
    assert(m->syncs < sizeof(m->sync_records) / sizeof(m->sync_records[0]));
    m->sync_records[m->syncs] = (struct sync_record){buffer, offset, size, direction, for_device};
    ++m->syncs;
    return m->fail_sync == m->syncs ? USB_IO : USB_OK;
}
static struct usb_dma_ops dma_ops(struct dma_model *m)
{ return (struct usb_dma_ops){m, true, allocate_dma, release_dma, sync_dma}; }
static void initialize_dma(struct dma_model *m)
{ memset(m, 0, sizeof(*m)); m->next_address = 0x81000000u; }
static void assert_sync_record(const struct dma_model *m, unsigned index, struct usb_dma_buffer *buffer,
                               size_t offset, size_t length, enum usb_dma_direction direction,
                               bool for_device)
{
    assert(index < m->syncs);
    const struct sync_record *record = &m->sync_records[index];
    assert(record->buffer == buffer && record->offset == offset && record->length == length);
    assert(record->direction == direction && record->for_device == for_device);
}
static struct usb_xhci_trb make_trb(uint32_t type, uint32_t value)
{
    struct usb_xhci_trb trb = {{0}};
    put_le32(trb.bytes, value);
    put_le32(trb.bytes + 8, value ^ 0x55aa55aau);
    put_le32(trb.bytes + 12, type << USB_XHCI_TRB_TYPE_SHIFT);
    return trb;
}
static const uint8_t *transfer_slot(const struct usb_xhci_transfer_ring *ring, uint32_t index)
{ return (const uint8_t *)ring->trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES; }

static void test_transfer_ring_cycle_wrap_and_completion(void)
{
    struct dma_model m; initialize_dma(&m); struct usb_dma_ops dma = dma_ops(&m);
    struct usb_xhci_transfer_ring ring;
    assert(usb_xhci_transfer_ring_init(&ring, &dma, 8) == USB_OK);
    assert(ring.usable_entries == 7 && ring.producer_cycle == 1 && ring.outstanding == 0);
    const uint8_t *link = transfer_slot(&ring, 7);
    assert(get_le32(link) == (uint32_t)ring.trbs.address && get_le32(link + 4) == 0);
    assert(((get_le32(link + 12) >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK) == USB_XHCI_TRB_LINK);
    assert(get_le32(link + 12) & USB_XHCI_TRB_TOGGLE_CYCLE);
    assert(!(get_le32(link + 12) & USB_XHCI_TRB_CYCLE));
    assert(usb_xhci_transfer_ring_mark_active(&ring) == USB_OK);

    struct usb_xhci_trb first[3] = {make_trb(USB_XHCI_TRB_ENABLE_SLOT, 10),
                                     make_trb(USB_XHCI_TRB_ADDRESS_DEVICE, 11),
                                     make_trb(USB_XHCI_TRB_CONFIGURE_ENDPOINT, 12)};
    uint64_t first_address = 0;
    assert(usb_xhci_transfer_ring_enqueue(&ring, first, 3, &first_address) == USB_OK);
    assert(first_address == ring.trbs.address && ring.producer_index == 3 && ring.producer_cycle == 1);
    for (uint32_t i = 0; i < 3; ++i) {
        assert((get_le32(transfer_slot(&ring, i) + 12) & USB_XHCI_TRB_CYCLE) != 0);
        assert(get_le32(transfer_slot(&ring, i)) == 10u + i);
    }
    uint32_t completed = 0;
    assert(usb_xhci_transfer_ring_complete_through(&ring, ring.trbs.address + USB_XHCI_TRB_BYTES, &completed) == USB_OK);
    assert(completed == 2 && ring.consumer_index == 2 && ring.outstanding == 1);

    struct usb_xhci_trb wrap[6];
    for (uint32_t i = 0; i < 6; ++i) wrap[i] = make_trb(USB_XHCI_TRB_NORMAL, 20u + i);
    assert(usb_xhci_transfer_ring_enqueue(&ring, wrap, 6, NULL) == USB_OK);
    assert(ring.producer_index == 2 && ring.producer_cycle == 0 && ring.outstanding == 7);
    assert((get_le32(transfer_slot(&ring, 3) + 12) & USB_XHCI_TRB_CYCLE) != 0);
    assert((get_le32(transfer_slot(&ring, 6) + 12) & USB_XHCI_TRB_CYCLE) != 0);
    assert(!(get_le32(transfer_slot(&ring, 0) + 12) & USB_XHCI_TRB_CYCLE));
    assert(!(get_le32(transfer_slot(&ring, 1) + 12) & USB_XHCI_TRB_CYCLE));
    assert(get_le32(transfer_slot(&ring, 7) + 12) & USB_XHCI_TRB_CYCLE);
    assert(usb_xhci_transfer_ring_enqueue(&ring, first, 1, NULL) == USB_BUSY);
    assert(usb_xhci_transfer_ring_complete_through(&ring, ring.trbs.address + 6u * USB_XHCI_TRB_BYTES,
                                                   &completed) == USB_OK);
    assert(completed == 5 && ring.consumer_index == 0 && ring.outstanding == 2);
    assert(usb_xhci_transfer_ring_complete_through(&ring, ring.trbs.address + USB_XHCI_TRB_BYTES,
                                                   &completed) == USB_OK);
    assert(completed == 2 && ring.consumer_index == 2 && ring.outstanding == 0);

    assert(usb_xhci_transfer_ring_enqueue(&ring, first, 1, NULL) == USB_OK);
    assert(!(get_le32(transfer_slot(&ring, 2) + 12) & USB_XHCI_TRB_CYCLE));
    assert(get_le32(transfer_slot(&ring, 3) + 12) & USB_XHCI_TRB_CYCLE); /* Invalid producer tail. */
    assert(m.syncs == 10u);
    for (unsigned i = 0; i < m.syncs; ++i)
        assert_sync_record(&m, i, &ring.trbs, 0, ring.trbs.size, USB_DMA_TO_DEVICE, true);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_BUSY);
    assert(m.releases == 0);
    assert(usb_xhci_transfer_ring_halted(&ring) == USB_OK);
    assert(!ring.controller_active && ring.outstanding == 0 && ring.producer_cycle == 1);
    assert(m.syncs == 11u);
    assert_sync_record(&m, 10, &ring.trbs, 0, ring.trbs.size, USB_DMA_TO_DEVICE, true);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_OK && m.releases == 1);
}

static void test_transfer_ring_fault_retention(void)
{
    struct dma_model m; initialize_dma(&m); struct usb_dma_ops dma = dma_ops(&m);
    struct usb_xhci_transfer_ring ring;
    assert(usb_xhci_transfer_ring_init(&ring, &dma, 8) == USB_OK);
    assert(usb_xhci_transfer_ring_mark_active(&ring) == USB_OK);
    struct usb_xhci_trb trb = make_trb(USB_XHCI_TRB_NOOP_COMMAND, 0);
    m.fail_sync = m.syncs + 2u; /* Revoke succeeds; data/tail publication fails. */
    assert(usb_xhci_transfer_ring_enqueue(&ring, &trb, 1, NULL) == USB_IO);
    assert(ring.faulted && ring.controller_active && ring.outstanding == ring.usable_entries);
    assert(m.syncs == 3u);
    for (unsigned i = 0; i < m.syncs; ++i)
        assert_sync_record(&m, i, &ring.trbs, 0, ring.trbs.size, USB_DMA_TO_DEVICE, true);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_BUSY && m.releases == 0);
    assert(usb_xhci_transfer_ring_halted(&ring) == USB_OK);
    assert(m.syncs == 4u);
    assert_sync_record(&m, 3, &ring.trbs, 0, ring.trbs.size, USB_DMA_TO_DEVICE, true);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_OK && m.releases == 1);

    initialize_dma(&m); m.noncoherent = true; dma = dma_ops(&m);
    assert(usb_xhci_transfer_ring_init(&ring, &dma, 8) == USB_UNSUPPORTED);
    assert(m.allocations == 1 && m.releases == 1);

    initialize_dma(&m); dma = dma_ops(&m);
    assert(usb_xhci_transfer_ring_init(&ring, &dma, 8) == USB_OK);
    assert(usb_xhci_transfer_ring_mark_active(&ring) == USB_OK);
    assert(usb_xhci_transfer_ring_enqueue(&ring, &trb, 1, NULL) == USB_OK);
    uint32_t completed = 0;
    assert(usb_xhci_transfer_ring_complete_through(&ring,
        ring.trbs.address + 2u * USB_XHCI_TRB_BYTES, &completed) == USB_INVALID);
    assert(ring.faulted && usb_xhci_transfer_ring_destroy(&ring) == USB_BUSY);
    assert(usb_xhci_transfer_ring_halted(&ring) == USB_OK);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_OK);
    puts("PASS: xHCI transfer ring wrap, ownership and stop proof");
}

static void simulate_event(struct usb_xhci_event_ring *ring, uint32_t index, uint8_t cycle, uint32_t value)
{
    uint8_t *event = (uint8_t *)ring->trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES;
    put_le32(event, value); put_le32(event + 8, value ^ 0xa55aa55au);
    put_le32(event + 12, (USB_XHCI_TRB_TRANSFER_EVENT << USB_XHCI_TRB_TYPE_SHIFT) |
                          (cycle ? USB_XHCI_TRB_CYCLE : 0u));
}

static void test_event_ring_cycle_and_stop(void)
{
    struct dma_model m; initialize_dma(&m); struct usb_dma_ops dma = dma_ops(&m);
    struct usb_xhci_event_ring ring;
    assert(usb_xhci_event_ring_init(&ring, &dma, 16) == USB_OK);
    assert(m.syncs == 2u);
    assert_sync_record(&m, 0, &ring.erst, 0, 16u, USB_DMA_TO_DEVICE, true);
    assert_sync_record(&m, 1, &ring.trbs, 0, ring.trbs.size, USB_DMA_FROM_DEVICE, true);
    uint64_t erst_address = 0, dequeue_address = 0;
    assert(usb_xhci_event_ring_erst_address(&ring, &erst_address) == USB_OK && erst_address == ring.erst.address);
    const uint8_t *erst = ring.erst.cpu;
    assert(get_le32(erst) == (uint32_t)ring.trbs.address && get_le32(erst + 4) == 0 && get_le32(erst + 8) == 16);
    assert(usb_xhci_event_ring_mark_active(&ring) == USB_OK);
    struct usb_xhci_trb event = {{0}};
    assert(usb_xhci_event_ring_read(&ring, &event) == USB_AGAIN);
    assert_sync_record(&m, 2, &ring.trbs, 0, USB_XHCI_TRB_BYTES, USB_DMA_FROM_DEVICE, false);
    for (uint32_t i = 0; i < 16; ++i) simulate_event(&ring, i, 1, 100u + i);
    for (uint32_t i = 0; i < 16; ++i) {
        assert(usb_xhci_event_ring_read(&ring, &event) == USB_OK);
        assert(get_le32(event.bytes) == 100u + i);
        assert_sync_record(&m, 3u + i, &ring.trbs, (size_t)i * USB_XHCI_TRB_BYTES,
                           USB_XHCI_TRB_BYTES, USB_DMA_FROM_DEVICE, false);
    }
    assert(ring.dequeue_index == 0 && ring.consumer_cycle == 0);
    assert(usb_xhci_event_ring_read(&ring, &event) == USB_AGAIN);
    assert_sync_record(&m, 19, &ring.trbs, 0, USB_XHCI_TRB_BYTES, USB_DMA_FROM_DEVICE, false);
    simulate_event(&ring, 0, 0, 999u);
    assert(usb_xhci_event_ring_read(&ring, &event) == USB_OK && get_le32(event.bytes) == 999u);
    assert_sync_record(&m, 20, &ring.trbs, 0, USB_XHCI_TRB_BYTES, USB_DMA_FROM_DEVICE, false);
    assert(usb_xhci_event_ring_dequeue_address(&ring, &dequeue_address) == USB_OK);
    assert(dequeue_address == ring.trbs.address + USB_XHCI_TRB_BYTES);
    m.fail_sync = m.syncs + 1u;
    assert(usb_xhci_event_ring_read(&ring, &event) == USB_IO);
    assert(m.syncs == 22u);
    assert_sync_record(&m, 21, &ring.trbs, USB_XHCI_TRB_BYTES, USB_XHCI_TRB_BYTES,
                       USB_DMA_FROM_DEVICE, false);
    assert(ring.faulted && usb_xhci_event_ring_destroy(&ring) == USB_BUSY && m.releases == 0);
    assert(usb_xhci_event_ring_halted(&ring) == USB_OK);
    assert(usb_xhci_event_ring_destroy(&ring) == USB_OK && m.releases == 2);
    puts("PASS: xHCI event ring cycle and stop proof");
}

int main(void)
{
    test_capability_discovery();
    test_transfer_ring_cycle_wrap_and_completion();
    test_transfer_ring_fault_retention();
    test_event_ring_cycle_and_stop();
    return 0;
}
