/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbCore.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 512u
#define BLOCKS 16u

struct disk_model {
    uint8_t media[BLOCK_SIZE * BLOCKS];
    uint32_t tag;
    uint32_t transfer_size;
    uint64_t lba;
    uint32_t blocks;
    unsigned phase;
    unsigned commands;
    unsigned data_commands;
    unsigned sync_commands;
    unsigned resets;
    unsigned clears;
    unsigned fail_data_command;
    bool fail_sync_once;
    bool current_sync;
    bool disconnected;
    bool lock_held;
};

struct test_lock { bool held; unsigned attempts; struct disk_model *disk; };
struct user_buffer { uint8_t bytes[BLOCK_SIZE * 8]; size_t length; int fail_copy; unsigned copies; };

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void set32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t getbe32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int lock_io(void *context)
{
    struct test_lock *lock = context;
    ++lock->attempts;
    if (lock->held) return USB_BUSY;
    lock->held = true;
    lock->disk->lock_held = true;
    return USB_OK;
}
static void unlock_io(void *context)
{
    struct test_lock *lock = context;
    assert(lock->held);
    lock->held = false;
    lock->disk->lock_held = false;
}
static int bot_control(void *context, const struct usb_setup *setup, void *data,
                       size_t size, size_t *actual, uint32_t timeout)
{
    struct disk_model *disk = context;
    (void)data;
    assert(disk->lock_held && size == 0 && timeout && setup->type == 0x21 &&
           setup->request == 0xff && setup->index == 3);
    ++disk->resets;
    *actual = 0;
    if (disk->disconnected) return USB_DISCONNECTED;
    disk->phase = 0;
    return USB_OK;
}
static int clear_halt(void *context, uint8_t endpoint)
{
    struct disk_model *disk = context;
    assert(disk->lock_held && (endpoint == 0x81 || endpoint == 0x02));
    ++disk->clears;
    return USB_OK;
}
static int bulk(void *context, uint8_t endpoint, void *buffer, size_t size,
                size_t *actual, uint32_t timeout)
{
    struct disk_model *disk = context;
    uint8_t *bytes = buffer;
    assert(disk->lock_held && timeout);
    *actual = 0;
    if (disk->disconnected) return USB_DISCONNECTED;
    if (disk->phase == 0) {
        assert(endpoint == 0x02 && size == 31 && get32(bytes) == 0x43425355);
        ++disk->commands;
        disk->tag = get32(bytes + 4);
        disk->transfer_size = get32(bytes + 8);
        uint8_t opcode = bytes[15];
        disk->current_sync = opcode == 0x35;
        if (opcode == 0x28 || opcode == 0x2a) {
            disk->lba = getbe32(bytes + 17);
            disk->blocks = ((uint32_t)bytes[22] << 8) | bytes[23];
            assert(disk->blocks && disk->lba < BLOCKS && disk->blocks <= BLOCKS - disk->lba);
            assert(disk->transfer_size == disk->blocks * BLOCK_SIZE);
        } else if (disk->current_sync) {
            assert(!disk->transfer_size);
            ++disk->sync_commands;
        } else {
            assert(0 && "unexpected SCSI command");
        }
        disk->phase = disk->transfer_size ? 1 : 2;
        *actual = size;
        return USB_OK;
    }
    if (disk->phase == 1) {
        ++disk->data_commands;
        assert(size == disk->transfer_size && size <= BLOCK_SIZE * BLOCKS);
        bool write = endpoint == 0x02;
        assert(write || endpoint == 0x81);
        size_t offset = (size_t)disk->lba * BLOCK_SIZE;
        if (write) memcpy(disk->media + offset, bytes, size);
        else memcpy(bytes, disk->media + offset, size);
        disk->phase = 2;
        if (disk->fail_data_command == disk->data_commands) {
            *actual = size / 2;
            return USB_TIMEOUT;
        }
        *actual = size;
        return USB_OK;
    }
    assert(disk->phase == 2 && endpoint == 0x81 && size == 13);
    memset(bytes, 0, size);
    set32(bytes, 0x53425355);
    set32(bytes + 4, disk->tag);
    if (disk->current_sync && disk->fail_sync_once) {
        bytes[12] = 1;
        disk->fail_sync_once = false;
    }
    disk->current_sync = false;
    disk->phase = 0;
    *actual = size;
    return USB_OK;
}

