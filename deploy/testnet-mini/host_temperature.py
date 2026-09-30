#!/usr/bin/env python3
"""Publish fresh macmon CPU/GPU temperatures for a read-only container mount.

Requires macmon on the macOS host. The node independently rejects samples
older than three seconds, including if this publisher is killed or hangs.
"""
import argparse
import datetime
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time


def temperature(sample, now):
    stamp = datetime.datetime.fromisoformat(sample["timestamp"].replace("Z", "+00:00"))
    if stamp.tzinfo is None or not 0 <= now - stamp.timestamp() <= 3:
        raise ValueError("stale or future host reading")
    cpu = float(sample["temp"]["cpu_temp_avg"])
    gpu = float(sample["temp"]["gpu_temp_avg"])
    if not all(math.isfinite(t) and 0 < t < 150 for t in (cpu, gpu)):
        raise ValueError("invalid host temperature")
    return round(max(cpu, gpu) * 1000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--macmon", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pending = args.output.with_suffix(".pending")
    args.output.unlink(missing_ok=True)
    proc = subprocess.Popen([args.macmon, "pipe", "-i", "1000"], stdout=subprocess.PIPE, text=True)
    try:
        for line in proc.stdout:
            try:
                value = temperature(json.loads(line), time.time())
                pending.write_text(f"{value}\n")
                os.replace(pending, args.output)
            except (ValueError, KeyError, TypeError, OverflowError) as exc:
                args.output.unlink(missing_ok=True)
                print(f"host sensor unavailable: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(proc.wait() or 1)
    finally:
        args.output.unlink(missing_ok=True)
        pending.unlink(missing_ok=True)
        if proc.poll() is None:
            proc.terminate()
            proc.wait()


if __name__ == "__main__":
    main()
