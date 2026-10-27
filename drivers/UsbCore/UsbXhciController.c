/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbXhci.h"

#define HCSPARAMS2 0x08u
#define OP_CMD 0x00u
#define OP_STS 0x04u
#define OP_PORTSC 0x400u
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
#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_OCA (1u << 3)
#define PORTSC_PR (1u << 4)
#define PORTSC_PLS_MASK (0xfu << 5)
#define PORTSC_PP (1u << 9)
#define PORTSC_SPEED_MASK (0xfu << 10)
#define PORTSC_PIC_MASK (3u << 14)
#define PORTSC_CSC (1u << 17)
#define PORTSC_PEC (1u << 18)
#define PORTSC_WRC (1u << 19)
#define PORTSC_OCC (1u << 20)
#define PORTSC_PRC (1u << 21)
#define PORTSC_PLC (1u << 22)
#define PORTSC_CEC (1u << 23)
#define PORTSC_CHANGE_MASK (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                            PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
#define PORTSC_WAKE_MASK (7u << 25)
#define PORTSC_RW_CONTROL_MASK (PORTSC_PLS_MASK | PORTSC_PP | PORTSC_PIC_MASK | \
                                PORTSC_WAKE_MASK)
#define RESET_TIMEOUT_US 10000000u
#define HALT_TIMEOUT_US 32000u
#define POLL_US 1000u

static int read_event(struct usb_xhci_controller *, struct usb_xhci_event *);
static int retain_event(struct usb_xhci_controller *, const struct usb_xhci_event *);

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

static int root_port_offset(const struct usb_xhci_controller *c, uint8_t port,
                            uint32_t *offset)
{
    if (!c || !offset || !port || port > c->caps.max_ports) return USB_RANGE;
    if (!(c->caps.usb2_ports[(port - 1u) / 8u] &
          (uint8_t)(1u << ((port - 1u) % 8u)))) return USB_UNSUPPORTED;
    uint64_t address = (uint64_t)c->caps.operational_offset + OP_PORTSC +
                       ((uint64_t)port - 1u) * 0x10u;
    if (address > UINT32_MAX || address > c->mmio.size ||
        c->mmio.size - (uint32_t)address < 4u) return USB_RANGE;
    *offset = (uint32_t)address;
    return USB_OK;
}

static int root_port_write(struct usb_xhci_controller *c, uint32_t offset,
                           uint32_t current, bool start_reset, uint32_t clear_changes)
{
    /* Preserve only PORTSC RW control fields. PED and change bits are RW1CS,
     * so echoing sampled status would disable/ack unrelated port state. */
    if (clear_changes & ~PORTSC_CHANGE_MASK) return USB_INVALID;
    uint32_t value = current & PORTSC_RW_CONTROL_MASK;
    if (start_reset) value |= PORTSC_PR;
    value |= clear_changes;
    return guarded_write(c, offset, value);
}

static uint32_t root_port_changes_decode(uint32_t portsc)
{
    uint32_t changes = 0;
    if (portsc & PORTSC_CSC) changes |= USB_XHCI_ROOT_PORT_CHANGE_CONNECT;
    if (portsc & PORTSC_PEC) changes |= USB_XHCI_ROOT_PORT_CHANGE_ENABLE;
    if (portsc & PORTSC_WRC) changes |= USB_XHCI_ROOT_PORT_CHANGE_WARM_RESET;
    if (portsc & PORTSC_OCC) changes |= USB_XHCI_ROOT_PORT_CHANGE_OVERCURRENT;
    if (portsc & PORTSC_PRC) changes |= USB_XHCI_ROOT_PORT_CHANGE_RESET;
    if (portsc & PORTSC_PLC) changes |= USB_XHCI_ROOT_PORT_CHANGE_LINK_STATE;
    if (portsc & PORTSC_CEC) changes |= USB_XHCI_ROOT_PORT_CHANGE_CONFIG_ERROR;
    return changes;
}

static uint32_t root_port_changes_encode(uint32_t changes)
{
    uint32_t portsc = 0;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_CONNECT) portsc |= PORTSC_CSC;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_ENABLE) portsc |= PORTSC_PEC;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_WARM_RESET) portsc |= PORTSC_WRC;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_OVERCURRENT) portsc |= PORTSC_OCC;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_RESET) portsc |= PORTSC_PRC;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_LINK_STATE) portsc |= PORTSC_PLC;
    if (changes & USB_XHCI_ROOT_PORT_CHANGE_CONFIG_ERROR) portsc |= PORTSC_CEC;
    return portsc;
}

