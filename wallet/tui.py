#!/usr/bin/env python3
"""Constella testnet-v5 wallet. Python standard library + existing C wallet core."""
from __future__ import annotations

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import curses
import os
from pathlib import Path
import sys
import time

from model import Backend, CHAIN, Store, Wallet, WalletError, address, coins, private_dir

TABS = ['Overview', 'Send', 'Receive', 'History']


def safe(value) -> str:
    """No terminal controls, bidi overrides, or unexpected wide glyphs from peers/config."""
    return ''.join(c if ' ' <= c <= '~' else '?' for c in str(value))


def short(value: str, n: int = 10) -> str:
    return value[:n] + '...' + value[-6:]


class UI:
    def __init__(self, screen, wallet, watch=False, demo=False):
        self.screen, self.wallet, self.watch, self.demo = screen, wallet, watch, demo
        self.tab = 0
        self.message = 'Welcome. Your keys stay on this device.'
        self.error = False
        self.form = None
        self.review = None
        self.selected = 0
        self.future = None
        self.job = ''
        self.pool = ThreadPoolExecutor(max_workers=1)
        self.refresh_due = 0.0
        self.colors = False
        curses.curs_set(0)
        screen.keypad(True)
        screen.timeout(100)
        if curses.has_colors():
            curses.start_color()
            try:
                curses.use_default_colors()
                bg = -1
            except curses.error:
                bg = curses.COLOR_BLACK
            for n, fg in enumerate((curses.COLOR_CYAN, curses.COLOR_WHITE, curses.COLOR_YELLOW, curses.COLOR_GREEN, curses.COLOR_RED, curses.COLOR_BLUE), 1):
                curses.init_pair(n, fg, bg)
            self.colors = True

    def style(self, color=2, bold=False):
        return (curses.color_pair(color) if self.colors else 0) | (curses.A_BOLD if bold else 0)

    def put(self, y, x, text, color=2, bold=False):
        h, w = self.screen.getmaxyx()
        if y < 0 or y >= h or x >= w - 1:
            return
        try:
            self.screen.addnstr(y, max(0, x), safe(text), max(0, w - max(0, x) - 1), self.style(color, bold))
        except curses.error:
            pass

    def line(self, y):
        self.put(y, 2, '-' * (self.screen.getmaxyx()[1] - 4), 6)

    def task(self, name, function):
        if self.future:
            return
        self.job = name
        self.future = self.pool.submit(function)

    def result(self):
        if not self.future or not self.future.done():
            return
        future, name = self.future, self.job
        self.future = None
        self.job = ''
        try:
            result = future.result()
            self.error = False
            if name == 'review':
                self.review = result
                self.form = {'kind': 'confirm', 'title': 'Review your payment', 'fields': [['Type SEND to confirm', '']], 'index': 0}
            elif name in ('send', 'retry'):
                self.form = None; self.review = None; self.tab = 3
                self.message = 'Payment ' + result['status'] + '. Saved locally; inclusion is checked separately.'
                if result.get('error'):
                    self.message = result['error']; self.error = True
                self.refresh_due = 0
            elif name == 'open':
                self.form = None; self.tab = 0; self.message = 'Wallet ready. B creates a private backup.'
                self.refresh_due = 0
            elif name == 'backup':
                self.form = None; self.message = 'Backup written and its address verified. Keep it private.'
            elif name == 'refresh':
                self.refresh_due = time.monotonic() + 15
                if self.wallet.node_error:
                    self.message = self.wallet.node_error; self.error = True
                else:
                    self.message = 'Balance updated from the v5 node. History is supplied by the Explorer.'
        except Exception as exc:
            self.message = str(exc); self.error = True
            if name == 'send':
                # A failed persistence acknowledgement may follow a successful broadcast.
                # Leave any receipt visible and require recovery through History.
                self.form = None; self.review = None; self.tab = 3
            self.refresh_due = time.monotonic() + 15

    def setup(self, create=False):
        self.form = {'kind': 'new' if create else 'open', 'title': 'Create a new testnet wallet' if create else 'Open wallet / restore from backup',
                     'fields': [['Wallet file', str(self.wallet.key)], ['Node', self.wallet.peer], ['Explorer (optional)', self.wallet.explorer]], 'index': 0}

    def send_form(self):
        if self.watch or self.demo:
            self.message = 'Read-only preview: sending is disabled.'; return
        self.form = {'kind': 'send', 'title': 'Send testnet coins',
                     'fields': [['Recipient address (or saved contact)', ''], ['Amount', ''], ['Fee', '0.001']], 'index': 0}

    def submit(self):
        f = self.form
        values = [field[1].strip() for field in f['fields']]
        kind = f['kind']
        if kind == 'send':
            recipient = self.wallet.store.data['contacts'].get(values[0], values[0])
            self.task('review', lambda: self.wallet.review(recipient, values[1], values[2]))
        elif kind == 'confirm':
            if values[0] != 'SEND':
                self.message = 'Type SEND exactly, then Enter; Esc cancels without signing.'; self.error = True; return
            reviewed = dict(self.review)
            self.task('send', lambda: self.wallet.send(reviewed))
        elif kind in ('new', 'open'):
            if not values[0] or not values[1]:
                raise WalletError('Wallet file and node are required.')
            def open_wallet():
                # Retain the working wallet if opening a replacement fails.
                candidate = Wallet(self.wallet.backend, self.wallet.store, Path(values[0]).expanduser().absolute(), values[1], values[2])
                candidate.open(create=kind == 'new')
                self.wallet = candidate
            self.task('open', open_wallet)
        elif kind == 'backup':
            target = Path(values[0]).expanduser().absolute()
            self.task('backup', lambda: self.wallet.backup(target))
        elif kind == 'contact':
            if not values[0] or len(values[0]) > 32 or not all(' ' <= c <= '~' for c in values[0]):
                raise WalletError('Use a contact name of 1-32 printable characters.')
            self.wallet.store.data['contacts'][values[0]] = address(values[1])
            self.wallet.store.save(); self.form = None; self.message = 'Contact saved. Use its name in the Send form.'

    def form_key(self, key):
        if key == '\x1b':
            self.form = None; self.review = None; self.message = 'Cancelled; no new payment sent.'; return
        f = self.form; index = f['index']; fields = f['fields']
        if key in ('\t', curses.KEY_DOWN):
            f['index'] = (index + 1) % (len(fields) + 1)
        elif key in (curses.KEY_BTAB, curses.KEY_UP):
            f['index'] = (index - 1) % (len(fields) + 1)
        elif key in ('\n', '\r', curses.KEY_ENTER):
            if index == len(fields) or f['kind'] == 'confirm':
                self.submit()
            else:
                f['index'] += 1
        elif index < len(fields):
            if key in ('\b', '\x7f', curses.KEY_BACKSPACE):
                fields[index][1] = fields[index][1][:-1]
            elif key == '\x15':
                fields[index][1] = ''
            elif isinstance(key, str) and ' ' <= key <= '~' and len(fields[index][1]) < 1024:
                fields[index][1] += key

    def rows(self):
        rows = [dict(r) for r in self.wallet.history]
        receipts = {r['id']: r for r in self.wallet.receipts()}
        for row in rows:
            if row['id'] in receipts and receipts[row['id']].get('status') != 'applied':
                row['status'] = receipts[row['id']]['status']
        ids = {r['id'] for r in rows}
        rows.extend(dict(r) for r in reversed(self.wallet.receipts()) if r['id'] not in ids)
        # Unresolved local submissions remain reachable at the top, including after restart.
        rows.sort(key=lambda r: r.get('status') == 'applied')
        return rows

    def key(self, key):
        if key in ('q', 'Q', '\x03') and not self.form:
            if self.future:
                self.message = 'Finishing the current operation. Quit when the activity indicator clears.'; return True
            return False
        h, w = self.screen.getmaxyx()
        if (h < 24 or w < 78) and key != '\x1b':
            return True  # Never submit an invisible review after a resize.
        if self.future:
            if self.job == 'refresh':
                if key in ('1', '2', '3', '4'):
                    self.tab = int(key) - 1
                elif key in ('\t', curses.KEY_RIGHT):
                    self.tab = (self.tab + 1) % 4
                elif key in (curses.KEY_BTAB, curses.KEY_LEFT):
                    self.tab = (self.tab - 1) % 4
            return True
        if self.form:
            self.form_key(key); return True
        if not self.wallet.address:
            if key in ('n', 'N'): self.setup(True)
            elif key in ('o', 'O'): self.setup(False)
            return True
        if key in ('1', '2', '3', '4'):
            self.tab = int(key) - 1
        elif key in ('\t', curses.KEY_RIGHT): self.tab = (self.tab + 1) % 4
        elif key in (curses.KEY_BTAB, curses.KEY_LEFT): self.tab = (self.tab - 1) % 4
        elif key in ('r', 'R'):
            if key == 'R' and self.tab == 3 and not (self.watch or self.demo):
                rows = self.rows()
                if rows:
                    row = rows[min(self.selected, len(rows) - 1)]
                    receipt = next((r for r in self.wallet.unresolved() if r['id'] == row['id']), None)
                    if receipt:
                        self.task('retry', lambda: self.wallet.retry(receipt))
                    else: self.message = 'No saved unverified transaction selected.'
            elif not self.demo:
                self.task('refresh', self.wallet.refresh)
        elif key in ('s', 'S') or (key in ('\n', '\r') and self.tab == 1): self.send_form()
        elif key in ('b', 'B') and not (self.watch or self.demo):
            self.form = {'kind': 'backup', 'title': 'Back up your wallet key', 'fields': [['New backup file (never overwritten)', '']], 'index': 0}
        elif key in ('o', 'O') and not (self.watch or self.demo): self.setup(False)
        elif key in ('a', 'A') and not (self.watch or self.demo):
            self.form = {'kind': 'contact', 'title': 'Save a recipient', 'fields': [['Contact name', ''], ['Address', '']], 'index': 0}
        elif key in ('c', 'C'):
            payload = base64.b64encode(self.wallet.address.encode()).decode()
            sys.stdout.write('\033]52;c;' + payload + '\a'); sys.stdout.flush()
            self.message = 'Address copy requested. Your terminal must support OSC 52 clipboard access.'
        elif key == curses.KEY_DOWN: self.selected += 1
        elif key == curses.KEY_UP: self.selected = max(0, self.selected - 1)
        return True

    def draw_form(self):
        f = self.form
        self.put(7, 4, f['title'], 1, True)
        y = 9
        if f['kind'] == 'confirm':
            r = self.review
            for text in ('Network: TESTNET v5 / ' + CHAIN, 'From: ' + r['from'], 'To:   ' + r['to'],
                         'Amount: ' + coins(r['amount']) + '   Fee: ' + coins(r['fee']),
                         'Total:  ' + coins(r['amount'] + r['fee']) + '   Nonce: ' + str(r['nonce'])):
                self.put(y, 4, text, 3 if text.startswith('Total') else 2); y += 1
            y += 1
        for index, (label, value) in enumerate(f['fields']):
            active = index == f['index']
            self.put(y, 4, label, 1 if active else 2, active)
            width = self.screen.getmaxyx()[1] - 12
            shown = value[-max(1, width - 3):]
            self.put(y + 1, 4, ('> ' if active else '  ') + shown + ('_' if active else ''), 2, active)
            y += 3
        labels = {'send': 'Review payment', 'confirm': 'Sign and send', 'new': 'Create wallet', 'open': 'Open wallet', 'backup': 'Write private backup', 'contact': 'Save contact'}
        self.put(y, 4, '[ ' + labels[f['kind']] + ' ]', 3, f['index'] == len(f['fields']))
        self.put(y + 2, 4, 'Tab / arrows: move    Enter: continue    Ctrl-U: clear field    Esc: cancel', 6)
        if f['kind'] in ('new', 'backup'):
            self.put(y + 3, 4, 'Key files are unencrypted. Store backups privately; no recovery service exists.', 3)

    def draw(self):
        self.screen.erase()
        h, w = self.screen.getmaxyx()
        if w < 78 or h < 24:
            self.put(1, 2, 'CONSTELLA wallet', 1, True)
            self.put(3, 2, f'Terminal is {w} x {h}. Resize to at least 78 x 24.')
            self.put(5, 2, 'q quits once any current operation finishes.')
            self.screen.refresh(); return
        self.put(1, 3, '*  C O N S T E L L A', 1, True)
        self.put(2, 6, 'Your corner of the constellation.', 6)
        badge = 'DEMO / NO NETWORK' if self.demo else 'WATCH ONLY' if self.watch else 'TESTNET v5'
        self.put(1, w - len(badge) - 4, badge, 3, True)
        self.put(2, w - 25, CHAIN, 6)
        self.line(4)
        x = 4
        for i, name in enumerate(TABS):
            self.put(5, x, f'{i + 1} {name}', 1 if i == self.tab else 2, i == self.tab)
            x += len(name) + 8
        if self.form:
            self.draw_form()
        elif not self.wallet.address:
            self.put(8, 5, 'A wallet of your own.', 1, True)
            self.put(10, 5, 'Create a fresh testnet wallet, or open a key file you already own.')
            self.put(12, 5, '[ N ] New wallet       [ O ] Open / restore wallet', 3, True)
            self.put(15, 5, 'No miner. No Docker. Sign locally and connect to a v5 node.')
            self.put(17, 5, 'Back up your key after creation. There is no password reset.', 3)
        elif self.tab == 0:
            self.overview()
        elif self.tab == 1:
            self.put(8, 4, 'Send a little starlight.', 1, True)
            self.put(10, 4, 'Review the recipient, amount, fee and total before signing.')
            self.put(12, 4, '[ Enter / S ] Compose a payment', 3, True)
            self.put(14, 4, '[ A ] Save a contact     [ B ] Back up wallet     [ O ] Open wallet / settings', 6)
            contacts = self.wallet.store.data.get('contacts', {})
            for i, (name, addr) in enumerate(list(contacts.items())[:max(0, h - 22)]):
                self.put(17 + i, 4, f'{name:32s} {short(addr)}')
            if self.wallet.unresolved():
                self.put(h - 7, 4, 'A saved payment needs verification. Open History before sending another.', 3)
        elif self.tab == 2:
            self.put(8, 4, 'Receive testnet coins', 1, True)
            self.put(10, 4, 'Share this public address. Keep your wallet key private.', 2)
            self.put(12, 4, self.wallet.address, 3, True)
            self.put(15, 4, '[ C ] Copy address through your terminal', 1)
            self.put(17, 4, 'Network: testnet v5   Chain: ' + CHAIN)
            self.put(19, 4, 'This address is for this testnet wallet; check the network before sending.', 6)
        else:
            self.draw_history()
        self.line(h - 5)
        activity = ('|/-\\'[int(time.monotonic() * 6) % 4] + ' ' + self.job + '...') if self.future else self.message
        self.put(h - 4, 3, activity, 5 if self.error and not self.future else 1)
        hints = 'Tab / arrows: move   Enter: continue   Ctrl-U: clear field   Esc: cancel' if self.form else '1-4 / Tab: screens   r: refresh   S: send   C: copy   B: backup   q: quit'
        self.put(h - 3, 3, hints, 6)
        if self.wallet.address:
            a = self.wallet.account
            connected = bool(a and not self.wallet.node_error and time.time() - self.wallet.account_at < 60)
            label = 'demo data (offline)' if self.demo else 'node connected' if connected else 'node offline / stale'
            self.put(h - 2, 3, label + '  |  ' + self.wallet.peer, 4 if connected else 3)
            if a:
                self.put(h - 2, w - 24, 'share ' + f'{a["height"]:,}', 6)
        self.screen.refresh()

    def overview(self):
        h, w = self.screen.getmaxyx(); a = self.wallet.account
        self.put(8, 4, 'BALANCE', 6, True)
        self.put(10, 4, coins(a['balance']) if a else 'Connecting...', 2, True)
        self.put(12, 4, 'testnet coins', 1)
        self.put(8, max(42, w - 33), 'WALLET', 6, True)
        self.put(10, max(42, w - 33), short(self.wallet.address), 3)
        self.put(12, max(42, w - 33), 'Outgoing nonce: ' + str(a['nonce'] if a else '--'))
        age = int(time.time() - self.wallet.account_at) if a else None
        self.put(13, 4, f'Node balance checked {age}s ago' if a else 'Waiting for an authenticated node response.', 6)
        self.line(14)
        self.put(16, 4, 'RECENT ACTIVITY', 1, True)
        rows = self.rows()
        if not rows:
            self.put(18, 4, 'No recent transfers yet. Receive coins to get started.', 6)
        for i, row in enumerate(rows[:max(0, min(3, h - 23))]):
            sent = row['from'] == self.wallet.address
            self.put(18 + i, 4, ('Sent    ' if sent else 'Received') + '  ' + coins(row['amount']) + '  ' + safe(row['status']), 2)
        if self.wallet.history_error:
            self.put(h - 6, 4, self.wallet.history_error, 3)

    def draw_history(self):
        h, w = self.screen.getmaxyx(); rows = self.rows()
        self.put(7, 4, 'Recent transactions', 1, True)
        self.put(8, 4, 'Explorer history + local receipts. Inclusion can change after a reorganization.', 6)
        if not rows:
            self.put(11, 4, 'No recent transactions available.', 6)
        else:
            self.selected = min(self.selected, len(rows) - 1)
            capacity = max(1, h - 20)
            start = max(0, self.selected - capacity + 1)
            self.put(10, 4, 'DIRECTION     AMOUNT                 STATUS              SHARE', 6)
            for i, row in enumerate(rows[start:start + capacity]):
                selected = start + i == self.selected
                direction = 'Sent' if row['from'] == self.wallet.address else 'Received'
                self.put(11 + i, 4, f'{">" if selected else " "} {direction:10} {coins(row["amount"]):21} {safe(row["status"]):19} {row.get("height", "--")}', 1 if selected else 2, selected)
            row = rows[self.selected]
            self.put(h - 9, 4, 'Tx: ' + row['id'], 3)
            self.put(h - 8, 4, 'To: ' + row['to'])
            depth = max(0, self.wallet.chain_height - int(row.get('height', self.wallet.chain_height)) + 1) if row['status'] == 'applied' else 0
            self.put(h - 7, 4, f'Fee {coins(row["fee"])}  |  {depth} share confirmations (Explorer snapshot)', 6)
        self.put(h - 6, 4, self.wallet.history_error or 'Up/down: select    R: retry the same saved transaction    r: refresh', 3 if self.wallet.history_error else 6)

    def run(self):
        try:
            running = True
            while running:
                self.result()
                if self.wallet.address and not self.form and not self.future and not self.demo and time.monotonic() >= self.refresh_due:
                    self.task('refresh', self.wallet.refresh)
                self.draw()
                try:
                    key = self.screen.get_wch()
                except curses.error:
                    continue
                try:
                    running = self.key(key)
                except (WalletError, OSError, ValueError) as exc:
                    self.message = str(exc); self.error = True
        finally:
            self.pool.shutdown(wait=True)


