/* Production xHCI controller lifecycle against a fault-injected register/DMA model. */
#define _POSIX_C_SOURCE 200112L
#include "UsbXhci.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP 0x40u
#define RT 0x500u
#define PORTSC (OP + 0x400u)
#define CMD_RUN 1u
#define CMD_RESET 2u
#define STS_HALTED 1u
#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_OCA (1u << 3)
#define PORTSC_PR (1u << 4)
#define PORTSC_PP (1u << 9)
#define PORTSC_SPEED_SHIFT 10u
#define PORTSC_CSC (1u << 17)
#define PORTSC_PEC (1u << 18)
#define PORTSC_OCC (1u << 20)
#define PORTSC_PRC (1u << 21)
#define PORTSC_CHANGE_MASK (0x7fu << 17)
#define PORTSC_CONTROL_MASK ((0xfu << 5) | PORTSC_PP | (3u << 14) | (7u << 25))

struct model {
    uint32_t regs[0x1000 / 4];
    uint64_t next_dma;
    unsigned accesses, fail_access, allocations, allocation_attempts, fail_allocate, releases, syncs, fail_sync;
    unsigned resets, starts, guard_calls, fail_guard, noncoherent_at, misalign_at, cnr_waits;
    unsigned reset_delay, cnr_delay, halt_delay, reset_remaining, cnr_remaining, halt_remaining;
    unsigned port_reset_delay, port_reset_remaining, port_writes;
    uint32_t crcr_reserved, erstba_reserved;
    struct usb_xhci_controller *controller;
    struct usb_xhci_device_slot *slot;
    uint32_t event_index;
    uint8_t event_cycle, next_slot, next_usb_address;
    uint8_t port_reset_speed;
    uint8_t device_descriptor[8];
    bool applied_error, guard_denied, guard_blocked, stuck_halt, stuck_reset, pending_on_reset;
    bool port_reset_overcurrent, port_reset_no_event, port_reset_unrelated_event;
    bool allow_live_release;
    bool auto_commands, auto_control;
};

static void put_le32(uint8_t *p, uint32_t value);
static void event_at(struct usb_xhci_controller *c, uint32_t index, uint8_t cycle,
                     uint32_t type, uint64_t parameter, uint8_t code, uint32_t fields);
static void queue_model_event(struct model *m, uint32_t type, uint64_t parameter,
                              uint8_t code, uint32_t fields);

