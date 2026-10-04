//go:build protocolv5 && !mainnet

package proto

// Isolated strict-signature candidate; requires its own database and node data.
const (
	ShareVersion  = 5
	BlockK        = 5
	Magic         = 0x54355443
	NetworkMarker = 5
	NetworkHeader = "network_testnet_v5.h"
)
