#!/usr/bin/env python3
"""Build and exercise unapproved mainnet images on disposable local networks.

No live peers, keys, data directories, deployment commands or remote Docker
contexts are used. Evidence belongs under ignored release/local/.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
DOCKER = ['docker', '--host', 'unix:///var/run/docker.sock']
CHAIN = 'e46fea4617869975'
GENESIS = '7f047959fda8cb9a7d4c2c22a3636a3ee4c5c62057287dcd7f9cd7bea9eccbaa'


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='new evidence directory')
    args = parser.parse_args()
    out = args.out.resolve()
    # A new directory prevents accidentally reusing operator state or evidence.
    out.mkdir(parents=True, exist_ok=False)
    prefix = 'constella-mainnet-qa-' + uuid.uuid4().hex[:10]
    containers, networks, images = [], [], []
    report = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'prefix': prefix, 'release_approved': False,
              'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'host_architecture': platform.machine(), 'phases': {}, 'images': {},
              'scope': 'Local native candidate packaging and fixture interoperability only'}

    def save():
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(*args, **kwargs):
        return subprocess.run(DOCKER + list(args), check=True, capture_output=True, text=True, timeout=120, **kwargs).stdout

    def phase(name, command):
        began = time.monotonic()
        print('START', name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        report['phases'][name] = {'exit': result.returncode, 'seconds': round(time.monotonic() - began, 3)}
        save()
        result.check_returncode()
        print('END', name, flush=True)

    user = str(os.getuid()) + ':' + str(os.getgid())
    hardened = ['--read-only', '--cap-drop=ALL', '--security-opt=no-new-privileges', '--pids-limit=64']

    def create(name, command):
        run('create', '--name', name, *command)
        containers.append(name)
        return name

    def oneshot(name, command, expected=0):
        create(name, command)
        with (out / (name + '.stdout')).open('wb') as stdout, (out / (name + '.stderr')).open('wb') as stderr:
            result = subprocess.run(DOCKER + ['start', '-a', name], stdout=stdout, stderr=stderr, timeout=120)
        state = json.loads(run('inspect', '--format', '{{json .State}}', name))
        report['phases'][name] = {'exit': result.returncode, 'state': state}
        save()
        if state['Running'] or state['OOMKilled'] or state['ExitCode'] != expected or result.returncode != expected:
            raise RuntimeError('Unexpected candidate exit: ' + name)
        return (out / (name + '.stdout')).read_text()

    try:
        save()
        # Curated public inputs: never send untracked private tests or local
        # deploy bundles through the permissive source Docker ignore rules.
        tracked = subprocess.check_output(['git', 'ls-files', '--', 'Makefile', 'src', 'tests', 'explorer',
            'third_party', 'LICENSE', 'THIRD_PARTY_NOTICES.md'], cwd=ROOT, text=True).splitlines()
        sources = set(tracked) | {'tools/mainnet-candidate/Dockerfile.node', 'tools/mainnet-candidate/Dockerfile.explorer',
                                  'tools/smoke-mainnet-candidate.py'}
        context = out / 'context'
        context.mkdir()
        report['source_sha256'] = {}
        for name in sorted(sources):
            source = ROOT / name
            if source.is_symlink() or not source.is_file() or source.name.lower() in ('agents.md', 'claude.md'):
                raise RuntimeError('Unexpected build input: ' + name)
            target = context / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            report['source_sha256'][name] = digest(source)
        save()
        for component, target in [('node', 'node'), ('audit', 'audit'), ('explorer', None)]:
            image = prefix + '-' + component
            dockerfile = 'node' if component == 'audit' else component
            command = DOCKER + ['build', '--build-arg', 'VCS_REF=' + report['revision'], '-t', image,
                '-f', str(context / ('tools/mainnet-candidate/Dockerfile.' + dockerfile))]
            if target:
                command += ['--target', target]
            phase('build-' + component, command + [str(context)])
            images.append(image)
            image_info = json.loads(run('image', 'inspect', image))[0]
            report['images'][component] = {'id': image_info['Id'], 'labels': image_info['Config']['Labels']}
            if image_info['Config']['Labels'].get('org.constella.release-approved') != 'false':
                raise RuntimeError('Candidate must remain explicitly unapproved')
        node_image, audit_image, web_image = [prefix + '-' + part for part in ('node', 'audit', 'explorer')]
        for component, image in [('node', node_image), ('explorer', web_image)]:
            version = oneshot(prefix + '-version-' + component,
                [*hardened, '--network=none', '--memory=64m', '--memory-swap=64m', '--cpus=0.25', image, '--version'])
            if 'protocol=5 network=mainnet chain=' + CHAIN not in version:
                raise RuntimeError('Wrong candidate image identity')
            report['images'][component]['version'] = version.strip()

        history = ROOT / 'tests/fixtures/sync-fork.mainnet-v5'
        report['fixture_sha256'] = digest(history)
        copies = []
        for directory in ('c-data', 'node1-data', 'node2-data'):
            data = out / directory
            data.mkdir()
            copied = data / 'shares.mainnet-v5'
            shutil.copyfile(history, copied)
            copies.append(copied)
        oracle = json.loads(oneshot(prefix + '-audit', [*hardened, '--network=none', '--user', user,
            '--memory=256m', '--memory-swap=256m', '--cpus=0.25', '-v', str(out / 'c-data') + ':/history:rw', audit_image, '/history']))
        if oracle['genesis'] != GENESIS or not oracle['accounts'] or not oracle['known']:
            raise RuntimeError('Unexpected candidate fixture state')
        report['oracle'] = oracle
        wrong = out / 'wrong-network'
        wrong.mkdir()
        shutil.copyfile(ROOT / 'tests/fixtures/sync-fork.testnet-v5', wrong / 'shares.mainnet-v5')
        wrong_hash = digest(wrong / 'shares.mainnet-v5')
        oneshot(prefix + '-wrong-network', [*hardened, '--network=none', '--user', user,
            '--memory=256m', '--memory-swap=256m', '--cpus=0.25',
            '-v', str(wrong) + ':/data:rw', node_image], expected=1)
        if 'fatal: foreign-network history; leaving original bytes intact' not in (out / (prefix + '-wrong-network.stderr')).read_text():
            raise RuntimeError('Wrong-network refusal reason not confirmed')
        if digest(wrong / 'shares.mainnet-v5') != wrong_hash:
            raise RuntimeError('Wrong-network history was changed')

        for suffix in ('p2p', 'database'):
            name = prefix + '-' + suffix
            run('network', 'create', '--internal', name)
            networks.append(name)
        db = prefix + '-db'
        postgres = json.loads((ROOT / 'release/build-inputs.json').read_text())['images']['postgres']['ref']
        create(db, ['--network', networks[1], '--memory=384m', '--memory-swap=384m', '--cpus=0.25', '--pids-limit=128',
            '-e', 'POSTGRES_USER=constella', '-e', 'POSTGRES_PASSWORD=disposable-qa', '-e', 'POSTGRES_DB=explorer',
            postgres, 'postgres', '-c', 'shared_buffers=64MB', '-c', 'max_connections=16'])
        run('start', db)
        for _ in range(90):
            ready = subprocess.run(DOCKER + ['exec', db, 'pg_isready', '-h', '127.0.0.1', '-U', 'constella', '-d', 'explorer'],
                                   capture_output=True, timeout=10)
            if ready.returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError('Disposable PostgreSQL did not become ready')
        for number, peer in ((1, 2), (2, 1)):
            name = prefix + '-node' + str(number)
            create(name, [*hardened, '--network', networks[0], '--user', user, '--tmpfs', '/tmp:rw,noexec,nosuid,size=32m',
                '--memory=512m', '--memory-swap=512m', '--cpus=0.5', '-e', 'CONSTELLA_MINE=0', '-e', 'CONSTELLA_DUTY=0',
                '-e', 'CONSTELLA_PRIVATE_NET=1', '-e', 'CONSTELLA_PEERS=' + prefix + '-node' + str(peer) + ':7043',
                '-v', str(out / ('node' + str(number) + '-data')) + ':/data:rw', node_image])
            run('start', name)
        web = prefix + '-explorer'
        create(web, [*hardened, '--network', networks[0], '--tmpfs', '/tmp:rw,noexec,nosuid,size=16m',
            '--memory=1024m', '--memory-swap=1024m', '--cpus=0.5',
            '-e', 'GOMAXPROCS=1', '-e', 'GOMEMLIMIT=768MiB', '-e', 'EXPLORER_HTTP=:3071',
            '-e', 'EXPLORER_NODE=' + prefix + '-node1:7043',
            '-e', 'EXPLORER_DB=postgres://constella:disposable-qa@' + db + '/explorer?sslmode=disable', web_image])
        run('network', 'connect', networks[1], web)
        run('start', web)
        # Docker internal bridges have no external gateway and may not publish
        # host ports. Probe HTTP from the owned Alpine database container instead.
        def fetch(path):
            return run('exec', db, 'wget', '-q', '-T', '5', '-O', '-', 'http://' + web + ':3071' + path)

        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            try:
                stats = json.loads(fetch('/api/stats'))
                meta = stats['meta']
                if meta.get('peer') == 'true' and meta.get('check') == 'ok':
                    break
            except (OSError, subprocess.CalledProcessError, ValueError, KeyError, AttributeError):
                pass
            time.sleep(1)
        else:
            raise RuntimeError('Candidate Explorer did not complete a full peer ledger check')
        for key in ('tip', 'height', 'escrow', 'blocks', 'txs', 'sci_paid', 'sci_claims'):
            if meta.get(key) != str(oracle[key]):
                raise RuntimeError('C/Explorer fixture mismatch: ' + key)
        for key, value in {'network': 'mainnet', 'chain_id': CHAIN, 'check_height': str(oracle['height']),
                           # Initial P2P sync serves the canonical path, not
                           # every off-path record in the source fork fixture.
                           'known': str(oracle['height']),
                           'check_count': str(len(oracle['accounts'])), 'check_total': str(len(oracle['accounts']))}.items():
            if meta.get(key) != value:
                raise RuntimeError('Incomplete candidate profile/account check: ' + key)
        if fetch('/healthz') != 'ok\n':
            raise RuntimeError('Candidate health check failed')
        html = fetch('/')
        if '<span class="net-name">Mainnet</span>' not in html or CHAIN not in html:
            raise RuntimeError('Candidate page identity mismatch')
        (out / 'stats.json').write_text(json.dumps(stats, indent=2) + '\n')
        (out / 'overview.html').write_text(html)
        report['containers'] = {}
        for name in (prefix + '-node1', prefix + '-node2', web, db):
            info = json.loads(run('inspect', name))[0]
            if not info['State']['Running'] or info['State']['OOMKilled'] or info['RestartCount']:
                raise RuntimeError('Unhealthy fixture: ' + name)
            report['containers'][name] = {'state': info['State'], 'restarts': info['RestartCount'],
                'image': info['Image'], 'memory_bytes': info['HostConfig']['Memory'],
                'nanocpus': info['HostConfig']['NanoCpus'], 'networks': list(info['NetworkSettings']['Networks'])}
        for name in (prefix + '-node1', prefix + '-node2'):
            # Docker writes C logger output on stderr; combined logs are saved in finally.
            combined = subprocess.run(DOCKER + ['logs', name], capture_output=True, text=True, check=True, timeout=10)
            if 'mode: validation-only; mining and thermal sampler disabled' not in combined.stdout + combined.stderr:
                raise RuntimeError('Validation-only mode not confirmed: ' + name)
        # Stop only owned services before checking final on-disk hashes.
        for name in (web, prefix + '-node2', prefix + '-node1'):
            run('stop', '-t', '10', name)
        if any(digest(path) != report['fixture_sha256'] for path in copies):
            raise RuntimeError('Mining-disabled fixture history changed')
        if any(digest(ROOT / name) != sha for name, sha in report['source_sha256'].items()):
            raise RuntimeError('Candidate build inputs changed during smoke test')
        report['status'] = 'passed'
    except BaseException as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        cleanup = {}
        for name in reversed(containers):
            logs = subprocess.run(DOCKER + ['logs', name], capture_output=True, timeout=30)
            (out / (name + '.combined.log')).write_bytes(logs.stdout + logs.stderr)
            state = subprocess.run(DOCKER + ['inspect', name], capture_output=True, timeout=30)
            # Full inspect includes disposable DB credentials, so retain locally only.
            (out / (name + '.inspect.json')).write_bytes(state.stdout)
            cleanup[name] = subprocess.run(DOCKER + ['rm', '-fv', name], capture_output=True, timeout=60).returncode
        for name in reversed(networks):
            cleanup[name] = subprocess.run(DOCKER + ['network', 'rm', name], capture_output=True, timeout=30).returncode
        for image in reversed(images):
            # Unique tags only; leave shared builder/base images and other projects alone.
            cleanup['image:' + image] = subprocess.run(DOCKER + ['image', 'rm', image], capture_output=True, timeout=60).returncode
        report['cleanup'] = cleanup
        report['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        if any(cleanup.values()):
            report['status'] = 'failed'
            report['cleanup_error'] = 'Owned local candidate fixture cleanup failed'
        save()
        if any(cleanup.values()):
            raise RuntimeError(report['cleanup_error'])


if __name__ == '__main__':
    main()
