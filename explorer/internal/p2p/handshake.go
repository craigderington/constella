package p2p

// The static-key handshake, mirroring src/net.c. Normative description:
// .superpowers/sdd/2026-09-26-peer-discovery/task-5-report.md §2, which
// supersedes docs/protocol.md until Task 11 updates it.
//
// Two phases, separated by message type rather than by arrival order, and
// symmetric: there is no initiator and no responder, both ends run this exact
// code.
//
//	MSG_AUTH  (9)  len 64   eph_pub[32] || id_pub[32]   sent on connect
//	MSG_AUTH2 (12) len 64   sig[64]                     sent on the peer's phase 1
//
// The spec's single-message form (design doc line 82) is unbuildable: the side
// that transmits first cannot sign an ephemeral key it has not yet seen.

import (
	"crypto/rand"
	"errors"
	"io"
	"net"
	"time"

	"github.com/craig/constella/explorer/internal/proto"
	"golang.org/x/crypto/curve25519"
)

// hsLabel is the 72-byte transcript's 8-byte ASCII prefix, no NUL.
const hsLabel = "CSTL-HS1"

// kdfLabel is the session-key derivation label, 9 ASCII bytes, no NUL. It is
// deliberately NOT "CSTL-P2P1": that was the superseded PSK schedule and the
// two must never collide.
const kdfLabel = "CSTL-P2P2"

// hsTranscript is the 72 bytes a side signs:
//
//	"CSTL-HS1" || eph_self (32) || eph_peer (32)
//
// eph_self is the signer's own ephemeral key, so the two ends sign different
// strings and each verifies the peer's with the operands swapped.
func hsTranscript(ephSelf, ephPeer []byte) []byte {
	t := make([]byte, 0, len(hsLabel)+64)
	t = append(t, hsLabel...)
	t = append(t, ephSelf...)
	return append(t, ephPeer...)
}

// hsSessionKeys derives both direction keys from the raw 32-byte X25519 output
// and the two raw 32-byte EdDSA-BLAKE2b public keys as they travelled in
// phase 1 — NOT BLAKE2b-256(pubkey), which is the address-store ID and a
// different thing.
//
//	k_lo = BLAKE2b-256-keyed(shared, "CSTL-P2P2" || "lo" || lo_id || hi_id)
//	k_hi = BLAKE2b-256-keyed(shared, "CSTL-P2P2" || "hi" || lo_id || hi_id)
//
// The message is 9 + 2 + 32 + 32 = 75 bytes; the key is the shared secret.
//
// selfIsLo — whether idSelf sorted lower, and so transmits under k_lo — is
// returned rather than recomputed by the caller. The sort decides both the
// hash input order and the direction split, and those two must never be able
// to disagree: one fact, one place.
func hsSessionKeys(shared, idSelf, idPeer []byte) (kLo, kHi []byte, selfIsLo bool) {
	selfIsLo = string(idSelf) < string(idPeer)
	lo, hi := idSelf, idPeer
	if !selfIsLo {
		lo, hi = idPeer, idSelf
	}
	return keyed(shared, []byte(kdfLabel), []byte("lo"), lo, hi),
		keyed(shared, []byte(kdfLabel), []byte("hi"), lo, hi),
		selfIsLo
}

// hsDerive completes the key schedule. The side whose id_pub sorts lower
// transmits under k_lo and receives under k_hi; the other side is the mirror.
//
// Rejection 3: an all-zero shared secret. curve25519.X25519 already returns an
// error for a low-order input, which covers every point that produces an
// all-zero output; the explicit isZero check mirrors the C node's own guard
// and is kept so the two implementations reject on the same stated condition
// rather than on a library's discretion.
func hsDerive(ephSk, ephPeer, idSelf, idPeer []byte) (*session, error) {
	shared, err := curve25519.X25519(ephSk, ephPeer)
	if err != nil {
		return nil, errors.New("peer sent a low-order ephemeral key")
	}
	if isZero(shared) {
		return nil, errors.New("peer sent a low-order ephemeral key")
	}
	kLo, kHi, selfIsLo := hsSessionKeys(shared, idSelf, idPeer)
	txKey, rxKey := kLo, kHi
	if !selfIsLo {
		txKey, rxKey = kHi, kLo
	}
	return newSessionKeys(txKey, rxKey)
}

// readHS reads one plaintext handshake frame and checks its type and length.
func readHS(r io.Reader, want byte) ([]byte, error) {
	typ, p, err := readRaw(r)
	if err != nil {
		return nil, err
	}
	if typ != want || len(p) != 64 {
		return nil, errors.New("unexpected handshake frame")
	}
	return p, nil
}

// staticHandshake runs both phases on conn and returns the live session. id is
// this side's static identity; it is stable for the life of the process, so
// the node sees the same peer across reconnects.
func staticHandshake(conn net.Conn, r io.Reader, id *identity) (*session, error) {
	// Without a deadline a hung or hostile peer parks Run here forever and
	// the reconnect loop never fires.
	if err := conn.SetDeadline(time.Now().Add(10 * time.Second)); err != nil {
		return nil, err
	}
	defer conn.SetDeadline(time.Time{})

	ephSk := make([]byte, 32)
	if _, err := rand.Read(ephSk); err != nil {
		return nil, err
	}
	ephPub, err := curve25519.X25519(ephSk, curve25519.Basepoint)
	if err != nil {
		return nil, err
	}

	// Phase 1 out.
	auth := make([]byte, 0, 64)
	auth = append(auth, ephPub...)
	auth = append(auth, id.pub...)
	if err := proto.WriteFrame(conn, proto.MsgAuth, auth); err != nil {
		return nil, err
	}

	// Phase 1 in.
	peerAuth, err := readHS(r, proto.MsgAuth)
	if err != nil {
		return nil, err
	}
	peerEph, peerID := peerAuth[:32], peerAuth[32:]

	// Rejection 1, at phase 1, before our phase 2 goes out. Identical
	// identities make lo_id == hi_id, so both ends would name the same key
	// "lo" and encrypt from nonce 0 with it — the one thing the AEAD
	// construction cannot survive.
	if equal(peerID, id.pub) {
		return nil, errors.New("peer presented our own identity")
	}

	// Phase 2 out: our signature over our own transcript.
	if err := proto.WriteFrame(conn, proto.MsgAuth2, id.sign(hsTranscript(ephPub, peerEph))); err != nil {
		return nil, err
	}

	// Phase 2 in.
	sig, err := readHS(r, proto.MsgAuth2)
	if err != nil {
		return nil, err
	}
	// Rejection 2, at phase 2. EdDSA-BLAKE2b, not crypto/ed25519.
	if !eddsaVerify(peerID, sig, hsTranscript(peerEph, ephPub)) {
		return nil, errors.New("peer signature failed verification")
	}
	// Rejection 3, after the signature check.
	return hsDerive(ephSk, peerEph, id.pub, peerID)
}
