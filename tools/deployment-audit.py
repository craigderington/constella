#!/usr/bin/env python3
"""Read-only, sanitized deployment evidence. The host's operator runs this.

Never reads keys or env files, prints complete Docker configuration, starts
containers, changes services, or connects to another host.
"""
import argparse
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile

SAFE_ENV = {
    "CONSTELLA_MINE", "CONSTELLA_THREADS", "CONSTELLA_DUTY",
    "CONSTELLA_TEMP_MAX", "CONSTELLA_PORT", "CONSTELLA_PRIVATE_NET",
    "EXPLORER_HTTP", "EXPLORER_UI_DIR", "GOMEMLIMIT", "GOGC",
}
DOCKER = ["docker", "--host", "unix:///var/run/docker.sock"]
UI_FILES = ["templates/" + name + ".html" for name in
            ("layout", "overview", "share", "address", "records", "missing")]
UI_FILES += ["static/explorer.css", "static/explorer.js"]


def run(args, **kwargs):
    if args[0] == "docker":
        args = DOCKER + args[1:]
    return subprocess.check_output(args, timeout=60, **kwargs)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def executable_hash(name, executable):
    # Read only one fixed executable. Never extract an archive to disk.
    raw = run(["docker", "cp", name + ":" + executable, "-"])
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        regular = [m for m in archive.getmembers() if m.isfile()]
        if len(regular) != 1 or regular[0].size > 128 * 1024 * 1024:
            raise ValueError("unexpected executable archive")
        member = regular[0]
        with archive.extractfile(member) as stream:
            h = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                h.update(chunk)
            digest = h.hexdigest()
        return {"path": executable, "bytes": member.size, "sha256": digest}


def inspect(name):
    c = json.loads(run(["docker", "inspect", name]))[0]
    labels = c["Config"].get("Labels") or {}
    service = labels.get("com.docker.compose.service", "")
    env = dict(v.split("=", 1) for v in c["Config"].get("Env", []))
    state, host = c["State"], c["HostConfig"]
    result = {
        "name": c["Name"], "id": c["Id"], "image_id": c["Image"],
        "project": labels.get("com.docker.compose.project"), "service": service,
        "provenance": {k: v for k, v in labels.items() if k in {
            "org.opencontainers.image.source", "org.opencontainers.image.revision",
            "org.opencontainers.image.version"}},
        "state": {k: state.get(k) for k in
                  ("Status", "Running", "OOMKilled", "StartedAt", "ExitCode")},
        "restart_count": c["RestartCount"],
        "resources": {k: host.get(k) for k in
                      ("NanoCpus", "Memory", "MemorySwap", "PidsLimit", "ReadonlyRootfs")},
        "environment": {k: env[k] for k in sorted(SAFE_ENV) if k in env},
        "mounts": [{k: m.get(k) for k in ("Type", "Name", "Destination", "RW")}
                   for m in c.get("Mounts", [])],
        "published_ports": c["NetworkSettings"].get("Ports"),
    }
    image = json.loads(run(["docker", "image", "inspect", c["Image"]]))[0]
    result["architecture"] = image.get("Architecture")
    result["image_provenance"] = {k: v for k, v in
        (image["Config"].get("Labels") or {}).items()
        if k.startswith("org.opencontainers.image.")}
    executable = "/explorer" if service == "explorer" else "/constella" if service == "node" else None
    if executable:
        try:
            result["executable"] = executable_hash(name, executable)
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            result["executable_error"] = type(exc).__name__
    # External presentation files can differ even when the image is unchanged.
    for mount in c.get("Mounts", []):
        target = mount.get("Destination")
        if target not in ("/ui", "/ui/current") or mount.get("Type") != "bind":
            continue
        source = Path(mount["Source"])
        ui = source / "current" if target == "/ui" else source
        result["external_ui"] = {
            "files": {p: sha(ui / p) for p in UI_FILES if (ui / p).is_file()},
            "complete": all((ui / p).is_file() for p in UI_FILES),
        }
    return result


def main():
    global DOCKER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--checkout", type=Path)
    parser.add_argument("--containers", nargs="+")
    parser.add_argument("--docker", default="docker", help="local Docker CLI executable")
    parser.add_argument("--docker-context", help="explicit local VM context, e.g. Colima")
    args = parser.parse_args()
    DOCKER = [args.docker, "--context", args.docker_context] if args.docker_context else [args.docker, "--host", "unix:///var/run/docker.sock"]
    os.umask(0o077)
    names = args.containers or run([
        "docker", "ps", "-a", "--filter", "label=com.docker.compose.service",
        "--format", "{{.Names}}"], text=True).splitlines()
    names = [n for n in names if "constella" in n]
    report = {"recorded_at": dt.datetime.now(dt.timezone.utc).isoformat(),
              "scope": "operator-collected deployment metadata; no secrets or service changes",
              "containers": [], "errors": []}
    for name in names:
        try:
            report["containers"].append(inspect(name))
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            report["errors"].append({"container": name, "error_type": type(exc).__name__})
    if args.checkout:
        report["checkout"] = {}
        for key, command in (("revision", ["rev-parse", "HEAD"]),
                             ("tracked_changes", ["status", "--porcelain", "--untracked-files=no"])):
            try:
                report["checkout"][key] = run(
                    ["git", "-C", str(args.checkout), *command], text=True).strip()
            except subprocess.SubprocessError:
                report["checkout"][key] = "unavailable"
        try:
            tracked = run(["git", "-C", str(args.checkout), "ls-files"], text=True).splitlines()
            report["checkout"]["source_sha256"] = {
                name: sha(args.checkout / name) for name in tracked
                if (name.startswith(("src/", "explorer/internal/", "explorer/cmd/"))
                    or name in ("Makefile", "Dockerfile", "explorer/Dockerfile", "explorer/go.mod", "explorer/go.sum"))
                and (args.checkout / name).is_file()
            }
        except (OSError, subprocess.SubprocessError):
            report["checkout"]["source_sha256"] = "unavailable"
    with args.out.open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print("Saved sanitized deployment evidence to", args.out)
    if report["errors"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
