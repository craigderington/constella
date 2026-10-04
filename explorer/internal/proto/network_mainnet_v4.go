//go:build mainnet && !protocolv5

package proto

// This profile is compile-time only. Candidate profiles require separate data.
const (
	ShareVersion  = 4
	BlockK        = 6
	Magic         = 0x4d345443
	NetworkMarker = 6
	NetworkHeader = "network_mainnet_v4.h"
)
