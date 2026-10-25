# Portable serial UFS transport

This source-only kernel component builds real legacy UTRD/UPIU/PRDT commands
in a caller-owned DMA region. It has no board addresses, SMMU writes, firmware
buffer pointers, LUN-zero selection or logical-block-size default. A platform
must initialize its controller/link from its own fixed sources and prove
exclusive legacy-mode ownership, with MCQ inactive, before starting this core.
The platform must consume startup UIC/power events; during command transport
only transfer-completion interrupt bit zero is expected.

The caller supplies separate CPU/bus addresses, the complete allocated span
and its actual address limit. The core checks alignment and full-span overflow.
It programs its own transfer list only after both inherited doorbells and run
bits are idle. All methods require caller serialization. Cache callbacks must
apply the platform's real cache maintenance and barriers, including descriptor
polling. Nothing in this component proves cache coherency or an IOMMU domain.

NOP and a single fDeviceInit SET precede bounded flag polling. Each normal LUN
is probed explicitly, using its UnitDescriptor and SCSI capacity; both sources
must agree on block count and block size. Only reset UNIT ATTENTION permits up
to three completed TEST UNIT READY attempts. Data commands, queries, syncs and
timeouts are never automatically replayed. Capacity and transfer paths support
READ/WRITE(10) and (16), including real 512-byte and 4096-byte media geometries.

All writing defaults to denial. The platform's allowWrite callback must verify
the full device/candidate/source/token/LUN/geometry/scratch grant and is called
before copying data and again immediately before submission. The core also
checks capacity, transfer size and observed unit write protection. It exposes
no raw mutating SCSI entry. The Dali grant implementation is still pending;
this callback contract and its simulated tests are not a runtime authorization.

SYNC dispatches an actual ordered SYNCHRONIZE CACHE(10), without IMMED.
Completed responses require matching transaction, tag and LUN, valid OCS and
bounded sense/residual fields. A valid underrun reports a partial byte count;
only that read prefix is copied. Partial operations and failed writes/syncs
poison the transport. Timeout, cache failures, unknown interrupts, unexpected
queues/base changes or malformed responses also quarantine it. Later commands
cannot overwrite the uncertain descriptors or resubmit a request. Completion
describes the last submitted command; callers report zero bytes for rejected
requests instead of reusing an earlier completion's byte count.

Close seals the core. The platform owns controller stop and buffer lifetime;
an exposed region cannot pass releaseAllowed until close and an observed HCE=0.
The core never frees a region itself. A platform must retain/quarantine it when
stop fails and must not allow AP/other-controller access to these same queues.

Native tests compile this production implementation with simulated hardware,
cache and policy, using different CPU/device addresses. The QEMU runner executes
the same commands against its UFS model on LUN2, verifies read/write/sync and
checks the entire synthetic disk for changes outside its permitted block.
Qtest copies bytes between host CPU and guest RAM; this is not an ARM cache or
XNU DMA allocation test. IOStorageFamily's source builder includes the object
and verifies its ABI symbol in the archive and final kernel. Actual Apple
toolchain compilation must be recorded separately from host/QEMU results.
Physical Dali allocation, initialization, IOBlockStorageDevice wiring, scratch
authorization and persistence remain separate required integration work.
