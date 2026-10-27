/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbXhci.h"

#define HCSPARAMS2 0x08u
#define OP_CMD 0x00u
#define OP_STS 0x04u
#define OP_CRCR 0x18u
#define OP_DCBAAP 0x30u
#define OP_CONFIG 0x38u
#define RT_IMAN 0x20u
#define RT_ERSTSZ 0x28u
#define RT_ERSTBA 0x30u
#define RT_ERDP 0x38u
#define CMD_RUN 0x01u
#define CMD_RESET 0x02u
#define CMD_EIE 0x04u
#define STS_HALTED 0x01u
#define STS_NOT_READY 0x0800u
#define STS_ERROR 0x1004u
#define RESET_TIMEOUT_US 10000000u
#define HALT_TIMEOUT_US 32000u
#define POLL_US 1000u

static void zero_bytes(void *memory, size_t size)
{
    uint8_t *p = memory;
    while (size--) *p++ = 0;
}

static void put_le64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static void put_le32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static void copy_bytes(void *dst, const void *src, size_t length)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (length--) *d++ = *s++;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct guarded_probe {
    const struct usb_mmio *mmio;
    int (*guard)(void *);
    void *guard_context;
};

static int probe_read(void *context, uint32_t offset, uint32_t *value)
{
    struct guarded_probe *probe = context;
    int result = probe->guard(probe->guard_context);
    return result ? result : usb_mmio_read(probe->mmio, offset, value);
}

static int guarded_read(struct usb_xhci_controller *c, uint32_t offset, uint32_t *value)
{
    int result = c->guard(c->guard_context);
    return result ? result : usb_mmio_read(&c->mmio, offset, value);
}

static int guarded_write(struct usb_xhci_controller *c, uint32_t offset, uint32_t value)
{
    int result = c->guard(c->guard_context);
    return result ? result : usb_mmio_write(&c->mmio, offset, value);
}

static int wait_bits(struct usb_xhci_controller *c, uint32_t offset, uint32_t mask,
                     uint32_t expected, uint32_t timeout_us)
{
    uint32_t elapsed = 0;
    for (;;) {
        uint32_t value;
        int result = guarded_read(c, offset, &value);
        if (result) return result;
        if ((value & mask) == expected) return USB_OK;
        if (elapsed == timeout_us) return USB_TIMEOUT;
        uint32_t delay = timeout_us - elapsed < POLL_US ? timeout_us - elapsed : POLL_US;
        c->mmio.delay_us(c->mmio.context, delay);
        elapsed += delay;
    }
}

static int write_address(struct usb_xhci_controller *c, uint32_t offset, uint64_t address)
{
    int result = guarded_write(c, offset, (uint32_t)address);
    return result ? result : guarded_write(c, offset + 4u, (uint32_t)(address >> 32));
}

/* An already-issued reset must complete before writing any operational or
 * runtime register. Reissuing a completed reset is safe under the lease. */
static int halt_and_reset(struct usb_xhci_controller *c)
{
    uint32_t op = c->caps.operational_offset, command, status;
    int result = wait_bits(c, op + OP_CMD, CMD_RESET, 0, RESET_TIMEOUT_US);
    if (result) return result;
    result = wait_bits(c, op + OP_STS, STS_NOT_READY, 0, RESET_TIMEOUT_US);
    if (result) return result;
    result = guarded_read(c, op + OP_CMD, &command);
    if (result) return result;
    result = guarded_write(c, op + OP_CMD, command & ~(CMD_RUN | CMD_EIE));
    if (result) return result;
    result = wait_bits(c, op + OP_STS, STS_HALTED, STS_HALTED, HALT_TIMEOUT_US);
    if (result) return result;
    result = guarded_read(c, op + OP_CMD, &command);
    if (result) return result;
    result = guarded_write(c, op + OP_CMD, (command & ~CMD_RUN) | CMD_RESET);
    if (result) return result;
    c->mmio.delay_us(c->mmio.context, 2000u);
    result = wait_bits(c, op + OP_CMD, CMD_RESET, 0, RESET_TIMEOUT_US);
    if (result) return result;
    result = wait_bits(c, op + OP_STS, STS_NOT_READY, 0, RESET_TIMEOUT_US);
    if (result) return result;
    result = guarded_read(c, op + OP_CMD, &command);
    if (result) return result;
    result = guarded_read(c, op + OP_STS, &status);
    if (result) return result;
    return !(command & (CMD_RUN | CMD_RESET)) && (status & STS_HALTED) &&
           !(status & STS_NOT_READY) ? USB_OK : USB_IO;
}

static int alloc_schedule(struct usb_xhci_controller *c, uint8_t slots,
                          uint32_t command_entries, uint32_t event_entries)
{
    size_t dcbaa_size = ((size_t)slots + 1u) * 8u;
    int result = usb_dma_alloc32(&c->dma, dcbaa_size, c->page_bytes, &c->dcbaa);
    if (result) return result;
    if (!c->dcbaa.coherent) return USB_UNSUPPORTED;
    if (c->scratch_count) {
        size_t array_size = ((size_t)c->scratch_count * 8u + 63u) & ~(size_t)63u;
        result = usb_dma_alloc32(&c->dma, array_size, c->page_bytes, &c->scratch_array);
        if (result) return result;
        if (!c->scratch_array.coherent) return USB_UNSUPPORTED;
        for (uint16_t i = 0; i < c->scratch_count; ++i) {
            result = usb_dma_alloc32(&c->dma, c->page_bytes, c->page_bytes, &c->scratch_pages[i]);
            if (result) return result;
            ++c->scratch_allocated;
            result = usb_dma_sync(&c->dma, &c->scratch_pages[i], 0, c->page_bytes,
                                  USB_DMA_BIDIRECTIONAL, true);
            if (result) return result;
            put_le64((uint8_t *)c->scratch_array.cpu + (size_t)i * 8u,
                     c->scratch_pages[i].address);
        }
        result = usb_dma_sync(&c->dma, &c->scratch_array, 0, array_size, USB_DMA_TO_DEVICE, true);
        if (result) return result;
        put_le64(c->dcbaa.cpu, c->scratch_array.address);
    }
    result = usb_dma_sync(&c->dma, &c->dcbaa, 0, dcbaa_size, USB_DMA_TO_DEVICE, true);
    if (result) return result;
    result = usb_xhci_transfer_ring_init(&c->commands, &c->dma, command_entries);
    if (result) return result;
    result = usb_xhci_event_ring_init(&c->events, &c->dma, event_entries);
    if (result) return result;
    result = usb_xhci_transfer_ring_mark_active(&c->commands);
    return result ? result : usb_xhci_event_ring_mark_active(&c->events);
}

