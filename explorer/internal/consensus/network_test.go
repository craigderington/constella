package consensus

import (
	"errors"
	"testing"

	"github.com/craig/constella/explorer/internal/proto"
)

func TestForeignNetworkDoesNotEnterOrphanPool(t *testing.T) {
	for _, profile := range []struct {
		version uint32
		marker  uint16
	}{{3, 0}, {4, 5}, {4, 6}} {
		if profile.version == proto.ShareVersion && profile.marker == proto.NetworkMarker {
			continue
		}
		c := NewChain()
		s := proto.Share{Version: profile.version, Rsv: profile.marker, Height: 1, Bits: proto.BitsMin}
		s.Prev[0] = 0xf1
		_, missing, err := c.AddAt(&proto.Msg{Share: s}, 0)
		if !errors.Is(err, ErrInvalid) || missing != nil || c.Len() != 1 || c.OrphanCount() != 0 {
			t.Fatal("foreign header entered consensus/orphan state")
		}
	}
}
