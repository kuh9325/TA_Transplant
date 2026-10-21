#!/usr/bin/env python3
"""Run explicit retail gameplay plans sequentially, retaining bounded evidence."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build-prof/rwe_test'))
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--plans', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--total-timeout', type=int, default=900)
    parser.add_argument('--selector', default='[.mission-followup]')
    parser.add_argument('--replay-wins', action='store_true')
    parser.add_argument('--replay-from', type=Path)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 180 or not args.timeout <= args.total_timeout <= 1800:
        parser.error('invalid time bounds')
    args.output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, RWE_DATA_PATH=str(args.data.resolve()))
    for key in ['RWE_WRITE_V7_SAVE', 'RWE_MISSION_REPLAY']:
        env.pop(key, None)
    results = []
    started = time.monotonic()

    def run(plan, name, replay=None):
        folder = args.output / name
        log_path = args.output / (name + '.log')
        remaining = args.total_timeout - (time.monotonic() - started)
        if remaining <= 0:
            results.append(dict(name=name, status='SKIPPED_TOTAL_BOUND'))
            return
        child_env = dict(env, RWE_MISSION_PLAN=str(plan.resolve()),
                         RWE_MISSION_AUDIT_OUTPUT=str(folder.resolve()))
        if replay:
            child_env['RWE_MISSION_REPLAY'] = str(replay.resolve())
        command = [str(args.binary.resolve()), args.selector, '--rng-seed', '2537990431']
        before = time.monotonic()
        with log_path.open('x') as log:
            process = subprocess.Popen(command, env=child_env,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = before + min(args.timeout, remaining)
            while True:
                pid, wait_status, usage = os.wait4(process.pid, os.WNOHANG)
                if pid:
                    code = os.waitstatus_to_exitcode(wait_status)
                    process.returncode = code
                    status = 'PASS' if code == 0 else 'FAIL'
                    break
                if time.monotonic() >= deadline:
                    import signal
                    os.killpg(process.pid, signal.SIGKILL)
                    _, wait_status, usage = os.wait4(process.pid, 0)
                    process.returncode = os.waitstatus_to_exitcode(wait_status)
                    code, status = None, 'TIMEOUT'
                    break
                time.sleep(0.05)
        result = dict(name=name, plan=str(plan.resolve()), command=command, status=status,
                      exit=code, seconds=time.monotonic() - before,
                      max_rss_bytes=usage.ru_maxrss, log_bytes=log_path.stat().st_size)
        report_path = folder / 'result.json'
        if report_path.exists():
            report = json.loads(report_path.read_text())
            result.update({k: report[k] for k in ['id', 'difficulty', 'ticks', 'outcome',
                                                 'save_continuation_equal']})
        elif (folder / 'progress.json').exists():
            result['last_progress'] = json.loads((folder / 'progress.json').read_text())
        results.append(result)
        (args.output / 'runs.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(result), flush=True)
        return result

    for plan in sorted(args.plans.glob('*.json')):
        replay = args.replay_from / plan.stem / 'result.json' if args.replay_from else None
        result = run(plan, plan.stem, replay)
        if args.replay_wins and result and result.get('outcome') == 'VICTORY' and result['status'] == 'PASS':
            run(plan, plan.stem + '-replay', args.output / plan.stem / 'result.json')
    (args.output / 'runs.json').write_text(json.dumps(results, indent=2) + '\n')
    raise SystemExit(0 if all(r['status'] == 'PASS' for r in results) else 1)


if __name__ == '__main__':
    main()
