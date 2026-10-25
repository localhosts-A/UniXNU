/* SPDX-License-Identifier: GPL-2.0-only */
/* Register semantics derived from MediaTek mtu3_gadget_ep0.c / mtu3_hw_regs.h,
 * Copyright (C) 2016 MediaTek Inc., Chunfeng Yun. Reference revision:
 * MiCode/MTK_kernel_device_modules e45c3199537fbba9aae5fb06c31fc8dbd3c1dcee.
 * This bounded CDC-specific state machine is shared by firmware and XNU. */
#include "UsbMtu3Ep0.h"

#define EPISR 0x80u
#define EPIESR 0x88u
#define EPIECR 0x8cu
#define EP0CSR 0x100u
#define RXCOUNT0 0x108u
#define FIFO0 0x300u
#define DEVICE_CONF 0x800u
#define EP_RST 0x804u
#define EP0_IRQ 1u
#define SETUPEND_IRQ (1u << 16)
#define RX_READY (1u << 16)
#define SETUP_READY (1u << 17)
#define TX_READY (1u << 18)
#define DATAEND (1u << 19)
#define DPHTX (1u << 20)
#define SENTSTALL (1u << 22)
#define FIFO_FULL (1u << 23)
#define SENDSTALL (1u << 25)
#define W1C (RX_READY | SETUP_READY | SENTSTALL)
#define ADDRESS_MASK (0x7fu << 24)
#define PACKET 64u

