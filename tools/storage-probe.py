#!/usr/bin/env python3
"""Measure optional static C storage-library footprints in a disposable builder.

This does not implement a node storage backend, change the 192 KiB gate, install
host packages, inspect live data or qualify these library versions for release.
"""
import argparse
import hashlib
import io
import json
import re
from pathlib import Path
import subprocess
import tarfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
DOCKER = ['docker', '--host', 'unix:///var/run/docker.sock']
LMDB_SOURCE = 'https://git.openldap.org/openldap/openldap/-/archive/LMDB_0.9.33/openldap-LMDB_0.9.33.tar.gz'
LMDB_SHA512 = '57404b35adb5136fcdf60552c2dd2626b9753868f2707d3279725e08145cee3be0d311189b2c6ef6879f25cf09962e6b423c70c8a2e09ef1b368948e873d92b5'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    image = json.loads((ROOT/'release/build-inputs.json').read_text())['images']['alpine_c_builder']['ref']
    name = 'constella-storage-probe-'+uuid.uuid4().hex[:10]
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        public = subprocess.check_output(['git', 'ls-files', '--', 'Makefile', 'src'], cwd=ROOT, text=True).splitlines()
        for file in public+['tests/storage_probe.c']:
            tar.add(ROOT/file, arcname='src/'+file)
    command = '''set -eu
apk add --no-cache build-base python3 sqlite-dev sqlite-static lmdb-dev
python3 - <<'PYLMDB'
import hashlib,io,pathlib,tarfile,urllib.request
expected="57404b35adb5136fcdf60552c2dd2626b9753868f2707d3279725e08145cee3be0d311189b2c6ef6879f25cf09962e6b423c70c8a2e09ef1b368948e873d92b5"
urls=("https://git.openldap.org/openldap/openldap/-/archive/LMDB_0.9.33/openldap-LMDB_0.9.33.tar.gz",
      "https://distfiles.alpinelinux.org/distfiles/v3.22/openldap-LMDB_0.9.33.tar.gz")
for url in urls:
    try:
        with urllib.request.urlopen(url,timeout=45) as response:data=response.read(64*1024*1024+1)
        if len(data)>64*1024*1024 or hashlib.sha512(data).hexdigest()!=expected:raise ValueError("LMDB source checksum mismatch")
        break
    except Exception:
        if url==urls[-1]:raise
root=pathlib.Path('/lmdb-source');root.mkdir()
wanted={'mdb.c','midl.c','lmdb.h','midl.h','LICENSE'};found=set()
with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as archive:
    for member in archive:
        name=pathlib.PurePosixPath(member.name).name
        if '/libraries/liblmdb/' not in member.name or name not in wanted:continue
        if not member.isfile() or member.size>2*1024*1024 or name in found:raise ValueError("Unexpected LMDB source member")
        (root/name).write_bytes(archive.extractfile(member).read());found.add(name)
if not {'mdb.c','midl.c','lmdb.h','midl.h'}<=found:raise ValueError("Incomplete LMDB source")
print('LMDB source SHA512 verified')
PYLMDB
cd /lmdb-source
cc -Os -flto -ffunction-sections -fdata-sections -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -pthread -c mdb.c midl.c
ar rcs /usr/lib/liblmdb.a mdb.o midl.o
cd /src
mkdir /out
apk list --installed musl gcc sqlite-static lmdb-dev > /out/packages.txt
make -B CC=cc constella-testnet-v5
cp constella-testnet-v5 /out/node-baseline
for candidate in SQLITE LMDB; do
  case "$candidate" in SQLITE) storage_libs='-lsqlite3 -lm';; LMDB) storage_libs='-llmdb -lm';; esac
  make -B CC=cc constella-testnet-v5 \
    APP='src/node.c src/cli.c src/main.c tests/storage_probe.c' \
    OPT="-Os -DSTORAGE_IN_NODE -DSTORAGE_$candidate" \
    LDFLAGS="-static -flto -pthread -Wl,--gc-sections,-z,relro,-z,now,-z,noexecstack -s $storage_libs"
  CONSTELLA_STORAGE_PROBE=1 ./constella-testnet-v5
  ./constella-testnet-v5 --version
  cp constella-testnet-v5 "/out/node-$candidate"
done
'''
    created = False
    try:
        subprocess.run(DOCKER+['create', '--name', name, '--cpus=1', '--memory=512m',
                              '--memory-swap=512m', '--pids-limit=256', image, 'sh', '-ec', command], check=True, stdout=subprocess.DEVNULL)
        created = True
        subprocess.run(DOCKER+['cp', '-', name+':/'], input=archive.getvalue(), check=True)
        with (args.out/'build.log').open('w') as log:
            run = subprocess.run(DOCKER+['start', '-a', name], stdout=log, stderr=subprocess.STDOUT, timeout=1200)
        state = json.loads(subprocess.check_output(DOCKER+['inspect', '--format', '{{json .State}}', name]))
        (args.out/'container-state.json').write_text(json.dumps(state, indent=2)+'\n')
        subprocess.run(DOCKER+['cp', name+':/out/.', str(args.out)], check=False, capture_output=True)
        if run.returncode or state['ExitCode'] or state['OOMKilled']:
            raise RuntimeError('Storage footprint build failed; inspect '+str(args.out/'build.log'))
        subprocess.run(DOCKER+['cp', name+':/out/.', str(args.out)], check=True)
        records = {}
        for file in ('node-baseline', 'node-SQLITE', 'node-LMDB'):
            path = args.out/file
            data = path.read_bytes()
            elf = subprocess.check_output(['readelf', '-lW', str(path)], text=True)
            if data[:4] != b'\x7fELF' or 'INTERP' in elf:
                raise ValueError('Probe is not a static ELF: '+file)
            records[file] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'static': True,
                             'elf_machine': int.from_bytes(data[18:20], 'little')}
        report = {'builder': image, 'architecture': 'native builder; see ELF/build log',
                  'source_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  'probe_source_sha256': hashlib.sha256((ROOT/'tests/storage_probe.c').read_bytes()).hexdigest(),
                  'artifacts': records, 'limit_bytes': 196608,
                  'lmdb_source': {'url': LMDB_SOURCE, 'sha512': LMDB_SHA512,
                                  'checksum_provenance': 'Alpine aports 3.22-stable/main/lmdb/APKBUILD'},
                  'installed_during_build': re.findall(r'Installing (\S+) \(([^)]+)\)', (args.out/'build.log').read_text()),
                  'scope': 'Optional API linkage/reopen smoke and lower-bound static footprint; no backend integration, adversarial/capacity/native-ARM qualification or deployment.'}
        (args.out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report['artifacts'], indent=2))
    finally:
        if created:
            subprocess.run(DOCKER+['rm', '-f', name], check=True, stdout=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
