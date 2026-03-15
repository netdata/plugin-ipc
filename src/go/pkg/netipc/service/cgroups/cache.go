//go:build unix

// L3: Client-side cgroups snapshot cache.
//
// Wraps an L2 client and maintains a local owned copy of the most
// recent successful snapshot. Lookup by hash+name is pure in-memory
// with no I/O.
//
// On refresh failure, the previous cache is preserved. The cache
// is empty only if no successful refresh has ever occurred.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.
package cgroups

import (
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

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

// Cache is an L3 client-side cgroups snapshot cache.
type Cache struct {
	client *Client
	items  []CacheItem

	systemdEnabled      uint32
	generation          uint64
	populated           bool
	refreshSuccessCount uint32
	refreshFailureCount uint32

	responseBuf []byte
}

// NewCache creates a new L3 cache. Creates the underlying L2 client
// context. Does NOT connect. Does NOT require the server to be running.
// Cache starts empty (populated == false).
func NewCache(runDir, serviceName string, config posix.ClientConfig) *Cache {
	return &Cache{
		client:      NewClient(runDir, serviceName, config),
		responseBuf: make([]byte, cacheResponseBufSize),
	}
}

// Refresh drives the L2 client (connect/reconnect as needed) and
// requests a fresh snapshot. On success, rebuilds the local cache.
// On failure, preserves the previous cache.
//
// Returns true if the cache was updated.
func (c *Cache) Refresh() bool {
	// Drive L2 connection lifecycle
	c.client.Refresh()

	// Attempt snapshot call
	view, err := c.client.CallSnapshot(c.responseBuf)
	if err != nil {
		// Refresh failed: preserve previous cache
		c.refreshFailureCount++
		return false
	}

	// Build new cache from snapshot view
	newItems := make([]CacheItem, 0, view.ItemCount)
	for i := uint32(0); i < view.ItemCount; i++ {
		iv, ierr := view.Item(i)
		if ierr != nil {
			// Malformed item: abort, preserve old cache
			c.refreshFailureCount++
			return false
		}
		newItems = append(newItems, CacheItem{
			Hash:    iv.Hash,
			Options: iv.Options,
			Enabled: iv.Enabled,
			Name:    iv.Name.String(),
			Path:    iv.Path.String(),
		})
	}

	// Replace old cache
	c.items = newItems
	c.systemdEnabled = view.SystemdEnabled
	c.generation = view.Generation
	c.populated = true
	c.refreshSuccessCount++

	return true
}

// Ready returns true if at least one successful refresh has occurred.
// Cheap cached boolean. No I/O, no syscalls.
//
// Note: ready means "has cached data", not "is connected."
func (c *Cache) Ready() bool {
	return c.populated
}

// Lookup finds a cached item by hash + name. Pure in-memory, no I/O.
// Returns the cached item and true, or a zero CacheItem and false
// if not found or cache is empty.
func (c *Cache) Lookup(hash uint32, name string) (CacheItem, bool) {
	if !c.populated {
		return CacheItem{}, false
	}
	for i := range c.items {
		if c.items[i].Hash == hash && c.items[i].Name == name {
			return c.items[i], true
		}
	}
	return CacheItem{}, false
}

// Status returns a diagnostic snapshot for the L3 cache.
func (c *Cache) Status() CacheStatus {
	return CacheStatus{
		Populated:           c.populated,
		ItemCount:           uint32(len(c.items)),
		SystemdEnabled:      c.systemdEnabled,
		Generation:          c.generation,
		RefreshSuccessCount: c.refreshSuccessCount,
		RefreshFailureCount: c.refreshFailureCount,
	}
}

// Close frees all cached items and closes the L2 client.
func (c *Cache) Close() {
	c.items = nil
	c.populated = false
	c.client.Close()
}
