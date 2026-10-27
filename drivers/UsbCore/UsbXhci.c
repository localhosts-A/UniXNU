/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbXhci.h"

#define XHCI_CAP_LENGTH 0x00u
#define XHCI_HCSPARAMS1 0x04u
#define XHCI_HCCPARAMS1 0x10u
#define XHCI_DBOFF 0x14u
#define XHCI_RTSOFF 0x18u
#define XHCI_CAP_MIN_BYTES 0x20u
#define XHCI_OP_PORTSC 0x400u
#define XHCI_OP_PAGESIZE 0x08u
#define XHCI_RUNTIME_MIN_BYTES 0x40u
#define XHCI_DOORBELL_BYTES_PER_SLOT 4u
#define XHCI_EXT_CAP_SUPPORTED_PROTOCOL 2u
#define XHCI_EXT_CAP_USB_NAME 0x20425355u
#define XHCI_EXT_CAP_MAX_COUNT 256u

static void zero_bytes(void *memory, size_t size)
{
    uint8_t *bytes = memory;
    while (size--) *bytes++ = 0;
}

static void copy_bytes(void *destination, const void *source, size_t size)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    while (size--) *out++ = *in++;
}

static uint32_t get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static bool range_inside(uint32_t aperture, uint32_t offset, uint32_t length)
{
    return length != 0 && offset <= aperture && length <= aperture - offset;
}

static bool ranges_overlap(uint32_t first, uint32_t first_size, uint32_t second, uint32_t second_size)
{
    uint64_t first_end = (uint64_t)first + first_size;
    uint64_t second_end = (uint64_t)second + second_size;
    return (uint64_t)first < second_end && (uint64_t)second < first_end;
}

/* xHCI 1.2 sections 4.9 and 6.5: a ring segment must fit within a
 * 64 KiB region; event segments also reserve complete 64-byte cache lines.
 * The next power-of-two alignment is at most 64 KiB and divides that region,
 * so an allocation of this size cannot straddle its boundary. */
static size_t ring_segment_alignment(size_t bytes)
{
    size_t alignment = 64u;
    while (alignment < bytes) alignment *= 2u;
    return alignment;
}

static int read_reg(const struct usb_mmio *mmio, uint32_t offset, uint32_t *value)
{
    if (!range_inside(mmio->size, offset, 4u)) return USB_RANGE;
    return usb_mmio_read(mmio, offset, value);
}

static bool safe_extended_range(const struct usb_mmio *mmio, const struct usb_xhci_capabilities *caps,
                               uint32_t offset, uint32_t length)
{
    uint32_t doorbell_size = ((uint32_t)caps->max_slots + 1u) * XHCI_DOORBELL_BYTES_PER_SLOT;
    uint32_t operational_size = XHCI_OP_PORTSC + (uint32_t)caps->max_ports * 0x10u;
    if (offset < XHCI_CAP_MIN_BYTES || !range_inside(mmio->size, offset, length)) return false;
    if (ranges_overlap(offset, length, caps->operational_offset, operational_size) ||
        ranges_overlap(offset, length, caps->doorbell_offset, doorbell_size) ||
        ranges_overlap(offset, length, caps->runtime_offset, XHCI_RUNTIME_MIN_BYTES)) return false;
    return true;
}

static int read_usb2_protocols(const struct usb_mmio *mmio, uint32_t extended_offset,
                               const struct usb_xhci_capabilities *layout,
                               struct usb_xhci_capabilities *caps)
{
    uint32_t visited[XHCI_EXT_CAP_MAX_COUNT];
    uint32_t count = 0;
    uint32_t offset = extended_offset;
    bool terminated = false;
    uint8_t protocol_ports[USB_XHCI_PORT_BITMAP_BYTES] = {0};

