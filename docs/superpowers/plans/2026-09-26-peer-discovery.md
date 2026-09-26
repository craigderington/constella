# Peer Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node started with no configuration finds honest peers, syncs, and cannot be isolated by an attacker who controls many addresses in few networks.

**Architecture:** A new `src/addr.c` owns the address store — netgroup bucketing keyed by a per-node secret, `new`/`tried` tables, eviction and persistence. `src/net.c` gains two gossip messages and swaps its pre-shared-key handshake for a static-key one built on the Ed25519 the wallet already uses. Outbound peer selection enforces network-group diversity, without which the table's diversity is decorative. The Go explorer mirrors the handshake and the gossip wire format.

**Tech Stack:** C11 (musl, `-Os`, static), monocypher (X25519/Ed25519/BLAKE2b, already vendored), Go 1.22 with `golang.org/x/crypto` (already vendored), Docker Compose.

**Spec:** `docs/superpowers/specs/2026-09-26-peer-discovery-design.md`

## Global Constraints

- The node binary must stay at or under **196,608 bytes (192 KB)** — raised from 153,600 by Task 1. `make size` is a hard gate. Current: 149,272.
- The binary grows in **4096-byte page steps**; an unchanged `make size` figure means "no page crossed", not "nothing added".
- **Consensus is untouched.** `SHARE_VERSION` stays 3 and the chain survives. If a task finds itself changing which shares or transactions are accepted, it has gone wrong.
- `NET_MAGIC` becomes `0x33545343` ("CST3") and is covered by the params drift guard in `explorer/internal/proto/params_test.go`, so C and Go move together or the build fails.
- **No new dependencies** in either language. Every primitive needed is already vendored.
- The existing AEAD framing — XChaCha20-Poly1305, 7-byte header as AD, implicit 64-bit counter nonce, per-direction keys — **does not change**. Only key agreement does.
- Netgroup is the **/16 for IPv4** and the **/32 for IPv6**.
- The explorer never validates signatures; it validates work, linkage, `tx_root` and claims.
- C11, `-Wall -Wextra` clean, terse lowercase comments explaining *why*, matching `src/`.
- Commit messages use a lowercase prefix and end with a `Co-Authored-By` trailer.
- Never `git push`. Never run `docker compose down -v`.

## Review Focus

Five things the spec implies but no task's own tests would otherwise exercise, most likely to bite first.

1. **An IPv4-mapped IPv6 address (`::ffff:1.2.3.4`).** Its netgroup must be the IPv4 `/16`, not the IPv6 `/32`. Get this wrong and an attacker gets a fresh bucket per address by connecting over v6 — eclipse resistance silently evaporates while every other test passes. → Task 2.
2. **Unroutable addresses in gossip** — `127.0.0.1`, `0.0.0.0`, `10/8`, `192.168/16`, `::1`. A peer that gossips these fills honest tables with addresses that can never connect, and they all share a netgroup. They must be rejected on receipt, not on dial. → Task 7.
3. **A node dialing itself.** With advertisement enabled, a node's own address comes back through gossip. Connecting to yourself wastes an outbound slot permanently and looks like a healthy peer. → Task 9.
4. **`peers.dat` from a different build, truncated, or hand-edited.** It must be discarded and rebuilt from seeds, never abort startup — the existing chain loader has the opposite behaviour and it is already a known defect. → Task 4.
5. **The bucket secret being readable or predictable.** If it is derivable from anything an attacker can observe, bucket placement becomes a public function and the whole defence collapses. It must be random, persisted with restrictive permissions, and never sent on the wire. → Task 3.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `src/addr.h` / `src/addr.c` | Netgroup extraction, bucket assignment, `new`/`tried` tables, promotion, eviction, persistence |

**Modified**

| File | Change |
|---|---|
| `Makefile` | `SIZE_MAX_BYTES` 196608 |
| `src/params.h` | `NET_MAGIC` → `0x33545343` |
| `src/net.h` / `src/net.c` | Static-key handshake replacing PSK; `MSG_ADDR`/`MSG_GETADDR`; netgroup-diverse outbound selection |
| `src/node.c` | Load `node.key`; `CONSTELLA_ADVERTISE`; wire `addr` into startup |
| `tests/test.c` | Netgroup, bucket, eviction, persistence, handshake vector, gossip validation |
| `explorer/internal/proto/proto.go` | `MsgAddr`/`MsgGetAddr`, `Magic`, handshake constants |
| `explorer/internal/proto/params_test.go` | `NET_MAGIC` guard already present — confirm it catches the bump |
| `explorer/internal/p2p/client.go` | Mirror handshake and gossip |
| `docs/protocol.md`, `CLAUDE.md`, `docker-compose.yml` | Document the new wire, drop PSK env vars |

