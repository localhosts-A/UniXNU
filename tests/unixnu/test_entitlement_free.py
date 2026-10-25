"""Compile the production empty-entitlement guard and exercise its refusal edges."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class EntitlementFreeTests(unittest.TestCase):
    def test_actual_loader_keeps_signature_failures_and_requires_a_signature(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler required')
        patch = Path(__file__).resolve().parents[2] / 'patches/xnu/source-entitlement-free-context.patch'
        loader = patch.read_text().split('diff --git a/bsd/kern/mach_loader.c', 1)[1]
        new_side = '\n'.join(line[1:] for line in loader.splitlines()
                             if line.startswith(('+', ' ')) and not line.startswith('+++'))
        helper = new_side[new_side.index('static boolean_t\nload_code_signature_enforced('):]
        helper = helper[:helper.index('\n}\n') + len('\n}\n')]
        failure = new_side[new_side.index('\t\t\t\tif (ret != LOAD_SUCCESS) {'):]
        failure = failure[:failure.index('\n\t\t\t\tif (got_code_signatures) {')]
        missing = new_side[new_side.index('\t\tif (!got_code_signatures &&'):]
        missing = missing[:missing.index('\n\t\t}') + len('\n\t\t}')]
        invalid_page = new_side[new_side.index('\t\t\t\t\t\t\tif (load_code_signature_enforced() ||'):]
        invalid_page = invalid_page[:invalid_page.index('\n\t\t\t\t\t\t\tresult->csflags')]
        source = r'''
#include <assert.h>
typedef int boolean_t;
#define TRUE 1
#define FALSE 0
#define LOAD_SUCCESS 0
#define LOAD_FAILURE 1
#define CS_HARD 1
#define CS_KILL 2
#define CS_ENFORCEMENT 4
#define printf(...) ((void)0)
static int enforcement;
static boolean_t cs_process_global_enforcement(void) { return enforcement; }
''' + helper + r'''
static int load_for_test(int signature_command, int signature_result) {
    int ret = LOAD_SUCCESS;
    boolean_t got_code_signatures = FALSE;
    if (signature_command) {
        ret = signature_result;
''' + failure + r'''
    }
    if (ret == LOAD_SUCCESS) {
''' + missing + r'''
    }
    return ret;
}
static int reject_invalid_page(unsigned flags) {
    int ret = LOAD_SUCCESS;
    struct { unsigned csflags; } state = { flags }, *result = &state;
''' + invalid_page + r'''
    return ret;
}
int main(void) {
    for (enforcement = 0; enforcement <= 1; enforcement++) {
        const int strict = XNU_SOURCE_ENTITLEMENT_FREE || enforcement;
        assert(load_for_test(1, LOAD_SUCCESS) == LOAD_SUCCESS);
        /* EPERM has already become LOAD_FAILURE here. No failed signature
         * may become executable unsigned code in the restricted profile.
         */
        for (int failure = 1; failure <= 8; failure++) {
            assert(load_for_test(1, failure) == (strict ? failure : LOAD_SUCCESS));
        }
        assert(load_for_test(0, LOAD_SUCCESS) == (strict ? LOAD_FAILURE : LOAD_SUCCESS));
        assert(reject_invalid_page(0) == (strict ? LOAD_FAILURE : LOAD_SUCCESS));
        for (unsigned flag = CS_HARD; flag <= CS_ENFORCEMENT; flag *= 2) {
            assert(reject_invalid_page(flag) == LOAD_FAILURE);
        }
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'loader.c'
            path.write_text(source)
            for enabled in (0, 1):
                output = Path(temporary) / f'loader-{enabled}'
                subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', f'-DXNU_SOURCE_ENTITLEMENT_FREE={enabled}',
                                path, '-o', output], check=True)
                subprocess.run([output], check=True)

    def test_actual_task_query_denies_absent_provider_and_preserves_real_provider(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('native C++ compiler required')
        path = Path(__file__).resolve().parents[2] / 'patches/xnu/source-entitlement-free-context.patch'
        iokit = path.read_text().split('diff --git a/iokit/bsddev/IOKitBSDInit.cpp', 1)[1].split('diff --git ', 1)[0]
        new_side = '\n'.join(line[1:] for line in iokit.splitlines()
                             if line.startswith(('+', ' ')) and not line.startswith('+++'))
        function = new_side[new_side.index('extern "C" boolean_t\nIOTaskHasStringEntitlement('):]
        source = r'''
#include <cstddef>
#include <cstdint>
#include <cassert>
using boolean_t = bool;
using task_t = void*;
using proc_t = void*;
using kern_return_t = int;
static const int KERN_SUCCESS = 0;
static task_t kernel_task = (void*)1;
static task_t current_task() { return (void*)2; }
static proc_t get_bsdtask_info(task_t task) { return task; }
static unsigned provider_calls;
static int provider_result;
struct OSObject {};
struct OSString : OSObject {};
static OSString provider_object;
static char provider_string[] = "owned-value";
static unsigned released_objects;
static char *copyOSStringAsCString(OSString *value) { assert(value == &provider_object); return provider_string; }
#define OSDynamicCast(type,obj) static_cast<type*>(obj)
#define OSSafeReleaseNULL(obj) do { (obj) = nullptr; released_objects++; } while (0)
static int provider_query(proc_t proc, const char *name) {
    assert(proc == (void*)2 && name != nullptr);
    provider_calls++;
    return provider_result;
}
static int provider_string_query(proc_t proc, const char *name, const char *value) {
    assert(value != nullptr);
    return provider_query(proc, name);
}
static int provider_copy_query(proc_t proc, const char *name, void **value) {
    int result = provider_query(proc, name);
    if (result == KERN_SUCCESS) *value = &provider_object;
    return result;
}
struct Provider { struct {
    int (*queryEntitlementBooleanWithProc)(proc_t, const char*);
    int (*queryEntitlementStringWithProc)(proc_t, const char*, const char*);
    int (*copyEntitlementAsOSObjectWithProc)(proc_t, const char*, void**);
} OSEntitlements; };
static Provider *amfi;
static uint64_t source_entitlement_free_query_denials;
#define os_atomic_inc(p,order) (++*(p))
''' + function + r'''
int main() {
    const char *name = "com.apple.private.custom-x18-abi";
    assert(!IOTaskHasEntitlement(kernel_task, name));
    assert(!IOTaskHasEntitlement((void*)2, nullptr));
    assert(!IOTaskHasStringEntitlement(kernel_task, name, "value"));
    assert(!IOTaskHasStringEntitlement((void*)2, name, nullptr));
    assert(IOTaskGetEntitlement(kernel_task, name) == nullptr);
    assert(IOTaskGetEntitlement((void*)2, nullptr) == nullptr);
    assert(provider_calls == 0 && source_entitlement_free_query_denials == 0);
#if XNU_SOURCE_ENTITLEMENT_FREE
    assert(!IOTaskHasEntitlement((void*)2, name));
    assert(!IOTaskHasEntitlement(nullptr, "com.apple.private.uexc"));
    assert(!IOTaskHasStringEntitlement((void*)2, name, "value"));
    assert(IOTaskGetEntitlement(nullptr, name) == nullptr);
    assert(provider_calls == 0 && source_entitlement_free_query_denials == 4);
#endif
    const auto denied = source_entitlement_free_query_denials;
    Provider provider = {{provider_query, provider_string_query, provider_copy_query}};
    amfi = &provider;
    assert(IOTaskHasEntitlement((void*)2, name));
    assert(IOTaskHasStringEntitlement(nullptr, name, "value"));
    assert(IOTaskGetEntitlement((void*)2, name) == provider_string);
    assert(released_objects == 1);
    provider_result = 5;
    assert(!IOTaskHasEntitlement(nullptr, name));
    assert(!IOTaskHasStringEntitlement((void*)2, name, "value"));
    assert(IOTaskGetEntitlement(nullptr, name) == nullptr);
    assert(provider_calls == 6 && released_objects == 1 && source_entitlement_free_query_denials == denied);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source_path = Path(temporary) / 'task-query.cpp'
            source_path.write_text(source)
            for enabled in (0, 1):
                output = Path(temporary) / f'task-query-{enabled}'
                subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                f'-DXNU_SOURCE_ENTITLEMENT_FREE={enabled}', source_path, '-o', output], check=True)
                subprocess.run([output], check=True)

    def test_production_guard_and_default_monitor_provider_paths(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler required')
        patch = Path(__file__).resolve().parents[2] / 'patches/xnu/source-entitlement-free-context.patch'
        # A normal contextual diff retains the unchanged function declaration
        # as context. Compile the new-side view, not additions alone.
        added = '\n'.join(line[1:] for line in patch.read_text().splitlines()
                          if line.startswith(('+', ' ')) and not line.startswith('+++'))
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
