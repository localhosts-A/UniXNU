#!/usr/bin/env python3
"""Write the compiler-qualified process/thread layout for an XNU build."""
import argparse
import json
from pathlib import Path

from process_layout import build_process_layout

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--objects-dir', type=Path,
                    default=ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837',
                    help='Exact XNU object directory containing the native kernel and DWARF objects')
parser.add_argument('--output', type=Path, default=ROOT / 'build/process-analysis/process-layout-plan.json')
args = parser.parse_args()
plan = build_process_layout(args.objects_dir)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(plan, indent=2) + '\n')
print('PASS: compiler-qualified process/thread layout bound to native SHA-256', plan['native_sha256'])
