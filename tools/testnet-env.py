#!/usr/bin/env python3
"""Create a private local testnet configuration; never overwrite one."""
import os
from pathlib import Path
import secrets
ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / '.env.example').read_text().replace('TESTNET_DB_PASSWORD=\n', 'TESTNET_DB_PASSWORD=' + secrets.token_hex(32) + '\n')
fd = os.open(ROOT / '.env', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as stream:
    stream.write(text)
print('Created .env with a random database password, mode 0600. Configure mining only if needed.')
