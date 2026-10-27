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
