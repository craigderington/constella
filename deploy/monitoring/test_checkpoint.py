import copy
import datetime as dt
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import checkpoint as cp

ROOT = Path(__file__).resolve().parents[2]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def evidence():
    at = dt.datetime.now(dt.timezone.utc).isoformat()
    node = {"chain_id": cp.CHAIN, "height": 42, "tip": "ab"*32,
            "accounts": {a: {"amount": "42", "nonce": "0", "next": "0"} for a in cp.ADDRESSES}}
    nodes = {name: copy.deepcopy(node) for name in ("nixos", "mini", "wolf359", "andromeda")}
    public = {"meta": {"chain_id": cp.CHAIN, "network": "testnet", "check": "ok", "peer": "true",
                       "check_count": "5", "check_total": "5", "check_at": at, "updated_at": at,
                       "height": "42", "tip": "ab"*32}}
    return nodes, public


class Comparison(unittest.TestCase):
    def test_complete_agreement(self):
        nodes, public = evidence()
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "ok")

    def test_missing_node_cannot_pass(self):
        nodes, public = evidence()
        self.assertEqual(cp.compare(nodes, public, [*nodes, "missing"])[0], "unverified")

    def test_tip_suffix_and_account_mismatch_detected(self):
        nodes, public = evidence()
        nodes["mini"]["tip"] = "ab"*31 + "ac"
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "mismatch")
        nodes, public = evidence()
        nodes["mini"]["accounts"][cp.ADDRESSES[0]]["amount"] = "43"
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "mismatch")

    def test_moving_height_not_claimed_as_mismatch_or_pass(self):
        nodes, public = evidence(); nodes["mini"]["height"] += 1
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "unverified")

    def test_stale_partial_and_unknown_inventory_fail_closed(self):
        for change in ({"check_at": "2020-01-01T00:00:00Z"}, {"check_count": "4"}, {"check_total": "6"}):
            nodes, public = evidence(); public["meta"].update(change)
            self.assertEqual(cp.compare(nodes, public, nodes)[0], "unverified")
        nodes, public = evidence(); del nodes["mini"]["accounts"][cp.ADDRESSES[0]]
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "unverified")

    def test_malformed_and_connection_failure(self):
        nodes, public = evidence(); nodes["mini"] = {"error": "unreachable"}
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "unverified")
        nodes, public = evidence(); nodes["mini"]["accounts"][cp.ADDRESSES[0]]["amount"] = -1
        self.assertEqual(cp.compare(nodes, public, nodes)[0], "unverified")


class LiveClient(unittest.TestCase):
    def run_ready(self, args, endpoint, addresses):
        deadline = time.monotonic()+10
        while True:
            result = subprocess.run([str(cp.CLIENT), endpoint, *addresses], capture_output=True, text=True, timeout=12)
            if "connection failed" not in result.stderr or time.monotonic() >= deadline:
                return result
            time.sleep(.05)

    def test_authenticated_faults(self):
        for mode, expected in ((0, 0), (1, 3), (2, 3), (3, 1), (4, 1), (5, 1)):
            with self.subTest(mode=mode):
                port = free_port()
                proc = subprocess.Popen([str(cp.CLIENT.with_name("checkpoint-fixture")), str(port), str(mode)],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    result = self.run_ready([], f"127.0.0.1:{port}", cp.ADDRESSES)
                    self.assertEqual(result.returncode, expected, result.stderr)
                    if expected: self.assertEqual(result.stdout, "")
                finally:
                    proc.terminate(); proc.wait(timeout=5)

    def test_real_replayed_node_accounts_and_full_hash(self):
        with tempfile.TemporaryDirectory(prefix="checkpoint-test-") as temp:
            data = Path(temp); raw = (ROOT/"tests/fixtures/sync-fork.testnet-v5").read_bytes()
            (data/"shares.testnet-v5").write_bytes(raw)
            addr = raw[50:82].hex(); port = free_port()
            env = {k:v for k,v in os.environ.items() if not k.startswith("CONSTELLA_")}
            env.update(CONSTELLA_MINE="0", CONSTELLA_DATA=temp, CONSTELLA_PORT=str(port),
                       CONSTELLA_PEERS="127.0.0.1:1", CONSTELLA_PRIVATE_NET="1")
            with (data/"node.log").open("w") as log:
                proc = subprocess.Popen([str(ROOT/"constella-testnet-v5")], env=env, stdout=log, stderr=log)
                try:
                    result = self.run_ready([], f"127.0.0.1:{port}", [addr])
                    self.assertEqual(result.returncode, 0, result.stderr+(data/"node.log").read_text())
                    got = json.loads(result.stdout)
                    self.assertEqual(got["height"], 28)
                    # Independently calculate header IDs from the public fixture.
                    import hashlib
                    ids = {}; offset = 0
                    while offset < len(raw):
                        size = int.from_bytes(raw[offset:offset+2], "little")
                        header = raw[offset+2:offset+126]
                        ids[hashlib.blake2b(header, digest_size=32).hexdigest()] = int.from_bytes(header[4:8], "little")
                        offset += size+2
                    self.assertEqual(ids[got["tip"]], got["height"])
                    balance = subprocess.check_output([str(ROOT/"constella-testnet-v5"), "balance", f"127.0.0.1:{port}", addr], text=True)
                    from decimal import Decimal
                    self.assertEqual(int(Decimal(balance.split()[0])*100000000), int(got["accounts"][addr]["amount"]))
                    self.assertFalse((data/"wallet.key").exists())
                    self.assertEqual((data/"shares.testnet-v5").read_bytes(), raw)
                finally:
                    proc.terminate(); proc.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
