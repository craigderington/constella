#!/usr/bin/env python3
"""Disposable Docker checks for sample expiry and PID-1 signal handling."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', default='constella-temperature:v5-release-20261005-amd64')
    args = parser.parse_args()
    name = 'constella-temperature-test-' + uuid.uuid4().hex[:10]
    with tempfile.TemporaryDirectory(prefix='constella-temperature-') as directory:
        root = Path(directory)
        source, output = root / 'source', root / 'output'
        source.mkdir()
        output.mkdir()
        sample = source / 'cpu'
        published = output / 'cpu_millidegrees'

        def wait_for(predicate):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if predicate():
                    return
                time.sleep(0.1)
            raise AssertionError('temperature publisher did not reach expected state')

        try:
            sample.write_text('52000\n')
            subprocess.run(['docker', 'run', '-d', '--name', name, '--network', 'none',
                            '--read-only', '--user', f'{os.getuid()}:{os.getgid()}',
                            '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
                            '-e', 'SOURCE_FILE=/sensor/cpu', '-v', f'{source}:/sensor:ro',
                            '-v', f'{output}:/thermal', args.image], check=True, stdout=subprocess.DEVNULL)
            wait_for(lambda: published.exists() and published.read_text().strip() == '52000')
            sample.write_text('invalid\n')
            wait_for(lambda: not published.exists())
            sample.write_text('53000\n')
            wait_for(lambda: published.exists())
            os.utime(sample, (time.time() - 30, time.time() - 30))
            wait_for(lambda: not published.exists())
            sample.write_text('54000\n')
            wait_for(lambda: published.exists())
            start = time.monotonic()
            subprocess.run(['docker', 'stop', '-t', '3', name], check=True, stdout=subprocess.DEVNULL)
            state = subprocess.check_output(['docker', 'inspect', '--format',
                                            '{{.State.ExitCode}} {{.State.OOMKilled}}', name], text=True).strip()
            assert state == '0 false', state
            assert not published.exists(), 'shutdown left a seemingly fresh sample'
            print(f'PASS: valid/invalid/stale samples; SIGTERM exit0 in {time.monotonic()-start:.2f}s; sample removed')
        finally:
            subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
