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

int usb_xhci_controller_next_event(struct usb_xhci_controller *c, struct usb_xhci_event *out)
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
