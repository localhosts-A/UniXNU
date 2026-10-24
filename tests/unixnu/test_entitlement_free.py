"""Compile the production empty-entitlement guard and exercise its refusal edges."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class EntitlementFreeTests(unittest.TestCase):
    def test_production_guard_and_default_monitor_provider_paths(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler required')
        patch = Path(__file__).resolve().parents[2] / 'patches/xnu/source-entitlement-free-context.patch'
        added = '\n'.join(line[1:] for line in patch.read_text().splitlines()
                          if line.startswith('+') and not line.startswith('+++'))
        condition = '#if defined(XNU_SOURCE_ENTITLEMENT_FREE)'
        helper_begin = added.index(condition)
        helper_end = added.index('static errno_t\naccelerate_entitlement_queries(')
        helper = added[helper_begin:helper_end]
        begin = added.index(condition, helper_end)
        end = added.index('#endif', begin) + len('#endif')
        guard = added[begin:end]
        source = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define CS_VALID 1
#define CS_ADHOC 2
#define EPERM 1
typedef struct { int value; } CS_GenericBlob;
typedef int errno_t;
struct cs_blob { unsigned csb_flags; void *csb_entitlements; int xml_error,der_error; void *xml; const CS_GenericBlob *der; size_t xml_length,der_length; void *csb_entitlements_blob,*csb_der_entitlements_blob; };
static void *amfi;
static unsigned getters,original_calls;
#define os_atomic_inc(p,order) (++*(p))
static int csblob_get_entitlements(struct cs_blob *b,void **p,size_t *n) { getters++; *p=b->xml; *n=b->xml_length; return b->xml_error; }
static int csblob_get_der_entitlements(struct cs_blob *b,const CS_GenericBlob **p,size_t *n) { getters++; *p=b->der; *n=b->der_length; return b->der_error; }
''' + helper + r'''
static int actual_guard(struct cs_blob *cs_blob) {
''' + guard + r'''
  (void)amfi; original_calls++; return 77;
}
int main(void) {
  struct cs_blob b={.csb_flags=CS_VALID|CS_ADHOC};
#if XNU_SOURCE_ENTITLEMENT_FREE && !CODE_SIGNING_MONITOR
  assert(actual_guard(&b)==0 && source_entitlement_free_accepts==1 && getters==2);
  for (int i=0;i<11;i++) {
    struct cs_blob bad=b;
    switch(i) {
      case 0: bad.xml=(void*)1; break;
      case 1: bad.der=(const CS_GenericBlob*)1; break;
      case 2: bad.xml_error=85; break;
      case 3: bad.der_error=85; break;
      case 4: bad.csb_entitlements=(void*)1; break;
      case 5: bad.csb_flags=CS_ADHOC; break;
      case 6: bad.csb_flags=CS_VALID; break;
      case 7: bad.xml_length=1; break;
      case 8: bad.der_length=1; break;
      case 9: bad.csb_entitlements_blob=(void*)1; break;
      case 10: bad.csb_der_entitlements_blob=(void*)1; break;
    }
    assert(actual_guard(&bad)==EPERM);
  }
  assert(source_entitlement_free_refusals==11 && source_entitlement_free_accepts==1 && original_calls==0);
  assert(source_entitlement_free_entitlement_refusals==6);
#else
  assert(actual_guard(&b)==77 && original_calls==1 && getters==0);
#endif
  amfi=(void*)1;
  unsigned old=original_calls;
  assert(actual_guard(&b)==77 && original_calls==old+1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'guard.c'
            path.write_text(source)
            for enabled, monitor in ((1, 0), (0, 0), (0, 1)):
                out = Path(directory) / f'guard-{enabled}-{monitor}'
                subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                                f'-DXNU_SOURCE_ENTITLEMENT_FREE={enabled}', f'-DCODE_SIGNING_MONITOR={monitor}',
                                path, '-o', out], check=True)
                subprocess.run([out], check=True)
            invalid = subprocess.run([compiler, '-std=c11', '-DXNU_SOURCE_ENTITLEMENT_FREE=1',
                                      '-DCODE_SIGNING_MONITOR=1', path, '-o', Path(directory) / 'invalid'],
                                     capture_output=True, text=True)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn('requires no code signing monitor', invalid.stderr)
