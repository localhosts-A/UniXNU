# Portable USB and MTU3 device components

This is shared production code for the Dali firmware and source-XNU adapters.
The current milestone implements the protocol/ownership layer, a USB2 host
coordinator with external-hub management, and a combined MTU3 CDC device MAC
using QMU, EP0 and nonzero endpoint configuration. Platform clocks/power/PHY/DMA
ownership, native controller workers, OS adapters and hardware qualification
remain separate work. No firmware/XNU USB functionality is enabled by adding
these files.

`UsbCore.dec` / `UsbCoreLib.inf` build all seven production C units with EDK2.
`UNIXNU_USB_EDK2` selects explicit EDK2 integer/size/bool types through
`UsbTypes.h`; ordinary host and XNU compilation keeps standard C headers.
This creates a real AArch64 archive, without installing a controller driver.
The isolated firmware's `tools/dali/build_usb_libraries.py` builds and checks
the production archive members and symbol definitions together with the Dali
platform and UEFI PMIF adapter libraries. Compilation/archiving is distinct
from EFI image linking, USB enumeration and hardware qualification.

`UsbCore.c` provides bounded MMIO, qualified 32-bit DMA, USB configuration and
CDC ACM controls, a nonblocking diagnostic ring, boot keyboard/mouse reports,
BOT/SCSI capacity/read/write/sync, write-protect probing and role quiesce. The
write-protect probe reads the MODE SENSE(6) device-specific header, tries page
`0x3f` then page `0`, and treats two rejected pages as writable while preserving
transport/protocol errors. Failed BOT recovery blocks new commands; uncertain
writes are never automatically replayed or repaired by a later flush. No VID/PID
is implicitly allocated: the OS adapter supplies its explicit configured
identity to CDC initialization.

`UsbHost.c` enumerates checked USB2 descriptors around a controller contract,
routes boot HID and BOT, and manages USB2 external hubs. Hub state uses standard
hub-class requests, validates descriptors and interrupt bitmaps, powers ports,
resets connected ports on bounded monotonic deadlines, reports attach/detach and
overcurrent events, and builds child routes with single/multi-TT and think-time
metadata. Failed child attach callbacks remain retryable after the hardware reset
deadline; unplug or stop delivers DETACH so the owner can clean partial child
state. A controller clock callback is required for hub timing. The owner must
quiesce children before stopping or disconnecting a hub; overcurrent remains
latched until the owner stops and restarts the hub. A controller that requires
an owned default-address slot before the first EP0 request can supply
`prepare_device`; it must record uncertain slot/DMA ownership before issuing
an operation. Failed initial descriptor or address setup then uses
`disable_device` with address zero and retains the host object if stop cannot
be proven. This is a controller contract, not an xHCI backend implementation.

`UsbXhci.c` is the portable low-level xHCI foundation: it validates capability
register windows and USB2 Supported Protocol port ranges without writing MMIO,
and owns coherent DMA32 command/transfer-ring and event-ring memory. It publishes
TRBs with the first entry published last, tracks completion pointers, validates
event cycles and retains memory after synchronization or completion faults until
the caller provides a fresh stop proof. Each single ring segment fits within a
64 KiB boundary, and event segments reserve full 64-byte lines with at most 4096
TRBs as required by xHCI 1.2 sections 4.9 and 6.5. It does not start the controller, issue doorbells,
translate IRQs, provide a DMA/IOMMU mapping, populate `usb_host_ops`, or publish
HID/BOT services. Those runtime owners remain required.

`UsbStorage.c` serializes block requests around a BOT device using caller-owned
lock and bounce-buffer callbacks. It chunks large `IOStorage` requests to the
BOT 32-bit transfer limit, checks block and byte ranges, reports only fully
completed chunks, and never retries a failed write. Quiesce closes admission
before issuing SYNCHRONIZE CACHE; failure remains retryable but keeps new I/O
closed. A permanently uncertain BOT write remains unsafe after a later flush and
requires device teardown/reinitialization. Detach requires the host owner to prove
the device endpoint/slot and DMA are stopped, even after a disconnect has marked
the medium offline. The Dali tree contains a concrete XNU
`IOBlockStorageDevice` service that consumes this interface; a production host
child owner still needs to publish it and bind BOT operations to a real xHCI
endpoint/DMA implementation.

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

`UsbMtu3Device.c` composes those components into the USB2 device MAC. The platform
must own/reset/power the MAC and PHY, select device/sink and establish DMA mapping
before initialization. Initialization only probes and allocates; start rejects
inherited active queues and absent VBUS, disables USB3, selects the hardware QMU
format and enables softconnect without forcing VBUS. A serialized IRQ/timer
worker handles speed, reset, disconnect, suspend/resume, CDC notification IN1,
diagnostic bulk IN2 and discard-only OUT2. Log writers never perform MMIO or wait.
DTR gates output, and packet-aligned chunks request an appended ZLP to complete
the host read. RX ZLP status is acknowledged without faulting the controller.

Configuration PREPARE stops queues without altering endpoint toggles; only the
successful status APPLY reconfigures endpoints. Aborted controls can cancel an
in-flight diagnostic chunk; these bytes are counted as dropped and never replayed.
The `tx_dma_completed` counter measures QMU completion, not delivery to host
userspace. Stop masks interrupts and disconnects before bounded queue cancellation.
A failed stop/cache handoff retains all six DMA allocations; the caller must retry
or keep them reserved, and cannot change role or power down. Successful stop is
idempotent. No UEFI/XNU binding or platform qualification is implied by this MAC.

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
halt/toggle, combined host-driven CDC/QMU transactions, every startup-write fault,
VBUS loss/reconnect, cache handoff failures and role-transition failures. These results do not
qualify MT6991 MMIO, actual QEMU USB devices or hardware USB.
