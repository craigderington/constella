#!/usr/bin/env python3
"""Read-only v5 Explorer checkpoint check; never releases mining itself."""
import argparse
from datetime import datetime, timezone
import json
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('url')
parser.add_argument('--exact', action='store_true', help='require a held chain and check at its exact height')
parser.add_argument('--genesis', action='store_true', help='only verify fresh v5 genesis before miners join')
args = parser.parse_args()


def require(condition, message):
    if not condition:
        raise SystemExit(message)


with urllib.request.urlopen(args.url.rstrip('/') + '/api/stats', timeout=15) as response:
    stats = json.load(response)
meta = stats['meta']
require(meta['chain_id'] == '2094b0868a27b032', 'wrong chain profile')
require(meta['network'] == 'testnet', 'wrong network')
if args.genesis:
    require(int(meta['height']) == 0 and int(meta['txs']) == 0, 'not fresh genesis')
else:
    require(meta['peer'] == 'true', 'Explorer disconnected')
    require(meta.get('check') == 'ok', 'ledger comparison has not passed')
    require(int(meta['check_count']) == int(meta['check_total']) > 0, 'incomplete account check')
    checked = datetime.fromisoformat(meta['check_at'].replace('Z', '+00:00'))
    require(0 <= (datetime.now(timezone.utc) - checked).total_seconds() <= 120, 'stale ledger check')
    if args.exact:
        require(meta['check_height'] == meta['height'], 'hold mining and wait for the exact-height check')
print(json.dumps(stats, indent=2))