    if (!extended_offset) return USB_UNSUPPORTED;
    for (uint32_t step = 0; step < XHCI_EXT_CAP_MAX_COUNT; ++step) {
        uint32_t header, id, next, name, port_info;
        int result;
        if ((offset & 3u) || !safe_extended_range(mmio, layout, offset, 4u)) return USB_RANGE;
        for (uint32_t i = 0; i < count; ++i) if (visited[i] == offset) return USB_INVALID;
        visited[count++] = offset;
        result = read_reg(mmio, offset, &header);
        if (result) return result;
        id = header & 0xffu;
        next = (header >> 8) & 0xffu;
        if (id == XHCI_EXT_CAP_SUPPORTED_PROTOCOL) {
            uint8_t major = (uint8_t)(header >> 24);
            if (!safe_extended_range(mmio, layout, offset, 12u)) return USB_RANGE;
            result = read_reg(mmio, offset + 4u, &name);
            if (result) return result;
            result = read_reg(mmio, offset + 8u, &port_info);
            if (result) return result;
            if (name == XHCI_EXT_CAP_USB_NAME) {
                uint32_t first_port = port_info & 0xffu;
                uint32_t port_count = (port_info >> 8) & 0xffu;
                if (!first_port || !port_count || first_port + port_count - 1u > layout->max_ports)
                    return USB_RANGE;
                for (uint32_t port = first_port; port < first_port + port_count; ++port) {
                    uint32_t bit = port - 1u;
                    uint8_t mask = (uint8_t)(1u << (bit & 7u));
                    uint8_t *all_word = &protocol_ports[bit >> 3];
                    if (*all_word & mask) return USB_INVALID;
                    *all_word = (uint8_t)(*all_word | mask);
                    if (major == 2u) {
                        uint8_t *usb2_word = &caps->usb2_ports[bit >> 3];
                        if (*usb2_word & mask) return USB_INVALID;
                        *usb2_word = (uint8_t)(*usb2_word | mask);
                    }
                }
                if (major == 2u) {
                    if (port_count > (uint32_t)UINT8_MAX - caps->usb2_port_count) return USB_RANGE;
                    caps->usb2_port_count = (uint8_t)(caps->usb2_port_count + port_count);
                }
            }
        }
        if (!next) { terminated = true; break; }
        if (next > (UINT32_MAX - offset) / 4u) return USB_RANGE;
        uint32_t following = offset + next * 4u;
        if (following <= offset) return USB_INVALID;
        offset = following;
    }
    if (!terminated) return USB_RANGE;
    return caps->usb2_port_count ? USB_OK : USB_UNSUPPORTED;
}

