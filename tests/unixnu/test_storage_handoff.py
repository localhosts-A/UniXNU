"""Compile DarwinBoot's real optional-protocol consumer with mock firmware."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StorageHandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler required')
        root = Path(__file__).resolve().parents[2]
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base / 'Library').mkdir()
        (base / 'Uefi.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint64_t UINT64; typedef size_t UINTN; typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE; typedef struct { unsigned char bytes[16]; } EFI_GUID;
#define VOID void
#define IN
#define EFIAPI
#define EFIERR(n) (0x8000000000000000ULL | (n))
#define EFI_ERROR(n) (((EFI_STATUS)(n) >> 63) != 0)
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED EFIERR(3)
#define EFI_BAD_BUFFER_SIZE EFIERR(4)
#define EFI_DEVICE_ERROR EFIERR(7)
#define EFI_OUT_OF_RESOURCES EFIERR(9)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_TIMEOUT EFIERR(18)
#define EFI_COMPROMISED_DATA EFIERR(33)
#define ByProtocol 2
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID*,void*,UINTN*,EFI_HANDLE**);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID*,void**);
} EFI_BOOT_SERVICES;
extern EFI_BOOT_SERVICES *gBS;
''')
        (base / 'Library/MemoryAllocationLib.h').write_text('#include <Uefi.h>\nVOID FreePool(VOID*);\n')
        (base / 'Library/UefiBootServicesTableLib.h').write_text('#include <Uefi.h>\n')
        (base / 'harness.c').write_text('''#include <Uefi.h>
#include <Protocol/UniXNUStorageQuiesce.h>
#include "StorageHandoff.h"
#include <string.h>
EFI_GUID gUniXNUStorageQuiesceProtocolGuid;
static struct {
  UNIXNU_STORAGE_QUIESCE_PROTOCOL providers[2]; EFI_HANDLE handles[2];
  unsigned scenario,locates,lookups,frees,calls[2],actions[2],closed[2];
  EFI_STATUS result[2];
} p;
static EFI_STATUS quiesce(UNIXNU_STORAGE_QUIESCE_PROTOCOL *self) {
  unsigned i=self==&p.providers[0] ? 0 : 1;
  ++p.calls[i];
  if (!p.closed[i]) { ++p.actions[i]; p.closed[i]=1; }
  return p.result[i];
}
static EFI_STATUS locate(int type,EFI_GUID *guid,void *registration,UINTN *count,EFI_HANDLE **handles) {
  (void)type; (void)guid; (void)registration; ++p.locates;
  if (p.scenario==0) return EFI_NOT_FOUND;
  if (p.scenario==6) return EFI_OUT_OF_RESOURCES;
  *count=p.scenario==7 ? 65 : p.scenario==8 ? 0 : 2;
  *handles=p.scenario==9 ? NULL : p.handles;
  return p.scenario==18 ? 1 : EFI_SUCCESS;
}
static EFI_STATUS protocol(EFI_HANDLE handle,EFI_GUID *guid,void **provider) {
  (void)guid; ++p.lookups;
  if (p.scenario==5 && handle==p.handles[0]) return EFI_DEVICE_ERROR;
  if (p.scenario==19 && handle==p.handles[0]) return 1;
  *provider=(p.scenario==11 && handle==p.handles[0]) ? NULL : handle;
  return EFI_SUCCESS;
}
VOID FreePool(VOID *memory) { (void)memory; ++p.frees; }
static EFI_BOOT_SERVICES services={locate,protocol};
EFI_BOOT_SERVICES *gBS=&services;
void run(unsigned scenario,UINT64 *out) {
  memset(&p,0,sizeof p); p.scenario=scenario;
  for (unsigned i=0;i<2;i++) {
    p.providers[i].Revision=UNIXNU_STORAGE_QUIESCE_REVISION;
    p.providers[i].Quiesce=quiesce; p.handles[i]=&p.providers[i];
  }
  if (scenario==2) p.providers[0].Revision=2;
  if (scenario==3) p.providers[0].Quiesce=NULL;
  if (scenario==4 || scenario==13) p.result[0]=EFI_TIMEOUT;
  if (scenario==13) p.result[1]=EFI_DEVICE_ERROR;
  if (scenario==10) p.handles[1]=p.handles[0];
  if (scenario==12) p.handles[0]=NULL;
  if (scenario==14) p.providers[0].Revision=0;
  if (scenario==17) p.result[0]=1;
  UINTN completed=999;
  out[0]=DarwinQuiesceStorage(scenario==16 ? NULL : &completed); out[1]=completed;
  out[2]=p.locates; out[3]=p.lookups; out[4]=p.frees;
  out[5]=p.calls[0]; out[6]=p.calls[1]; out[7]=p.actions[0]; out[8]=p.actions[1];
  out[9]=DarwinQuiesceStorage(scenario==16 ? NULL : &completed); out[10]=completed;
  out[11]=p.frees; out[12]=p.calls[0]; out[13]=p.calls[1]; out[14]=p.actions[0]; out[15]=p.actions[1];
}
''')
        driver = root / 'drivers/DarwinBoot/DarwinBootPkg/Driver'
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(root / 'package/UniXNUPkg/Include'), '-I', str(driver),
                        str(driver / 'StorageHandoff.c'), str(base / 'harness.c'),
                        '-o', str(base / 'handoff.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'handoff.so'))
        cls.lib.run.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, scenario):
        out = (ctypes.c_uint64 * 16)()
        self.lib.run(scenario, out)
        return list(out)

    def test_absent_optional_protocol_preserves_normal_boot(self):
        self.assertEqual(self.run_case(0), [0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])

    def test_every_provider_is_called_and_repeated_handoff_stays_closed(self):
        out = self.run_case(1)
        self.assertEqual(out[:9], [0, 2, 1, 2, 1, 1, 1, 1, 1])
        self.assertEqual(out[9:], [0, 2, 2, 2, 2, 1, 1])
        out = self.run_case(10)
        self.assertEqual((out[0], out[1], out[12], out[14], out[15]), (0, 2, 4, 1, 0))

    def test_malformed_interfaces_do_not_skip_later_providers(self):
        for scenario, error in ((2, 3), (3, 33), (5, 7), (11, 33), (12, 33), (14, 3)):
            with self.subTest(scenario=scenario):
                out = self.run_case(scenario)
                self.assertEqual(out[0], (1 << 63) | error)
                self.assertEqual(out[1], 1)
                self.assertEqual((out[5], out[6], out[7], out[8]), (0, 1, 0, 1))
                self.assertEqual((out[9], out[10], out[11], out[15]), (out[0], 1, 2, 1))

    def test_failure_remains_failure_and_all_remaining_controllers_are_attempted(self):
        for scenario, completed in ((4, 1), (13, 0)):
            out = self.run_case(scenario)
            self.assertEqual((out[0], out[1], out[5], out[6]), ((1 << 63) | 18, completed, 1, 1))
            self.assertEqual(out[9:], [out[0], completed, 2, 2, 2, 1, 1])

    def test_bad_enumeration_and_allocation_failure_never_call_a_provider(self):
        for scenario, error, frees in ((6, 9, 0), (7, 4, 1), (8, 33, 1), (9, 33, 0)):
            with self.subTest(scenario=scenario):
                out = self.run_case(scenario)
                self.assertEqual((out[0], out[1], out[3], out[4]), ((1 << 63) | error, 0, 0, frees))
                self.assertEqual(out[5:9], [0, 0, 0, 0])

    def test_null_output_is_rejected_before_any_boot_service(self):
        out = self.run_case(16)
        self.assertEqual(out[:5], [(1 << 63) | 2, 999, 0, 0, 0])

    def test_warning_cannot_authorize_kernel_entry(self):
        for scenario, completed, lookups in ((17, 1, 2), (18, 0, 0), (19, 1, 2)):
            with self.subTest(scenario=scenario):
                out = self.run_case(scenario)
                self.assertEqual((out[0], out[1], out[3]), ((1 << 63) | 7, completed, lookups))
                self.assertEqual((out[9], out[10], out[11]), (out[0], completed, 2))


if __name__ == '__main__':
    unittest.main()