static int program_schedule(struct usb_xhci_controller *c, uint8_t slots)
{
    uint32_t op = c->caps.operational_offset, rt = c->caps.runtime_offset, value;
    int result = guarded_read(c, op + OP_CONFIG, &value);
    if (result) return result;
    result = guarded_write(c, op + OP_CONFIG, (value & ~0xffu) | slots);
    if (result) return result;
    result = write_address(c, op + OP_DCBAAP, c->dcbaa.address);
    if (result) return result;
    result = guarded_read(c, op + OP_CRCR, &value);
    if (result) return result;
    /* Preserve reserved bits 5:4; never replay stop/abort command bits. */
    if (value & 0x0eu) return USB_BUSY;
    result = write_address(c, op + OP_CRCR, c->commands.trbs.address | (value & 0x30u) | 1u);
    if (result) return result;
    result = guarded_read(c, rt + RT_IMAN, &value);
    if (result) return result;
    result = guarded_write(c, rt + RT_IMAN, value & ~3u); /* IE=0; IP is W1C. */
    if (result) return result;
    result = guarded_read(c, rt + RT_ERSTSZ, &value);
    if (result) return result;
    result = guarded_write(c, rt + RT_ERSTSZ, (value & 0xffff0000u) | 1u);
    if (result) return result;
    result = write_address(c, rt + RT_ERDP, c->events.trbs.address);
    if (result) return result;
    result = guarded_read(c, rt + RT_ERSTBA, &value);
    if (result) return result;
    return write_address(c, rt + RT_ERSTBA, c->events.erst.address | (value & 0x3fu));
}

int usb_xhci_controller_start(struct usb_xhci_controller *c, const struct usb_mmio *mmio,
                              const struct usb_dma_ops *dma, int (*guard)(void *),
                              void *guard_context, uint8_t slots,
                              uint32_t command_entries, uint32_t event_entries)
{
    uint32_t command, status, hcs2, cap;
    if (!c || !mmio || !dma || !guard || !mmio->read32 || !mmio->write32 ||
        !mmio->barrier || !mmio->delay_us || !slots || !dma->qualified ||
        !dma->allocate || !dma->release || !dma->sync) return USB_INVALID;
    if (c->state != USB_XHCI_CONTROLLER_EMPTY) return USB_BUSY;
    int result = guard(guard_context);
    if (result) return result;
    struct guarded_probe probe = {mmio, guard, guard_context};
    struct usb_mmio checked_mmio = *mmio;
    checked_mmio.context = &probe;
    checked_mmio.read32 = probe_read;
    /* CNR must be clear before capability discovery reads PAGESIZE. */
    result = usb_mmio_read(&checked_mmio, 0, &cap);
    if (result) return result;
    uint32_t op = cap & 0xffu;
    if (op < 0x20u || (op & 3u) || mmio->size < 8u || op > mmio->size - 8u)
        return USB_RANGE;
    result = usb_mmio_read(&checked_mmio, op + OP_STS, &status);
    if (result) return result;
    if ((status & STS_NOT_READY) || !(status & STS_HALTED)) return USB_BUSY;
    result = usb_mmio_read(&checked_mmio, op + OP_CMD, &command);
    if (result) return result;
    if (command & (CMD_RUN | CMD_RESET)) return USB_BUSY;
    struct usb_xhci_capabilities caps;
    result = usb_xhci_read_capabilities(&checked_mmio, &caps);
    if (result) return result;
    if (slots > caps.max_slots || command_entries < USB_XHCI_MIN_TRANSFER_RING_ENTRIES ||
        command_entries > USB_XHCI_MAX_TRANSFER_RING_ENTRIES ||
        event_entries < USB_XHCI_MIN_EVENT_RING_ENTRIES ||
        event_entries > USB_XHCI_MAX_EVENT_RING_ENTRIES) return USB_RANGE;
    result = usb_mmio_read(&checked_mmio, HCSPARAMS2, &hcs2);
    if (result) return result;
    uint16_t scratch_count = (uint16_t)(((hcs2 >> 16) & 0x3e0u) | ((hcs2 >> 27) & 0x1fu));
    size_t scratch_array_size = ((size_t)scratch_count * 8u + 63u) & ~(size_t)63u;
    /* This controller path uses 4 KiB pages. PAGESIZE is read-only; an
     * additional supported-size bit is not proof that the platform has
     * selected a larger active page. Do not let the array cross a page. */
    uint32_t page_bytes = 4096u;
    if (scratch_array_size > page_bytes) return USB_UNSUPPORTED;
    zero_bytes(c, sizeof(*c));
    c->mmio = *mmio; c->dma = *dma; c->caps = caps;
    c->guard = guard; c->guard_context = guard_context;
    c->scratch_count = scratch_count; c->page_bytes = page_bytes;
    c->slot_limit = slots;
    c->state = USB_XHCI_CONTROLLER_FAULTED; /* Reset writes may take effect on error. */
    result = halt_and_reset(c);
    if (result) return result;
    result = alloc_schedule(c, slots, command_entries, event_entries);
    if (result) return result;
    result = program_schedule(c, slots);
    if (result) return result;
    c->acknowledged_event = c->events.trbs.address;
    result = guarded_read(c, caps.operational_offset + OP_CMD, &command);
    if (result) return result;
    result = guarded_write(c, caps.operational_offset + OP_CMD, (command & ~CMD_EIE) | CMD_RUN);
    if (result) return result;
    result = wait_bits(c, caps.operational_offset + OP_STS, STS_HALTED, 0, HALT_TIMEOUT_US);
    if (result) return result;
    result = guarded_read(c, caps.operational_offset + OP_STS, &status);
    if (result) return result;
    if (status & (STS_NOT_READY | STS_ERROR)) return USB_IO;
    c->state = USB_XHCI_CONTROLLER_RUNNING;
    return USB_OK;
}

