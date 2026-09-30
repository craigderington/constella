#!/usr/bin/env python3
"""Exercise real sampler/worker integration with synthetic inputs in a lab.

Only disposable Compose services receive these inputs. Never mount them into
live miners. The live canary uses its real host sensor separately.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', type=Path)
    ap.add_argument('--image', default='constella:pause-fix-20260929')
    args = ap.parse_args()
    root = args.directory.resolve()
    root.mkdir(mode=0o700)
    modes = {k: 'running' for k in ('battery', 'sensor', 'hot')}
    stop = threading.Event()
    services = {}
    for name in modes:
        p = root/name
        for d in ('data', 'thermal', 'power/BAT0', 'power/AC'):
            (p/d).mkdir(parents=True, exist_ok=True)
        (p/'power/BAT0/type').write_text('Battery\n')
        (p/'power/AC/type').write_text('Mains\n')
        (p/'power/AC/online').write_text('1\n')
        services[name] = {
            'image': args.image, 'restart': 'no', 'cpus': 0.35,
            'user': f'{os.getuid()}:{os.getgid()}',
            'environment': {'CONSTELLA_THREADS':'2', 'CONSTELLA_DUTY':'25',
                            'CONSTELLA_TEMP_MAX':'80', 'CONSTELLA_TEMP_FILE':'/thermal/cpu',
                            'CONSTELLA_BATTERY_PAUSE':'1', 'CONSTELLA_PEERS':''},
            'volumes': [f'{p}/data:/data', f'{p}/thermal:/thermal:ro',
                        f'{p}/power:/sys/class/power_supply:ro'],
            'networks': ['lab'],
        }
    config = root/'compose.json'
    config.write_text(json.dumps({'services':services, 'networks':{'lab':{'internal':True}}}, indent=2))
    dc = ['docker','compose','-p','constella-pause-'+root.name,'-f',str(config)]

    def run(cmd):
        return subprocess.run(cmd, check=True, capture_output=True, text=True)

    def publish():
        while not stop.is_set():
            for name, mode in modes.items():
                p = root/name
                (p/'power/AC/online').write_text('0\n' if mode=='battery' else '1\n')
                if mode!='sensor':
                    (p/'thermal/next').write_text('100000\n' if mode=='hot' else '45000\n')
                    (p/'thermal/next').replace(p/'thermal/cpu')
            stop.wait(0.2)

    def statuses(name):
        p = run(dc+['logs','--no-color',name])
        log = p.stdout+p.stderr
        (root/(name+'.log')).write_text(log)
        rows=[]
        for line in log.splitlines():
            m=re.search(r'duty=(\d+)%.*\(([^)]+)\) found=(\d+) ([0-9.]+) cand/s sci=(\d+)/',line)
            if m:
                rows.append({'duty':int(m[1]),'reason':m[2], 'found':int(m[3]),
                             'candidates_per_second':float(m[4]),'science_found':int(m[5])})
        return rows

    def wait(predicate, label, timeout=150):
        until=time.monotonic()+timeout
        while time.monotonic()<until:
            if predicate():return
            time.sleep(2)
        raise RuntimeError('timeout: '+label)

    thread=threading.Thread(target=publish)
    thread.start()
    evidence={'image':args.image,'cases':{}}
    try:
        time.sleep(0.5)
        run(dc+['up','-d'])
        wait(lambda: all(statuses(n) and statuses(n)[-1]['duty']>0
                         and statuses(n)[-1]['science_found']>0 for n in modes), 'initial mining')
        before={n:statuses(n)[-1] for n in modes}
        for n in modes:modes[n]=n
        expected={'battery':'on battery, paused','sensor':'sensor unavailable','hot':'too hot, stopped'}
        wait(lambda: all(sum(r['reason']==expected[n] and r['duty']==0
                             for r in statuses(n))>=3 for n in modes), 'three paused statuses')
        for n in modes:
            paused=[r for r in statuses(n) if r['reason']==expected[n] and r['duty']==0]
            a,b=paused[-2:]
            assert a['candidates_per_second']==b['candidates_per_second']==0, n
            assert a['found']==b['found'] and a['science_found']==b['science_found'], n
            evidence['cases'][n]={'before':before[n], 'paused':paused[-2:]}
            modes[n]='running'
        wait(lambda: all(statuses(n)[-1]['duty']>0 and
                         statuses(n)[-1]['science_found']>evidence['cases'][n]['paused'][-1]['science_found']
                         and statuses(n)[-1]['candidates_per_second']>0 for n in modes), 'both lanes resume')
        for n in modes:evidence['cases'][n]['resumed']=statuses(n)[-1]
        # A real sampler-induced pause must also permit a clean daemon exit.
        for n in modes:modes[n]=n
        time.sleep(6)
        started=time.monotonic()
        run(dc+['stop','-t','5'])
        evidence['stop_seconds']=round(time.monotonic()-started,3)
        for n in modes:
            cid=run(dc+['ps','-aq',n]).stdout.strip()
            state=json.loads(run(['docker','inspect',cid]).stdout)[0]['State']
            assert state['ExitCode']==0, (n,state)
            evidence['cases'][n]['shutdown_exit_code']=0
        evidence['status']='PASS'
        (root/'result.json').write_text(json.dumps(evidence,indent=2)+'\n')
        print(json.dumps(evidence,indent=2),flush=True)
    finally:
        try:
            run(dc+['stop','-t','5'])
        finally:
            stop.set();thread.join()


if __name__=='__main__':main()
