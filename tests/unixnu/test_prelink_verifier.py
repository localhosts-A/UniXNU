#!/usr/bin/env python3
"""Verify that the independent audit recomputes emitted structure and bindings."""
import copy
import hashlib
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'kits/NativePrelink')]
from emit_prelink import emit
from test_emit_prelink import images
from verify_prelink import verify


class VerifyPrelinkTests(unittest.TestCase):
    def candidate(self):
        native, original = images()
        candidate, report = emit(native, original, ['test.component'], 0x4000)
        audit = verify(native, original, candidate, report, ['test.component'], 0x4000)
        return native, original, candidate, report, audit

    def test_independently_verifies_pointer_table_and_mach_o_layout(self):
        _native, _original, _candidate, _report, audit = self.candidate()
        self.assertEqual(audit['pointer_table_rebindings_verified'], 4)
        self.assertEqual(audit['component_headers_and_bytes_verified'], 1)
        self.assertTrue(audit['kmod_and_sparse_index_verified'])
        self.assertFalse(audit['instruction_relocation_complete'])
        self.assertFalse(audit['abi_qualified'])
        self.assertFalse(audit['loadable'])

    def test_refuses_candidate_pointer_tampering(self):
        native, original, candidate, report, _audit = self.candidate()
        changed = bytearray(candidate)
        from native_prelink import backed_offset, macho
        site = report['bindings'][0]['new_site']
        offset = backed_offset(macho(candidate)['segments'], site, 8, len(candidate))
        changed[offset] ^= 1
        forged = copy.deepcopy(report)
        forged['candidate_sha256'] = hashlib.sha256(changed).hexdigest()
        with self.assertRaisesRegex(ValueError, 'Candidate pointer'):
            verify(native, original, bytes(changed), forged, ['test.component'], 0x4000)

    def test_refuses_report_claiming_unverified_loadability(self):
        native, original, candidate, report, _audit = self.candidate()
        forged = copy.deepcopy(report)
        forged['loadable'] = True
        with self.assertRaisesRegex(ValueError, 'claims unverified loadability'):
            verify(native, original, candidate, forged, ['test.component'], 0x4000)

    def test_refuses_kernelcache_or_candidate_hash_mismatch(self):
        native, original, candidate, report, _audit = self.candidate()
        bad = copy.deepcopy(report)
        bad['original_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'hashes or slide'):
            verify(native, original, candidate, bad, ['test.component'], 0x4000)


if __name__ == '__main__':
    unittest.main()
