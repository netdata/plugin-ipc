//go:build windows

package cgroupssnapshot

import (
	"time"

	wintransport "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

const (
	defaultSupportedProfiles = wintransport.ProfileNamedPipe
	defaultPreferredProfiles = wintransport.ProfileNamedPipe
)

type transportClient interface {
	CallMessage(requestMessage []byte, responseMessage []byte, timeout time.Duration) (int, error)
	Close() error
}

func connectTransport(config Config, timeout time.Duration) (transportClient, error) {
	transportConfig := wintransport.NewConfig(config.ServiceNamespace, config.ServiceName)
	transportConfig.SupportedProfiles = config.SupportedProfiles
	transportConfig.PreferredProfiles = config.PreferredProfiles
	transportConfig.MaxRequestPayloadBytes = config.MaxRequestPayloadBytes
	transportConfig.MaxRequestBatchItems = config.MaxRequestBatchItems
	transportConfig.MaxResponsePayloadBytes = config.MaxResponsePayloadBytes
	transportConfig.MaxResponseBatchItems = config.MaxResponseBatchItems
	transportConfig.AuthToken = config.AuthToken
	return wintransport.Dial(transportConfig, timeout)
}
