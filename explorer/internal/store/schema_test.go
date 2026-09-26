package store

import (
	"context"
	"database/sql"
	"os"
	"strings"
	"testing"

	_ "github.com/lib/pq"
)

func TestMigratesVersionZeroNumericColumns(t *testing.T) {
	dsn := os.Getenv("EXPLORER_TEST_DB")
	if dsn == "" {
		t.Skip("EXPLORER_TEST_DB is not set")
	}
	ctx := context.Background()
	db, err := sql.Open("postgres", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	t.Cleanup(func() {
		_, _ = db.ExecContext(context.Background(), `DROP TABLE IF EXISTS schema_version, payouts, accounts,
			sci_payouts, claims, txs, meta, shares CASCADE`)
	})
	if _, err := db.ExecContext(ctx, `DROP TABLE IF EXISTS schema_version, payouts, accounts,
		sci_payouts, claims, txs, meta, shares CASCADE`); err != nil {
		t.Fatal(err)
	}
	old := strings.ReplaceAll(strings.Split(string(schema), "CREATE TABLE IF NOT EXISTS schema_version")[0],
		"NUMERIC(20,0)", "BIGINT")
	if _, err := db.ExecContext(ctx, old); err != nil {
		t.Fatal(err)
	}
	if _, err := db.ExecContext(ctx, `INSERT INTO shares
		(id, raw, height, prev, time, miner, bits, k, tlen, ntx, nsci, is_block, p)
		VALUES ($1,$2,1,$3,now(),$4,384,0,4,0,0,true,'2')`,
		[]byte{1}, []byte{2}, []byte{3}, []byte{4}); err != nil {
		t.Fatal(err)
	}
	if _, err := db.ExecContext(ctx, `INSERT INTO txs
		(uid, id, share_id, idx, from_addr, to_addr, amount, fee, nonce)
		VALUES ($1,$2,$3,0,$4,$5,$6,$7,$8)`,
		[]byte{9}, []byte{10}, []byte{1}, []byte{11}, []byte{12},
		int64(9223372036854775807), int64(1), int64(0)); err != nil {
		t.Fatal(err)
	}
	st, err := Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer st.DB.Close()
	var version int
	if err := st.DB.QueryRowContext(ctx, `SELECT version FROM schema_version WHERE id = TRUE`).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != 1 {
		t.Fatalf("schema version = %d, want 1", version)
	}
	var typ string
	if err := st.DB.QueryRowContext(ctx, `SELECT data_type FROM information_schema.columns
		WHERE table_name = 'txs' AND column_name = 'amount'`).Scan(&typ); err != nil {
		t.Fatal(err)
	}
	if typ != "numeric" {
		t.Fatalf("tx amount type = %q, want numeric", typ)
	}
	var amount string
	if err := st.DB.QueryRowContext(ctx, `SELECT amount::text FROM txs WHERE uid = $1`, []byte{9}).Scan(&amount); err != nil {
		t.Fatal(err)
	}
	if amount != "9223372036854775807" {
		t.Fatalf("migrated amount = %s", amount)
	}
}
