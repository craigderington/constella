"""Disposable Docker mining-adapter qualification. Never uses a live node/key."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
DOCKER = ['docker', '--host', 'unix:///var/run/docker.sock']
BINARY = Path(os.environ['CONSTELLA_WALLET_TEST_BINARY']).resolve()

def docker(*args, check=True):
    return subprocess.run([*DOCKER, *map(str, args)], check=check, capture_output=True, text=True, timeout=60).stdout.strip()

def core(*args):
    p = subprocess.run([str(BINARY), '--wallet-core', *map(str, args)], capture_output=True, text=True, timeout=20)
    data = json.loads(p.stdout)
    if p.returncode or not data.get('ok'):
        raise RuntimeError(data.get('error', 'core failed'))
    return data

def main():
    suffix = uuid.uuid4().hex[:12]
    name = 'constella-wallet-test-' + suffix
    volume = name + '-data'
    image = name + ':fixture'
    try:
        with tempfile.TemporaryDirectory(prefix='constella-wallet-docker-') as directory:
            root = Path(directory)
            shutil.copy2(ROOT / 'wallet/tests/peer-test', root / 'peer')
            (root / 'Dockerfile').write_text('FROM scratch\nCOPY peer /peer\nENTRYPOINT ["/peer"]\n')
            docker('build', '--network=none', '-t', image, root)
            # Generate the disposable key as the miner's root user, in its volume.
            # Host-created key ownership must not be mistaken for miner ownership.
            docker('volume', 'create', volume)
            expected = json.loads(docker('run', '--rm', '--network=none', '--read-only',
                '--user=0:0', '--cap-drop=ALL', '--security-opt=no-new-privileges:true',
                '--mount', f'type=volume,src={volume},dst=/data',
                '--mount', f'type=bind,src={BINARY},dst=/wallet-signer,readonly',
                '--entrypoint=/wallet-signer', image, '--wallet-core', 'new', '/data/wallet.key'))['address']
            docker('create', '--name', name, '--read-only', '--cap-drop=ALL',
                   '--security-opt=no-new-privileges:true', '--memory=64m', '--pids-limit=16',
                   '--mount', f'type=volume,src={volume},dst=/data',
                   '-p', '127.0.0.1::18473', '-e', 'CONSTELLA_DATA=/data', '-e', 'CONSTELLA_PORT=18473',
                   image, '18473', 'drop')
            key = root / 'wallet.key'
            docker('cp', name + ':/data/wallet.key', key)
            retained = key.read_bytes()  # Test-only key; no live wallet is involved.
            docker('start', name)
            before = json.loads(docker('inspect', name))[0]
            binding = before['NetworkSettings']['Ports']['18473/tcp'][0]
            peer = binding['HostIp'] + ':' + binding['HostPort']
            for _ in range(30):
                try:
                    core('balance', peer, expected)
                    break
                except RuntimeError:
                    time.sleep(.1)
            else:
                raise RuntimeError('Disposable peer unavailable')
            env = dict(os.environ, CONSTELLA_TEST_MINER=name, CONSTELLA_TEST_MINER_ADDRESS=expected,
                       CONSTELLA_WALLET_TEST_BINARY=str(BINARY))
            runner = os.environ.get('CONSTELLA_WALLET_TEST_RUNNER')
            test_command = ([runner, 'docker_reward_transfer_lost_ack_and_retry', '--ignored', '--nocapture']
                            if runner else ['cargo', 'test', '--manifest-path', str(ROOT / 'wallet/Cargo.toml'), '--locked',
                                            'docker_reward_transfer_lost_ack_and_retry', '--', '--ignored', '--nocapture'])
            subprocess.run(test_command,
                           env=env, check=True, timeout=180)
            after = json.loads(docker('inspect', name))[0]
            assert before['State']['StartedAt'] == after['State']['StartedAt']
            assert before['Id'] == after['Id'] and before['Mounts'] == after['Mounts']
            assert after['RestartCount'] == 0 and not after['State']['OOMKilled']
            check_key = root / 'retained.key'
            docker('cp', name + ':/data/wallet.key', check_key)
            assert check_key.read_bytes() == retained
            assert check_key.stat().st_mode & 0o777 == 0o600
            helpers = docker('ps', '-aq', '--filter', 'label=constella.wallet.signer=true')
            assert not helpers, 'Signer helper left behind'
            print('Docker mining test passed: wrong address refused, signing in read-only volume, lost-ACK recovery, identical-byte retry, miner unchanged, key unchanged, helpers removed.')
    finally:
        docker('rm', '-f', name, check=False)
        docker('volume', 'rm', volume, check=False)
        docker('image', 'rm', image, check=False)

if __name__ == '__main__':
    main()