static void init_model(struct model *m, unsigned scratchpads)
{
    memset(m, 0, sizeof(*m));
    m->next_dma = 0x81000000u;
    m->event_cycle = 1u; m->next_slot = 1u; m->next_usb_address = 1u;
    m->device_descriptor[0] = 18u; m->device_descriptor[1] = 1u;
    m->device_descriptor[2] = 0u; m->device_descriptor[3] = 2u;
    m->device_descriptor[7] = 8u;
    m->regs[0] = 0x01000040u;
    m->regs[1] = 8u | (1u << 8) | (4u << 24);
    m->regs[2] = ((scratchpads & 31u) << 27) | ((scratchpads >> 5) << 21);
    m->regs[4] = (0x800u / 4u << 16) | 5u;
    m->regs[5] = 0x600u; m->regs[6] = RT;
    m->regs[(OP + 4u) / 4u] = STS_HALTED;
    m->regs[(OP + 8u) / 4u] = 1u;
    m->regs[0x800u / 4u] = 2u | (2u << 24);
    m->regs[0x804u / 4u] = 0x20425355u;
    m->regs[0x808u / 4u] = 1u | (4u << 8);
    m->regs[PORTSC / 4u] = PORTSC_CCS | PORTSC_PP;
    m->port_reset_speed = USB_XHCI_SPEED_HIGH;
}
static int guard(void *context)
{
    struct model *m = context; ++m->guard_calls;
    m->guard_blocked = m->guard_denied || m->guard_calls == m->fail_guard;
    return m->guard_blocked ? USB_BUSY : USB_OK;
}
static uint32_t get_le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    struct model *m = context; assert(!m->guard_blocked && offset < sizeof(m->regs) && !(offset & 3u));
    if (++m->accesses == m->fail_access) return USB_IO;
    if (offset == OP + 4u && (m->regs[offset / 4u] & 0x800u) &&
        !(m->regs[OP / 4u] & CMD_RESET)) ++m->cnr_waits;
    *value = m->regs[offset / 4u]; return USB_OK;
}
static void apply_write(struct model *m, uint32_t offset, uint32_t value)
{
    if (offset == OP) {
        if (value & CMD_RESET) {
            ++m->resets;
            if (m->stuck_reset) { m->regs[offset / 4u] = value; return; }
            memset(&m->regs[(OP + 0x10u) / 4u], 0, 0x30u);
            memset(&m->regs[(RT + 0x20u) / 4u], 0, 0x20u);
            m->regs[(OP + 0x18u) / 4u] = m->crcr_reserved;
            m->regs[(RT + 0x30u) / 4u] = m->erstba_reserved;
            m->regs[(RT + 0x20u) / 4u] = m->pending_on_reset ? 1u : 0u;
            m->reset_remaining = m->reset_delay; m->cnr_remaining = m->cnr_delay;
            m->regs[OP / 4u] = m->reset_remaining ? CMD_RESET : 0;
            m->regs[(OP + 4u) / 4u] = STS_HALTED |
                (m->reset_remaining || m->cnr_remaining ? 0x800u : 0u);
        } else {
            m->regs[offset / 4u] = value;
            if (value & CMD_RUN) {
                ++m->starts;
                m->regs[(OP + 4u) / 4u] &= ~STS_HALTED;
            } else if (!m->stuck_halt) {
                m->halt_remaining = m->halt_delay;
                if (!m->halt_remaining) m->regs[(OP + 4u) / 4u] |= STS_HALTED;
            }
        }
    } else if (offset == RT + 0x20u) {
        uint32_t pending = (m->regs[offset / 4u] & 1u) & ~(value & 1u);
        m->regs[offset / 4u] = (value & ~1u) | pending;
    } else if (offset == PORTSC) {
        ++m->port_writes;
        uint32_t port = m->regs[offset / 4u];
        port &= ~(value & PORTSC_CHANGE_MASK); /* RW1CS status changes. */
        port = (port & ~PORTSC_CONTROL_MASK) | (value & PORTSC_CONTROL_MASK);
        if (value & PORTSC_PED) port &= ~PORTSC_PED; /* RW1CS: software may disable only. */
        if (value & PORTSC_PR) {
            port |= PORTSC_PR;
            port &= ~PORTSC_PED;
            m->port_reset_remaining = m->port_reset_delay ? m->port_reset_delay : 1u;
        }
        m->regs[offset / 4u] = port;
    } else {
        m->regs[offset / 4u] = value;
        if (offset == 0x600u && m->auto_commands && m->controller &&
            m->controller->command_pending) {
            uint64_t address = m->controller->pending_command;
            uint32_t index = (uint32_t)((address - m->controller->commands.trbs.address) /
                                        USB_XHCI_TRB_BYTES);
            uint8_t *command = (uint8_t *)m->controller->commands.trbs.cpu +
                               (size_t)index * USB_XHCI_TRB_BYTES;
            uint32_t control = get_le32(command + 12u);
            uint32_t type = (control >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK;
            uint8_t slot_id = (uint8_t)(control >> 24);
            if (type == USB_XHCI_TRB_ENABLE_SLOT) slot_id = m->next_slot++;
            if (type == USB_XHCI_TRB_ADDRESS_DEVICE && m->slot &&
                m->slot->slot_id == slot_id && m->slot->output_context.cpu) {
                uint8_t *out = m->slot->output_context.cpu;
                uint32_t state = control & (1u << 9) ? 1u : 2u;
                uint8_t assigned = state == 1u ? 0u : m->next_usb_address++;
                put_le32(out + 12u, (uint32_t)assigned | (state << 27));
            }
            if (type == USB_XHCI_TRB_CONFIGURE_ENDPOINT && m->slot &&
                m->slot->slot_id == slot_id && m->slot->output_context.cpu) {
                uint8_t *in_slot = (uint8_t *)m->slot->input_context.cpu +
                                   m->controller->caps.context_bytes;
                uint32_t state = get_le32((uint8_t *)m->slot->output_context.cpu + 12u);
                memcpy(m->slot->output_context.cpu, in_slot, m->controller->caps.context_bytes);
                put_le32((uint8_t *)m->slot->output_context.cpu + 12u, state);
            }
            event_at(m->controller, m->event_index, m->event_cycle,
                     USB_XHCI_TRB_COMMAND_EVENT, address, USB_XHCI_CC_SUCCESS,
                     (uint32_t)slot_id << 24);
            if (++m->event_index == m->controller->events.entries) {
                m->event_index = 0; m->event_cycle ^= 1u;
            }
        } else if (offset >= 0x604u && m->auto_control && m->slot &&
                   m->slot->control.pending && offset == 0x600u + (uint32_t)m->slot->slot_id * 4u) {
            if (m->slot->control.input && m->slot->control.data.cpu) {
                size_t length = m->slot->control.requested < sizeof(m->device_descriptor) ?
                                m->slot->control.requested : sizeof(m->device_descriptor);
                memcpy(m->slot->control.data.cpu, m->device_descriptor, length);
            }
            event_at(m->controller, m->event_index, m->event_cycle,
                     USB_XHCI_TRB_TRANSFER_EVENT, m->slot->control.status_trb,
                     USB_XHCI_CC_SUCCESS,
                     ((uint32_t)m->slot->slot_id << 24) | (1u << 16));
            if (++m->event_index == m->controller->events.entries) {
                m->event_index = 0; m->event_cycle ^= 1u;
            }
        }
    }
}
static int write32(void *context, uint32_t offset, uint32_t value)
{
    struct model *m = context; assert(!m->guard_blocked && offset < sizeof(m->regs) && !(offset & 3u));
    bool fail = ++m->accesses == m->fail_access;
    if (!fail || m->applied_error) apply_write(m, offset, value);
    return fail ? USB_IO : USB_OK;
}
static void barrier(void *context) { (void)context; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static void delay(void *context, uint32_t us)
{
    struct model *m = context; assert(us);
    if (m->reset_remaining && !--m->reset_remaining) m->regs[OP / 4u] &= ~CMD_RESET;
    if (!m->reset_remaining && m->cnr_remaining && !--m->cnr_remaining)
        m->regs[(OP + 4u) / 4u] &= ~0x800u;
    if (m->halt_remaining && !--m->halt_remaining)
        m->regs[(OP + 4u) / 4u] |= STS_HALTED;
    if (m->port_reset_remaining && !--m->port_reset_remaining) {
        uint32_t port = m->regs[PORTSC / 4u];
        port &= ~PORTSC_PR;
        if (m->port_reset_overcurrent) {
            port |= PORTSC_OCA | PORTSC_OCC;
            port &= ~PORTSC_PED;
        } else {
            port |= PORTSC_PRC | PORTSC_PED;
            port &= ~(0xfu << PORTSC_SPEED_SHIFT);
            port |= (uint32_t)m->port_reset_speed << PORTSC_SPEED_SHIFT;
        }
        m->regs[PORTSC / 4u] = port;
        if (!m->port_reset_no_event) {
            if (m->port_reset_unrelated_event)
                queue_model_event(m, USB_XHCI_TRB_TRANSFER_EVENT, 0x4560u,
                                  USB_XHCI_CC_SUCCESS, (2u << 24) | (1u << 16));
            if (m->port_reset_unrelated_event)
                queue_model_event(m, USB_XHCI_TRB_PORT_STATUS_EVENT, 2u << 24,
                                  USB_XHCI_CC_SUCCESS, 0);
            queue_model_event(m, USB_XHCI_TRB_PORT_STATUS_EVENT, 1u << 24,
                              USB_XHCI_CC_SUCCESS, 0);
        }
    }
}
static int allocate(void *context, size_t size, size_t alignment, uint64_t limit,
                    struct usb_dma_buffer *buffer)
{
    struct model *m = context; assert(limit == UINT32_MAX);
    if (++m->allocation_attempts == m->fail_allocate) return USB_NO_MEMORY;
    if (posix_memalign(&buffer->cpu, alignment, size)) return USB_NO_MEMORY;
    uint64_t mask = (uint64_t)alignment - 1u;
    m->next_dma = (m->next_dma + mask) & ~mask;
    buffer->address = m->next_dma + (m->allocation_attempts == m->misalign_at ? 64u : 0u);
    buffer->size = size; buffer->coherent = m->allocation_attempts != m->noncoherent_at;
    m->next_dma += (uint64_t)size;
    ++m->allocations; return USB_OK;
}
static void release(void *context, struct usb_dma_buffer *buffer)
{
    struct model *m = context;
    if (m->allow_live_release) { ++m->releases; free(buffer->cpu); return; }
    assert(!(m->regs[OP / 4u] & (CMD_RUN | CMD_RESET)));
    assert(m->regs[(OP + 4u) / 4u] & STS_HALTED);
    assert(!(m->regs[(OP + 0x18u) / 4u] & ~0x3fu) && !m->regs[(OP + 0x30u) / 4u]);
    assert(!(m->regs[(RT + 0x30u) / 4u] & ~0x3fu) && !m->regs[(RT + 0x38u) / 4u]);
    ++m->releases; free(buffer->cpu);
}
static int sync_dma(void *context, struct usb_dma_buffer *buffer, size_t offset, size_t length,
                    enum usb_dma_direction direction, bool for_device)
{
    struct model *m = context;
    assert(buffer && offset <= buffer->size && length <= buffer->size - offset);
    assert(direction <= USB_DMA_BIDIRECTIONAL); (void)for_device;
    return ++m->syncs == m->fail_sync ? USB_IO : USB_OK;
}
static struct usb_mmio mmio(struct model *m)
{ return (struct usb_mmio){m, sizeof(m->regs), read32, write32, barrier, delay}; }
static struct usb_dma_ops dma(struct model *m)
{ return (struct usb_dma_ops){m, true, allocate, release, sync_dma}; }
static struct usb_xhci_controller *new_controller(void)
{
    struct usb_xhci_controller *c = calloc(1, sizeof(*c));
    assert(c); return c;
}
static int start(struct model *m, struct usb_xhci_controller *c)
{
    struct usb_mmio io = mmio(m); struct usb_dma_ops d = dma(m);
    m->controller = c;
    return usb_xhci_controller_start(c, &io, &d, guard, m, 4, 16, 16);
}
static uint64_t get_le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8u * i);
    return value;
}
static void put_le32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
static void event_at(struct usb_xhci_controller *c, uint32_t index, uint8_t cycle,
                     uint32_t type, uint64_t parameter, uint8_t code, uint32_t fields)
{
    uint8_t *p = (uint8_t *)c->events.trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES;
    put_le32(p, (uint32_t)parameter); put_le32(p + 4, (uint32_t)(parameter >> 32));
    put_le32(p + 8, (uint32_t)code << 24);
    put_le32(p + 12, (type << USB_XHCI_TRB_TYPE_SHIFT) | fields | cycle);
}

