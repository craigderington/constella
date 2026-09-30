//go:build protocolv4 && !mainnet

package proto

// This profile is compile-time only. Candidate profiles require separate data.
const (
	ShareVersion  = 4
	BlockK        = 5
	Magic         = 0x54345443
	NetworkMarker = 5
	NetworkHeader = "network_testnet_v4.h"
)