static int controller_ready(struct usb_xhci_controller *c)
{
    uint32_t status;
    if (!c || c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    int result = guarded_read(c, c->caps.operational_offset + OP_STS, &status);
    if (result || (status & (STS_HALTED | STS_NOT_READY | STS_ERROR))) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result ? result : USB_IO;
    }
    return USB_OK;
}

int usb_xhci_controller_submit_command(struct usb_xhci_controller *c,
                                       const struct usb_xhci_trb *trb)
{
    if (!c || !trb) return USB_INVALID;
    if (c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    if (c->command_pending) return USB_BUSY;
    uint32_t control = (uint32_t)trb->bytes[12] | ((uint32_t)trb->bytes[13] << 8) |
                       ((uint32_t)trb->bytes[14] << 16) | ((uint32_t)trb->bytes[15] << 24);
    uint32_t type = (control >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK;
    if (!((type >= USB_XHCI_TRB_ENABLE_SLOT && type <= USB_XHCI_TRB_RESET_DEVICE) ||
          type == USB_XHCI_TRB_NOOP_COMMAND)) return USB_INVALID;
    int result = controller_ready(c);
    if (result) return result;
    uint64_t address = 0;
    result = usb_xhci_transfer_ring_enqueue(&c->commands, trb, 1u, &address);
    if (result) {
        if (c->commands.faulted) c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result;
    }
    c->pending_command = address;
    c->command_pending = true; /* Publication precedes any uncertain MMIO. */
    result = guarded_write(c, c->caps.doorbell_offset, 0u);
    if (!result) {
        uint32_t posted;
        result = guarded_read(c, c->caps.doorbell_offset, &posted);
    }
    if (result) c->state = USB_XHCI_CONTROLLER_FAULTED;
    return result;
}

static int read_event(struct usb_xhci_controller *c, struct usb_xhci_event *out)
{
    if (!c || !out) return USB_INVALID;
    int result = controller_ready(c);
    if (result) return result;
    uint64_t current;
    result = usb_xhci_event_ring_dequeue_address(&c->events, &current);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
    struct usb_xhci_trb raw;
    result = usb_xhci_event_ring_read(&c->events, &raw);
    if (result != USB_OK && result != USB_AGAIN) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
    /* When another event is present, ERDP may advance only to this handled
     * TRB. When the cycle mismatches, current is the observed empty slot.
     * Never write the same pointer twice. */
    if (current != c->acknowledged_event) {
        int ack = write_address(c, c->caps.runtime_offset + RT_ERDP, current | 8u);
        if (ack) { c->state = USB_XHCI_CONTROLLER_FAULTED; return ack; }
        c->acknowledged_event = current;
    }
    if (result == USB_AGAIN) return result;
    struct usb_xhci_event event;
    result = usb_xhci_event_decode(&raw, &event);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
    if (event.kind == USB_XHCI_EVENT_COMMAND_COMPLETION) {
        if (!c->command_pending || !event.data.command.command_trb_pointer_valid ||
            event.completion_code == USB_XHCI_CC_COMMAND_ABORTED ||
            event.data.command.command_trb_pointer != c->pending_command) {
            c->state = USB_XHCI_CONTROLLER_FAULTED;
            return USB_PHASE_ERROR;
        }
        uint32_t completed = 0;
        result = usb_xhci_transfer_ring_complete_through(&c->commands, c->pending_command, &completed);
        if (result || completed != 1u) {
            c->state = USB_XHCI_CONTROLLER_FAULTED;
            return result ? result : USB_PHASE_ERROR;
        }
        c->command_pending = false;
        c->pending_command = 0;
    } else if (event.kind == USB_XHCI_EVENT_HOST_CONTROLLER) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
    }
    *out = event;
    return USB_OK;
}

static int retain_event(struct usb_xhci_controller *c, const struct usb_xhci_event *event)
{
    if (c->pending_event_count == USB_XHCI_PENDING_EVENTS) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return USB_RANGE;
    }
    uint8_t index = (uint8_t)((c->pending_event_head + c->pending_event_count) %
                              USB_XHCI_PENDING_EVENTS);
    c->pending_events[index] = *event;
    ++c->pending_event_count;
    return USB_OK;
}

int usb_xhci_controller_next_event(struct usb_xhci_controller *c, struct usb_xhci_event *out)
{
    if (!c || !out) return USB_INVALID;
    if (c->pending_event_count) {
        *out = c->pending_events[c->pending_event_head];
        c->pending_event_head = (uint8_t)((c->pending_event_head + 1u) % USB_XHCI_PENDING_EVENTS);
        --c->pending_event_count;
        return USB_OK;
    }
    return read_event(c, out);
}

