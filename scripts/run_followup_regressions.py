#!/usr/bin/env python3
"""Bound network/timing/hash tests in an external process, including deadlocks."""
import argparse
import pathlib
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=pathlib.Path)
parser.add_argument("--timeout", type=int, default=90)
parser.add_argument("--repeat", type=int, default=1)
args = parser.parse_args()
for iteration in range(args.repeat):
    started = time.monotonic()
    try:
        result = subprocess.run([str(args.binary.resolve()), "[followup]"], timeout=args.timeout)
    except subprocess.TimeoutExpired:
        raise SystemExit("FAIL: regression child timed out (possible deadlock)")
    if result.returncode:
        raise SystemExit(result.returncode)
    print(f"Regression iteration {iteration + 1}: PASS ({time.monotonic() - started:.2f}s)", flush=True)
