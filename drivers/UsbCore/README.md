# Portable USB and MTU3 device components

This is shared production code for the Dali firmware and source-XNU adapters.
The current milestone implements the protocol/ownership layer, MTU3 QMU queues,
CDC EP0 and nonzero endpoint configuration. Complete controller initialization, host/hub enumeration,
OS adapters and hardware qualification remain separate work. No firmware/XNU
USB functionality is enabled by adding these files.

`UsbCore.c` provides bounded MMIO, qualified 32-bit DMA, USB configuration and
CDC ACM controls, a nonblocking diagnostic ring, boot keyboard/mouse reports,
BOT/SCSI capacity/read/write/sync and role quiesce. Failed BOT recovery blocks
new commands; uncertain writes are never automatically replayed or repaired by
a later flush. No VID/PID is implicitly allocated: the OS adapter supplies its
explicit configured identity to CDC initialization.

`UsbMtu3Ep0.c` handles real EP0 registers/FIFO with 64-byte packets, short/ZLP
responses, OUT bounds, W1C acknowledgements and a one-second transaction deadline.
All CDC settings, including line coding and DTR/RTS, remain pending until the
hardware status ACK. New SETUP, SETUPEND, reset, timeout or fault discards pending
changes. Configuration/halt callbacks must reserve/quiesce endpoint resources in
PREPARE, enable them in APPLY and roll back in ABORT. FIFO tails require native
8/16-bit writes; widening them is invalid. Polling must continue without IRQs to
observe DATAEND clearing. The port owner stops nonzero queues before bus reset or
disconnect/stop. A transaction timeout resets only EP0, preserving device address
and configuration; a bus reset clears both and retains the diagnostic log ring.

`UsbMtu3Endpoints.c` reads actual endpoint/FIFO/IPPC version capabilities before
programming notification IN1 and bulk IN2/OUT2. It supports shared USB2 and separate
FIFO layouts with two 512-byte slots per endpoint; original/enhanced burst/mult
fields are zero for USB2. Configuration/deconfiguration refuses active QMU queues;
halt operations require the selected queue inactive, and clear-halt resets its
data toggle. The owner must stop/cancel queues and reset ring ownership first.
Partial register writes latch a fault until a successful deconfiguration. These
components do not acquire power/clocks/PHY or provide an OS controller driver.

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
corruption, DMA-stop timeout, EP0 abort/status timing, FIFO layouts, endpoint
halt/toggle and role-transition failures. These results do not
qualify MT6991 MMIO, actual QEMU USB devices or hardware USB.