static bool backend_effect(const struct usb_control_reply *reply)
{
    return reply->effect == USB_CONTROL_CONFIGURATION ||
        reply->effect == USB_CONTROL_CLEAR_HALT || reply->effect == USB_CONTROL_SET_HALT;
}
static int abort_transfer(struct usb_mtu3_ep0 *ep)
{
    int result = USB_OK;
    if (ep->prepared) result = ep->ops.effect(ep->ops.context, &ep->pending, USB_EP0_ABORT);
    ep->prepared = false;
    ep->pending = (struct usb_control_reply){0};
    ep->position = ep->length = 0; ep->zlp = false;
    ep->state = result ? USB_EP0_FAULTED : USB_EP0_IDLE;
    return result;
}
static int fault(struct usb_mtu3_ep0 *ep, int result)
{
    (void)abort_transfer(ep); ep->state = USB_EP0_FAULTED;
    return result;
}
/* Never echo W1C or read-only status bits from a register read. */
static int csr_write(struct usb_mtu3_ep0 *ep, uint32_t csr, uint32_t ack, uint32_t set)
{
    uint32_t value = csr & ~(W1C | FIFO_FULL);
    return usb_mmio_write(&ep->mac, EP0CSR, value | ack | set);
}
static int stall(struct usb_mtu3_ep0 *ep, uint32_t csr, uint32_t ack)
{
    int result = abort_transfer(ep);
    if (result) return result;
    result = csr_write(ep, csr & ~(TX_READY | DATAEND), ack, SENDSTALL);
    ep->state = result ? USB_EP0_FAULTED : USB_EP0_STALLED;
    return result ? result : USB_STALL;
}
static int fifo_read(struct usb_mtu3_ep0 *ep, uint8_t *bytes, size_t length)
{
    for (size_t at = 0; at < length;) {
        uint32_t value;
        int result = usb_mmio_read(&ep->mac, FIFO0, &value);
        if (result) return result;
        size_t count = length - at < 4 ? length - at : 4;
        for (size_t i = 0; i < count; ++i) bytes[at++] = (uint8_t)(value >> (8u * (unsigned)i));
    }
    return USB_OK;
}
static int fifo_write(struct usb_mtu3_ep0 *ep, const uint8_t *bytes, size_t length)
{
    size_t at = 0;
    while (at < length) {
        size_t left = length - at;
        unsigned width = left >= 4 ? 4 : left >= 2 ? 2 : 1;
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= (uint32_t)bytes[at++] << (8u * i);
        int result;
        if (width == 4) result = usb_mmio_write(&ep->mac, FIFO0, value);
        else {
            ep->mac.barrier(ep->mac.context);
            result = ep->ops.write_tail(ep->ops.context, FIFO0, width, value);
            ep->mac.barrier(ep->mac.context);
        }
        if (result) return result;
    }
    return USB_OK;
}
static int prepare(struct usb_mtu3_ep0 *ep)
{
    if (!backend_effect(&ep->pending)) return USB_OK;
    if (!ep->ops.effect) return USB_UNSUPPORTED;
    ep->prepared = true; /* Also roll back partial preparation on failure. */
    return ep->ops.effect(ep->ops.context, &ep->pending, USB_EP0_PREPARE);
}
static int status(struct usb_mtu3_ep0 *ep, uint32_t csr, uint32_t ack)
{
    int result = prepare(ep);
    if (result) return stall(ep, csr, ack);
    result = csr_write(ep, csr & ~TX_READY, ack, DATAEND);
    if (result) return fault(ep, result);
    ep->state = USB_EP0_STATUS;
    return USB_OK;
}
static int complete(struct usb_mtu3_ep0 *ep)
{
    int result = USB_OK;
    if (ep->prepared) result = ep->ops.effect(ep->ops.context, &ep->pending, USB_EP0_APPLY);
    if (!result && ep->pending.effect == USB_CONTROL_ADDRESS) {
        uint32_t value;
        result = usb_mmio_read(&ep->mac, DEVICE_CONF, &value);
        if (!result) result = usb_mmio_write(&ep->mac, DEVICE_CONF,
            (value & ~ADDRESS_MASK) | ((uint32_t)ep->pending.argument << 24));
    }
    if (result) return fault(ep, result);
    usb_cdc_commit_control(ep->cdc, &ep->pending);
    ep->prepared = false; ep->pending = (struct usb_control_reply){0};
    ep->state = USB_EP0_IDLE;
    return USB_OK;
}
static int transmit(struct usb_mtu3_ep0 *ep, uint32_t csr)
{
    size_t count = ep->length - ep->position;
    if (count > PACKET) count = PACKET;
    int result = fifo_write(ep, ep->data + ep->position, count);
    if (result) return fault(ep, result);
    ep->position += count;
    bool last = count < PACKET || (ep->position == ep->length && !ep->zlp);
    result = csr_write(ep, csr, 0, TX_READY);
    if (result) return fault(ep, result);
    ep->state = last ? USB_EP0_IN_LAST : USB_EP0_IN;
    return USB_OK;
}
static int setup(struct usb_mtu3_ep0 *ep, uint32_t csr, uint64_t now)
{
    int result = abort_transfer(ep);
    if (result) return result;
    uint32_t count;
    result = usb_mmio_read(&ep->mac, RXCOUNT0, &count);
    if (result) return fault(ep, result);
    if (count != 8) return stall(ep, csr, SETUP_READY);
    uint8_t bytes[8];
    result = fifo_read(ep, bytes, sizeof(bytes));
    if (!result) result = usb_setup_decode(bytes, sizeof(bytes), &ep->setup);
    if (result) return fault(ep, result);
    ep->started_us = now;
    csr &= ~(SENDSTALL | TX_READY | DATAEND | DPHTX);
    if (!(ep->setup.type & 0x80) && ep->setup.length) {
        if (ep->setup.length > sizeof(ep->data)) return stall(ep, csr, SETUP_READY);
        result = csr_write(ep, csr, SETUP_READY, 0);
        if (result) return fault(ep, result);
        ep->length = ep->setup.length; ep->state = USB_EP0_OUT;
        return USB_OK;
    }
    uint32_t ack = SETUP_READY;
    if (ep->setup.length) {
        /* Establish the IN data phase before dispatch, including requests that
         * will stall. This follows the controller's documented Linux sequence. */
        result = csr_write(ep, csr, SETUP_READY, DPHTX);
        if (result) return fault(ep, result);
        csr |= DPHTX; ack = 0;
    }
    result = usb_cdc_control(ep->cdc, &ep->setup, 0, 0, ep->data, sizeof(ep->data), &ep->pending);
    if (result) return stall(ep, csr, ack);
    if (!ep->setup.length) return status(ep, csr, SETUP_READY);
    ep->length = ep->pending.length;
    ep->zlp = ep->length < ep->setup.length && ep->length % PACKET == 0;
    return transmit(ep, csr);
}