int usb_xhci_read_capabilities(const struct usb_mmio *mmio, struct usb_xhci_capabilities *out)
{
    struct usb_xhci_capabilities caps;
    uint32_t cap_version, hcsp1, hcc1, db_off, rt_off, page_mask;
    uint32_t op_offset, doorbell_offset, runtime_offset, extended_dwords;
    uint32_t doorbell_size, port_size;
    int result;
    if (!mmio || !out || mmio->size < XHCI_CAP_MIN_BYTES) return USB_INVALID;

    result = read_reg(mmio, XHCI_CAP_LENGTH, &cap_version);
    if (result) return result;
    result = read_reg(mmio, XHCI_HCSPARAMS1, &hcsp1);
    if (result) return result;
    result = read_reg(mmio, XHCI_HCCPARAMS1, &hcc1);
    if (result) return result;
    result = read_reg(mmio, XHCI_DBOFF, &db_off);
    if (result) return result;
    result = read_reg(mmio, XHCI_RTSOFF, &rt_off);
    if (result) return result;

    uint8_t cap_length = (uint8_t)cap_version;
    uint16_t version = (uint16_t)(cap_version >> 16);
    uint8_t slots = (uint8_t)hcsp1;
    uint16_t interrupts = (uint16_t)((hcsp1 >> 8) & 0x7ffu);
    uint8_t ports = (uint8_t)(hcsp1 >> 24);
    op_offset = cap_length;
    doorbell_offset = db_off & ~3u;
    runtime_offset = rt_off & ~0x1fu;
    extended_dwords = hcc1 >> 16;

    if (version < 0x0100u || cap_length < XHCI_CAP_MIN_BYTES || (cap_length & 3u) ||
        !slots || !interrupts || !ports || (db_off & 3u) || (rt_off & 0x1fu)) return USB_INVALID;
    if (extended_dwords > UINT32_MAX / 4u) return USB_RANGE;
    uint32_t extended_offset = extended_dwords * 4u;
    doorbell_size = ((uint32_t)slots + 1u) * XHCI_DOORBELL_BYTES_PER_SLOT;
    port_size = (uint32_t)ports * 0x10u;
    if (doorbell_offset < op_offset || runtime_offset < op_offset ||
        !range_inside(mmio->size, op_offset, 0x0cu) ||
        !range_inside(mmio->size, op_offset + XHCI_OP_PORTSC, port_size) ||
        !range_inside(mmio->size, doorbell_offset, doorbell_size) ||
        !range_inside(mmio->size, runtime_offset, XHCI_RUNTIME_MIN_BYTES) ||
        ranges_overlap(doorbell_offset, doorbell_size, runtime_offset, XHCI_RUNTIME_MIN_BYTES) ||
        ranges_overlap(doorbell_offset, doorbell_size, op_offset, XHCI_OP_PORTSC + port_size) ||
        ranges_overlap(runtime_offset, XHCI_RUNTIME_MIN_BYTES, op_offset, XHCI_OP_PORTSC + port_size)) return USB_RANGE;

    result = read_reg(mmio, op_offset + XHCI_OP_PAGESIZE, &page_mask);
    if (result) return result;
    if (page_mask & ~0xffffu) return USB_INVALID;
    if (!(page_mask & 1u)) return USB_UNSUPPORTED;

    zero_bytes(&caps, sizeof(caps));
    caps.version = version;
    caps.max_slots = slots;
    caps.max_interrupts = interrupts;
    caps.max_ports = ports;
    caps.context_bytes = (hcc1 & (1u << 2)) ? 64u : 32u;
    caps.address_64bit = (hcc1 & 1u) != 0;
    caps.operational_offset = op_offset;
    caps.doorbell_offset = doorbell_offset;
    caps.runtime_offset = runtime_offset;
    caps.page_size_mask = page_mask;
    result = read_usb2_protocols(mmio, extended_offset, &caps, &caps);
    if (result) return result;
    copy_bytes(out, &caps, sizeof(caps));
    return USB_OK;
}

