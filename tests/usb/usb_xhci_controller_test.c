/* Production xHCI controller lifecycle against a fault-injected register/DMA model. */
#define _POSIX_C_SOURCE 200112L
#include "UsbXhciHost.h"
#include "usb_xhci_controller_test_api.h"
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
#define PORTSC_WRC (1u << 19)
#define PORTSC_OCC (1u << 20)
#define PORTSC_PRC (1u << 21)
#define PORTSC_PLC (1u << 22)
#define PORTSC_CEC (1u << 23)
#define PORTSC_CHANGE_MASK (0x7fu << 17)
#define PORTSC_CONTROL_MASK ((0xfu << 5) | PORTSC_PP | (3u << 14) | (7u << 25))

static const uint8_t disk_device_descriptor[18] = {
    18, 1, 0, 2, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 0, 0, 1
};
static const uint8_t disk_configuration_descriptor[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,
    7, 5, 0x81, 2, 0, 2, 0,
    7, 5, 0x02, 2, 0, 2, 0
};
static const uint8_t disk_full_speed_configuration_descriptor[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0
};
static const uint8_t hid_bot_configuration_descriptor[57] = {
    9, 2, 57, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x3f, 0,
    7, 5, 0x81, 3, 8, 0, 10,
    9, 4, 1, 0, 2, 8, 6, 0x50, 0,
    7, 5, 0x83, 2, 64, 0, 0,
    7, 5, 0x03, 2, 64, 0, 0
};
static const uint8_t hub_device_descriptor[18] = {
    18, 1, 0, 2, 9, 0, 1, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 0, 0, 1
};
static const uint8_t hub_configuration_descriptor[25] = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 9, 0, 0, 0,
    7, 5, 0x81, 3, 1, 0, 12
};
static const uint8_t hub_descriptor[9] = {9, 0x29, 1, 0x21, 0, 10, 0, 0, 0};