int usb_mtu3_ep0_init(struct usb_mtu3_ep0 *ep, const struct usb_mmio *mac,
                      const struct usb_mtu3_ep0_ops *ops, struct usb_cdc *cdc, bool owned)
{
    if (!ep || !mac || !ops || !cdc || !cdc->vendor || !cdc->product) return USB_INVALID;
    if (!owned || !mac->read32 || !mac->write32 || !mac->barrier || !ops->write_tail) return USB_UNSUPPORTED;
    if (mac->size < EP_RST + 4) return USB_RANGE;
    *ep = (struct usb_mtu3_ep0){0}; ep->mac = *mac; ep->ops = *ops; ep->cdc = cdc;
    return USB_OK;
}
static int reset_fifo(struct usb_mtu3_ep0 *ep)
{
    uint32_t value;
    int result = usb_mmio_read(&ep->mac, EP_RST, &value);
    if (!result) result = usb_mmio_write(&ep->mac, EP_RST, value | 1u);
    if (!result) result = usb_mmio_write(&ep->mac, EP_RST, value & ~1u);
    if (!result) result = usb_mmio_write(&ep->mac, EP0CSR, PACKET | W1C);
    if (!result) result = usb_mmio_write(&ep->mac, EPISR, EP0_IRQ | SETUPEND_IRQ);
    return result;
}
int usb_mtu3_ep0_bus_reset(struct usb_mtu3_ep0 *ep, bool high_speed)
{
    if (!ep || !ep->cdc || ep->state == USB_EP0_OFF) return USB_INVALID;
    int result = abort_transfer(ep);
    if (result) return result;
    uint32_t value;
    result = reset_fifo(ep);
    if (!result) result = usb_mmio_read(&ep->mac, DEVICE_CONF, &value);
    if (!result) result = usb_mmio_write(&ep->mac, DEVICE_CONF, value & ~ADDRESS_MASK);
    if (result) return fault(ep, result);
    usb_cdc_bus_reset(ep->cdc, high_speed);
    return USB_OK;
}
int usb_mtu3_ep0_start(struct usb_mtu3_ep0 *ep)
{
    if (!ep || !ep->cdc) return USB_INVALID;
    if (ep->state != USB_EP0_OFF) return USB_BUSY;
    ep->state = USB_EP0_IDLE;
    int result = usb_mtu3_ep0_bus_reset(ep, ep->cdc->high_speed);
    if (!result) result = usb_mmio_write(&ep->mac, EPIESR, EP0_IRQ | SETUPEND_IRQ);
    return result ? fault(ep, result) : USB_OK;
}
int usb_mtu3_ep0_stop(struct usb_mtu3_ep0 *ep)
{
    if (!ep || !ep->cdc) return USB_INVALID;
    if (ep->state == USB_EP0_OFF) return USB_OK;
    int first = usb_mtu3_ep0_bus_reset(ep, ep->cdc->high_speed);
    int result = usb_mmio_write(&ep->mac, EPIECR, EP0_IRQ | SETUPEND_IRQ);
    if (!first) first = result;
    ep->state = first ? USB_EP0_FAULTED : USB_EP0_OFF;
    return first;
}
int usb_mtu3_ep0_poll(struct usb_mtu3_ep0 *ep, uint64_t now)
{
    if (!ep || !ep->cdc || now < ep->last_us) return USB_INVALID;
    if (ep->state == USB_EP0_OFF) return USB_DISCONNECTED;
    if (ep->state == USB_EP0_FAULTED) return USB_IO;
    ep->last_us = now;
    uint32_t irq = 0, csr;
    int result = usb_mmio_read(&ep->mac, EPISR, &irq);
    irq &= EP0_IRQ | SETUPEND_IRQ;
    if (!result && irq) result = usb_mmio_write(&ep->mac, EPISR, irq);
    if (!result) result = usb_mmio_read(&ep->mac, EP0CSR, &csr);
    if (result) return fault(ep, result);
    if ((irq & SETUPEND_IRQ) || (csr & SENTSTALL)) {
        result = abort_transfer(ep);
        if (result) return result;
        csr &= ~(SENDSTALL | TX_READY | DATAEND);
        if (csr & SENTSTALL) {
            result = csr_write(ep, csr, SENTSTALL, 0);
            if (result) return fault(ep, result);
        }
    } else if (ep->state == USB_EP0_STATUS && !(csr & DATAEND)) {
        result = complete(ep);
        if (result) return result;
    }
    if (csr & SETUP_READY) return setup(ep, csr, now);
    if (ep->state == USB_EP0_IDLE || ep->state == USB_EP0_STALLED) return USB_AGAIN;
    if (now - ep->started_us >= 1000000) {
        result = abort_transfer(ep);
        if (!result) result = reset_fifo(ep);
        if (result) return fault(ep, result);
        /* EP0 timeout does not reset device address/configuration. The port owner
         * must explicitly reset the entire device if it wants to re-enumerate. */
        return USB_TIMEOUT;
    }
    if (!(irq & EP0_IRQ)) return USB_AGAIN;
    if (ep->state == USB_EP0_IN || ep->state == USB_EP0_IN_LAST) {
        if (csr & (TX_READY | FIFO_FULL)) return USB_AGAIN;
        if (ep->state == USB_EP0_IN) return transmit(ep, csr);
        return status(ep, csr, 0);
    }
    if (ep->state == USB_EP0_OUT && (csr & RX_READY)) {
        uint32_t count;
        result = usb_mmio_read(&ep->mac, RXCOUNT0, &count);
        if (result) return fault(ep, result);
        if (count > PACKET || count > ep->length - ep->position ||
            (count < PACKET && count != ep->length - ep->position)) return stall(ep, csr, RX_READY);
        result = fifo_read(ep, ep->data + ep->position, count);
        if (result) return fault(ep, result);
        ep->position += count;
        if (ep->position != ep->length) return csr_write(ep, csr, RX_READY, 0);
        result = usb_cdc_control(ep->cdc, &ep->setup, ep->data, ep->length, 0, 0, &ep->pending);
        if (result) return stall(ep, csr, RX_READY);
        return status(ep, csr, RX_READY);
    }
    return USB_AGAIN;
}
