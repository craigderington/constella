import contextlib
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'wallet'))
from model import Backend, CHAIN, MAX, Store, Wallet, WalletError, amount, coins, explorer_json, plain
from tui import UI, demo_wallet, safe


class MoneyTests(unittest.TestCase):
    def test_exact_amounts(self):
        self.assertEqual(amount('0.00000001'), 1)
        self.assertEqual(amount('184467440737.09551615'), MAX)
        self.assertEqual(amount(plain(MAX)), MAX)
        self.assertEqual(coins(217218415772), '2,172.18415772')
        for value in ('1e8', '-1', 'NaN', '1.000000001', '184467440737.09551616', ' 1', '1,000', ''):
            with self.subTest(value=value), self.assertRaises(WalletError):
                amount(value)

    def test_terminal_sanitization(self):
        self.assertEqual(safe('a\x1b[31m\n\u202eb'), 'a?[31m??b')


class FakeBackend:
    def __init__(self):
        self.calls = []
        self.error = False
        self.next = 0
        self.nonce = 0

    def call(self, command, *args):
        self.calls.append((command, args))
        if command == 'balance':
            return {'balance': '1000000000', 'nonce': str(self.nonce), 'next': str(self.next), 'height': 42}
        if command == 'prepare':
            return {'ok': True, 'id': '11' * 32, 'raw': '00' * 152, 'from': 'aa' * 32,
                    'to': args[2], 'amount': str(amount(args[3])), 'fee': str(amount(args[4])), 'nonce': args[5]}
        if command == 'broadcast':
            if self.error:
                raise WalletError('lost acknowledgement')
            return {'status': 'accepted'}
        if command == 'address':
            return {'address': 'aa' * 32}
        raise AssertionError(command)


class ModelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.store = Store(self.root / 'state')
        self.backend = FakeBackend()
        self.wallet = Wallet(self.backend, self.store, self.root / 'wallet.key', '127.0.0.1:18473', 'https://example.test')
        self.wallet.address = 'aa' * 32

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def review(self):
        return self.wallet.review('bb' * 32, '1', '0.001')

    def test_review_has_no_signing_side_effect(self):
        self.review()
        self.assertEqual([c[0] for c in self.backend.calls], ['balance'])
        self.assertFalse(self.wallet.receipts())

    def test_insufficient_pending_and_fee(self):
        for value, fee in [('11', '.001'), ('1', '2'), ('0', '0'), ('1e1', '0')]:
            with self.assertRaises(WalletError): self.wallet.review('bb' * 32, value, fee)
        self.backend.next = 1
        with self.assertRaises(WalletError): self.review()

    def test_save_before_broadcast(self):
        review = self.review()
        old = self.backend.call
        def call(command, *args):
            if command == 'broadcast':
                disk = json.loads(self.store.path.read_text())
                self.assertEqual(disk['receipts'][0]['raw'], args[1])
                self.assertEqual(disk['receipts'][0]['status'], 'saved')
            return old(command, *args)
        self.backend.call = call
        receipt = self.wallet.send(review)
        self.assertEqual(receipt['status'], 'accepted')
        with self.assertRaises(WalletError): self.review()

    def test_disk_failure_prevents_broadcast(self):
        review = self.review()
        with patch.object(self.store, 'save', side_effect=OSError('disk full')):
            with self.assertRaises(OSError): self.wallet.send(review)
        self.assertNotIn('broadcast', [c[0] for c in self.backend.calls])

    def test_lost_ack_restart_reuses_exact_bytes(self):
        self.backend.error = True
        tx = self.wallet.send(self.review())
        self.assertEqual(tx['status'], 'unknown')
        self.store.close()
        self.store = Store(self.root / 'state')
        self.wallet.store = self.store
        with self.assertRaises(WalletError): self.review()
        self.backend.error = False
        self.wallet.retry(self.wallet.unresolved()[0])
        submitted = [c[1][1] for c in self.backend.calls if c[0] == 'broadcast']
        self.assertEqual(submitted, [tx['raw'], tx['raw']])
        self.assertEqual(sum(c[0] == 'prepare' for c in self.backend.calls), 1)

    def test_settings_change_prevents_sign(self):
        review = self.review(); self.wallet.peer = 'other:18473'
        with self.assertRaises(WalletError): self.wallet.send(review)
        self.assertNotIn('prepare', [c[0] for c in self.backend.calls])

    def test_signed_review_mismatch_prevents_broadcast(self):
        review = self.review()
        original = self.backend.call
        def call(command, *args):
            data = original(command, *args)
            if command == 'prepare': data['to'] = 'cc' * 32
            return data
        self.backend.call = call
        with self.assertRaises(WalletError): self.wallet.send(review)
        self.assertNotIn('broadcast', [c[0] for c in self.backend.calls])

    def test_chain_mismatch_does_not_confirm(self):
        self.wallet.send(self.review())
        with patch('model.explorer_json', return_value={'meta': {'chain_id': 'wrong', 'height': '99'}}):
            self.wallet.refresh()
        self.assertTrue(self.wallet.unresolved())
        self.assertIn('different chain', self.wallet.history_error)

    def test_inclusion_and_reorg(self):
        tx = self.wallet.send(self.review())
        self.backend.nonce = self.backend.next = 1
        row = {**tx, 'amount': 100000000, 'fee': 100000, 'nonce': 0, 'status': 'applied', 'height': 43}
        values = [{'meta': {'chain_id': CHAIN, 'height': '45'}}, {'address': self.wallet.address, 'txs': [row]}]
        with patch('model.explorer_json', side_effect=values): self.wallet.refresh()
        self.assertFalse(self.wallet.unresolved())
        row['status'] = 'orphaned'
        with patch('model.explorer_json', side_effect=values): self.wallet.refresh()
        self.assertTrue(self.wallet.unresolved())

    def test_stale_explorer_cannot_hide_peer_rollback(self):
        tx = self.wallet.send(self.review())
        tx['status'] = 'applied'
        row = {**tx, 'amount': 100000000, 'fee': 100000, 'nonce': 0, 'height': 43}
        values = [{'meta': {'chain_id': CHAIN, 'height': '45'}}, {'address': self.wallet.address, 'txs': [row]}]
        with patch('model.explorer_json', side_effect=values): self.wallet.refresh()
        self.assertEqual(tx['status'], 'unverified')
        with self.assertRaises(WalletError): self.review()

    def test_private_state_and_lock(self):
        self.store.save()
        self.assertEqual(self.store.path.stat().st_mode & 0o777, 0o600)
        with self.assertRaises(WalletError): Store(self.root / 'state')
        link = self.root / 'alias'; link.symlink_to(self.root / 'state')
        with self.assertRaises(WalletError): Store(link)

    def test_backup_no_overwrite(self):
        self.wallet.key.write_text('42' * 32 + '\n'); self.wallet.key.chmod(0o600)
        target = self.root / 'backup.key'
        self.wallet.backup(target)
        self.assertEqual(target.read_bytes(), self.wallet.key.read_bytes())
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)
        with self.assertRaises(FileExistsError): self.wallet.backup(target)

    def test_remote_plaintext_explorer_refused(self):
        with self.assertRaises(WalletError): explorer_json('http://example.test', '/api/stats')
        with self.assertRaises(WalletError): explorer_json('https://user:secret@example.test', '/api/stats')


class BackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / 'constella-wallet-core'
        cls.backend = Backend(cls.binary)
        cls.peer_binary = ROOT / 'wallet/tests/peer-test'

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.key = self.root / 'wallet.key'
        self.addr = self.backend.call('new', self.key)['address']

    def tearDown(self):
        self.tmp.cleanup()

    def test_key_round_trip_and_refusals(self):
        self.assertEqual(self.backend.call('address', self.key)['address'], self.addr)
        retained = self.key.read_bytes()
        with self.assertRaises(WalletError): self.backend.call('new', self.key)
        self.assertEqual(self.key.read_bytes(), retained)
        self.key.chmod(0o644)
        with self.assertRaises(WalletError): self.backend.call('address', self.key)
        self.key.chmod(0o600)
        link = self.root / 'alias.key'; link.symlink_to(self.key)
        with self.assertRaises(WalletError): self.backend.call('address', link)

    def test_bad_values_refused_before_network(self):
        for value, fee in [('0', '0'), ('1', '2'), ('184467440737.09551615', '1'), ('1.000000001', '0')]:
            with self.assertRaises(WalletError):
                self.backend.call('prepare', self.key, '127.0.0.1:1', 'bb' * 32, value, fee, '0')
        with self.assertRaises(WalletError): self.backend.call('broadcast', '127.0.0.1:1', '00' * 152)

    @contextlib.contextmanager
    def peer(self, drop=False):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        with (self.root / 'peer.log').open('w+') as log:
            p = subprocess.Popen([str(self.peer_binary), str(port)] + (['drop'] if drop else []), stdout=log, stderr=log)
            try:
                peer = f'127.0.0.1:{port}'
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if p.poll() is not None: self.fail('peer exited')
                    try:
                        self.backend.call('balance', peer, self.addr); break
                    except WalletError: time.sleep(.05)
                else: self.fail('peer not ready')
                yield peer
            finally:
                p.terminate(); p.wait(timeout=5)

    def test_authenticated_sign_broadcast_duplicate_and_nonce_guard(self):
        with self.peer() as peer:
            balance = self.backend.call('balance', peer, self.addr)
            self.assertEqual(balance['balance'], '1000000000')
            with self.assertRaises(WalletError):
                self.backend.call('prepare', self.key, peer, 'bb' * 32, '1', '.001', '1')
            tx = self.backend.call('prepare', self.key, peer, 'bb' * 32, '1', '.001', '0')
            self.assertEqual(self.backend.call('broadcast', peer, tx['raw'])['status'], 'accepted')
            self.assertEqual(self.backend.call('broadcast', peer, tx['raw'])['status'], 'duplicate')
            with self.assertRaises(WalletError): self.backend.call('prepare', self.key, peer, 'bb' * 32, '1', '.001', '0')
            wire = tx['raw'][:-2] + ('00' if tx['raw'][-2:] != '00' else '01')
            with self.assertRaises(WalletError): self.backend.call('broadcast', peer, wire)

    def test_real_lost_ack_and_restart(self):
        with self.peer(drop=True) as peer:
            store = Store(self.root / 'state')
            try:
                wallet = Wallet(self.backend, store, self.key, peer, '')
                wallet.open()
                tx = wallet.send(wallet.review('bb' * 32, '1', '0.001'))
                self.assertEqual(tx['status'], 'unknown')
                store.close(); store = Store(self.root / 'state'); wallet.store = store
                tx = wallet.retry(wallet.unresolved()[0])
                self.assertEqual(tx['status'], 'duplicate')
                self.assertEqual(len(wallet.receipts()), 1)
            finally: store.close()

    def test_real_node_applies_transfer_and_replays_it_once(self):
        # Public deterministic test seed, never a user's wallet. Real proof, node and ledger.
        key = self.root / 'fixture.key'
        key.write_text('42' + '00' * 31 + '\n'); key.chmod(0o600)
        sender = self.backend.call('address', key)['address']
        data = self.root / 'chain'; data.mkdir()
        fixture = data / 'shares.testnet-v5'
        helper = ROOT / 'wallet/tests/chain-test'
        subprocess.run([helper, 'fund', fixture], check=True, timeout=120)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        peer = f'127.0.0.1:{port}'
        env = {k: v for k, v in os.environ.items() if not k.startswith('CONSTELLA_')}
        env.update(CONSTELLA_DATA=str(data), CONSTELLA_PORT=str(port), CONSTELLA_MINE='0', CONSTELLA_PEERS='127.0.0.1:1')
        with (self.root / 'node.log').open('w') as log:
            def start():
                p = subprocess.Popen([ROOT / 'constella-testnet-v5'], env=env, stdout=log, stderr=log)
                for _ in range(60):
                    try:
                        self.backend.call('balance', peer, sender); return p
                    except WalletError:
                        if p.poll() is not None: self.fail('real node exited')
                        time.sleep(.05)
                p.terminate(); p.wait(timeout=5); self.fail('real node unavailable')
            p = start()
            try:
                self.assertEqual(self.backend.call('balance', peer, sender)['balance'], '1500000000')
                tx = self.backend.call('prepare', key, peer, self.addr, '1', '.001', '0')
                self.assertEqual(self.backend.call('broadcast', peer, tx['raw'])['status'], 'accepted')
                subprocess.run([helper, 'include', fixture, tx['raw'], peer], check=True, timeout=120)
                self.assertEqual(self.backend.call('balance', peer, self.addr)['balance'], '100000000')
                self.assertEqual(self.backend.call('balance', peer, sender)['nonce'], '1')
                p.terminate(); p.wait(timeout=5); self.assertEqual(p.returncode, 0)
                p = start()
                self.assertEqual(self.backend.call('balance', peer, self.addr)['balance'], '100000000')
                self.assertEqual(self.backend.call('balance', peer, sender)['nonce'], '1')
                self.assertEqual(self.backend.call('broadcast', peer, tx['raw'])['status'], 'rejected')
                self.assertEqual(self.backend.call('balance', peer, self.addr)['balance'], '100000000')
            finally:
                p.terminate(); p.wait(timeout=5)


