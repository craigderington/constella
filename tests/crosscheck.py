#!/usr/bin/env python3
"""Cross-check the C primitives against Python reference implementations."""
import hashlib, random, subprocess, sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./test_constella"
random.seed(1729)

def run(args, data):
    return subprocess.run([BIN] + args, input=data, capture_output=True, text=True, check=True).stdout.split()

def is_prime(n):  # deterministic-enough Miller-Rabin for checking
    if n < 2: return False
    for q in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % q == 0: return n == q
    d, s = n - 1, 0
    while d % 2 == 0: d //= 2; s += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41):
        x = pow(a, d, n)
        if x in (1, n - 1): continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1: break
        else: return False
    return True

# 1. Fermat base-2 on random odd numbers, biased toward primes
nums = []
for bits in (64, 65, 127, 128, 192, 256, 511, 512, 768, 1024):
    for _ in range(40):
        x = random.getrandbits(bits) | (1 << (bits - 1)) | 1
        if random.random() < 0.5:
            while not is_prime(x): x += 2
        nums.append(x)
got = run(["--prp"], "\n".join(f"{x:x}" for x in nums) + "\n")
bad = [x for x, g in zip(nums, got) if int(g) != (pow(2, x - 1, x) == 1)]
print(f"prp:     {len(nums) - len(bad)}/{len(nums)} match python pow()")

# 2. BLAKE2b-256 on random lengths including block boundaries
msgs = [bytes(random.getrandbits(8) for _ in range(n)) for n in (0, 1, 84, 92, 127, 128, 129, 255, 256, 257, 400)]
got = run(["--b2"], "\n".join(m.hex() for m in msgs) + "\n")
b2bad = sum(g != hashlib.blake2b(m, digest_size=32).hexdigest() for m, g in zip(msgs, got))
print(f"blake2b: {len(msgs) - b2bad}/{len(msgs)} match hashlib")

# 3. Mined shares: every claimed prime is prime per Miller-Rabin
mbad = 0
for bits in (64, 128, 256, 384):
    p, tl = run(["--mine", str(bits)], "")
    p, tl = int(p), int(tl)
    offs = [0, 4, 6, 10, 12, 16][:tl]
    ok = all(is_prime(p + o) for o in offs) and p.bit_length() == bits and p % 210 == 97
    mbad += not ok
    print(f"mine:    {bits:4d} bits tuple={tl} p={str(p)[:24]}... {'ok' if ok else 'FAIL'}")

sys.exit(1 if bad or b2bad or mbad else 0)
