"""Real validation-only nodes: replay, peer sync, account agreement, no miners/keys."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
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
        nodes, logs = [], []
        fixture = FIXTURE.read_bytes()
        address = fixture[2 + 48:2 + 80].hex()
        try:
            for index in range(2):
                data = lab / str(index); data.mkdir()
                if index == 0:
                    (data / "shares.v3").write_bytes(fixture)
                settings = env | {"CONSTELLA_DATA": str(data), "CONSTELLA_PORT": str(ports[index]),
                                  "CONSTELLA_PEERS": f"127.0.0.1:{ports[0]}" if index else "127.0.0.1:1"}
                log = (lab / f"node-{index}.log").open("wb"); logs.append(log)
                nodes.append(subprocess.Popen([BINARY], cwd=ROOT, env=settings, stdout=log, stderr=log))
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
            for value in ("", "false", "2", "-1", "1x"):
                result = subprocess.run([BINARY], env=env | {"CONSTELLA_MINE": value,
                                        "CONSTELLA_DATA": str(lab / "bad-mode")}, capture_output=True, timeout=4)
                assert result.returncode == 1 and b"CONSTELLA_MINE must be 0 or 1" in result.stderr
                assert not (lab / "bad-mode").exists()
        finally:
            for node in nodes:
                if node.poll() is None: node.terminate()
            for node in nodes:
                try: node.wait(timeout=5)
                except subprocess.TimeoutExpired: node.kill(); node.wait(); raise
            for log in logs: log.close()
        assert all(node.returncode == 0 for node in nodes)
        for index in range(2):
            log = (lab / f"node-{index}.log").read_text()
            assert "mode: validation-only" in log and "threads=0 duty<=0%" in log
            assert "throttle:" not in log and "mined " not in log
    print("validation-only: replay/sync and balances agree; no spending key or worker threads; strict mode parsing and clean shutdown passed")


if __name__ == "__main__":
    main()
