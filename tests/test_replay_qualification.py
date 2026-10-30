"""Integrity checks for reusing only the completed C part of local evidence."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('qualification', ROOT / 'tools/qualify-replay.py')
QUALIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALIFY)


class CReplayEvidenceIntegrity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.previous = self.root / 'previous'
        self.previous.mkdir()
        data = ROOT / 'tests/fixtures/sync-fork.testnet-v5'
        (self.previous / 'c-data').mkdir()
        shutil.copyfile(data, self.previous / 'c-data/shares.testnet-v5')
        self.expected = {'sha256': QUALIFY.digest(data), 'bytes': data.stat().st_size, 'records': QUALIFY.framing(data)}
        self.binary = self.root / 'current-binary'
        self.binary.write_bytes(b'unit-test-binary-placeholder-not-an-executed-oracle')
        shutil.copyfile(self.binary, self.previous / 'replay-audit')
        files = subprocess.check_output(['git', 'ls-files', '--', 'src'], cwd=ROOT, text=True).splitlines()
        files.append('tests/replay_audit.c')
        self.report = {'input': self.expected, 'status': 'failed', 'source_sha256': {name: QUALIFY.digest(ROOT / name) for name in files},
                       'phases': {'constella-test-c': {'exit': 0, 'state': {'ExitCode': 0, 'OOMKilled': False}},
                                  'constella-test-go': {'exit': 137, 'state': {'ExitCode': 137, 'OOMKilled': True}}}}
        self.state = {'known': self.expected['records'], 'note': 'unit fixture, not a proof-validation claim'}
        (self.previous / 'c-state.json').write_text(json.dumps(self.state))
        (self.previous / 'constella-test-c.stdout').write_text(json.dumps(self.state))
        self.save()

    def save(self):
        (self.previous / 'results.json').write_text(json.dumps(self.report))

    def check(self):
        return QUALIFY.checked_c_cache(self.previous, self.expected, self.binary)

    def test_later_go_failure_does_not_invalidate_completed_c_component(self):
        state, provenance = self.check()
        self.assertEqual(state, self.state)
        self.assertEqual(provenance['input_sha256'], self.expected['sha256'])

    def test_changed_state_cannot_replace_original_c_output(self):
        state = dict(self.state, known=self.state['known'] + 1)
        (self.previous / 'c-state.json').write_text(json.dumps(state))
        with self.assertRaisesRegex(ValueError, 'state differs'):
            self.check()

    def test_repaired_or_changed_history_is_rejected(self):
        (self.previous / 'c-data/shares.testnet-v5').write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'copy changed'):
            self.check()

    def test_failed_c_phase_cannot_be_reused(self):
        self.report['phases']['constella-test-c']['state']['OOMKilled'] = True
        self.save()
        with self.assertRaisesRegex(ValueError, 'did not pass'):
            self.check()

    def test_stale_source_or_binary_is_rejected(self):
        self.report['source_sha256']['src/params.h'] = '0' * 64
        self.save()
        with self.assertRaisesRegex(ValueError, 'source differs'):
            self.check()
        self.report['source_sha256']['src/params.h'] = QUALIFY.digest(ROOT / 'src/params.h')
        self.save()
        (self.previous / 'replay-audit').write_bytes(b'changed executable')
        with self.assertRaisesRegex(ValueError, 'binary differs'):
            self.check()


if __name__ == '__main__':
    unittest.main()