static int copy_to(void *context, uint64_t offset, void *destination, size_t length)
{
    struct user_buffer *buffer = context;
    ++buffer->copies;
    if (buffer->fail_copy == 1 || offset > buffer->length || length > buffer->length - (size_t)offset)
        return USB_IO;
    memcpy(destination, buffer->bytes + (size_t)offset, length);
    return USB_OK;
}
static int copy_from(void *context, uint64_t offset, const void *source, size_t length)
{
    struct user_buffer *buffer = context;
    ++buffer->copies;
    if (buffer->fail_copy == 2 || offset > buffer->length || length > buffer->length - (size_t)offset)
        return USB_IO;
    memcpy(buffer->bytes + (size_t)offset, source, length);
    return USB_OK;
}

static void init(struct disk_model *disk, struct usb_bot *bot, struct usb_block_io *io,
                 struct test_lock *lock, uint8_t *bounce, size_t bounce_size)
{
    lock->disk = disk;
    struct usb_transport transport = {disk, bot_control, bulk, clear_halt};
    assert(usb_bot_init(bot, &transport, 3, 0x81, 0x02, 0) == USB_OK);
    bot->blocks = BLOCKS;
    bot->block_size = BLOCK_SIZE;
    assert(usb_block_io_init(io, bot, bounce, bounce_size, lock, lock_io, unlock_io) == USB_OK);
}

static void test_chunked_read_write(void)
{
    struct disk_model disk = {0};
    struct usb_bot bot;
    struct usb_block_io io;
    struct test_lock lock = {0};
    uint8_t bounce[BLOCK_SIZE * 2] = {0};
    init(&disk, &bot, &io, &lock, bounce, sizeof(bounce));

    for (size_t i = 0; i < sizeof(disk.media); ++i) disk.media[i] = (uint8_t)(i * 17u + 3u);
    struct user_buffer user = {.length = BLOCK_SIZE * 5};
    struct usb_block_buffer buffer = {&user, user.length, copy_to, copy_from};
    uint64_t completed = UINT64_MAX;
    assert(usb_block_io_transfer(&io, false, 2, 5, &buffer, &completed) == USB_OK);
    assert(completed == user.length && disk.data_commands == 3 && lock.attempts == 1 && !lock.held);
    assert(!memcmp(user.bytes, disk.media + 2 * BLOCK_SIZE, user.length));

    for (size_t i = 0; i < user.length; ++i) user.bytes[i] ^= (uint8_t)(0x5au + i);
    memset(disk.media + 4 * BLOCK_SIZE, 0, user.length);
    buffer.length = user.length;
    assert(usb_block_io_transfer(&io, true, 4, 5, &buffer, &completed) == USB_OK);
    assert(completed == user.length && disk.data_commands == 6 && !lock.held);
    assert(!memcmp(user.bytes, disk.media + 4 * BLOCK_SIZE, user.length));
    assert(usb_block_io_sync(&io) == USB_OK && disk.sync_commands == 1 && !lock.held);
}