---

## Task 1: Raise the size gate and bump the wire magic

Everything else needs the headroom, and the magic bump must land before any wire change so old and new nodes refuse each other cleanly rather than failing at decryption.

**Files:**
- Modify: `Makefile`, `src/params.h`, `explorer/internal/proto/proto.go`
- Test: `explorer/internal/proto/params_test.go` (already guards `NET_MAGIC`)

**Interfaces:**
- Consumes: nothing.
- Produces: `NET_MAGIC` = `0x33545343`; `SIZE_MAX_BYTES` = 196608.

- [ ] **Step 1: Confirm the existing guard catches a one-sided bump**

Change `NET_MAGIC` in `src/params.h` to `0x33545343u` but leave Go alone.

Run: `make explorer-test`
Expected: FAIL with `NET_MAGIC: C=0x33545343 Go=0x32545343`

This proves the guard works before you rely on it.

- [ ] **Step 2: Bump both sides**

`src/params.h`:
```c
#define NET_MAGIC      0x33545343u     /* "CST3": the handshake changed, so an
                                        * old peer must refuse rather than fail
                                        * opaquely at decrypt */
```

`explorer/internal/proto/proto.go`:
```go
Magic        = 0x33545343 // "CST3" — must match NET_MAGIC in src/params.h
```

- [ ] **Step 3: Raise the gate**

`Makefile`:
```make
SIZE_MAX_BYTES ?= 196608
```

- [ ] **Step 4: Verify**

Run: `make test && make explorer-test && make size`
Expected: 174/174, explorer green, `constella: 149272 bytes (limit 196608)`

- [ ] **Step 5: Commit**

```bash
git add Makefile src/params.h explorer/internal/proto/proto.go
git commit -m "net: bump magic to CST3 and raise the size gate to 192 KB

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Netgroup extraction

The single function eclipse resistance rests on. Pure, and therefore fully testable.

**Files:**
- Create: `src/addr.h`, `src/addr.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `int addr_netgroup(const uint8_t ip[16], uint8_t out[8])` — writes the network-group key and returns its length; `int addr_is_routable(const uint8_t ip[16])`.

Addresses are stored as 16 bytes throughout, IPv4 held v4-mapped (`::ffff:a.b.c.d`), matching the wire format.

- [ ] **Step 1: Write the failing test**

Add to `tests/test.c`, called from `main()`:

```c
static void mk4(uint8_t ip[16], uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    memset(ip, 0, 10); ip[10] = 0xff; ip[11] = 0xff;
    ip[12] = a; ip[13] = b; ip[14] = c; ip[15] = d;
}

static void t_netgroup(void) {
    uint8_t ip[16], g1[8], g2[8];
    int n1, n2;

    /* IPv4: the /16 is the group, so the last two octets must not matter */
    mk4(ip, 203, 0, 113, 7);   n1 = addr_netgroup(ip, g1);
    mk4(ip, 203, 0, 200, 99);  n2 = addr_netgroup(ip, g2);
    CHECK(n1 == n2 && n1 == 2 && !memcmp(g1, g2, (size_t)n1));

    /* a different /16 is a different group */
    mk4(ip, 203, 1, 113, 7);   addr_netgroup(ip, g2);
    CHECK(memcmp(g1, g2, 2));

    /* Review Focus 1: a v4-mapped v6 address groups by the IPv4 /16, not the
     * v6 /32. Otherwise an attacker gets a fresh bucket per address simply by
     * connecting over v6, and eclipse resistance quietly disappears. */
    mk4(ip, 203, 0, 113, 7);
    CHECK(addr_netgroup(ip, g2) == 2 && !memcmp(g1, g2, 2));

    /* native IPv6 groups by the /32 */
    uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 1, 2, 3, 4};
    CHECK(addr_netgroup(v6, g1) == 4);
    v6[7] = 99;                                   /* below the /32 */
    CHECK(addr_netgroup(v6, g2) == 4 && !memcmp(g1, g2, 4));
    v6[3] = 0xb9;                                 /* inside the /32 */
    CHECK(addr_netgroup(v6, g2) == 4 && memcmp(g1, g2, 4));

    /* routability: gossiping these fills honest tables with dead entries */
    mk4(ip, 8, 8, 8, 8);        CHECK(addr_is_routable(ip));
    mk4(ip, 127, 0, 0, 1);      CHECK(!addr_is_routable(ip));
    mk4(ip, 0, 0, 0, 0);        CHECK(!addr_is_routable(ip));
    mk4(ip, 10, 0, 0, 1);       CHECK(!addr_is_routable(ip));
    mk4(ip, 192, 168, 1, 1);    CHECK(!addr_is_routable(ip));
    mk4(ip, 172, 16, 0, 1);     CHECK(!addr_is_routable(ip));
    mk4(ip, 169, 254, 1, 1);    CHECK(!addr_is_routable(ip));
    uint8_t lo6[16] = {0}; lo6[15] = 1;
    CHECK(!addr_is_routable(lo6));
    uint8_t ula[16] = {0xfd}; CHECK(!addr_is_routable(ula));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test_constella`
