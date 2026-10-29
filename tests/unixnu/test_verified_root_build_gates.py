from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class VerifiedRootBuildGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = (ROOT / "scripts/build-xnu-4k-baseline.sh").read_text()
        cls.desktop = (ROOT / "scripts/build-cpu-desktop-kernel.py").read_text()

    def test_audit_accepts_an_external_verified_provider_without_probe_prelink(self):
        start = self.build.index("OWNER_ROOT_PROBE=${XNU_VERIFIED_ROOT_PROBE:-0}")
        end = self.build.index("ADRENO_FIRMWARE_PROBE=", start)
        gates = self.build[start:end]
        self.assertIn("VERIFIED_ROOT_PROVIDER=${XNU_VERIFIED_ROOT_PROVIDER:-0}", gates)
        self.assertIn("[ \"$OWNER_ROOT_PROBE\" != 1 ] && [ \"$VERIFIED_ROOT_PROVIDER\" != 1 ]", gates)
        self.assertIn("[ \"$VERIFIED_ROOT_PROVIDER\" != 1 ] && [ \"${XNU_NATIVE_VIRTIO_BLOCK:-0}\" != 1 ]", gates)
        self.assertIn("-DXNU_VERIFIED_ROOT_PROVIDER=$VERIFIED_ROOT_PROVIDER", self.build)

    def test_provider_mode_invalidates_the_cached_bsd_init_object(self):
        start = self.build.index("OWNER_ROOT_STAMP=")
        end = self.build.index("OWNER_CACHE_MOUNT=", start)
        block = self.build[start:end]
        self.assertIn("$OWNER_ROOT_PROBE:$VERIFIED_ROOT_PROVIDER:$OWNER_ROOT_AUDIT:$OWNER_ROOT_TRUST", block)
        self.assertIn("bsd/DEVELOPMENT/bsd_init.o", block)

    def test_qemu_source_storage_virtio_is_explicit_and_separate_from_hardware_aes_mode(self):
        start = self.build.index('QEMU_SOURCE_STORAGE_VIRTIO=${XNU_NATIVE_VIRTIO_QEMU_SOURCE_STORAGE:-0}')
        end = self.build.index('NATIVE_RAM_BLOCK=', start)
        gate = self.build[start:end]
        for requirement in ('XNU_SOURCE_STORAGE_FAMILY', 'XNU_SOURCE_CORECRYPTO14',
                            'CRYPTO_BOOTSTRAP_MODE', 'XNU_VERIFIED_ROOT_PROVIDER',
                            'XNU_OWNER_ROOT_AUDIT', 'XNU_OWNER_ROOT_TRUST'):
            self.assertIn(requirement, gate)
        self.assertIn('XNU_NATIVE_AES', gate)
        self.assertIn('XNU_NATIVE_UFS', gate)
        self.assertIn('XNU_NATIVE_RAM_BLOCK', gate)
        self.assertIn('-DXNU_NATIVE_VIRTIO_QEMU_SOURCE_STORAGE=1', gate)
        self.assertIn('"$QEMU_SOURCE_STORAGE_VIRTIO"', self.build)

    def test_desktop_profile_exports_the_new_boolean(self):
        self.assertIn("VERIFIED_ROOT_PROVIDER", self.desktop)


if __name__ == "__main__":
    unittest.main()
