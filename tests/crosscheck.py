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

# 4. Science lane: region derivation, gap validity and payout weight
SCI_BITS, SCI_G_MIN, SCI_G_MAX, SCI_G_STEP = 256, 384, 4096, 123

def sci_region(anchor, miner):
    seed = hashlib.blake2b(b"CSTL-SCI1" + anchor + miner, digest_size=32).digest()
    return (1 << (SCI_BITS - 1)) | int.from_bytes(seed[:24], "big")

def sci_valid(base, k, g):
    if not (SCI_G_MIN <= g <= SCI_G_MAX) or k >= (1 << 40):
        return False
    p = base + k
    return is_prime(p) and is_prime(p + g) and not any(is_prime(p + i) for i in range(1, g))

def sci_work(g):
    if g < SCI_G_MIN: return 0
    d = g - SCI_G_MIN
    e = min(d // SCI_G_STEP, 40)
    return (1 << e) + ((1 << e) * (d % SCI_G_STEP) // SCI_G_STEP)

# share_root: the commitment C and Go must agree on byte for byte
def sci_ser(k, g):
    return k.to_bytes(8, "little") + g.to_bytes(4, "little")

def share_root(txs, claims):
    if not txs and not claims:
        return bytes(32)
    buf = b"CSTL-TXR" + b"".join(txs) + b"CSTL-SCI" + b"".join(sci_ser(k, g) for k, g in claims)
    return hashlib.blake2b(buf, digest_size=32).digest()

root_vectors = [
    ([], bytes(32).hex()),
    ([(950, 776)], "ed71d999b7eac9a786db8c2876b73a5f776bc238c887bcc9f5e6a31805586d9e"),
    ([(950, 776), (1726, 400)], "984e182436e7b5c9c892305c84f15f13748f04b9b020a259df021fc86e4697b5"),
]
rbad = sum(share_root([], c).hex() != want for c, want in root_vectors)
print(f"root:    {len(root_vectors) - rbad}/{len(root_vectors)} share_root vectors match python")

# domain separation: 3*152 == 38*12 == 456, so the same bytes split two ways
flat = bytes((i * 7 + 3) % 256 for i in range(456))
as_tx = share_root([flat[i * 152:(i + 1) * 152] for i in range(3)], [])
as_sci = share_root([], [(int.from_bytes(flat[i * 12:i * 12 + 8], "little"),
                          int.from_bytes(flat[i * 12 + 8:i * 12 + 12], "little")) for i in range(38)])
dbad = as_tx == as_sci
print(f"root:    domain tags separate the split: {'ok' if not dbad else 'FAIL'}")

cases = [(bytes(32), bytes([1]) * 32, 950, 776),     # the real gap: merit 4.37
         (bytes(32), bytes([1]) * 32, 950, 846),     # a prime sits inside
         (bytes(32), bytes([1]) * 32, 950, 777),     # p+g composite
         (bytes(32), bytes([1]) * 32, 951, 776),     # p composite
         (bytes(32), bytes([1]) * 32, 746, 176),     # real gap, below the floor
         (bytes(32), bytes([2]) * 32, 950, 776),     # another miner's region
         (bytes([0xaa]) * 32, bytes([1]) * 32, 950, 776)]  # another anchor
inp = "".join(f"{a.hex()} {m.hex()} {k} {g}\n" for a, m, k, g in cases)
got = run(["--sci"], inp)
sbad = 0
for i, (a, m, k, g) in enumerate(cases):
    base, ok, w = int(got[i * 3]), got[i * 3 + 1] == "1", int(got[i * 3 + 2])
    want_base, want_ok, want_w = sci_region(a, m), sci_valid(sci_region(a, m), k, g), sci_work(g)
    bad_here = base != want_base or ok != want_ok or w != want_w
    sbad += bad_here
    print(f"sci:     k={k:5d} g={g:5d} valid={ok!s:5s} work={w:<10d} {'ok' if not bad_here else 'FAIL'}")
print(f"sci:     {len(cases) - sbad}/{len(cases)} match python")

# 6. The P2P handshake key schedule, against a pure-Python X25519.
#    Deliberately not a crypto library: the point of a pinned vector is that
#    two *independent* implementations agree, and the RFC 7748 reference ladder
#    below shares no lineage with monocypher. Keyed BLAKE2b is stdlib.
X_P, X_A24 = 2**255 - 19, 121665

def _cswap(swap, a, b):
    d = (swap * ((a - b) % X_P)) % X_P
    return (a - d) % X_P, (b + d) % X_P

def x25519(k_bytes, u_bytes):
    """RFC 7748 section 5, the Montgomery ladder, verbatim."""
    k = bytearray(k_bytes)
    k[0] &= 248; k[31] &= 127; k[31] |= 64
    k = int.from_bytes(k, "little")
    x1 = int.from_bytes(u_bytes, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in range(254, -1, -1):
        kt = (k >> t) & 1
        swap ^= kt
        x2, x3 = _cswap(swap, x2, x3)
        z2, z3 = _cswap(swap, z2, z3)
        swap = kt
        a = (x2 + z2) % X_P; aa = a * a % X_P
        b = (x2 - z2) % X_P; bb = b * b % X_P
        e = (aa - bb) % X_P
        c = (x3 + z3) % X_P; d = (x3 - z3) % X_P
        da = d * a % X_P; cb = c * b % X_P
        x3 = (da + cb) % X_P; x3 = x3 * x3 % X_P
        z3 = (da - cb) % X_P; z3 = x1 * (z3 * z3 % X_P) % X_P
        x2 = aa * bb % X_P
        z2 = e * ((aa + X_A24 * e) % X_P) % X_P
    x2, x3 = _cswap(swap, x2, x3)
    z2, z3 = _cswap(swap, z2, z3)
    return (x2 * pow(z2, X_P - 2, X_P) % X_P).to_bytes(32, "little")

X_BASE = (9).to_bytes(32, "little")

def hs_keys(eph_a_sk, eph_b_sk, id_a, id_b):
    shared = x25519(eph_a_sk, x25519(eph_b_sk, X_BASE))
    lo_id, hi_id = (id_a, id_b) if id_a < id_b else (id_b, id_a)
    k = lambda d: hashlib.blake2b(b"CSTL-P2P2" + d + lo_id + hi_id,
                                  key=shared, digest_size=32).digest()
    return k(b"lo"), k(b"hi")

# RFC 7748 section 6.1 first: if the ladder itself is wrong, everything below
# agrees with the C for the wrong reason.
_a = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
_b = bytes.fromhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
rfc_ok = (x25519(_a, X_BASE).hex() == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"
          and x25519(_a, x25519(_b, X_BASE)).hex()
          == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
print(f"x25519:  RFC 7748 6.1 {'ok' if rfc_ok else 'FAIL'}")

hs_cases = [(bytes(i + 1 for i in range(32)), bytes(255 - i for i in range(32)),
             b"\xaa" * 32, b"\x55" * 32)]                # the vector pinned in tests/test.c
for _ in range(24):
    hs_cases.append(tuple(bytes(random.getrandbits(8) for _ in range(32)) for _ in range(4)))
got = run(["--hs"], "".join(" ".join(v.hex() for v in c) + "\n" for c in hs_cases))
hsbad = 0
for i, c in enumerate(hs_cases):
    want = hs_keys(*c)
    hsbad += (got[i * 2], got[i * 2 + 1]) != (want[0].hex(), want[1].hex())
print(f"hs:      {len(hs_cases) - hsbad}/{len(hs_cases)} match python x25519 + keyed blake2b")

# 7. The handshake signature scheme, against a pure-Python EdDSA-BLAKE2b.
#    This is NOT RFC 8032 Ed25519: monocypher hashes with BLAKE2b-512 where
#    RFC 8032 uses SHA-512, so the two never interoperate. The structure is
#    otherwise RFC 8032's, which is why the reference ladder below is the RFC's
#    with one hash swapped - and why it is worth cross-checking at all: the Go
#    explorer has to hand-write this scheme, and a pinned value transcribed out
#    of monocypher would only say "the C agrees with itself".
ED_Q = 2**252 + 27742317777372353535851937790883648493
ED_D = -121665 * pow(121666, X_P - 2, X_P) % X_P
ED_SQRT_M1 = pow(2, (X_P - 1) // 4, X_P)

def _ed_recover_x(y, sign):
    x2 = (y * y - 1) * pow(ED_D * y * y + 1, X_P - 2, X_P) % X_P
    x = pow(x2, (X_P + 3) // 8, X_P)
    if x * x % X_P != x2: x = x * ED_SQRT_M1 % X_P
    if (x & 1) != sign: x = X_P - x
    return x

_ED_GY = 4 * pow(5, X_P - 2, X_P) % X_P
ED_G = (_ed_recover_x(_ED_GY, 0), _ED_GY, 1, _ed_recover_x(_ED_GY, 0) * _ED_GY % X_P)

def _ed_add(a, b):
    A = (a[1] - a[0]) * (b[1] - b[0]) % X_P
    B = (a[1] + a[0]) * (b[1] + b[0]) % X_P
    C = 2 * a[3] * b[3] * ED_D % X_P
    Dd = 2 * a[2] * b[2] % X_P
    e, f, g, h = B - A, Dd - C, Dd + C, B + A
    return (e * f % X_P, g * h % X_P, f * g % X_P, e * h % X_P)

def _ed_mul(k, pt):
    r = (0, 1, 1, 0)
    while k > 0:
        if k & 1: r = _ed_add(r, pt)
        pt = _ed_add(pt, pt)
        k >>= 1
    return r

def _ed_compress(pt):
    zi = pow(pt[2], X_P - 2, X_P)
    x, y = pt[0] * zi % X_P, pt[1] * zi % X_P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")

def _b2_512(data): return hashlib.blake2b(data, digest_size=64).digest()
def _b2_modq(data): return int.from_bytes(_b2_512(data), "little") % ED_Q

def eddsa_b2_sign(seed, msg):
    """monocypher crypto_eddsa_key_pair + crypto_eddsa_sign, from the seed."""
    h = bytearray(_b2_512(seed))
    h[0] &= 248; h[31] &= 127; h[31] |= 64          # crypto_eddsa_trim_scalar
    a = int.from_bytes(h[:32], "little")
    prefix = _b2_512(seed)[32:]                      # the untrimmed upper half
    pub = _ed_compress(_ed_mul(a, ED_G))
    r = _b2_modq(prefix + msg)                       # deterministic nonce
    R = _ed_compress(_ed_mul(r, ED_G))
    k = _b2_modq(R + pub + msg)
    return pub, R + ((r + k * a) % ED_Q).to_bytes(32, "little")

sig_cases = [(bytes((i * 7 + 13) & 0xff for i in range(32)),
              b"constella handshake signature vector")]     # pinned in tests/test.c
for n in (0, 1, 32, 72, 127, 128, 129, 200):                 # incl. the 72-byte transcript
    sig_cases.append((bytes(random.getrandbits(8) for _ in range(32)),
                      bytes(random.getrandbits(8) for _ in range(n))))
got = run(["--sig"], "".join(f"{sd.hex()} {m.hex()}\n" for sd, m in sig_cases))
sigbad = 0
for i, (sd, m) in enumerate(sig_cases):
    want_pub, want_sig = eddsa_b2_sign(sd, m)
    sigbad += (got[i * 2], got[i * 2 + 1]) != (want_pub.hex(), want_sig.hex())
print(f"eddsa:   {len(sig_cases) - sigbad}/{len(sig_cases)} match python eddsa-blake2b "
      f"(NOT rfc8032 ed25519)")

sys.exit(1 if bad or b2bad or mbad or sbad or rbad or dbad or hsbad or sigbad or not rfc_ok else 0)
