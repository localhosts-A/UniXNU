/* SPDX-License-Identifier: GPL-2.0-only */
/* MediaTek register/FIFO layout: mtu3_core.c / mtu3_hw_regs.h, Copyright (C)
 * 2016 MediaTek Inc., Chunfeng Yun; fixed reference revision
 * MiCode/MTK_kernel_device_modules e45c3199537fbba9aae5fb06c31fc8dbd3c1dcee. */
#include "UsbMtu3Endpoints.h"

#define ACTIVE (1u << 15)
#define DMAREQ (1u << 29)
#define SENDSTALL (1u << 21)
#define SENTSTALL (1u << 22)
#define RX_READY (1u << 16)
#define DONE_MASK ((1u << 1) | (1u << 2) | (1u << 18))
#define LPM_MASK ((1u << 1) | (1u << 2) | (1u << 18))

static const uint8_t addresses[3] = {0x81, 0x82, 0x02};
static uint32_t csr(uint8_t address)
{ return (address & 0x80 ? 0x100u : 0x200u) + 0x10u * (uint32_t)(address & 15); }
static uint32_t qcsr(uint8_t address)
{ return (address & 0x80 ? 0x500u : 0x600u) + 0x10u * (uint32_t)(address & 15); }
static uint32_t rst_bit(uint8_t address)
{ return 1u << ((address & 0x80 ? 16u : 0u) + (unsigned)(address & 15)); }
static int update(struct usb_mtu3_endpoints *ep, uint32_t offset, uint32_t clear, uint32_t set)
{
    uint32_t value;
    int result = usb_mmio_read(&ep->mac, offset, &value);
    return result ? result : usb_mmio_write(&ep->mac, offset, (value & ~clear) | set);
}
static int reset(struct usb_mtu3_endpoints *ep, uint8_t address)
{
    uint32_t bit = rst_bit(address);
    int result = update(ep, 0x804, 0, bit);
    return result ? result : update(ep, 0x804, bit, 0);
}
static int idle(struct usb_mtu3_endpoints *ep, uint8_t address)
{
    uint32_t value;
    int result = usb_mmio_read(&ep->mac, qcsr(address), &value);
    return result ? result : value & ACTIVE ? USB_BUSY : USB_OK;
}
static int all_idle(struct usb_mtu3_endpoints *ep)
{
    for (unsigned i = 0; i < 3; ++i) {
        int result = idle(ep, addresses[i]);
        if (result) return result;
    }
    return USB_OK;
}
static int fault(struct usb_mtu3_endpoints *ep, int result)
{ ep->faulted = true; return result; }

