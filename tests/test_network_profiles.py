"""Isolated five-profile lab. All nodes, ports, keys and files are disposable.

No external peers, databases, Docker stacks or production services are used.
Run from the repo root. --write-fixtures regenerates public test vectors/proofs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import threading
import time

from peer_flood import exercise

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures"
PROFILES = [
    ("network_legacy.h", "", 3, 5, 0x33545343, 0, "shares.v3"),
    ("network_testnet_v4.h", "protocolv4", 4, 5, 0x54345443, 5, "shares.testnet-v4"),
    ("network_mainnet_v4.h", "mainnet", 4, 6, 0x4D345443, 6, "shares.mainnet-v4"),
    ("network_testnet_v5.h", "protocolv5", 5, 5, 0x54355443, 5, "shares.testnet-v5"),
    ("network_mainnet_v5.h", "protocolv5,mainnet", 5, 6, 0x4D355443, 6, "shares.mainnet-v5"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=180, **kw)


def checked(cmd, **kw):
    result = run(cmd, **kw)
    if result.returncode:
        raise AssertionError(f"{cmd} failed:\n{result.stdout.decode(errors='replace')}\n{result.stderr.decode(errors='replace')}")
    return result.stdout


def verify_info(info, profile):
    _, _, version, block, magic, marker, filename = profile
    assert (info["version"], info["block_k"], info["magic"], info["marker"], info["file"]) == (
        version, block, magic, marker, filename)
    genesis = struct.pack("<II32sQ32sHH32sQ", version, 0, bytes(32), 1790121600,
                          bytes(32), 384, marker, bytes(32), 0)
    cid = hashlib.blake2b(struct.pack("<IIIQ", version, block, 384, 1790121600), digest_size=32).digest()[:8]
    assert info["genesis"] == genesis.hex()
    assert info["genesis_id"] == hashlib.blake2b(genesis, digest_size=32).hexdigest()
    assert info["chain_id"] == cid.hex()
    ap = bytes.fromhex("07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c")
    bp = bytes.fromhex("3ebcb692149344dc54e58160cf90bed9eea1dd14e81c8e91de557af7d7afd915")
    shared = bytes.fromhex("cef531834c2843a22541cc4a0f40492e7b0c34baea021fbf7d1caab2f35a4263")
    ia, ib = bytes([0xaa])*32, bytes([0x55])*32
    domain = struct.pack("<I", magic) + cid
    transcript = b"CSTL-HS1" + ap + bp if version == 3 else b"CSTL-HS2" + domain + ap + bp + ia + ib
    assert info["transcript"] == transcript.hex()
    for direction in ("lo", "hi"):
        msg = (b"CSTL-P2P2" if version == 3 else b"CSTL-P2P3") + direction.encode() + ib + ia
        if version >= 4:
            msg += domain
        assert info[f"key_{direction}"] == hashlib.blake2b(msg, key=shared, digest_size=32).hexdigest()


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        part = sock.recv(n-len(data))
        if not part:
            return None
        data.extend(part)
    return data


def translating_proxy(target, client_magic, server_magic):
    """Rewrite only frame magic. Authentication must still bind the network."""
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0)); listener.listen(); listener.settimeout(10)
    port = listener.getsockname()[1]

    def relay(source, destination, magic):
        try:
            while True:
                header = recv_exact(source, 7)
                if header is None:
                    break
                payload = recv_exact(source, int.from_bytes(header[5:7], "little"))
                if payload is None:
                    break
                destination.sendall(struct.pack("<I", magic) + header[4:] + payload)
        except OSError:
            pass
        finally:
            try:
                destination.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def serve():
        try:
            with listener, listener.accept()[0] as client, socket.create_connection(("127.0.0.1", target), 3) as server:
                client.settimeout(5); server.settimeout(5)
                worker = threading.Thread(target=relay, args=(client, server, server_magic))
                worker.start(); relay(server, client, client_magic); worker.join(6)
        except OSError:
            listener.close()

    thread = threading.Thread(target=serve)
    thread.start()
    return port, thread


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-fixtures", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    evidence = {"scope": "disposable localhost only", "profiles": {}, "checks": {}}
    with tempfile.TemporaryDirectory(prefix="constella-network-") as directory:
        lab = Path(directory)
        core = (ROOT / "Makefile").read_text().split("CORE := ", 1)[1].split("\nAPP ", 1)[0].replace("\\\n", " ").split()
        core.remove("src/chain.c")  # included by the probe for the clock model
        probes, infos, shares, transactions = [], {}, [], []
        for index, profile in enumerate(PROFILES):
            print(f"building and checking {profile[0]}", flush=True)
            probe = lab / f"probe-{index}"; probes.append(probe)
            flags = shlex.split(os.environ.get("CONSTELLA_TEST_CFLAGS", "-O2"))
            checked([os.environ.get("CC", "cc"), "-std=c11", "-D_GNU_SOURCE", "-pthread", *flags,
                     f"-DCONSTELLA_NETWORK={index}", "-Isrc", "-o", str(probe), *core,
                     "src/node.c", "tests/network_probe.c", "-lm"])
            checked([probe, "key-policy"])
            info = json.loads(checked([probe, "info"])); verify_info(info, profile)
            infos[profile[0]] = info
            evidence["profiles"][profile[0]] = {k: info[k] for k in ("chain_id", "genesis_id", "magic", "file")}
            share = lab / f"share-{index}"; share.write_bytes(checked([probe, "mine"])); shares.append(share)
            tx = lab / f"tx-{index}"; tx.write_bytes(checked([probe, "tx"])); transactions.append(tx)
            if args.write_fixtures and index:
                (FIXTURES / PROFILES[index][6].replace("shares.", "sync-fork.")).write_bytes(
                    checked([probe, "fork-fixture"]))
        assert len({x["genesis_id"] for x in infos.values()}) == len(PROFILES)
        if args.write_fixtures:
            (FIXTURES / "network-profiles.json").write_text(json.dumps(infos, indent=2) + "\n")
        else:
            assert infos == json.loads((FIXTURES / "network-profiles.json").read_text()), "network vectors changed"
        evidence["clock_model"] = json.loads(checked([probes[0], "clock"]))
        print("profiles, independent Python vectors and 32-phase clock model: passed", flush=True)
        for target, probe in enumerate(probes):
            for source in range(len(PROFILES)):
                data = lab / f"submit-{target}-{source}"; data.mkdir()
                result = run([probe, "submit", data, shares[source]])
                observed = json.loads(result.stdout)
                assert observed == {"result": 0 if source == target else 4,
                                    "entries": 2 if source == target else 1, "orphans": 0}, observed
                assert result.returncode == (0 if source == target else 1)
                tx_result = run([probe, "verifytx", transactions[source]])
                assert tx_result.returncode == (0 if source == target else 1)
                raw = shares[source].read_bytes(); record = struct.pack("<H", len(raw)) + raw
                for named_source in (False, True):
                    replay = lab / f"replay-{target}-{source}-{named_source}"; replay.mkdir()
                    filename = PROFILES[source if named_source else target][6]
                    path = replay / filename; path.write_bytes(record)
                    result = run([probe, "replay", replay])
                    assert result.returncode == (0 if source == target else 1), result.stderr.decode()
                    assert path.read_bytes() == record, "foreign history was modified"
        evidence["checks"]["shares_transactions_replay"] = "all ordered profile pairs passed"
        print("shares, transaction signatures, wrong-volume and renamed-history rejection: passed", flush=True)
        nodes, logs, ports = [], [], []
        try:
            for index, probe in enumerate(probes):
                port = available_port(); ports.append(port)
                data = lab / f"node-{index}"; data.mkdir()
                fixture = FIXTURES / PROFILES[index][6].replace("shares.", "sync-fork.")
                (data / PROFILES[index][6]).write_bytes(fixture.read_bytes())
                env = os.environ.copy()
                for name in ("CONSTELLA_ADVERTISE", "CONSTELLA_KEY", "CONSTELLA_PRIVATE_NET"):
                    env.pop(name, None)
                env.update(CONSTELLA_DATA=str(data), CONSTELLA_PORT=str(port), CONSTELLA_THREADS="1",
                           CONSTELLA_PEERS="127.0.0.1:1", CONSTELLA_ADDR="01"*32,
                           CONSTELLA_TEMP_FILE=str(lab / "absent-sensor"), CONSTELLA_DUTY="1")
                log = open(lab / f"node-{index}.log", "wb"); logs.append(log)
                node = subprocess.Popen([probe, "node"], cwd=ROOT, env=env, stdout=log, stderr=log); nodes.append(node)
                for _ in range(100):
                    if node.poll() is not None:
                        raise AssertionError((lab / f"node-{index}.log").read_text())
                    if run([probe, "client", f"127.0.0.1:{port}"]).returncode == 0:
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("node did not become ready")
            for target, port in enumerate(ports):
                for source, probe in enumerate(probes):
                    direct = run([probe, "client", f"127.0.0.1:{port}"])
                    assert direct.returncode == (0 if source == target else 1)
                    translated, thread = translating_proxy(port, PROFILES[source][4], PROFILES[target][4])
                    try:
                        result = run([probe, "client", f"127.0.0.1:{translated}"])
                        assert result.returncode == (0 if source == target else 1)
                    finally:
                        thread.join(12)
                        assert not thread.is_alive(), "proxy did not shut down"
            evidence["checks"]["wire"] = "all ordered pairs direct and magic-rewriting proxy passed"
            print("live C handshakes, encrypted account queries and magic-rewriting proxy: passed", flush=True)
            evidence["peer_flood"] = {}
            for index, profile in enumerate(PROFILES):
                fixture = FIXTURES / PROFILES[index][6].replace("shares.", "sync-fork.")
                evidence["peer_flood"][profile[0]] = exercise(probes[index], nodes[index], ports[index], fixture)
                print(f"mixed flood with healthy account queries and full fixture sync: {profile[0]} passed", flush=True)
            for index, profile in enumerate(PROFILES):
                env = os.environ.copy(); env["CONSTELLA_TEST_PEER"] = f"127.0.0.1:{ports[index]}"
                env.pop("EXPLORER_TEST_DB", None)  # this lab never uses a database
                cmd = ["go", "test", "-race", "-count=1", "-mod=vendor"]
                if profile[1]: cmd += ["-tags", profile[1]]
                result = subprocess.run(cmd + ["./..."], cwd=ROOT / "explorer", env=env,
                                        capture_output=True, timeout=240)
                assert result.returncode == 0, result.stdout.decode() + result.stderr.decode()
                print(f"Go race suite and real C/Go interop: {profile[0]} passed", flush=True)
            evidence["checks"]["go"] = "full race suite and C/Go interoperability passed for all profiles"
        finally:
            for node in nodes:
                if node.poll() is None: node.terminate()
            for node in nodes:
                try: node.wait(timeout=10)
                except subprocess.TimeoutExpired: node.kill(); node.wait(); raise
            for log in logs: log.close()
        assert all(node.returncode == 0 for node in nodes), "node shutdown failed"
        evidence["checks"]["shutdown"] = "all disposable nodes exited cleanly"
    if args.output: args.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print("protocol lab: passed; disposable processes and data removed", flush=True)


if __name__ == "__main__":
    main()
