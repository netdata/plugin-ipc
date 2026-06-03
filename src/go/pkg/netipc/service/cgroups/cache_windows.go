//go:build windows

package cgroups

import (
	raw "github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw"
)

// NewCache creates a new L3 cache. Does NOT connect.
func NewCache(runDir, serviceName string, config ClientConfig) *Cache {
	return &Cache{inner: raw.NewCache(runDir, serviceName, clientConfigToTransport(config))}
}
