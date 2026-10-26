package store

import (
	"context"
	"encoding/binary"
	"math"
	"os"
	"reflect"
	"testing"

	"github.com/craigderington/constella/explorer/internal/proto"
)

// The REAL display score must strictly preserve g's ordering throughout the
// protocol range, including adjacent values rather than only common gaps.
func TestClaimMeritOrder(t *testing.T) {
	previous := float32(-1)
	for g := proto.SciGMin; g <= proto.SciGMax; g++ {
		merit := float32(float64(g) / (float64(proto.SciBits) * math.Ln2))
		if merit <= previous {
			t.Fatalf("merit does not increase at gap %d: %g <= %g", g, merit, previous)
		}
		previous = merit
	}
}

func TestTopClaimsRanking(t *testing.T) {
	dsn := os.Getenv("EXPLORER_TEST_DB")
	if dsn == "" {
		t.Skip("EXPLORER_TEST_DB is not set; requires a disposable database")
	}
	ctx := context.Background()
	s, err := Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer s.DB.Close()
	defer func() {
		_, _ = s.DB.ExecContext(ctx, `DROP TABLE IF EXISTS schema_version,payouts,accounts,sci_payouts,claims,txs,meta,shares CASCADE`)
	}()
	if _, err := s.DB.ExecContext(ctx, `TRUNCATE shares,txs,claims,accounts,payouts,sci_payouts,meta RESTART IDENTITY CASCADE`); err != nil {
		t.Fatal(err)
	}
	// Include boundary gaps, repeated merits at different heights, and an
	// orphan with the highest possible merit. These are SQL fixtures, not proofs.
	gaps := []int{proto.SciGMax, proto.SciGMin, proto.SciGMin + 1, 700, 700,
		proto.SciGMax - 1, proto.SciGMax, proto.SciGMin, proto.SciGMax, proto.SciGMax}
	for i, gap := range gaps {
		id := make([]byte, 32)
		binary.LittleEndian.PutUint64(id, uint64(i+1))
		if _, err := s.DB.ExecContext(ctx, `INSERT INTO shares
			(id,raw,height,prev,time,miner,bits,k,tlen,ntx,nsci,is_block,p,on_main)
			VALUES ($1,$1,$2,$1,to_timestamp(($2::integer)::double precision),$1,496,0,4,0,1,false,'2',$3)`,
			id, i+1, i != len(gaps)-1); err != nil {
			t.Fatal(err)
		}
		if _, err := s.DB.ExecContext(ctx, `INSERT INTO claims
			(uid,share_id,idx,miner,epoch,k,g,p,merit,work,certified,payable)
			VALUES ($1,$1,0,$1,0,0,$2,'2',$3,1,true,true)`,
			id, gap, float64(gap)/(float64(proto.SciBits)*math.Ln2)); err != nil {
			t.Fatal(err)
		}
	}
	compare := func() {
		t.Helper()
		for _, limit := range []int{0, 1, 8, proto.SciGMax} {
			want, err := scanClaims(s.DB.QueryContext(ctx, `SELECT `+claimCols+` FROM claims c
				JOIN shares sh ON sh.id=c.share_id WHERE sh.on_main
				ORDER BY c.merit DESC,sh.height DESC LIMIT $1`, limit))
			if err != nil {
				t.Fatal(err)
			}
			got, err := s.TopClaims(ctx, limit)
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("ranking differs from original query at limit %d", limit)
			}
		}
	}
	compare()
	// A reorg changes eligibility immediately; no stale cached leaderboard.
	if _, err := s.DB.ExecContext(ctx, `UPDATE shares SET on_main=NOT on_main WHERE height >= $1`, len(gaps)-2); err != nil {
		t.Fatal(err)
	}
	compare()
	if _, err := s.DB.ExecContext(ctx, `UPDATE shares SET on_main=false`); err != nil {
		t.Fatal(err)
	}
	compare()
}
