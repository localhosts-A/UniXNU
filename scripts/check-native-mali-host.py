#!/usr/bin/env python3
"""Compile and run actual NativeMali core fixtures; no GPU/device is accessed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = ROOT / 'drivers/NativeMali'
    out = ROOT / 'build/native-mali-host'
    out.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(os.environ.get('CXX', 'clang++'))
    if not compiler:
        raise SystemExit('A C++17 host compiler is required')
    tests = [('mmu', ['NativeMaliMmu.cpp']), ('firmware', ['NativeMaliFirmware.cpp']),
             ('resources', ['NativeMaliResources.cpp']),
             ('csf', ['NativeMaliCsf.cpp']),
             ('hardware', ['NativeMaliHardware.cpp']),
             ('platform', ['NativeMaliPlatform.cpp', 'NativeMaliResources.cpp']),
             ('boot', ['NativeMaliBoot.cpp', 'NativeMaliScheduler.cpp', 'NativeMaliSession.cpp', 'NativeMaliSha256.cpp', 'NativeMaliCsf.cpp', 'NativeMaliHardware.cpp',
                       'NativeMaliPlatform.cpp', 'NativeMaliResources.cpp', 'NativeMaliMmu.cpp',
                       'NativeMaliFirmware.cpp', 'NativeMaliFirmwareMemory.cpp', 'NativeMaliFirmwareSetup.cpp']),
             ('firmware-memory', ['NativeMaliMmu.cpp', 'NativeMaliFirmware.cpp',
                                  'NativeMaliFirmwareMemory.cpp', 'NativeMaliFirmwareSetup.cpp'])]
    report = {'schema': 1, 'compiler': subprocess.check_output([compiler, '--version'], text=True),
              'hardware_exercised': False, 'cases': {}}
    for name, units in tests:
        files = [source / unit for unit in units] + [source / 'tests' / (name + '.cpp')]
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                   '-fno-exceptions', '-fno-rtti', '-I', str(source)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        executable = out / name
        command += [*map(str, files), '-o', str(executable)]
        subprocess.run(command, check=True)
        # Apple clang's ASan does not support LeakSanitizer. Enabling it aborts
        # before main(), independently of the production core under test.
        asan = 'abort_on_error=1' if sys.platform == 'darwin' else 'detect_leaks=1:abort_on_error=1'
        env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS=asan)
        result = subprocess.run([str(executable)], env=env, capture_output=True, text=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            print(result.stdout + result.stderr, file=sys.stderr)
            result.check_returncode()
        print(result.stdout, end='')
        report['cases'][name] = {'command': command, 'output': result.stdout,
                                'sources': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                            for p in files + sorted(source.glob('*.h'))}}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