int usb_mtu3_endpoints_init(struct usb_mtu3_endpoints *ep, const struct usb_mmio *mac,
                           const struct usb_mmio *ippc, bool owned, bool gen2cp_unsupported)
{
    if (!ep || !mac || !ippc) return USB_INVALID;
    *ep = (struct usb_mtu3_endpoints){0};
    if (!owned || !mac->read32 || !mac->write32 || !mac->barrier || !ippc->read32) return USB_UNSUPPORTED;
    if (mac->size < 0xc14 || ippc->size < 0xa8) return USB_RANGE;
    uint32_t version, cap, info, tx, rx;
    int result = usb_mmio_read(ippc, 0xa4, &version);
    if (!result) result = usb_mmio_read(ippc, 0x28, &cap);
    if (!result) result = usb_mmio_read(mac, 0xc10, &info);
    if (!result) result = usb_mmio_read(mac, 0xc08, &tx);
    if (!result) result = usb_mmio_read(mac, 0xc0c, &rx);
    if (result) return result;
    /* Zero/all-ones reads are not a usable capability description. */
    if (!(version >> 16) || (version >> 16) == 0xffff || cap == UINT32_MAX ||
        (info & 31) < 2 || ((info >> 8) & 31) < 2 || info == UINT32_MAX) return USB_UNSUPPORTED;
    ep->enhanced = !gen2cp_unsupported && (version >> 16) >= 0x1003;
    ep->separate_fifo = (cap & 0xff) != 0;
    if (ep->separate_fifo) { ep->tx_bytes = tx; ep->rx_bytes = rx; }
    else {
        ep->tx_base = 64; ep->tx_bytes = (tx / 512 / 2) * 512;
        ep->rx_base = ep->tx_base + ep->tx_bytes; ep->rx_bytes = ep->tx_bytes;
    }
    /* FIFOADDR is a 13-bit field in units of 16 bytes. Reject malformed
     * capabilities instead of truncating them while programming the address. */
    if (tx > 131008 || rx > 131008 || ep->tx_bytes < 2048 || ep->rx_bytes < 1024 ||
        ep->tx_base + 2048 > 131072 || ep->rx_base + 1024 > 131072) return USB_RANGE;
    ep->mac = *mac;
    return USB_OK;
}
int usb_mtu3_endpoints_configure(struct usb_mtu3_endpoints *ep, bool high_speed)
{
    if (!ep || !ep->mac.read32) return USB_INVALID;
    if (ep->faulted) return USB_IO;
    if (ep->configured) return USB_BUSY;
    int result = all_idle(ep);
    if (result) return result;
    /* Two FIFO slots per endpoint. USB2 has burst=mult=0, so version-dependent
     * MAX_PKT/MULT fields are zero on both original and enhanced controllers. */
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t address = addresses[i];
        uint32_t fifo = i == 0 ? ep->tx_base : i == 1 ? ep->tx_base + 1024 : ep->rx_base;
        uint32_t packet = i == 0 ? 16 : high_speed ? 512 : 64;
        uint32_t csr1 = (1u << 8) | (i == 0 ? 1u << 4 : 0);
        uint32_t csr2 = (fifo >> 4) | (9u << 16) | (i == 0 ? 9u << 24 : 0);
        result = reset(ep, address);
        if (!result) result = usb_mmio_write(&ep->mac, csr(address), packet | DMAREQ);
        if (!result) result = usb_mmio_write(&ep->mac, csr(address) + 4, csr1);
        if (!result) result = usb_mmio_write(&ep->mac, csr(address) + 8, csr2);
        if (result) return fault(ep, result);
    }
    result = update(ep, 0x828, 0, LPM_MASK);
    if (!result) result = update(ep, 0x860, 0, 1u << 2);
    if (!result) result = usb_mmio_write(&ep->mac, 0x708, DONE_MASK);
    if (result) return fault(ep, result);
    ep->configured = true; ep->halted_in = ep->halted_out = 0;
    return USB_OK;
}
int usb_mtu3_endpoints_deconfigure(struct usb_mtu3_endpoints *ep)
{
    if (!ep || !ep->mac.read32) return USB_INVALID;
    if (!ep->configured && !ep->faulted) return USB_OK;
    int result = all_idle(ep);
    if (result) return result;
    result = usb_mmio_write(&ep->mac, 0x70c, DONE_MASK);
    for (unsigned i = 0; !result && i < 3; ++i) {
        uint8_t address = addresses[i];
        result = usb_mmio_write(&ep->mac, csr(address), 0);
        if (!result) result = usb_mmio_write(&ep->mac, csr(address) + 4, 0);
        if (!result) result = usb_mmio_write(&ep->mac, csr(address) + 8, 0);
        if (!result) result = reset(ep, address);
    }
    if (!result) result = update(ep, 0x828, LPM_MASK, 0);
    if (!result) result = update(ep, 0x860, 1u << 2, 0);
    if (result) return fault(ep, result);
    ep->configured = ep->faulted = false; ep->halted_in = ep->halted_out = 0;
    return USB_OK;
}
int usb_mtu3_endpoints_halt(struct usb_mtu3_endpoints *ep, uint8_t address, bool halt)
{
    if (!ep || !ep->mac.read32) return USB_INVALID;
    if (ep->faulted) return USB_IO;
    if (!ep->configured || (address != 0x81 && address != 0x82 && address != 0x02)) return USB_INVALID;
    int result = idle(ep, address);
    if (result) return result;
    uint32_t value;
    result = usb_mmio_read(&ep->mac, csr(address), &value);
    if (result) return fault(ep, result);
    value &= ~(SENTSTALL | (address & 0x80 ? 0u : RX_READY));
    value = halt ? value | SENDSTALL : (value & ~SENDSTALL) | SENTSTALL;
    result = usb_mmio_write(&ep->mac, csr(address), value);
    if (!result && !halt) result = reset(ep, address);
    if (result) return fault(ep, result);
    uint16_t *map = address & 0x80 ? &ep->halted_in : &ep->halted_out;
    uint16_t bit = (uint16_t)(1u << (address & 15));
    if (halt) *map |= bit; else *map &= (uint16_t)~bit;
    return USB_OK;
}
