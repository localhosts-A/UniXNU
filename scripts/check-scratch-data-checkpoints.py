#!/usr/bin/env python3
"""Qualify a genuine APFS/memdev refusal without misreporting writable Data success."""
import hashlib
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[1]
base = runpy.run_path(str(ROOT / 'scripts/check-original-launchd-main-checkpoints.py'))
require = base['require']
expected = base['candidate']('native-owner-scratch-data-candidate',
    '3a4359eb443e706802f7672e4165f2f69cc79fcc498b73381353c72db7db776d',
    'a85a4fb651ca8cde566291e75989331d5d44d0d45bb71ebc1a7f98e397aa12e7')
seed = (ROOT / 'build/owner-data-fixture/owner-data-32.raw').read_bytes()
digest = 'd7cd79af299b9dfc1b26ed886000c455f950470c26cffdf796478609bffb6aa9'
require(len(seed) == 33554432 and seed[32:36] == b'NXSB' and hashlib.sha256(seed).hexdigest() == digest, 'Exact host-formatted scratch APFS input remains unchanged')
positive, absent = [base['report']('owner-hvf-scratch-data' + suffix) for suffix in ('', '-absent')]
for d in (positive, absent):
    require(d['kernel_sha256'] == expected and not d['bounded_timeout_interrupted'] and d['native_4k_mmu_observed'] and
            d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr'] == 80 and
            d['owner_cache_mount']['_native_owner_cache_status'] == 0 and d['owner_boot_manifest']['status'] == 0 and
            d['original_amfi_table_owner_observed'] and d['original_crypto_table_owner_observed'], 'Keep actual native-4K authenticated original disk root/cache path')
    require(d['stop_symbol'] == '_panic' and d['argument_memory']['x0'].startswith('offline scratch Data mount failed: %d') and
            d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0) and
            d['native_virtio_globals']['_native_virtio_read_failures'] == 0 and d['native_virtio_integrity']['_native_virtio_integrity_failures'] == 0,
            'Real mount failure must stop startup, without disk/signature failure substitution')
data = positive['scratch_data']
require(data['input']['sha256'] == data['actual_seed_sha256'] == digest and data['actual_seed_bytes'] == data['input']['bytes'] == len(seed) and
        data['input']['guest_mutable'] and not data['input']['initial_seed_signature_verified'] and not data['input']['production_storage_qualified'],
        'Hash the actual loaded memdev bytes; never label an unsigned mutable seed as an authenticated OS image')
require(data['state'] == dict(_native_owner_data_status=2, _native_owner_data_stage=2, _native_owner_data_readonly=-1, _native_owner_data_device=-1),
        'Original APFS mount of md0 returns real ENOENT before a Data mount exists')
data = absent['scratch_data']
require(data['input'] is None and data['actual_seed_bytes'] == 0 and data['actual_seed_sha256'] == '00' * 32 and
        data['state'] == dict(_native_owner_data_status=22, _native_owner_data_stage=0, _native_owner_data_readonly=-1, _native_owner_data_device=-1),
        'Missing scratch media must refuse early, not continue on a read-only Data namespace')
print('PASS: actual 32 MiB memdev bytes match host seed; genuine APFS mount refuses ENOENT 2')
print('PASS: absent scratch media refuses EINVAL 22; writable Data remains unqualified')
