/* SPDX-License-Identifier: GPL-2.0-only */
/* Controller register sequencing based on MediaTek mtu3_core.c/mtu3_qmu.c,
 * Copyright (C) 2016 MediaTek Inc., Chunfeng Yun. Fixed reference revision:
 * MiCode/MTK_kernel_device_modules e45c3199537fbba9aae5fb06c31fc8dbd3c1dcee. */
#include "UsbMtu3Device.h"

#define POWER 0x2404u
#define COMMON_INTR 0x241cu
#define COMMON_ENABLE 0x2418u
#define USB_RESET (1u << 2)
#define USB_DISCONNECT (1u << 5)
#define COMMON_MASK (0x37u | (1u << 9))
#define DONE_MASK 0x40006u
#define FATAL_ERRORS ((1u << 1) | (1u << 2) | (1u << 17) | (1u << 18))
#define ERROR_MASK (FATAL_ERRORS | (1u << 20))
#define SOFT_CONNECT ((1u << 6) | 1u)
#define VBUS_VALID (1u << 9)

static int update(struct usb_mtu3_device *d, uint32_t offset, uint32_t clear, uint32_t set)
{
    uint32_t value;
    int result = usb_mmio_read(&d->mac, offset, &value);
    return result ? result : usb_mmio_write(&d->mac, offset, (value & ~clear) | set);
}
static void completed(void *context, void *cookie, int result, size_t actual)
{
    struct usb_mtu3_device *d = context;
    struct usb_mtu3_channel *c = cookie;
    if (c == &d->channels[1]) {
        if (!result) d->tx_dma_completed += actual;
        else __atomic_fetch_add(&d->cdc->log.dropped, c->submitted, __ATOMIC_RELAXED);
    } else if (c == &d->channels[2] && !result) d->rx_discarded += actual;
    else if (c == &d->channels[0] && result) d->notification_state = UINT16_MAX;
    c->submitted = 0;
}
static int cancel_channel(struct usb_mtu3_device *d, unsigned index, int reason)
{
    return usb_qmu_cancel(&d->channels[index].queue, reason, completed, d);
}
static int cancel_all(struct usb_mtu3_device *d, int reason)
{
    int first = USB_OK;
    for (unsigned i = 0; i < 3; ++i) {
        int result = cancel_channel(d, i, reason);
        if (!first) first = result;
    }
    return first;
}
static int tail(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    struct usb_mtu3_device *d = context;
    return d->write_tail(d->tail_context, offset, width, value);
}
static int endpoint_effect(void *context, const struct usb_control_reply *reply, enum usb_ep0_phase phase)
{
    struct usb_mtu3_device *d = context;
    if (d->tearing_down && phase == USB_EP0_ABORT) { d->configuring = false; return USB_OK; }
    if (d->faulted) return USB_IO;
    bool configuration = reply->effect == USB_CONTROL_CONFIGURATION;
    unsigned index = reply->argument == 0x81 ? 0 : reply->argument == 0x82 ? 1 : 2;
    int result = USB_OK;
    if (phase == USB_EP0_PREPARE) {
        d->configuring = true;
        result = configuration ? cancel_all(d, USB_DISCONNECTED) : cancel_channel(d, index, USB_STALL);
    } else if (phase == USB_EP0_APPLY) {
        if (!configuration) result = usb_mtu3_endpoints_halt(&d->endpoints, reply->argument,
                                                           reply->effect == USB_CONTROL_SET_HALT);
        else {
            /* Do not reset endpoint toggles in PREPARE: an aborted control must
             * preserve the host's committed configuration and data sequence. */
            result = usb_mtu3_endpoints_deconfigure(&d->endpoints);
            if (!result && reply->argument) result = usb_mtu3_endpoints_configure(&d->endpoints, d->cdc->high_speed);
            d->notification_state = UINT16_MAX;
        }
        d->configuring = false;
    } else if (phase == USB_EP0_ABORT) {
        /* PREPARE only cancelled queues; endpoint CSR/toggles are unchanged. */
        d->configuring = false;
    }
    if (result) d->faulted = true;
    return result;
}
static int idle_hardware(struct usb_mtu3_device *d)
{
    /* Reject inherited queues rather than replacing a base with live DMA. */
    const uint32_t offsets[3] = {0x510, 0x520, 0x620};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t value;
        int result = usb_mmio_read(&d->mac, offsets[i], &value);
        if (result) return result;
        if (value & (1u << 15)) return USB_BUSY;
    }
    return USB_OK;
}
int usb_mtu3_device_init(struct usb_mtu3_device *d, const struct usb_mmio *mac,
                         const struct usb_mmio *ippc, const struct usb_dma_ops *dma,
                         struct usb_cdc *cdc, void *tail_context,
                         int (*write_tail)(void *, uint32_t, unsigned, uint32_t),
                         bool qualified, bool gen2cp_unsupported)
{
    if (!d || !mac || !ippc || !dma || !cdc || !write_tail || !mac->delay_us) return USB_INVALID;
    if (d->initialized) return USB_BUSY;
    if (!qualified || !dma->qualified) return USB_UNSUPPORTED;
    if (mac->size < 0x2464 || ippc->size < 0xa8) return USB_RANGE;
    *d = (struct usb_mtu3_device){0}; d->mac = *mac; d->ippc = *ippc; d->dma = *dma;
    d->cdc = cdc; d->tail_context = tail_context; d->write_tail = write_tail;
    int result = usb_mtu3_endpoints_init(&d->endpoints, mac, ippc, qualified, gen2cp_unsupported);
    if (!result) result = idle_hardware(d);
    struct usb_mtu3_ep0_ops ops = {d, tail, endpoint_effect};
    if (!result) result = usb_mtu3_ep0_init(&d->ep0, mac, &ops, cdc, qualified);
    const uint8_t addresses[3] = {0x81, 0x82, 0x02};
    for (unsigned i = 0; !result && i < 3; ++i) {
        struct usb_mtu3_channel *c = &d->channels[i]; c->address = addresses[i];
        result = usb_qmu_init(&c->queue, mac, dma, c->address & 15, i != 2, d->endpoints.enhanced);
        if (!result) result = usb_dma_alloc32(dma, i == 0 ? 16 : 512, 64, &c->buffer);
    }
    if (result) {
        for (unsigned i = 0; i < 3; ++i) {
            struct usb_mtu3_channel *c = &d->channels[i];
            usb_dma_free(dma, &c->buffer);
            if (c->queue.descriptors.cpu) (void)usb_qmu_destroy(&c->queue);
        }
        *d = (struct usb_mtu3_device){0}; return result;
    }
    d->initialized = d->quiesced = true; d->notification_state = UINT16_MAX;
    return USB_OK;
}
static int disable_interrupts(struct usb_mtu3_device *d)
{
    const uint32_t offsets[] = {0xc, 0x8c, 0x70c, 0x71c, COMMON_ENABLE, 0x850};
    int first = USB_OK;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        int result = usb_mmio_write(&d->mac, offsets[i], i < 4 ? UINT32_MAX : 0);
        if (!first) first = result;
    }
    return first;
}
static int clear_events(struct usb_mtu3_device *d)
{
    /* QISAR1 is read-update, not W1C. Acknowledge its detailed error sources. */
    const uint32_t offsets[] = {0x80, COMMON_INTR, 0x854, 0x700, 0x780, 0x7c0, 0x7d0};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        int result = usb_mmio_write(&d->mac, offsets[i], UINT32_MAX);
        if (result) return result;
    }
    return USB_OK;
}
int usb_mtu3_device_start(struct usb_mtu3_device *d)
{
    if (!d || !d->initialized) return USB_INVALID;
    if (d->faulted) return USB_IO;
    if (d->started || !d->quiesced) return USB_BUSY;
    uint32_t vbus;
    int result = usb_mmio_read(&d->ippc, 0x18, &vbus);
    if (result) return result;
    if (!(vbus & VBUS_VALID)) return USB_AGAIN;
    result = idle_hardware(d);
    if (result) return result;
    d->quiesced = false;
    result = update(d, POWER, SOFT_CONNECT, 0);
    if (!result) result = disable_interrupts(d);
    if (!result) result = clear_events(d);
    if (!result) result = update(d, 0x141c, 1, 0); /* USB3 disabled. */
    if (!result) result = usb_mmio_write(&d->mac, 0x800, 0); /* HW speed selection. */
    if (!result) result = update(d, 0xc84, 3, 0); /* Never force VBUS. */
    if (!result) result = usb_mmio_write(&d->mac, 0x428, d->endpoints.enhanced ? UINT32_MAX : 0);
    if (!result) result = usb_mmio_write(&d->mac, 0x400, 0); /* GPD checksum disabled. */
    if (!result) result = update(d, 0x2414, 0x43f, 0); /* No test/forced speed/U3 fallback. */
    if (!result) result = update(d, 0x2424, 0xfu << 16, 0); /* Chirp response delay. */
    if (!result) result = usb_mmio_write(&d->mac, 0x828, 1); /* EP0 L1 exit check. */
    if (!result) result = usb_mmio_write(&d->mac, 0x860, 0);
    if (!result) result = usb_mtu3_ep0_start(&d->ep0);
    if (!result) result = usb_mmio_write(&d->mac, COMMON_ENABLE, COMMON_MASK);
    if (!result) result = usb_mmio_write(&d->mac, 0x718, ERROR_MASK);
    if (!result) result = usb_mmio_write(&d->mac, 0x850, 1);
    if (!result) result = usb_mmio_write(&d->mac, 0x8, 0x33); /* BMU/QMU/MAC2/EPCTL */
    if (!result) result = update(d, POWER, 0, (1u << 5) | (1u << 10) | SOFT_CONNECT);
    if (result) { d->faulted = true; (void)usb_mtu3_device_stop(d); return result; }
    d->started = true; d->connected = d->suspended = false;
    return USB_OK;
}
int usb_mtu3_device_stop(struct usb_mtu3_device *d)
{
    if (!d || !d->initialized) return USB_INVALID;
    if (d->quiesced) return USB_OK;
    d->started = false; d->tearing_down = true;
    int first = update(d, POWER, SOFT_CONNECT, 0);
    int result = disable_interrupts(d); if (!first) first = result;
    result = cancel_all(d, USB_DISCONNECTED); if (!first) first = result;
    /* Never deconfigure/reset endpoints while any DMA stop is uncertain. */
    if (!result) {
        result = usb_mtu3_endpoints_deconfigure(&d->endpoints); if (!first) first = result;
        if (d->ep0.state != USB_EP0_OFF) { result = usb_mtu3_ep0_stop(&d->ep0); if (!first) first = result; }
    }
    d->tearing_down = false; d->connected = d->suspended = d->configuring = false;
    if (first) d->faulted = true;
    else d->quiesced = true;
    return first;
}
int usb_mtu3_device_destroy(struct usb_mtu3_device *d)
{
    if (!d || !d->initialized) return USB_INVALID;
    int result = usb_mtu3_device_stop(d);
    if (result) return result;
    for (unsigned i = 0; i < 3; ++i) {
        struct usb_qmu *q = &d->channels[i].queue;
        if (!q->descriptors.cpu || !q->endpoint || q->endpoint > 15) return USB_INVALID;
        if (q->running || q->pending) return USB_BUSY;
    }
    for (unsigned i = 0; i < 3; ++i) {
        result = usb_qmu_destroy(&d->channels[i].queue);
        if (result) return result;
        usb_dma_free(&d->dma, &d->channels[i].buffer);
    }
    *d = (struct usb_mtu3_device){0}; return USB_OK;
}
static int reset_link(struct usb_mtu3_device *d, bool high, bool connected)
{
    d->tearing_down = true;
    int result = cancel_all(d, USB_DISCONNECTED);
    if (!result) result = usb_mtu3_endpoints_deconfigure(&d->endpoints);
    if (!result) result = usb_mtu3_ep0_bus_reset(&d->ep0, high);
    if (!result) result = clear_events(d);
    d->tearing_down = false; d->configuring = false;
    if (result) return result;
    d->connected = connected; d->suspended = false; d->notification_state = UINT16_MAX;
    return USB_OK;
}
static int submit(struct usb_mtu3_device *d, unsigned index, size_t length)
{
    struct usb_mtu3_channel *c = &d->channels[index];
    bool zlp = index == 1 && length % (d->cdc->high_speed ? 512u : 64u) == 0;
    int result = usb_qmu_submit(&c->queue, &c->buffer, 0, length, zlp, c);
    if (result) {
        if (index == 1) __atomic_fetch_add(&d->cdc->log.dropped, length, __ATOMIC_RELAXED);
        return result;
    }
    c->submitted = length;
    return usb_qmu_start(&c->queue);
}
static int service_data(struct usb_mtu3_device *d)
{
    for (unsigned i = 0; i < 3; ++i) {
        int result = usb_qmu_reap(&d->channels[i].queue, completed, d);
        if (result && result != USB_AGAIN) return result;
    }
    if (!d->cdc->configuration || d->configuring || d->suspended) return USB_OK;
    struct usb_mtu3_channel *n = &d->channels[0], *tx = &d->channels[1], *rx = &d->channels[2];
    uint16_t serial_state = d->cdc->control_lines & 1 ? 3 : 0;
    if (!n->submitted && !(d->cdc->halted_in & 2) && serial_state != d->notification_state) {
        const uint8_t header[10] = {0xa1, 0x20, 0, 0, 0, 0, 2, 0, (uint8_t)serial_state, 0};
        uint8_t *bytes = n->buffer.cpu;
        for (unsigned i = 0; i < sizeof(header); ++i) bytes[i] = header[i];
        int result = submit(d, 0, sizeof(header));
        if (result) return result;
        d->notification_state = serial_state;
    }
    if (!rx->submitted && !(d->cdc->halted_out & 4)) {
        int result = submit(d, 2, d->cdc->high_speed ? 512 : 64);
        if (result) return result;
    }
    if (!tx->submitted && (d->cdc->control_lines & 1) && !(d->cdc->halted_in & 4)) {
        size_t count = usb_log_take(&d->cdc->log, tx->buffer.cpu, tx->buffer.size);
        if (count) return submit(d, 1, count);
    }
    return USB_OK;
}
int usb_mtu3_device_poll(struct usb_mtu3_device *d, uint64_t now)
{
    if (!d || !d->initialized || now < d->last_us) return USB_INVALID;
    if (d->faulted) return USB_IO;
    if (!d->started) return USB_DISCONNECTED;
    d->last_us = now;
    uint32_t vbus, common, link, speed = 0, errors;
    int result = usb_mmio_read(&d->ippc, 0x18, &vbus);
    if (!result) result = usb_mmio_read(&d->mac, COMMON_INTR, &common);
    if (!result) result = usb_mmio_write(&d->mac, COMMON_INTR, common & COMMON_MASK);
    if (!result) result = usb_mmio_read(&d->mac, 0x854, &link);
    if (!result && (link & 1)) result = usb_mmio_write(&d->mac, 0x854, 1);
    if (result) goto failed;
    if (!(vbus & VBUS_VALID) || (common & USB_DISCONNECT)) {
        if (d->connected || d->cdc->configuration) result = reset_link(d, d->cdc->high_speed, false);
        if (result) goto failed;
        return USB_DISCONNECTED;
    }
    if ((common & USB_RESET) || (link & 1)) {
        result = usb_mmio_read(&d->mac, 0x800, &speed);
        if (result) goto failed;
        speed &= 7;
        if (speed != 0 && speed != 1 && speed != 3) { result = USB_UNSUPPORTED; goto failed; }
        result = reset_link(d, speed == 3, speed != 0);
        if (result) goto failed;
        if (speed) {
            result = usb_mmio_write(&d->mac, 0x2444, 0xa4f0);
            if (!result) result = update(d, POWER, 0, (1u << 14) | (1u << 13));
            if (result) goto failed;
        }
    }
    if (common & 1) d->suspended = true;
    if (common & ((1u << 1) | (1u << 9))) d->suspended = false;
    result = usb_mmio_read(&d->mac, 0x710, &errors);
    if (result) goto failed;
    if (errors & (1u << 20)) {
        /* RX ZLP is a legal USB packet, not a corrupted request. With COZ set,
         * QMU continues the same RX descriptor. Clear the detailed status. */
        uint32_t zlp;
        result = usb_mmio_read(&d->mac, 0x7d0, &zlp);
        if (!result) result = usb_mmio_write(&d->mac, 0x7d0, zlp);
        if (result) goto failed;
    }
    if (errors & FATAL_ERRORS) { result = USB_IO; goto failed; }
    uint32_t done;
    result = usb_mmio_read(&d->mac, 0x700, &done);
    if (!result && (done & DONE_MASK)) result = usb_mmio_write(&d->mac, 0x700, done & DONE_MASK);
    if (result) goto failed;
    if (!d->connected) return USB_AGAIN;
    result = usb_mtu3_ep0_poll(&d->ep0, now);
    if (result != USB_OK && result != USB_AGAIN && result != USB_STALL && result != USB_TIMEOUT) goto failed;
    result = service_data(d);
    if (result) goto failed;
    return USB_OK;
failed:
    d->faulted = true; (void)usb_mtu3_device_stop(d); return result;
}
size_t usb_mtu3_device_log(struct usb_mtu3_device *d, const void *bytes, size_t length)
{ return d && d->initialized ? usb_log_write(&d->cdc->log, bytes, length) : 0; }
