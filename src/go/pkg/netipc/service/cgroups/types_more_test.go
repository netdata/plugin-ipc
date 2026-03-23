package cgroups

import (
	"testing"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

func TestHandlersSnapshotMaxItems(t *testing.T) {
	h := Handlers{}

	got := h.snapshotMaxItems(4096)
	want := protocol.EstimateCgroupsMaxItems(4096)
	if got != want {
		t.Fatalf("snapshotMaxItems default = %d, want %d", got, want)
	}

	h.SnapshotMaxItems = 7
	if got := h.snapshotMaxItems(4096); got != 7 {
		t.Fatalf("snapshotMaxItems override = %d, want 7", got)
	}
}
