"""Compile the actual portable UFS command core with a hardware-only simulator."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UfsTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('clang++') or shutil.which('c++')
        if not cc:
            raise unittest.SkipTest('C++ compiler required')
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[2]
        core = root/'drivers/UfsTransport'
        library = Path(cls.temp.name)/'core.so'
        subprocess.run([cc, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I', str(core), str(core/'UfsTransport.cpp'),
                        str(Path(__file__).with_name('ufs_transport_harness.cpp')), '-o', str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.ufs_core_case.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.ufs_identity_case.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.ufs_tur_probe_case.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.ufs_reset_data_case.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def case(self, scenario=0):
        out = (ctypes.c_uint64*32)()
        self.lib.ufs_core_case(scenario, out)
        self.assertEqual((out[11], out[25]), (0, 0), 'malformed wire descriptor/CDB')
        return list(out)

    def test_real_lun_geometry_512_4096_high_dma_and_read16(self):
        for scenario, size, opcode in ((0,4096,0x28),(1,512,0x28),(2,4096,0x28),(3,4096,0x88)):
            out = self.case(scenario)
            self.assertEqual(out[:4], [0,0,0,0])
            self.assertEqual((out[4],out[6],out[7],out[8],out[12],out[14],out[17],out[19]), (1,23,0,opcode,size,1,2,0))
            self.assertEqual(out[5], 131072 if scenario==1 else 4096)
            self.assertEqual(out[21:25], [0,0,1,0])

    def test_dma_window_refuses_high_physical_address_before_command(self):
        out = self.case(28)
        self.assertEqual((out[0],out[9]), (1,0))

    def test_disabled_lun_and_mismatched_descriptor_or_capacity_never_publish_geometry(self):
        for scenario, expected in ((17,8),(18,8),(19,2)):
            out = self.case(scenario)
            self.assertEqual((out[2],out[12],out[13]), (expected,0,0))

    def test_timeout_and_zero_doorbell_without_ocs_never_copy_or_replay(self):
        for scenario in (4,5):
            out = self.case(scenario)
            self.assertEqual((out[3],out[4],out[5],out[6],out[7]), (4,1,0,0xa5,1))
            self.assertEqual((out[19],out[20]), (2,out[9]))

    def test_ocs_tag_lun_transaction_sense_or_uic_faults_quarantine(self):
        for scenario, error in ((6,5),(7,6),(8,6),(9,6),(10,6),(11,5)):
            out = self.case(scenario)
            self.assertEqual((out[3],out[6],out[7]), (error,0xa5,1))
            self.assertEqual((out[19],out[20]), (2,out[9]))

    def test_cache_failure_never_reports_data_or_replays(self):
        for scenario, rings in ((12,0),(13,1)):
            out = self.case(scenario)
            self.assertEqual((out[3],out[4],out[6],out[7]), (9,rings,0xa5,1))
            self.assertEqual((out[19],out[20]), (2,out[9]))

    def test_partial_read_copies_only_valid_prefix_and_partial_write_is_not_replayed(self):
        for scenario in (14,15):
            out = self.case(scenario)
            self.assertEqual((out[3],out[4],out[5],out[7],out[19],out[20]), (10,1,2048,1,2,out[9]))
        self.assertEqual((self.case(14)[6],self.case(14)[16]), (23,0xa5))

    def test_default_denial_unit_write_protect_revoked_grant_and_range_overflow_never_ring(self):
        for scenario, error in ((16,3),(20,3),(23,3),(24,1)):
            out = self.case(scenario)
            self.assertEqual((out[3],out[4],out[7]), (error,0,0))

    def test_valid_external_grant_and_real_sync_dispatch(self):
        out = self.case(25)
        self.assertEqual((out[3],out[4],out[5],out[8],out[15],out[19]), (0,1,4096,0x2a,2,0))

    def test_sync_failure_and_uncertain_write_doorbell_poison_without_replay(self):
        for scenario, error in ((26,7),(27,5)):
            out = self.case(scenario)
            self.assertEqual((out[3],out[4],out[7],out[19],out[20]), (error,1,1,2,out[9]))

    def test_changed_trl_or_untracked_request_is_rejected_before_touching_descriptors(self):
        for scenario in (21,22,34):
            out = self.case(scenario)
            self.assertEqual((out[3],out[4],out[6],out[7],out[20]), (5,0,0xa5,1,out[9]))
            self.assertEqual(out[30], 1)

    def test_known_reset_unit_attention_only_tur_has_three_attempt_bound(self):
        out = self.case(32)
        self.assertEqual((out[0],out[1],out[2],out[29]), (0,0,0,3))
        out = self.case(33)
        self.assertEqual((out[1],out[2],out[7],out[29]), (0,7,0,1))

    def test_stuck_initialization_flag_is_bounded_and_set_once(self):
        out = self.case(30)
        self.assertEqual((out[1],out[2],out[7],out[14]), (4,2,1,1))

    def tur(self, scenario):
        out = (ctypes.c_uint64*16)()
        self.lib.ufs_tur_probe_case(scenario, out)
        self.assertEqual((out[0], out[1], out[11]), (0, 0, 0))
        return list(out)

    def test_bus_reset_ua_acknowledged_by_tur_before_capacity_fixed_and_descriptor_sense(self):
        for scenario in (1, 10, 12):
            with self.subTest(scenario=scenario):
                out = self.tur(scenario)
                self.assertEqual((out[2], out[3], out[4], out[9], out[10]), (0, 2, 1, 0, 1))

    def test_repeated_reset_ua_stops_at_three_turs_without_capacity_or_geometry(self):
        for scenario, ascq in ((2, 2), (11, 0)):
            out = self.tur(scenario)
            self.assertEqual(out[2:11], [7, 3, 0, 2, 6, 0x29, ascq, 0, 0])
            self.assertEqual(out[12:15], [2, 2, 0])

    def test_other_ascq_sense_key_asc_or_target_status_is_not_retried(self):
        for scenario in (3, 4, 5, 6, 13):
            out = self.tur(scenario)
            self.assertEqual((out[2], out[3], out[4], out[9], out[10]), (7, 1, 0, 0, 0))
            self.assertEqual(out[12:15], [2, 2, 0])

    def test_tur_timeout_ocs_and_protocol_errors_quarantine_after_one_submission(self):
        for scenario, status in ((7, 4), (8, 5), (9, 6)):
            out = self.tur(scenario)
            self.assertEqual((out[2], out[3], out[4], out[9], out[10]), (status, 1, 0, 1, 0))
            self.assertEqual(out[12:15], [2, 2, 0])

    def test_bus_reset_on_read_sync_or_write_never_replays_the_command(self):
        for operation in (0, 1, 2):
            out = (ctypes.c_uint64*10)()
            self.lib.ufs_reset_data_case(operation, out)
            self.assertEqual(list(out[:8]), [0, 0, 0, 7, 1, 6, 0x29, 2])
            self.assertEqual((out[8], out[9]), (0 if operation == 0 else 1, 0))

    def test_absent_lun_query_result_is_preserved_without_geometry_or_scsi_retry(self):
        out = self.case(36)
        self.assertEqual((out[1],out[2],out[7],out[12],out[13],out[18],out[29]), (0,7,0,0,0,0xfc,0))

    def test_frozen_clock_still_has_hard_poll_bound(self):
        out = self.case(31)
        self.assertEqual((out[3],out[4],out[7],out[26],out[20]), (4,1,1,300001,out[9]))
        self.assertEqual((out[27],out[28],out[31]), (4,4,out[9]))

    def test_wide_capacity_write16_uses_same_lun_and_policy(self):
        out = self.case(35)
        self.assertEqual((out[3],out[8],out[15],out[17],out[19]), (0,0x8a,2,2,0))

    def test_device_identity_uses_serial_descriptor_and_rejects_missing_or_malformed(self):
        def identity(fault=0):
            out = (ctypes.c_uint64*8)(); raw = (ctypes.c_uint8*256)()
            self.lib.ufs_identity_case(fault, out, raw)
            return list(out), bytes(raw[:out[3]])
        out, raw = identity()
        self.assertEqual((out[0],out[1],out[2],out[3],out[6]), (0,0,0,8,0))
        self.assertEqual(raw, bytes((0x1f,0x3a,6,5,0,ord('D'),0,ord('A'))))
        for fault in (37,39):
            out, _ = identity(fault)
            self.assertEqual((out[2],out[6]), (6,1))
        out, _ = identity(38)
        self.assertEqual((out[2],out[6]), (3,0))
