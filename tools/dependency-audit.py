#!/usr/bin/env python3
"""Query OSV for locked crate/module versions; this is not reachability analysis."""
import argparse
import concurrent.futures
import datetime as dt
import json
from pathlib import Path
import re
import tomllib
import urllib.request
ROOT = Path(__file__).resolve().parents[1]

def read(url, data=None):
    request=urllib.request.Request(url, data=data, headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:return json.load(response)

def go_queries(text):
    for line in text.splitlines():
        match=re.fullmatch(r'\s*(?:require\s+)?(\S+)\s+(v\S+)(?:\s+// indirect)?\s*',line)
        if match:yield {'package':{'name':match[1],'ecosystem':'Go'},'version':match[2]}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--fail-on-rust-advisories',action='store_true')
    args=parser.parse_args()
    lock=tomllib.loads((ROOT/'wallet/Cargo.lock').read_text())
    queries=[{'package':{'name':p['name'],'ecosystem':'crates.io'},'version':p['version']}
             for p in lock['package'] if p.get('source','').startswith('registry+')]
    queries.extend(go_queries((ROOT/'explorer/go.mod').read_text()))
    data=read('https://api.osv.dev/v1/querybatch',json.dumps({'queries':queries}).encode())
    if len(data['results'])!=len(queries):raise ValueError('Incomplete vulnerability response')
    matches=[{'query':q,'vulns':r['vulns']} for q,r in zip(queries,data['results']) if r.get('vulns')]
    ids=sorted({v['id'] for m in matches for v in m['vulns']})
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        details=dict(zip(ids,pool.map(lambda x:read('https://api.osv.dev/v1/vulns/'+x),ids)))
    report={'recorded_at':dt.datetime.now(dt.timezone.utc).isoformat(),'queries':queries,
            'version_matches':matches,'details':details,
            'limitation':'Version matches only. Run govulncheck for Go reachability; assess Rust features and target separately.'}
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(report,indent=2)+'\n')
    for m in matches:print(m['query']['package']['name'],m['query']['version'],','.join(v['id'] for v in m['vulns']))
    print('Checked',len(queries),'locked dependencies;',len(ids),'advisory identifiers matched')
    if args.fail_on_rust_advisories and any(m['query']['package']['ecosystem']=='crates.io' and
       any(not details[v['id']].get('withdrawn') for v in m['vulns']) for m in matches):raise SystemExit(1)
if __name__=='__main__':main()