static bool valid_trb_type(uint32_t control)
{
    uint32_t type = (control >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK;
    switch (type) {
    case USB_XHCI_TRB_NORMAL:
    case USB_XHCI_TRB_SETUP_STAGE:
    case USB_XHCI_TRB_DATA_STAGE:
    case USB_XHCI_TRB_STATUS_STAGE:
    case USB_XHCI_TRB_ISOCH:
    case USB_XHCI_TRB_ENABLE_SLOT:
    case USB_XHCI_TRB_DISABLE_SLOT:
    case USB_XHCI_TRB_ADDRESS_DEVICE:
    case USB_XHCI_TRB_CONFIGURE_ENDPOINT:
    case USB_XHCI_TRB_EVALUATE_CONTEXT:
    case USB_XHCI_TRB_RESET_ENDPOINT:
    case USB_XHCI_TRB_STOP_ENDPOINT:
    case USB_XHCI_TRB_SET_TR_DEQUEUE:
    case USB_XHCI_TRB_RESET_DEVICE:
    case USB_XHCI_TRB_NOOP_COMMAND:
        return true;
    default:
        return false;
    }
}

static void set_link_trb(struct usb_xhci_transfer_ring *ring, uint8_t cycle)
{
    uint8_t *link = (uint8_t *)ring->trbs.cpu + (size_t)ring->usable_entries * USB_XHCI_TRB_BYTES;
    put_le32(link, (uint32_t)ring->trbs.address);
    put_le32(link + 4, (uint32_t)(ring->trbs.address >> 32));
    put_le32(link + 8, 0);
    put_le32(link + 12, (USB_XHCI_TRB_LINK << USB_XHCI_TRB_TYPE_SHIFT) |
                         USB_XHCI_TRB_TOGGLE_CYCLE | (cycle ? USB_XHCI_TRB_CYCLE : 0u));
}

static int sync_transfer_ring(struct usb_xhci_transfer_ring *ring)
{
    return usb_dma_sync(&ring->dma, &ring->trbs, 0, ring->trbs.size, USB_DMA_TO_DEVICE, true);
}

static int transfer_ring_fail(struct usb_xhci_transfer_ring *ring, int result)
{
    ring->faulted = true;
    if (ring->controller_active) ring->outstanding = ring->usable_entries;
    return result;
}

int usb_xhci_transfer_ring_init(struct usb_xhci_transfer_ring *ring, const struct usb_dma_ops *dma,
                                uint32_t entries)
{
    if (!ring || !dma) return USB_INVALID;
    zero_bytes(ring, sizeof(*ring));
    if (entries < USB_XHCI_MIN_TRANSFER_RING_ENTRIES || entries > USB_XHCI_MAX_TRANSFER_RING_ENTRIES)
        return USB_RANGE;
    ring->dma = *dma;
    size_t bytes = (size_t)entries * USB_XHCI_TRB_BYTES;
    int result = usb_dma_alloc32(&ring->dma, bytes, ring_segment_alignment(bytes), &ring->trbs);
    if (result) { zero_bytes(ring, sizeof(*ring)); return result; }
    if (!ring->trbs.coherent) {
        usb_dma_free(&ring->dma, &ring->trbs);
        zero_bytes(ring, sizeof(*ring));
        return USB_UNSUPPORTED;
    }
    ring->entries = entries;
    ring->usable_entries = entries - 1u;
    ring->producer_cycle = 1u;
    set_link_trb(ring, 0u);
    result = sync_transfer_ring(ring);
    if (result) {
        usb_dma_free(&ring->dma, &ring->trbs);
        zero_bytes(ring, sizeof(*ring));
        return result;
    }
    return USB_OK;
}

int usb_xhci_transfer_ring_mark_active(struct usb_xhci_transfer_ring *ring)
{
    if (!ring || !ring->trbs.cpu || ring->controller_active) return USB_INVALID;
    if (ring->faulted) return USB_IO;
    if (ring->producer_index >= ring->usable_entries || ring->consumer_index >= ring->usable_entries ||
        ring->outstanding > ring->usable_entries || ring->producer_cycle > 1u) return USB_INVALID;
    ring->controller_active = true;
    return USB_OK;
}

int usb_xhci_transfer_ring_enqueue(struct usb_xhci_transfer_ring *ring, const struct usb_xhci_trb *items,
                                   size_t count, uint64_t *first_address)
{
    uint32_t index, control, link_control, new_index, new_outstanding;
    uint8_t cycle, link_cycle = 0;
    bool link_changed = false;
    int result;
    if (!ring || !ring->trbs.cpu || !items || !count) return USB_INVALID;
    if (ring->faulted) return USB_IO;
    if (ring->producer_index >= ring->usable_entries || ring->consumer_index >= ring->usable_entries ||
        ring->outstanding > ring->usable_entries || ring->producer_cycle > 1u) return USB_INVALID;
    if (count > (size_t)(ring->usable_entries - ring->outstanding)) return USB_BUSY;
    for (size_t i = 0; i < count; ++i) {
        control = get_le32(items[i].bytes + 12);
        if (!valid_trb_type(control)) return USB_INVALID;
    }

    /* Revoke old cycle ownership before rewriting a slot. This matters when a
     * ring has wrapped more than once and the old cycle bit matches the new one. */
    index = ring->producer_index;
    cycle = ring->producer_cycle;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *slot = (uint8_t *)ring->trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES;
        control = get_le32(slot + 12);
        control = (control & ~USB_XHCI_TRB_CYCLE) | (cycle ? 0u : USB_XHCI_TRB_CYCLE);
        put_le32(slot + 12, control);
        if (++index == ring->usable_entries) {
            index = 0;
            cycle ^= 1u;
        }
    }
    result = sync_transfer_ring(ring);
    if (result) return transfer_ring_fail(ring, result);

    index = ring->producer_index;
    cycle = ring->producer_cycle;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *slot = (uint8_t *)ring->trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES;
        copy_bytes(slot, items[i].bytes, 12u);
        control = get_le32(items[i].bytes + 12);
        control = (control & ~USB_XHCI_TRB_CYCLE) | (cycle ? 0u : USB_XHCI_TRB_CYCLE);
        put_le32(slot + 12, control);
        if (++index == ring->usable_entries) {
            link_cycle = cycle;
            link_changed = true;
            index = 0;
            cycle ^= 1u;
        }
    }
    if (link_changed) {
        link_control = get_le32((uint8_t *)ring->trbs.cpu +
                                (size_t)ring->usable_entries * USB_XHCI_TRB_BYTES + 12u);
        link_control = (link_control & ~USB_XHCI_TRB_CYCLE) |
                       (link_cycle ? USB_XHCI_TRB_CYCLE : 0u);
        put_le32((uint8_t *)ring->trbs.cpu + (size_t)ring->usable_entries * USB_XHCI_TRB_BYTES + 12u,
                 link_control);
    }

    /* Keep the next free TRB cycle-invalid so the controller stops at the
     * producer even when that slot contains a descriptor from an older lap. */
    new_index = index;
    new_outstanding = ring->outstanding + (uint32_t)count;
    if (new_outstanding < ring->usable_entries) {
        uint8_t *tail = (uint8_t *)ring->trbs.cpu + (size_t)new_index * USB_XHCI_TRB_BYTES;
        control = get_le32(tail + 12);
        control = (control & ~USB_XHCI_TRB_CYCLE) | (cycle ? 0u : USB_XHCI_TRB_CYCLE);
        put_le32(tail + 12, control);
    }
    result = sync_transfer_ring(ring);
    if (result) return transfer_ring_fail(ring, result);

    index = ring->producer_index;
    cycle = ring->producer_cycle;
    uint64_t first_dma = ring->trbs.address + (uint64_t)index * USB_XHCI_TRB_BYTES;
    /* A running xHC may already be polling this ring from an earlier
     * doorbell. Publish every later TRB first, including across the Link TRB,
     * while the first TRB remains cycle-invalid. The intervening DMA sync is
     * the release barrier before handing over that first TRB. */
    for (size_t i = 0; i < count; ++i) {
        uint8_t *slot = (uint8_t *)ring->trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES;
        if (i != 0) {
            control = get_le32(slot + 12);
            control = (control & ~USB_XHCI_TRB_CYCLE) | (cycle ? USB_XHCI_TRB_CYCLE : 0u);
            put_le32(slot + 12, control);
        }
        if (++index == ring->usable_entries) {
            index = 0;
            cycle ^= 1u;
        }
    }
    result = sync_transfer_ring(ring);
    if (result) return transfer_ring_fail(ring, result);
    uint8_t *first = (uint8_t *)ring->trbs.cpu +
                     (size_t)ring->producer_index * USB_XHCI_TRB_BYTES;
    control = get_le32(first + 12);
    control = (control & ~USB_XHCI_TRB_CYCLE) |
              (ring->producer_cycle ? USB_XHCI_TRB_CYCLE : 0u);
    put_le32(first + 12, control);
    result = sync_transfer_ring(ring);
    if (result) return transfer_ring_fail(ring, result);
    ring->producer_index = index;
    ring->producer_cycle = cycle;
    ring->outstanding = new_outstanding;
    if (first_address) *first_address = first_dma;
    return USB_OK;
}

