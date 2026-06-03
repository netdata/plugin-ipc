//go:build windows

package cgroups

import (
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

func clientConfigToTransport(config ClientConfig) windows.ClientConfig {
	return windows.ClientConfig{
		SupportedProfiles:       config.SupportedProfiles,
		PreferredProfiles:       config.PreferredProfiles,
		MaxRequestBatchItems:    config.MaxRequestBatchItems,
		MaxResponsePayloadBytes: config.MaxResponsePayloadBytes,
		MaxResponseBatchItems:   config.MaxRequestBatchItems,
		AuthToken:               config.AuthToken,
	}
}

func serverConfigToTransport(config ServerConfig) windows.ServerConfig {
	return windows.ServerConfig{
		SupportedProfiles:       config.SupportedProfiles,
		PreferredProfiles:       config.PreferredProfiles,
		MaxRequestBatchItems:    config.MaxRequestBatchItems,
		MaxResponsePayloadBytes: config.MaxResponsePayloadBytes,
		MaxResponseBatchItems:   config.MaxRequestBatchItems,
		AuthToken:               config.AuthToken,
	}
}
