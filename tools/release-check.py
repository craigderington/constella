#!/usr/bin/env python3
"""Verify a source checkout's public release essentials; no network access."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ['LICENSE', 'README.md', 'SECURITY.md', 'CHANGELOG.md', 'CONTRIBUTING.md',
            'constella-wallet', 'Dockerfile', 'explorer/Dockerfile', 'docker-compose.yml',
            'release/VERSION', 'release/network.json', 'release/build-inputs.json',
            'release/deployment-inventory.json', 'docs/release-v0.1.md', 'docs/mainnet-readiness.md',
            'docs/capacity.md', 'docs/protocol-v5.md', 'deploy/public-testnet/compose.miner.yml',
            'deploy/public-testnet/temperature.sh', 'deploy/public-testnet/Dockerfile.temperature',
            'deploy/monitoring/checkpoint_client.c', 'deploy/monitoring/checkpoint_fixture.c',
            'deploy/monitoring/checkpoint.py', 'deploy/monitoring/test_checkpoint.py',
            'tools/deployment-audit.py', 'tools/miner.py', 'tools/testnet-env.py',
            'tools/dependency-audit.py', 'tools/secret-audit.py', 'tools/package-release.py',
            '.gitleaks.toml', 'release/security-tools.json', 'THIRD_PARTY_NOTICES.md']


def forbidden(name):
    p = Path(name)
    return (any(x.lower() in ('agents.md', 'claude.md') for x in p.parts) or
            p.name in ('.env', 'peers.dat', 'web_session.txt') or
            p.suffix in ('.key', '.pem') or
            (p.name.endswith('.env') and not p.name.endswith('.env.example')) or
            (p.name.startswith('.env.') and p.name != '.env.example') or
            name.startswith(('constella-data/', 'constella-data-archive/', 'release/local/')))


def main():
    tracked = set(subprocess.check_output(['git', 'ls-files'], cwd=ROOT, text=True).splitlines())
    missing = sorted(set(REQUIRED) - tracked)
    prohibited = sorted(x for x in tracked if forbidden(x))
    if missing or prohibited:
        raise SystemExit('Release source guard failed: '+json.dumps({'missing': missing, 'prohibited': prohibited}))
    version = (ROOT/'release/VERSION').read_text().strip()
    if not re.fullmatch(r'0\.1\.0(?:-rc\.\d+)?', version):
        raise SystemExit('Unexpected v0.1 release version')
    network = json.loads((ROOT/'release/network.json').read_text())
    if network['chain_id'] != '2094b0868a27b032' or network['protocol_version'] != 5 or network['mainnet_approved']:
        raise SystemExit('This release is exclusively the current v5 testnet')
    for file in ('Dockerfile', 'explorer/Dockerfile', 'deploy/public-testnet/Dockerfile.temperature'):
        for line in (ROOT/file).read_text().splitlines():
            if line.startswith('FROM ') and line.split()[1] != 'scratch' and '@sha256:' not in line:
                raise SystemExit('Unpinned build input: '+file)
    print('PASS: tracked release inputs, local-only exclusions, v5 identity and pinned base images')


if __name__ == '__main__':
    main()
