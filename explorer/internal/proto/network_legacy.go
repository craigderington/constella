//go:build !protocolv4 && !mainnet

package proto

// This profile is compile-time only. Candidate profiles require separate data.
const (
	ShareVersion  = 3
	BlockK        = 5
	Magic         = 0x33545343
	NetworkMarker = 0
	NetworkHeader = "network_legacy.h"
)
