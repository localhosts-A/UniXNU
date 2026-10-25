// SPDX-License-Identifier: GPL-2.0-only
/* MTU3 QMU hardware formats and sequencing follow MediaTek's public driver:
 * Copyright (C) 2016 MediaTek Inc. Author: Chunfeng Yun.
 * MiCode/MTK_kernel_device_modules e45c3199537fbba9aae5fb06c31fc8dbd3c1dcee,
 * drivers/usb/mtu3/{mtu3_qmu.c,mtu3.h,mtu3_hw_regs.h}.
 * This adapter enforces 32-bit DMA, bounded stop, coherent descriptor memory and
 * explicit request lifetimes for firmware and source XNU backends.
 */
#include "UsbMtu3Qmu.h"
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error MTU3 QMU descriptor ownership requires a little-endian target
#endif

#define HWO 1u
#define IOC 0x80u
#define ACTIVE (1u << 15)
#define START 1u
#define RESUME 2u
#define STOP 4u
#define DMA_REQUEST (1u << 29)
#define FLUSH_FIFO (1u << 20)

static void zero_bytes(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static uint32_t *gpd(struct usb_qmu *q, uint16_t index)
{ return (uint32_t *)((uint8_t *)q->descriptors.cpu + (size_t)index * USB_QMU_DESCRIPTOR_BYTES); }
static uint16_t next_index(uint16_t index) { return (uint16_t)((index + 1) % USB_QMU_DESCRIPTORS); }
static uint32_t queue_register(const struct usb_qmu *q, uint32_t offset)
{ return (q->transmit ? 0x510u : 0x610u) + ((uint32_t)q->endpoint - 1) * 0x10u + offset; }
static uint32_t high_register(const struct usb_qmu *q)
{ return (q->transmit ? 0x484u : 0x4c4u) + ((uint32_t)q->endpoint - 1) * 4u; }
static uint32_t endpoint_register(const struct usb_qmu *q)
{ return (q->transmit ? 0x110u : 0x210u) + ((uint32_t)q->endpoint - 1) * 0x10u; }
static int modify(struct usb_qmu *q, uint32_t offset, uint32_t clear, uint32_t set)
{
    uint32_t value;
    int result = usb_mmio_read(&q->mac, offset, &value);
    return result ? result : usb_mmio_write(&q->mac, offset, (value & ~clear) | set);
}
static bool valid(const struct usb_qmu *q)
{ return q && q->descriptors.cpu && q->endpoint && q->endpoint <= 15; }
static int descriptor_sync(struct usb_qmu *q, uint16_t index, bool device)
{
    return usb_dma_sync(&q->dma, &q->descriptors, (size_t)index * USB_QMU_DESCRIPTOR_BYTES,
                        USB_QMU_DESCRIPTOR_BYTES, USB_DMA_BIDIRECTIONAL, device);
}
int usb_qmu_init(struct usb_qmu *q, const struct usb_mmio *mac, const struct usb_dma_ops *dma,
                 uint8_t endpoint, bool transmit, bool enhanced)
{
    if (!q || !mac || !mac->read32 || !mac->write32 || !mac->barrier || !mac->delay_us ||
        mac->size < 0x800 || !dma || !endpoint || endpoint > 15) return USB_INVALID;
    zero_bytes(q, sizeof(*q)); q->mac = *mac; q->dma = *dma;
    q->endpoint = endpoint; q->transmit = transmit; q->enhanced = enhanced;
    int result = usb_dma_alloc32(&q->dma, USB_QMU_DESCRIPTORS * USB_QMU_DESCRIPTOR_BYTES, 64, &q->descriptors);
    if (result) return result;
    if (!q->descriptors.coherent) {
        usb_dma_free(&q->dma, &q->descriptors); return USB_UNSUPPORTED;
    }
    result = usb_dma_sync(&q->dma, &q->descriptors, 0, q->descriptors.size, USB_DMA_BIDIRECTIONAL, true);
    if (result) usb_dma_free(&q->dma, &q->descriptors);
    return result;
}
int usb_qmu_submit(struct usb_qmu *q, struct usb_dma_buffer *buffer, size_t offset,
                   size_t length, bool append_zlp, void *cookie)
{
    if (!valid(q) || !buffer || !buffer->cpu) return USB_INVALID;
    if (q->faulted) return USB_IO;
    if (!length || length > (q->enhanced ? 1048572u : 65532u) || offset > buffer->size || length > buffer->size - offset ||
        buffer->address > UINT32_MAX || offset > UINT32_MAX - buffer->address ||
        length - 1 > UINT32_MAX - (buffer->address + offset) || (append_zlp && !q->transmit)) return USB_RANGE;
    if (q->pending >= USB_QMU_DESCRIPTORS - 1) return USB_BUSY;
    uint64_t address = buffer->address + offset;
    if (address < q->descriptors.address + q->descriptors.size && q->descriptors.address < address + length) return USB_RANGE;
    for (uint16_t n = 0, i = q->consumer; n < q->pending; ++n, i = next_index(i)) {
        const struct usb_qmu_request *old = &q->requests[i];
        uint64_t start = old->buffer->address + old->offset;
        if (start < address + length && address < start + old->length) return USB_BUSY;
    }
    int result = usb_dma_sync(&q->dma, buffer, offset, length,
                              q->transmit ? USB_DMA_TO_DEVICE : USB_DMA_FROM_DEVICE, true);
    if (result) return result;
    uint16_t next = next_index(q->producer);
    uint32_t *current = gpd(q, q->producer), *tail = gpd(q, next);
    /* The following terminator is always software-owned before linking it. */
    __atomic_store_n(&tail[0], 0u, __ATOMIC_RELEASE);
    result = descriptor_sync(q, next, true);
    if (result) return result;
    current[0] = q->transmit ? 0u : (uint32_t)length << (q->enhanced ? 12 : 16);
    current[1] = (uint32_t)(q->descriptors.address + (size_t)next * USB_QMU_DESCRIPTOR_BYTES);
    current[2] = (uint32_t)address;
    current[3] = q->transmit ? (uint32_t)length : 0u;
    if (append_zlp) {
        if (q->enhanced) current[0] |= 1u << 6;
        else current[3] |= 1u << 29;
    }
    result = descriptor_sync(q, q->producer, true);
    if (result) return result;
    q->requests[q->producer] = (struct usb_qmu_request){buffer, offset, length, cookie};
    q->producer = next; ++q->pending;
    /* The ring is coherent/uncached. Publish HWO last, after payload sync and all
     * descriptor fields; no fallible cache operation follows publication. */
    __atomic_store_n(&current[0], current[0] | IOC | HWO, __ATOMIC_RELEASE);
    q->mac.barrier(q->mac.context);
    if (q->running) {
        result = usb_mmio_write(&q->mac, queue_register(q, 0), RESUME);
        uint32_t control = 0;
        if (!result) result = usb_mmio_read(&q->mac, queue_register(q, 0), &control);
        if (!result && !(control & ACTIVE)) result = usb_mmio_write(&q->mac, queue_register(q, 0), RESUME);
        if (result) {
            /* Ownership has transferred. Retain the request until cancellation,
             * even though the doorbell failed; submit still reports acceptance. */
            q->faulted = true;
        }
    }
    return USB_OK;
}
int usb_qmu_start(struct usb_qmu *q)
{
    if (!valid(q)) return USB_INVALID;
    if (q->faulted) return USB_IO;
    if (q->running) return USB_OK;
    if (!q->pending) return USB_AGAIN;
    uint32_t control;
    int result = usb_mmio_read(&q->mac, queue_register(q, 0), &control);
    if (result) return result;
    if (control & ACTIVE) return USB_BUSY; /* Never replace a live queue base. */
    result = usb_mmio_write(&q->mac, queue_register(q, 4), (uint32_t)(q->descriptors.address + (size_t)q->consumer * 16));
    if (!result) result = modify(q, high_register(q), 0xf, 0); /* clear inherited high start-address bits */
    if (!result) result = modify(q, endpoint_register(q), 0, DMA_REQUEST);
    if (q->transmit) {
        if (!result) result = modify(q, 0x404, 0, 1u << q->endpoint);
        if (!result) result = usb_mmio_write(&q->mac, 0x788, (1u << q->endpoint) | (1u << (q->endpoint + 16)));
    } else {
        if (!result) result = modify(q, 0x40c, 1u << q->endpoint, 1u << (q->endpoint + 16));
        if (!result) result = usb_mmio_write(&q->mac, 0x7c8, (1u << q->endpoint) | (1u << (q->endpoint + 16)));
        if (!result) result = usb_mmio_write(&q->mac, 0x7d8, 1u << (q->endpoint + 16));
    }
    if (result) { q->faulted = true; return result; }
    /* A failed MMIO write may have reached hardware: stop must subsequently be
     * attempted before any ring/data memory is released. */
    q->running = true;
    result = usb_mmio_write(&q->mac, queue_register(q, 0), START);
    if (result) q->faulted = true;
    return result;
}
int usb_qmu_reap(struct usb_qmu *q, usb_qmu_completion complete, void *context)
{
    if (!valid(q) || !complete) return USB_INVALID;
    if (q->faulted) return USB_IO;
    if (!q->pending) return USB_OK;
    if (!q->running) return USB_AGAIN;
    uint32_t low, high;
    int result = usb_mmio_read(&q->mac, queue_register(q, 8), &low);
    if (!result) result = usb_mmio_read(&q->mac, high_register(q), &high);
    if (result) return result;
    uint64_t current = ((uint64_t)((high >> 8) & 15) << 32) | low;
    if (current < q->descriptors.address || current - q->descriptors.address >= q->descriptors.size ||
        (current - q->descriptors.address) % 16) { q->faulted = true; return USB_RANGE; }
    uint16_t cursor = (uint16_t)((current - q->descriptors.address) / 16);
    uint16_t budget = (uint16_t)((cursor + USB_QMU_DESCRIPTORS - q->consumer) % USB_QMU_DESCRIPTORS);
    if (budget > q->pending) { q->faulted = true; return USB_RANGE; }
    for (uint16_t n = 0; n < budget; ++n) {
        uint16_t index = q->consumer;
        result = descriptor_sync(q, index, false);
        if (result) { q->faulted = true; return result; }
        uint32_t *current_gpd = gpd(q, index);
        if (__atomic_load_n(&current_gpd[0], __ATOMIC_ACQUIRE) & HWO) break;
        const struct usb_qmu_request request = q->requests[index];
        uint32_t mask = q->enhanced ? 0xfffffu : 0xffffu;
        size_t actual = current_gpd[3] & mask;
        uint32_t allowed = IOC | (q->enhanced && q->transmit ? 1u << 6 : 0u);
        if (!q->transmit) allowed |= (uint32_t)request.length << (q->enhanced ? 12 : 16);
        if (!request.buffer || actual > request.length || (q->transmit && actual != request.length) ||
            (q->transmit ? ((current_gpd[0] & IOC) == 0 || (current_gpd[0] & ~allowed)) : current_gpd[0] != allowed) ||
            (current_gpd[3] & ~(mask | (q->transmit && !q->enhanced ? 1u << 29 : 0u))) ||
            current_gpd[1] != q->descriptors.address + (size_t)next_index(index) * 16 ||
            current_gpd[2] != request.buffer->address + request.offset) {
            q->faulted = true; return USB_PHASE_ERROR;
        }
        result = usb_dma_sync(&q->dma, request.buffer, request.offset, request.length,
                              q->transmit ? USB_DMA_TO_DEVICE : USB_DMA_FROM_DEVICE, false);
        if (result) { q->faulted = true; return result; }
        zero_bytes(&q->requests[index], sizeof(q->requests[index]));
        q->consumer = next_index(index); --q->pending;
        complete(context, request.cookie, USB_OK, actual);
    }
    return USB_OK;
}
int usb_qmu_stop(struct usb_qmu *q)
{
    if (!valid(q)) return USB_INVALID;
    if (!q->running) return USB_OK;
    uint32_t control;
    int result = usb_mmio_read(&q->mac, queue_register(q, 0), &control);
    if (!result && (control & ACTIVE)) result = usb_mmio_write(&q->mac, queue_register(q, 0), STOP);
    if (!result && q->transmit) result = modify(q, endpoint_register(q), 0, FLUSH_FIFO);
    if (!result) result = usb_mmio_poll(&q->mac, queue_register(q, 0), ACTIVE, 0, 1000, 1);
    if (!result && q->transmit) result = modify(q, endpoint_register(q), 0, FLUSH_FIFO);
    if (!result) result = modify(q, endpoint_register(q), DMA_REQUEST, 0);
    if (result) { q->faulted = true; return result; }
    q->running = false;
    return USB_OK;
}
int usb_qmu_cancel(struct usb_qmu *q, int reason, usb_qmu_completion complete, void *context)
{
    if (!valid(q) || !complete || reason == USB_OK) return USB_INVALID;
    int result = usb_qmu_stop(q);
    if (result) return result;
    int first = USB_OK;
    while (q->pending) {
        struct usb_qmu_request request = q->requests[q->consumer];
        result = usb_dma_sync(&q->dma, request.buffer, request.offset, request.length,
                              q->transmit ? USB_DMA_TO_DEVICE : USB_DMA_FROM_DEVICE, false);
        if (!first) first = result;
        zero_bytes(&q->requests[q->consumer], sizeof(request));
        q->consumer = next_index(q->consumer); --q->pending;
        complete(context, request.cookie, result ? result : reason, 0);
    }
    q->producer = q->consumer = 0;
    zero_bytes(q->descriptors.cpu, q->descriptors.size);
    result = usb_dma_sync(&q->dma, &q->descriptors, 0, q->descriptors.size, USB_DMA_BIDIRECTIONAL, true);
    if (!first) first = result;
    if (first) q->faulted = true;
    return first;
}
int usb_qmu_destroy(struct usb_qmu *q)
{
    if (!valid(q)) return USB_INVALID;
    if (q->running || q->pending) return USB_BUSY;
    usb_dma_free(&q->dma, &q->descriptors);
    zero_bytes(q, sizeof(*q)); return USB_OK;
}
