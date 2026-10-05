#!/usr/bin/env python3
"""Check operator verification gates without making network requests."""
from contextlib import redirect_stdout
from datetime import datetime, timedelta, timezone
import io
import json
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch

VERIFY = Path(__file__).with_name('verify.py')


class VerifyTests(unittest.TestCase):
    def check(self, changes=None, flags=(), rejected=None):
        meta = {'chain_id': '2094b0868a27b032', 'network': 'testnet', 'height': '473',
                'txs': '1', 'peer': 'true', 'check': 'ok', 'check_height': '473',
                'check_count': '3', 'check_total': '3',
                'check_at': datetime.now(timezone.utc).isoformat()}
        meta.update(changes or {})
        response = io.BytesIO(json.dumps({'meta': meta}).encode())
        with patch.object(sys, 'argv', [str(VERIFY), 'http://unused.invalid', *flags]), \
                patch('urllib.request.urlopen', return_value=response), redirect_stdout(io.StringIO()):
            if rejected:
                with self.assertRaisesRegex(SystemExit, rejected):
                    runpy.run_path(str(VERIFY), run_name='__main__')
            else:
                runpy.run_path(str(VERIFY), run_name='__main__')

    def test_exact_v5_checkpoint(self):
        self.check(flags=['--exact'])

    def test_v3_cannot_authorize_v5_rollout(self):
        self.check({'chain_id': 'a8f4562e57e74f9d'}, rejected='wrong chain')

    def test_incomplete_account_check(self):
        self.check({'check_count': '2'}, rejected='incomplete account')

    def test_stale_or_disconnected(self):
        self.check({'check_at': (datetime.now(timezone.utc)-timedelta(minutes=5)).isoformat()}, rejected='stale')
        self.check({'peer': 'false'}, rejected='disconnected')

    def test_held_height_must_match(self):
        self.check({'check_height': '472'}, flags=['--exact'], rejected='hold mining')

    def test_fresh_genesis_and_reused_history(self):
        self.check({'height': '0', 'txs': '0'}, flags=['--genesis'])
        self.check(flags=['--genesis'], rejected='not fresh genesis')


if __name__ == '__main__':
    unittest.main()
