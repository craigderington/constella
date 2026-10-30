#!/usr/bin/env python3
"""Offline v5 C/Go full-state replay in unique bounded Docker fixtures.

The history is mounted read-only for Go and copied before C startup (which may
repair damaged suffixes). No node connection, wallet, live database or deploy.
Requires a native Linux toolchain, static C compiler, Go and local Docker.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
DOCKER = ['docker', '--host', 'unix:///var/run/docker.sock']
CORE = ['blake2b', 'bn', 'share', 'science', 'sieve', 'tx', 'wallet', 'chain', 'ledger', 'util']


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def framing(path):
    count = 0
    with path.open('rb') as source:
        while header := source.read(2):
            if len(header) != 2:
                raise ValueError('Incomplete history length')
            length = struct.unpack('<H', header)[0]
            if not 128 <= length <= 4096 or len(source.read(length)) != length:
                raise ValueError('Invalid or incomplete history record')
            count += 1
    if not count:
        raise ValueError('Empty history')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--history', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='new evidence directory')
    args = parser.parse_args()
    history = args.history.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = json.loads((ROOT / 'release/build-inputs.json').read_text())['images']
    token = 'constella-replay-qa-' + uuid.uuid4().hex[:10]
    names, network = [], False
    report = {'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'base_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'input': {'sha256': digest(history), 'bytes': history.stat().st_size, 'records': framing(history)},
              'phases': {}, 'limits': {'c': {'memory_mib': 768, 'cpus': 1},
                  'go': {'memory_mib': 1024, 'cpus': 1}, 'postgres': {'memory_mib': 512, 'cpus': 0.5}},
              'images': {k: inputs[k]['ref'] for k in ('alpine_c_builder', 'postgres')},
              'scope': 'Real v5 history and C/Go state/SQL differentials; no peak-RAM or matching-hardware/90-day/year qualification'}
    sources = subprocess.check_output(['git', 'ls-files', '--', 'src', 'explorer'], cwd=ROOT, text=True).splitlines()
    sources += ['tests/replay_audit.c', 'explorer/internal/indexer/replay_audit_test.go', 'tools/qualify-replay.py']
    report['source_sha256'] = {p: digest(ROOT / p) for p in sorted(set(sources))}
    env = os.environ | {'GOCACHE': os.environ.get('GOCACHE', '/tmp/constella-replay-go-cache'), 'CGO_ENABLED': '0'}

    def save():
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    def phase(name, command, **kwargs):
        print('START', name, flush=True)
        began = time.monotonic()
        with (out / (name + '.log')).open('w') as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=4 * 3600, **kwargs)
        report['phases'][name] = {'exit': result.returncode, 'seconds': round(time.monotonic()-began, 3)}
        save()
        print('END', name, report['phases'][name], flush=True)
        result.check_returncode()

    def run_container(name, command, **kwargs):
        result = subprocess.run(DOCKER + ['create', '--name', name, *command], check=True, capture_output=True)
        names.append(name)
        print('START', name, flush=True)
        began = time.monotonic()
        with (out / (name + '.stdout')).open('wb') as stdout, (out / (name + '.stderr')).open('wb') as stderr:
            result = subprocess.run(DOCKER + ['start', '-a', name], stdout=stdout, stderr=stderr,
                                    timeout=4 * 3600, **kwargs)
        state = json.loads(subprocess.check_output(DOCKER + ['inspect', '--format', '{{json .State}}', name]))
        report['phases'][name] = {'exit': result.returncode, 'state': state,
                                 'seconds': round(time.monotonic()-began, 3)}
        save()
        print('END', name, report['phases'][name], flush=True)
        if result.returncode or state['ExitCode'] or state['OOMKilled']:
            raise RuntimeError('Replay container failed: ' + name)
        return out / (name + '.stdout')

    try:
        save()
        cc = shutil.which('musl-gcc') or shutil.which('cc')
        if not cc:
            raise RuntimeError('No C compiler')
        c_binary = out / 'replay-audit'
        phase('build-c', [cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-D_GNU_SOURCE', '-DCONSTELLA_NETWORK=3',
                         '-pthread', '-Isrc', '-static', '-o', str(c_binary),
                         *['src/' + p + '.c' for p in CORE], 'src/vendor/monocypher.c', 'tests/replay_audit.c', '-lm'])
        go_binary = out / 'indexer-audit.test'
        phase('build-go', ['go', 'test', '-C', 'explorer', '-mod=vendor', '-tags', 'protocolv5', '-c',
                          '-o', str(go_binary), './internal/indexer'], env=env)
        copied = out / 'c-data'
        copied.mkdir()
        shutil.copyfile(history, copied / 'shares.testnet-v5')
        c_log = run_container(token + '-c', ['--memory=768m', '--memory-swap=768m', '--cpus=1', '--pids-limit=32',
            '--user', str(os.getuid()) + ':' + str(os.getgid()),
            '--network=none', '--read-only', '--cap-drop=ALL', '--security-opt=no-new-privileges',
            '-v', str(c_binary) + ':/audit:ro', '-v', str(copied) + ':/history:rw',
            inputs['alpine_c_builder']['ref'], '/audit', '/history'])
        if digest(copied / 'shares.testnet-v5') != report['input']['sha256']:
            raise RuntimeError('C replay repaired or altered its disposable history copy')
        c_state = json.loads(c_log.read_text())
        if c_state['known'] != report['input']['records']:
            raise RuntimeError('C did not accept every captured record')
        c_report = out / 'c-state.json'
        c_report.write_text(json.dumps(c_state, indent=2) + '\n')
        subprocess.run(DOCKER + ['network', 'create', '--internal', token], check=True, capture_output=True)
        network = True
        db = token + '-db'
        subprocess.run(DOCKER + ['run', '-d', '--name', db, '--network', token, '--memory=512m', '--memory-swap=512m',
            '--cpus=0.5', '--pids-limit=128', '-e', 'POSTGRES_USER=constella', '-e', 'POSTGRES_PASSWORD=constella',
            '-e', 'POSTGRES_DB=explorer', inputs['postgres']['ref']], check=True, capture_output=True)
        names.append(db)
        for _ in range(120):
            if subprocess.run(DOCKER + ['exec', db, 'pg_isready', '-h', '127.0.0.1', '-U', 'constella', '-d', 'explorer'],
                              capture_output=True).returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError('Disposable database did not become ready')
        go_log = run_container(token + '-go', ['--memory=1024m', '--memory-swap=1024m', '--cpus=1', '--pids-limit=128',
            '--network', token, '--read-only', '--tmpfs', '/tmp:rw,noexec,nosuid,size=16m',
            '--cap-drop=ALL', '--security-opt=no-new-privileges', '-e', 'GOMAXPROCS=1', '-e', 'GOMEMLIMIT=768MiB',
            '-e', 'EXPLORER_TEST_DB=postgres://constella:constella@' + db + '/explorer?sslmode=disable',
            '-e', 'CONSTELLA_REPLAY_HISTORY=/history', '-e', 'CONSTELLA_REPLAY_SHA256=' + report['input']['sha256'],
            '-e', 'CONSTELLA_REPLAY_C_STATE=/c-state.json', '-v', str(history) + ':/history:ro',
            '-v', str(c_report) + ':/c-state.json:ro', '-v', str(go_binary) + ':/audit:ro',
            inputs['alpine_c_builder']['ref'], '/audit', '-test.run=^TestRealHistoryReplayAudit$', '-test.v', '-test.timeout=4h'])
        records = [json.loads(line.split('REPLAY_REPORT ', 1)[1]) for line in go_log.read_text().splitlines() if 'REPLAY_REPORT ' in line]
        if len(records) != 1:
            raise RuntimeError('Missing unique Go replay report')
        report['replay'] = records[0]
        if digest(history) != report['input']['sha256']:
            raise RuntimeError('Read-only source history changed during qualification')
        if any(digest(ROOT / name) != value for name, value in report['source_sha256'].items()):
            raise RuntimeError('Source changed during qualification; retain evidence and rerun')
        report['status'] = 'passed'
    finally:
        cleanup = {}
        for name in reversed(names):
            state = subprocess.run(DOCKER + ['inspect', '--format', '{{json .State}}', name], capture_output=True, text=True)
            (out / (name + '.state.json')).write_text(state.stdout)
            logs = subprocess.run(DOCKER + ['logs', name], capture_output=True)
            (out / (name + '.combined.log')).write_bytes(logs.stdout + logs.stderr)
            removed = subprocess.run(DOCKER + ['rm', '-fv', name], capture_output=True)
            cleanup[name] = removed.returncode
        if network:
            cleanup[token] = subprocess.run(DOCKER + ['network', 'rm', token], capture_output=True).returncode
        report['cleanup'] = cleanup
        report['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
        if any(cleanup.values()):
            raise RuntimeError('Disposable fixture cleanup failed')


if __name__ == '__main__':
    main()
