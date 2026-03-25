//go:build windows

// L3: Client-side cgroups snapshot cache (Windows).
//
// Identical cache logic as the POSIX version. Uses Windows Client.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.

package raw

import (
	"time"

	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

// cacheKey is the composite key for O(1) hash+name lookup.
type cacheKey struct {
	hash uint32
	name string
}

// Cache is an L3 client-side cgroups snapshot cache.
type Cache struct {
	client *Client
	items  []CacheItem
	// O(1) lookup index: (hash, name) -> index into items slice
	lookupIndex map[cacheKey]int

	systemdEnabled      uint32
	generation          uint64
	populated           bool
	refreshSuccessCount uint32
	refreshFailureCount uint32
	epoch               time.Time
	lastRefreshTs       int64
}

// NewCache creates a new L3 cache.
func NewCache(runDir, serviceName string, config windows.ClientConfig) *Cache {
	return &Cache{
		client: NewSnapshotClient(runDir, serviceName, config),
		epoch:  time.Now(),
	}
}

// Refresh drives the L2 client and requests a fresh snapshot.
func (c *Cache) Refresh() bool {
	c.client.Refresh()

	view, err := c.client.CallSnapshot()
	if err != nil {
		c.refreshFailureCount++
		return false
	}

	newItems := make([]CacheItem, 0, view.ItemCount)
	for i := uint32(0); i < view.ItemCount; i++ {
		iv, ierr := view.Item(i)
		if ierr != nil {
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

	// Rebuild lookup index
	idx := make(map[cacheKey]int, len(newItems))
	for i := range newItems {
		idx[cacheKey{hash: newItems[i].Hash, name: newItems[i].Name}] = i
	}

	c.items = newItems
	c.lookupIndex = idx
	c.systemdEnabled = view.SystemdEnabled
	c.generation = view.Generation
	c.populated = true
	c.refreshSuccessCount++
	c.lastRefreshTs = time.Since(c.epoch).Milliseconds()

	return true
}

// Ready returns true if at least one successful refresh has occurred.
func (c *Cache) Ready() bool {
	return c.populated
}

// Lookup finds a cached item by hash + name. O(1) via map. No I/O.
func (c *Cache) Lookup(hash uint32, name string) (CacheItem, bool) {
	if !c.populated {
		return CacheItem{}, false
	}
	if i, ok := c.lookupIndex[cacheKey{hash: hash, name: name}]; ok {
		return c.items[i], true
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
		ConnectionState:     c.client.state,
		LastRefreshTs:       c.lastRefreshTs,
	}
}

// Close frees all cached items and closes the L2 client.
func (c *Cache) Close() {
	c.items = nil
	c.lookupIndex = nil
	c.populated = false
	c.client.Close()
}
