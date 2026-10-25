"""Execute the portable production C implementation with sanitizers and fault injection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UsbCoreTests(unittest.TestCase):
    def run_fixture(self, source, expected, extra=(), production=()):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        self.assertIsNotNone(compiler, "C compiler is required for USB production-code tests")
        with tempfile.TemporaryDirectory(prefix="unixnu-usb-") as tmp:
            binary = Path(tmp) / "usb-core-test"
            command = [compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-Wconversion", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                       "-I", str(ROOT / "drivers/UsbCore/include"),
                       str(ROOT / "drivers/UsbCore/UsbCore.c"), *production, *extra, str(Path(__file__).with_name(source)),
                       "-o", str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(expected, result.stdout)
            print(result.stdout.strip())

    def test_production_core(self):
        self.run_fixture("usb_core_test.c", "PASS: USB production core")

    def test_host_enumeration(self):
        self.run_fixture("usb_host_test.c", "PASS: USB production host enumeration",
                         production=[str(ROOT / "drivers/UsbCore/UsbHost.c")])

    def test_host_hub(self):
        self.run_fixture("usb_hub_test.c", "PASS: USB production hub",
                         production=[str(ROOT / "drivers/UsbCore/UsbHost.c")])

    def test_mtu3_qmu(self):
        self.run_fixture("usb_qmu_test.c", "PASS: MTU3 QMU production", [str(ROOT / "drivers/UsbCore/UsbMtu3Qmu.c")])

    def test_mtu3_ep0(self):
        self.run_fixture("usb_ep0_test.c", "PASS: MTU3 EP0 production", [str(ROOT / "drivers/UsbCore/UsbMtu3Ep0.c")])

    def test_mtu3_endpoints(self):
        self.run_fixture("usb_endpoints_test.c", "PASS: MTU3 endpoint production", [str(ROOT / "drivers/UsbCore/UsbMtu3Endpoints.c")])

    def test_mtu3_device(self):
        self.run_fixture("usb_device_test.c", "PASS: MTU3 device production", [str(ROOT / "drivers/UsbCore" / name) for name in
            ("UsbMtu3Qmu.c", "UsbMtu3Ep0.c", "UsbMtu3Endpoints.c", "UsbMtu3Device.c")])


if __name__ == "__main__":
    unittest.main()
