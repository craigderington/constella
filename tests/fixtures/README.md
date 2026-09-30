# Sync fork fixture

`sync-fork.v3` is the first 29 public share records (4,466 bytes) from the
2026-09-28 Gate 2 local checkpoint. It contains canonical heights 1–28 followed
by a competing height-1 share. It contains no keys. All records are validated
by the real chain loader/submission code in the sync regression.

SHA-256: `c5e95cad9a88729d8d1a8df030d6cc5d69c1ac77b086752dcf9c22b8b232f481`.

The regression withholds the final side share, then delivers a known canonical
share and that weaker side share through the real node message handler. Both
must advance the next GETCHAIN locator even while the canonical tip is unchanged.