int usb_xhci_controller_command(struct usb_xhci_controller *c, const struct usb_xhci_trb *trb,
                                uint8_t expected_slot, uint32_t timeout_ms,
                                struct usb_xhci_event *completion)
{
    if (!c || !trb || !completion || !timeout_ms || timeout_ms > 10000u) return USB_INVALID;
    int result = usb_xhci_controller_submit_command(c, trb);
    if (result) return result;
    uint32_t elapsed = 0;
    for (;;) {
        struct usb_xhci_event event;
        result = read_event(c, &event);
        if (result == USB_AGAIN) {
            if (elapsed >= timeout_ms * 1000u) {
                c->state = USB_XHCI_CONTROLLER_FAULTED;
                return USB_TIMEOUT;
            }
            uint32_t delay = timeout_ms * 1000u - elapsed < POLL_US ?
                             timeout_ms * 1000u - elapsed : POLL_US;
            c->mmio.delay_us(c->mmio.context, delay);
            elapsed += delay;
            continue;
        }
        if (result) return result;
        if (event.kind == USB_XHCI_EVENT_COMMAND_COMPLETION) {
            if (expected_slot && event.data.command.slot_id != expected_slot) {
                c->state = USB_XHCI_CONTROLLER_FAULTED;
                return USB_PHASE_ERROR;
            }
            *completion = event;
            return event.completion_code == USB_XHCI_CC_SUCCESS ? USB_OK : USB_COMMAND_FAILED;
        }
        result = retain_event(c, &event);
        if (result) return result;
    }
}

static uint64_t control_trb_address(const struct usb_xhci_transfer_ring *ring, uint32_t index)
{
    return ring->trbs.address + (uint64_t)(index % ring->usable_entries) * USB_XHCI_TRB_BYTES;
}

int usb_xhci_control_submit(struct usb_xhci_control_td *td, struct usb_xhci_controller *c,
                            struct usb_xhci_transfer_ring *ring, uint8_t slot,
                            const struct usb_setup *setup, const void *out_data, size_t capacity)
{
    if (!td || !c || !ring || !setup || !slot) return USB_INVALID;
    if (td->pending || td->complete || td->faulted || td->data.cpu) return USB_BUSY;
    if (c->state != USB_XHCI_CONTROLLER_RUNNING || !ring->controller_active ||
        ring->faulted || !ring->trbs.cpu || ring->outstanding || ring->usable_entries < 3u ||
        ring->producer_index >= ring->usable_entries) return USB_INVALID;
    if ((size_t)slot * 8u >= c->dcbaa.size) return USB_RANGE;
    bool input = (setup->type & 0x80u) != 0;
    size_t length = setup->length;
    if (length && ((!input && !out_data) || capacity < length)) return USB_RANGE;
    int result = controller_ready(c);
    if (result) return result;

    zero_bytes(td, sizeof(*td));
    td->controller = c; td->ring = ring; td->dma = c->dma; td->slot = slot;
    td->requested = length; td->input = input; td->trb_count = length ? 3u : 2u;
    if (length) {
        result = usb_dma_alloc32(&c->dma, length, 65536u, &td->data);
        if (result) { zero_bytes(td, sizeof(*td)); return result; }
        if (!input) copy_bytes(td->data.cpu, out_data, length);
        result = usb_dma_sync(&c->dma, &td->data, 0, length,
                              input ? USB_DMA_FROM_DEVICE : USB_DMA_TO_DEVICE, true);
        if (result) { usb_dma_free(&c->dma, &td->data); zero_bytes(td, sizeof(*td)); return result; }
    }
    struct usb_xhci_trb trbs[3] = {{{0}}, {{0}}, {{0}}};
    put_le32(trbs[0].bytes, (uint32_t)setup->type | ((uint32_t)setup->request << 8) |
                           ((uint32_t)setup->value << 16));
    put_le32(trbs[0].bytes + 4, (uint32_t)setup->index | ((uint32_t)setup->length << 16));
    put_le32(trbs[0].bytes + 8, 8u);
    put_le32(trbs[0].bytes + 12, (USB_XHCI_TRB_SETUP_STAGE << USB_XHCI_TRB_TYPE_SHIFT) |
             (1u << 6) | (length ? (input ? 3u : 2u) << 16 : 0u));
    uint32_t start = ring->producer_index;
    unsigned status_index = length ? 2u : 1u;
    if (length) {
        put_le64(trbs[1].bytes, td->data.address);
        put_le32(trbs[1].bytes + 8, (uint32_t)length);
        put_le32(trbs[1].bytes + 12, (USB_XHCI_TRB_DATA_STAGE << USB_XHCI_TRB_TYPE_SHIFT) |
                 (input ? ((1u << 16) | (1u << 2)) : 0u));
        td->data_trb = control_trb_address(ring, start + 1u);
    }
    put_le32(trbs[status_index].bytes + 12,
             (USB_XHCI_TRB_STATUS_STAGE << USB_XHCI_TRB_TYPE_SHIFT) | (1u << 5) |
             ((!length || !input) ? (1u << 16) : 0u));
    td->status_trb = control_trb_address(ring, start + status_index);
    td->pending = true; /* Ring publication can have taken effect on failure. */
    result = usb_xhci_transfer_ring_enqueue(ring, trbs, td->trb_count, NULL);
    if (result) {
        if (ring->faulted) { td->faulted = true; c->state = USB_XHCI_CONTROLLER_FAULTED; }
        else { usb_dma_free(&c->dma, &td->data); zero_bytes(td, sizeof(*td)); }
        return result;
    }
    uint32_t doorbell = c->caps.doorbell_offset + (uint32_t)slot * 4u;
    result = guarded_write(c, doorbell, 1u); /* DCI 1 is EP0. */
    if (!result) { uint32_t posted; result = guarded_read(c, doorbell, &posted); }
    if (result) { td->faulted = true; c->state = USB_XHCI_CONTROLLER_FAULTED; }
    return result;
}