class FakeScreen:
    def __init__(self, h, w): self.h, self.w = h, w; self.erase()
    def getmaxyx(self): return self.h, self.w
    def erase(self): self.lines = [' ' * self.w for _ in range(self.h)]
    def addnstr(self, y, x, text, n, attr):
        assert 0 <= y < self.h and 0 <= x < self.w
        text = text[:n]; assert x + len(text) < self.w
        self.lines[y] = self.lines[y][:x] + text + self.lines[y][x+len(text):]
    def refresh(self): pass
    def keypad(self, value): pass
    def timeout(self, value): pass


class UITests(unittest.TestCase):
    def ui(self, size=(30, 100)):
        with patch('tui.curses.curs_set'), patch('tui.curses.has_colors', return_value=False):
            return UI(FakeScreen(*size), demo_wallet(), demo=True)

    def test_tabs_small_terminal_and_review_fit(self):
        for size in [(24, 78), (27, 78), (30, 100), (40, 140), (12, 40)]:
            ui = self.ui(size)
            try:
                for tab in range(4): ui.tab = tab; ui.draw()
                ui.review = {'from': 'aa'*32, 'to': 'bb'*32, 'amount': 100000000, 'fee': 100000, 'nonce': 0}
                ui.form = {'kind': 'confirm', 'title': 'Review your payment', 'fields': [['Type SEND to confirm', '']], 'index': 0}
                ui.draw()
                if size[0] >= 24:
                    text = '\n'.join(ui.screen.lines)
                    self.assertIn('Total:', text)
                    self.assertIn('Sign and send', text)
            finally: ui.pool.shutdown()

    def test_escape_never_sends_and_demo_is_read_only(self):
        ui = self.ui()
        try:
            ui.send_form(); self.assertIsNone(ui.form)
            ui.form = {'kind': 'confirm', 'fields': [['Confirm', 'SEND']], 'index': 0}
            ui.form_key('\x1b')
            self.assertIsNone(ui.form); self.assertIsNone(ui.future)
        finally: ui.pool.shutdown()

    def test_enter_without_send_cannot_sign(self):
        ui = self.ui()
        try:
            ui.form = {'kind': 'confirm', 'fields': [['Confirm', '']], 'index': 0}
            ui.form_key('\n')
            self.assertIsNone(ui.future)
        finally: ui.pool.shutdown()


if __name__ == '__main__': unittest.main()
