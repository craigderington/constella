"""Release safety contracts. No public connections or live Docker operations."""
import datetime as dt
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT/path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value
miner = module('release_miner', 'tools/miner.py')
guard = module('release_guard', 'tools/release-check.py')
deps = module('release_dependencies', 'tools/dependency-audit.py')

class Release(unittest.TestCase):
    def checkpoint(self):
        now = dt.datetime.now(dt.timezone.utc)
        tip = 'ab'*32
        public = {'meta': {'chain_id': miner.CHAIN, 'network': 'testnet', 'peer': 'true',
                          'check': 'ok', 'height': '42', 'tip': tip, 'check_count': '6',
                          'check_total': '6', 'updated_at': now.isoformat(), 'check_at': now.isoformat()}}
        return now, public, {'chain_id': miner.CHAIN, 'height': 42, 'tip': tip}

    def test_go_module_identity_matches_public_repository(self):
        self.assertTrue((ROOT/'explorer/go.mod').read_text().startswith('module github.com/craigderington/constella/explorer\n'))

    def test_arm_wallet_passes_matching_c_compiler_and_linker(self):
        command = subprocess.check_output(['make', '-n', 'wallet', 'WALLET_TARGET=aarch64-unknown-linux-musl', 'WALLET_LINKER=musl-gcc'], cwd=ROOT, text=True)
        self.assertIn('CC_aarch64_unknown_linux_musl="musl-gcc"', command)
        self.assertIn('CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER="musl-gcc"', command)
        self.assertNotIn('CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER=', command)

    def test_dependency_scan_includes_inline_indirect_requirements(self):
        queries = list(deps.go_queries("require (\nexample.org/a v1.2.3\n)\nrequire example.org/b v2.0.0 // indirect\n"))
        self.assertEqual([q['package']['name'] for q in queries], ['example.org/a', 'example.org/b'])

    def test_current_tip_with_growing_account_inventory_can_release(self):
        now, public, local = self.checkpoint()
        self.assertTrue(miner.valid_checkpoint(public, local, now))

    def test_wrong_network_full_hash_or_partial_account_check_cannot_release(self):
        for key, value in [('chain_id', '00'*8), ('network', 'mainnet'),
                           ('tip', 'ab'*31+'ac'), ('check_count', '5'), ('peer', 'false')]:
            with self.subTest(key=key):
                now, public, local = self.checkpoint(); public['meta'][key] = value
                self.assertFalse(miner.valid_checkpoint(public, local, now))

    def test_stale_and_future_ledger_checks_cannot_release(self):
        for delta in (-100, 10):
            now, public, local = self.checkpoint()
            public['meta']['check_at'] = (now+dt.timedelta(seconds=delta)).isoformat()
            self.assertFalse(miner.valid_checkpoint(public, local, now))

    def test_local_only_guidance_keys_and_runtime_state_are_excluded(self):
        for name in ['AGENTS.md', 'docs/CLAUDE.md', '.env', 'host.env', 'wallet.key',
                     'release/local/mining-release.json', 'constella-data-archive/history']:
            self.assertTrue(guard.forbidden(name), name)
        for name in ['LICENSE', '.env.example', 'tests/fixtures/low-order-keys.txt']:
            self.assertFalse(guard.forbidden(name), name)

    def test_source_versions_and_site_match_active_network(self):
        network = json.loads((ROOT/'release/network.json').read_text())
        self.assertEqual(network['chain_id'], miner.CHAIN)
        self.assertIn(network['chain_id'], (ROOT/'README.md').read_text())
        self.assertIn(network['chain_id'], (ROOT/'site/index.html').read_text())
        self.assertIn((ROOT/'release/VERSION').read_text().strip(), (ROOT/'src/version.h').read_text())

    def test_public_compose_is_validation_only_with_private_database(self):
        # Compose interpolation/config parsing requires its CLI, not a daemon.
        result = subprocess.run(['docker', 'compose', '--env-file', '/dev/null', '--profile', 'explorer', 'config', '--format', 'json'],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        config = json.loads(result.stdout); services = config['services']
        self.assertEqual(services['node']['environment']['CONSTELLA_MINE'], '0')
        self.assertNotIn('ports', services['postgres'])
        self.assertTrue(config['networks']['database']['internal'])
        self.assertEqual(services['explorer']['ports'][0]['host_ip'], '127.0.0.1')
        self.assertEqual(services['node']['ports'][0]['host_ip'], '127.0.0.1')
        self.assertNotIn('constella-cloud-v5', config['name'])

if __name__ == '__main__':
    unittest.main()
