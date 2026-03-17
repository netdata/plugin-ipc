// Package cgroups provides L2 orchestration and L3 caching for the
// cgroups snapshot service.
//
// Pure composition of L1 transport + Codec. No direct socket/pipe calls.
// Client manages connection lifecycle with at-least-once retry.
// Server handles accept, read, dispatch, respond.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.
package cgroups

import "github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"

// Poll/receive timeout for server loops (ms). Controls shutdown detection latency.
const serverPollTimeoutMs = 100

// ---------------------------------------------------------------------------
//  Client state (shared across platforms)
// ---------------------------------------------------------------------------

// ClientState represents the connection state machine.
type ClientState int

const (
	StateDisconnected ClientState = iota
	StateConnecting
	StateReady
	StateNotFound
	StateAuthFailed
	StateIncompatible
	StateBroken
)

// ClientStatus is a diagnostic counters snapshot.
type ClientStatus struct {
	State          ClientState
	ConnectCount   uint32
	ReconnectCount uint32
	CallCount      uint32
	ErrorCount     uint32
}

// ---------------------------------------------------------------------------
//  Typed server handlers (shared across platforms)
// ---------------------------------------------------------------------------

// Handlers defines the public typed callback surface for the managed server.
//
// All callbacks are optional. A nil callback means the method is unsupported
// and should fail with INTERNAL_ERROR if requested.
type Handlers struct {
	OnIncrement     func(uint64) (uint64, bool)
	OnStringReverse func(string) (string, bool)
	OnSnapshot      func(*protocol.CgroupsRequest, *protocol.CgroupsBuilder) bool

	// SnapshotMaxItems optionally caps the number of snapshot items the
	// internal builder reserves directory space for. When zero, the library
	// derives a safe upper bound from the negotiated response buffer size.
	SnapshotMaxItems uint32
}

func (h Handlers) snapshotMaxItems(responseBufSize int) uint32 {
	if h.SnapshotMaxItems != 0 {
		return h.SnapshotMaxItems
	}
	return protocol.EstimateCgroupsMaxItems(responseBufSize)
}

// ---------------------------------------------------------------------------
//  L3 cache types (shared across platforms)
// ---------------------------------------------------------------------------

// Default response buffer size for L3 cache refresh.
const cacheResponseBufSize = 65536

// CacheItem is an owned copy of a single cgroup item.
// Built from ephemeral L2 views during cache construction.
type CacheItem struct {
	Hash    uint32
	Options uint32
	Enabled uint32
	Name    string // owned copy
	Path    string // owned copy
}

// CacheStatus is a diagnostic snapshot for the L3 cache.
type CacheStatus struct {
	Populated           bool
	ItemCount           uint32
	SystemdEnabled      uint32
	Generation          uint64
	RefreshSuccessCount uint32
	RefreshFailureCount uint32
	ConnectionState     ClientState // underlying L2 client state
	LastRefreshTs       int64       // monotonic timestamp (ms) of last successful refresh, 0 if never
}
