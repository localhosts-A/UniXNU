#!/usr/bin/env python3
"""Qualify real APFS discovery, RAM Data writes, timezone success and UUID refusal."""
import hashlib
import json
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[1]
base = runpy.run_path(str(ROOT / 'scripts/check-original-launchd-main-checkpoints.py'))
require = base['require']
pins = {
    'discovery': ('native-owner-ram-block-discovery-candidate', 'bdc5172c0c3381d126b35305d44eefb9e93823aed3d5ec9493ae7e836d9cd5dc', '18f02ff65b5d05baa3a52732d49cef09e3f288d3d1e11a7dbe8112b2de9f643f'),
    'category': ('native-owner-ram-block-category-candidate', 'f51ae7cb5677d2790490c9c71c36d12239e8e8ce6360fc79ce80e5ff600122e2', '91fa09ce1931a5d6f6410375c9354c219db74553b48dcc7fad1844862356d840'),
    'mount': ('native-owner-ram-data-mount-candidate', '06e378c64b30771396c0da87f6acf380c88a6789ecf41c56627fec8dbf5d7e5f', 'f36566e9490543351f4f867e21f898cd66e3fe40088da29cdbf49a806690e74f'),
    'wrong': ('native-owner-ram-data-wrong-uuid-candidate', 'cf0eff16bf5932a76da18c2925e7442c2de8ad9d30b9ac83a84a879d8c425156', '943a26366c1eea01ffe67db8b028dadd657688ed24776069f41be0a4fee2c01d'),
}
expected = {k: base['candidate'](*v) for k, v in pins.items()}
seed = (ROOT / 'build/owner-data-fixture/owner-data-32.raw').read_bytes()
digest = 'd7cd79af299b9dfc1b26ed886000c455f950470c26cffdf796478609bffb6aa9'
require(len(seed) == 33554432 and hashlib.sha256(seed).hexdigest() == digest, 'Keep unchanged host seed')
names = ('ram-block-discovery', 'ram-block-category', 'ram-data-mount', 'ram-data-tzinit-symlink', 'ram-data-first-abort', 'ram-data-wrong-uuid')
discovery, category, mount, timezone, abort, wrong = [base['report']('owner-hvf-' + name) for name in names]
for d, key in zip((discovery, category, mount, timezone, abort, wrong), ('discovery', 'category', 'mount', 'mount', 'mount', 'wrong')):
    require(d['kernel_sha256'] == expected[key] and d['qemu_accelerator'] == 'hvf' and d['native_4k_mmu_observed'] and
            d['kernel_page_globals'] == dict(_PAGE_SHIFT_CONST=12, _page_shift=12, _page_shift_user32=12, _page_size=4096) and
            d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr'] == 80 and
            d['owner_cache_mount']['_native_owner_cache_status'] == 0 and d['owner_boot_manifest']['status'] == 0,
            'Use exact native-4K candidates with genuine owner root/cache/metadata')
    require(d['original_amfi_table_owner_observed'] and d['original_crypto_table_owner_observed'] and
            d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0) and
            d['native_virtio_globals']['_native_virtio_read_failures'] == 0 and d['native_virtio_integrity']['_native_virtio_integrity_failures'] == 0 and
            not d['functional_ipados_boot_demonstrated'] and not d['sheng_hardware_tested'], 'Preserve code/media validation and scope')
require(discovery['native_ram_block']['state']['_native_ram_block_ready'] == 0 and
        discovery['native_ram_block']['state']['_native_ram_block_reads'] == 0, 'Keep the genuine original matching-category failure control')
for d in (category, mount, timezone, abort, wrong):
    ram = d['native_ram_block']
    require(ram['input']['sha256'] == ram['seed_sha256'] == digest and ram['state']['_native_ram_block_ready'] == 1 and
            ram['state']['_native_ram_block_bytes'] == len(seed) and ram['state']['_native_ram_block_reads'] >= 29 and
            ram['state']['_native_ram_block_failures'] == 0 and not ram['input']['initial_seed_signature_verified'] and
            not ram['input']['production_storage_qualified'], 'Actual RAM storage bytes/read requests, without a persistent or signed Data claim')
entries = category['registry_string_events']
require(any(e['key'] == 'UUID' and e['value'] == '18AB5557-DC2D-469B-8758-855E60F5B0DF' for e in entries) and
        any(e['key'] == 'BSD Name' and e['value'] == 'disk1s1' for e in entries), 'Original APFS really publishes the observed scratch volume')
for d in (mount, timezone, abort):
    data = d['scratch_data']
    require(data['actual_seed_sha256'] == digest and data['actual_seed_bytes'] == len(seed) and
            data['actual_device_path'] == '/dev/disk1s1' and data['state'] == dict(_native_owner_data_status=0,
                _native_owner_data_stage=4, _native_owner_data_readonly=0, _native_owner_data_device=16777218) and
            d['native_ram_block']['state']['_native_ram_block_writes'] >= 30, 'Actual selected device mounts writable and completes real writes')
require(mount['bounded_timeout_interrupted'] and mount['stop_symbol'] == 'timeout-interrupt' and
        mount['native_ram_block']['state']['_native_ram_block_reads'] == 517 and mount['native_ram_block']['state']['_native_ram_block_writes'] == 1509,
        'A 45-second continuation with later process crashes is a bounded observation, not functional UI success')
require(timezone['stop_symbol'] == '_symlink' and timezone['function_return_stack_matched'] and int(timezone['x0'], 16) == 0 and
        timezone['entry_string_events'][-1]['matched'] and timezone['entry_string_events'][-1]['value'] == '/var/db/timezone/zoneinfo',
        'Previously failing original timezone syscall must now genuinely return zero')
reason = bytes.fromhex(abort['entry_indirect_memory']['1:20:100']['bytes']).split(b'\0')[0].decode()
require(abort['stop_symbol'] == '_abort_with_payload' and abort['function_return_stack_matched'] and int(abort['x0'], 16) == 0 and
        reason.startswith("couldn't dlopen libobjc-trampolines.dylib:") and 'no such file, not in dyld cache' in reason,
        'Preserve the next actual original abort, do not replace it with success')
require(wrong['scratch_data']['state'] == dict(_native_owner_data_status=2, _native_owner_data_stage=1,
        _native_owner_data_readonly=-1, _native_owner_data_device=-1) and wrong['native_ram_block']['state']['_native_ram_block_writes'] == 0 and
        wrong['stop_symbol'] == '_panic' and wrong['argument_memory']['x0'].startswith('offline scratch Data mount failed'), 'Wrong expected UUID must refuse before a writable mount')
audit = json.loads((ROOT / 'build/native-ram-block/bindings.json').read_text())
require(audit['virtual_methods_checked'] == 190 and audit['concrete_overrides'] == 24 and len(audit['bindings']) == 11 and
        audit['unresolved_original_imports'] == 0 and audit['compiled_object_sha256'] == hashlib.sha256((ROOT / 'build/native-ram-block/NativeRamBlock.o').read_bytes()).hexdigest(),
        'Actual compiled RAM object retains original storage ABI/imports')
print('PASS: original APFS discovers actual RAM volume; native-4K writable Data performs real reads/writes')
print('PASS: original timezone syscall returns zero; wrong UUID refuses before writes')
print('Next observed abort: missing libobjc-trampolines path; functional UI, keybags, firmware enrollment and sheng hardware remain open')