Expected: FAIL — `addr.h: No such file or directory`

- [ ] **Step 3: Write `src/addr.h`**

```c
/* Peer address store. Addresses are always 16 bytes, IPv4 held v4-mapped, so
 * one representation covers both families on the wire and in the tables. */
#ifndef ADDR_H
#define ADDR_H
#include <stdint.h>

#define ADDR_NEW_BUCKETS   32
#define ADDR_TRIED_BUCKETS 8
#define ADDR_BUCKET_SIZE   32

/* Network group: the /16 for IPv4, the /32 for IPv6. Bucketing by this rather
 * than by address is what bounds how much of a table one attacker can hold.
 * Returns the key length in bytes. */
int addr_netgroup(const uint8_t ip[16], uint8_t out[8]);

/* Loopback, unspecified, RFC1918, link-local and ULA are never worth storing:
 * they cannot be dialled across the internet and they all share a netgroup. */
int addr_is_routable(const uint8_t ip[16]);
#endif
```

- [ ] **Step 4: Write `src/addr.c`**

```c
#include "addr.h"
#include <string.h>

static int v4mapped(const uint8_t ip[16]) {
    static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    return !memcmp(ip, pfx, 12);
}

int addr_netgroup(const uint8_t ip[16], uint8_t out[8]) {
    if (v4mapped(ip)) { out[0] = ip[12]; out[1] = ip[13]; return 2; }
    memcpy(out, ip, 4);
    return 4;
}

int addr_is_routable(const uint8_t ip[16]) {
    if (v4mapped(ip)) {
        uint8_t a = ip[12], b = ip[13];
        if (a == 0 || a == 127 || a == 10) return 0;
        if (a == 192 && b == 168) return 0;
        if (a == 172 && (b & 0xf0) == 16) return 0;
        if (a == 169 && b == 254) return 0;
        if (a >= 224) return 0;                       /* multicast, reserved */
        return 1;
    }
    static const uint8_t zero[16] = {0};
    if (!memcmp(ip, zero, 16)) return 0;              /* :: */
    if (!memcmp(ip, zero, 15) && ip[15] == 1) return 0; /* ::1 */
    if ((ip[0] & 0xfe) == 0xfc) return 0;             /* fc00::/7 ULA */
    if (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80) return 0; /* fe80::/10 */
    if (ip[0] == 0xff) return 0;                      /* multicast */
    return 1;
}
```

Add `src/addr.c` to `CORE` in the `Makefile`, and `#include "addr.h"` to `tests/test.c`.

- [ ] **Step 5: Verify, then confirm the test can fail**

Run: `make test`
Expected: PASS, count up by ~16.

Then temporarily make `addr_netgroup` return `memcpy(out, ip, 4); return 4;` unconditionally (the v6 path for everything). Rebuild: the v4-mapped assertions must fail. Restore byte-identical and re-run. **Report both outputs.** A netgroup function that cannot fail this test is not protecting anything.

- [ ] **Step 6: Commit**

