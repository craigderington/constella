package p2p

// EdDSA over Curve25519 with BLAKE2b — the signature scheme the C node uses
// for node identities, wallet keys and transactions (monocypher's
// crypto_eddsa_sign / crypto_eddsa_check).
//
// THIS IS NOT RFC 8032 Ed25519 AND crypto/ed25519 CANNOT VERIFY IT.
// The construction is RFC 8032's with exactly one substitution: every one of
// the three hashes is BLAKE2b-512 instead of SHA-512. Mixing the two fails
// 100% of the time in both directions, and because both ends of a live
// handshake would share the mistake, only the pinned vector in eddsa_test.go
// catches it. See task-5-report.md §2, "Signature algorithm".
//
//	H            = BLAKE2b-512
//	h            = H(seed)                    64 bytes
//	a            = clamp(h[0:32])             a[0] &= 248; a[31] &= 127; a[31] |= 64
//	prefix       = h[32:64]                   used untrimmed
//	A  (pub key) = compress(a * B)
//	r            = H(prefix || M)    mod L    deterministic nonce
//	R            = compress(r * B)
//	k            = H(R || A || M)    mod L
//	S            = (r + k * a)       mod L
//	signature    = R || S                     64 bytes
//
// L = 2^252 + 27742317777372353535851937790883648493, which is the order
// filippo.io/edwards25519's Scalar type reduces modulo, so it is never written
// out here. Signing is deterministic: the nonce is a hash of the prefix and
// the message, so a given (seed, message) has exactly one valid signature.

import (
	"crypto/subtle"
	"errors"
	"github.com/craig/constella/explorer/internal/signature"

	"filippo.io/edwards25519"
	"golang.org/x/crypto/blake2b"
)

const (
	seedSize      = 32
	pubKeySize    = 32
	signatureSize = 64
)

// hash512 is the H of the construction above: BLAKE2b with a 512-bit digest
// and no key. Keyed BLAKE2b-256 is a different primitive used by the KDF; the
// two must not be confused.
func hash512(parts ...[]byte) []byte {
	h, err := blake2b.New512(nil)
	if err != nil {
		panic(err) // New512(nil) cannot fail
	}
	for _, p := range parts {
		_, _ = h.Write(p)
	}
	return h.Sum(nil)
}

// identity is a node's static EdDSA-BLAKE2b key pair, expanded once from its
// 32-byte seed. The seed is what src/wallet.c stores in node.key.
type identity struct {
	scalar *edwards25519.Scalar // a
	prefix []byte               // h[32:64]
	pub    []byte               // A, compressed
}

func newIdentity(seed []byte) (*identity, error) {
	if len(seed) != seedSize {
		return nil, errors.New("eddsa: seed must be 32 bytes")
	}
	h := hash512(seed)
	// SetBytesWithClamping applies exactly the clamp above and then reduces
	// the little-endian result modulo L.
	a, err := edwards25519.NewScalar().SetBytesWithClamping(h[:32])
	if err != nil {
		return nil, err
	}
	pub := new(edwards25519.Point).ScalarBaseMult(a).Bytes()
	return &identity{scalar: a, prefix: h[32:64], pub: pub}, nil
}

// sign returns R || S over msg. Deterministic.
func (id *identity) sign(msg []byte) []byte {
	// r = H(prefix || M) mod L. SetUniformBytes takes the 64-byte digest as a
	// little-endian integer and reduces it, which is the RFC's "mod L".
	r, err := edwards25519.NewScalar().SetUniformBytes(hash512(id.prefix, msg))
	if err != nil {
		panic(err) // a 64-byte input cannot fail
	}
	R := new(edwards25519.Point).ScalarBaseMult(r).Bytes()

	k, err := edwards25519.NewScalar().SetUniformBytes(hash512(R, id.pub, msg))
	if err != nil {
		panic(err)
	}
	// S = k*a + r mod L
	S := edwards25519.NewScalar().MultiplyAdd(k, id.scalar, r)

	sig := make([]byte, 0, signatureSize)
	sig = append(sig, R...)
	return append(sig, S.Bytes()...)
}

// Peer identities always require non-small-order keys, including on legacy networks.
func eddsaVerify(pub, sig, msg []byte) bool {
	return signature.Check(pub, sig, msg, true)
}

// isZero reports whether b is all zero, in constant time.
func isZero(b []byte) bool {
	zero := make([]byte, len(b))
	return subtle.ConstantTimeCompare(b, zero) == 1
}
