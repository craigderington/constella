"""Short mixed-flood acceptance check; called only with disposable lab nodes."""
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time


def exercise(probe, node, port, fixture):
    address = f"127.0.0.1:{port}"
    def query(command, *extra):
        result = subprocess.run([probe, command, address, *extra], capture_output=True, timeout=4)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        return result.stdout
    def cpu():
        fields = Path(f"/proc/{node.pid}/stat").read_text().split()
        return (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK")
    baseline = query("client")
    before = cpu(); started = time.monotonic()
    attackers = [subprocess.Popen([probe, "flood", address, mode], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE) for mode in ("tx", "getchain", "getchain")]
    stopping = threading.Event()
    churn_count = [0]
    def churn():
        while not stopping.is_set():
            try:
                with socket.create_connection(("127.0.0.1", port), .1):
                    churn_count[0] += 1
            except OSError:
                pass
            stopping.wait(.01)
    thread = threading.Thread(target=churn); thread.start()
    latencies = []
    try:
        while time.monotonic() - started < 4:
            began = time.monotonic()
            assert query("client") == baseline, "account state changed during read-only abuse"
            query("sync-fixture", fixture)
            latencies.append(time.monotonic() - began)
            time.sleep(.03)
        elapsed = time.monotonic() - started
        used = cpu() - before
        assert latencies and max(latencies) < 3, latencies
        assert used / elapsed < .8, f"node CPU service duty {used / elapsed:.3f}"
        traffic = []
        for process in attackers:
            stdout, stderr = process.communicate(timeout=12)
            assert process.returncode == 0, stderr.decode(errors="replace")
            traffic.append(json.loads(stdout))
        assert query("client") == baseline
        query("sync-fixture", fixture)
        return {"healthy_query_and_sync_cycles": len(latencies), "max_cycle_seconds": round(max(latencies), 4),
                "node_cpu_fraction_one_core": round(used / elapsed, 4), "measurement_seconds": round(elapsed, 4),
                "connection_churn": churn_count[0], "attackers": traffic}
    finally:
        stopping.set(); thread.join(2)
        assert not thread.is_alive()
        for process in attackers:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=2)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
