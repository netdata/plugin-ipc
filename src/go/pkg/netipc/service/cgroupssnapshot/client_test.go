package cgroupssnapshot

import (
	"errors"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

type fakeTransportStep struct {
	expectedMessageID uint64
	responseMessage   []byte
	err               error
}

type fakeTransport struct {
	t          *testing.T
	steps      []fakeTransportStep
	callCount  int
	closeCount int
	closed     bool
}

func (f *fakeTransport) CallMessage(requestMessage []byte, responseMessage []byte, _ time.Duration) (int, error) {
	f.t.Helper()

	if f.closed {
		return 0, errors.New("call on closed transport")
	}
	if f.callCount >= len(f.steps) {
		return 0, errors.New("unexpected transport call")
	}

	step := f.steps[f.callCount]
	f.callCount++

	header, err := protocol.DecodeMessageHeader(requestMessage)
	if err != nil {
		return 0, err
	}
	if step.expectedMessageID != 0 && header.MessageID != step.expectedMessageID {
		return 0, errors.New("unexpected request message id")
	}
	if step.err != nil {
		return 0, step.err
	}
	if len(step.responseMessage) > len(responseMessage) {
		return 0, errors.New("response buffer too small")
	}

	copy(responseMessage, step.responseMessage)
	return len(step.responseMessage), nil
}

func (f *fakeTransport) Close() error {
	f.closeCount++
	f.closed = true
	return nil
}

type fakeConnector struct {
	t       *testing.T
	results []fakeConnectResult
	calls   int
}

type fakeConnectResult struct {
	transport transportClient
	err       error
}

func (c *fakeConnector) connect(_ Config, _ time.Duration) (transportClient, error) {
	c.t.Helper()

	if c.calls >= len(c.results) {
		return nil, errors.New("unexpected connect")
	}
	result := c.results[c.calls]
	c.calls++
	return result.transport, result.err
}

func strictTestConfig() Config {
	config := NewConfig("test-namespace", "test-service")
	config.SupportedProfiles = 1
	config.PreferredProfiles = 1
	config.MaxResponsePayloadBytes = DefaultMaxResponsePayloadBytes
	config.MaxResponseBatchItems = DefaultMaxResponseBatchItems
	config.AuthToken = 1
	return config
}

func snapshotItemsA() []protocol.CgroupsSnapshotItem {
	return []protocol.CgroupsSnapshotItem{
		{
			Hash:    123,
			Options: 0x2,
			Enabled: true,
			Name:    "system.slice-nginx",
			Path:    "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
		},
		{
			Hash:    456,
			Options: 0x4,
			Enabled: false,
			Name:    "docker-1234",
			Path:    "",
		},
	}
}

func snapshotItemsB() []protocol.CgroupsSnapshotItem {
	return []protocol.CgroupsSnapshotItem{
		{
			Hash:    123,
			Options: 0x2,
			Enabled: true,
			Name:    "system.slice-nginx",
			Path:    "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
		},
		{
			Hash:    9001,
			Options: 0x6,
			Enabled: true,
			Name:    "system.slice-netdata",
			Path:    "/sys/fs/cgroup/system.slice/netdata.service/cgroup.procs",
		},
	}
}

func fixedResponseForTest(
	t *testing.T,
	messageID uint64,
	generation uint64,
	systemdEnabled bool,
	items []protocol.CgroupsSnapshotItem,
) []byte {
	t.Helper()

	payloadLenCap, err := protocol.MaxBatchPayloadLen(DefaultMaxResponsePayloadBytes, uint32(len(items)))
	if err != nil {
		t.Fatalf("MaxBatchPayloadLen() error = %v", err)
	}

	payload := make([]byte, payloadLenCap)
	builder, err := protocol.NewCgroupsSnapshotResponseBuilder(payload, generation, systemdEnabled, 0, uint32(len(items)))
	if err != nil {
		t.Fatalf("NewCgroupsSnapshotResponseBuilder() error = %v", err)
	}
	for idx, item := range items {
		if err := builder.AddItem(item); err != nil {
			t.Fatalf("AddItem(%d) error = %v", idx, err)
		}
	}
	payloadLen, err := builder.Finish()
	if err != nil {
		t.Fatalf("Finish() error = %v", err)
	}

	header, err := protocol.EncodeMessageHeader(protocol.MessageHeader{
		Magic:           protocol.MessageMagic,
		Version:         protocol.MessageVersion,
		HeaderLen:       protocol.MessageHeaderLen,
		Kind:            protocol.MessageKindResponse,
		Flags:           protocol.MessageFlagBatch,
		Code:            protocol.MethodCgroupsSnapshot,
		TransportStatus: protocol.TransportStatusOK,
		PayloadLen:      uint32(payloadLen),
		ItemCount:       uint32(len(items)),
		MessageID:       messageID,
	})
	if err != nil {
		t.Fatalf("EncodeMessageHeader() error = %v", err)
	}

	message := make([]byte, 0, protocol.MessageHeaderLen+payloadLen)
	message = append(message, header[:]...)
	message = append(message, payload[:payloadLen]...)
	return message
}

func malformedButTransportValidResponseForTest(t *testing.T, messageID uint64) []byte {
	t.Helper()

	message := fixedResponseForTest(t, messageID, 42, true, snapshotItemsA())
	base := protocol.MessageHeaderLen + protocol.CgroupsSnapshotResponseHeaderLen
	if len(message) < base+4 {
		t.Fatalf("fixed response too short to corrupt")
	}

	// Keep the outer envelope valid while corrupting the first item offset.
	message[base+0] = 1
	message[base+1] = 0
	message[base+2] = 0
	message[base+3] = 0
	return message
}

func transportStatusResponseForTest(t *testing.T, messageID uint64, status uint16) []byte {
	t.Helper()

	header, err := protocol.EncodeMessageHeader(protocol.MessageHeader{
		Magic:           protocol.MessageMagic,
		Version:         protocol.MessageVersion,
		HeaderLen:       protocol.MessageHeaderLen,
		Kind:            protocol.MessageKindResponse,
		Flags:           protocol.MessageFlagBatch,
		Code:            protocol.MethodCgroupsSnapshot,
		TransportStatus: status,
		PayloadLen:      0,
		ItemCount:       0,
		MessageID:       messageID,
	})
	if err != nil {
		t.Fatalf("EncodeMessageHeader() error = %v", err)
	}

	message := make([]byte, protocol.MessageHeaderLen)
	copy(message, header[:])
	return message
}

func installFakeConnector(t *testing.T, connector *fakeConnector) {
	t.Helper()

	previous := connectTransportFunc
	connectTransportFunc = connector.connect
	t.Cleanup(func() {
		connectTransportFunc = previous
	})
}

func mustNewClient(t *testing.T) *Client {
	t.Helper()

	client, err := NewClient(strictTestConfig())
	if err != nil {
		t.Fatalf("NewClient() error = %v", err)
	}
	return client
}

func TestNewClientRejectsImplicitInsecureDefaults(t *testing.T) {
	_, err := NewClient(NewConfig("test-namespace", "test-service"))
	if err == nil {
		t.Fatalf("expected NewClient to reject implicit defaults")
	}
}

func TestRefreshFailureAfterReadyDisconnectsAndKeepsCache(t *testing.T) {
	transport := &fakeTransport{
		t: t,
		steps: []fakeTransportStep{
			{expectedMessageID: 1, responseMessage: fixedResponseForTest(t, 1, 42, true, snapshotItemsA())},
			{expectedMessageID: 2, err: errors.New("synthetic transport failure")},
		},
	}
	installFakeConnector(t, &fakeConnector{
		t: t,
		results: []fakeConnectResult{
			{transport: transport},
		},
	})

	client := mustNewClient(t)
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("first Refresh() error = %v", err)
	}

	cache := client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 2 {
		t.Fatalf("unexpected initial cache: %+v", cache)
	}

	if err := client.Refresh(5 * time.Second); err == nil {
		t.Fatalf("expected second Refresh() to fail")
	}

	cache = client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 2 {
		t.Fatalf("cache should stay populated after refresh failure: %+v", cache)
	}
	if client.Lookup(456, "docker-1234") == nil {
		t.Fatalf("expected cached lookup to survive refresh failure")
	}
	if client.transport != nil {
		t.Fatalf("expected failed transport to be disconnected")
	}
	if transport.closeCount != 1 {
		t.Fatalf("expected transport to be closed once, got %d", transport.closeCount)
	}
}

