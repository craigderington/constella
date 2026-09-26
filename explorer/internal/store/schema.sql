-- Raw shares are the source of truth; every other table is derived and replaceable.
CREATE TABLE IF NOT EXISTS shares (
    seq        BIGSERIAL,
    id         BYTEA PRIMARY KEY,
    raw        BYTEA NOT NULL,
    height     INTEGER NOT NULL,
    prev       BYTEA NOT NULL,
    time       TIMESTAMPTZ NOT NULL,
    miner      BYTEA NOT NULL,
    bits       SMALLINT NOT NULL,
    k          BIGINT NOT NULL,
    tlen       SMALLINT NOT NULL,
    ntx        SMALLINT NOT NULL,
    nsci       SMALLINT NOT NULL DEFAULT 0,
    is_block   BOOLEAN NOT NULL,
    p          TEXT NOT NULL,
    certified  BOOLEAN,
    on_main    BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX IF NOT EXISTS shares_main_height ON shares (height DESC) WHERE on_main;
CREATE INDEX IF NOT EXISTS shares_blocks ON shares (height DESC) WHERE on_main AND is_block;
CREATE INDEX IF NOT EXISTS shares_miner ON shares (miner, height DESC);
CREATE INDEX IF NOT EXISTS shares_seq ON shares (seq);

CREATE TABLE IF NOT EXISTS txs (
    uid       BYTEA PRIMARY KEY,           -- share id || idx
    id        BYTEA NOT NULL,
    share_id  BYTEA NOT NULL REFERENCES shares(id),
    idx       SMALLINT NOT NULL,
    from_addr BYTEA NOT NULL,
    to_addr   BYTEA NOT NULL,
    amount    NUMERIC(20,0) NOT NULL,
    fee       NUMERIC(20,0) NOT NULL,
    nonce     NUMERIC(20,0) NOT NULL,
    status    TEXT NOT NULL DEFAULT 'pending' -- applied | skipped | orphaned
);
CREATE INDEX IF NOT EXISTS txs_from ON txs (from_addr);
CREATE INDEX IF NOT EXISTS txs_to ON txs (to_addr);
CREATE INDEX IF NOT EXISTS txs_id ON txs (id);

CREATE TABLE IF NOT EXISTS claims (
    uid       BYTEA PRIMARY KEY,           -- share id || idx
    share_id  BYTEA NOT NULL REFERENCES shares(id),
    idx       SMALLINT NOT NULL,
    miner     BYTEA NOT NULL,
    epoch     INTEGER NOT NULL,
    k         BIGINT NOT NULL,
    g         INTEGER NOT NULL,
    p         TEXT NOT NULL,
    merit     REAL NOT NULL,               -- display only, never consensus
    work      BIGINT NOT NULL,
    certified BOOLEAN,
    payable   BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX IF NOT EXISTS claims_miner ON claims (miner);
CREATE INDEX IF NOT EXISTS claims_g ON claims (g DESC);

CREATE TABLE IF NOT EXISTS sci_payouts (
    block_id BYTEA NOT NULL,
    addr     BYTEA NOT NULL,
    amount   NUMERIC(20,0) NOT NULL,
    PRIMARY KEY (block_id, addr)
);

CREATE TABLE IF NOT EXISTS accounts (
    addr    BYTEA PRIMARY KEY,
    balance NUMERIC(20,0) NOT NULL,
    nonce   NUMERIC(20,0) NOT NULL,
    shares  INTEGER NOT NULL,
    blocks  INTEGER NOT NULL,
    earned  NUMERIC(20,0) NOT NULL
);

CREATE TABLE IF NOT EXISTS payouts (
    block_id BYTEA NOT NULL,
    addr     BYTEA NOT NULL,
    amount   NUMERIC(20,0) NOT NULL,
    PRIMARY KEY (block_id, addr)
);
CREATE INDEX IF NOT EXISTS payouts_addr ON payouts (addr);

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS schema_version (
    id      BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (id),
    version INTEGER NOT NULL
);
INSERT INTO schema_version (id, version) VALUES (TRUE, 0)
ON CONFLICT (id) DO NOTHING;

-- Version 1 upgrades the original signed BIGINT monetary columns to the
-- uint64-compatible NUMERIC representation used by the wire protocol. This
-- block is transactional because Store.Open applies the schema in a DB tx.
DO $$
DECLARE v INTEGER;
BEGIN
    SELECT version INTO v FROM schema_version WHERE id = TRUE;
    IF v > 1 THEN
        RAISE EXCEPTION 'unsupported explorer schema version %', v;
    ELSIF v < 1 THEN
        ALTER TABLE txs ALTER COLUMN amount TYPE NUMERIC(20,0) USING amount::numeric;
        ALTER TABLE txs ALTER COLUMN fee TYPE NUMERIC(20,0) USING fee::numeric;
        ALTER TABLE txs ALTER COLUMN nonce TYPE NUMERIC(20,0) USING nonce::numeric;
        ALTER TABLE sci_payouts ALTER COLUMN amount TYPE NUMERIC(20,0) USING amount::numeric;
        ALTER TABLE accounts ALTER COLUMN balance TYPE NUMERIC(20,0) USING balance::numeric;
        ALTER TABLE accounts ALTER COLUMN nonce TYPE NUMERIC(20,0) USING nonce::numeric;
        ALTER TABLE accounts ALTER COLUMN earned TYPE NUMERIC(20,0) USING earned::numeric;
        ALTER TABLE payouts ALTER COLUMN amount TYPE NUMERIC(20,0) USING amount::numeric;
        UPDATE schema_version SET version = 1 WHERE id = TRUE;
    END IF;
END $$;
