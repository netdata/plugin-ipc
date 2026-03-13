//go:build unix

package cgroupssnapshot

import (
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

const (
	defaultSupportedProfiles = posix.ProfileUDSSeqpacket
	defaultPreferredProfiles = posix.ProfileUDSSeqpacket
)

type transportClient interface {
	CallMessage(requestMessage []byte, responseMessage []byte, timeout time.Duration) (int, error)
	Close() error
}

func connectTransport(config Config, timeout time.Duration) (transportClient, error) {
	transportConfig := posix.NewConfig(config.ServiceNamespace, config.ServiceName)
	transportConfig.SupportedProfiles = config.SupportedProfiles
	transportConfig.PreferredProfiles = config.PreferredProfiles
	transportConfig.MaxRequestPayloadBytes = config.MaxRequestPayloadBytes
	transportConfig.MaxRequestBatchItems = config.MaxRequestBatchItems
	transportConfig.MaxResponsePayloadBytes = config.MaxResponsePayloadBytes
	transportConfig.MaxResponseBatchItems = config.MaxResponseBatchItems
	transportConfig.AuthToken = config.AuthToken
	return posix.Dial(transportConfig, timeout)
}