int usb_xhci_transfer_ring_complete_through(struct usb_xhci_transfer_ring *ring, uint64_t address,
                                            uint32_t *completed_trbs)
{
    if (completed_trbs) *completed_trbs = 0;
    if (!ring || !ring->trbs.cpu || !completed_trbs) return USB_INVALID;
    if (ring->faulted) return USB_IO;
    uint64_t base = ring->trbs.address;
    uint64_t end = base + (uint64_t)ring->usable_entries * USB_XHCI_TRB_BYTES;
    if (address < base || address >= end || ((address - base) & (USB_XHCI_TRB_BYTES - 1u)))
        return transfer_ring_fail(ring, USB_INVALID);
    if (!ring->outstanding) return transfer_ring_fail(ring, USB_INVALID);
    uint32_t index = (uint32_t)((address - base) / USB_XHCI_TRB_BYTES);
    uint32_t distance = (index + ring->usable_entries - ring->consumer_index) % ring->usable_entries;
    if (distance >= ring->outstanding) return transfer_ring_fail(ring, USB_INVALID);
    uint32_t done = distance + 1u;
    ring->consumer_index = (index + 1u) % ring->usable_entries;
    ring->outstanding -= done;
    *completed_trbs = done;
    return USB_OK;
}

int usb_xhci_transfer_ring_complete_event(struct usb_xhci_transfer_ring *ring,
                                          const struct usb_xhci_event *event,
                                          uint32_t *completed_trbs)
{
    if (completed_trbs) *completed_trbs = 0;
    if (!ring || !event || !completed_trbs) return USB_INVALID;
    if (event->kind != USB_XHCI_EVENT_TRANSFER || event->type != USB_XHCI_TRB_TRANSFER_EVENT ||
        event->data.transfer.event_data || !event->data.transfer.trb_pointer_valid ||
        event->completion_code == USB_XHCI_CC_INVALID ||
        event->completion_code == USB_XHCI_CC_RING_UNDERRUN ||
        event->completion_code == USB_XHCI_CC_RING_OVERRUN ||
        event->completion_code == USB_XHCI_CC_VF_EVENT_RING_FULL) return USB_INVALID;
    return usb_xhci_transfer_ring_complete_through(ring, event->data.transfer.pointer_or_data,
                                                   completed_trbs);
}

