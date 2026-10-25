# Firmware storage ownership handoff

`UNIXNU_STORAGE_QUIESCE_PROTOCOL` is an optional platform interface defined in
UniXNUPkg. DarwinBoot enumerates its providers after closing all boot inputs
and before requesting the final memory map. Each provider must close new
submissions, wait for outstanding work within a bounded deadline, issue real
device synchronization for completed writes, stop queues and hardware, and
revoke firmware DMA before returning exact `EFI_SUCCESS`. No firmware buffer
or descriptor pointers are inherited by XNU.

An absent interface preserves the existing RAM-only path. Present interfaces
are validated and bounded to 64 providers. Every provider is attempted even if
another fails, retaining the first error. Warnings are treated as errors; no
failure authorizes kernel entry. There is no rollback or reopening. Providers
must remain quiesced across repeated calls and ExitBootServices retries, without
replaying an uncertain write. DarwinBoot invokes quiesce once before the first
final map; EBS retries only refresh that map and retry firmware exit.

The consumer is implemented in DarwinBoot's separate repository and pinned by
the `drivers/DarwinBoot` gitlink. The generic package supplies the protocol
header/GUID. A platform enabling physical storage must publish its provider even
if controller initialization failed, so failure can be reported at handoff.
This interface does not by itself implement a Dali provider or XNU controller.

```sh
python3 -m unittest discover -s tests/unixnu -v
python3 scripts/test-darwinboot.py --storage-handoff \
  --edk2 /src/Mu_Basecore --uefi-efi /private/built/UniXNU.efi
```

The native tests compile the production consumer and check absent/bad/multiple
providers, failed enumeration, warning/error propagation and repeated calls.
The explicit QEMU mode rebuilds DarwinBoot, removes any prior result before
starting, and loads the standalone StorageValidation fixture from its test ESP.
The real loader first refuses a valid boot when a provider returns a timeout.
The next boot seals a provider before the final map and deliberately returns
`EFI_INVALID_PARAMETER` from the first EBS call. The real retry exits firmware
and enters the synthetic Mach-O with the provider still sealed exactly once.
The result records source, EFI and raw-console hashes. A failed build or run
cannot leave a prior successful result JSON available.

`Test/StorageValidation.inf` is a standalone QEMU fixture, excluded from the
Dali DSC/FDF. It changes the QEMU Boot Services table only to force the retry,
restoring the exact original function pointer and CRC before real EBS. These
tests qualify the consumer and loader flow only. MT6991 request completion,
device synchronization, DMA teardown, XNU ownership and persistence still need
their own production implementations and hardware evidence.
