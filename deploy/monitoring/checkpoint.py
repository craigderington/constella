"""Read-only full-tip/known-account agreement via verified SSH tunnels.

No production SSH; the Explorer contributes its public HTTPS checkpoint only.
This does not enumerate every node's ledger or audit escrow/science totals.
"""
import concurrent.futures
import contextlib
import datetime as dt
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

CHAIN = "2094b0868a27b032"
CLIENT = Path(__file__).with_name("checkpoint-client")
ADDRESSES = (
    "a1db4f540811717bba3bd9925ca7c90da78e2ecffaf958774007776a9e0304ca",
    "97b39cf292c2b7ff3eaba31b4ebd66124a9bab3cd303764f834db3faa0c042fc",
    "fcf40e6ab4d0738bbe5759d0659a718a2e393f85b0195b6ddf85b123abc633d8",
    "c0a7170238f0d85bdd0c8e882b9055edb4dd810d565e05a228c6c9a14c48047e",
    "0116f85b2f0202d0f36e2035d5f3db71dcf78d9ec4acba79b9a198c9219e62c0",
)


def compare(samples, public, expected):
    """Never turn absent, malformed, moving or partial evidence into a pass."""
    if not expected or set(samples) != set(expected):
        return "unverified", "Missing required node samples"
    try:
        for name, node in samples.items():
            if node.get("error"):
                return "unverified", f"{name}: {node['error']}"
            if node["chain_id"] != CHAIN or not re.fullmatch(r"[0-9a-f]{64}", node["tip"]):
                return "mismatch", "Wrong network or invalid full tip"
            if type(node["height"]) is not int or node["height"] < 1:
                raise ValueError("invalid node height")
            if set(node["accounts"]) != set(ADDRESSES):
                return "unverified", "Configured account inventory is incomplete"
            for account in node["accounts"].values():
                if set(account) != {"amount", "nonce", "next"}:
                    raise ValueError("invalid account fields")
                for value in account.values():
                    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+", value) or int(value) >= 2**64:
                        raise ValueError("invalid account integer")
        heights = {n["height"] for n in samples.values()}
        if len(heights) != 1:
            return "unverified", "Nodes sampled at different heights; retry"
        first = next(iter(samples.values()))
        if any(n["tip"] != first["tip"] for n in samples.values()):
            return "mismatch", "Full tips differ at the same height"
        if any(n["accounts"] != first["accounts"] for n in samples.values()):
            return "mismatch", "Account balances or confirmed/pending nonces differ at a common full tip"
        meta = public["meta"]
        if meta["chain_id"] != CHAIN or meta["network"] != "testnet" or meta["check"] != "ok":
            return "mismatch", "Explorer network or ledger check failed"
        if int(meta["check_count"]) != len(ADDRESSES) or int(meta["check_total"]) != len(ADDRESSES):
            return "unverified", "Explorer account inventory changed or verification is partial"
        current = dt.datetime.now(dt.timezone.utc)
        for key in ("check_at", "updated_at"):
            age = (current - dt.datetime.fromisoformat(meta[key].replace("Z", "+00:00"))).total_seconds()
            if not 0 <= age < 120:
                return "unverified", "Explorer checkpoint is stale or future-dated"
        if meta["peer"] != "true":
            return "unverified", "Explorer has no upstream peer"
        if int(meta["height"]) != first["height"]:
            return "unverified", "Explorer and nodes sampled at different heights; retry"
        if meta["tip"] != first["tip"]:
            return "mismatch", "Explorer full tip differs at the same height"
        return "ok", "Full tip and configured account balances/nonces agree across active homelab nodes"
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        return "unverified", "Checkpoint schema could not be verified: " + str(exc)


@contextlib.contextmanager
def tunnel(host, control):
    # A private control socket lets us close exactly this read-only connection.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    address = host.rsplit("@", 1)[-1]
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
           "-o", "ConnectTimeout=10", "-o", "ExitOnForwardFailure=yes",
           "-o", "ControlPersist=no", "-M", "-S", str(control), "-fNT",
           "-L", f"127.0.0.1:{port}:{address}:18473", host]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        yield f"127.0.0.1:{port}"
    finally:
        if control.exists():
            subprocess.run(["ssh", "-S", str(control), "-O", "exit", host],
                           capture_output=True, timeout=5)


def query(endpoint):
    try:
        result = subprocess.run([str(CLIENT), endpoint, *ADDRESSES],
                                capture_output=True, text=True, timeout=12)
        if result.returncode:
            return {"error": result.stderr.strip() or f"Client exit {result.returncode}"}
        return json.loads(result.stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        return {"error": str(exc)}


def collect(hosts, public_reader, attempts=12):
    deadline = time.monotonic() + 120
    record = {"status": "unverified", "scope": "active homelab full-tip and configured-account agreement",
              "accounts_checked": list(ADDRESSES), "expected_nodes": list(hosts), "attempts": [],
              "limitations": ["Not an independent full-history replay or complete ledger-state audit.",
                              "Existing protocol provides bracketed observations, not an atomic ledger snapshot.",
                              "Account inventory is configured, not enumerated from every node.",
                              "Cloud node is represented only by the public Explorer checkpoint."]}
    if not CLIENT.is_file():
        return record | {"message": "Checkpoint client missing; run make checkpoint-client locally"}
    try:
        with tempfile.TemporaryDirectory(prefix="constella-checkpoint-") as temp, contextlib.ExitStack() as stack:
            endpoints = {name: stack.enter_context(tunnel(config[0], Path(temp)/name))
                         for name, config in hosts.items()}
            for attempt in range(attempts):
                if time.monotonic() > deadline-25:
                    record.update(status="unverified", message="Checkpoint time budget exhausted")
                    break
                with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)+1) as pool:
                    jobs = {name: pool.submit(query, endpoint) for name, endpoint in endpoints.items()}
                    public_job = pool.submit(public_reader)
                    samples = {name: job.result() for name, job in jobs.items()}
                    try:
                        public = public_job.result()
                    except (OSError, ValueError, TimeoutError) as exc:
                        public = {"error": str(exc)}
                status, message = compare(samples, public, hosts)
                snapshot = {"at": dt.datetime.now(dt.timezone.utc).isoformat(), "nodes": samples,
                            "public": public, "status": status, "message": message}
                record["attempts"].append(snapshot)
                record.update(status=status, message=message, checkpoint=snapshot)
                if status == "ok":
                    break
                if attempt+1 < attempts:
                    time.sleep(2)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        record.update(status="unverified", message=str(exc))
    return record