```bash
git add src/addr.h src/addr.c Makefile tests/test.c
git commit -m "addr: network-group extraction and routability

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: The address tables

**Files:**
- Modify: `src/addr.h`, `src/addr.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `addr_netgroup`, `addr_is_routable` (Task 2).
- Produces:
```c
typedef struct { uint8_t ip[16]; uint16_t port; uint32_t seen; uint8_t tried, ok; } addr_t;
void addr_init(const uint8_t secret[16]);
int  addr_add(const uint8_t ip[16], uint16_t port, uint32_t seen);   /* 1 stored */
int  addr_good(const uint8_t ip[16], uint16_t port);                 /* handshake ok */
int  addr_select(addr_t *out, const uint8_t (*avoid)[8], int navoid);
int  addr_count(int tried);
int  addr_bucket_of(const uint8_t ip[16], int tried);                /* test hook */
```

`addr_good` is called on a **completed handshake**. Promotion to `tried` needs **two** such calls on separate connection attempts — one proves someone answered once, two proves durable reachability.

- [ ] **Step 1: Write the failing test — the eclipse property is the point**

```c
static void t_addr_tables(void) {
    uint8_t secret[16] = {0};
    for (int i = 0; i < 16; i++) secret[i] = (uint8_t)(i * 7 + 1);
    addr_init(secret);

    uint8_t ip[16];
    /* Review Focus 5 / the security claim, written executably: 10,000
     * addresses from one /16 must occupy ONE bucket — 32 of 1024 new slots.
     * If this cannot fail, we built a hash table, not eclipse resistance. */
    for (int i = 0; i < 10000; i++) {
        mk4(ip, 203, 0, (uint8_t)(i >> 8), (uint8_t)i);
        addr_add(ip, 7043, 1000 + (uint32_t)i);
    }
    CHECK(addr_count(0) <= ADDR_BUCKET_SIZE);

    mk4(ip, 203, 0, 1, 1);
    int b = addr_bucket_of(ip, 0);
    for (int i = 0; i < 200; i++) {
        mk4(ip, 203, 0, (uint8_t)(i >> 8), (uint8_t)i);
        CHECK(addr_bucket_of(ip, 0) == b);      /* same /16 -> same bucket */
    }

    /* a different secret must place the same netgroup differently, or an
     * attacker could predict a victim's layout */
    int b1 = addr_bucket_of(ip, 0);
    uint8_t other[16]; memset(other, 0xA5, 16);
    addr_init(other);
    CHECK(addr_bucket_of(ip, 0) != b1 || 1);    /* may collide; see below */
    int diff = 0;
    for (int i = 0; i < 64; i++) {
        mk4(ip, (uint8_t)(10 + i), 0, 1, 1);
        addr_init(secret); int x = addr_bucket_of(ip, 0);
        addr_init(other);  if (addr_bucket_of(ip, 0) != x) diff++;
    }
    CHECK(diff > 40);            /* overwhelmingly different, not identical */

    /* unroutable is refused outright */
    addr_init(secret);
    mk4(ip, 127, 0, 0, 1); CHECK(addr_add(ip, 7043, 1) == 0);
    mk4(ip, 10, 0, 0, 1);  CHECK(addr_add(ip, 7043, 1) == 0);

    /* promotion needs two handshakes on separate attempts */
    mk4(ip, 198, 51, 100, 4);
    CHECK(addr_add(ip, 7043, 1) == 1);
    CHECK(addr_count(1) == 0);
    addr_good(ip, 7043);  CHECK(addr_count(1) == 0);   /* one is not enough */
    addr_good(ip, 7043);  CHECK(addr_count(1) == 1);   /* two promotes */

    /* selection honours the avoid list, so outbound stays netgroup-diverse */
    uint8_t avoid[1][8]; int n = addr_netgroup(ip, avoid[0]);
    (void)n;
    addr_t got;
    for (int i = 0; i < 20; i++) {
        if (addr_select(&got, avoid, 1)) {
            uint8_t g[8]; addr_netgroup(got.ip, g);
            CHECK(memcmp(g, avoid[0], 2));
        }
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test_constella`
Expected: FAIL — `addr_init` and friends undeclared.

- [ ] **Step 3: Implement**

Append to `src/addr.h`:

```c
typedef struct { uint8_t ip[16]; uint16_t port; uint32_t seen; uint8_t tried, ok; } addr_t;

/* `secret` keys bucket placement. It is random per node, persisted, and never
 * gossiped: if an attacker learns it they can shop for addresses that land in
 * a victim's buckets and the whole defence collapses. */
void addr_init(const uint8_t secret[16]);
int  addr_add(const uint8_t ip[16], uint16_t port, uint32_t seen);
int  addr_good(const uint8_t ip[16], uint16_t port);
int  addr_select(addr_t *out, const uint8_t (*avoid)[8], int navoid);
int  addr_count(int tried);
int  addr_bucket_of(const uint8_t ip[16], int tried);
```

In `src/addr.c` add the tables, a BLAKE2b-keyed bucket function, insert with
stalest-entry eviction, `addr_good` incrementing `ok` and promoting at 2, and
`addr_select` drawing mostly from `tried` while skipping any candidate whose
netgroup appears in `avoid`. Use `blake2b` from `blake2b.h`, already linked.

- [ ] **Step 4: Verify**

Run: `make test`
Expected: PASS.

- [ ] **Step 5: Prove the eclipse test can fail**

Temporarily change the bucket function to `bucket = H(secret || full_ip) mod nbuckets` — per-address rather than per-netgroup, which is the natural-looking mistake. Rebuild. The 10,000-address assertion must fail, because the addresses now spread across every bucket. Restore byte-identical and re-run. **Report both outputs.**

- [ ] **Step 6: Commit**

```bash
git add src/addr.h src/addr.c tests/test.c
git commit -m "addr: new/tried tables bucketed by network group

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: Persistence

**Files:**
- Modify: `src/addr.h`, `src/addr.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: Task 3's tables.
- Produces: `int addr_load(const char *datadir);` `void addr_save(const char *datadir);`

`addr_load` generates and persists a fresh random secret when no file exists.

- [ ] **Step 1: Write the failing test**

```c
/* Review Focus 4: a corrupt or foreign peers.dat must be discarded and the
 * node must start. The chain loader's opposite behaviour — stop at the bad
 * record, keep the bad suffix — is a known defect; do not repeat it here. */
static void t_addr_persist(void) {
    const char *dir = "/tmp/constella-addrtest";
    char path[256];
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/peers.dat", dir);
    unlink(path);

    CHECK(addr_load(dir) == 0);          /* no file: fresh secret, empty */
    uint8_t ip[16];
    mk4(ip, 198, 51, 100, 9);
    addr_add(ip, 7043, 42);
    addr_good(ip, 7043); addr_good(ip, 7043);
    int n_new = addr_count(0), n_tried = addr_count(1);
    int b = addr_bucket_of(ip, 1);
    addr_save(dir);

    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) == n_new && addr_count(1) == n_tried);
    CHECK(addr_bucket_of(ip, 1) == b);   /* the secret round-tripped too */

    FILE *f = fopen(path, "r+"); CHECK(f != NULL);
    if (f) { fseek(f, 3, SEEK_SET); fputc(0xff, f); fclose(f); }
    CHECK(addr_load(dir) == 0);          /* corrupt: discarded, still starts */
    CHECK(addr_count(0) == 0 && addr_count(1) == 0);

    f = fopen(path, "wb"); if (f) { fwrite("xx", 1, 2, f); fclose(f); }
    CHECK(addr_load(dir) == 0);          /* truncated: same */
    unlink(path);
}
```

- [ ] **Step 2–4:** run (FAIL), implement a magic + version header, the secret, then both tables with a trailing BLAKE2b checksum; on any mismatch zero the tables and keep the fresh secret. Write via `<dir>/peers.dat.tmp` + `fsync` + `rename`, mode `0600` — the secret lives in this file. Re-run (PASS).

- [ ] **Step 5: Prove it can fail** — remove the checksum verification, confirm the corrupt-file assertion fails, restore, re-run. Report both.

- [ ] **Step 6: Commit**

```bash
git add src/addr.h src/addr.c tests/test.c
git commit -m "addr: persist tables and bucket secret, discard corrupt files

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 5: Node identity and the static-key handshake (C)

**Files:**
- Modify: `src/net.h`, `src/net.c`, `src/node.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `wallet_t`, `wallet_load` (existing, `src/wallet.h`).
- Produces: `net_init(uint16_t port, const char *peers_csv, const wallet_t *id, net_msg_fn, net_conn_fn)` — the `psk_hex` parameter is **replaced** by the node identity. Test hook: `int net_handshake_vector(uint8_t out_lo[32], uint8_t out_hi[32], const uint8_t eph_a_sk[32], const uint8_t eph_b_sk[32], const uint8_t id_a[32], const uint8_t id_b[32]);`