int usb_xhci_transfer_ring_halted(struct usb_xhci_transfer_ring *ring)
{
    if (!ring || !ring->trbs.cpu || !ring->controller_active) return USB_INVALID;
    ring->controller_active = false;
    ring->faulted = false;
    ring->producer_index = 0;
    ring->consumer_index = 0;
    ring->outstanding = 0;
    ring->producer_cycle = 1u;
    zero_bytes(ring->trbs.cpu, ring->trbs.size);
    set_link_trb(ring, 0u);
    int result = sync_transfer_ring(ring);
    if (result) ring->faulted = true;
    return result;
}

int usb_xhci_transfer_ring_destroy(struct usb_xhci_transfer_ring *ring)
{
    if (!ring) return USB_INVALID;
    if (ring->controller_active) return USB_BUSY;
    usb_dma_free(&ring->dma, &ring->trbs);
    zero_bytes(ring, sizeof(*ring));
    return USB_OK;
}

static int write_erst(struct usb_xhci_event_ring *ring)
{
    uint8_t *entry = ring->erst.cpu;
    zero_bytes(entry, 16u);
    put_le32(entry, (uint32_t)ring->trbs.address);
    put_le32(entry + 4, (uint32_t)(ring->trbs.address >> 32));
    put_le32(entry + 8, ring->entries);
    int result = usb_dma_sync(&ring->dma, &ring->erst, 0, 16u, USB_DMA_TO_DEVICE, true);
    if (result) return result;
    return usb_dma_sync(&ring->dma, &ring->trbs, 0, ring->trbs.size, USB_DMA_FROM_DEVICE, true);
}