func TestRefreshReconnectsAfterReadyFailureAndUpdatesCache(t *testing.T) {
	firstTransport := &fakeTransport{
		t: t,
		steps: []fakeTransportStep{
			{expectedMessageID: 1, responseMessage: fixedResponseForTest(t, 1, 42, true, snapshotItemsA())},
			{expectedMessageID: 2, err: errors.New("synthetic transport failure")},
		},
	}
	secondTransport := &fakeTransport{
		t: t,
		steps: []fakeTransportStep{
			{expectedMessageID: 3, responseMessage: fixedResponseForTest(t, 3, 84, true, snapshotItemsB())},
		},
	}
	connector := &fakeConnector{
		t: t,
		results: []fakeConnectResult{
			{transport: firstTransport},
			{transport: secondTransport},
		},
	}
	installFakeConnector(t, connector)

	client := mustNewClient(t)
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("first Refresh() error = %v", err)
	}
	if err := client.Refresh(5 * time.Second); err == nil {
		t.Fatalf("expected second Refresh() to fail")
	}
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("third Refresh() after reconnect error = %v", err)
	}

	cache := client.Cache()
	if cache.Generation != 84 || len(cache.Items) != 2 {
		t.Fatalf("unexpected cache after reconnect: %+v", cache)
	}
	if client.Lookup(9001, "system.slice-netdata") == nil {
		t.Fatalf("expected refreshed cache item after reconnect")
	}
	if connector.calls != 2 {
		t.Fatalf("expected two transport connections, got %d", connector.calls)
	}
	if firstTransport.closeCount != 1 {
		t.Fatalf("expected first transport to be closed once, got %d", firstTransport.closeCount)
	}
	if secondTransport.closeCount != 0 {
		t.Fatalf("expected second transport to remain connected, got %d closes", secondTransport.closeCount)
	}
}

