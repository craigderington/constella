// Package signature implements the node's EdDSA-BLAKE2b verification policy.
package signature

import (
	"filippo.io/edwards25519"
	"golang.org/x/crypto/blake2b"
)

func hash512(parts ...[]byte) []byte {
	h, _ := blake2b.New512(nil)
	for _, p := range parts {
		_, _ = h.Write(p)
	}
	return h.Sum(nil)
}

// Check preserves Monocypher's cofactored equation and encoding rules.
// strict additionally rejects every small-order public key, including
// noncanonical encodings. Enable it for P2P and v5 transaction consensus.
func Check(pub, sig, msg []byte, strict bool) bool {
	if len(pub) != 32 || len(sig) != 64 {
		return false
	}
	A, err := new(edwards25519.Point).SetBytes(pub)
	if err != nil {
		return false
	}
	if strict && new(edwards25519.Point).MultByCofactor(A).Equal(edwards25519.NewIdentityPoint()) == 1 {
		return false
	}
	R, err := new(edwards25519.Point).SetBytes(sig[:32])
	if err != nil {
		return false
	}
	S, err := edwards25519.NewScalar().SetCanonicalBytes(sig[32:])
	if err != nil {
		return false // S >= L
	}
	k, err := edwards25519.NewScalar().SetUniformBytes(hash512(sig[:32], pub, msg))
	if err != nil {
		return false
	}
	// sum = [S]B - [k]A
	minusA := new(edwards25519.Point).Negate(A)
	sum := new(edwards25519.Point).VarTimeDoubleScalarBaseMult(k, minusA, S)
	// [8](sum - R) == identity
	diff := new(edwards25519.Point).Subtract(sum, R)
	diff.MultByCofactor(diff)
	return diff.Equal(edwards25519.NewIdentityPoint()) == 1
}