int usb_xhci_event_ring_init(struct usb_xhci_event_ring *ring, const struct usb_dma_ops *dma,
                             uint32_t entries)
{
    if (!ring || !dma) return USB_INVALID;
    zero_bytes(ring, sizeof(*ring));
    if (entries < USB_XHCI_MIN_EVENT_RING_ENTRIES || entries > USB_XHCI_MAX_EVENT_RING_ENTRIES)
        return USB_RANGE;
    ring->dma = *dma;
    int result = usb_dma_alloc32(&ring->dma, 64u, 64u, &ring->erst);
    if (result) { zero_bytes(ring, sizeof(*ring)); return result; }
    if (!ring->erst.coherent) {
        usb_dma_free(&ring->dma, &ring->erst);
        zero_bytes(ring, sizeof(*ring));
        return USB_UNSUPPORTED;
    }
    size_t bytes = ((size_t)entries * USB_XHCI_TRB_BYTES + 63u) & ~(size_t)63u;
    result = usb_dma_alloc32(&ring->dma, bytes, ring_segment_alignment(bytes), &ring->trbs);
    if (result) {
        usb_dma_free(&ring->dma, &ring->erst);
        zero_bytes(ring, sizeof(*ring));
        return result;
    }
    if (!ring->trbs.coherent) {
        usb_dma_free(&ring->dma, &ring->trbs);
        usb_dma_free(&ring->dma, &ring->erst);
        zero_bytes(ring, sizeof(*ring));
        return USB_UNSUPPORTED;
    }
    ring->entries = entries;
    ring->consumer_cycle = 1u;
    result = write_erst(ring);
    if (result) {
        usb_dma_free(&ring->dma, &ring->trbs);
        usb_dma_free(&ring->dma, &ring->erst);
        zero_bytes(ring, sizeof(*ring));
        return result;
    }
    return USB_OK;
}

int usb_xhci_event_ring_mark_active(struct usb_xhci_event_ring *ring)
{
    if (!ring || !ring->trbs.cpu || ring->controller_active) return USB_INVALID;
    if (ring->faulted) return USB_IO;
    ring->controller_active = true;
    return USB_OK;
}

int usb_xhci_event_ring_read(struct usb_xhci_event_ring *ring, struct usb_xhci_trb *event)
{
    if (!ring || !event || !ring->trbs.cpu || !ring->controller_active) return USB_INVALID;
    if (ring->faulted) return USB_IO;
    size_t offset = (size_t)ring->dequeue_index * USB_XHCI_TRB_BYTES;
    int result = usb_dma_sync(&ring->dma, &ring->trbs, offset, USB_XHCI_TRB_BYTES,
                              USB_DMA_FROM_DEVICE, false);
    if (result) { ring->faulted = true; return result; }
    const uint8_t *slot = (const uint8_t *)ring->trbs.cpu + offset;
    uint32_t control = get_le32(slot + 12);
    uint8_t cycle = (control & USB_XHCI_TRB_CYCLE) ? 1u : 0u;
    if (cycle != ring->consumer_cycle) return USB_AGAIN;
    copy_bytes(event->bytes, slot, USB_XHCI_TRB_BYTES);
    if (++ring->dequeue_index == ring->entries) {
        ring->dequeue_index = 0;
        ring->consumer_cycle ^= 1u;
    }
    return USB_OK;
}