static void queue_model_event(struct model *m, uint32_t type, uint64_t parameter,
                              uint8_t code, uint32_t fields)
{
    assert(m->controller);
    event_at(m->controller, m->event_index, m->event_cycle, type, parameter, code, fields);
    if (++m->event_index == m->controller->events.entries) {
        m->event_index = 0;
        m->event_cycle ^= 1u;
    }
}

static void test_commands_and_events(void)
{
    struct model m; init_model(&m, 0); struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_trb command = {{0}};
    put_le32(command.bytes + 12, USB_XHCI_TRB_NOOP_COMMAND << USB_XHCI_TRB_TYPE_SHIFT);
    assert(usb_xhci_controller_submit_command(c, &command) == USB_OK);
    uint64_t pending = c->pending_command;
    assert(c->command_pending && pending == c->commands.trbs.address && c->commands.outstanding == 1);
    assert(usb_xhci_controller_submit_command(c, &command) == USB_BUSY);
    struct usb_xhci_event event, unchanged; memset(&event, 0xa5, sizeof(event)); unchanged = event;
    assert(usb_xhci_controller_next_event(c, &event) == USB_AGAIN && !memcmp(&event, &unchanged, sizeof(event)));
    event_at(c, 0, 1, USB_XHCI_TRB_PORT_STATUS_EVENT, 3u << 24, USB_XHCI_CC_SUCCESS, 0);
    event_at(c, 1, 1, USB_XHCI_TRB_TRANSFER_EVENT, 0x1230u, USB_XHCI_CC_SUCCESS, (2u << 24) | (1u << 16));
    event_at(c, 2, 1, USB_XHCI_TRB_COMMAND_EVENT, pending, USB_XHCI_CC_SUCCESS, 0);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK && event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE);
    assert(event.data.port_status.port_id == 3 && c->command_pending);
    assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)c->events.trbs.address);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK && event.kind == USB_XHCI_EVENT_TRANSFER);
    assert(c->command_pending && c->commands.outstanding == 1);
    assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)(c->events.trbs.address + 16u) + 8u);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK && event.kind == USB_XHCI_EVENT_COMMAND_COMPLETION);
    assert(event.completion_code == USB_XHCI_CC_SUCCESS && !c->command_pending && !c->commands.outstanding);
    /* Command failure still retires the completed command and preserves status. */
    assert(usb_xhci_controller_submit_command(c, &command) == USB_OK);
    event_at(c, 3, 1, USB_XHCI_TRB_COMMAND_EVENT, c->pending_command, 5u, 0);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK && event.completion_code == 5u);
    assert(!c->command_pending && !c->commands.outstanding);
    /* Unknown events remain visible. */
    event_at(c, 4, 1, 63u, 0x1234u, 9u, 0);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK && event.kind == USB_XHCI_EVENT_RAW && event.type == 63u);
    assert(usb_xhci_controller_next_event(c, &event) == USB_AGAIN);
    assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)(c->events.trbs.address + 5u * 16u) + 8u);
    for (uint32_t i = 5; i < 16; ++i)
        event_at(c, i, 1, USB_XHCI_TRB_PORT_STATUS_EVENT, (uint64_t)(i + 1u) << 24,
                 USB_XHCI_CC_SUCCESS, 0);
    event_at(c, 0, 0, USB_XHCI_TRB_TRANSFER_EVENT, 0x4560u, USB_XHCI_CC_SHORT_PACKET, (2u << 24) | (1u << 16));
    for (uint32_t i = 5; i < 16; ++i) {
        assert(usb_xhci_controller_next_event(c, &event) == USB_OK && event.data.port_status.port_id == i + 1u);
        if (i > 5) assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)(c->events.trbs.address + i * 16u) + 8u);
    }
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_TRANSFER && event.completion_code == USB_XHCI_CC_SHORT_PACKET);
    assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)c->events.trbs.address + 8u);
    assert(usb_xhci_controller_next_event(c, &event) == USB_AGAIN);
    assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)(c->events.trbs.address + 16u) + 8u);
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    for (unsigned failure = 0; failure < 7; ++failure) {
        init_model(&m, 0); c = new_controller(); assert(start(&m, c) == USB_OK);
        if (failure == 0) {
            m.fail_access = m.accesses + 2u; m.applied_error = true; /* DB0 write applied, error */
            assert(usb_xhci_controller_submit_command(c, &command) == USB_IO);
        } else {
            assert(usb_xhci_controller_submit_command(c, &command) == USB_OK);
            if (failure == 3 || failure == 4) {
                event_at(c, 0, 1, USB_XHCI_TRB_PORT_STATUS_EVENT, 1u << 24, USB_XHCI_CC_SUCCESS, 0);
                assert(usb_xhci_controller_next_event(c, &event) == USB_OK);
            }
            uint64_t ptr = c->pending_command;
            event_at(c, failure == 3 || failure == 4 ? 1u : 0u, 1, USB_XHCI_TRB_COMMAND_EVENT,
                     failure == 1 ? ptr + 16u : failure == 5 ? ptr | 1u : ptr,
                     failure == 2 ? USB_XHCI_CC_COMMAND_RING_STOPPED :
                     failure == 6 ? USB_XHCI_CC_COMMAND_ABORTED : USB_XHCI_CC_SUCCESS, 0);
            if (failure == 3 || failure == 4) { m.fail_access = m.accesses + failure - 1u; m.applied_error = true; }
            memset(&event, 0xa5, sizeof(event)); unchanged = event;
            int result = usb_xhci_controller_next_event(c, &event);
            assert(result == (failure == 3 || failure == 4 ? USB_IO : USB_PHASE_ERROR));
            assert(!memcmp(&event, &unchanged, sizeof(event)));
        }
        assert(c->state == USB_XHCI_CONTROLLER_FAULTED && c->command_pending && m.releases == 0);
        m.fail_access = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    puts("PASS: xHCI command doorbell, shared event delivery and retained faults");
}