The node's key is `<datadir>/node.key`, loaded with the existing `wallet_load(&w, path, 1)` — deliberately **not** the payout key.

- [ ] **Step 1: Write the failing test** — a pinned handshake vector with fixed ephemeral and static keys, asserting the derived `k_lo`/`k_hi` as hex. Compute the expected values with the Python in Step 3 *before* writing them down, exactly as the AEAD vector was produced.

- [ ] **Step 2: Run to verify it fails** — `net_handshake_vector` undeclared.

- [ ] **Step 3: Implement.** Replace `MSG_AUTH`'s challenge/proof with `eph_pub[32] || id_pub[32] || sig[64]`, where `sig` covers `"CSTL-HS1" || eph_self || eph_peer` once both are known — so the signature binds the session, not just the identity. Derive:

```
shared  = X25519(eph_sk, eph_peer_pub)
k_lo/hi = BLAKE2b-keyed(shared, "CSTL-P2P2" || "lo"|"hi" || min(id) || max(id))
```

Reject `id_self == id_peer` (a node talking to itself) and reject a signature that does not verify. Keep the existing AEAD framing untouched.

Generate the pinned vector independently in Python and check it against the C before committing:

```bash
python3 - <<'PY'
# expected k_lo/k_hi for the fixed test keys; cross-check against the C output
PY
```

- [ ] **Step 4: Verify** — `make test`, and confirm the vector matches.
- [ ] **Step 5: Prove it can fail** — flip one byte of the `"CSTL-P2P2"` label, confirm the vector test fails, restore. Report both.
- [ ] **Step 6: Commit.**

---

## Task 6: Handshake mirror in Go

**Files:**
- Modify: `explorer/internal/p2p/client.go`, `explorer/internal/proto/proto.go`
- Test: `explorer/internal/p2p/client_test.go`

**Interfaces:**
- Consumes: Task 5's wire format and pinned vector.
- Produces: a Go handshake producing identical `k_lo`/`k_hi`.

- [ ] **Step 1:** Write the failing test asserting **the same pinned hex** as Task 5. If Go disagrees, stop and report — that mismatch is the whole reason the vector exists.
- [ ] **Step 2–4:** run (FAIL), implement with `curve25519`, stdlib `ed25519` and `x/crypto/blake2b`, re-run (PASS).
- [ ] **Step 5:** break the label as in Task 5, confirm failure, restore.
- [ ] **Step 6: Commit.**

---

## Task 7: Gossip messages (C)

**Files:**
- Modify: `src/net.h`, `src/net.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `addr_add` (Task 3).
- Produces: `MSG_GETADDR = 10`, `MSG_ADDR = 11`; `ADDR_MAX_ENTRIES 180`.

Payload: `u16 count` then `count ×` { `ip[16]`, `u16 port`, `u32 seen` } — 22 bytes each. 180 entries is 3,960 bytes, inside `NET_MAXPAY` with margin.

- [ ] **Step 1: Write the failing test**

```c
/* Review Focus 2: unroutable addresses must be dropped on receipt. A peer
 * gossiping 127.0.0.1 or 10/8 otherwise fills honest tables with entries that
 * can never connect — and they all share one netgroup. */
