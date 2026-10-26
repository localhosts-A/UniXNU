# Native Mali G925

This path targets MT6991/G925, native 4 KiB XNU and the pinned CSF firmware.
It is under implementation. Passing host fixtures or an XNU build does not
qualify GPU execution, platform power transitions, or iOS display output.

## Kernel boundaries

`drivers/NativeMali` contains the portable production parser, GPU MMU,
firmware loader/configuration, register protocol, MT6991 power/mailbox route,
boot lifecycle, resident CSG scheduler, and per-connection memory/ABI core.
`NativeMaliXnuMemory` adapts owned/persistent IOKit descriptors to those cores.
The device repository supplies `DaliMali` and its IOUserClient.

The driver accepts resources/firmware only from the explicit synthetic Mali
nub. The firmware SHA-256 must match its resource descriptor. The Dali profile
requires `dali-mali=1 maxmem=1992`: before any power MMIO, the service verifies
that the actual XNU managed physical end is below the GPUEB reserved region.
The memory limit is a temporary conservative ownership policy; a resource
property alone is not an XNU VM reservation.

AS0 is private to the MCU. Each connection gets one available AS1..15, its
own page tables, generation-scoped handles and a 256 MiB allocation budget.
No physical addresses, firmware input pages, or MMIO doorbells are exported.
Queue output pages are read-only client mappings. Queue input staging is
anonymous userspace memory; a kernel call validates the monotonic insert
position and capacity before publishing it to the private firmware input.

Groups occupy actual firmware slots. The capacity is bounded by the firmware
metadata and a preboot AS0 arena; no overcommit is exposed. AS mutations
suspend every group in that AS, change mappings, synchronize GPU caches/TLBs,
then resume from the actual firmware extraction position. Shutdown requires
TERMINATE ACK and AS disable, or a completed hardware reset. Any uncertain
DMA ownership retains allocations and their callback owner.

Tiler heaps use a 32-byte context in a dedicated page, 64-byte chunk headers,
and the CSF packed chunk size/address format. Initial chunks form a linked
list. OOM uses actual firmware counters and replies with a newly mapped
chunk, a deferred zero pointer when other fragments can free space, or a
device-lost error on exhaustion. Protected execution is unsupported.

## Userspace ABI

`NativeMaliABI.h` is shared C layout: one 128-byte versioned request and a
256-byte response through IOConnectCallStructMethod selector0. Every reserved
field must be zero. Object handles and mapping tokens are connection-local.
Properties come from GPU registers and the booted CSF interface.

`WAIT_VALUE` samples an owned, 16-byte-aligned GPU memory location and succeeds
only when its real value exceeds the supplied target-minus-one. It does not
advance a fence or infer completion from a successful kick. Requests use
bounded deadlines and the service also polls firmware events on a timer.

CPU mappings of owned/imported DMA pages use Normal noncacheable attributes.
Physical-page cache attributes are reference counted across overlapping
imports and restored when the last allocation releases them. Page table and
cached image publication uses explicit ARM cache maintenance.

## Offline checks

Run `python3 scripts/check-native-mali-host.py --sanitize`. The integrated
model walks the production GPU tables and the copied firmware data; its MCU,
ACK and extraction actors are explicitly simulated. Tests cover malformed
metadata, late/missing ACKs, foreign handles, alias translations, ring bounds,
heap growth, actual-value waits, and retention after an unconfirmed stop.
The report always records `hardware_exercised=false`.

Protocol references are pinned in `scripts/data/mali-g925-inputs.json`.
The numeric CSF and MT6991 hardware protocols were checked against the public
Arm/MediaTek driver sources and private passive r49 evidence. Linux driver
implementation is not linked into XNU. Existing NativeAdreno MIT cache and
IOKit descriptor conventions informed the separate native memory adapter.
