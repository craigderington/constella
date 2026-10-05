#!/usr/bin/env python3
"""Exercise the Craig-run cutover script on disposable LOCAL Docker projects."""
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def run(args, **kwargs):
    result = subprocess.run(args, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return result.stdout


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    os.environ.pop('DOCKER_CONTEXT', None)
    os.environ['DOCKER_HOST'] = 'unix:///var/run/docker.sock'
    token = secrets.token_hex(5)
    old_name, new_name = 'constella-cutover-old-' + token, 'constella-cutover-new-' + token
    http_port, p2p_port = free_port(), free_port()
    with tempfile.TemporaryDirectory(prefix='constella-cutover-') as directory:
        lab = Path(directory)
        old_dir, new_dir = lab / 'deploy/lightsail', lab / 'deploy/testnet-v5'
        old_dir.mkdir(parents=True)
        new_dir.mkdir(parents=True)
        old_compose = (ROOT / 'deploy/lightsail/compose.yml').read_text().replace('constella-cloud-testnet', old_name)
        new_compose = (ROOT / 'deploy/testnet-v5/compose.cloud.yml').read_text().replace('constella-cloud-v5', new_name)
        (old_dir / 'compose.yml').write_text(old_compose)
        (new_dir / 'compose.cloud.yml').write_text(new_compose)
        # Only project names and the local HTTP test port differ from the operator script.
        script = (ROOT / 'deploy/testnet-v5/cloud-cutover.sh').read_text()
        script = script.replace('constella-cloud-testnet', old_name).replace('constella-cloud-v5', new_name)
        script = script.replace('http://127.0.0.1:3071', f'http://127.0.0.1:{http_port}')
        (new_dir / 'cloud-cutover.sh').write_text(script)
        common = f'\nP2P_BIND=127.0.0.1\nP2P_PORT={p2p_port}\nEXPLORER_HTTP_PORT={http_port}\nNODE_PEERS=127.0.0.1:1\nNODE_ADVERTISE=127.0.0.1:{p2p_port}\n'
        (old_dir / 'production.env').write_text((ROOT / 'deploy/lightsail/.env.example').read_text() + common + '\nPOSTGRES_PASSWORD=' + secrets.token_hex(32) + '\n')
        (new_dir / 'cloud.env').write_text((ROOT / 'deploy/testnet-v5/cloud.env.example').read_text() + common + '\nV5_DB_PASSWORD=' + secrets.token_hex(32) + '\n')
        for env in (old_dir / 'production.env', new_dir / 'cloud.env'):
            env.chmod(0o600)
        old = ['docker', 'compose', '--env-file', str(old_dir / 'production.env'), '-f', str(old_dir / 'compose.yml')]
        new = ['docker', 'compose', '--env-file', str(new_dir / 'cloud.env'), '-f', str(new_dir / 'compose.cloud.yml')]
        operator = ['sh', str(new_dir / 'cloud-cutover.sh')]
        backup = lab / 'private-backup'

        def ready(chain, height):
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                try:
                    with urllib.request.urlopen(f'http://127.0.0.1:{http_port}/api/stats', timeout=3) as response:
                        meta = json.load(response)['meta']
                    if meta.get('chain_id') == chain and meta.get('height') == str(height) and (height == 0 or meta.get('check') == 'ok'):
                        return meta
                except (OSError, ValueError):
                    pass
                time.sleep(1)
            raise AssertionError('expected chain/ledger did not become ready')

        def sql(database, query):
            return run(old + ['exec', '-T', 'postgres', 'psql', '-U', 'constella', '-d', database, '-At', '-c', query]).strip()

        try:
            run(old + ['create', 'node'])
            cid = run(old + ['ps', '-a', '-q', 'node']).decode().strip()
            run(['docker', 'run', '--rm', '--network', 'none', '--volumes-from', cid, '-v',
                 str(ROOT / 'tests/fixtures/sync-fork.v3') + ':/fixture:ro', 'alpine:3.20',
                 'sh', '-c', 'cp /fixture /data/shares.v3 && chown 0:0 /data/shares.v3 && chmod 600 /data/shares.v3'])
            run(old + ['up', '-d', '--no-deps', 'postgres', 'node'])
            run(old + ['up', '-d', 'explorer'])
            before = ready('a8f4562e57e74f9d', 28)
            run(operator + ['backup', str(backup)])
            assert (backup / 'COMPLETE').is_file()
            assert (backup.stat().st_mode & 0o777) == 0o700
            sql('postgres', 'CREATE DATABASE restore_check OWNER constella')
            run(old + ['exec', '-T', 'postgres', 'pg_restore', '-U', 'constella', '-d', 'restore_check', '--exit-on-error'], input=(backup / 'explorer.dump').read_bytes())
            tables = sql('explorer', "SELECT tablename FROM pg_tables WHERE schemaname='public' ORDER BY tablename").decode().splitlines()
            for table in tables:
                identifier = '"' + table.replace('"', '""') + '"'
                query = f"SELECT md5(coalesce(string_agg(row_to_json(t)::text, E'\\n' ORDER BY row_to_json(t)::text), '')) FROM {identifier} t"
                assert sql('explorer', query) == sql('restore_check', query), table
            run(operator + ['start', str(backup)])
            ready('2094b0868a27b032', 0)
            repeat = subprocess.run(operator + ['start', str(backup)], capture_output=True)
            assert repeat.returncode != 0 and b'already exists' in repeat.stderr
            run(operator + ['rollback', str(backup)])
            after = ready('a8f4562e57e74f9d', 28)
            assert before['tip'] == after['tip']
            print(json.dumps({'result': 'passed', 'checks': ['private v3 backup and image archive',
                'Postgres restore with all-table digest equality', 'fresh v5 genesis',
                'existing-volume refusal', 'rollback to exact v3 tip and ledger'], 'restored_tables': tables}, indent=2))
        finally:
            for command in (new, old):
                subprocess.run(command + ['down', '-v', '--remove-orphans', '--timeout', '60'], capture_output=True)


if __name__ == '__main__':
    main()