int usb_xhci_control_event(struct usb_xhci_control_td *td, const struct usb_xhci_event *event)
{
    if (!td || !event || !td->pending || td->faulted || !td->controller || !td->ring ||
        td->controller->state != USB_XHCI_CONTROLLER_RUNNING)
        return USB_INVALID;
    if (event->kind != USB_XHCI_EVENT_TRANSFER || event->data.transfer.slot_id != td->slot ||
        event->data.transfer.endpoint_id != 1u) return USB_AGAIN;
    if (event->data.transfer.trb_pointer_valid && !event->data.transfer.event_data &&
        event->data.transfer.pointer_or_data == td->data_trb && td->input && td->requested &&
        !td->short_seen && event->completion_code == USB_XHCI_CC_SHORT_PACKET &&
        event->data.transfer.residual_bytes <= td->requested) {
        td->actual = td->requested - event->data.transfer.residual_bytes;
        td->short_seen = true;
        return USB_AGAIN;
    }
    if (!event->data.transfer.trb_pointer_valid || event->data.transfer.event_data ||
        event->data.transfer.pointer_or_data != td->status_trb ||
        event->completion_code != USB_XHCI_CC_SUCCESS) {
        td->faulted = true; td->controller->state = USB_XHCI_CONTROLLER_FAULTED;
        return event->completion_code == USB_XHCI_CC_STALL_ERROR ? USB_STALL : USB_PHASE_ERROR;
    }
    uint32_t completed = 0;
    int result = usb_xhci_transfer_ring_complete_through(td->ring, td->status_trb, &completed);
    if (!result && completed != td->trb_count) result = USB_PHASE_ERROR;
    if (!result && td->input && td->requested)
        result = usb_dma_sync(&td->controller->dma, &td->data, 0, td->requested, USB_DMA_FROM_DEVICE, false);
    if (result) { td->faulted = true; td->controller->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
    if (!td->short_seen) td->actual = td->requested;
    td->pending = false; td->complete = true;
    return USB_OK;
}

int usb_xhci_control_finish(struct usb_xhci_control_td *td, void *buffer, size_t capacity, size_t *actual)
{
    if (!td || !actual || !td->complete || td->faulted || !td->controller) return USB_INVALID;
    if (td->input && td->actual && (!buffer || capacity < td->actual)) return USB_RANGE;
    if (td->input && td->actual) copy_bytes(buffer, td->data.cpu, td->actual);
    *actual = td->actual;
    usb_dma_free(&td->dma, &td->data);
    zero_bytes(td, sizeof(*td));
    return USB_OK;
}

int usb_xhci_control_release_stopped(struct usb_xhci_control_td *td)
{
    if (!td || !td->ring || !td->controller || td->ring->controller_active || td->ring->faulted)
        return USB_INVALID;
    usb_dma_free(&td->dma, &td->data);
    zero_bytes(td, sizeof(*td));
    return USB_OK;
}

static int valid_ep0_packet(enum usb_xhci_speed speed, uint8_t packet)
{
    if (speed == USB_XHCI_SPEED_LOW) return packet == 8u;
    if (speed == USB_XHCI_SPEED_HIGH) return packet == 64u;
    if (speed == USB_XHCI_SPEED_FULL)
        return packet == 8u || packet == 16u || packet == 32u || packet == 64u;
    return 0;
}

static int validate_route(const struct usb_xhci_controller *c,
                          const struct usb_xhci_route *route, enum usb_xhci_speed speed)
{
    bool tt_parent_found = false;
    if (!route || !route->root_port || route->root_port > c->caps.max_ports ||
        route->depth > USB_XHCI_MAX_ROUTE_DEPTH ||
        !(c->caps.usb2_ports[(route->root_port - 1u) / 8u] &
          (uint8_t)(1u << ((route->root_port - 1u) % 8u)))) return USB_RANGE;
    for (uint8_t i = 0; i < route->depth; ++i) {
        if (!route->hub_slot[i] || route->hub_slot[i] > c->slot_limit ||
            !route->hub_port_count[i] || route->hub_port_count[i] > 127u ||
            !route->hub_port[i] || route->hub_port[i] > route->hub_port_count[i])
            return USB_RANGE;
        for (uint8_t j = 0; j < i; ++j)
            if (route->hub_slot[j] == route->hub_slot[i]) return USB_RANGE;
        if (route->hub_slot[i] == route->tt_hub_slot) {
            if (route->hub_port[i] != route->tt_port) return USB_RANGE;
            tt_parent_found = true;
        }
    }
    for (uint8_t i = route->depth; i < USB_XHCI_MAX_ROUTE_DEPTH; ++i)
        if (route->hub_slot[i] || route->hub_port[i] || route->hub_port_count[i])
            return USB_INVALID;
    if (route->multi_tt && (!route->tt_hub_slot || !route->depth)) return USB_INVALID;
    if (route->tt_hub_slot) {
        if ((speed != USB_XHCI_SPEED_LOW && speed != USB_XHCI_SPEED_FULL) ||
            route->tt_hub_slot > c->slot_limit || !route->tt_port || route->tt_port > 127u ||
            !route->depth || !tt_parent_found) return USB_RANGE;
    } else if (route->tt_port || route->multi_tt) return USB_INVALID;
    if (speed < USB_XHCI_SPEED_FULL || speed > USB_XHCI_SPEED_HIGH) return USB_INVALID;
    return USB_OK;
}

static int slot_sync_input(struct usb_xhci_device_slot *slot)
{
    return usb_dma_sync(&slot->dma, &slot->input_context, 0,
                        slot->input_context.size, USB_DMA_TO_DEVICE, true);
}

static void slot_build_input(struct usb_xhci_device_slot *slot, uint8_t packet)
{
    uint8_t stride = slot->controller->caps.context_bytes;
    uint8_t *base = slot->input_context.cpu;
    uint8_t *context = base + stride;
    uint8_t *ep0 = context + stride; /* Input control + Slot + EP0 contexts. */
    uint32_t route = 0;
    for (uint8_t i = 0; i < slot->route.depth; ++i) {
        uint32_t port = slot->route.hub_port_count[i] > 14u ? 15u :
                        slot->route.hub_port[i];
        route |= port << (4u * i);
    }
    put_le32(base + 4u, 3u); /* Add Slot and EP0 contexts. */
    put_le32(context, route | ((uint32_t)slot->speed << 20) | (1u << 27) |
                      (slot->route.multi_tt ? (1u << 25) : 0u));
    put_le32(context + 4u, (uint32_t)slot->route.root_port << 16);
    if (slot->route.tt_hub_slot)
        put_le32(context + 8u, (uint32_t)slot->route.tt_hub_slot |
                                ((uint32_t)slot->route.tt_port << 8));
    put_le32(ep0 + 4u, (3u << 1) | (4u << 3) | ((uint32_t)packet << 16));
    put_le64(ep0 + 8u, slot->ep0.trbs.address | slot->ep0.producer_cycle);
}

static int slot_release_memory(struct usb_xhci_device_slot *slot, bool controller_stopped)
{
    int result;
    if (slot->ep0.trbs.cpu && slot->ep0.controller_active) {
        result = usb_xhci_transfer_ring_halted(&slot->ep0);
        if (result) {
            if (!controller_stopped) return result;
            result = usb_dma_sync(&slot->dma, &slot->ep0.trbs, 0, slot->ep0.trbs.size,
                                  USB_DMA_TO_DEVICE, true);
            if (result) return result;
            slot->ep0.controller_active = false;
            slot->ep0.faulted = false;
            slot->ep0.outstanding = 0;
        }
    } else if (slot->ep0.trbs.cpu && slot->ep0.faulted) {
        if (!controller_stopped) return USB_IO;
        result = usb_dma_sync(&slot->dma, &slot->ep0.trbs, 0, slot->ep0.trbs.size,
                              USB_DMA_TO_DEVICE, true);
        if (result) return result;
        slot->ep0.faulted = false;
        slot->ep0.outstanding = 0;
    }
    if (slot->control.data.cpu || slot->control.pending || slot->control.complete ||
        slot->control.faulted) {
        if (slot->ep0.controller_active || slot->ep0.faulted) return USB_BUSY;
        result = usb_xhci_control_release_stopped(&slot->control);
        if (result) return result;
    }
    if (slot->ep0.trbs.cpu) {
        result = usb_xhci_transfer_ring_destroy(&slot->ep0);
        if (result) return result;
    }
    usb_dma_free(&slot->dma, &slot->input_context);
    usb_dma_free(&slot->dma, &slot->output_context);
    return USB_OK;
}

static void make_slot_command(struct usb_xhci_trb *trb, uint32_t type,
                              uint64_t parameter, uint8_t slot_id, bool bsr)
{
    zero_bytes(trb, sizeof(*trb));
    put_le64(trb->bytes, parameter);
    put_le32(trb->bytes + 12u, (type << USB_XHCI_TRB_TYPE_SHIFT) |
             ((uint32_t)slot_id << 24) | (bsr ? (1u << 9) : 0u));
}

int usb_xhci_device_prepare(struct usb_xhci_device_slot *slot, struct usb_xhci_controller *c,
                            const struct usb_xhci_route *route, enum usb_xhci_speed speed,
                            bool *cleanup_required)
{
    if (!slot || !c || !route || !cleanup_required) return USB_INVALID;
    if (slot->state != USB_XHCI_SLOT_EMPTY || c->state != USB_XHCI_CONTROLLER_RUNNING)
        return USB_BUSY;
    int result = validate_route(c, route, speed);
    if (result) return result;
    slot->controller = c; slot->dma = c->dma; slot->route = *route; slot->speed = speed;
    slot->ep0_max_packet = speed == USB_XHCI_SPEED_HIGH ? 64u : 8u;
    slot->state = USB_XHCI_SLOT_FAULTED;
    *cleanup_required = true; /* Enable Slot may take effect despite MMIO failure. */
    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_ENABLE_SLOT, 0, 0, false);
    result = usb_xhci_controller_command(c, &command, 0, 1000u, &completion);
    if (result) {
        /* Without a valid returned Slot ID, only controller reset can prove cleanup. */
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result;
    }
    slot->slot_id = completion.data.command.slot_id;
    if (!slot->slot_id || slot->slot_id > c->slot_limit ||
        (size_t)slot->slot_id * 8u + 8u > c->dcbaa.size) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return USB_PHASE_ERROR;
    }
    slot->state = USB_XHCI_SLOT_ENABLED;

    size_t stride = c->caps.context_bytes;
    result = usb_dma_alloc32(&slot->dma, stride * 32u, 64u, &slot->output_context);
    if (result) return result;
    if (!slot->output_context.coherent) return USB_UNSUPPORTED;
    result = usb_dma_alloc32(&slot->dma, stride * 33u, 64u, &slot->input_context);
    if (result) return result;
    if (!slot->input_context.coherent) return USB_UNSUPPORTED;
    result = usb_xhci_transfer_ring_init(&slot->ep0, &slot->dma, 16u);
    if (result) return result;
    result = usb_xhci_transfer_ring_mark_active(&slot->ep0);
    if (result) return result;
    put_le64((uint8_t *)c->dcbaa.cpu + (size_t)slot->slot_id * 8u,
             slot->output_context.address);
    result = usb_dma_sync(&c->dma, &c->dcbaa, (size_t)slot->slot_id * 8u, 8u,
                          USB_DMA_TO_DEVICE, true);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED; return result; }
    slot_build_input(slot, slot->ep0_max_packet);
    result = slot_sync_input(slot);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED; return result; }
    make_slot_command(&command, USB_XHCI_TRB_ADDRESS_DEVICE, slot->input_context.address,
                      slot->slot_id, true);
    result = usb_xhci_controller_command(c, &command, slot->slot_id, 1000u, &completion);
    if (result) {
        if (c->state != USB_XHCI_CONTROLLER_RUNNING) slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    result = usb_dma_sync(&slot->dma, &slot->output_context, 0,
                          slot->output_context.size, USB_DMA_FROM_DEVICE, false);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED; return result; }
    uint8_t *out_slot = slot->output_context.cpu;
    uint32_t state = get_le32(out_slot + 12u) >> 27;
    if ((get_le32(out_slot + 12u) & 0xffu) != 0 || state != 1u) {
        c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED;
        return USB_PHASE_ERROR;
    }
    slot->state = USB_XHCI_SLOT_DEFAULT;
    return USB_OK;
}

