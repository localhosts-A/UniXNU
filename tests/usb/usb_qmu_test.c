/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200112L
#include "UsbMtu3Qmu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct model {
    uint32_t regs[0x800 / 4];
    uint64_t next_dma;
    unsigned allocations, releases, syncs, mmio_writes, flushes, delay, completed, resumes;
    int fail_sync_at, result;
    size_t actual;
    bool noncoherent, stop_timeout, resume_error, ignore_resume_once;
    struct usb_qmu *queue;
};
static uint32_t qcsr(const struct usb_qmu *q) { return (q->transmit ? 0x510u : 0x610u) + (uint32_t)(q->endpoint - 1) * 16; }
static uint32_t qhi(const struct usb_qmu *q) { return (q->transmit ? 0x484u : 0x4c4u) + (uint32_t)(q->endpoint - 1) * 4; }
static int read32(void *ctx, uint32_t offset, uint32_t *out) {
    struct model *m = ctx; assert(offset < sizeof(m->regs) && !(offset & 3)); *out = m->regs[offset / 4]; return USB_OK;
}
static int write32(void *ctx, uint32_t offset, uint32_t value) {
    struct model *m = ctx; assert(offset < sizeof(m->regs) && !(offset & 3)); ++m->mmio_writes;
    if (offset == qcsr(m->queue)) {
        if (value == 1) {
            m->regs[offset / 4] = 1u << 15;
            m->regs[(offset + 8) / 4] = m->regs[(offset + 4) / 4];
        } else if (value == 2) {
            ++m->resumes;
            if (m->resume_error) return USB_IO;
            if (m->ignore_resume_once) { m->ignore_resume_once = false; return USB_OK; }
            m->regs[offset / 4] = 1u << 15;
        } else if (value == 4) {
            if (!m->stop_timeout) m->regs[offset / 4] = 0;
        } else assert(0);
    } else {
        m->regs[offset / 4] = value;
        uint32_t ep = (m->queue->transmit ? 0x110u : 0x210u) + (uint32_t)(m->queue->endpoint - 1) * 16;
        if (offset == ep && (value & (1u << 20))) {
            ++m->flushes;
            m->regs[offset / 4] &= ~(1u << 20); /* FIFO flush command self-clears. */
        }
    }
    return USB_OK;
}
static void barrier(void *ctx) { (void)ctx; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static void delay(void *ctx, uint32_t us) { ((struct model *)ctx)->delay += us; }
static int allocate(void *ctx, size_t size, size_t alignment, uint64_t limit, struct usb_dma_buffer *out) {
    struct model *m = ctx; assert(limit == UINT32_MAX && alignment >= 64); ++m->allocations;
    if (posix_memalign(&out->cpu, alignment, size)) return USB_NO_MEMORY;
    out->address = m->next_dma; m->next_dma += (size + 4095) & ~(uint64_t)4095;
    out->size = size; out->coherent = !m->noncoherent; return USB_OK;
}
static void release(void *ctx, struct usb_dma_buffer *buffer) { ++((struct model *)ctx)->releases; free(buffer->cpu); }
static int sync_memory(void *ctx, struct usb_dma_buffer *buffer, size_t at, size_t size, enum usb_dma_direction direction, bool device) {
    struct model *m = ctx; (void)direction; (void)device; assert(at + size <= buffer->size); ++m->syncs;
    return m->fail_sync_at > 0 && m->syncs == (unsigned)m->fail_sync_at ? USB_IO : USB_OK;
}
static void completion(void *ctx, void *cookie, int result, size_t actual) {
    struct model *m = ctx; assert(cookie != NULL); ++m->completed; m->result = result; m->actual = actual;
}
static void initialize(struct model *m, struct usb_qmu *q, bool transmit, bool enhanced) {
    memset(m, 0, sizeof(*m)); m->next_dma = 0x81000000; m->queue = q;
    struct usb_mmio mac = {m, sizeof(m->regs), read32, write32, barrier, delay};
    struct usb_dma_ops dma = {m, true, allocate, release, sync_memory};
    assert(usb_qmu_init(q, &mac, &dma, 2, transmit, enhanced) == USB_OK);
}
/* Independently act as the device: consume one descriptor, write RX count and
 * advance the current pointer. No production queue function performs this work. */
static void finish(struct model *m, size_t actual) {
    struct usb_qmu *q = m->queue;
    uint32_t *d = (uint32_t *)((uint8_t *)q->descriptors.cpu + (size_t)q->consumer * 16);
    assert(d[0] & 1);
    if (!q->transmit) d[3] = (uint32_t)actual;
    d[0] &= ~1u;
    m->regs[(qcsr(q) + 8) / 4] = d[1];
}
static void test_formats(void) {
    for (unsigned transmit = 0; transmit < 2; ++transmit) for (unsigned enhanced = 0; enhanced < 2; ++enhanced) {
        struct model m; struct usb_qmu q; initialize(&m, &q, transmit != 0, enhanced != 0);
        assert(!m.mmio_writes && q.descriptors.size == 144 * 16);
        struct usb_dma_buffer data;
        assert(usb_dma_alloc32(&q.dma, 256, 64, &data) == USB_OK);
        assert(usb_qmu_submit(&q, &data, 64, 128, transmit != 0, &data) == USB_OK);
        uint32_t *d = q.descriptors.cpu;
        assert(d[1] == q.descriptors.address + 16 && d[2] == data.address + 64);
        if (!transmit) assert(d[0] == ((128u << (enhanced ? 12 : 16)) | 0x81u) && !d[3]);
        else assert(d[0] == (enhanced ? 0xc1u : 0x81u) && d[3] == (128u | (enhanced ? 0u : 1u << 29)));
        assert(usb_qmu_reap(&q, completion, &m) == USB_AGAIN);
        assert(usb_qmu_submit(&q, &data, 64, 64, false, &data) == USB_BUSY);
        m.regs[qhi(&q) / 4] = 0xau; /* inherited upper start address must be cleared */
        assert(usb_qmu_start(&q) == USB_OK && !(m.regs[qhi(&q) / 4] & 15));
        assert(usb_qmu_reap(&q, completion, &m) == USB_OK && !m.completed);
        finish(&m, transmit ? 128 : 37);
        assert(usb_qmu_reap(&q, completion, &m) == USB_OK && m.completed == 1 && m.actual == (transmit ? 128u : 37u));
        assert(!q.pending && usb_qmu_destroy(&q) == USB_BUSY);
        assert(usb_qmu_stop(&q) == USB_OK && !q.running);
        assert(!transmit || m.flushes == 2);
        struct usb_dma_ops dma = q.dma;
        assert(usb_qmu_destroy(&q) == USB_OK);
        usb_dma_free(&dma, &data);
        assert(m.releases == 2);
    }
}
static void test_ring_and_timeout(void) {
    struct model m; struct usb_qmu q; initialize(&m, &q, true, true);
    struct usb_dma_buffer data; assert(usb_dma_alloc32(&q.dma, 144 * 64, 64, &data) == USB_OK);
    struct usb_dma_ops dma = q.dma;
    for (size_t n = 0; n < 143; ++n) assert(usb_qmu_submit(&q, &data, n * 64, 64, false, &data) == USB_OK);
    assert(usb_qmu_submit(&q, &data, 143 * 64, 64, false, &data) == USB_BUSY);
    assert(usb_qmu_start(&q) == USB_OK);
    for (unsigned n = 0; n < 143; ++n) { finish(&m, 64); assert(usb_qmu_reap(&q, completion, &m) == USB_OK); }
    assert(!q.pending && m.completed == 143);
    for (unsigned n = 0; n < 400; ++n) {
        assert(usb_qmu_submit(&q, &data, 0, 64, false, &data) == USB_OK);
        finish(&m, 64); assert(usb_qmu_reap(&q, completion, &m) == USB_OK);
    }
    assert(m.completed == 543 && q.producer == (143 + 400) % 144);
    m.regs[qcsr(&q) / 4] = 0; m.ignore_resume_once = true;
    unsigned resumes = m.resumes;
    assert(usb_qmu_submit(&q, &data, 0, 64, false, &data) == USB_OK);
    assert(m.resumes == resumes + 2 && (m.regs[qcsr(&q) / 4] & (1u << 15)));
    m.stop_timeout = true;
    assert(usb_qmu_cancel(&q, USB_DISCONNECTED, completion, &m) == USB_TIMEOUT);
    assert(q.running && q.pending == 1 && m.completed == 543 && !m.releases && m.delay == 1000);
    assert(usb_qmu_destroy(&q) == USB_BUSY);
    m.stop_timeout = false;
    assert(usb_qmu_cancel(&q, USB_DISCONNECTED, completion, &m) == USB_OK);
    assert(!q.running && !q.pending && m.completed == 544 && m.result == USB_DISCONNECTED);
    assert(usb_qmu_submit(&q, &data, 0, 64, false, &data) == USB_IO);
    assert(usb_qmu_destroy(&q) == USB_OK);
    usb_dma_free(&dma, &data); assert(m.releases == 2);
}
static void test_corruption_and_mapping(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        struct model m; struct usb_qmu q; initialize(&m, &q, false, true);
        struct usb_dma_ops dma = q.dma; struct usb_dma_buffer data;
        assert(usb_dma_alloc32(&dma, 256, 64, &data) == USB_OK);
        assert(usb_qmu_submit(&q, &data, SIZE_MAX, 64, false, &data) == USB_RANGE);
        assert(usb_qmu_submit(&q, &data, 0, 64, true, &data) == USB_RANGE);
        assert(usb_qmu_submit(&q, &q.descriptors, 0, 64, false, &data) == USB_RANGE);
        assert(usb_qmu_submit(&q, &data, 0, 64, false, &data) == USB_OK && usb_qmu_start(&q) == USB_OK);
        finish(&m, 32);
        if (mode == 0) ++m.regs[(qcsr(&q) + 8) / 4]; /* misaligned hardware current pointer */
        else if (mode == 1) ((uint32_t *)q.descriptors.cpu)[2] += 64; /* corrupted payload address */
        else if (mode == 2) ((uint32_t *)q.descriptors.cpu)[3] = 65; /* impossible received count */
        else m.regs[qhi(&q) / 4] |= 1u << 8; /* unexpected 36-bit current address */
        int result = usb_qmu_reap(&q, completion, &m);
        assert(result == (mode == 0 || mode == 3 ? USB_RANGE : USB_PHASE_ERROR));
        assert(q.faulted && !m.completed && q.pending == 1);
        assert(usb_qmu_cancel(&q, USB_IO, completion, &m) == USB_OK && m.completed == 1);
        assert(usb_qmu_destroy(&q) == USB_OK); usb_dma_free(&dma, &data);
    }
    struct model m; struct usb_qmu q; initialize(&m, &q, true, false);
    struct usb_dma_ops dma = q.dma; struct usb_dma_buffer data;
    assert(usb_dma_alloc32(&dma, 65536, 64, &data) == USB_OK);
    assert(usb_qmu_submit(&q, &data, 0, 65533, false, &data) == USB_RANGE);
    m.fail_sync_at = (int)m.syncs + 1;
    assert(usb_qmu_submit(&q, &data, 0, 64, false, &data) == USB_IO && !q.pending && !m.mmio_writes);
    m.fail_sync_at = 0;
    assert(usb_qmu_submit(&q, &data, 0, 64, false, &data) == USB_OK && usb_qmu_start(&q) == USB_OK);
    m.resume_error = true;
    assert(usb_qmu_submit(&q, &data, 64, 64, false, &data) == USB_OK && q.pending == 2 && q.faulted);
    assert(usb_qmu_cancel(&q, USB_IO, completion, &m) == USB_OK && m.completed == 2);
    assert(usb_qmu_destroy(&q) == USB_OK); usb_dma_free(&dma, &data);
}
static void test_descriptor_memory_policy(void) {
    struct model m = {.next_dma = 0x81000000, .noncoherent = true}; struct usb_qmu q;
    m.queue = &q;
    struct usb_mmio mac = {&m, sizeof(m.regs), read32, write32, barrier, delay};
    struct usb_dma_ops dma = {&m, false, allocate, release, sync_memory};
    assert(usb_qmu_init(&q, &mac, &dma, 1, true, true) == USB_UNSUPPORTED && !m.allocations);
    dma.qualified = true;
    assert(usb_qmu_init(&q, &mac, &dma, 1, true, true) == USB_UNSUPPORTED);
    assert(m.allocations == 1 && m.releases == 1 && !m.mmio_writes && !q.descriptors.cpu);
}
int main(void) {
    test_formats(); test_ring_and_timeout(); test_corruption_and_mapping(); test_descriptor_memory_policy();
    puts("PASS: MTU3 QMU production ring/MMIO/DMA tests against register model; hardware_qualified=false");
    return 0;
}
