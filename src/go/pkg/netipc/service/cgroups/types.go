// Package cgroups provides L2 orchestration and L3 caching for the
// cgroups snapshot service.
//
// Pure composition of L1 transport + Codec. No direct socket/pipe calls.
// Client manages connection lifecycle with at-least-once retry.
// Server handles accept, read, dispatch, respond.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.
package cgroups

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
//  Server handler (shared across platforms)
// ---------------------------------------------------------------------------

// HandlerFunc is the server handler callback. Receives (methodCode,
// requestPayload). Returns (responsePayload, ok). If ok is false,
// transport_status = INTERNAL_ERROR with empty payload.
type HandlerFunc func(methodCode uint16, request []byte) ([]byte, bool)

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
}