int usb_xhci_device_address(struct usb_xhci_device_slot *slot, uint8_t packet,
                            uint8_t *assigned_address)
{
    if (!slot || !assigned_address || slot->state != USB_XHCI_SLOT_DEFAULT ||
        !slot->controller || slot->controller->state != USB_XHCI_CONTROLLER_RUNNING)
        return USB_INVALID;
    if (!valid_ep0_packet(slot->speed, packet)) return USB_RANGE;
    struct usb_xhci_controller *c = slot->controller;
    slot->ep0_max_packet = packet;
    slot_build_input(slot, packet);
    int result = slot_sync_input(slot);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED; return result; }
    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_ADDRESS_DEVICE, slot->input_context.address,
                      slot->slot_id, false);
    result = usb_xhci_controller_command(c, &command, slot->slot_id, 5000u, &completion);
    if (result) return result; /* Keep the default slot and its cleanup ownership. */
    result = usb_dma_sync(&slot->dma, &slot->output_context, 0,
                          slot->output_context.size, USB_DMA_FROM_DEVICE, false);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED; return result; }
    uint32_t address_state = get_le32((uint8_t *)slot->output_context.cpu + 12u);
    uint8_t address = (uint8_t)address_state;
    if (!address || address > 127u || (address_state >> 27) != 2u) {
        c->state = USB_XHCI_CONTROLLER_FAULTED; slot->state = USB_XHCI_SLOT_FAULTED;
        return USB_PHASE_ERROR;
    }
    slot->address = address;
    slot->state = USB_XHCI_SLOT_ADDRESSED;
    *assigned_address = address;
    return USB_OK;
}

