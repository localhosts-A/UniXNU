# Portable USB and MTU3 QMU core

This is shared production code for the Dali firmware and source-XNU adapters.
The current milestone implements the protocol/ownership layer and MTU3 QMU
queue operations. Complete controller initialization, EP0, host/hub enumeration,
OS adapters and hardware qualification remain separate work. No firmware/XNU
USB functionality is enabled by adding these files.

`UsbCore.c` provides bounded MMIO, qualified 32-bit DMA, USB configuration and
CDC ACM controls, a nonblocking diagnostic ring, boot keyboard/mouse reports,
BOT/SCSI capacity/read/write/sync and role quiesce. Failed BOT recovery blocks
new commands; uncertain writes are never automatically replayed or repaired by
a later flush. No VID/PID is implicitly allocated: the OS adapter supplies its
explicit configured identity to CDC initialization.

`UsbMtu3Qmu.c` follows the pinned MediaTek descriptor/register definitions and
supports original and enhanced GPD formats using 32-bit addresses. Its backend
must prove controller/DMA ownership and initialize the endpoint/FIFO first.
Descriptor buffers must be uncached or hardware coherent. Data buffers remain
owned until completion/cancellation; callback consumers may release them but
must not re-enter or destroy the queue. A failed stop retains all buffers, and
destroy refuses a running or nonempty queue. Submission after publication
reports acceptance even if RESUME fails; the queue latches a fault and retains
that request for cancellation. Zero-length bulk transfers require the later
controller's PIO path rather than an unsupported old-QMU zero-length GPD.

Run production-code tests without accessing a device:

```sh
python3 -m unittest discover -s tests/usb -p 'test_*.py' -v
CC=gcc python3 -m unittest discover -s tests/usb -p 'test_*.py' -v
```

The C fixtures compile with conversion warnings as errors, AddressSanitizer and
UndefinedBehaviorSanitizer. They independently model transports/registers,
including malformed descriptors, concurrent log writers, short/uncertain
writes, failed recovery, queue wrap, stale high addresses, hardware completion
corruption, DMA-stop timeout and role-transition failures. These results do not
qualify MT6991 MMIO, actual QEMU USB devices or hardware USB.