static int root_port_drain_old_events(struct usb_xhci_controller *c)
{
    /* Let the normal owner process all events that predate this reset. */
    if (c->pending_event_count) return USB_BUSY;
    for (;;) {
        if (c->pending_event_count == USB_XHCI_PENDING_EVENTS) return USB_BUSY;
        struct usb_xhci_event event;
        int result = read_event(c, &event);
        if (result == USB_AGAIN) break;
        if (result) return result;
        result = retain_event(c, &event);
        if (result) return result;
        if (c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_IO;
    }
    return c->pending_event_count ? USB_BUSY : USB_OK;
}

int usb_xhci_root_port_reset(struct usb_xhci_controller *c, uint8_t port,
                             uint32_t timeout_ms, enum usb_xhci_speed *speed)
{
    if (!c || !speed || !timeout_ms || timeout_ms > 10000u) return USB_INVALID;
    if (c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    int result = controller_ready(c);
    if (result) return result;
    uint32_t offset;
    result = root_port_offset(c, port, &offset);
    if (result) return result;
    result = root_port_drain_old_events(c);
    if (result) return result;

    uint32_t value;
    result = guarded_read(c, offset, &value);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
    if (!(value & PORTSC_PP)) return USB_BUSY; /* Power belongs to the platform owner. */
    if (!(value & PORTSC_CCS)) return USB_DISCONNECTED;
    if (value & PORTSC_OCA) return USB_IO;
    if (value & PORTSC_PR) return USB_BUSY;

    if (value & PORTSC_PRC) {
        result = root_port_write(c, offset, value, false, PORTSC_PRC);
        if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
        result = guarded_read(c, offset, &value);
        if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
        if (value & PORTSC_PRC) {
            c->state = USB_XHCI_CONTROLLER_FAULTED;
            return USB_IO;
        }
    }
    if (!(value & PORTSC_PP)) return USB_BUSY;
    if (!(value & PORTSC_CCS)) return USB_DISCONNECTED;
    if (value & PORTSC_OCA) return USB_IO;
    if (value & PORTSC_PR) return USB_BUSY;
    result = root_port_write(c, offset, value, true, 0);
    if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }

    bool port_event_seen = false;
    uint32_t elapsed = 0, timeout_us = timeout_ms * 1000u;
    for (;;) {
        if (c->pending_event_count == USB_XHCI_PENDING_EVENTS) {
            c->state = USB_XHCI_CONTROLLER_FAULTED;
            return USB_RANGE;
        }
        struct usb_xhci_event event;
        result = read_event(c, &event);
        if (result != USB_OK && result != USB_AGAIN) return result;
        bool matching_port_event =
            result == USB_OK &&
            event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE &&
            event.data.port_status.port_id == port;
        if (result == USB_OK) {
            result = retain_event(c, &event);
            if (result) return result;
        }
        if (c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_IO;

        result = guarded_read(c, offset, &value);
        if (result) { c->state = USB_XHCI_CONTROLLER_FAULTED; return result; }
        if (value & PORTSC_OCA) {
            /* The matching PSC, if present, is already retained above. */
            c->state = USB_XHCI_CONTROLLER_FAULTED;
            return USB_IO;
        }
        if (matching_port_event && !(value & PORTSC_PR) && (value & PORTSC_PRC))
            port_event_seen = true;
        if (port_event_seen && !(value & PORTSC_PR) && (value & PORTSC_PRC)) break;
        if (elapsed >= timeout_us) {
            /* Reset or its required PSC may still be pending; stop before reuse. */
            c->state = USB_XHCI_CONTROLLER_FAULTED;
            return USB_TIMEOUT;
        }
        uint32_t delay = timeout_us - elapsed < POLL_US ?
                         timeout_us - elapsed : POLL_US;
        c->mmio.delay_us(c->mmio.context, delay);
        elapsed += delay;
    }

    enum usb_xhci_speed detected = USB_XHCI_SPEED_FULL;
    if (value & PORTSC_OCA) result = USB_IO;
    else if (!(value & PORTSC_CCS)) result = USB_DISCONNECTED;
    else if (!(value & PORTSC_PED)) result = USB_IO;
    else {
        uint32_t speed_id = (value & PORTSC_SPEED_MASK) >> 10;
        if (speed_id == USB_XHCI_SPEED_FULL) detected = USB_XHCI_SPEED_FULL;
        else if (speed_id == USB_XHCI_SPEED_LOW) detected = USB_XHCI_SPEED_LOW;
        else if (speed_id == USB_XHCI_SPEED_HIGH) detected = USB_XHCI_SPEED_HIGH;
        else result = USB_UNSUPPORTED;
    }
    int clear = root_port_write(c, offset, value, false, PORTSC_PRC);
    if (clear) { c->state = USB_XHCI_CONTROLLER_FAULTED; return clear; }
    int verify = guarded_read(c, offset, &value);
    if (verify) { c->state = USB_XHCI_CONTROLLER_FAULTED; return verify; }
    if (value & (PORTSC_PR | PORTSC_PRC)) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return USB_IO;
    }
    if (value & PORTSC_OCA) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return USB_IO;
    }
    if (!result) {
        if (!(value & PORTSC_PP)) result = USB_BUSY;
        else if (!(value & PORTSC_CCS)) result = USB_DISCONNECTED;
        else if (!(value & PORTSC_PED)) result = USB_IO;
        else if (((value & PORTSC_SPEED_MASK) >> 10) != (uint32_t)detected)
            result = USB_IO;
    }
    if (!result) *speed = detected;
    return result;
}

