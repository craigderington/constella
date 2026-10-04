"""Real validation-only nodes: replay, peer sync, account agreement, no miners/keys."""
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "constella"
FIXTURE = ROOT / "tests/fixtures/sync-fork.v3"


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    with tempfile.TemporaryDirectory(prefix="constella-validator-") as temporary:
        lab = Path(temporary)
        env = os.environ.copy()
        for key in ("CONSTELLA_ADDR", "CONSTELLA_ADVERTISE", "CONSTELLA_PRIVATE_NET"):
            env.pop(key, None)
        env.update(CONSTELLA_MINE="0", CONSTELLA_DUTY="100", CONSTELLA_THREADS="256",
                   CONSTELLA_TEMP_FILE=str(lab / "absent-sensor"),
                   CONSTELLA_KEY=str(lab / "do-not-read-spending-key"))
        sentinel = b"not a spending key; validator must never read this\n"
        (lab / "do-not-read-spending-key").write_bytes(sentinel)
        ports = [port(), port()]
        # A disposable TCP relay starts disconnected, then heals. It never
        # forwards a byte to any host other than the local fixture validator.
        relay = socket.socket(); relay.bind(("127.0.0.1", 0)); relay.listen(); relay.settimeout(.2)
        relay_port = relay.getsockname()[1]
        healed, stopping = threading.Event(), threading.Event()
        def bridge(client):
            upstream = None
            try:
                if not healed.is_set(): return
                upstream = socket.create_connection(("127.0.0.1", ports[0]), timeout=2)
                client.settimeout(2)
                while not stopping.is_set():
                    ready, _, _ = select.select([client, upstream], [], [], .2)
                    for source in ready:
                        data = source.recv(65536)
                        if not data: return
                        (upstream if source is client else client).sendall(data)
            except OSError:
                pass
            finally:
                client.close()
                if upstream: upstream.close()
        def serve():
            while not stopping.is_set():
                try: client, _ = relay.accept()
                except TimeoutError: continue
                except OSError: break
                threading.Thread(target=bridge, args=(client,), daemon=True).start()
        relay_thread = threading.Thread(target=serve, daemon=True); relay_thread.start()
        nodes, logs = [], []
        configurations = []
        fixture = FIXTURE.read_bytes()
        address = fixture[2 + 48:2 + 80].hex()
        try:
            for index in range(2):
                data = lab / str(index); data.mkdir()
                if index == 0:
                    (data / "shares.v3").write_bytes(fixture)
                settings = env | {"CONSTELLA_DATA": str(data), "CONSTELLA_PORT": str(ports[index]),
                                  "CONSTELLA_PEERS": f"127.0.0.1:{relay_port}" if index else "127.0.0.1:1"}
                configurations.append(settings)
                log = (lab / f"node-{index}.log").open("wb"); logs.append(log)
                nodes.append(subprocess.Popen([BINARY], cwd=ROOT, env=settings, stdout=log, stderr=log))
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                disconnected = subprocess.run([BINARY, "balance", f"127.0.0.1:{ports[1]}", address], capture_output=True, timeout=4)
                if disconnected.returncode == 0: break
                time.sleep(.1)
            assert disconnected.returncode == 0 and b"height 0)" in disconnected.stdout
            healed.set()
            responses = []
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                assert all(n.poll() is None for n in nodes), "validator exited"
                responses = [subprocess.run([BINARY, "balance", f"127.0.0.1:{p}", address],
                                            capture_output=True, timeout=4) for p in ports]
                if all(r.returncode == 0 and b"height 28)" in r.stdout for r in responses):
                    break
                time.sleep(.1)
            assert len(responses) == 2 and all(b"height 28)" in r.stdout for r in responses)
            assert responses[0].stdout == responses[1].stdout, "replayed/synced ledgers differ"
            for index, node in enumerate(nodes):
                assert not (lab / str(index) / "wallet.key").exists()
                assert (lab / str(index) / "node.key").stat().st_mode & 0o777 == 0o600
                # Linux/Docker: neither thermal sampler nor either mining lane exists.
                tasks = Path(f"/proc/{node.pid}/task")
                if tasks.exists():
                    assert len(list(tasks.iterdir())) == 1, "unexpected worker thread"
            assert (lab / "do-not-read-spending-key").read_bytes() == sentinel
            reference = responses[0].stdout
            # Abrupt process death releases the writer lock. Its durable history
            # and a cold backup both have to reproduce the same account state.
            nodes[1].kill(); nodes[1].wait(timeout=5)
            assert nodes[1].returncode == -9
            shutil.copytree(lab / "1", lab / "backup")
            retained = (lab / "1" / "shares.v3").read_bytes()
            identity = (lab / "1" / "node.key").read_bytes()
            nodes[1] = subprocess.Popen([BINARY], cwd=ROOT, env=configurations[1], stdout=logs[1], stderr=logs[1])
            shutil.copytree(lab / "backup", lab / "2")
            # Incomplete final record models a torn tail; acknowledged records
            # remain intact when the restored validator repairs that suffix.
            with (lab / "2" / "shares.v3").open("ab") as tail: tail.write(b"\xff")
            ports.append(port())
            restored = env | {"CONSTELLA_DATA":str(lab / "2"), "CONSTELLA_PORT":str(ports[2]), "CONSTELLA_PEERS":"127.0.0.1:1"}
            log = (lab / "node-2.log").open("wb"); logs.append(log)
            nodes.append(subprocess.Popen([BINARY], cwd=ROOT, env=restored, stdout=log, stderr=log))
            for index in (1,2):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    result = subprocess.run([BINARY,"balance",f"127.0.0.1:{ports[index]}",address], capture_output=True, timeout=4)
                    if result.returncode == 0 and result.stdout == reference: break
                    assert nodes[index].poll() is None, "restart/restore exited"
                    time.sleep(.1)
                assert result.returncode == 0 and result.stdout == reference
                assert (lab / str(index) / "shares.v3").read_bytes() == retained
                assert (lab / str(index) / "node.key").read_bytes() == identity
                assert not (lab / str(index) / "wallet.key").exists()
            for name in ("CONSTELLA_PORT", "CONSTELLA_THREADS", "CONSTELLA_DUTY", "CONSTELLA_TEMP_MAX", "CONSTELLA_BATTERY_PAUSE"):
                for value in ("", "12junk", "-1", "999999999999999999999999999999999", " 2"):
                    result = subprocess.run([BINARY], env=env | {name: value, "CONSTELLA_DATA": str(lab / "bad-number")}, capture_output=True, timeout=4)
                    assert result.returncode == 1 and b"invalid numeric configuration" in result.stderr
                    assert not (lab / "bad-number").exists()
            for value in ("64junk", "9999999999999999999999999999999", "-1"):
                result = subprocess.run([BINARY, "bench", value], capture_output=True, timeout=4)
                assert result.returncode == 2
            for value in ("", "false", "2", "-1", "1x"):
                result = subprocess.run([BINARY], env=env | {"CONSTELLA_MINE": value,
                                        "CONSTELLA_DATA": str(lab / "bad-mode")}, capture_output=True, timeout=4)
                assert result.returncode == 1 and b"CONSTELLA_MINE must be 0 or 1" in result.stderr
                assert not (lab / "bad-mode").exists()
        finally:
            stopping.set(); relay.close(); relay_thread.join(timeout=2)
            for node in nodes:
                if node.poll() is None: node.terminate()
            for node in nodes:
                try: node.wait(timeout=5)
                except subprocess.TimeoutExpired: node.kill(); node.wait(); raise
            for log in logs: log.close()
        assert all(node.returncode == 0 for node in nodes)
        for index in range(3):
            log = (lab / f"node-{index}.log").read_text()
            assert "mode: validation-only" in log and "threads=0 duty<=0%" in log
            assert "throttle:" not in log and "mined " not in log
    print("validation-only: partition/heal, SIGKILL/restart and backup/torn-tail restore reproduce balances; no spending key or workers; strict parsing and clean shutdown passed")


if __name__ == "__main__":
    main()