static void test_failures_and_bounds(void)
{
    struct disk_model disk = {0};
    struct usb_bot bot;
    struct usb_block_io io;
    struct test_lock lock = {0};
    uint8_t bounce[BLOCK_SIZE] = {0};
    init(&disk, &bot, &io, &lock, bounce, sizeof(bounce));
    struct user_buffer user = {.length = BLOCK_SIZE * 3};
    struct usb_block_buffer buffer = {&user, user.length, copy_to, copy_from};
    uint64_t completed;

    assert(usb_block_io_transfer(&io, false, BLOCKS - 1, 2, &buffer, &completed) == USB_RANGE && !completed);
    buffer.length--;
    assert(usb_block_io_transfer(&io, false, 0, 3, &buffer, &completed) == USB_RANGE && !completed);
    buffer.length++;
    lock.held = true;
    assert(usb_block_io_transfer(&io, false, 0, 3, &buffer, &completed) == USB_BUSY && !completed);
    lock.held = false;

    disk.fail_data_command = 2;
    assert(usb_block_io_transfer(&io, false, 0, 3, &buffer, &completed) == USB_TIMEOUT);
    assert(completed == BLOCK_SIZE && disk.data_commands == 2 && !lock.held);

    struct disk_model write_disk = {0};
    struct usb_bot write_bot;
    struct usb_block_io write_io;
    struct test_lock write_lock = {0};
    uint8_t write_bounce[BLOCK_SIZE] = {0};
    init(&write_disk, &write_bot, &write_io, &write_lock, write_bounce, sizeof(write_bounce));
    write_disk.fail_data_command = 2;
    assert(usb_block_io_transfer(&write_io, true, 0, 3, &buffer, &completed) == USB_TIMEOUT);
    assert(completed == BLOCK_SIZE && write_bot.write_uncertain && write_disk.data_commands == 2);

    struct disk_model copy_disk = {0};
    struct usb_bot copy_bot;
    struct usb_block_io copy_io;
    struct test_lock copy_lock = {0};
    uint8_t copy_bounce[BLOCK_SIZE] = {0};
    init(&copy_disk, &copy_bot, &copy_io, &copy_lock, copy_bounce, sizeof(copy_bounce));
    user.fail_copy = 2;
    buffer.length = BLOCK_SIZE;
    assert(usb_block_io_transfer(&copy_io, false, 0, 1, &buffer, &completed) == USB_IO && !completed);
    assert(copy_disk.data_commands == 1 && !copy_lock.held);
}

static void test_quiesce_and_detach(void)
{
    struct disk_model disk = {0};
    struct usb_bot bot;
    struct usb_block_io io;
    struct test_lock lock = {0};
    uint8_t bounce[BLOCK_SIZE] = {0};
    init(&disk, &bot, &io, &lock, bounce, sizeof(bounce));
    struct user_buffer user = {.length = BLOCK_SIZE};
    struct usb_block_buffer buffer = {&user, user.length, copy_to, copy_from};
    uint64_t completed;

    disk.fail_sync_once = true;
    assert(usb_block_io_quiesce(&io) == USB_COMMAND_FAILED && !io.accepting && !io.quiesced);
    unsigned commands = disk.commands;
    assert(usb_block_io_transfer(&io, false, 0, 1, &buffer, &completed) == USB_BUSY && !completed);
    assert(disk.commands == commands);
    assert(usb_block_io_quiesce(&io) == USB_OK && io.quiesced && !io.accepting);
    commands = disk.commands;
    assert(usb_block_io_quiesce(&io) == USB_OK && disk.commands == commands);
    assert(usb_block_io_sync(&io) == USB_BUSY);
    assert(usb_block_io_detach(&io, false) == USB_BUSY && io.online && bot.present);
    assert(usb_block_io_detach(&io, true) == USB_OK && !io.online && !bot.present);
    assert(usb_block_io_detach(&io, false) == USB_OK);

    struct disk_model disconnect_disk = {0};
    struct usb_bot disconnect_bot;
    struct usb_block_io disconnect_io;
    struct test_lock disconnect_lock = {0};
    uint8_t disconnect_bounce[BLOCK_SIZE] = {0};
    init(&disconnect_disk, &disconnect_bot, &disconnect_io, &disconnect_lock,
         disconnect_bounce, sizeof(disconnect_bounce));
    struct user_buffer disconnected_user = {.length = BLOCK_SIZE};
    struct usb_block_buffer disconnected_buffer = {
        &disconnected_user, disconnected_user.length, copy_to, copy_from
    };
    disconnect_disk.disconnected = true;
    assert(usb_block_io_transfer(&disconnect_io, false, 0, 1,
                                 &disconnected_buffer, &completed) == USB_DISCONNECTED);
    assert(!disconnect_io.online && !disconnect_io.detached && !disconnect_bot.present);
    assert(usb_block_io_detach(&disconnect_io, false) == USB_BUSY && !disconnect_io.detached);
    assert(usb_block_io_detach(&disconnect_io, true) == USB_OK && disconnect_io.detached);
}

int main(void)
{
    test_chunked_read_write();
    test_failures_and_bounds();
    test_quiesce_and_detach();
    puts("PASS: USB production block I/O chunking, serialization, sync, quiesce and detach");
    return 0;
}