int usb_xhci_root_port_get_status(struct usb_xhci_controller *c, uint8_t port,
                                  struct usb_xhci_root_port_status *status)
{
    if (!c || !status || c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    int result = controller_ready(c);
    if (result) return result;
    uint32_t offset;
    result = root_port_offset(c, port, &offset);
    if (result) return result;
    uint32_t value;
    result = guarded_read(c, offset, &value);
    if (result) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result;
    }
    struct usb_xhci_root_port_status candidate = {0};
    candidate.connected = (value & PORTSC_CCS) != 0;
    candidate.enabled = (value & PORTSC_PED) != 0;
    candidate.overcurrent = (value & PORTSC_OCA) != 0;
    candidate.resetting = (value & PORTSC_PR) != 0;
    candidate.powered = (value & PORTSC_PP) != 0;
    candidate.speed_id = (uint8_t)((value & PORTSC_SPEED_MASK) >> 10);
    candidate.changes = root_port_changes_decode(value);
    *status = candidate;
    return USB_OK;
}

int usb_xhci_root_port_ack_changes(struct usb_xhci_controller *c, uint8_t port,
                                   uint32_t changes)
{
    if (!c || !changes || (changes & ~USB_XHCI_ROOT_PORT_CHANGE_MASK) ||
        c->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    int result = controller_ready(c);
    if (result) return result;
    uint32_t offset;
    result = root_port_offset(c, port, &offset);
    if (result) return result;
    uint32_t value;
    result = guarded_read(c, offset, &value);
    if (result) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result;
    }
    uint32_t clear = root_port_changes_encode(changes) & value;
    if (!clear) return USB_OK;
    result = root_port_write(c, offset, value, false, clear);
    if (result) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result;
    }
    result = guarded_read(c, offset, &value);
    if (result) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return result;
    }
    if (value & clear) {
        c->state = USB_XHCI_CONTROLLER_FAULTED;
        return USB_IO;
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
    for (size_t i = 0; i < USB_XHCI_MAX_NONCONTROL_ENDPOINTS; ++i) {
        struct usb_xhci_device_endpoint *endpoint = &slot->endpoints[i];
        struct usb_xhci_transfer_ring *ring = &endpoint->ring;
        if (ring->trbs.cpu && ring->controller_active) {
            result = usb_xhci_transfer_ring_halted(ring);
            if (result) {
                if (!controller_stopped) return result;
                result = usb_dma_sync(&slot->dma, &ring->trbs, 0, ring->trbs.size,
                                      USB_DMA_TO_DEVICE, true);
                if (result) return result;
                ring->controller_active = false;
                ring->faulted = false;
                ring->outstanding = 0;
            }
        } else if (ring->trbs.cpu && ring->faulted) {
            if (!controller_stopped) return USB_IO;
            result = usb_dma_sync(&slot->dma, &ring->trbs, 0, ring->trbs.size,
                                  USB_DMA_TO_DEVICE, true);
            if (result) return result;
            ring->faulted = false;
            ring->outstanding = 0;
        }
        if (endpoint->transfer_pending && !controller_stopped) return USB_BUSY;
        if (ring->trbs.cpu) {
            result = usb_xhci_transfer_ring_destroy(ring);
            if (result) return result;
        }
        usb_dma_free(&slot->dma, &endpoint->transfer_data);
        zero_bytes(endpoint, sizeof(*endpoint));
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

static int endpoint_dci(const struct usb_endpoint *descriptor, uint8_t *dci)
{
    if (!descriptor || !dci || (descriptor->address & 0x70u) ||
        !(descriptor->address & 0x0fu)) return USB_INVALID;
    uint8_t number = descriptor->address & 0x0fu;
    *dci = (uint8_t)(number * 2u + ((descriptor->address & 0x80u) ? 1u : 0u));
    return *dci <= 31u ? USB_OK : USB_RANGE;
}

static uint8_t floor_log2_u32(uint32_t value)
{
    uint8_t result = 0;
    while (value > 1u) { value >>= 1; ++result; }
    return result;
}

static struct usb_xhci_device_endpoint *find_slot_endpoint(
    struct usb_xhci_device_slot *slot, uint8_t address)
{
    if (!slot || (address & 0x70u) || !(address & 0x0fu)) return NULL;
    uint8_t number = address & 0x0fu;
    uint8_t dci = (uint8_t)(number * 2u + ((address & 0x80u) ? 1u : 0u));
    if (dci < 2u || dci > 31u) return NULL;
    struct usb_xhci_device_endpoint *endpoint = &slot->endpoints[dci - 2u];
    return endpoint->configured && endpoint->descriptor.address == address ? endpoint : NULL;
}

static int validate_endpoint_descriptor(enum usb_xhci_speed speed,
                                        const struct usb_endpoint *descriptor,
                                        uint8_t *dci, uint8_t *interval,
                                        uint8_t *max_burst, uint16_t *max_esit)
{
    if (!descriptor || !dci || !interval || !max_burst || !max_esit ||
        (descriptor->type != 2u && descriptor->type != 3u) ||
        !descriptor->max_packet || !descriptor->transactions ||
        descriptor->transactions > 3u) return USB_INVALID;
    int result = endpoint_dci(descriptor, dci);
    if (result) return result;
    if (descriptor->type == 2u) {
        if (descriptor->transactions != 1u || speed == USB_XHCI_SPEED_LOW ||
            (speed == USB_XHCI_SPEED_FULL &&
             descriptor->max_packet != 8u && descriptor->max_packet != 16u &&
             descriptor->max_packet != 32u && descriptor->max_packet != 64u) ||
            (speed == USB_XHCI_SPEED_HIGH && descriptor->max_packet != 512u))
            return USB_UNSUPPORTED;
        *interval = speed == USB_XHCI_SPEED_HIGH ? descriptor->interval : 0u;
        *max_burst = 0u;
        *max_esit = 0u;
        return USB_OK;
    }

    if (!descriptor->interval) return USB_INVALID;
    if (speed == USB_XHCI_SPEED_LOW) {
        if (descriptor->max_packet > 8u || descriptor->transactions != 1u ||
            descriptor->interval < 10u) return USB_UNSUPPORTED;
        *interval = floor_log2_u32((uint32_t)descriptor->interval * 8u);
        *max_burst = 0u;
    } else if (speed == USB_XHCI_SPEED_FULL) {
        if (descriptor->max_packet > 64u || descriptor->transactions != 1u)
            return USB_UNSUPPORTED;
        *interval = floor_log2_u32((uint32_t)descriptor->interval * 8u);
        *max_burst = 0u;
    } else if (speed == USB_XHCI_SPEED_HIGH) {
        if (descriptor->interval > 16u || descriptor->max_packet > 1024u ||
            (descriptor->transactions == 2u && descriptor->max_packet < 513u) ||
            (descriptor->transactions == 3u && descriptor->max_packet < 683u) ||
            ((uint32_t)descriptor->max_packet * descriptor->transactions > 1024u &&
             descriptor->interval != 1u)) return USB_UNSUPPORTED;
        *interval = (uint8_t)(descriptor->interval - 1u);
        *max_burst = (uint8_t)(descriptor->transactions - 1u);
    } else return USB_INVALID;
    *max_esit = (uint16_t)(descriptor->max_packet *
                           (uint16_t)(*max_burst + 1u));
    return USB_OK;
}

int usb_xhci_device_configure_endpoint(struct usb_xhci_device_slot *slot,
                                       const struct usb_endpoint *descriptor)
{
    if (!slot || !slot->controller || !descriptor || slot->state != USB_XHCI_SLOT_ADDRESSED ||
        slot->controller->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    uint8_t dci, interval, max_burst;
    uint16_t max_esit;
    int result = validate_endpoint_descriptor(slot->speed, descriptor, &dci,
                                               &interval, &max_burst, &max_esit);
    if (result) return result;
    struct usb_xhci_device_endpoint *endpoint = &slot->endpoints[dci - 2u];
    if (endpoint->configured || endpoint->ring.trbs.cpu || endpoint->transfer_data.cpu)
        return USB_BUSY;
    endpoint->descriptor = *descriptor;
    endpoint->dci = dci;
    endpoint->input = (descriptor->address & 0x80u) != 0;
    result = usb_xhci_transfer_ring_init(&endpoint->ring, &slot->dma, 16u);
    if (result) { zero_bytes(endpoint, sizeof(*endpoint)); return result; }

    uint8_t stride = slot->controller->caps.context_bytes;
    uint8_t *input = slot->input_context.cpu;
    uint8_t *input_slot = input + stride;
    uint8_t *input_endpoint = input + (size_t)(dci + 1u) * stride;
    copy_bytes(input_slot, slot->output_context.cpu, stride);
    uint32_t slot_info = get_le32(input_slot);
    uint32_t context_entries = (slot_info >> 27) & 0x1fu;
    if (context_entries < dci) {
        slot_info = (slot_info & ~(0x1fu << 27)) | ((uint32_t)dci << 27);
        put_le32(input_slot, slot_info);
    }
    zero_bytes(input, stride);
    put_le32(input + 4u, 1u | (1u << dci)); /* Add Slot and this Endpoint context. */
    zero_bytes(input_endpoint, stride);
    uint32_t info0 = (uint32_t)interval << 16;
    put_le32(input_endpoint, info0);
    uint32_t endpoint_type = descriptor->type == 2u ?
        ((descriptor->address & 0x80u) ? 6u : 2u) :
        ((descriptor->address & 0x80u) ? 7u : 3u);
    uint32_t info1 = (3u << 1) | (endpoint_type << 3) |
                     ((uint32_t)max_burst << 8) |
                     ((uint32_t)descriptor->max_packet << 16);
    put_le32(input_endpoint + 4u, info1);
    put_le64(input_endpoint + 8u, endpoint->ring.trbs.address |
                                   endpoint->ring.producer_cycle);
    uint16_t average_trb = descriptor->type == 2u ? 3072u : 1024u;
    put_le32(input_endpoint + 16u, (uint32_t)average_trb |
                                    ((uint32_t)max_esit << 16));
    result = slot_sync_input(slot);
    if (result) goto configure_endpoint_unpublished_failure;
    result = usb_xhci_transfer_ring_mark_active(&endpoint->ring);
    if (result) goto configure_endpoint_unpublished_failure;

    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_CONFIGURE_ENDPOINT,
                      slot->input_context.address, slot->slot_id, false);
    result = usb_xhci_controller_command(slot->controller, &command, slot->slot_id,
                                         5000u, &completion);
    if (result) goto configure_endpoint_published_failure;
    result = usb_dma_sync(&slot->dma, &slot->output_context, 0,
                          slot->output_context.size, USB_DMA_FROM_DEVICE, false);
    if (result) goto configure_endpoint_published_failure;
    uint8_t *output_endpoint = (uint8_t *)slot->output_context.cpu +
                               (size_t)dci * stride;
    if ((get_le32(output_endpoint) & 7u) != 1u ||
        ((get_le32(output_endpoint + 4u) >> 3) & 7u) != endpoint_type ||
        (get_le32(output_endpoint + 4u) >> 16) != descriptor->max_packet) {
        result = USB_PHASE_ERROR;
        goto configure_endpoint_published_failure;
    }
    endpoint->configured = true;
    return USB_OK;

configure_endpoint_unpublished_failure:
    if (endpoint->ring.trbs.cpu && !endpoint->ring.controller_active &&
        !endpoint->ring.faulted) (void)usb_xhci_transfer_ring_destroy(&endpoint->ring);
    zero_bytes(endpoint, sizeof(*endpoint));
    return result;

configure_endpoint_published_failure:
    endpoint->faulted = true;
    slot->controller->state = USB_XHCI_CONTROLLER_FAULTED;
    slot->state = USB_XHCI_SLOT_FAULTED;
    return result;
}

static int endpoint_set_dequeue(struct usb_xhci_device_slot *slot,
                                struct usb_xhci_device_endpoint *endpoint,
                                uint32_t timeout_ms)
{
    if (!endpoint->needs_dequeue) return USB_OK;
    if (endpoint->ring.outstanding || endpoint->ring.consumer_index >= endpoint->ring.usable_entries ||
        endpoint->ring.consumer_cycle > 1u) return USB_PHASE_ERROR;
    uint64_t address = endpoint->ring.trbs.address +
                       (uint64_t)endpoint->ring.consumer_index * USB_XHCI_TRB_BYTES;
    address |= endpoint->ring.consumer_cycle;
    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_SET_TR_DEQUEUE, address,
                      slot->slot_id, false);
    uint32_t control = get_le32(command.bytes + 12u) | ((uint32_t)endpoint->dci << 16);
    put_le32(command.bytes + 12u, control);
    int result = usb_xhci_controller_command(slot->controller, &command, slot->slot_id,
                                             timeout_ms, &completion);
    if (!result) endpoint->needs_dequeue = false;
    return result;
}

int usb_xhci_device_reset_endpoint(struct usb_xhci_device_slot *slot, uint8_t address,
                                   uint8_t endpoint_address, uint32_t timeout_ms)
{
    if (!slot || !slot->controller || !timeout_ms || timeout_ms > 10000u ||
        slot->state != USB_XHCI_SLOT_ADDRESSED || slot->controller->state != USB_XHCI_CONTROLLER_RUNNING ||
        address != slot->address) return USB_INVALID;
    struct usb_xhci_device_endpoint *endpoint = find_slot_endpoint(slot, endpoint_address);
    if (!endpoint) return USB_INVALID;
    if (endpoint->needs_dequeue) return USB_OK;
    if (!endpoint->halted) return endpoint->transfer_pending || endpoint->faulted ? USB_BUSY : USB_OK;
    if (endpoint->transfer_pending || endpoint->faulted || endpoint->ring.outstanding)
        return USB_BUSY;
    struct usb_xhci_trb command;
    struct usb_xhci_event completion;
    make_slot_command(&command, USB_XHCI_TRB_RESET_ENDPOINT, 0, slot->slot_id, false);
    uint32_t control = get_le32(command.bytes + 12u) | ((uint32_t)endpoint->dci << 16);
    put_le32(command.bytes + 12u, control); /* TSP=0 resets the USB2 data toggle. */
    int result = usb_xhci_controller_command(slot->controller, &command, slot->slot_id,
                                             timeout_ms, &completion);
    if (!result) {
        endpoint->halted = false;
        endpoint->needs_dequeue = true;
    } else if (slot->controller->state != USB_XHCI_CONTROLLER_RUNNING) {
        endpoint->faulted = true;
        slot->state = USB_XHCI_SLOT_FAULTED;
    }
    return result;
}

static int device_endpoint_transfer_one(struct usb_xhci_device_slot *slot,
                                        struct usb_xhci_device_endpoint *endpoint,
                                        void *buffer, size_t offset, size_t length,
                                        uint32_t timeout_ms, size_t *actual)
{
    struct usb_xhci_controller *controller = slot->controller;
    if (endpoint->transfer_pending || endpoint->faulted || endpoint->halted ||
        endpoint->ring.faulted || endpoint->ring.outstanding) return USB_BUSY;
    int result = endpoint_set_dequeue(slot, endpoint, timeout_ms);
    if (result) return result;
    if (length) {
        result = usb_dma_alloc32(&slot->dma, length, 65536u, &endpoint->transfer_data);
        if (result) return result;
        if (!endpoint->input) copy_bytes(endpoint->transfer_data.cpu,
                                         (const uint8_t *)buffer + offset, length);
        result = usb_dma_sync(&slot->dma, &endpoint->transfer_data, 0, length,
                              endpoint->input ? USB_DMA_FROM_DEVICE : USB_DMA_TO_DEVICE, true);
        if (result) {
            usb_dma_free(&slot->dma, &endpoint->transfer_data);
            return result;
        }
    }

    endpoint->input = (endpoint->descriptor.address & 0x80u) != 0;
    struct usb_xhci_trb transfer = {{0}};
    if (length) put_le64(transfer.bytes, endpoint->transfer_data.address);
    put_le32(transfer.bytes + 8u, (uint32_t)length);
    put_le32(transfer.bytes + 12u,
             (USB_XHCI_TRB_NORMAL << USB_XHCI_TRB_TYPE_SHIFT) | USB_XHCI_TRB_IOC |
             (endpoint->input ? USB_XHCI_TRB_ISP : 0u));
    endpoint->transfer_length = (uint32_t)length;
    endpoint->transfer_pending = true;
    result = usb_xhci_transfer_ring_enqueue(&endpoint->ring, &transfer, 1u,
                                           &endpoint->transfer_trb);
    if (result) {
        if (endpoint->ring.faulted) {
            endpoint->faulted = true;
            controller->state = USB_XHCI_CONTROLLER_FAULTED;
            slot->state = USB_XHCI_SLOT_FAULTED;
            return result;
        }
        endpoint->transfer_pending = false;
        endpoint->transfer_trb = 0;
        endpoint->transfer_length = 0;
        usb_dma_free(&slot->dma, &endpoint->transfer_data);
        return result;
    }
    uint32_t doorbell = controller->caps.doorbell_offset + (uint32_t)slot->slot_id * 4u;
    result = guarded_write(controller, doorbell, endpoint->dci);
    if (!result) { uint32_t posted; result = guarded_read(controller, doorbell, &posted); }
    if (result) {
        endpoint->faulted = true;
        controller->state = USB_XHCI_CONTROLLER_FAULTED;
        slot->state = USB_XHCI_SLOT_FAULTED;
        return result;
    }

    uint32_t elapsed = 0, timeout_us = timeout_ms * 1000u;
    for (;;) {
        struct usb_xhci_event event;
        result = read_event(controller, &event);
        if (result == USB_AGAIN) {
            if (elapsed >= timeout_us) break;
            uint32_t delay = timeout_us - elapsed < POLL_US ? timeout_us - elapsed : POLL_US;
            controller->mmio.delay_us(controller->mmio.context, delay);
            elapsed += delay;
            continue;
        }
        if (result) goto endpoint_transfer_failstop;
        if (event.kind == USB_XHCI_EVENT_TRANSFER &&
            event.data.transfer.slot_id == slot->slot_id &&
            event.data.transfer.endpoint_id == endpoint->dci) {
            if (!event.data.transfer.trb_pointer_valid || event.data.transfer.event_data ||
                event.data.transfer.pointer_or_data != endpoint->transfer_trb ||
                event.data.transfer.residual_bytes > endpoint->transfer_length) {
                result = USB_PHASE_ERROR;
                goto endpoint_transfer_failstop;
            }
            if (event.completion_code == USB_XHCI_CC_STALL_ERROR) {
                uint32_t completed = 0;
                result = usb_xhci_transfer_ring_complete_event(&endpoint->ring, &event,
                                                              &completed);
                if (result || completed != 1u) {
                    result = result ? result : USB_PHASE_ERROR;
                    goto endpoint_transfer_failstop;
                }
                size_t transferred = length - event.data.transfer.residual_bytes;
                if (endpoint->input && transferred) {
                    result = usb_dma_sync(&slot->dma, &endpoint->transfer_data, 0, length,
                                          USB_DMA_FROM_DEVICE, false);
                    if (result) goto endpoint_transfer_failstop;
                    copy_bytes((uint8_t *)buffer + offset, endpoint->transfer_data.cpu,
                               transferred);
                }
                endpoint->transfer_pending = false;
                endpoint->transfer_trb = 0;
                endpoint->transfer_length = 0;
                endpoint->halted = true;
                usb_dma_free(&slot->dma, &endpoint->transfer_data);
                *actual = transferred;
                return USB_STALL;
            }
            if (event.completion_code != USB_XHCI_CC_SUCCESS &&
                event.completion_code != USB_XHCI_CC_SHORT_PACKET) {
                result = USB_PHASE_ERROR;
                goto endpoint_transfer_failstop;
            }
            if ((!endpoint->input && event.data.transfer.residual_bytes) ||
                (event.completion_code == USB_XHCI_CC_SUCCESS &&
                 event.data.transfer.residual_bytes)) {
                result = USB_PHASE_ERROR;
                goto endpoint_transfer_failstop;
            }
            uint32_t completed = 0;
            result = usb_xhci_transfer_ring_complete_event(&endpoint->ring, &event,
                                                          &completed);
            if (result || completed != 1u) {
                result = result ? result : USB_PHASE_ERROR;
                goto endpoint_transfer_failstop;
            }
            size_t transferred = length - event.data.transfer.residual_bytes;
            if (endpoint->input && transferred) {
                result = usb_dma_sync(&slot->dma, &endpoint->transfer_data, 0, length,
                                      USB_DMA_FROM_DEVICE, false);
                if (result) goto endpoint_transfer_failstop;
                copy_bytes((uint8_t *)buffer + offset, endpoint->transfer_data.cpu, transferred);
            }
            endpoint->transfer_pending = false;
            endpoint->transfer_trb = 0;
            endpoint->transfer_length = 0;
            usb_dma_free(&slot->dma, &endpoint->transfer_data);
            *actual = transferred;
            return USB_OK;
        }
        result = retain_event(controller, &event);
        if (result) goto endpoint_transfer_failstop;
    }
    result = USB_TIMEOUT;

endpoint_transfer_failstop:
    endpoint->faulted = true;
    controller->state = USB_XHCI_CONTROLLER_FAULTED;
    slot->state = USB_XHCI_SLOT_FAULTED;
    return result;
}

static int device_endpoint_transfer(struct usb_xhci_device_slot *slot, uint8_t address,
                                    uint8_t endpoint_address, void *buffer, size_t length,
                                    size_t *actual, uint32_t timeout_ms,
                                    uint8_t required_type, bool one_td)
{
    if (!slot || !slot->controller || !actual || !timeout_ms || timeout_ms > 10000u ||
        (!buffer && length) || length > UINT32_MAX || address != slot->address ||
        slot->state != USB_XHCI_SLOT_ADDRESSED ||
        slot->controller->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    struct usb_xhci_device_endpoint *endpoint = find_slot_endpoint(slot, endpoint_address);
    if (!endpoint || endpoint->descriptor.type != required_type) return USB_UNSUPPORTED;
    bool input = (endpoint->descriptor.address & 0x80u) != 0;
    if (one_td && (!input || !length)) return USB_UNSUPPORTED;
    if (one_td && length > (size_t)endpoint->descriptor.max_packet *
                               endpoint->descriptor.transactions) return USB_RANGE;
    size_t total = 0;
    bool once = false;
    do {
        size_t chunk = length - total;
        if (chunk > 65536u) chunk = 65536u;
        size_t transferred = 0;
        int result = device_endpoint_transfer_one(slot, endpoint, buffer, total, chunk,
                                                  timeout_ms, &transferred);
        if (result) {
            *actual = total + transferred;
            return result;
        }
        total += transferred;
        once = true;
        if (transferred < chunk || one_td) break;
    } while (total < length || !once);
    *actual = total;
    return USB_OK;
}

int usb_xhci_device_interrupt(struct usb_xhci_device_slot *slot, uint8_t address,
                              uint8_t endpoint, void *buffer, size_t capacity,
                              size_t *actual, uint32_t timeout_ms)
{
    return device_endpoint_transfer(slot, address, endpoint, buffer, capacity,
                                    actual, timeout_ms, 3u, true);
}

int usb_xhci_device_bulk(struct usb_xhci_device_slot *slot, uint8_t address,
                         uint8_t endpoint, void *buffer, size_t length,
                         size_t *actual, uint32_t timeout_ms)
{
    return device_endpoint_transfer(slot, address, endpoint, buffer, length,
                                    actual, timeout_ms, 2u, false);
}

int usb_xhci_device_disable(struct usb_xhci_device_slot *slot, uint32_t timeout_ms)
{
    if (!slot || !slot->controller || !slot->slot_id ||
        slot->controller->state != USB_XHCI_CONTROLLER_RUNNING ||
        slot->state == USB_XHCI_SLOT_FAULTED || slot->control.pending) return USB_INVALID;
    for (size_t i = 0; i < USB_XHCI_MAX_NONCONTROL_ENDPOINTS; ++i)
        if (slot->endpoints[i].transfer_pending) return USB_BUSY;
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
