/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIXNU_USB_MTU3_DEVICE_H
#define UNIXNU_USB_MTU3_DEVICE_H
#include "UsbMtu3Ep0.h"
#include "UsbMtu3Endpoints.h"
#include "UsbMtu3Qmu.h"
#ifdef __cplusplus
extern "C" {
#endif

struct usb_mtu3_channel {
    struct usb_qmu queue;
    struct usb_dma_buffer buffer;
    size_t submitted;
    uint8_t address;
};
struct usb_mtu3_device {
    struct usb_mmio mac, ippc;
    struct usb_dma_ops dma;
    struct usb_cdc *cdc;
    struct usb_mtu3_ep0 ep0;
    struct usb_mtu3_endpoints endpoints;
    struct usb_mtu3_channel channels[3]; /* notification IN1, log IN2, discard OUT2 */
    void *tail_context;
    int (*write_tail)(void *, uint32_t, unsigned, uint32_t);
    uint64_t tx_dma_completed, rx_discarded, last_us;
    uint16_t notification_state;
    bool initialized, started, connected, suspended, configuring, tearing_down;
    bool quiesced, faulted;
};

/* The platform must validate resources, acquire exclusive MAC/PHY/clocks and
 * reset the device MAC, establish the actual DMA policy, and select device/sink
 * before calling init. No clocks, PHY, TCPC, VBUS-source or IOMMU bypass is set
 * here. init reads capabilities and allocates independent DMA buffers only.
 * Zero-initialize storage and never reinitialize a live object. */
int usb_mtu3_device_init(struct usb_mtu3_device *, const struct usb_mmio *mac,
                         const struct usb_mmio *ippc, const struct usb_dma_ops *,
                         struct usb_cdc *, void *tail_context,
                         int (*write_tail)(void *, uint32_t, unsigned, uint32_t),
                         bool port_qualified, bool gen2cp_unsupported);
/* start only enables USB2 softconnect when hardware reports VBUS valid. */
int usb_mtu3_device_start(struct usb_mtu3_device *);
/* Serialized IRQ/timer worker; monotonic microseconds. No allocations; no waits
 * except bounded queue stop during control changes/reset/fault. OUT data is
 * drained/discarded: this interface exposes diagnostic output, not a shell. */
int usb_mtu3_device_poll(struct usb_mtu3_device *, uint64_t now_us);
size_t usb_mtu3_device_log(struct usb_mtu3_device *, const void *, size_t);
/* Stop submissions, disconnect pullup, mask IRQs, cancel/stop all queues, then
 * deconfigure endpoints and EP0. A failure retains every DMA buffer and the
 * caller MUST NOT power down, change role or release the object. Retry is safe.
 * Successful stop is idempotent for ExitBootServices retries. */
int usb_mtu3_device_stop(struct usb_mtu3_device *);
int usb_mtu3_device_destroy(struct usb_mtu3_device *);

#ifdef __cplusplus
}
#endif
#endif