int usb_xhci_device_configure_hub(struct usb_xhci_device_slot *slot, uint8_t port_count,
                                  uint16_t tt_think_time_ns, bool multi_tt)
{
    if (!slot || !slot->controller || slot->state != USB_XHCI_SLOT_ADDRESSED ||
        slot->controller->state != USB_XHCI_CONTROLLER_RUNNING || !port_count ||
        port_count > 127u || (multi_tt && slot->speed != USB_XHCI_SPEED_HIGH))
        return USB_INVALID;
    uint8_t think_time = 0;
    if (slot->speed == USB_XHCI_SPEED_HIGH && tt_think_time_ns) {
        if (tt_think_time_ns > 2664u || tt_think_time_ns % 666u) return USB_RANGE;
        think_time = (uint8_t)(tt_think_time_ns / 666u - 1u);
    } else if (tt_think_time_ns) return USB_INVALID;

    uint8_t stride = slot->controller->caps.context_bytes;
    uint8_t *input_slot = (uint8_t *)slot->input_context.cpu + stride;
    copy_bytes(input_slot, slot->output_context.cpu, stride);
    uint32_t info = get_le32(input_slot);
    info |= 1u << 26;
    if (multi_tt) info |= 1u << 25;
    else info &= ~(1u << 25);
    put_le32(input_slot, info);
    uint32_t info2 = get_le32(input_slot + 4u);
    info2 = (info2 & 0x00ffffffu) | ((uint32_t)port_count << 24);
    put_le32(input_slot + 4u, info2);
    uint32_t tt = get_le32(input_slot + 8u);
    tt = (tt & ~(3u << 16)) | ((uint32_t)think_time << 16);
    put_le32(input_slot + 8u, tt);
    put_le32(input_slot + 12u, 0); /* Device State is output-only. */
    zero_bytes(slot->input_context.cpu, stride);
    put_le32((uint8_t *)slot->input_context.cpu + 4u, 1u); /* Add Slot context. */
    int result = slot_sync_input(slot);
    if (result) {
        slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
        slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_CONFIGURE_ENDPOINT,
                      slot->input_context.address, slot->slot_id, false);
    result = usb_xhci_controller_command(slot->controller, &command, slot->slot_id,
                                         5000u, &completion);
    if (result) {
        if (slot->controller->state != USB_XHCI_CONTROLLER_RUNNING)
            slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    result = usb_dma_sync(&slot->dma, &slot->output_context, 0,
                          slot->output_context.size, USB_DMA_FROM_DEVICE, false);
    if (result) {
        slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
        slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    uint8_t *out = slot->output_context.cpu;
    if (!(get_le32(out) & (1u << 26)) ||
        ((get_le32(out + 4u) >> 24) & 0xffu) != port_count) {
        slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
        slot->state = USB_XHCI_SLOT_FAULTED;
        return USB_PHASE_ERROR;
    }
    slot->hub_port_count = port_count;
    slot->is_hub = true;
    return USB_OK;
}

int usb_xhci_device_control(struct usb_xhci_device_slot *slot, uint8_t address,
                            const struct usb_setup *setup, void *buffer, size_t capacity,
                            size_t *actual, uint32_t timeout_ms)
{
    if (!slot || !setup || !actual || !timeout_ms || timeout_ms > 10000u ||
        (!buffer && setup->length)) return USB_INVALID;
    if (!slot->controller || slot->controller->state != USB_XHCI_CONTROLLER_RUNNING ||
        (slot->state != USB_XHCI_SLOT_DEFAULT && slot->state != USB_XHCI_SLOT_ADDRESSED) ||
        (slot->state == USB_XHCI_SLOT_DEFAULT ? address != 0 : address != slot->address))
        return USB_INVALID;
    int result = usb_xhci_control_submit(&slot->control, slot->controller, &slot->ep0,
                                         slot->slot_id, setup,
                                         (setup->type & 0x80u) ? NULL : buffer, capacity);
    if (result) {
        if (slot->control.faulted || slot->controller->state != USB_XHCI_CONTROLLER_RUNNING)
            slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms * 1000u) {
        struct usb_xhci_event event;
        result = read_event(slot->controller, &event);
        if (result == USB_AGAIN) {
            uint32_t delay = timeout_ms * 1000u - elapsed < POLL_US ?
                             timeout_ms * 1000u - elapsed : POLL_US;
            slot->controller->mmio.delay_us(slot->controller->mmio.context, delay);
            elapsed += delay;
            continue;
        }
        if (result) { slot->state = USB_XHCI_SLOT_FAULTED; return result; }
        if (event.kind == USB_XHCI_EVENT_TRANSFER &&
            event.data.transfer.slot_id == slot->slot_id &&
            event.data.transfer.endpoint_id == 1u) {
            result = usb_xhci_control_event(&slot->control, &event);
            if (result == USB_AGAIN) continue;
            if (result) { slot->state = USB_XHCI_SLOT_FAULTED; return result; }
            size_t done = 0;
            result = usb_xhci_control_finish(&slot->control, buffer, capacity, &done);
            if (result) return result;
            *actual = done;
            return USB_OK;
        }
        result = retain_event(slot->controller, &event);
        if (result) { slot->state = USB_XHCI_SLOT_FAULTED; return result; }
    }
    slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
    slot->state = USB_XHCI_SLOT_FAULTED;
    return USB_TIMEOUT;
}

int usb_xhci_device_disable(struct usb_xhci_device_slot *slot, uint32_t timeout_ms)
{
    if (!slot || !slot->controller || !slot->slot_id ||
        slot->controller->state != USB_XHCI_CONTROLLER_RUNNING ||
        slot->state == USB_XHCI_SLOT_FAULTED || slot->control.pending) return USB_INVALID;
    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_DISABLE_SLOT, 0, slot->slot_id, false);
    int result = usb_xhci_controller_command(slot->controller, &command, slot->slot_id,
                                             timeout_ms, &completion);
    if (result) return result;
    put_le64((uint8_t *)slot->controller->dcbaa.cpu + (size_t)slot->slot_id * 8u, 0);
    result = usb_dma_sync(&slot->controller->dma, &slot->controller->dcbaa,
                          (size_t)slot->slot_id * 8u, 8u, USB_DMA_TO_DEVICE, true);
    if (result) {
        slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
        slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    result = slot_release_memory(slot, false);
    if (result) {
        slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
        slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }
    zero_bytes(slot, sizeof(*slot));
    return USB_OK;
}

int usb_xhci_device_release_stopped(struct usb_xhci_device_slot *slot)
{
    if (!slot || !slot->controller || slot->state == USB_XHCI_SLOT_EMPTY ||
        slot->controller->state != USB_XHCI_CONTROLLER_EMPTY) return USB_INVALID;
    int result = slot_release_memory(slot, true);
    if (result) return result;
    zero_bytes(slot, sizeof(*slot));
    return USB_OK;
}

int usb_xhci_controller_stop(struct usb_xhci_controller *c)
{
    if (!c) return USB_INVALID;
    if (c->state == USB_XHCI_CONTROLLER_EMPTY) return USB_OK;
    if (!c->guard) return USB_IO;
    int result = c->guard(c->guard_context);
    if (result) return result;
    c->state = USB_XHCI_CONTROLLER_FAULTED;
    if (!c->reset_proven) {
        result = halt_and_reset(c);
        if (result) return result;
        c->reset_proven = true;
    }
    if (c->commands.controller_active) {
        result = usb_xhci_transfer_ring_halted(&c->commands);
        if (result) return result;
    } else if (c->commands.faulted) {
        result = usb_dma_sync(&c->dma, &c->commands.trbs, 0, c->commands.trbs.size,
                              USB_DMA_TO_DEVICE, true);
        if (result) return result;
        c->commands.faulted = false;
    }
    if (c->events.controller_active) {
        result = usb_xhci_event_ring_halted(&c->events);
        if (result) return result;
    } else if (c->events.faulted) {
        result = usb_dma_sync(&c->dma, &c->events.erst, 0, 16u, USB_DMA_TO_DEVICE, true);
        if (result) return result;
        result = usb_dma_sync(&c->dma, &c->events.trbs, 0, c->events.trbs.size,
                              USB_DMA_FROM_DEVICE, true);
        if (result) return result;
        c->events.faulted = false;
    }
    result = usb_xhci_transfer_ring_destroy(&c->commands);
    if (result) return result;
    result = usb_xhci_event_ring_destroy(&c->events);
    if (result) return result;
    for (uint16_t i = 0; i < c->scratch_allocated; ++i) usb_dma_free(&c->dma, &c->scratch_pages[i]);
    usb_dma_free(&c->dma, &c->scratch_array);
    usb_dma_free(&c->dma, &c->dcbaa);
    zero_bytes(c, sizeof(*c));
    return USB_OK;
}