static void test_root_port_reset(void)
{
    struct model m; init_model(&m, 0); m.port_reset_delay = 2;
    m.port_reset_unrelated_event = true;
    m.regs[PORTSC / 4u] |= PORTSC_CSC | PORTSC_PEC | PORTSC_PRC;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    enum usb_xhci_speed speed = 0;
    queue_model_event(&m, USB_XHCI_TRB_TRANSFER_EVENT, 0x1230u,
                      USB_XHCI_CC_SUCCESS, (2u << 24) | (1u << 16));
    assert(usb_xhci_root_port_reset(c, 1u, 100u, &speed) == USB_BUSY);
    assert(m.port_writes == 0u && c->pending_event_count == 1u);
    struct usb_xhci_event event;
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_TRANSFER && event.data.transfer.pointer_or_data == 0x1230u);
    assert(usb_xhci_root_port_reset(c, 1u, 100u, &speed) == USB_OK);
    assert(speed == USB_XHCI_SPEED_HIGH);
    uint32_t port = m.regs[PORTSC / 4u];
    assert((port & (PORTSC_CCS | PORTSC_PED | PORTSC_PP)) ==
           (PORTSC_CCS | PORTSC_PED | PORTSC_PP));
    assert(!(port & (PORTSC_PR | PORTSC_PRC)) &&
           ((port >> PORTSC_SPEED_SHIFT) & 0xfu) == USB_XHCI_SPEED_HIGH);
    assert((port & (PORTSC_CSC | PORTSC_PEC)) == (PORTSC_CSC | PORTSC_PEC));
    assert(m.port_writes == 3u); /* Clear stale PRC, assert PR, acknowledge PRC. */
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_TRANSFER && event.data.transfer.pointer_or_data == 0x4560u);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE && event.data.port_status.port_id == 2u);
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE && event.data.port_status.port_id == 1u);
    assert(usb_xhci_controller_next_event(c, &event) == USB_AGAIN);

    unsigned before = m.port_writes;
    assert(usb_xhci_root_port_reset(c, 0u, 100u, &speed) == USB_RANGE);
    c->caps.usb2_ports[0] &= (uint8_t)~(1u << 1);
    assert(usb_xhci_root_port_reset(c, 2u, 100u, &speed) == USB_UNSUPPORTED);
    assert(m.port_writes == before);
    m.regs[PORTSC / 4u] &= ~PORTSC_PP;
    assert(usb_xhci_root_port_reset(c, 1u, 100u, &speed) == USB_BUSY);
    assert(m.port_writes == before);
    m.regs[PORTSC / 4u] |= PORTSC_PP;
    m.regs[PORTSC / 4u] &= ~PORTSC_CCS;
    assert(usb_xhci_root_port_reset(c, 1u, 100u, &speed) == USB_DISCONNECTED);
    assert(m.port_writes == before);
    m.regs[PORTSC / 4u] |= PORTSC_CCS;
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* Overcurrent aborts the reset, faults the controller and preserves its PSC. */
    init_model(&m, 0); m.port_reset_delay = 1; m.port_reset_overcurrent = true;
    c = new_controller(); assert(start(&m, c) == USB_OK);
    speed = USB_XHCI_SPEED_LOW;
    assert(usb_xhci_root_port_reset(c, 1u, 10u, &speed) == USB_IO);
    assert(speed == USB_XHCI_SPEED_LOW && c->state == USB_XHCI_CONTROLLER_FAULTED);
    assert((m.regs[PORTSC / 4u] & (PORTSC_OCA | PORTSC_OCC)) == (PORTSC_OCA | PORTSC_OCC));
    assert(!(m.regs[PORTSC / 4u] & PORTSC_PRC));
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE && event.data.port_status.port_id == 1u);
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* PORTSC alone cannot complete a reset without the required matching PSC. */
    init_model(&m, 0); m.port_reset_delay = 1; m.port_reset_no_event = true;
    c = new_controller(); assert(start(&m, c) == USB_OK);
    speed = USB_XHCI_SPEED_FULL;
    assert(usb_xhci_root_port_reset(c, 1u, 3u, &speed) == USB_TIMEOUT);
    assert(speed == USB_XHCI_SPEED_FULL && c->state == USB_XHCI_CONTROLLER_FAULTED);
    assert((m.regs[PORTSC / 4u] & (PORTSC_PRC | PORTSC_PED)) == (PORTSC_PRC | PORTSC_PED));
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* A timeout after PR publication leaves ownership fail-stopped. */
    init_model(&m, 0); m.port_reset_delay = 10; c = new_controller();
    assert(start(&m, c) == USB_OK);
    speed = USB_XHCI_SPEED_FULL;
    assert(usb_xhci_root_port_reset(c, 1u, 1u, &speed) == USB_TIMEOUT);
    assert(speed == USB_XHCI_SPEED_FULL && c->state == USB_XHCI_CONTROLLER_FAULTED);
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* An applied-but-error PR write also requires controller stop/reset. */
    init_model(&m, 0); c = new_controller(); assert(start(&m, c) == USB_OK);
    m.fail_access = m.accesses + 4u; m.applied_error = true;
    assert(usb_xhci_root_port_reset(c, 1u, 10u, &speed) == USB_IO);
    assert(c->state == USB_XHCI_CONTROLLER_FAULTED);
    m.fail_access = 0;
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);
    puts("PASS: xHCI USB2 root-port reset and retained failure model");
}

