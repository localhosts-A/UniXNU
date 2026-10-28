# Kernel prelink

The prelinked kernel combines a kernel built from source with the original Apple kernelcache. The source kernel provides the code that UniXNU changes. The original kernelcache provides the Apple kexts and platform services, which the source tree leaves to the kernelcache.

## Inputs

| Input | Source |
| --- | --- |
| Native kernel | `out/<board>/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf` |
| Original kernelcache | IPSW `KernelCache`, or `--kernelcache FILE` |
| Component list | `[kernel.prelink] roots` |
| Source-provided component identities | `[kernel.prelink] source_interfaces` |
| Explicit donor-to-native exports without donor names | `[kernel.prelink] source_interface_bindings` |
| Slide | `[kernel.prelink] slide` |
| Early security initializer | `[kernel.prelink] early_security_init` |

The kernelcache is the LZFSE payload of the `KernelCache` IM4P. The arm64 slice is selected and checked against `kernelcache_sha256`.

## Prelink steps

`kits/NativePrelink/emit_prelink.py` performs these steps:

1. Parse the native and original Mach-O images.
2. Read `__PRELINK_INFO` from the original kernelcache.
3. Select the components named in `roots` plus the kernel interfaces they import.
   A board may declare `source_interfaces` when the native kernel already contains
   a component implementation. Its selected prelink tool must retain the bundle
   version, dependencies, and IOKit personalities as codeless metadata while
   omitting the duplicate executable and kmod record. The default NativePrelink
   emitter does not apply that board-specific transformation by itself. When a
   donor address has no kernelcache symbol name, the board may pin a narrow
   `source_interface_bindings` entry containing its bundle, donor VA, and native
   export name. Such entries require the exact pinned kernelcache and a unique
   executable donor segment and native export.
4. Copy the selected component segments into the native kernel's empty coalesced segments.
5. Rebind every pointer that references a selected component.
6. Apply the slide to every selected address.
7. Rewrite the module headers and kmod entries.
8. Build a sparse prelink index.
9. Write `kernel.macho`.
10. Run `scripts/verify_prelink.py` against the native kernel, original kernelcache, emitted candidate, and report. It independently recomputes the selected closure and pointer-table targets, then checks component Mach-O headers, file-backed bytes, kmod addresses, and the sparse index.

The output keeps the original component code unchanged. The native kernel supplies the platform code, the CPU code, the storage code and the device drivers selected in the defconfig.

## Coalesced segments

The original kernelcache places kext segments in coalesced ranges:

| Original segment | Native segment |
| --- | --- |
| `__PRELINK_TEXT` | `__TEXT` |
| `__PLK_TEXT_EXEC` | `__TEXT_EXEC` |
| `__PLK_DATA_CONST` | `__DATA_CONST` |
| `__PRELINK_DATA` | `__DATA` |

The prelink copies each component segment into the matching range. The ranges keep their original virtual addresses, so pointers inside the original code stay valid after the slide.

## Pointer rebinding

The native kernel imports symbols from the original components. The prelink resolves each import to the component export and writes the slid address at the import site. A board-specific source-interface tool must resolve any import targeting an omitted executable to exactly one native export; unresolved or ambiguous bindings fail closed. An explicit donor-to-native mapping applies only to its pinned donor address. The report records each configured symbol, its donor and native address, and how many relocation entries used it.

The plan records every binding:

```json
{
  "bindings": [
    { "new_site": "0xfffffff007123456", "new_target": "0xfffffff004abcdef" }
  ]
}
```

The report is written next to the output. It also records the component list, the instruction inventory and the module entry points.

The independent verifier writes `kernel.verification.json`. A successful structural audit proves only the checks listed in that file. It deliberately leaves instruction-relocation completeness, source ABI, runtime qualification, and `loadable` false; pointer-table and header consistency alone do not qualify a device image.

## Early security initialization

`early_security_init` selects an original security extension and an address in its executable segment. The prelink slides the address and records it for the kernel's early security path. The selected component must carry `AppleSecurityExtension = true` in the prelink metadata.

## Kernelcache pin

A board can pin the kernelcache:

```toml
[kernel.prelink]
kernelcache_sha256 = "..."
```

The prelink refuses a different kernelcache. This ties the native kernel to the original component ABI it was qualified against.

## Sparsity and capacity

The prelink emits a sparse index. The kernel patch `native-4k-bootstrap-capacity` increases the bootstrap page table capacity for native 4 KiB mappings. The boot progress patch adds checkpoint output around the prelink path.

## Report

`emit_prelink.py` writes a JSON report next to the output:

| Field | Meaning |
| --- | --- |
| `components` | Original bundle identifiers included |
| `source_interfaces` | Original component identities implemented by the native kernel |
| `source_interface_bindings` | Explicit donor addresses resolved to unique native exports |
| `codeless_components` | Bundle metadata and personalities without duplicate executable code |
| `kernel_interfaces` | Components that export kernel interfaces |
| `bindings` | Import sites and slid targets |
| `instruction_inventory` | PC-relative encodings and inline data |
| `module_entrypoints` | Module load addresses after the slide |
| `early_security_initializers` | Selected early initializer addresses |

`verify_prelink.py` writes a separate sidecar with hashes for all three exact inputs, closure and pointer-table checks, and explicit false values for instruction, ABI, runtime, and loadability qualification.

## Next steps

- [Build the kernel](/guide/build-kernel) lists the kernel flags.
- [Four-kilobyte pages](/xnu/four-k-page-tables) explains the page size patches.
- [Integrity and boot policy](/concepts/security) explains the trust cache path.
