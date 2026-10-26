#!/usr/bin/env python3
"""Explicitly start/hold/release the public Linux testnet miner; no SSH."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CHAIN = '2094b0868a27b032'
COMPOSE = ['docker', '--host', 'unix:///var/run/docker.sock', 'compose', '--project-name', 'constella-testnet-v5',
           '-f', str(ROOT/'docker-compose.yml'), '-f', str(ROOT/'deploy/public-testnet/compose.miner.yml')]


def run(args):
    return subprocess.check_output(args, cwd=ROOT, text=True, timeout=45).strip()


def compose(*args):
    return run(COMPOSE + list(args))


def inspect():
    name = compose('ps', '-q', 'node')
    if not name or '\n' in name:
        raise ValueError('Expected exactly one running testnet miner')
    c = json.loads(run(['docker', '--host', 'unix:///var/run/docker.sock', 'inspect', name]))[0]
    env = dict(x.split('=', 1) for x in c['Config']['Env'])
    if not c['State']['Running'] or c['State']['OOMKilled'] or env.get('CONSTELLA_MINE') != '1':
        raise ValueError('Miner is not running in the intended mode')
    if c['Config']['Labels'].get('com.docker.compose.project') != 'constella-testnet-v5':
        raise ValueError('Unexpected Compose project')
    address = env.get('CONSTELLA_ADDR', '')
    if not re.fullmatch('[0-9a-fA-F]{64}', address):
        raise ValueError('Set a valid, backed-up testnet payout address')
    if env.get('CONSTELLA_TEMP_FILE') != '/thermal/mining_cpu_millidegrees':
        raise ValueError('Expected the held real-temperature input')
    return c, env, address


def valid_checkpoint(public, local, current):
    meta = public['meta']
    if meta['network'] != 'testnet' or meta['chain_id'] != CHAIN or local['chain_id'] != CHAIN:
        return False
    if meta['peer'] != 'true' or meta['check'] != 'ok':
        return False
    if not 0 < int(meta['check_count']) == int(meta['check_total']):
        return False
    for name in ('updated_at', 'check_at'):
        stamp = dt.datetime.fromisoformat(meta[name].replace('Z', '+00:00'))
        if not 0 <= (current - stamp).total_seconds() < 90:
            return False
    return (int(meta['height']) == local['height'] > 0 and
            re.fullmatch('[0-9a-f]{64}', local['tip']) is not None and
            meta['tip'] == local['tip'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['start', 'hold', 'release'])
    args = parser.parse_args()
    if args.action == 'start':
        compose('up', '-d', '--no-deps', '--no-build', 'temperature')
        compose('exec', '-T', 'temperature', 'rm', '-f', '/thermal/mining_cpu_millidegrees')
        compose('up', '-d', '--no-deps', '--no-build', 'node')
        print('Started held. Wait for peer synchronization, then run release.')
        return
    if args.action == 'hold':
        compose('exec', '-T', 'temperature', 'rm', '-f', '/thermal/mining_cpu_millidegrees')
        print('Mining held; validation continues.')
        return
    compose('exec', '-T', 'temperature', 'sh', '-ec',
            'test ! -e /thermal/mining_cpu_millidegrees; test ! -L /thermal/mining_cpu_millidegrees')
    c, env, address = inspect()
    binding = c['NetworkSettings']['Ports']['7043/tcp'][0]
    # The local port must be available on loopback, never inferred as a remote host.
    if binding['HostIp'] not in ('127.0.0.1', '0.0.0.0'):
        raise ValueError('Use an IPv4 loopback-accessible local P2P binding')
    client = ROOT/'deploy/monitoring/checkpoint-client'
    if not client.is_file():
        raise ValueError('Build the read-only client first: make checkpoint-client')
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        local = json.loads(run([str(client), '127.0.0.1:'+binding['HostPort'], address]))
        request = urllib.request.Request('https://explorer.catasterism.xyz/api/stats',
                                        headers={'User-Agent': 'ConstellaRelease/0.1'})
        with urllib.request.urlopen(request, timeout=15) as response:
            public = json.load(response)
        if valid_checkpoint(public, local, dt.datetime.now(dt.timezone.utc)):
            break
        time.sleep(2)
    else:
        raise ValueError('No fresh matching full-tip checkpoint; mining remains held')
    again, current_env, _ = inspect()
    if again['Id'] != c['Id'] or again['Image'] != c['Image'] or current_env != env:
        raise ValueError('Miner changed during verification; mining remains held')
    cap = int(env['CONSTELLA_TEMP_MAX']) * 1000
    if not 40000 <= cap <= 90000:
        raise ValueError('Use an explicit temperature cap between 40 and 90 C')
    script = ('test ! -e /thermal/mining_cpu_millidegrees; '
              'test ! -L /thermal/mining_cpu_millidegrees; '
              'now=$(date +%s); modified=$(stat -c %Y /thermal/cpu_millidegrees); '
              'value=$(cat /thermal/cpu_millidegrees); age=$((now-modified)); '
              'test "$age" -ge 0; test "$age" -le 3; '
              'test "$value" -ge 1000; test "$value" -lt '+str(cap)+'; '
              'ln -s cpu_millidegrees /thermal/mining_cpu_millidegrees; '
              'printf "temperature_millidegrees=%s age_seconds=%s\\n" "$value" "$age"')
    sensor = compose('exec', '-T', 'temperature', 'sh', '-eu', '-c', script)
    os.umask(0o077)
    receipt = ROOT/'release/local/mining-release.json'
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps({'at': dt.datetime.now(dt.timezone.utc).isoformat(),
                                  'container': c['Id'], 'image': c['Image'], 'checkpoint': local,
                                  'sensor': sensor}, indent=2)+'\n')
    print('Mining released after full-tip agreement and fresh real-temperature check. '+sensor)


if __name__ == '__main__':
    main()
