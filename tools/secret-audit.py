#!/usr/bin/env python3
"""Run the pinned, checksum-verified Linux AMD64 Gitleaks tool; redact findings."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile
import urllib.request
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    if platform.system()!='Linux' or platform.machine()!='x86_64':
        raise SystemExit('This pinned scanner archive is Linux AMD64; use an isolated matching runner')
    config=json.loads((ROOT/'release/security-tools.json').read_text())
    with urllib.request.urlopen(config['archive_url'],timeout=60) as response:raw=response.read()
    if hashlib.sha256(raw).hexdigest()!=config['archive_sha256']:
        raise ValueError('Scanner archive checksum mismatch')
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='constella-secret-scan-') as temp:
        binary=Path(temp)/'gitleaks'
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            member=archive.getmember('gitleaks')
            if not member.isfile() or member.size>128*1024*1024:raise ValueError('Unexpected scanner archive')
            binary.write_bytes(archive.extractfile(member).read())
        binary.chmod(0o700)
        result=subprocess.run([str(binary),'git','--redact','--no-banner','--log-opts=--all --full-history',
                               '--report-format','json','--report-path',str(args.out.resolve()),str(ROOT)],check=False)
        raise SystemExit(result.returncode)
if __name__=='__main__':main()