static void test_control_transfer(void)
{
    struct model m; init_model(&m, 0); struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_transfer_ring ring;
    assert(usb_xhci_transfer_ring_init(&ring, &c->dma, 8) == USB_OK);
    assert(usb_xhci_transfer_ring_mark_active(&ring) == USB_OK);
    struct usb_xhci_control_td td = {0};
    struct usb_setup setup = {0x80, 6, 0x100, 0, 18};
    assert(usb_xhci_control_submit(&td, c, &ring, 2, &setup, NULL, 18) == USB_OK);
    assert(td.pending && td.requested == 18 && !(td.data.address & 0xffffu));
    assert(m.regs[(0x600u + 2u * 4u) / 4u] == 1u);
    const uint8_t *trbs = ring.trbs.cpu;
    assert(get_le64(trbs) == UINT64_C(0x0012000001000680));
    assert(get_le64(trbs + 16u) == td.data.address);
    assert((trbs[28] & 0x04u) != 0); /* Data ISP for short IN. */
    for (size_t i = 0; i < 18; ++i) ((uint8_t *)td.data.cpu)[i] = (uint8_t)(i + 1u);
    event_at(c, 0, 1, USB_XHCI_TRB_TRANSFER_EVENT, td.data_trb, USB_XHCI_CC_SHORT_PACKET,
             (2u << 24) | (1u << 16));
    put_le32((uint8_t *)c->events.trbs.cpu + 8u, (USB_XHCI_CC_SHORT_PACKET << 24) | 10u);
    struct usb_xhci_event event;
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK);
    assert(usb_xhci_control_event(&td, &event) == USB_AGAIN && td.pending && td.short_seen);
    assert(ring.outstanding == 3 && td.actual == 8 && !m.releases);
    event_at(c, 1, 1, USB_XHCI_TRB_TRANSFER_EVENT, td.status_trb, USB_XHCI_CC_SUCCESS,
             (2u << 24) | (1u << 16));
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK);
    assert(usb_xhci_control_event(&td, &event) == USB_OK && td.complete && !ring.outstanding);
    uint8_t output[18]; memset(output, 0xa5, sizeof(output)); size_t actual = 99;
    assert(usb_xhci_control_finish(&td, output, 7, &actual) == USB_RANGE && actual == 99);
    m.allow_live_release = true;
    assert(usb_xhci_control_finish(&td, output, sizeof(output), &actual) == USB_OK && actual == 8);
    m.allow_live_release = false;
    for (size_t i = 0; i < 8; ++i) assert(output[i] == i + 1u);
    assert(output[8] == 0xa5);

    /* A no-data OUT request has an IN status stage and no DMA data buffer. */
    setup = (struct usb_setup){0, 9, 1, 0, 0};
    assert(usb_xhci_control_submit(&td, c, &ring, 2, &setup, NULL, 0) == USB_OK);
    uint32_t status_control = (uint32_t)trbs[4u * 16u + 12u] |
        ((uint32_t)trbs[4u * 16u + 13u] << 8) | ((uint32_t)trbs[4u * 16u + 14u] << 16);
    assert(status_control & (1u << 16));
    event_at(c, 2, 1, USB_XHCI_TRB_TRANSFER_EVENT, td.status_trb, USB_XHCI_CC_SUCCESS,
             (2u << 24) | (1u << 16));
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK);
    assert(usb_xhci_control_event(&td, &event) == USB_OK);
    assert(usb_xhci_control_finish(&td, NULL, 0, &actual) == USB_OK && actual == 0);

    /* OUT data and its IN status cross the Link TRB, with copied DMA data. */
    const uint8_t outgoing[] = {1, 2, 3, 4};
    setup = (struct usb_setup){0x21, 0x20, 0, 1, sizeof(outgoing)};
    assert(usb_xhci_control_submit(&td, c, &ring, 2, &setup, outgoing, sizeof(outgoing)) == USB_OK);
    assert(!memcmp(td.data.cpu, outgoing, sizeof(outgoing)) && td.status_trb == ring.trbs.address);
    assert(!(trbs[6u * 16u + 14u] & 1u) && (trbs[14u] & 1u));
    struct usb_xhci_event other_event = {0};
    other_event.kind = USB_XHCI_EVENT_PORT_STATUS_CHANGE;
    assert(usb_xhci_control_event(&td, &other_event) == USB_AGAIN && td.pending);
    event_at(c, 3, 1, USB_XHCI_TRB_TRANSFER_EVENT, td.status_trb, USB_XHCI_CC_SUCCESS,
             (2u << 24) | (1u << 16));
    assert(usb_xhci_controller_next_event(c, &event) == USB_OK);
    assert(usb_xhci_control_event(&td, &event) == USB_OK);
    m.allow_live_release = true;
    assert(usb_xhci_control_finish(&td, NULL, 0, &actual) == USB_OK && actual == sizeof(outgoing));
    m.allow_live_release = false;
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_transfer_ring_halted(&ring) == USB_OK);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_OK && m.allocations == m.releases);
    free(c);

    /* Doorbell errors and invalid/stalled events retain data until fresh stop. */
    for (unsigned failure = 0; failure < 4; ++failure) {
        init_model(&m, 0); c = new_controller(); assert(start(&m, c) == USB_OK);
        assert(usb_xhci_transfer_ring_init(&ring, &c->dma, 8) == USB_OK);
        assert(usb_xhci_transfer_ring_mark_active(&ring) == USB_OK);
        memset(&td, 0, sizeof(td)); setup = (struct usb_setup){0x80, 6, 0x100, 0, 18};
        if (failure == 0) { m.fail_access = m.accesses + 2u; m.applied_error = true; }
        int result = usb_xhci_control_submit(&td, c, &ring, 1, &setup, NULL, 18);
        if (!failure) assert(result == USB_IO);
        else if (failure < 3) {
            assert(result == USB_OK);
            event_at(c, 0, 1, USB_XHCI_TRB_TRANSFER_EVENT,
                     failure == 1 ? td.status_trb + 16u : td.data_trb,
                     failure == 1 ? USB_XHCI_CC_SUCCESS : USB_XHCI_CC_STALL_ERROR,
                     (1u << 24) | (1u << 16));
            assert(usb_xhci_controller_next_event(c, &event) == USB_OK);
            assert(usb_xhci_control_event(&td, &event) == (failure == 1 ? USB_PHASE_ERROR : USB_STALL));
        } else {
            assert(result == USB_OK && usb_xhci_controller_next_event(c, &event) == USB_AGAIN);
            m.stuck_halt = true;
            assert(usb_xhci_controller_stop(c) == USB_TIMEOUT && !m.releases && td.pending);
            m.stuck_halt = false;
        }
        assert(td.pending && (failure == 3 || td.faulted) &&
               c->state == USB_XHCI_CONTROLLER_FAULTED && !m.releases);
        assert(usb_xhci_control_release_stopped(&td) == USB_INVALID);
        m.fail_access = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK);
        assert(usb_xhci_transfer_ring_halted(&ring) == USB_OK);
        assert(usb_xhci_control_release_stopped(&td) == USB_OK);
        assert(usb_xhci_transfer_ring_destroy(&ring) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    /* The data and endpoint ring may have different DMA allocator contexts. */
    struct model other; init_model(&m, 0); init_model(&other, 0);
    other.next_dma = 0x85000000u;
    c = new_controller(); assert(start(&m, c) == USB_OK);
    struct usb_dma_ops ring_dma = dma(&other);
    assert(usb_xhci_transfer_ring_init(&ring, &ring_dma, 8) == USB_OK);
    assert(usb_xhci_transfer_ring_mark_active(&ring) == USB_OK);
    memset(&td, 0, sizeof(td)); setup = (struct usb_setup){0x80, 6, 0x100, 0, 18};
    m.fail_access = m.accesses + 2u; m.applied_error = true;
    assert(usb_xhci_control_submit(&td, c, &ring, 1, &setup, NULL, 18) == USB_IO);
    m.fail_access = 0;
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_transfer_ring_halted(&ring) == USB_OK);
    assert(usb_xhci_control_release_stopped(&td) == USB_OK);
    assert(usb_xhci_transfer_ring_destroy(&ring) == USB_OK);
    assert(m.allocations == m.releases && other.allocations == other.releases);
    free(c);
    puts("PASS: xHCI EP0 control short/status and retained fault model");
}

