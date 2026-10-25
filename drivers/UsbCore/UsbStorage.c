/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbCore.h"

static int acquire(struct usb_block_io *io)
{
    if (!io || !io->lock || !io->unlock) return USB_INVALID;
    return io->lock(io->lock_context);
}

int usb_block_io_init(struct usb_block_io *io, struct usb_bot *bot, void *bounce,
                      size_t bounce_size, void *lock_context,
                      int (*lock)(void *), void (*unlock)(void *))
{
    if (!io || !bot || !bot->present || !bot->blocks ||
        bot->block_size < 512 || bot->block_size > 4096 ||
        (bot->block_size & (bot->block_size - 1)) || !bounce ||
        bounce_size < bot->block_size || !lock || !unlock) return USB_INVALID;
    io->bot = bot;
    io->lock_context = lock_context;
    io->lock = lock;
    io->unlock = unlock;
    io->bounce = bounce;
    io->bounce_size = bounce_size;
    io->online = true;
    io->accepting = true;
    io->quiesced = false;
    io->detached = false;
    return USB_OK;
}

int usb_block_io_transfer(struct usb_block_io *io, bool write, uint64_t lba,
                          uint64_t blocks, const struct usb_block_buffer *buffer,
                          uint64_t *completed_bytes)
{
    if (completed_bytes) *completed_bytes = 0;
    if (!io || !buffer || !completed_bytes || !blocks ||
        (write ? !buffer->copy_to : !buffer->copy_from)) return USB_INVALID;

    int result = acquire(io);
    if (result) return result;
    if (!io->online || !io->bot || !io->bot->present) {
        io->online = false;
        io->accepting = false;
        result = USB_DISCONNECTED;
        goto done;
    }
    if (!io->accepting || io->quiesced) {
        result = USB_BUSY;
        goto done;
    }

    struct usb_bot *bot = io->bot;
    if (!bot->blocks || !bot->block_size || lba >= bot->blocks ||
        blocks > bot->blocks - lba || blocks > UINT64_MAX / bot->block_size) {
        result = USB_RANGE;
        goto done;
    }
    uint64_t total_bytes = blocks * bot->block_size;
    if (buffer->length != total_bytes ||
        (write && !buffer->copy_to) || (!write && !buffer->copy_from)) {
        result = USB_RANGE;
        goto done;
    }

    size_t max_blocks = io->bounce_size / bot->block_size;
    size_t wire_max_blocks = (size_t)(UINT32_MAX / bot->block_size);
    if (max_blocks > wire_max_blocks) max_blocks = wire_max_blocks;
    if (!max_blocks || max_blocks > UINT32_MAX) {
        result = USB_RANGE;
        goto done;
    }

    uint64_t completed_blocks = 0;
    while (completed_blocks < blocks) {
        uint64_t remaining = blocks - completed_blocks;
        uint32_t chunk_blocks = remaining > max_blocks ? (uint32_t)max_blocks : (uint32_t)remaining;
        size_t chunk_bytes = (size_t)chunk_blocks * bot->block_size;
        uint64_t offset = completed_blocks * bot->block_size;

        if (write) {
            result = buffer->copy_to(buffer->context, offset, io->bounce, chunk_bytes);
            if (result) goto done;
            result = usb_bot_write(bot, lba + completed_blocks, chunk_blocks,
                                   io->bounce, chunk_bytes);
        } else {
            result = usb_bot_read(bot, lba + completed_blocks, chunk_blocks,
                                  io->bounce, chunk_bytes);
            if (!result)
                result = buffer->copy_from(buffer->context, offset, io->bounce, chunk_bytes);
        }
        if (result) {
            if (result == USB_DISCONNECTED) {
                io->online = false;
                io->accepting = false;
            }
            goto done;
        }
        completed_blocks += chunk_blocks;
        *completed_bytes = completed_blocks * bot->block_size;
    }
    result = USB_OK;

done:
    io->unlock(io->lock_context);
    return result;
}

int usb_block_io_sync(struct usb_block_io *io)
{
    int result = acquire(io);
    if (result) return result;
    if (!io->online || !io->bot || !io->bot->present) {
        io->online = false;
        io->accepting = false;
        result = USB_DISCONNECTED;
    } else if (!io->accepting || io->quiesced) {
        result = USB_BUSY;
    } else {
        result = usb_bot_sync(io->bot);
        if (result == USB_DISCONNECTED) {
            io->online = false;
            io->accepting = false;
        }
    }
    io->unlock(io->lock_context);
    return result;
}

int usb_block_io_quiesce(struct usb_block_io *io)
{
    int result = acquire(io);
    if (result) return result;
    if (!io->online || !io->bot || !io->bot->present) {
        io->online = false;
        io->accepting = false;
        result = USB_DISCONNECTED;
    } else if (io->quiesced) {
        result = USB_OK;
    } else {
        io->accepting = false;
        result = usb_bot_sync(io->bot);
        if (!result) io->quiesced = true;
        else if (result == USB_DISCONNECTED) io->online = false;
    }
    io->unlock(io->lock_context);
    return result;
}

int usb_block_io_detach(struct usb_block_io *io, bool controller_stopped)
{
    int result = acquire(io);
    if (result) return result;
    if (io->detached) {
        result = USB_OK;
    } else if (!controller_stopped) {
        result = USB_BUSY;
    } else {
        io->accepting = false;
        io->quiesced = true;
        if (io->bot) usb_bot_detach(io->bot);
        io->online = false;
        io->detached = true;
        result = USB_OK;
    }
    io->unlock(io->lock_context);
    return result;
}
