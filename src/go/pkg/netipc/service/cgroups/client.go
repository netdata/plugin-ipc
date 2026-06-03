//go:build unix

package cgroups

import (
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

func clientConfigToTransport(config ClientConfig) posix.ClientConfig {
	return posix.ClientConfig{
		SupportedProfiles:       config.SupportedProfiles,
		PreferredProfiles:       config.PreferredProfiles,
		MaxRequestBatchItems:    config.MaxRequestBatchItems,
		MaxResponsePayloadBytes: config.MaxResponsePayloadBytes,
		MaxResponseBatchItems:   config.MaxRequestBatchItems,
		AuthToken:               config.AuthToken,
	}
}

func serverConfigToTransport(config ServerConfig) posix.ServerConfig {
	return posix.ServerConfig{
		SupportedProfiles:       config.SupportedProfiles,
		PreferredProfiles:       config.PreferredProfiles,
		MaxRequestBatchItems:    config.MaxRequestBatchItems,
		MaxResponsePayloadBytes: config.MaxResponsePayloadBytes,
		MaxResponseBatchItems:   config.MaxRequestBatchItems,
		AuthToken:               config.AuthToken,
	}
}