static void test_device_slots_and_routing(void)
{
    struct model m; init_model(&m, 0); m.auto_commands = true; m.auto_control = true;
    m.device_descriptor[7] = 64u;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_device_slot root = {0}, child = {0};
    m.slot = &root;
    struct usb_xhci_route route = {.root_port = 1};
    bool cleanup = false;
    assert(usb_xhci_device_prepare(&root, c, &route, USB_XHCI_SPEED_HIGH, &cleanup) == USB_OK);
    assert(cleanup && root.slot_id == 1 && root.state == USB_XHCI_SLOT_DEFAULT);
    assert(root.output_context.size == 2048u && root.input_context.size == 2112u);
    assert(get_le64((const uint8_t *)c->dcbaa.cpu + 8u) == root.output_context.address);
    uint8_t *input = root.input_context.cpu;
    uint32_t slot_dword0 = get_le32(input + 64u);
    uint32_t ep0_dword1 = get_le32(input + 128u + 4u);
    assert((slot_dword0 & (0xfu << 20)) == (USB_XHCI_SPEED_HIGH << 20));
    assert(slot_dword0 & (1u << 27));
    assert(get_le32(input + 68u) == (1u << 16));
    assert(ep0_dword1 == ((3u << 1) | (4u << 3) | (64u << 16)));

    uint8_t assigned = 0;
    assert(usb_xhci_device_address(&root, 64u, &assigned) == USB_OK);
    assert(assigned == 1 && root.address == assigned && root.state == USB_XHCI_SLOT_ADDRESSED);
    /* The command waiter must retain a port event that precedes command completion. */
    event_at(c, m.event_index, m.event_cycle, USB_XHCI_TRB_PORT_STATUS_EVENT,
             1u << 24, USB_XHCI_CC_SUCCESS, 0);
    if (++m.event_index == c->events.entries) { m.event_index = 0; m.event_cycle ^= 1u; }
    assert(usb_xhci_device_configure_hub(&root, 4u, 1332u, true) == USB_OK);
    assert(root.is_hub && root.hub_port_count == 4);
    assert(get_le32((uint8_t *)root.output_context.cpu) & (1u << 26));
    assert(get_le32((uint8_t *)root.output_context.cpu) & (1u << 25));
    assert(((get_le32((uint8_t *)root.output_context.cpu + 8u) >> 16) & 3u) == 1u);
    struct usb_xhci_event preserved;
    assert(usb_xhci_controller_next_event(c, &preserved) == USB_OK &&
           preserved.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE);

    m.slot = &child;
    struct usb_xhci_route child_route = {.root_port = 1, .depth = 1,
        .hub_slot = {1}, .hub_port = {3}, .hub_port_count = {4},
        .tt_hub_slot = 1, .tt_port = 3,
        .multi_tt = true};
    struct usb_xhci_device_slot rejected_route = {0};
    struct usb_xhci_route invalid_route = child_route;
    unsigned allocation_attempts = m.allocation_attempts;
    m.slot = &rejected_route; cleanup = false;
    invalid_route.tt_hub_slot = 2; /* In-range, but not an ancestor in this path. */
    assert(usb_xhci_device_prepare(&rejected_route, c, &invalid_route,
                                   USB_XHCI_SPEED_LOW, &cleanup) == USB_RANGE);
    assert(!cleanup && rejected_route.state == USB_XHCI_SLOT_EMPTY &&
           m.allocation_attempts == allocation_attempts && m.next_slot == 2u);
    invalid_route = child_route; invalid_route.tt_port = 4;
    assert(usb_xhci_device_prepare(&rejected_route, c, &invalid_route,
                                   USB_XHCI_SPEED_LOW, &cleanup) == USB_RANGE);
    assert(!cleanup && rejected_route.state == USB_XHCI_SLOT_EMPTY &&
           m.allocation_attempts == allocation_attempts && m.next_slot == 2u);
    m.slot = &child;
    cleanup = false;
    assert(usb_xhci_device_prepare(&child, c, &child_route, USB_XHCI_SPEED_LOW,
                                   &cleanup) == USB_OK);
    assert(cleanup && child.slot_id == 2 && child.state == USB_XHCI_SLOT_DEFAULT);
    input = child.input_context.cpu;
    slot_dword0 = get_le32(input + 64u);
    assert((slot_dword0 & 0xfffffu) == 3u);
    assert((slot_dword0 & (0xfu << 20)) == (USB_XHCI_SPEED_LOW << 20));
    assert(slot_dword0 & (1u << 25));
    assert(get_le32(input + 72u) == (1u | (3u << 8)));
    assert(get_le32(input + 68u) == (1u << 16));
    m.device_descriptor[7] = 8u;
    struct usb_setup setup = {0x80, 6, 0x100, 0, 8};
    uint8_t descriptor[8] = {0}; size_t actual = 99;
    m.allow_live_release = true; /* Status completion retires the data TD. */
    assert(usb_xhci_device_control(&child, 0, &setup, descriptor, sizeof(descriptor),
                                   &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == sizeof(descriptor) && descriptor[0] == 18u && descriptor[7] == 8u);
    assert(usb_xhci_device_address(&child, descriptor[7], &assigned) == USB_OK && assigned == 2u);
    m.allow_live_release = true; /* Disable Slot is the per-slot stop proof. */
    assert(usb_xhci_device_disable(&child, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(child.state == USB_XHCI_SLOT_EMPTY && !get_le64((const uint8_t *)c->dcbaa.cpu + 16u));
    m.slot = &root;
    m.allow_live_release = true;
    assert(usb_xhci_device_disable(&root, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(root.state == USB_XHCI_SLOT_EMPTY && !get_le64((const uint8_t *)c->dcbaa.cpu + 8u));
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* Slot/input/EP0 offsets must also honor 32-byte context controllers. */
    {
        init_model(&m, 0); m.regs[4] &= ~(1u << 2); m.auto_commands = true;
        c = new_controller(); assert(start(&m, c) == USB_OK);
        struct usb_xhci_device_slot small_context = {0}; m.slot = &small_context;
        cleanup = false;
        assert(usb_xhci_device_prepare(&small_context, c, &route, USB_XHCI_SPEED_HIGH,
                                       &cleanup) == USB_OK);
        assert(cleanup && c->caps.context_bytes == 32u);
        assert(small_context.output_context.size == 1024u &&
               small_context.input_context.size == 1056u);
        uint8_t *small_input = small_context.input_context.cpu;
        assert((get_le32(small_input + 32u) & (0xfu << 20)) ==
               (USB_XHCI_SPEED_HIGH << 20));
        assert(get_le32(small_input + 36u) == (1u << 16));
        assert(get_le32(small_input + 68u) == ((3u << 1) | (4u << 3) | (64u << 16)));
        assert(usb_xhci_device_address(&small_context, 64u, &assigned) == USB_OK &&
               assigned == 1u);
        m.allow_live_release = true;
        assert(usb_xhci_device_disable(&small_context, 100u) == USB_OK);
        m.allow_live_release = false;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }

    /* A nested full-speed hub keeps its translator on the upstream HS hub;
     * hubs with more than 14 ports encode the Route String nibble as 15. */
    {
        init_model(&m, 0); m.next_slot = 3u; m.auto_commands = true;
        c = new_controller(); assert(start(&m, c) == USB_OK);
        struct usb_xhci_device_slot nested_child = {0}; m.slot = &nested_child;
        struct usb_xhci_route nested_route = {.root_port = 1, .depth = 2,
            .hub_slot = {1, 2}, .hub_port = {3, 5}, .hub_port_count = {16, 8},
            .tt_hub_slot = 1,
            .tt_port = 3, .multi_tt = true};
        cleanup = false;
        assert(usb_xhci_device_prepare(&nested_child, c, &nested_route,
                                       USB_XHCI_SPEED_LOW, &cleanup) == USB_OK);
        assert(cleanup && nested_child.slot_id == 3u);
        input = nested_child.input_context.cpu;
        slot_dword0 = get_le32(input + 64u);
        assert((slot_dword0 & 0xfffffu) == 0x5fu);
        assert(slot_dword0 & (1u << 25));
        assert(get_le32(input + 72u) == (1u | (3u << 8)));
        m.allow_live_release = true;
        assert(usb_xhci_device_disable(&nested_child, 100u) == USB_OK);
        m.allow_live_release = false;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }

    /* TT context keeps the actual high port number while Route String uses 15. */
    {
        init_model(&m, 0); m.next_slot = 2u; m.auto_commands = true;
        c = new_controller(); assert(start(&m, c) == USB_OK);
        struct usb_xhci_device_slot wide_hub_child = {0}; m.slot = &wide_hub_child;
        struct usb_xhci_route wide_port_route = {.root_port = 1, .depth = 1,
            .hub_slot = {1}, .hub_port = {16}, .hub_port_count = {16},
            .tt_hub_slot = 1, .tt_port = 16};
        cleanup = false;
        unsigned before_route_allocations = m.allocation_attempts;
        wide_port_route.hub_port[0] = 17u; wide_port_route.tt_port = 17u;
        assert(usb_xhci_device_prepare(&wide_hub_child, c, &wide_port_route,
                                       USB_XHCI_SPEED_LOW, &cleanup) == USB_RANGE);
        assert(!cleanup && wide_hub_child.state == USB_XHCI_SLOT_EMPTY &&
               m.allocation_attempts == before_route_allocations && m.next_slot == 2u);
        wide_port_route.hub_port[0] = 16u; wide_port_route.tt_port = 16u;
        assert(usb_xhci_device_prepare(&wide_hub_child, c, &wide_port_route,
                                       USB_XHCI_SPEED_LOW, &cleanup) == USB_OK);
        input = wide_hub_child.input_context.cpu;
        slot_dword0 = get_le32(input + 64u);
        assert((slot_dword0 & 0xfffffu) == 15u);
        assert(get_le32(input + 72u) == (1u | (16u << 8)));
        m.allow_live_release = true;
        assert(usb_xhci_device_disable(&wide_hub_child, 100u) == USB_OK);
        m.allow_live_release = false;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }

    /* Each pre-publication allocation failure retains the enabled-slot cleanup. */
    for (unsigned fail_offset = 1u; fail_offset <= 3u; ++fail_offset) {
        init_model(&m, 0); m.auto_commands = true; c = new_controller();
        assert(start(&m, c) == USB_OK);
        struct usb_xhci_device_slot failed = {0}; m.slot = &failed;
        m.fail_allocate = m.allocation_attempts + fail_offset;
        cleanup = false;
        assert(usb_xhci_device_prepare(&failed, c, &route, USB_XHCI_SPEED_FULL,
                                       &cleanup) == USB_NO_MEMORY);
        assert(cleanup && failed.state == USB_XHCI_SLOT_ENABLED && failed.slot_id == 1u);
        m.fail_allocate = 0; m.allow_live_release = true;
        assert(usb_xhci_device_disable(&failed, 100u) == USB_OK);
        m.allow_live_release = false;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    puts("PASS: xHCI Enable Slot, BSR/address, routed hub TT context and retained cleanup model");
}
static void test_running_schedule(void)
{
    struct model m; init_model(&m, 3);
    m.pending_on_reset = true; m.reset_delay = 2; m.cnr_delay = 5; m.halt_delay = 2;
    m.crcr_reserved = 0x30u; m.erstba_reserved = 0x25u;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    assert(c->state == USB_XHCI_CONTROLLER_RUNNING && m.starts == 1 && m.resets == 1 && m.cnr_waits);
    assert(c->scratch_count == 3 && c->scratch_allocated == 3);
    assert(get_le64(c->dcbaa.cpu) == c->scratch_array.address);
    for (size_t i = 0; i < 3; ++i)
        assert(get_le64((const uint8_t *)c->scratch_array.cpu + 8u * i) == c->scratch_pages[i].address);
    assert(m.regs[(OP + 0x38u) / 4u] == 4);
    assert(m.regs[(OP + 0x30u) / 4u] == (uint32_t)c->dcbaa.address);
    assert(m.regs[(OP + 0x18u) / 4u] == ((uint32_t)c->commands.trbs.address | 0x31u));
    assert(m.regs[(RT + 0x28u) / 4u] == 1);
    assert(m.regs[(RT + 0x38u) / 4u] == (uint32_t)c->events.trbs.address);
    assert(m.regs[(RT + 0x30u) / 4u] == ((uint32_t)c->events.erst.address | 0x25u));
    assert(m.regs[(RT + 0x20u) / 4u] == 1u); /* IP is W1C; IE stays off. */
    assert(c->commands.controller_active && c->events.controller_active);
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(c->state == USB_XHCI_CONTROLLER_EMPTY && m.resets == 2);
    assert(m.allocations == m.releases && usb_xhci_controller_stop(c) == USB_OK);
    free(c);
}
static void test_failure_retention(void)
{
    struct model baseline; init_model(&baseline, 2);
    struct usb_xhci_controller *c = new_controller();
    assert(start(&baseline, c) == USB_OK);
    unsigned start_accesses = baseline.accesses, start_allocations = baseline.allocation_attempts;
    unsigned start_syncs = baseline.syncs, start_guards = baseline.guard_calls;
    assert(usb_xhci_controller_stop(c) == USB_OK);
    unsigned stop_accesses = baseline.accesses - start_accesses;
    unsigned stop_guards = baseline.guard_calls - start_guards;
    free(c);

    for (unsigned point = 1; point <= start_accesses; ++point) {
        for (unsigned applied = 0; applied < 2; ++applied) {
            struct model m; init_model(&m, 2); c = new_controller();
            m.fail_access = point; m.applied_error = applied != 0;
            assert(start(&m, c) != USB_OK && m.releases == 0);
            m.fail_access = 0;
            assert(usb_xhci_controller_stop(c) == USB_OK);
            assert(m.allocations == m.releases);
            free(c);
        }
    }
    for (unsigned point = 1; point <= stop_accesses; ++point) {
        for (unsigned applied = 0; applied < 2; ++applied) {
            struct model m; init_model(&m, 2); c = new_controller();
            assert(start(&m, c) == USB_OK);
            m.fail_access = m.accesses + point; m.applied_error = applied != 0;
            assert(usb_xhci_controller_stop(c) != USB_OK);
            assert(c->state == USB_XHCI_CONTROLLER_FAULTED && m.releases == 0);
            m.fail_access = 0;
            assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
            free(c);
        }
    }
    for (unsigned point = 1; point <= start_allocations; ++point) {
        struct model m; init_model(&m, 2); c = new_controller();
        m.fail_allocate = point;
        assert(start(&m, c) == USB_NO_MEMORY && c->state == USB_XHCI_CONTROLLER_FAULTED);
        m.fail_allocate = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    for (unsigned point = 1; point <= start_syncs; ++point) {
        struct model m; init_model(&m, 2); c = new_controller();
        m.fail_sync = point;
        assert(start(&m, c) == USB_IO && c->state == USB_XHCI_CONTROLLER_FAULTED);
        m.fail_sync = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    for (unsigned point = 1; point <= start_guards; ++point) {
        struct model m; init_model(&m, 2); c = new_controller();
        m.fail_guard = point;
        assert(start(&m, c) == USB_BUSY);
        m.fail_guard = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    for (unsigned point = 1; point <= stop_guards; ++point) {
        struct model m; init_model(&m, 2); c = new_controller();
        assert(start(&m, c) == USB_OK);
        m.fail_guard = m.guard_calls + point;
        assert(usb_xhci_controller_stop(c) == USB_BUSY && m.releases == 0);
        m.fail_guard = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    for (unsigned point = 1; point <= 3; ++point) {
        struct model m; init_model(&m, 0); c = new_controller();
        assert(start(&m, c) == USB_OK);
        m.fail_sync = m.syncs + point;
        assert(usb_xhci_controller_stop(c) == USB_IO && m.releases == 0);
        m.fail_sync = 0;
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    printf("PASS: xHCI controller startup/stop %u/%u MMIO fault positions retain DMA\n",
           start_accesses, stop_accesses);
}
static void test_guards_and_timeouts(void)
{
    struct model m; init_model(&m, 0); struct usb_xhci_controller *c = new_controller();
    m.guard_denied = true;
    assert(start(&m, c) == USB_BUSY && !m.accesses && !m.allocations);
    m.guard_denied = false; m.regs[(OP + 4u) / 4u] = 0;
    assert(start(&m, c) == USB_BUSY && !m.allocations);
    init_model(&m, 0); m.regs[(OP + 4u) / 4u] |= 0x800u;
    assert(start(&m, c) == USB_BUSY && !m.allocations);
    init_model(&m, 0); assert(start(&m, c) == USB_OK);
    m.guard_denied = true;
    assert(usb_xhci_controller_stop(c) == USB_BUSY && !m.releases);
    m.guard_denied = false; m.stuck_halt = true;
    assert(usb_xhci_controller_stop(c) == USB_TIMEOUT && !m.releases);
    m.stuck_halt = false;
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);
    init_model(&m, 0); c = new_controller(); m.stuck_reset = true;
    assert(start(&m, c) == USB_TIMEOUT && c->state == USB_XHCI_CONTROLLER_FAULTED);
    assert(usb_xhci_controller_stop(c) == USB_TIMEOUT && !m.releases);
    m.stuck_reset = false; m.regs[OP / 4u] &= ~CMD_RESET;
    assert(usb_xhci_controller_stop(c) == USB_OK);
    free(c);

    for (unsigned point = 1; point <= 2; ++point) {
        init_model(&m, 2); c = new_controller(); m.noncoherent_at = point;
        assert(start(&m, c) == USB_UNSUPPORTED && c->state == USB_XHCI_CONTROLLER_FAULTED);
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
    init_model(&m, 2); c = new_controller(); m.misalign_at = 2;
    assert(start(&m, c) == USB_RANGE && c->state == USB_XHCI_CONTROLLER_FAULTED);
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);
    init_model(&m, 513); c = new_controller();
    m.regs[(OP + 8u) / 4u] = 3u; /* A support bit does not select 8 KiB. */
    assert(start(&m, c) == USB_UNSUPPORTED && !m.resets && !m.allocations);
    free(c);
    const unsigned counts[] = {35, 512};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        init_model(&m, counts[i]); c = new_controller();
        assert(start(&m, c) == USB_OK && c->scratch_count == counts[i] &&
               c->scratch_allocated == counts[i]);
        assert(c->page_bytes == 4096u);
        assert(c->scratch_array.size <= c->page_bytes &&
               !(c->scratch_array.address & (c->page_bytes - 1u)));
        assert(get_le64((const uint8_t *)c->scratch_array.cpu + 8u * (counts[i] - 1u)) ==
               c->scratch_pages[counts[i] - 1u].address);
        assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
        free(c);
    }
}
int main(void)
{
    test_running_schedule(); test_failure_retention(); test_guards_and_timeouts(); test_commands_and_events();
    test_control_transfer(); test_device_slots_and_routing(); test_root_port_reset();
    puts("PASS: xHCI production controller lifecycle model; hardware_qualified=false");
    return 0;
}
