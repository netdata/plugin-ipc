package cgroupssnapshot

import (
	"errors"
	"fmt"
	"syscall"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const (
	NameMaxBytes                   uint32 = 255
	PathMaxBytes                   uint32 = 4096
	DefaultMaxResponsePayloadBytes        = protocol.CgroupsSnapshotItemHeaderLen + NameMaxBytes + 1 + PathMaxBytes + 1
	DefaultMaxResponseBatchItems   uint32 = 1000
)

type Config struct {
	ServiceNamespace        string
	ServiceName             string
	SupportedProfiles       uint32
	PreferredProfiles       uint32
	MaxRequestPayloadBytes  uint32
	MaxRequestBatchItems    uint32
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	AuthToken               uint64
}

type CacheItem struct {
	Hash    uint32
	Options uint32
	Enabled bool
	Name    string
	Path    string
}

type Cache struct {
	Generation     uint64
	SystemdEnabled bool
	Items          []CacheItem
}

type Client struct {
	config          Config
	transport       transportClient
	nextMessageID   uint64
	responseMessage []byte
	cache           Cache
}

var connectTransportFunc = connectTransport

func NewConfig(serviceNamespace, serviceName string) Config {
	return Config{
		ServiceNamespace:       serviceNamespace,
		ServiceName:            serviceName,
		MaxRequestPayloadBytes: protocol.CgroupsSnapshotRequestPayloadLen,
		MaxRequestBatchItems:   1,
	}
}

func NewClient(config Config) (*Client, error) {
	if config.ServiceNamespace == "" || config.ServiceName == "" {
		return nil, errors.New("service_namespace and service_name must be set")
	}
	if config.SupportedProfiles == 0 ||
		config.PreferredProfiles == 0 ||
		config.MaxResponsePayloadBytes == 0 ||
		config.MaxResponseBatchItems == 0 ||
		config.AuthToken == 0 {
		return nil, errors.New("auth_token, supported_profiles, preferred_profiles, max_response_payload_bytes, and max_response_batch_items must be set explicitly")
	}

	responseCapacity, err := protocol.MaxBatchTotalSize(
		effectiveResponsePayloadLimit(config.MaxResponsePayloadBytes),
		effectiveResponseBatchLimit(config.MaxResponseBatchItems),
	)
	if err != nil {
		return nil, err
	}

	return &Client{
		config:          config,
		nextMessageID:   1,
		responseMessage: make([]byte, responseCapacity),
	}, nil
}

func (c *Client) Refresh(timeout time.Duration) error {
	if c == nil {
		return errors.New("nil cgroups snapshot client")
	}
	if err := c.ensureTransport(timeout); err != nil {
		return err
	}

	messageID := c.nextMessageID
	c.nextMessageID++

	requestPayload := protocol.EncodeCgroupsSnapshotRequestPayload(protocol.CgroupsSnapshotRequest{Flags: 0})
	header, err := protocol.EncodeMessageHeader(protocol.MessageHeader{
		Magic:           protocol.MessageMagic,
		Version:         protocol.MessageVersion,
		HeaderLen:       protocol.MessageHeaderLen,
		Kind:            protocol.MessageKindRequest,
		Flags:           0,
		Code:            protocol.MethodCgroupsSnapshot,
		TransportStatus: protocol.TransportStatusOK,
		PayloadLen:      protocol.CgroupsSnapshotRequestPayloadLen,
		ItemCount:       1,
		MessageID:       messageID,
	})
	if err != nil {
		return err
	}

	requestMessage := make([]byte, 0, protocol.MessageHeaderLen+len(requestPayload))
	requestMessage = append(requestMessage, header[:]...)
	requestMessage = append(requestMessage, requestPayload[:]...)

	responseLen, err := c.transport.CallMessage(requestMessage, c.responseMessage, timeout)
	if err != nil {
		c.disconnectTransport()
		return err
	}

	response := c.responseMessage[:responseLen]
	decodedHeader, err := protocol.DecodeMessageHeader(response)
	if err != nil {
		c.disconnectTransport()
		return err
	}
	if decodedHeader.Kind != protocol.MessageKindResponse ||
		decodedHeader.Code != protocol.MethodCgroupsSnapshot ||
		decodedHeader.MessageID != messageID ||
		decodedHeader.Flags != protocol.MessageFlagBatch {
		c.disconnectTransport()
		return errors.New("invalid cgroups snapshot response envelope")
	}
	if decodedHeader.TransportStatus != protocol.TransportStatusOK {
		c.disconnectTransport()
		return transportStatusErr(decodedHeader.TransportStatus)
	}

	total, err := protocol.MessageTotalSize(decodedHeader)
	if err != nil {
		c.disconnectTransport()
		return err
	}
	if total != responseLen {
		c.disconnectTransport()
		return errors.New("response length does not match envelope")
	}

	view, err := protocol.DecodeCgroupsSnapshotView(response[protocol.MessageHeaderLen:], decodedHeader.ItemCount)
	if err != nil {
		c.disconnectTransport()
		return err
	}

	cache := Cache{
		Generation:     view.Generation,
		SystemdEnabled: view.SystemdEnabled,
		Items:          make([]CacheItem, 0, view.ItemCount),
	}
	for index := uint32(0); index < view.ItemCount; index++ {
		item, err := view.ItemViewAt(index)
		if err != nil {
			c.disconnectTransport()
			return err
		}
		cache.Items = append(cache.Items, CacheItem{
			Hash:    item.Hash,
			Options: item.Options,
			Enabled: item.Enabled,
			Name:    item.NameView.CopyString(),
			Path:    item.PathView.CopyString(),
		})
	}

	c.cache = cache
	return nil
}

func (c *Client) Cache() *Cache {
	if c == nil {
		return nil
	}
	return &c.cache
}

func (c *Client) Close() error {
	if c == nil {
		return nil
	}
	if c.transport == nil {
		return nil
	}
	err := c.transport.Close()
	c.transport = nil
	return err
}

func (c *Client) Lookup(hash uint32, name string) *CacheItem {
	if c == nil {
		return nil
	}
	for i := range c.cache.Items {
		if c.cache.Items[i].Hash == hash && c.cache.Items[i].Name == name {
			return &c.cache.Items[i]
		}
	}
	return nil
}

func (c *Client) ensureTransport(timeout time.Duration) error {
	if c.transport != nil {
		return nil
	}
	transport, err := connectTransportFunc(c.config, timeout)
	if err != nil {
		return err
	}
	c.transport = transport
	return nil
}

func (c *Client) disconnectTransport() {
	if c != nil {
		_ = c.Close()
	}
}

func effectiveResponsePayloadLimit(configured uint32) uint32 {
	if configured != 0 {
		return configured
	}
	return DefaultMaxResponsePayloadBytes
}

func effectiveResponseBatchLimit(configured uint32) uint32 {
	if configured != 0 {
		return configured
	}
	return DefaultMaxResponseBatchItems
}

func transportStatusErr(status uint16) error {
	switch status {
	case protocol.TransportStatusOK:
		return nil
	case protocol.TransportStatusAuthFailed:
		return syscall.EACCES
	case protocol.TransportStatusLimitExceeded:
		return syscall.EMSGSIZE
	case protocol.TransportStatusUnsupported:
		return syscall.ENOTSUP
	case protocol.TransportStatusInternalError:
		return syscall.EIO
	case protocol.TransportStatusBadEnvelope, protocol.TransportStatusIncompatible:
		return syscall.EPROTO
	default:
		return fmt.Errorf("unexpected transport status %d", status)
	}
}