struct model {
    uint32_t regs[0x1000 / 4];
    struct usb_mmio platform_mmio;
    bool platform_mmio_bound;
    uint64_t next_dma;
    unsigned accesses, fail_access, allocations, allocation_attempts, fail_allocate, releases, syncs, fail_sync;
    unsigned resets, starts, guard_calls, fail_guard, noncoherent_at, misalign_at, cnr_waits;
    unsigned reset_delay, cnr_delay, halt_delay, reset_remaining, cnr_remaining, halt_remaining;
    unsigned port_reset_delay, port_reset_remaining, port_writes;
    uint32_t crcr_reserved, erstba_reserved;
    struct usb_xhci_controller *controller;
    struct usb_xhci_device_slot *slot;
    struct usb_xhci_device_slot *slot_by_id[256];
    bool slot_is_hub[256], pending_slot_is_hub;
    uint32_t event_index;
    uint32_t operation_order, last_endpoint_doorbell_order, last_set_dequeue_order;
    unsigned configure_endpoint_commands, reset_endpoint_commands, set_dequeue_commands;
    unsigned endpoint_doorbells;
    uint32_t last_command_control, last_command_dword0, last_command_dword1;
    uint8_t last_command_type;
    uint8_t event_cycle, next_slot, next_usb_address;
    uint8_t port_reset_speed;
    uint8_t endpoint_completion_code, endpoint_pattern_start;
    uint32_t endpoint_residual;
    const uint8_t *expected_out_data;
    size_t expected_out_size;
    uint8_t device_descriptor[8];
    uint8_t test_media[4096];
    uint8_t *bot_media;
    size_t bot_media_size;
    uint8_t bot_cdb[16];
    uint32_t bot_tag, bot_transfer_size, bot_lba, bot_blocks;
    unsigned bot_phase, bot_commands, bot_sync_commands, bot_capacity_commands;
    unsigned bot_read_only_commands, bot_read_commands, bot_write_commands;
    uint16_t bot_tt_clear_value[2], bot_tt_clear_index[2];
    uint8_t bot_clear_feature_endpoint[2];
    unsigned bot_class_resets, bot_tt_buffer_clears, bot_clear_features, bot_stalls;
    uint8_t bot_cdb_length;
    unsigned host_control_requests, host_get_device8, host_get_device18;
    unsigned host_get_config_header, host_get_config, host_set_config;
    bool host_stack_enumeration, bot_direction_in, composite_hid_bot;
    uint8_t bot_in_endpoint, bot_out_endpoint;
    uint8_t hid_input_report[8];
    unsigned hid_interrupts, hid_protocol_selections;
    uint8_t hub_interrupt_bitmap[USB_HOST_HUB_MAX_BITMAP_BYTES];
    uint16_t hub_port_status, hub_port_change;
    uint32_t hub_reset_complete_ms, current_ms;
    unsigned hub_interrupts, hub_power_sets, hub_power_clears, hub_port_resets;
    bool applied_error, guard_denied, guard_blocked, stuck_halt, stuck_reset, pending_on_reset;
    bool port_reset_overcurrent, port_reset_no_event, port_reset_unrelated_event;
    bool allow_live_release, stall_next_bot_transfer;
    bool auto_commands, auto_control, auto_endpoint, endpoint_output_matches;
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
    m->endpoint_completion_code = USB_XHCI_CC_SUCCESS;
    m->endpoint_pattern_start = 0xa0u;
    m->bot_media = m->test_media;
    m->bot_media_size = sizeof(m->test_media);
    m->bot_in_endpoint = 0x81u;
    m->bot_out_endpoint = 0x02u;
    m->hid_input_report[2] = 4u;
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
static uint32_t get_be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}
static struct usb_xhci_device_slot *slot_by_id(struct model *m, uint8_t slot_id)
{ return slot_id ? m->slot_by_id[slot_id] : NULL; }
static struct usb_setup current_control_setup(const struct usb_xhci_device_slot *slot)
{
    const struct usb_xhci_transfer_ring *ring = slot->control.ring;
    assert(ring && ring->trbs.cpu && slot->control.trb_count &&
           ring->usable_entries >= slot->control.trb_count);
    uint32_t index = ring->producer_index;
    for (uint8_t i = 0; i < slot->control.trb_count; ++i)
        index = index ? index - 1u : ring->usable_entries - 1u;
    const uint8_t *bytes = (const uint8_t *)ring->trbs.cpu + (size_t)index * USB_XHCI_TRB_BYTES;
    uint32_t control = get_le32(bytes + 12u);
    assert(((control >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK) ==
           USB_XHCI_TRB_SETUP_STAGE);
    if (slot->control.requested) {
        uint32_t data_index = index + 1u;
        if (data_index == ring->usable_entries) data_index = 0u;
        const uint8_t *data = (const uint8_t *)ring->trbs.cpu +
                              (size_t)data_index * USB_XHCI_TRB_BYTES;
        uint32_t data_control = get_le32(data + 12u);
        uint64_t data_address = (uint64_t)get_le32(data) |
                                ((uint64_t)get_le32(data + 4u) << 32);
        assert(((data_control >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK) ==
               USB_XHCI_TRB_DATA_STAGE);
        assert(data_address == slot->control.data.address &&
               get_le32(data + 8u) == slot->control.requested);
    }
    uint32_t first = get_le32(bytes), second = get_le32(bytes + 4u);
    return (struct usb_setup){
        (uint8_t)first, (uint8_t)(first >> 8), (uint16_t)(first >> 16),
        (uint16_t)second, (uint16_t)(second >> 16)
    };
}
static void complete_host_control(struct model *m, struct usb_xhci_device_slot *slot)
{
    struct usb_xhci_control_td *td = &slot->control;
    struct usb_setup setup = current_control_setup(slot);
    assert(td->requested == setup.length);
    ++m->host_control_requests;
    if (setup.type == 0x80u && setup.request == 6u) {
        const uint8_t *response;
        size_t response_size;
        if (setup.value == 0x0100u) {
            response = m->slot_is_hub[slot->slot_id] ? hub_device_descriptor :
                       disk_device_descriptor;
            response_size = sizeof(disk_device_descriptor);
            if (setup.length == 8u) ++m->host_get_device8;
            else if (setup.length == sizeof(disk_device_descriptor)) ++m->host_get_device18;
            else assert(0 && "unexpected device descriptor length");
        } else if (setup.value == 0x0200u) {
            if (m->slot_is_hub[slot->slot_id]) {
                response = hub_configuration_descriptor;
                response_size = sizeof(hub_configuration_descriptor);
            } else if (m->composite_hid_bot) {
                response = hid_bot_configuration_descriptor;
                response_size = sizeof(hid_bot_configuration_descriptor);
            } else if (slot->speed == USB_XHCI_SPEED_FULL) {
                response = disk_full_speed_configuration_descriptor;
                response_size = sizeof(disk_full_speed_configuration_descriptor);
            } else {
                response = disk_configuration_descriptor;
                response_size = sizeof(disk_configuration_descriptor);
            }
            if (setup.length == 9u) ++m->host_get_config_header;
            else if (setup.length == response_size) ++m->host_get_config;
            else assert(0 && "unexpected configuration descriptor length");
        } else {
            assert(0 && "unexpected descriptor request");
            return;
        }
        assert(td->input && td->data.cpu && setup.length <= response_size);
        memcpy(td->data.cpu, response, setup.length);
    } else if (setup.type == 0u && setup.request == 9u && setup.value == 1u &&
               setup.index == 0u && setup.length == 0u) {
        assert(!td->input && !td->data.cpu);
        ++m->host_set_config;
    } else if (!m->slot_is_hub[slot->slot_id] && m->composite_hid_bot &&
               setup.type == 0x21u && setup.request == 0x0bu && setup.value == 0u &&
               setup.index == 0u && setup.length == 0u) {
        assert(!td->input && !td->data.cpu);
        ++m->hid_protocol_selections;
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0xa0u &&
               setup.request == 6u && setup.value == 0x2900u && setup.index == 0u &&
               (setup.length == 7u || setup.length == sizeof(hub_descriptor))) {
        assert(td->input && td->data.cpu);
        memcpy(td->data.cpu, hub_descriptor, setup.length);
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0xa0u &&
               setup.request == 0u && setup.value == 0u &&
               setup.length == 4u && setup.index == 0u) {
        assert(td->input && td->data.cpu);
        memset(td->data.cpu, 0, 4u);
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0xa3u &&
               setup.request == 0u && setup.value == 0u &&
               setup.length == 4u && setup.index == 1u) {
        assert(td->input && td->data.cpu);
        if (m->hub_reset_complete_ms && m->current_ms >= m->hub_reset_complete_ms) {
            m->hub_port_status &= (uint16_t)~0x0010u;
            m->hub_port_status |= 0x0003u; /* connected, enabled, full speed */
            m->hub_port_change |= 0x0010u;
            m->hub_reset_complete_ms = 0;
        }
        put_le32(td->data.cpu, (uint32_t)m->hub_port_status |
                               ((uint32_t)m->hub_port_change << 16));
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0x23u &&
               setup.length == 0u && setup.index == 1u && setup.request == 3u &&
               setup.value == 8u) {
        assert(!td->input && !td->data.cpu);
        m->hub_port_status |= 0x0100u;
        ++m->hub_power_sets;
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0x23u &&
               setup.length == 0u && setup.index == 1u && setup.request == 3u &&
               setup.value == 4u) {
        assert(!td->input && !td->data.cpu);
        m->hub_port_status |= 0x0010u;
        m->hub_reset_complete_ms = m->current_ms + 50u;
        ++m->hub_port_resets;
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0x23u &&
               setup.request == 8u && setup.length == 0u && setup.index == 1u &&
               (setup.value == 0x9021u || setup.value == 0x1022u)) {
        assert(!td->input && !td->data.cpu && m->bot_tt_buffer_clears < 2u);
        unsigned index = m->bot_tt_buffer_clears++;
        m->bot_tt_clear_value[index] = setup.value;
        m->bot_tt_clear_index[index] = setup.index;
    } else if (m->slot_is_hub[slot->slot_id] && setup.type == 0x23u &&
               setup.length == 0u && setup.index == 1u && setup.request == 1u) {
        assert(!td->input && !td->data.cpu);
        if (setup.value == 8u) {
            m->hub_port_status &= (uint16_t)~0x0100u;
            ++m->hub_power_clears;
        } else if (setup.value >= 16u && setup.value <= 20u) {
            m->hub_port_change &= (uint16_t)~(1u << (setup.value - 16u));
        } else {
            assert(0 && "unexpected hub port feature clear");
        }
    } else if (!m->slot_is_hub[slot->slot_id] && setup.type == 0x21u &&
               setup.request == 0xffu && setup.value == 0u && setup.index == 0u &&
               setup.length == 0u) {
        assert(!td->input && !td->data.cpu);
        ++m->bot_class_resets;
    } else if (!m->slot_is_hub[slot->slot_id] && setup.type == 0x02u &&
               setup.request == 1u && setup.value == 0u && setup.length == 0u &&
               (setup.index == m->bot_in_endpoint || setup.index == m->bot_out_endpoint ||
                (m->composite_hid_bot && setup.index == 0x81u))) {
        assert(!td->input && !td->data.cpu && m->bot_clear_features < 2u);
        m->bot_clear_feature_endpoint[m->bot_clear_features++] = (uint8_t)setup.index;
    } else {
        assert(0 && "unexpected xHCI-backed host control request");
    }
}
static const uint8_t *current_endpoint_transfer_trb(
    const struct usb_xhci_device_endpoint *endpoint)
{
    const struct usb_xhci_transfer_ring *ring = &endpoint->ring;
    assert(endpoint->transfer_pending && endpoint->transfer_trb >= ring->trbs.address);
    uint64_t offset = endpoint->transfer_trb - ring->trbs.address;
    assert(!(offset % USB_XHCI_TRB_BYTES));
    uint64_t index = offset / USB_XHCI_TRB_BYTES;
    assert(index < ring->usable_entries);
    const uint8_t *trb = (const uint8_t *)ring->trbs.cpu +
                         (size_t)index * USB_XHCI_TRB_BYTES;
    uint32_t control = get_le32(trb + 12u);
    uint64_t address = (uint64_t)get_le32(trb) |
                       ((uint64_t)get_le32(trb + 4u) << 32);
    assert(((control >> USB_XHCI_TRB_TYPE_SHIFT) & USB_XHCI_TRB_TYPE_MASK) ==
           USB_XHCI_TRB_NORMAL);
    assert(address == endpoint->transfer_data.address &&
           get_le32(trb + 8u) == endpoint->transfer_length);
    return trb;
}
static void complete_bot_endpoint(struct model *m, struct usb_xhci_device_endpoint *endpoint)
{
    uint8_t *bytes = endpoint->transfer_data.cpu;
    size_t size = endpoint->transfer_length;
    assert(bytes && endpoint->descriptor.type == 2u);
    (void)current_endpoint_transfer_trb(endpoint);
    if (!endpoint->input) {
        assert(endpoint->descriptor.address == m->bot_out_endpoint);
        if (m->bot_phase == 0u) {
            assert(size == 31u && get_le32(bytes) == 0x43425355u);
            m->bot_tag = get_le32(bytes + 4u);
            m->bot_transfer_size = get_le32(bytes + 8u);
            m->bot_direction_in = (bytes[12] & 0x80u) != 0;
            m->bot_cdb_length = bytes[14];
            assert((bytes[12] & 0x7fu) == 0u && bytes[13] == 0u &&
                   m->bot_cdb_length && m->bot_cdb_length <= sizeof(m->bot_cdb));
            memcpy(m->bot_cdb, bytes + 15u, sizeof(m->bot_cdb));
            ++m->bot_commands;
            if (!m->bot_transfer_size) {
                assert(!m->bot_direction_in && m->bot_cdb_length == 10u &&
                       m->bot_cdb[0] == 0x35u);
                ++m->bot_sync_commands;
                m->bot_phase = 2u;
            } else {
                assert((m->bot_direction_in &&
                        ((m->bot_cdb[0] == 0x25u && m->bot_cdb_length == 10u && m->bot_transfer_size == 8u) ||
                         (m->bot_cdb[0] == 0x1au && m->bot_cdb_length == 6u && m->bot_transfer_size == 4u) ||
                         (m->bot_cdb[0] == 0x28u && m->bot_cdb_length == 10u))) ||
                       (!m->bot_direction_in && m->bot_cdb[0] == 0x2au &&
                        m->bot_cdb_length == 10u));
                if (m->bot_cdb[0] == 0x25u) ++m->bot_capacity_commands;
                if (m->bot_cdb[0] == 0x1au) ++m->bot_read_only_commands;
                if (m->bot_cdb[0] == 0x28u) ++m->bot_read_commands;
                if (m->bot_cdb[0] == 0x2au) ++m->bot_write_commands;
                m->bot_phase = 1u;
            }
        } else {
            assert(m->bot_phase == 1u && !m->bot_direction_in &&
                   size == m->bot_transfer_size && m->bot_cdb[0] == 0x2au);
            m->bot_lba = get_be32(m->bot_cdb + 2u);
            m->bot_blocks = ((uint32_t)m->bot_cdb[7] << 8) | m->bot_cdb[8];
            size_t offset = (size_t)m->bot_lba * 512u;
            assert(m->bot_media && m->bot_blocks && size == (size_t)m->bot_blocks * 512u &&
                   offset <= m->bot_media_size && size <= m->bot_media_size - offset);
            memcpy(m->bot_media + offset, bytes, size);
            m->bot_phase = 2u;
        }
    } else {
        assert(endpoint->descriptor.address == m->bot_in_endpoint);
        if (m->bot_phase == 1u) {
            assert(m->bot_direction_in && size == m->bot_transfer_size);
            if (m->bot_cdb[0] == 0x25u) {
                assert(size == 8u);
                assert(m->bot_media && m->bot_media_size >= 512u &&
                       m->bot_media_size / 512u <= UINT32_MAX);
                put_be32(bytes, (uint32_t)(m->bot_media_size / 512u - 1u));
                put_be32(bytes + 4u, 512u);
            } else if (m->bot_cdb[0] == 0x1au) {
                assert(size == 4u);
                memset(bytes, 0, size);
                bytes[2] = 0x80u; /* The modeled medium is write-protected. */
            } else if (m->bot_cdb[0] == 0x28u) {
                m->bot_lba = get_be32(m->bot_cdb + 2u);
                m->bot_blocks = ((uint32_t)m->bot_cdb[7] << 8) | m->bot_cdb[8];
                size_t offset = (size_t)m->bot_lba * 512u;
                assert(m->bot_media && m->bot_blocks && size == (size_t)m->bot_blocks * 512u &&
                       offset <= m->bot_media_size && size <= m->bot_media_size - offset);
                memcpy(bytes, m->bot_media + offset, size);
            } else {
                assert(0 && "unexpected BOT data-in command");
            }
            m->bot_phase = 2u;
        } else {
            assert(m->bot_phase == 2u && size == 13u);
            memset(bytes, 0, size); put_le32(bytes, 0x53425355u);
            put_le32(bytes + 4u, m->bot_tag); m->bot_phase = 0u;
        }
    }
}
static void complete_host_endpoint(struct model *m, struct usb_xhci_device_slot *slot,
                                   struct usb_xhci_device_endpoint *endpoint)
{
    if (m->slot_is_hub[slot->slot_id] && endpoint->descriptor.type == 3u) {
        assert(endpoint->input && endpoint->descriptor.address == 0x81u &&
               endpoint->transfer_data.cpu && endpoint->transfer_length == 1u);
        (void)current_endpoint_transfer_trb(endpoint);
        memcpy(endpoint->transfer_data.cpu, m->hub_interrupt_bitmap, 1u);
        ++m->hub_interrupts;
        return;
    }
    if (m->composite_hid_bot && endpoint->descriptor.type == 3u &&
        endpoint->descriptor.address == 0x81u) {
        assert(endpoint->input && endpoint->transfer_data.cpu && endpoint->transfer_length == 8u);
        (void)current_endpoint_transfer_trb(endpoint);
        memcpy(endpoint->transfer_data.cpu, m->hid_input_report, sizeof(m->hid_input_report));
        ++m->hid_interrupts;
        return;
    }
    complete_bot_endpoint(m, endpoint);
}
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    struct model *m = context; assert(!m->guard_blocked && offset < sizeof(m->regs) && !(offset & 3u));
    if (++m->accesses == m->fail_access) return USB_IO;
    if (offset == OP + 4u && (m->regs[offset / 4u] & 0x800u) &&
        !(m->regs[OP / 4u] & CMD_RESET)) ++m->cnr_waits;
    uint32_t modeled = m->regs[offset / 4u];
    if (m->platform_mmio_bound) {
        /* Model the controller's hardware-updated register state in the owner's
         * mapped aperture, then read it back through the production guarded callback. */
        int status = m->platform_mmio.write32(m->platform_mmio.context, offset, modeled);
        if (status != USB_OK) return status;
        status = m->platform_mmio.read32(m->platform_mmio.context, offset, value);
        if (status != USB_OK) return status;
        return *value == modeled ? USB_OK : USB_IO;
    }
    *value = modeled; return USB_OK;
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
            ++m->operation_order;
            m->last_command_type = (uint8_t)type;
            m->last_command_control = control;
            m->last_command_dword0 = get_le32(command);
            m->last_command_dword1 = get_le32(command + 4u);
            if (type == USB_XHCI_TRB_CONFIGURE_ENDPOINT) ++m->configure_endpoint_commands;
            if (type == USB_XHCI_TRB_RESET_ENDPOINT) ++m->reset_endpoint_commands;
            if (type == USB_XHCI_TRB_SET_TR_DEQUEUE) {
                ++m->set_dequeue_commands;
                m->last_set_dequeue_order = m->operation_order;
            }
            if (type == USB_XHCI_TRB_ENABLE_SLOT) {
                slot_id = m->next_slot++;
                assert(slot_id && m->slot && !m->slot_by_id[slot_id]);
                m->slot_by_id[slot_id] = m->slot;
                m->slot_is_hub[slot_id] = m->pending_slot_is_hub;
            }
            struct usb_xhci_device_slot *command_slot = slot_by_id(m, slot_id);
            if (type == USB_XHCI_TRB_ADDRESS_DEVICE && command_slot &&
                command_slot->output_context.cpu && command_slot->input_context.cpu) {
                uint8_t stride = m->controller->caps.context_bytes;
                uint8_t *input = command_slot->input_context.cpu;
                uint8_t *out = command_slot->output_context.cpu;
                uint32_t state = control & (1u << 9) ? 1u : 2u;
                uint8_t assigned = state == 1u ? 0u : m->next_usb_address++;
                memcpy(out, input + stride, stride);
                put_le32(out + 12u, (uint32_t)assigned | (state << 27));
                memcpy(out + stride, input + 2u * stride, stride);
            }
            if (type == USB_XHCI_TRB_CONFIGURE_ENDPOINT && command_slot &&
                command_slot->output_context.cpu) {
                uint8_t stride = m->controller->caps.context_bytes;
                uint8_t *input = command_slot->input_context.cpu;
                uint8_t *output = command_slot->output_context.cpu;
                uint32_t add_flags = get_le32(input + 4u);
                if (add_flags & 1u) {
                    uint32_t state = get_le32(output + 12u);
                    memcpy(output, input + stride, stride);
                    put_le32(output + 12u, state);
                }
                for (uint8_t dci = 1u; dci <= 31u; ++dci) {
                    if (!(add_flags & (1u << dci))) continue;
                    size_t context_offset = (size_t)(dci + 1u) * stride;
                    memcpy(output + (size_t)dci * stride, input + context_offset, stride);
                    size_t output_offset = (size_t)dci * stride;
                    uint32_t ep0 = get_le32(output + output_offset);
                    put_le32(output + output_offset, (ep0 & ~7u) | 1u);
                }
            }
            event_at(m->controller, m->event_index, m->event_cycle,
                     USB_XHCI_TRB_COMMAND_EVENT, address, USB_XHCI_CC_SUCCESS,
                     (uint32_t)slot_id << 24);
            if (type == USB_XHCI_TRB_DISABLE_SLOT) {
                m->slot_by_id[slot_id] = NULL;
                m->slot_is_hub[slot_id] = false;
            }
            if (++m->event_index == m->controller->events.entries) {
                m->event_index = 0; m->event_cycle ^= 1u;
            }
        } else if (offset >= 0x604u && m->auto_control && value == 1u) {
            uint8_t slot_id = (uint8_t)((offset - 0x600u) / 4u);
            struct usb_xhci_device_slot *doorbell_slot = slot_by_id(m, slot_id);
            assert(doorbell_slot);
            if (doorbell_slot->control.pending &&
                offset == 0x600u + (uint32_t)slot_id * 4u) {
                if (m->host_stack_enumeration) {
                    complete_host_control(m, doorbell_slot);
                } else if (doorbell_slot->control.input && doorbell_slot->control.data.cpu) {
                    size_t length = doorbell_slot->control.requested < sizeof(m->device_descriptor) ?
                                    doorbell_slot->control.requested : sizeof(m->device_descriptor);
                    memcpy(doorbell_slot->control.data.cpu, m->device_descriptor, length);
                }
                event_at(m->controller, m->event_index, m->event_cycle,
                         USB_XHCI_TRB_TRANSFER_EVENT, doorbell_slot->control.status_trb,
                         USB_XHCI_CC_SUCCESS,
                         ((uint32_t)slot_id << 24) | (1u << 16));
                if (++m->event_index == m->controller->events.entries) {
                    m->event_index = 0; m->event_cycle ^= 1u;
                }
            }
        } else if (offset >= 0x604u && m->auto_endpoint && value >= 2u && value <= 31u) {
            uint8_t slot_id = (uint8_t)((offset - 0x600u) / 4u);
            struct usb_xhci_device_slot *doorbell_slot = slot_by_id(m, slot_id);
            assert(doorbell_slot);
            uint8_t dci = (uint8_t)value;
            struct usb_xhci_device_endpoint *endpoint = &doorbell_slot->endpoints[dci - 2u];
            assert(endpoint->transfer_pending && endpoint->dci == dci);
            ++m->endpoint_doorbells;
            m->last_endpoint_doorbell_order = ++m->operation_order;
            uint8_t completion_code = m->endpoint_completion_code;
            if (m->host_stack_enumeration && m->stall_next_bot_transfer &&
                !m->slot_is_hub[slot_id] && endpoint->descriptor.type == 2u) {
                m->stall_next_bot_transfer = false;
                ++m->bot_stalls;
                completion_code = USB_XHCI_CC_STALL_ERROR;
            } else if (m->host_stack_enumeration) {
                complete_host_endpoint(m, doorbell_slot, endpoint);
            } else {
                if (endpoint->input && endpoint->transfer_data.cpu) {
                    uint8_t *data = endpoint->transfer_data.cpu;
                    for (uint32_t i = 0; i < endpoint->transfer_length; ++i)
                        data[i] = (uint8_t)(m->endpoint_pattern_start + i);
                } else if (m->expected_out_data) {
                    m->endpoint_output_matches = endpoint->transfer_length == m->expected_out_size &&
                        !memcmp(endpoint->transfer_data.cpu, m->expected_out_data,
                                m->expected_out_size);
                }
            }
            event_at(m->controller, m->event_index, m->event_cycle,
                     USB_XHCI_TRB_TRANSFER_EVENT, endpoint->transfer_trb,
                     completion_code,
                     ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16));
            put_le32((uint8_t *)m->controller->events.trbs.cpu +
                     (size_t)m->event_index * USB_XHCI_TRB_BYTES + 8u,
                     ((uint32_t)completion_code << 24) |
                     (m->endpoint_residual & 0x00ffffffu));
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
    if (!fail || m->applied_error) {
        if (m->platform_mmio_bound) {
            int status = m->platform_mmio.write32(m->platform_mmio.context, offset, value);
            if (status != USB_OK) return status;
        }
        apply_write(m, offset, value);
    }
    return fail ? USB_IO : USB_OK;
}
static void barrier(void *context)
{
    struct model *m = context;
    if (m->platform_mmio_bound && m->platform_mmio.barrier)
        m->platform_mmio.barrier(m->platform_mmio.context);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static void delay(void *context, uint32_t us)
{
    struct model *m = context; assert(us);
    if (m->platform_mmio_bound && m->platform_mmio.delay_us)
        m->platform_mmio.delay_us(m->platform_mmio.context, us);
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
static int start_with_dma(struct model *m, struct usb_xhci_controller *c,
                          const struct usb_dma_ops *external_dma)
{
    struct usb_mmio io = mmio(m);
    struct usb_dma_ops d = external_dma ? *external_dma : dma(m);
    m->controller = c;
    return usb_xhci_controller_start(c, &io, &d, guard, m, 4, 16, 16);
}
#ifndef USB_XHCI_TEST_MODEL_ONLY
static int start(struct model *m, struct usb_xhci_controller *c)
{ return start_with_dma(m, c, NULL); }
#endif
struct adapter_lock_state { bool held; unsigned acquisitions, releases; };
static int adapter_test_lock(void *context)
{
    struct adapter_lock_state *state = context;
    if (state->held) return USB_BUSY;
    state->held = true; ++state->acquisitions; return USB_OK;
}
static void adapter_test_unlock(void *context)
{
    struct adapter_lock_state *state = context;
    assert(state->held);
    state->held = false; ++state->releases;
}
static uint32_t adapter_test_clock(void *context)
{ return *(const uint32_t *)context; }
#ifndef USB_XHCI_TEST_MODEL_ONLY
static uint64_t get_le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8u * i);
    return value;
}
#endif
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

