#!/usr/bin/env python3
"""Local, bounded mission batches. Never writes retail data or existing saves.

Requires the opt-in Catch test in MissionCompletionAudit.inc. Each fresh output
directory contains per-mission JSON, a bounded engine log and process result.
Partial/time-limited runs remain distinguishable from passes.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=Path('build-prof/rwe_test'))
    parser.add_argument('--start', type=int, default=0)
    parser.add_argument('--count', type=int, default=175)
    parser.add_argument('--batch-size', type=int, default=10)
    parser.add_argument('--ticks', type=int, default=300)
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--save-check', action='store_true')
    args = parser.parse_args()
    if not (0 <= args.start < 175 and 1 <= args.count <= 175 - args.start
            and 1 <= args.batch_size <= 175 and 0 <= args.ticks <= 18000
            and 1 <= args.timeout <= 600):
        parser.error('invalid bounds')
    args.output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, RWE_DATA_PATH=str(args.data.resolve()),
               RWE_MISSION_AUDIT_TICKS=str(args.ticks))
    env.pop('RWE_MISSION_AUDIT_SAVE', None)
    env.pop('RWE_WRITE_V7_SAVE', None)
    if args.save_check:
        env['RWE_MISSION_AUDIT_SAVE'] = '1'
    results = []
    for start in range(args.start, args.start + args.count, args.batch_size):
        count = min(args.batch_size, args.start + args.count - start)
        folder = args.output / f'batch-{start:03d}'
        env.update(RWE_MISSION_AUDIT_START=str(start), RWE_MISSION_AUDIT_COUNT=str(count),
                   RWE_MISSION_AUDIT_OUTPUT=str(folder.resolve()))
        command = [str(args.binary.resolve()), '[.mission-audit]', '--rng-seed', '2537990431']
        before = time.monotonic()
        print(f'START {start} count={count}', flush=True)
        with (args.output / f'batch-{start:03d}.log').open('x') as log:
            try:
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=args.timeout, check=False)
                status = 'PASS' if result.returncode == 0 else 'FAIL'
                code = result.returncode
            except subprocess.TimeoutExpired:
                status, code = 'TIMEOUT', None
        results.append(dict(start=start, count=count, status=status, exit=code,
                            seconds=time.monotonic() - before, command=command))
        (args.output / 'batches.json').write_text(json.dumps(results, indent=2) + '\n')
        print(results[-1], flush=True)
    raise SystemExit(0 if all(r['status'] == 'PASS' for r in results) else 1)


if __name__ == '__main__':
    main()
