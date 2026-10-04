//go:build protocolv5 && mainnet

package proto

// Isolated strict-signature candidate; requires its own database and node data.
const (
	ShareVersion  = 5
	BlockK        = 6
	Magic         = 0x4d355443
	NetworkMarker = 6
	NetworkHeader = "network_mainnet_v5.h"
)