#ifndef USB_XHCI_TEST_MODEL_ONLY
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

static void test_root_port_status(void)
{
    struct model m; init_model(&m, 0);
    m.regs[PORTSC / 4u] |= PORTSC_PED | PORTSC_OCA |
        (USB_XHCI_SPEED_HIGH << PORTSC_SPEED_SHIFT) |
        PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC |
        PORTSC_PRC | PORTSC_PLC | PORTSC_CEC;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_host_device devices[1] = {0};
    struct adapter_lock_state lock_state = {0};
    struct usb_xhci_host_adapter adapter;
    assert(usb_xhci_host_adapter_init(&adapter, c, devices, 1u,
                                     adapter_test_lock, adapter_test_unlock, &lock_state,
                                     adapter_test_clock, &m.current_ms, 100u) == USB_OK);

    struct usb_xhci_root_port_status status = {0};
    assert(usb_xhci_host_adapter_root_port_get_status(&adapter, 1u, &status) == USB_OK);
    assert(status.connected && status.enabled && status.overcurrent && !status.resetting &&
           status.powered && status.speed_id == USB_XHCI_SPEED_HIGH &&
           status.changes == USB_XHCI_ROOT_PORT_CHANGE_MASK && !m.port_writes);
    const uint32_t stable = m.regs[PORTSC / 4u] &
        (PORTSC_CCS | PORTSC_PED | PORTSC_OCA | PORTSC_PP | (0xfu << PORTSC_SPEED_SHIFT));

    uint32_t first_ack = USB_XHCI_ROOT_PORT_CHANGE_CONNECT |
                         USB_XHCI_ROOT_PORT_CHANGE_RESET;
    assert(usb_xhci_host_adapter_root_port_ack_changes(&adapter, 1u, first_ack) == USB_OK);
    assert(m.port_writes == 1u &&
           (m.regs[PORTSC / 4u] & stable) == stable &&
           !(m.regs[PORTSC / 4u] & (PORTSC_CSC | PORTSC_PRC)) &&
           (m.regs[PORTSC / 4u] & (PORTSC_PEC | PORTSC_WRC | PORTSC_OCC |
                                   PORTSC_PLC | PORTSC_CEC)) ==
           (PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PLC | PORTSC_CEC));
    struct usb_xhci_root_port_status unchanged = status;
    assert(usb_xhci_host_adapter_root_port_get_status(&adapter, 1u, &status) == USB_OK);
    assert(status.connected && status.enabled && status.overcurrent && status.powered &&
           status.speed_id == USB_XHCI_SPEED_HIGH &&
           status.changes == (USB_XHCI_ROOT_PORT_CHANGE_MASK & ~first_ack));
    assert(unchanged.changes == USB_XHCI_ROOT_PORT_CHANGE_MASK);
    unchanged = status;

    uint32_t writes = m.port_writes;
    assert(usb_xhci_host_adapter_root_port_ack_changes(&adapter, 1u, 1u << 31) == USB_INVALID);
    c->caps.usb2_ports[0] &= (uint8_t)~(1u << 1);
    assert(usb_xhci_host_adapter_root_port_get_status(&adapter, 2u, &unchanged) == USB_UNSUPPORTED);
    assert(unchanged.changes == (USB_XHCI_ROOT_PORT_CHANGE_MASK & ~first_ack));
    struct usb_xhci_root_port_status sentinel = {
        .connected = true, .enabled = true, .overcurrent = true, .resetting = true,
        .powered = true, .speed_id = 99u, .changes = 0xa5a5a5a5u
    };
    assert(usb_xhci_host_adapter_root_port_get_status(&adapter, 0u, &sentinel) == USB_RANGE);
    assert(sentinel.connected && sentinel.enabled && sentinel.overcurrent && sentinel.resetting &&
           sentinel.powered && sentinel.speed_id == 99u && sentinel.changes == 0xa5a5a5a5u);
    assert(m.port_writes == writes && !lock_state.held &&
           lock_state.acquisitions == lock_state.releases);

    assert(usb_xhci_host_adapter_root_port_ack_changes(
               &adapter, 1u, USB_XHCI_ROOT_PORT_CHANGE_MASK) == USB_OK);
    assert(usb_xhci_host_adapter_root_port_ack_changes(&adapter, 1u, first_ack) == USB_OK);
    assert(usb_xhci_host_adapter_root_port_get_status(&adapter, 1u, &status) == USB_OK &&
           status.changes == 0u && (m.regs[PORTSC / 4u] & stable) == stable);
    assert(m.port_writes == writes + 1u);
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_host_adapter_release_stopped(&adapter) == USB_OK);
    assert(m.allocations == m.releases && !lock_state.held);
    free(c);

    /* Read failure preserves output and fail-stops controller ownership. */
    init_model(&m, 0); c = new_controller(); assert(start(&m, c) == USB_OK);
    status = sentinel;
    m.fail_access = m.accesses + 2u; /* Ready check succeeds; PORTSC read fails. */
    assert(usb_xhci_root_port_get_status(c, 1u, &status) == USB_IO);
    assert(c->state == USB_XHCI_CONTROLLER_FAULTED &&
           status.connected == sentinel.connected && status.enabled == sentinel.enabled &&
           status.overcurrent == sentinel.overcurrent && status.resetting == sentinel.resetting &&
           status.powered == sentinel.powered && status.speed_id == sentinel.speed_id &&
           status.changes == sentinel.changes);
    m.fail_access = 0;
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* Applied-but-error RW1C publication still requires a controller stop. */
    init_model(&m, 0); c = new_controller(); assert(start(&m, c) == USB_OK);
    m.regs[PORTSC / 4u] |= PORTSC_CSC;
    m.fail_access = m.accesses + 3u; m.applied_error = true;
    assert(usb_xhci_root_port_ack_changes(c, 1u,
                                          USB_XHCI_ROOT_PORT_CHANGE_CONNECT) == USB_IO);
    assert(c->state == USB_XHCI_CONTROLLER_FAULTED && !(m.regs[PORTSC / 4u] & PORTSC_CSC));
    m.fail_access = 0; m.applied_error = false;
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);
    puts("PASS: xHCI guarded root-port snapshot and selected change acknowledgement");
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