int usb_xhci_event_decode(const struct usb_xhci_trb *trb, struct usb_xhci_event *event)
{
    struct usb_xhci_event decoded;
    uint32_t dword0, dword1, dword2, dword3;
    uint64_t parameter;

    if (!trb || !event) return USB_INVALID;
    zero_bytes(&decoded, sizeof(decoded));
    copy_bytes(decoded.raw.bytes, trb->bytes, USB_XHCI_TRB_BYTES);
    dword0 = get_le32(decoded.raw.bytes);
    dword1 = get_le32(decoded.raw.bytes + 4u);
    dword2 = get_le32(decoded.raw.bytes + 8u);
    dword3 = get_le32(decoded.raw.bytes + 12u);
    decoded.type = (uint8_t)((dword3 >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK);
    decoded.cycle = (dword3 & USB_XHCI_TRB_CYCLE) ? 1u : 0u;
    decoded.completion_code = (uint8_t)(dword2 >> 24);
    parameter = ((uint64_t)dword1 << 32) | dword0;

    switch (decoded.type) {
    case USB_XHCI_TRB_TRANSFER_EVENT:
        decoded.kind = USB_XHCI_EVENT_TRANSFER;
        decoded.data.transfer.pointer_or_data = parameter;
        decoded.data.transfer.residual_bytes = dword2 & 0x00ffffffu;
        decoded.data.transfer.event_data = (dword3 & USB_XHCI_TRB_EVENT_DATA) != 0;
        decoded.data.transfer.endpoint_id = (uint8_t)((dword3 >> 16) & 0x1fu);
        decoded.data.transfer.slot_id = (uint8_t)(dword3 >> 24);
        decoded.data.transfer.trb_pointer_valid =
            !decoded.data.transfer.event_data &&
            decoded.completion_code != USB_XHCI_CC_INVALID &&
            decoded.completion_code != USB_XHCI_CC_RING_UNDERRUN &&
            decoded.completion_code != USB_XHCI_CC_RING_OVERRUN &&
            decoded.completion_code != USB_XHCI_CC_VF_EVENT_RING_FULL;
        break;
    case USB_XHCI_TRB_COMMAND_EVENT:
        decoded.kind = USB_XHCI_EVENT_COMMAND_COMPLETION;
        decoded.data.command.command_trb_pointer = parameter & ~(uint64_t)0x0fu;
        decoded.data.command.completion_parameter = dword2 & 0x00ffffffu;
        decoded.data.command.vf_id = (uint8_t)((dword3 >> 16) & 0xffu);
        decoded.data.command.slot_id = (uint8_t)(dword3 >> 24);
        decoded.data.command.command_trb_pointer_valid =
            (parameter & 0x0fu) == 0 &&
            decoded.completion_code != USB_XHCI_CC_INVALID &&
            decoded.completion_code != USB_XHCI_CC_COMMAND_RING_STOPPED;
        break;
    case USB_XHCI_TRB_PORT_STATUS_EVENT:
        decoded.kind = USB_XHCI_EVENT_PORT_STATUS_CHANGE;
        decoded.data.port_status.port_id = (uint8_t)(dword0 >> 24);
        break;
    case USB_XHCI_TRB_HOST_CONTROLLER_EVENT:
        decoded.kind = USB_XHCI_EVENT_HOST_CONTROLLER;
        break;
    default:
        decoded.kind = USB_XHCI_EVENT_RAW;
        break;
    }

    copy_bytes(event, &decoded, sizeof(decoded));
    return USB_OK;
}

int usb_xhci_event_ring_dequeue_address(const struct usb_xhci_event_ring *ring, uint64_t *address)
{
    if (!ring || !address || !ring->trbs.cpu || ring->dequeue_index >= ring->entries) return USB_INVALID;
    *address = ring->trbs.address + (uint64_t)ring->dequeue_index * USB_XHCI_TRB_BYTES;
    return USB_OK;
}

int usb_xhci_event_ring_erst_address(const struct usb_xhci_event_ring *ring, uint64_t *address)
{
    if (!ring || !address || !ring->erst.cpu) return USB_INVALID;
    *address = ring->erst.address;
    return USB_OK;
}

int usb_xhci_event_ring_halted(struct usb_xhci_event_ring *ring)
{
    if (!ring || !ring->trbs.cpu || !ring->erst.cpu || !ring->controller_active) return USB_INVALID;
    ring->controller_active = false;
    ring->faulted = false;
    ring->dequeue_index = 0;
    ring->consumer_cycle = 1u;
    zero_bytes(ring->trbs.cpu, ring->trbs.size);
    int result = write_erst(ring);
    if (result) ring->faulted = true;
    return result;
}

int usb_xhci_event_ring_destroy(struct usb_xhci_event_ring *ring)
{
    if (!ring) return USB_INVALID;
    if (ring->controller_active) return USB_BUSY;
    usb_dma_free(&ring->dma, &ring->trbs);
    usb_dma_free(&ring->dma, &ring->erst);
    zero_bytes(ring, sizeof(*ring));
    return USB_OK;
}
