#!/usr/bin/env python3
"""Package a clean committed native testnet build; never publish or deploy."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
def run(args):
    return subprocess.check_output(args, cwd=ROOT, text=True, timeout=120).strip()
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for part in iter(lambda:f.read(1024*1024), b''):h.update(part)
    return h.hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--wallet',type=Path,required=True)
    args=p.parse_args()
    run(['python3','tools/release-check.py'])
    for command in (['git','diff','--exit-code'],['git','diff','--cached','--exit-code']):run(command)
    revision=run(['git','rev-parse','HEAD']); version=(ROOT/'release/VERSION').read_text().strip()
    arch=platform.machine()
    if platform.system()!='Linux' or arch not in ('x86_64','aarch64'):
        raise SystemExit('Package only qualified native Linux AMD64/ARM64 builds')
    args.out.mkdir(parents=True,exist_ok=False)
    files={'constella-testnet-v5':ROOT/'constella-testnet-v5','constella-explorer':ROOT/'constella-explorer','constella-wallet':args.wallet.resolve()}
    banners={}
    for name,path in files.items():
        banners[name]=run([str(path),'--version'])
        if name!='constella-wallet' and 'chain=2094b0868a27b032' not in banners[name]:
            raise ValueError('Wrong release network: '+name)
        # Enforce static-link qualification for every distributed native binary.
        result=subprocess.run(['readelf','-lW',str(path)],capture_output=True,text=True,check=True)
        if 'INTERP' in result.stdout:raise ValueError('Dynamic executable: '+name)
        shutil.copy2(path,args.out/name)
    if (args.out/'constella-testnet-v5').stat().st_size>196608:raise ValueError('Node exceeds size gate')
    for name in ('LICENSE','THIRD_PARTY_NOTICES.md','README.md'):
        shutil.copy2(ROOT/name,args.out/name)
    metadata=json.loads(run(['cargo','metadata','--manifest-path','wallet/Cargo.toml','--locked','--offline','--format-version','1']))
    licenses=args.out/'licenses'; licenses.mkdir()
    shutil.copytree(ROOT/'third_party/runtime',licenses/'runtime')
    inventory=[]
    for package in metadata['packages']:
        crate_root=Path(package['manifest_path']).parent.resolve()
        record={k:package.get(k) for k in ('name','version','license','source')}
        record['notices']=[]
        candidates=list(crate_root.glob('LICENSE*'))+list(crate_root.glob('COPYING*'))
        if package.get('license_file'):candidates.append(crate_root/package['license_file'])
        for file in sorted(set(candidates)):
            resolved=file.resolve()
            if not resolved.is_relative_to(crate_root) or not resolved.is_file() or resolved.stat().st_size>1024*1024:continue
            target=licenses/(package['name']+'-'+package['version'])/file.name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(resolved,target);record['notices'].append(str(target.relative_to(args.out)))
        inventory.append(record)
    (args.out/'rust-dependencies.json').write_text(json.dumps(inventory,indent=2)+'\n')
    shutil.copytree(ROOT/'src/vendor',licenses/'Monocypher',ignore=shutil.ignore_patterns('*.c','*.h'))
    # Include vendored Go notices with their relative module paths.
    for path in (ROOT/'explorer/vendor').rglob('*'):
        if path.is_file() and any(word in path.name.upper() for word in ('LICENSE','LICENCE','COPYING','PATENTS')):
            target=licenses/'go'/path.relative_to(ROOT/'explorer/vendor');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
    source=args.out/('constella-'+version+'-source.tar')
    subprocess.run(['git','archive','--format=tar','--prefix=constella-'+version+'/', '-o',str(source.resolve()),revision],cwd=ROOT,check=True)
    manifest={'version':version,'revision':revision,'network':'testnet','chain_id':'2094b0868a27b032','architecture':arch,'banners':banners,'build_inputs':json.loads((ROOT/'release/build-inputs.json').read_text()),'go_build':run(['go','version','-m',str(files['constella-explorer'])]),'limitations':['Testnet candidate; mainnet not approved.','Current scans/qualification must be reviewed separately.']}
    (args.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    checksums=''.join(digest(path)+'  '+str(path.relative_to(args.out))+'\n' for path in sorted(args.out.rglob('*')) if path.is_file())
    (args.out/'SHA256SUMS').write_text(checksums)
    archive=Path(str(args.out)+'.tar.gz')
    with tarfile.open(archive,'w:gz') as tar:tar.add(args.out,arcname=args.out.name)
    print('Prepared',archive,'sha256='+digest(archive),'revision='+revision)
if __name__=='__main__':main()
