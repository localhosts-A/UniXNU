# Native Mali G925

This path targets MT6991/G925, native 4 KiB XNU and the pinned CSF firmware.
The native XNU source, Darwin PanVK ICD, NativeMetal plugin bundle, and offline
compiler fixtures build from the `codex/dali-mali-metal-offline` branches.
Passing those builds does not qualify GPU execution, platform power
transitions, or iOS display output.

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
property alone is not an XNU VM reservation. The firmware is a private input
and is not included in source or public CI artifacts.

For the published render path, the IOGPUFamily plugin initializes in
`backboardd` and PanVK opens the `DaliMali` service there. The service publishes
`NativeSandboxGrantProcess=backboardd` before registration; the generic XNU
grant checks that exact process name. Plugin discovery and sandbox access
remain pending device validation.

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

On a macOS 26 runner with Xcode 26.6, run the reproducible Darwin build from
the repository root:

```sh
brew install llvm spirv-llvm-translator spirv-tools glslang cmake pkgconf ninja
python3 -m venv .cache/mali-venv
.cache/mali-venv/bin/python -m pip install -r scripts/data/mali-build-requirements.txt
rustup toolchain install 1.98.0 --profile minimal
rustup target add --toolchain 1.98.0 aarch64-apple-ios aarch64-apple-darwin
git submodule update --init drivers/NativeMetal
python3 scripts/prepare-panvk-darwin.py --fetch --darwin
python3 scripts/build-panvk-host-tools.py --dependency-prefix "$(brew --prefix spirv-llvm-translator)"
python3 scripts/check-mali-offline-compiler.py
python3 scripts/build-panvk-darwin.py --target host
python3 scripts/build-panvk-darwin.py --target ios
python3 scripts/build-mali-metal-plugin.py --fetch --target host
python3 scripts/check-native-mali-metal-air.py
python3 scripts/build-mali-metal-plugin.py --target ios
```

The output includes host and iOS17 Vulkan ICDs, signed Metal bundles with the
matching ICD inside, SDK/compiler/source profiles, symbol/dependency reports,
and SPIR-V/compiler fixtures under `build/`. `build-mali-metal-plugin.py`
requires the matching PanVK library and verifies the bundle signature, Mach-O
architecture, entry point, dependencies, and file hashes. The macOS CI
`NativeMali PanVK Darwin` artifact retains these outputs for 30 days. The
separate `NativeMali Metal` workflow checks plugin compilation and completed
frame ownership without a GPU.

## Device trial checklist (pending)

No GPU context, submission, install, reboot, or flash has been performed. The
physical device remains unqualified. Before a user-authorized trial:

1. Confirm the exact REDMI K80 Ultra MT6991 unit, recoverable boot path, and a
   known-good baseline image. Keep the existing single-core boot arguments and
   add `dali-mali=1 maxmem=1992` only for the trial boot.
2. From a directly read, unit-matched runtime FDT and the matching firmware,
   build the Apple DT with `scripts/make-dali-adt.py --fdt <private-fdt>
   --dram-size 0x300000000 --mali-firmware <private-csf-image> --output <adt>`.
   Check the emitted resource/FW SHA report against `scripts/data/mali-g925-inputs.json`.
   Keep the firmware and generated ADT private; neither belongs in CI artifacts.
3. Stage the source-XNU artifact and `DaliMaliAccelerator.kext` from the device
   repository CI together with the iOS17 `NativeMaliMetal.bundle` from the
   generic repository CI. The accelerator kext depends on the target's actual
   IOGPUFamily; install/sign/trust integration must match the target image.
   The package does not modify the original Apple trust modules.
4. First verify boot with Mali disabled, then use the recovery route and boot
   logs to verify resource parsing and the kernel's reservation check. Only
   after an explicitly authorized GPU trial may the service be enabled; record
   each reset, power, mailbox, SMMU, MCU/CSF ACK, fault, timeout, and retention
   result before exercising a queue.
5. For NativeMetal, verify plugin discovery, one harmless allocation, a real
   kernel completion/value wait, IOSurface import/cache visibility, then
   register a process-lifetime sink with
   `native_mali_metal_set_frame_sink` and verify successful-frame handoff and
   the display's own frame acknowledgement.
   Separate GPU completion from panel presentation; the offline frame test
   does not prove vblank or scanout.

Current state: all steps above are pending. There is no claim of successful
hardware boot, firmware execution, GPU submission, Metal rendering, or display.

Protocol references are pinned in `scripts/data/mali-g925-inputs.json`.
The numeric CSF and MT6991 hardware protocols were checked against the public
Arm/MediaTek driver sources and private passive r49 evidence. Linux driver
implementation is not linked into XNU. Existing NativeAdreno MIT cache and
IOKit descriptor conventions informed the separate native memory adapter.