static void test_noncontrol_endpoints(void)
{
    struct model m; init_model(&m, 0); m.auto_commands = true; m.auto_control = true;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_device_slot slot = {0}; m.slot = &slot;
    struct usb_xhci_route route = {.root_port = 1};
    bool cleanup = false;
    assert(usb_xhci_device_prepare(&slot, c, &route, USB_XHCI_SPEED_HIGH,
                                   &cleanup) == USB_OK && cleanup);
    uint8_t address = 0;
    assert(usb_xhci_device_address(&slot, 64u, &address) == USB_OK && address == 1u);

    struct usb_endpoint bulk_out = {0x02, 2, 8, 512, 0, 1};
    struct usb_endpoint bulk_in = {0x82, 2, 8, 512, 0, 1};
    struct usb_endpoint interrupt_in = {0x81, 3, 4, 8, 0, 1};
    struct usb_endpoint high_bandwidth = {0x83, 3, 1, 1024, 0, 3};
    struct usb_endpoint invalid = {0x84, 3, 0, 8, 0, 1};
    assert(usb_xhci_device_configure_endpoint(&slot, &invalid) == USB_INVALID);
    assert(usb_xhci_device_configure_endpoint(&slot, &bulk_out) == USB_OK);
    assert(usb_xhci_device_configure_endpoint(&slot, &bulk_in) == USB_OK);
    assert(usb_xhci_device_configure_endpoint(&slot, &interrupt_in) == USB_OK);
    assert(usb_xhci_device_configure_endpoint(&slot, &high_bandwidth) == USB_OK);
    assert(m.configure_endpoint_commands == 4u);
    uint8_t *contexts = slot.output_context.cpu;
    assert(((get_le32(contexts) >> 27) & 0x1fu) == 7u);
    assert(((get_le32(contexts + 4u * 64u) >> 16) & 0xffu) == 8u);
    uint32_t bulk_out_info = get_le32(contexts + 4u * 64u + 4u);
    assert((bulk_out_info & 0x3fu) == ((3u << 1) | (2u << 3)));
    assert((bulk_out_info >> 16) == 512u);
    uint32_t interrupt_info = get_le32(contexts + 3u * 64u);
    assert(((interrupt_info >> 16) & 0xffu) == 3u);
    uint32_t high_bandwidth_info = get_le32(contexts + 7u * 64u + 4u);
    assert(((high_bandwidth_info >> 8) & 0xffu) == 2u);
    assert(((high_bandwidth_info >> 16) & 0xffffu) == 1024u);
    assert((get_le32(contexts + 7u * 64u + 16u) >> 16) == 3072u);

    m.auto_endpoint = true;
    const uint8_t out_data[] = {0x11, 0x22, 0x33, 0x44};
    m.expected_out_data = out_data; m.expected_out_size = sizeof(out_data);
    uint8_t actual_buffer[16]; size_t actual = 99;
    m.allow_live_release = true; /* The completed TD releases its private bounce buffer. */
    assert(usb_xhci_device_bulk(&slot, address, 0x02, (void *)out_data,
                                sizeof(out_data), &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == sizeof(out_data) && m.endpoint_output_matches);
    m.expected_out_data = NULL; m.expected_out_size = 0;

    m.endpoint_completion_code = USB_XHCI_CC_SHORT_PACKET;
    m.endpoint_residual = 2u; m.endpoint_pattern_start = 0x30u;
    memset(actual_buffer, 0xee, sizeof(actual_buffer)); actual = 99;
    m.allow_live_release = true;
    assert(usb_xhci_device_bulk(&slot, address, 0x82, actual_buffer, 8u,
                                &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == 6u && actual_buffer[0] == 0x30u && actual_buffer[5] == 0x35u &&
           actual_buffer[6] == 0xeeu);

    queue_model_event(&m, USB_XHCI_TRB_PORT_STATUS_EVENT, 1u << 24,
                      USB_XHCI_CC_SUCCESS, 0);
    m.endpoint_residual = 3u; m.endpoint_pattern_start = 0xa0u;
    memset(actual_buffer, 0xee, sizeof(actual_buffer)); actual = 99;
    m.allow_live_release = true;
    assert(usb_xhci_device_interrupt(&slot, address, 0x81, actual_buffer, 8u,
                                     &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == 5u && actual_buffer[0] == 0xa0u && actual_buffer[4] == 0xa4u &&
           actual_buffer[5] == 0xeeu);
    struct usb_xhci_event preserved;
    assert(usb_xhci_controller_next_event(c, &preserved) == USB_OK &&
           preserved.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE);

    m.endpoint_completion_code = USB_XHCI_CC_STALL_ERROR;
    m.endpoint_residual = 2u;
    m.allow_live_release = true;
    assert(usb_xhci_device_interrupt(&slot, address, 0x81, actual_buffer, 8u,
                                     &actual, 100u) == USB_STALL);
    m.allow_live_release = false;
    struct usb_xhci_device_endpoint *endpoint = &slot.endpoints[3u - 2u];
    assert(actual == 6u && actual_buffer[0] == 0xa0u && actual_buffer[5] == 0xa5u &&
           endpoint->halted && !endpoint->transfer_pending &&
           endpoint->ring.outstanding == 0 && c->state == USB_XHCI_CONTROLLER_RUNNING);
    assert(usb_xhci_device_reset_endpoint(&slot, address, 0x81, 100u) == USB_OK);
    assert(m.reset_endpoint_commands == 1u && m.last_command_type == USB_XHCI_TRB_RESET_ENDPOINT);
    assert(((m.last_command_control >> 16) & 0x1fu) == endpoint->dci &&
           !(m.last_command_control & (1u << 9)) && !endpoint->halted && endpoint->needs_dequeue);

    struct usb_setup clear_halt = {0x02, 1, 0, 0x81, 0};
    assert(usb_xhci_device_control(&slot, address, &clear_halt, NULL, 0,
                                   &actual, 100u) == USB_OK && actual == 0);
    uint64_t expected_dequeue = (endpoint->ring.trbs.address +
                                 (uint64_t)endpoint->ring.consumer_index * USB_XHCI_TRB_BYTES) |
                                endpoint->ring.consumer_cycle;
    m.endpoint_completion_code = USB_XHCI_CC_SUCCESS;
    m.endpoint_residual = 0;
    m.allow_live_release = true;
    assert(usb_xhci_device_interrupt(&slot, address, 0x81, actual_buffer, 8u,
                                     &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(m.set_dequeue_commands == 1u && !endpoint->needs_dequeue &&
           m.last_set_dequeue_order < m.last_endpoint_doorbell_order);
    assert(m.last_command_type == USB_XHCI_TRB_SET_TR_DEQUEUE &&
           m.last_command_dword0 == (uint32_t)expected_dequeue);

    /* Complete the 15-TRB ring lap and reset a stalled endpoint at DCS=0. */
    for (unsigned i = 0; i < 12u; ++i) {
        actual = 99;
        m.allow_live_release = true;
        assert(usb_xhci_device_interrupt(&slot, address, 0x81, actual_buffer, 8u,
                                         &actual, 100u) == USB_OK);
        m.allow_live_release = false;
        assert(actual == 8u);
    }
    assert(endpoint->ring.consumer_index == 0u && endpoint->ring.consumer_cycle == 0u &&
           endpoint->ring.producer_index == 0u && endpoint->ring.producer_cycle == 0u);
    m.endpoint_completion_code = USB_XHCI_CC_STALL_ERROR;
    m.endpoint_residual = 2u;
    m.allow_live_release = true;
    assert(usb_xhci_device_interrupt(&slot, address, 0x81, actual_buffer, 8u,
                                     &actual, 100u) == USB_STALL);
    m.allow_live_release = false;
    assert(actual == 6u && endpoint->halted);
    assert(usb_xhci_device_reset_endpoint(&slot, address, 0x81, 100u) == USB_OK);
    assert(usb_xhci_device_control(&slot, address, &clear_halt, NULL, 0,
                                   &actual, 100u) == USB_OK && actual == 0);
    expected_dequeue = (endpoint->ring.trbs.address +
                        (uint64_t)endpoint->ring.consumer_index * USB_XHCI_TRB_BYTES) |
                       endpoint->ring.consumer_cycle;
    m.endpoint_completion_code = USB_XHCI_CC_SUCCESS;
    m.endpoint_residual = 0u;
    m.allow_live_release = true;
    assert(usb_xhci_device_interrupt(&slot, address, 0x81, actual_buffer, 8u,
                                     &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(m.set_dequeue_commands == 2u && !endpoint->needs_dequeue &&
           m.last_command_type == USB_XHCI_TRB_SET_TR_DEQUEUE &&
           m.last_command_dword0 == (uint32_t)expected_dequeue &&
           !(m.last_command_dword0 & 1u));

    m.allow_live_release = true;
    assert(usb_xhci_device_disable(&slot, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(usb_xhci_controller_stop(c) == USB_OK && m.allocations == m.releases);
    free(c);

    /* A timed-out Normal TRB retains its bounce buffer until controller stop. */
    init_model(&m, 0); m.auto_commands = true;
    c = new_controller(); assert(start(&m, c) == USB_OK);
    struct usb_xhci_device_slot timed = {0}; m.slot = &timed;
    assert(usb_xhci_device_prepare(&timed, c, &route, USB_XHCI_SPEED_HIGH,
                                   &cleanup) == USB_OK);
    assert(usb_xhci_device_address(&timed, 64u, &address) == USB_OK);
    assert(usb_xhci_device_configure_endpoint(&timed, &interrupt_in) == USB_OK);
    uint8_t timed_buffer[8] = {0};
    assert(usb_xhci_device_interrupt(&timed, address, 0x81, timed_buffer,
                                     sizeof(timed_buffer), &actual, 1u) == USB_TIMEOUT);
    assert(c->state == USB_XHCI_CONTROLLER_FAULTED &&
           timed.endpoints[3u - 2u].transfer_pending &&
           timed.endpoints[3u - 2u].transfer_data.cpu && !m.releases);
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_device_release_stopped(&timed) == USB_OK &&
           m.allocations == m.releases);
    free(c);
    puts("PASS: xHCI USB2 non-control endpoint contexts, transfers, STALL recovery and retained timeout");
}

static void test_xhci_host_adapter(void)
{
    struct model m; init_model(&m, 0); m.auto_commands = true; m.auto_control = true;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_host_device devices[2];
    memset(devices, 0, sizeof(devices));
    struct adapter_lock_state lock_state = {0};
    uint32_t clock_ms = 1234u;
    struct usb_xhci_host_adapter adapter;
    assert(usb_xhci_host_adapter_init(&adapter, c, devices, 2u,
                                     adapter_test_lock, adapter_test_unlock, &lock_state,
                                     adapter_test_clock, &clock_ms, 100u) == USB_OK);
    struct usb_host_ops ops = {0};
    assert(usb_xhci_host_adapter_get_ops(&adapter, &ops) == USB_OK);
    assert(ops.prepare_device && ops.address_device && ops.control && ops.configure_hub &&
           ops.configure_endpoint && ops.interrupt && ops.bulk && ops.reset_endpoint &&
           ops.disable_device && ops.now_ms && ops.now_ms(ops.context) == clock_ms);

    struct usb_host_route root_route = {.root_port = 1u};
    bool cleanup = false;
    m.slot = &devices[0].slot;
    assert(ops.prepare_device(ops.context, &root_route, USB_HOST_HIGH_SPEED, &cleanup) == USB_OK);
    assert(cleanup && devices[0].owned && devices[0].slot.state == USB_XHCI_SLOT_DEFAULT);
    uint8_t hub_address = 0;
    assert(ops.address_device(ops.context, &root_route, USB_HOST_HIGH_SPEED, 64u, 55u,
                              &hub_address) == USB_OK && hub_address == 1u);
    assert(ops.configure_hub(ops.context, &root_route, hub_address, USB_HOST_HIGH_SPEED,
                             16u, 1332u, true) == USB_OK);
    assert(devices[0].slot.is_hub && devices[0].slot.hub_port_count == 16u);

    enum usb_host_speed root_speed = USB_HOST_LOW_SPEED;
    assert(usb_xhci_host_adapter_reset_root_port(&adapter, 1u, 100u, &root_speed) == USB_OK &&
           root_speed == USB_HOST_HIGH_SPEED);
    struct usb_xhci_event event;
    assert(usb_xhci_host_adapter_next_event(&adapter, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE && event.data.port_status.port_id == 1u);
    queue_model_event(&m, USB_XHCI_TRB_PORT_STATUS_EVENT, 2u << 24, USB_XHCI_CC_SUCCESS, 0);
    assert(usb_xhci_host_adapter_next_event(&adapter, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE && event.data.port_status.port_id == 2u);

    struct usb_host_route child_route = {
        .root_port = 1u, .depth = 1u, .hub_address = {1u}, .hub_port = {16u},
        .tt_hub_address = 1u, .tt_port = 16u, .tt_think_time_ns = 1332u, .multi_tt = true
    };
    m.slot = &devices[1].slot;
    cleanup = false;
    assert(ops.prepare_device(ops.context, &child_route, USB_HOST_FULL_SPEED, &cleanup) == USB_OK);
    assert(cleanup && devices[1].owned && devices[1].route.depth == 1u &&
           devices[1].slot.route.depth == 1u && devices[1].slot.route.hub_slot[0] == 1u &&
           devices[1].slot.route.hub_port[0] == 16u &&
           devices[1].slot.route.hub_port_count[0] == 16u &&
           devices[1].slot.route.tt_hub_slot == 1u && devices[1].slot.route.tt_port == 16u &&
           devices[1].slot.route.multi_tt);
    m.device_descriptor[7] = 8u;
    struct usb_setup get_descriptor = {0x80, 6, 0x100, 0, 8};
    uint8_t descriptor[8] = {0}; size_t actual = 99u;
    m.allow_live_release = true;
    assert(ops.control(ops.context, &child_route, 0u, &get_descriptor, descriptor,
                       sizeof(descriptor), &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == sizeof(descriptor) && descriptor[0] == 18u && descriptor[7] == 8u);
    uint8_t child_address = 0;
    assert(ops.address_device(ops.context, &child_route, USB_HOST_FULL_SPEED, 8u, 56u,
                              &child_address) == USB_OK && child_address == 2u);

    queue_model_event(&m, USB_XHCI_TRB_PORT_STATUS_EVENT, 3u << 24, USB_XHCI_CC_SUCCESS, 0);
    actual = 99u;
    m.allow_live_release = true;
    assert(ops.control(ops.context, &child_route, child_address, &get_descriptor, descriptor,
                       sizeof(descriptor), &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == sizeof(descriptor) && descriptor[0] == 18u && descriptor[7] == 8u);
    /* Queued owner events remain drainable after a synchronous operation faults the controller. */
    c->state = USB_XHCI_CONTROLLER_FAULTED;
    assert(usb_xhci_host_adapter_next_event(&adapter, &event) == USB_OK &&
           event.kind == USB_XHCI_EVENT_PORT_STATUS_CHANGE && event.data.port_status.port_id == 3u);
    c->state = USB_XHCI_CONTROLLER_RUNNING;

    struct usb_endpoint bulk_out = {0x02, 2, 0, 64, 0, 1};
    struct usb_endpoint interrupt_in = {0x81, 3, 10, 8, 0, 1};
    assert(ops.configure_endpoint(ops.context, &child_route, child_address,
                                  USB_HOST_FULL_SPEED, &bulk_out) == USB_OK);
    assert(ops.configure_endpoint(ops.context, &child_route, child_address,
                                  USB_HOST_FULL_SPEED, &interrupt_in) == USB_OK);
    m.auto_endpoint = true;
    const uint8_t out_data[] = {0x11, 0x22, 0x33, 0x44};
    m.expected_out_data = out_data; m.expected_out_size = sizeof(out_data);
    m.allow_live_release = true;
    assert(ops.bulk(ops.context, &child_route, child_address, 0x02, (void *)out_data,
                    sizeof(out_data), &actual, 100u) == USB_OK);
    m.allow_live_release = false;
    assert(actual == sizeof(out_data) && m.endpoint_output_matches);
    m.expected_out_data = NULL; m.expected_out_size = 0;

    uint8_t input[8] = {0};
    m.endpoint_completion_code = USB_XHCI_CC_STALL_ERROR;
    m.endpoint_residual = 2u;
    m.allow_live_release = true;
    assert(ops.interrupt(ops.context, &child_route, child_address, 0x81, input, sizeof(input),
                         &actual, 100u) == USB_STALL);
    m.allow_live_release = false;
    assert(actual == 6u && devices[1].slot.endpoints[3u - 2u].halted);
    assert(ops.reset_endpoint(ops.context, &child_route, child_address, 0x81) == USB_OK);
    struct usb_setup clear_halt = {0x02, 1, 0, 0x81, 0};
    m.allow_live_release = true;
    assert(ops.control(ops.context, &child_route, child_address, &clear_halt, NULL, 0,
                       &actual, 100u) == USB_OK && actual == 0u);
    m.allow_live_release = false;
    m.endpoint_completion_code = USB_XHCI_CC_SUCCESS;
    m.endpoint_residual = 0u;
    m.allow_live_release = true;
    assert(ops.interrupt(ops.context, &child_route, child_address, 0x81, input, sizeof(input),
                         &actual, 100u) == USB_OK && actual == sizeof(input));
    m.allow_live_release = false;
    assert(m.reset_endpoint_commands == 1u && m.set_dequeue_commands == 1u &&
           m.last_set_dequeue_order < m.last_endpoint_doorbell_order);

    assert(usb_xhci_host_adapter_release_stopped(&adapter) == USB_BUSY);
    m.allow_live_release = true;
    assert(ops.disable_device(ops.context, &child_route, 0u) == USB_OK);
    m.allow_live_release = false;
    assert(!devices[1].owned && devices[1].slot.state == USB_XHCI_SLOT_EMPTY);
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_host_adapter_release_stopped(&adapter) == USB_OK);
    assert(!devices[0].owned && devices[0].slot.state == USB_XHCI_SLOT_EMPTY &&
           !devices[1].owned && m.allocations == m.releases);
    assert(!lock_state.held && lock_state.acquisitions == lock_state.releases);
    free(c);
    puts("PASS: xHCI usb_host_ops adapter routes, transfers and stopped cleanup");
}

static void test_xhci_host_enumeration_bot(void)
{
    struct model m; init_model(&m, 0); m.auto_commands = true;
    m.auto_control = true; m.host_stack_enumeration = true;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);

    struct usb_xhci_host_device slots[1]; memset(slots, 0, sizeof(slots));
    struct adapter_lock_state lock_state = {0};
    uint32_t clock_ms = 1u;
    struct usb_xhci_host_adapter adapter;
    assert(usb_xhci_host_adapter_init(&adapter, c, slots, 1u,
                                     adapter_test_lock, adapter_test_unlock, &lock_state,
                                     adapter_test_clock, &clock_ms, 100u) == USB_OK);
    struct usb_host_ops ops = {0};
    assert(usb_xhci_host_adapter_get_ops(&adapter, &ops) == USB_OK);
    struct usb_host_route route = {.root_port = 1u};
    struct usb_host_device device = {0};
    m.slot = &slots[0].slot;
    m.allow_live_release = true; /* Transfers below complete before their bounce buffers are released. */
    assert(usb_host_enumerate(&device, &ops, &route, USB_HOST_HIGH_SPEED, 5u) == USB_OK);
    assert(device.state == USB_HOST_CONFIGURED && device.address == 1u &&
           device.configuration == 1u && device.has_bot && device.bot.present);
    assert(device.bot.in_endpoint == 0x81u && device.bot.out_endpoint == 0x02u &&
           device.bot.block_size == 0u);
    assert(m.host_control_requests == 5u && m.host_get_device8 == 1u &&
           m.host_get_device18 == 1u && m.host_get_config_header == 1u &&
           m.host_get_config == 1u && m.host_set_config == 1u);
    assert(m.configure_endpoint_commands == 2u && slots[0].owned &&
           slots[0].slot.state == USB_XHCI_SLOT_ADDRESSED);

    m.auto_endpoint = true;
    assert(usb_host_start_storage(&device) == USB_OK &&
           device.bot.blocks == 8u && device.bot.block_size == 512u);
    uint8_t written[512], readback[512];
    for (size_t i = 0; i < sizeof(written); ++i) written[i] = (uint8_t)(i ^ 0xa7u);
    assert(usb_bot_write(&device.bot, 2u, 1u, written, sizeof(written)) == USB_OK);
    assert(usb_bot_sync(&device.bot) == USB_OK);
    memset(readback, 0, sizeof(readback));
    assert(usb_bot_read(&device.bot, 2u, 1u, readback, sizeof(readback)) == USB_OK);
    assert(!memcmp(written, readback, sizeof(written)) && m.bot_commands == 4u &&
           m.bot_sync_commands == 1u && m.bot_phase == 0u);

    assert(usb_host_disconnect(&device, NULL, NULL, NULL) == USB_OK);
    m.allow_live_release = false;
    assert(device.state == USB_HOST_EMPTY && !slots[0].owned);
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_host_adapter_release_stopped(&adapter) == USB_OK);
    assert(m.allocations == m.releases && !lock_state.held &&
           lock_state.acquisitions == lock_state.releases);
    free(c);
    puts("PASS: xHCI controller + host enumeration + BOT capacity/read/write/sync");
}

struct xhci_hub_bot_context {
    struct model *model;
    struct usb_host_ops ops;
    struct usb_xhci_host_device *slots;
    struct usb_host_device child;
    struct usb_host_route child_route;
    enum usb_host_speed child_speed;
    unsigned attaches, detaches;
};

static int enumerate_hub_child(void *context, enum usb_host_hub_event_kind kind,
                               uint8_t port, const struct usb_host_route *route,
                               enum usb_host_speed speed)
{
    struct xhci_hub_bot_context *test = context;
    assert(test && test->model && test->slots && route && port == 1u);
    if (kind == USB_HOST_HUB_ATTACH) {
        assert(test->child.state == USB_HOST_EMPTY && speed == USB_HOST_FULL_SPEED);
        test->model->slot = &test->slots[1].slot;
        test->model->pending_slot_is_hub = false;
        test->child_route = *route;
        test->child_speed = speed;
        int result = usb_host_enumerate(&test->child, &test->ops, route, speed, 2u);
        if (!result) ++test->attaches;
        return result;
    }
    if (kind == USB_HOST_HUB_DETACH || kind == USB_HOST_HUB_OVERCURRENT) {
        ++test->detaches;
        return usb_host_disconnect(&test->child, NULL, NULL, NULL);
    }
    return USB_INVALID;
}

static void test_xhci_hub_enumeration_child_bot(void)
{
    struct model m; init_model(&m, 0);
    m.auto_commands = true; m.auto_control = true; m.host_stack_enumeration = true;
    m.hub_port_status = 0x0001u; m.hub_port_change = 0x0001u;
    m.hub_interrupt_bitmap[0] = 0x02u; /* Port one changed. */
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);

    struct usb_xhci_host_device slots[2]; memset(slots, 0, sizeof(slots));
    struct adapter_lock_state lock_state = {0};
    struct usb_xhci_host_adapter adapter;
    assert(usb_xhci_host_adapter_init(&adapter, c, slots, 2u,
                                     adapter_test_lock, adapter_test_unlock, &lock_state,
                                     adapter_test_clock, &m.current_ms, 100u) == USB_OK);
    struct xhci_hub_bot_context test = {0};
    test.model = &m; test.slots = slots;
    assert(usb_xhci_host_adapter_get_ops(&adapter, &test.ops) == USB_OK);
    struct usb_host_route root_route = {.root_port = 1u};
    enum usb_host_speed root_speed = USB_HOST_LOW_SPEED;
    assert(usb_xhci_host_adapter_reset_root_port(&adapter, 1u, 100u, &root_speed) == USB_OK &&
           root_speed == USB_HOST_HIGH_SPEED);

    struct usb_host_device hub_device = {0}; struct usb_host_hub hub = {0};
    m.slot = &slots[0].slot; m.pending_slot_is_hub = true;
    m.allow_live_release = true;
    assert(usb_host_enumerate(&hub_device, &test.ops, &root_route,
                              root_speed, 1u) == USB_OK);
    assert(hub_device.state == USB_HOST_CONFIGURED && hub_device.hub_interface.present &&
           hub_device.device_descriptor[4] == 9u && hub_device.address == 1u &&
           slots[0].slot.state == USB_XHCI_SLOT_ADDRESSED);
    assert(usb_host_hub_start(&hub_device, &hub) == USB_OK);
    assert(hub.initialized && hub.port_count == 1u && !hub.multi_tt &&
           hub.tt_think_time_ns == 1332u && m.hub_power_sets == 1u &&
           slots[0].slot.is_hub && slots[0].slot.hub_port_count == 1u);

    m.auto_endpoint = true;
    m.current_ms = 20u;
    assert(usb_host_hub_poll(&hub_device, &hub, 10u,
                             enumerate_hub_child, &test) == USB_AGAIN);
    assert(m.hub_port_resets == 1u && !test.attaches && !test.child.address);
    m.current_ms = 70u;
    assert(usb_host_hub_poll(&hub_device, &hub, 10u,
                             enumerate_hub_child, &test) == USB_OK);
    assert(test.attaches == 1u && hub.ports[0].attached && test.child.state == USB_HOST_CONFIGURED &&
           test.child.has_bot && test.child.bot.present && test.child.address == 2u &&
           test.child_speed == USB_HOST_FULL_SPEED);
    assert(test.child_route.depth == 1u && test.child_route.hub_address[0] == hub_device.address &&
           test.child_route.hub_port[0] == 1u &&
           test.child_route.tt_hub_address == hub_device.address &&
           test.child_route.tt_port == 1u && test.child_route.tt_think_time_ns == 1332u &&
           !test.child_route.multi_tt);
    assert(slots[1].route.depth == 1u && slots[1].slot.route.depth == 1u &&
           slots[1].slot.route.hub_slot[0] == slots[0].slot.slot_id &&
           slots[1].slot.route.hub_port[0] == 1u &&
           slots[1].slot.route.tt_hub_slot == slots[0].slot.slot_id &&
           slots[1].slot.route.tt_port == 1u);
    const uint8_t *child_input_slot = (const uint8_t *)slots[1].slot.input_context.cpu +
                                      c->caps.context_bytes;
    uint32_t child_slot_info = get_le32(child_input_slot);
    assert((child_slot_info & 0x000fffffu) == 1u &&
           ((child_slot_info >> 20) & 0x0fu) == USB_XHCI_SPEED_FULL &&
           !(child_slot_info & (1u << 25)));
    assert(get_le32(child_input_slot + 4u) == (1u << 16));
    assert((get_le32(child_input_slot + 8u) & 0x0000ffffu) ==
           ((uint32_t)slots[0].slot.slot_id | (1u << 8)));

    m.stall_next_bot_transfer = true;
    assert(usb_host_start_storage(&test.child) == USB_STALL);
    assert(!m.stall_next_bot_transfer);
    assert(m.bot_stalls == 1u && m.bot_class_resets == 1u);
    assert(m.reset_endpoint_commands == 1u);
    assert(m.bot_tt_buffer_clears == 2u && m.bot_clear_features == 2u);
    assert(m.bot_tt_clear_value[0] == 0x9021u && m.bot_tt_clear_index[0] == 1u);
    assert(m.bot_tt_clear_value[1] == 0x1022u && m.bot_tt_clear_index[1] == 1u);
    assert(m.bot_clear_feature_endpoint[0] == 0x81u);
    assert(m.bot_clear_feature_endpoint[1] == 0x02u && m.bot_phase == 0u);
    assert(usb_host_start_storage(&test.child) == USB_OK &&
           test.child.bot.blocks == 8u && test.child.bot.block_size == 512u);
    uint8_t written[512], readback[512];
    for (size_t i = 0; i < sizeof(written); ++i) written[i] = (uint8_t)(i ^ 0x6du);
    assert(usb_bot_write(&test.child.bot, 2u, 1u, written, sizeof(written)) == USB_OK);
    assert(usb_bot_sync(&test.child.bot) == USB_OK);
    memset(readback, 0, sizeof(readback));
    assert(usb_bot_read(&test.child.bot, 2u, 1u, readback, sizeof(readback)) == USB_OK);
    assert(!memcmp(written, readback, sizeof(written)) && m.bot_commands == 4u &&
           m.bot_sync_commands == 1u && m.bot_phase == 0u && m.hub_interrupts == 1u);

    assert(usb_host_hub_stop(&hub_device, &hub, enumerate_hub_child, &test) == USB_OK);
    assert(test.detaches == 1u && test.child.state == USB_HOST_EMPTY && !slots[1].owned &&
           m.hub_power_clears == 1u && !hub.initialized);
    assert(usb_host_disconnect(&hub_device, NULL, NULL, NULL) == USB_OK);
    m.allow_live_release = false;
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_xhci_host_adapter_release_stopped(&adapter) == USB_OK);
    assert(m.allocations == m.releases && !slots[0].owned && !slots[1].owned &&
           !lock_state.held && lock_state.acquisitions == lock_state.releases);
    free(c);
    puts("PASS: xHCI controller + hub TT child enumeration + BOT storage lifecycle");
}

static void test_xhci_host_adapter_cleanup_retention(void)
{
    struct model m; init_model(&m, 0);
    m.auto_commands = true; m.auto_control = true; m.host_stack_enumeration = true;
    struct usb_xhci_controller *c = new_controller();
    assert(start(&m, c) == USB_OK);
    struct usb_xhci_host_device devices[1];
    memset(devices, 0, sizeof(devices));
    struct adapter_lock_state lock_state = {0};
    struct usb_xhci_host_adapter adapter;
    assert(usb_xhci_host_adapter_init(&adapter, c, devices, 1u,
                                     adapter_test_lock, adapter_test_unlock, &lock_state,
                                     NULL, NULL, 100u) == USB_OK);
    struct usb_host_ops ops = {0};
    assert(usb_xhci_host_adapter_get_ops(&adapter, &ops) == USB_OK && !ops.now_ms &&
           ops.controller_stopped);
    struct usb_host_route route = {.root_port = 1u};
    m.slot = &devices[0].slot;
    struct usb_host_device host_device = {0};
    m.allow_live_release = true;
    assert(usb_host_enumerate(&host_device, &ops, &route,
                              USB_HOST_HIGH_SPEED, 1u) == USB_OK);
    assert(host_device.state == USB_HOST_CONFIGURED && devices[0].owned);

    m.allow_live_release = false;
    m.fail_access = m.accesses + 2u; /* Apply the Disable Slot doorbell, then report I/O failure. */
    m.applied_error = true;
    assert(usb_host_stop_device(&host_device) == USB_IO);
    assert(devices[0].owned && devices[0].slot.state == USB_XHCI_SLOT_ADDRESSED &&
           c->state == USB_XHCI_CONTROLLER_FAULTED &&
           host_device.state == USB_HOST_FAULTED && !host_device.stop_proven);
    assert(usb_host_confirm_controller_stopped(&host_device) == USB_BUSY &&
           !host_device.stop_proven);
    assert(usb_host_release_stopped_device(&host_device, NULL, NULL, NULL) == USB_BUSY);

    m.fail_access = 0;
    m.applied_error = false;
    m.stuck_halt = true;
    assert(usb_xhci_controller_stop(c) == USB_TIMEOUT && !host_device.stop_proven);
    assert(usb_host_confirm_controller_stopped(&host_device) == USB_BUSY &&
           usb_host_release_stopped_device(&host_device, NULL, NULL, NULL) == USB_BUSY);
    m.stuck_halt = false;
    assert(usb_xhci_controller_stop(c) == USB_OK);
    assert(usb_host_confirm_controller_stopped(&host_device) == USB_OK &&
           host_device.stop_proven);
    assert(usb_host_release_stopped_device(&host_device, NULL, NULL, NULL) == USB_OK &&
           host_device.state == USB_HOST_EMPTY);
    assert(usb_xhci_host_adapter_release_stopped(&adapter) == USB_OK);
    assert(!devices[0].owned && devices[0].slot.state == USB_XHCI_SLOT_EMPTY &&
           m.allocations == m.releases);
    assert(!lock_state.held && lock_state.acquisitions == lock_state.releases);
    free(c);
    puts("PASS: host state releases only after retryable controller-wide stop proof");
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
    test_control_transfer(); test_device_slots_and_routing(); test_noncontrol_endpoints();
    test_xhci_host_adapter(); test_xhci_host_enumeration_bot();
    test_xhci_hub_enumeration_child_bot();
    test_xhci_host_adapter_cleanup_retention();
    test_root_port_status();
    test_root_port_reset();
    puts("PASS: xHCI production controller lifecycle model; hardware_qualified=false");
    return 0;
}
#else
struct usb_xhci_test_host {
    struct model model;
    struct usb_xhci_controller *controller;
    struct usb_xhci_host_device devices[1];
    struct usb_xhci_host_adapter adapter;
    struct adapter_lock_state lock_state;
    uint32_t clock_ms;
    bool controller_started;
};

struct usb_xhci_test_host *usb_xhci_test_host_create_with_mmio(
    uint8_t *media, size_t media_size, const uint8_t keyboard_report[8],
    const struct usb_dma_ops *dma_ops, const struct usb_mmio *platform_mmio)
{
    if (!media || media_size < 512u || media_size % 512u ||
        media_size / 512u > UINT32_MAX || !keyboard_report || !dma_ops ||
        !dma_ops->qualified || !dma_ops->allocate || !dma_ops->release || !dma_ops->sync ||
        (platform_mmio && (!platform_mmio->context || platform_mmio->size < 0x1000u ||
                           !platform_mmio->read32 || !platform_mmio->write32 ||
                           !platform_mmio->barrier || !platform_mmio->delay_us)))
        return NULL;
    struct usb_xhci_test_host *host = calloc(1, sizeof(*host));
    if (!host) return NULL;
    init_model(&host->model, 0);
    if (platform_mmio) {
        host->model.platform_mmio = *platform_mmio;
        host->model.platform_mmio_bound = true;
    }
    host->model.auto_commands = true;
    host->model.auto_control = true;
    host->model.auto_endpoint = true;
    host->model.host_stack_enumeration = true;
    host->model.composite_hid_bot = true;
    host->model.bot_media = media;
    host->model.bot_media_size = media_size;
    host->model.bot_in_endpoint = 0x83u;
    host->model.bot_out_endpoint = 0x03u;
    memcpy(host->model.hid_input_report, keyboard_report,
           sizeof(host->model.hid_input_report));
    host->model.slot = &host->devices[0].slot;
    host->model.allow_live_release = true;
    host->controller = new_controller();
    if (start_with_dma(&host->model, host->controller, dma_ops) != USB_OK) {
        host->model.allow_live_release = false;
        if (usb_xhci_controller_stop(host->controller) == USB_OK)
            free(host->controller);
        free(host);
        return NULL;
    }
    host->controller_started = true;
    host->clock_ms = 1u;
    int result = usb_xhci_host_adapter_init(&host->adapter, host->controller,
        host->devices, 1u, adapter_test_lock, adapter_test_unlock, &host->lock_state,
        adapter_test_clock, &host->clock_ms, 100u);
    if (result != USB_OK) {
        host->model.allow_live_release = false;
        if (usb_xhci_controller_stop(host->controller) == USB_OK) {
            host->controller_started = false;
            free(host->controller);
        }
        free(host);
        return NULL;
    }
    return host;
}

struct usb_xhci_test_host *usb_xhci_test_host_create(
    uint8_t *media, size_t media_size, const uint8_t keyboard_report[8],
    const struct usb_dma_ops *dma_ops)
{
    return usb_xhci_test_host_create_with_mmio(media, media_size, keyboard_report,
                                               dma_ops, NULL);
}

int usb_xhci_test_host_get_ops(struct usb_xhci_test_host *host,
                               struct usb_host_ops *ops)
{
    if (!host || !host->controller_started || !ops) return USB_INVALID;
    return usb_xhci_host_adapter_get_ops(&host->adapter, ops);
}

int usb_xhci_test_host_get_counters(struct usb_xhci_test_host *host,
                                    struct usb_xhci_test_host_counters *out)
{
    if (!host || !out) return USB_INVALID;
    struct usb_xhci_test_host_counters counters = {
        .host_control_requests = host->model.host_control_requests,
        .hid_protocol_selections = host->model.hid_protocol_selections,
        .hid_interrupts = host->model.hid_interrupts,
        .bot_capacity_commands = host->model.bot_capacity_commands,
        .bot_read_only_commands = host->model.bot_read_only_commands,
        .bot_read_commands = host->model.bot_read_commands,
        .bot_sync_commands = host->model.bot_sync_commands,
        .bot_write_commands = host->model.bot_write_commands,
        .controller_state = (unsigned)host->controller->state,
        .owned_slots = host->devices[0].owned ? 1u : 0u
    };
    *out = counters;
    return USB_OK;
}

int usb_xhci_test_host_inject_disable_slot_applied_error(struct usb_xhci_test_host *host)
{
    if (!host || !host->controller_started || !host->devices[0].owned ||
        host->controller->state != USB_XHCI_CONTROLLER_RUNNING) return USB_INVALID;
    host->model.allow_live_release = false;
    host->model.fail_access = host->model.accesses + 2u;
    host->model.applied_error = true;
    return USB_OK;
}

int usb_xhci_test_host_set_stuck_halt(struct usb_xhci_test_host *host, bool stuck)
{
    if (!host || !host->controller_started) return USB_INVALID;
    host->model.fail_access = 0;
    host->model.applied_error = false;
    host->model.stuck_halt = stuck;
    return USB_OK;
}

int usb_xhci_test_host_stop(struct usb_xhci_test_host *host)
{
    if (!host || !host->controller_started) return USB_INVALID;
    host->model.allow_live_release = false;
    int result = usb_xhci_controller_stop(host->controller);
    if (result) return result;
    result = usb_xhci_host_adapter_release_stopped(&host->adapter);
    if (result) return result;
    host->controller_started = false;
    return USB_OK;
}

int usb_xhci_test_host_destroy(struct usb_xhci_test_host *host)
{
    if (!host) return USB_INVALID;
    if (host->controller_started) return USB_BUSY;
    free(host->controller);
    free(host);
    return USB_OK;
}
#endif