func TestMalformedResponseKeepsPreviousCacheAndDisconnects(t *testing.T) {
	transport := &fakeTransport{
		t: t,
		steps: []fakeTransportStep{
			{expectedMessageID: 1, responseMessage: fixedResponseForTest(t, 1, 42, true, snapshotItemsA())},
			{expectedMessageID: 2, responseMessage: malformedButTransportValidResponseForTest(t, 2)},
		},
	}
	installFakeConnector(t, &fakeConnector{
		t: t,
		results: []fakeConnectResult{
			{transport: transport},
		},
	})

	client := mustNewClient(t)
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("first Refresh() error = %v", err)
	}
	if err := client.Refresh(5 * time.Second); err == nil {
		t.Fatalf("expected malformed response to fail")
	}

	cache := client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 2 {
		t.Fatalf("cache should stay populated after malformed response: %+v", cache)
	}
	if client.Lookup(123, "system.slice-nginx") == nil {
		t.Fatalf("expected cached lookup to survive malformed response")
	}
	if client.transport != nil {
		t.Fatalf("expected malformed response to disconnect transport")
	}
	if transport.closeCount != 1 {
		t.Fatalf("expected transport to be closed once, got %d", transport.closeCount)
	}
}

func TestTransportStatusFailureKeepsPreviousCacheAndDisconnects(t *testing.T) {
	transport := &fakeTransport{
		t: t,
		steps: []fakeTransportStep{
			{expectedMessageID: 1, responseMessage: fixedResponseForTest(t, 1, 42, true, snapshotItemsA())},
			{expectedMessageID: 2, responseMessage: transportStatusResponseForTest(t, 2, protocol.TransportStatusLimitExceeded)},
		},
	}
	installFakeConnector(t, &fakeConnector{
		t: t,
		results: []fakeConnectResult{
			{transport: transport},
		},
	})

	client := mustNewClient(t)
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("first Refresh() error = %v", err)
	}

	err := client.Refresh(5 * time.Second)
	if err == nil {
		t.Fatalf("expected transport-status failure")
	}
	if !errors.Is(err, transportStatusErr(protocol.TransportStatusLimitExceeded)) {
		t.Fatalf("expected limit-exceeded transport error, got %v", err)
	}

	cache := client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 2 {
		t.Fatalf("cache should stay populated after transport-status failure: %+v", cache)
	}
	if client.Lookup(456, "docker-1234") == nil {
		t.Fatalf("expected cached lookup to survive transport-status failure")
	}
	if client.transport != nil {
		t.Fatalf("expected transport-status failure to disconnect transport")
	}
	if transport.closeCount != 1 {
		t.Fatalf("expected transport to be closed once, got %d", transport.closeCount)
	}
}