static void t_addr_msg(void) {
    uint8_t secret[16]; memset(secret, 3, 16);
    addr_init(secret);
    uint8_t buf[4096]; uint8_t ip[16];
    int n = 0;
    mk4(ip, 198, 51, 100, 1);  n += addr_msg_put(buf + n, ip, 7043, 100);
    mk4(ip, 127, 0, 0, 1);     n += addr_msg_put(buf + n, ip, 7043, 100);
    mk4(ip, 10, 1, 1, 1);      n += addr_msg_put(buf + n, ip, 7043, 100);
    CHECK(addr_msg_ingest(buf, (uint16_t)n, 3) == 1);   /* only the routable one */

    CHECK(addr_msg_ingest(buf, 5, 3) == -1);            /* short/malformed */
    CHECK(addr_msg_ingest(buf, (uint16_t)n, ADDR_MAX_ENTRIES + 1) == -1);
}
```

- [ ] **Step 2–4:** run (FAIL), implement `addr_msg_put`/`addr_msg_ingest` with strict bounds — validate the count before indexing and the length against the count — plus answering `GETADDR` **once per connection** and rate-limiting unsolicited `ADDR`. Re-run (PASS).
- [ ] **Step 5:** remove the `addr_is_routable` check, confirm the test fails, restore. Report both.
- [ ] **Step 6: Commit.**

---

## Task 8: Gossip mirror in Go

**Files:** `explorer/internal/proto/proto.go`, `explorer/internal/p2p/client.go`, tests alongside.

Same message numbers, same 22-byte entry layout, same 180 cap, same routability filter. Pin a shared encode vector in both languages. Prove the Go test fails against a deliberately wrong entry size before fixing.

- [ ] Steps 1–6 as above.

---

## Task 9: Netgroup-diverse outbound selection

**Files:**
- Modify: `src/net.c`, `src/node.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `addr_select` (Task 3).
- Produces: eight outbound slots, each in a distinct netgroup.

- [ ] **Step 1: Write the failing test** — build a table containing only one `/16`, then assert that at most one outbound candidate can be selected. Also assert **Review Focus 3**: a node never selects its own advertised address.

- [ ] **Step 2–4:** run (FAIL), implement — collect the netgroups of current outbound peers into the `avoid` list, pass to `addr_select`, and skip self. `dial()` takes an `addr_t` rather than a seed index; the existing `seed_t` path stays for `CONSTELLA_PEERS`. Re-run (PASS).
- [ ] **Step 5:** remove the `avoid` argument, confirm the diversity assertion fails, restore.
- [ ] **Step 6: Commit.**

---

## Task 10: Bootstrap

**Files:** `src/net.c`, `src/node.c`, `docker-compose.yml`.

Order: `peers.dat`, then DNS seeds, then hardcoded fallbacks. `CONSTELLA_PEERS` remains a manual override.

**`getaddrinfo` is already used by `dial()`**, so the resolver is already linked and DNS seeding costs only the seed-list logic — the spec's open question is settled by inspection, not measurement. Record the `make size` delta anyway.

- [ ] **Step 1:** Write the failing test for `CONSTELLA_ADVERTISE` parsing (valid `host:port`, missing port, garbage, oversized) — the parsing is testable even though DNS is not.
- [ ] **Step 2–4:** run, implement, verify.
- [ ] **Step 5: Commit.**

---

## Task 11: Remove the PSK, document, measure

**Files:** `src/net.c`, `src/net.h`, `src/cli.c`, `src/node.c`, `explorer/`, `docker-compose.yml`, `docs/protocol.md`, `CLAUDE.md`.

- [ ] **Step 1:** Delete `CONSTELLA_P2P_KEY` / `EXPLORER_P2P_KEY`, the `psk` state, `auth_proof`, and the `psk_hex` parameters. Removal, not deprecation — leaving a shared-secret path invites someone to use it and believe they are authenticated. Confirm `grep -ri psk src/ explorer/ docker-compose.yml` returns nothing but history.
- [ ] **Step 2:** Update `docs/protocol.md` with the handshake, both gossip messages, the bucketing rule, and the `CST3` magic — enough that a third implementation could be built from it.
- [ ] **Step 3:** Update `CLAUDE.md`: discovery in Verified with what was actually observed, the new size figure, and what remains unverified.
- [ ] **Step 4:** Run `make test`, `make explorer-test`, `make size`. Report the final byte count against 196,608 and how many page steps discovery cost.
- [ ] **Step 5:** Bring up the stack and confirm nodes discover each other **with `CONSTELLA_PEERS` unset** — the whole point. Then stop one node, restart it, and confirm it reconnects from `peers.dat` without any configured seed.
- [ ] **Step 6: Commit.**

---

## Notes for the executor

- **Do not run `docker compose down -v`.** It destroys wallets, the sharechain and Postgres. It is the repo owner's command.
- Consensus is untouched; the existing chain survives this work.
- This project has produced **five** tests that passed regardless of the bug they claimed to cover. Every step above that says "prove it can fail" is there because of one of them. A test nobody has watched fail is not a test.
