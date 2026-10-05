"""Local Docker only: exercise the cloud manifest in disposable isolated volumes.

Run: python3 -B deploy/testnet-v5/test_cloud.py
The tested images must already exist. Never run this against a production daemon.
"""
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    result = subprocess.run(args, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    # Explicitly select the local socket; never inherit a remote Docker context.
    os.environ.pop("DOCKER_CONTEXT", None)
    os.environ["DOCKER_HOST"] = "unix:///var/run/docker.sock"
    project = "constella-v5-cloud-smoke-" + secrets.token_hex(4)
    with tempfile.TemporaryDirectory(prefix=project) as temporary:
        lab = Path(temporary)
        http_port, p2p_port = port(), port()
        env = lab / "smoke.env"
        env.write_text((ROOT / "deploy/testnet-v5/cloud.env.example").read_text()
                       + f"\nV5_DB_PASSWORD={secrets.token_hex(32)}\n"
                       + f"EXPLORER_HTTP_PORT={http_port}\nP2P_PORT={p2p_port}\n"
                       + "P2P_BIND=127.0.0.1\nNODE_PEERS=127.0.0.1:1\n"
                       + f"NODE_ADVERTISE=127.0.0.1:{p2p_port}\n")
        env.chmod(0o600)
        command = ["docker", "compose", "-p", project, "--env-file", str(env),
                   "-f", str(ROOT / "deploy/testnet-v5/compose.cloud.yml")]

        def compose(*args):
            return run(*command, *args)

        def inspect(service):
            cid = compose("ps", "-a", "-q", service).decode().strip()
            return json.loads(run("docker", "inspect", cid))[0]

        def ready():
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/api/stats", timeout=3) as response:
                        stats = json.load(response)
                    meta = stats["meta"]
                    if meta.get("height") == "28" and meta.get("check") == "ok" and meta.get("peer") == "true":
                        assert meta["chain_id"] == "2094b0868a27b032"
                        assert int(meta["check_count"]) == int(meta["check_total"]) > 0
                        return stats
                except (OSError, ValueError):
                    pass
                time.sleep(1)
            raise AssertionError("fixture ledger/peer did not become ready")

        try:
            compose("config", "--quiet")
            compose("create", "node")
            node = inspect("node")
            # Initialize as the node's UID. docker cp can preserve host ownership;
            # a capability-free root process cannot bypass another UID's mode bits.
            run("docker", "run", "--rm", "--network", "none", "--volumes-from", node["Id"],
                "--mount", f"type=bind,src={ROOT / 'tests/fixtures/sync-fork.testnet-v5'},dst=/fixture,readonly",
                "alpine:3.20", "sh", "-c", "cp /fixture /data/shares.testnet-v5 && chown 0:0 /data/shares.testnet-v5 && chmod 600 /data/shares.testnet-v5")
            compose("up", "-d", "--no-deps", "postgres", "node")
            compose("up", "-d", "explorer")
            stats = ready()
            containers = {name: inspect(name) for name in ("node", "explorer", "postgres")}
            expected = {"node": (384, 500000000), "explorer": (640, 500000000), "postgres": (384, 250000000)}
            for name, item in containers.items():
                config = item["HostConfig"]
                assert config["Memory"] == expected[name][0] * 1024**2
                assert config["NanoCpus"] == expected[name][1]
                assert "ALL" in config["CapDrop"]
                assert config["LogConfig"]["Config"] == {"max-size": "10m", "max-file": "3"}
                assert item["RestartCount"] == 0 and not item["State"]["OOMKilled"]
            assert containers["node"]["HostConfig"]["ReadonlyRootfs"]
            assert containers["explorer"]["HostConfig"]["ReadonlyRootfs"]
            assert not containers["postgres"]["HostConfig"]["PortBindings"]
            binding = containers["explorer"]["HostConfig"]["PortBindings"]["3071/tcp"]
            assert all(p["HostIp"] == "127.0.0.1" for p in binding)
            networks = {name: set(item["NetworkSettings"]["Networks"]) for name, item in containers.items()}
            assert not networks["node"] & networks["postgres"]
            assert networks["explorer"] == networks["node"] | networks["postgres"]
            node = containers["node"]
            logs = compose("logs", "node").decode()
            assert "mode: validation-only" in logs and "threads=0 duty<=0%" in logs
            run("docker", "cp", node["Id"] + ":/data/node.key", str(lab / "identity-before"))
            identity = (lab / "identity-before").read_bytes()
            assert (lab / "identity-before").stat().st_mode & 0o777 == 0o600
            missing = subprocess.run(["docker", "cp", node["Id"] + ":/data/wallet.key", str(lab / "unexpected-wallet")], capture_output=True)
            assert missing.returncode != 0 and not (lab / "unexpected-wallet").exists()
            assert len(list(Path(f"/proc/{node['State']['Pid']}/task").iterdir())) == 1
            # Restarting explorer must not restart or replace the node.
            compose("restart", "explorer")
            ready()
            assert inspect("node")["State"]["StartedAt"] == node["State"]["StartedAt"]
            # A clean node restart must preserve identity and indexed agreement.
            compose("restart", "node")
            run("docker", "cp", node["Id"] + ":/data/node.key", str(lab / "identity-after"))
            assert (lab / "identity-after").read_bytes() == identity
            ready()
            compose("stop", "-t", "60", "explorer", "node", "postgres")
            for name in containers:
                state = inspect(name)["State"]
                assert state["ExitCode"] == 0 and not state["OOMKilled"]
            print(json.dumps({"result": "passed", "scope": "disposable local Docker, height-28 fixture",
                              "checks": ["resource limits", "read-only filesystems", "private database",
                                         "separate networks", "loopback HTTP", "ledger agreement", "no mining threads/wallet",
                                         "persistent identity", "independent explorer restart", "clean shutdown"],
                              "meta": stats["meta"]}, indent=2))
        except BaseException:
            # Logs do not include the Compose environment/DB password.
            print(compose("logs", "--tail", "40").decode(), flush=True)
            raise
        finally:
            # Only this random test project's disposable volumes are removed.
            compose("down", "-v", "--remove-orphans", "--timeout", "60")


if __name__ == "__main__":
    main()