def demo_wallet():
    class Memory:
        data = {'contacts': {}, 'receipts': []}
    w = Wallet(None, Memory(), Path('/demo/wallet.key'), 'demo.node:18473', '')
    w.address = 'a1' * 32
    w.account = {'balance': 217218415772, 'nonce': 1, 'next': 1, 'height': 39180}
    w.account_at = w.history_at = time.time(); w.chain_height = 39180
    w.history = [{'id': 'e3' * 32, 'from': w.address, 'to': 'fc' * 32, 'amount': 100000000, 'fee': 100000, 'height': 39072, 'status': 'applied'},
                 {'id': '66' * 32, 'from': 'fc' * 32, 'to': w.address, 'amount': 100000000, 'fee': 100000, 'height': 39079, 'status': 'applied'}]
    return w


def main():
    parser = argparse.ArgumentParser(description='Constella testnet-v5 wallet: no Docker or local miner required.')
    parser.add_argument('--wallet', type=Path, help='existing key file; never copied automatically')
    parser.add_argument('--node', help='authenticated P2P host:port')
    parser.add_argument('--explorer', help='HTTPS Explorer for recent history; empty disables it')
    parser.add_argument('--watch', help='public address; disables signing and key access')
    parser.add_argument('--state-dir', type=Path, default=Path.home() / '.local/share/constella-wallet-v5')
    parser.add_argument('--backend', type=Path, default=Path(__file__).resolve().parents[1] / 'constella-wallet-core')
    parser.add_argument('--demo', action='store_true', help='interactive preview: no files, keys or network access')
    args = parser.parse_args()
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        parser.error('Open the wallet in an interactive terminal.')
    store = None
    try:
        os.umask(0o077)
        if args.demo:
            wallet = demo_wallet()
        else:
            store = Store(args.state_dir.expanduser().absolute())
            config = store.data.get('config', {})
            wallet = Wallet(Backend(args.backend), store,
                            (args.wallet or Path(config.get('wallet', str(Path.home() / '.constella-testnet-v5/wallet.key')))).expanduser().absolute(),
                            args.node or config.get('peer', 'explorer.catasterism.xyz:7043'),
                            args.explorer if args.explorer is not None else config.get('explorer', 'https://explorer.catasterism.xyz'))
            if args.watch:
                wallet.address = address(args.watch)
            elif wallet.key.exists() or wallet.key.is_symlink():
                wallet.open()
        curses.wrapper(lambda screen: UI(screen, wallet, bool(args.watch), args.demo).run())
    except (WalletError, OSError, ValueError) as exc:
        print(f'constella-wallet: {exc}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if store:
            store.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
