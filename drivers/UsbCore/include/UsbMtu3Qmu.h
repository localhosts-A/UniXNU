/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIXNU_USB_MTU3_QMU_H
#define UNIXNU_USB_MTU3_QMU_H
#include "UsbCore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_QMU_DESCRIPTORS 144
#define USB_QMU_DESCRIPTOR_BYTES 16
struct usb_qmu_request {
    struct usb_dma_buffer *buffer;
    size_t offset, length;
    void *cookie;
};
struct usb_qmu {
    struct usb_mmio mac;
    struct usb_dma_ops dma;
    struct usb_dma_buffer descriptors;
    struct usb_qmu_request requests[USB_QMU_DESCRIPTORS];
    uint16_t producer, consumer, pending;
    uint8_t endpoint;
    bool transmit, enhanced, running, faulted;
};
typedef void (*usb_qmu_completion)(void *context, void *cookie, int result, size_t actual);

/* Initialization only allocates the ring. The backend must already own and have
 * initialized MAC/PHY/endpoint/FIFO; this module does not acquire clocks/power.
 * Callbacks may release data but must not re-enter or destroy this queue. */
int usb_qmu_init(struct usb_qmu *, const struct usb_mmio *, const struct usb_dma_ops *,
                 uint8_t endpoint, bool transmit, bool enhanced);
int usb_qmu_submit(struct usb_qmu *, struct usb_dma_buffer *, size_t offset,
                   size_t length, bool append_zlp, void *cookie);
int usb_qmu_start(struct usb_qmu *);
int usb_qmu_reap(struct usb_qmu *, usb_qmu_completion, void *context);
int usb_qmu_stop(struct usb_qmu *);
/* Cancellation does not drop buffers or reset the ring until stop is proven. */
int usb_qmu_cancel(struct usb_qmu *, int reason, usb_qmu_completion, void *context);
/* Refuses to release memory while DMA is running or requests remain. */
int usb_qmu_destroy(struct usb_qmu *);

#ifdef __cplusplus
}
#endif
#endif
