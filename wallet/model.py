"""Wallet operations shared by the terminal UI and future desktop clients."""
from __future__ import annotations

import datetime as dt
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

CHAIN = '2094b0868a27b032'
COIN = 100_000_000
MAX = (1 << 64) - 1
ADDRESS = re.compile(r'[0-9a-fA-F]{64}\Z')


class WalletError(Exception):
    pass


def amount(text: str) -> int:
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]{1,8})?', text):
        raise WalletError('Use a positive decimal amount with at most 8 decimal places.')
    whole, _, fraction = text.partition('.')
    value = int(whole) * COIN + int((fraction + '00000000')[:8])
    if value > MAX:
        raise WalletError('Amount exceeds the supported range.')
    return value


def coins(value: int | str) -> str:
    value = int(value)
    return f'{value // COIN:,}.{value % COIN:08d}'


def plain(value: int) -> str:
    return f'{value // COIN}.{value % COIN:08d}'


def address(value: str) -> str:
    if not ADDRESS.fullmatch(value):
        raise WalletError('An address must contain exactly 64 hexadecimal characters.')
    return value.lower()


def private_dir(path: Path) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    st = path.lstat()
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise WalletError(f'{path} must be a private directory owned by you (mode 0700).')


def read_private(path: Path) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        st = os.fstat(f.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
            raise WalletError('File must be regular, owned by you, and private (mode 0600).')
        data = f.read(2_000_001)
        if len(data) > 2_000_000:
            raise WalletError('Wallet state file is too large.')
        return data


def atomic_json(path: Path, data: dict) -> None:
    fd, name = tempfile.mkstemp(prefix='.wallet-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as f:
            json.dump(data, f, indent=2)
            f.write('\n')
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class Backend:
    def __init__(self, binary: Path):
        self.binary = str(binary.resolve())
        p = self.call('profile')
        if p.get('api') != 1 or p.get('chain_id') != CHAIN or p.get('network') != 'testnet-v5':
            raise WalletError('This wallet requires its matching testnet-v5 backend.')

    def call(self, *args: str) -> dict:
        try:
            result = subprocess.run([self.binary, *map(str, args)], capture_output=True,
                                    text=True, timeout=15, check=False)
        except subprocess.TimeoutExpired as exc:
            raise WalletError('Operation timed out. A saved send must be checked before retrying.') from exc
        except OSError as exc:
            raise WalletError('Cannot run wallet backend. Run make wallet first.') from exc
        try:
            data = json.loads(result.stdout)
        except (ValueError, TypeError) as exc:
            raise WalletError('Invalid response from the wallet backend.') from exc
        if result.returncode or not data.get('ok'):
            raise WalletError(data.get('error', 'Wallet operation failed.'))
        return data


class Store:
    def __init__(self, directory: Path):
        self.directory = directory
        private_dir(directory)
        self.lock = os.open(directory / 'lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            st = os.fstat(self.lock)
            if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
                raise WalletError('Wallet lock must be a private regular file owned by you.')
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except (OSError, WalletError) as exc:
            os.close(self.lock)
            raise WalletError('Another wallet window is using this state directory.') from exc
        self.path = directory / 'state.json'
        try:
            try:
                self.data = json.loads(read_private(self.path))
            except FileNotFoundError:
                self.data = {'version': 1, 'chain_id': CHAIN, 'receipts': [], 'contacts': {}}
            if self.data.get('version') != 1 or self.data.get('chain_id') != CHAIN:
                raise WalletError('Wallet state belongs to a different network or version.')
            if not isinstance(self.data.get('receipts'), list) or not isinstance(self.data.get('contacts'), dict):
                raise WalletError('Invalid wallet state.')
        except Exception:
            os.close(self.lock)
            raise

    def save(self) -> None:
        atomic_json(self.path, self.data)

    def close(self) -> None:
        os.close(self.lock)


def explorer_json(base: str, route: str) -> dict:
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme not in ('https', 'http') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise WalletError('Explorer must be an HTTPS URL, or HTTP on loopback for local testing.')
    if parsed.scheme != 'https' and parsed.hostname not in ('localhost', '127.0.0.1', '::1'):
        raise WalletError('Use HTTPS for a remote Explorer.')
    req = urllib.request.Request(base.rstrip('/') + route, headers={'Accept': 'application/json', 'User-Agent': 'Constella-Wallet/0.1'})
    # Do not follow a server redirect to another destination or protocol.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None
    with urllib.request.build_opener(NoRedirect).open(req, timeout=5) as response:
        data = response.read(1_000_001)
        if len(data) > 1_000_000:
            raise WalletError('Explorer response is too large.')
        return json.loads(data)


class Wallet:
    def __init__(self, backend: Backend, store: Store, key: Path, peer: str, explorer: str):
        self.backend, self.store = backend, store
        self.key, self.peer, self.explorer = key, peer, explorer
        self.address = ''
        self.account: dict | None = None
        self.account_at = 0.0
        self.history: list[dict] = []
        self.chain_height = 0
        self.history_at = 0.0
        self.history_error = ''
        self.node_error = ''

    def open(self, create: bool = False) -> None:
        if create:
            private_dir(self.key.parent)
        self.address = address(self.backend.call('new' if create else 'address', self.key)['address'])
        self.store.data['config'] = {'wallet': str(self.key), 'peer': self.peer, 'explorer': self.explorer}
        self.store.save()

    def receipts(self) -> list[dict]:
        return [r for r in self.store.data['receipts'] if r['from'] == self.address]

    def unresolved(self) -> list[dict]:
        return [r for r in self.receipts() if r.get('status') != 'applied']

    def refresh(self) -> None:
        if not self.address:
            return
        try:
            a = self.backend.call('balance', self.peer, self.address)
            for name in ('balance', 'nonce', 'next'):
                a[name] = int(a[name])
                if not 0 <= a[name] <= MAX:
                    raise WalletError('Invalid account value.')
            self.account, self.account_at, self.node_error = a, time.time(), ''
        except WalletError as exc:
            self.node_error = str(exc)
        self.history_error = ''
        if not self.explorer:
            self.history_error = 'No Explorer configured; transaction inclusion is unverified.'
            return
        try:
            stats = explorer_json(self.explorer, '/api/stats')
            if stats['meta']['chain_id'] != CHAIN:
                raise WalletError('Explorer is on a different chain. History was not updated.')
            tip = int(stats['meta']['height'])
            data = explorer_json(self.explorer, '/api/address/' + self.address)
            if data['address'] != self.address:
                raise WalletError('Explorer returned a different account.')
            rows = data['txs']
            if not isinstance(rows, list):
                raise WalletError('Invalid transaction history.')
            for row in rows:
                address(row['id']); address(row['from']); address(row['to'])
                if self.address not in (row['from'], row['to']):
                    raise WalletError('Explorer returned unrelated transactions.')
                for field in ('amount', 'fee', 'nonce', 'height'):
                    if type(row[field]) is not int or not 0 <= row[field] <= MAX:
                        raise WalletError('Invalid transaction value in Explorer history.')
            self.history, self.chain_height, self.history_at = rows, tip, time.time()
            by_id = {r['id']: r for r in rows}
            changed = False
            for receipt in self.receipts():
                row = by_id.get(receipt['id'])
                if self.account and not self.node_error and self.account['nonce'] <= int(receipt['nonce']):
                    # A lagging Explorer cannot override the peer's fresh account nonce.
                    if receipt.get('status') == 'applied' or (row and row['status'] == 'applied'):
                        receipt.update(status='unverified'); changed = True
                elif row and row['status'] == 'applied' and all(str(row[k]) == str(receipt[k]) for k in ('from', 'to', 'amount', 'fee', 'nonce')):
                    receipt.update(status='applied', height=row['height']); changed = True
                elif row and row['status'] != 'applied':
                    receipt.update(status='unverified'); changed = True
            if changed:
                self.store.save()
        except (OSError, ValueError, KeyError, TypeError, WalletError) as exc:
            self.history_error = f'History unavailable: {exc}'

    def review(self, to: str, value: str, fee: str) -> dict:
        to = address(to.strip())
        value_atoms, fee_atoms = amount(value.strip()), amount(fee.strip())
        if not value_atoms or value_atoms + fee_atoms > MAX:
            raise WalletError('Amount must be greater than zero and the total must fit the supported range.')
        if fee_atoms > min(value_atoms, COIN):
            raise WalletError('Fee exceeds the wallet safety limit.')
        if self.unresolved():
            raise WalletError('A saved transfer is still unverified. Check History; R retries that same transaction.')
        # Always obtain a fresh peer answer for review; Explorer balances cannot authorize a spend.
        a = self.backend.call('balance', self.peer, self.address)
        self.account = {**a, **{k: int(a[k]) for k in ('balance', 'nonce', 'next')}}
        self.account_at, self.node_error = time.time(), ''
        if int(a['nonce']) != int(a['next']):
            raise WalletError('Another outgoing transaction is pending. Wait for inclusion before sending.')
        if value_atoms + fee_atoms > int(a['balance']):
            raise WalletError('Insufficient balance including the fee.')
        return {'from': self.address, 'to': to, 'amount': value_atoms, 'fee': fee_atoms,
                'nonce': int(a['next']), 'peer': self.peer, 'wallet': str(self.key), 'chain_id': CHAIN}

    def send(self, review: dict) -> dict:
        if review['from'] != self.address or review['peer'] != self.peer or review['wallet'] != str(self.key) or review['chain_id'] != CHAIN:
            raise WalletError('Wallet settings changed. Review the payment again.')
        if self.unresolved():
            raise WalletError('Resolve the existing saved payment first.')
        tx = self.backend.call('prepare', self.key, self.peer, review['to'], plain(review['amount']), plain(review['fee']), str(review['nonce']))
        for name in ('from', 'to', 'amount', 'fee', 'nonce'):
            if str(tx[name]) != str(review[name]):
                raise WalletError('Signed transaction does not match the review. Nothing broadcast.')
        address(tx['id'])
        if not re.fullmatch(r'[0-9a-f]{304}', tx['raw']):
            raise WalletError('Invalid signed transaction. Nothing broadcast.')
        tx.update(status='saved', created_at=dt.datetime.now(dt.timezone.utc).isoformat())
        self.store.data['receipts'].append(tx)
        # If persistence fails, stop before network submission. A crash afterwards can only retry these bytes.
        self.store.save()
        return self.retry(tx)

    def retry(self, tx: dict) -> dict:
        if tx['from'] != self.address or tx.get('status') == 'applied':
            raise WalletError('This payment cannot be retried from this wallet.')
        try:
            result = self.backend.call('broadcast', self.peer, tx['raw'])
            tx['status'] = result['status']
            tx.pop('error', None)
        except WalletError as exc:
            tx.update(status='unknown', error=str(exc))
        self.store.save()
        return tx

    def backup(self, target: Path) -> None:
        # Validate through C first, then copy a small private regular file without overwriting.
        self.backend.call('address', self.key)
        data = read_private(self.key)
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        if self.backend.call('address', target)['address'] != self.address:
            raise WalletError('Backup address mismatch; retain the original wallet.')
        directory = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
