//go:build unix

package posix

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const (
	testAuthToken uint64 = 0xDEADBEEFCAFEBABE
)

var testServiceCounter atomic.Uint64

// uniqueService returns a unique service name per test to avoid socket conflicts.
func uniqueService(t *testing.T) string {
	t.Helper()
	n := testServiceCounter.Add(1)
	return fmt.Sprintf("gotest_%d_%d", os.Getpid(), n)
}

func testRunDir(t *testing.T) string {
	t.Helper()
	dir := filepath.Join(os.TempDir(), "nipc_go_test")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatalf("cannot create run dir: %v", err)
	}
	return dir
}

func defaultServerConfig() ServerConfig {
	return ServerConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: 4096,
		MaxResponseBatchItems:   16,
		AuthToken:               testAuthToken,
		Backlog:                 4,
	}
}

func defaultClientConfig() ClientConfig {
	return ClientConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: 4096,
		MaxResponseBatchItems:   16,
		AuthToken:               testAuthToken,
	}
}

// serverResult holds the result of an Accept call.
type serverResult struct {
	session *Session
	err     error
}

// startListener creates a listener and returns it. The caller must close it.
func startListener(t *testing.T, runDir, service string, cfg ServerConfig) *Listener {
	t.Helper()
	listener, err := Listen(runDir, service, cfg)
	if err != nil {
		t.Fatalf("Listen failed: %v", err)
	}
	return listener
}

// acceptAsync starts accepting in a goroutine and returns a channel.
func acceptAsync(listener *Listener) <-chan serverResult {
	ch := make(chan serverResult, 1)
	go func() {
		session, err := listener.Accept()
		ch <- serverResult{session, err}
	}()
	return ch
}

// ---------------------------------------------------------------------------
//  Test: Single client ping-pong
// ---------------------------------------------------------------------------

func TestSingleClientPingPong(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	// Client connects
	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	// Wait for server accept
	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Verify negotiated values
	if client.SelectedProfile != protocol.ProfileBaseline {
		t.Errorf("client profile = 0x%x, want 0x%x", client.SelectedProfile, protocol.ProfileBaseline)
	}
	if server.SelectedProfile != protocol.ProfileBaseline {
		t.Errorf("server profile = 0x%x, want 0x%x", server.SelectedProfile, protocol.ProfileBaseline)
	}

	// Client sends request
	payload := []byte("hello from client")
	hdr := protocol.Header{
		Kind:      protocol.KindRequest,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 42,
	}
	if err := client.Send(&hdr, payload); err != nil {
		t.Fatalf("client Send: %v", err)
	}

	// Server receives
	recvBuf := make([]byte, 4096)
	rHdr, rPayload, err := server.Receive(recvBuf)
	if err != nil {
		t.Fatalf("server Receive: %v", err)
	}

	if rHdr.Kind != protocol.KindRequest {
		t.Errorf("server received kind=%d, want %d", rHdr.Kind, protocol.KindRequest)
	}
	if rHdr.MessageID != 42 {
		t.Errorf("server received message_id=%d, want 42", rHdr.MessageID)
	}
	if !bytes.Equal(rPayload, payload) {
		t.Errorf("server received payload mismatch")
	}

	// Server sends response
	respHdr := protocol.Header{
		Kind:      protocol.KindResponse,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 42,
	}
	respPayload := []byte("response from server")
	if err := server.Send(&respHdr, respPayload); err != nil {
		t.Fatalf("server Send: %v", err)
	}

	// Client receives response
	rHdr, rPayload, err = client.Receive(recvBuf)
	if err != nil {
		t.Fatalf("client Receive: %v", err)
	}
	if rHdr.Kind != protocol.KindResponse {
		t.Errorf("client received kind=%d, want %d", rHdr.Kind, protocol.KindResponse)
	}
	if rHdr.MessageID != 42 {
		t.Errorf("client received message_id=%d, want 42", rHdr.MessageID)
	}
	if !bytes.Equal(rPayload, respPayload) {
		t.Errorf("client received payload mismatch")
	}
}

// ---------------------------------------------------------------------------
//  Test: Multi-client (2 clients)
// ---------------------------------------------------------------------------

func TestMultiClient(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	// Connect two clients
	const numClients = 2
	clients := make([]*Session, numClients)
	servers := make([]*Session, numClients)

	for i := 0; i < numClients; i++ {
		acceptCh := acceptAsync(listener)

		cCfg := defaultClientConfig()
		c, err := Connect(runDir, service, &cCfg)
		if err != nil {
			t.Fatalf("Connect[%d] failed: %v", i, err)
		}
		clients[i] = c

		sr := <-acceptCh
		if sr.err != nil {
			t.Fatalf("Accept[%d] failed: %v", i, sr.err)
		}
		servers[i] = sr.session
	}

	defer func() {
		for i := 0; i < numClients; i++ {
			clients[i].Close()
			servers[i].Close()
		}
	}()

	// Each client sends a unique message
	for i := 0; i < numClients; i++ {
		payload := []byte(fmt.Sprintf("client_%d", i))
		hdr := protocol.Header{
			Kind:      protocol.KindRequest,
			Code:      protocol.MethodIncrement,
			ItemCount: 1,
			MessageID: uint64(100 + i),
		}
		if err := clients[i].Send(&hdr, payload); err != nil {
			t.Fatalf("client[%d] Send: %v", i, err)
		}
	}

	// Each server receives and echoes
	buf := make([]byte, 4096)
	for i := 0; i < numClients; i++ {
		rHdr, rPayload, err := servers[i].Receive(buf)
		if err != nil {
			t.Fatalf("server[%d] Receive: %v", i, err)
		}

		expected := []byte(fmt.Sprintf("client_%d", i))
		if !bytes.Equal(rPayload, expected) {
			t.Errorf("server[%d] payload = %q, want %q", i, rPayload, expected)
		}

		resp := protocol.Header{
			Kind:      protocol.KindResponse,
			Code:      rHdr.Code,
			ItemCount: 1,
			MessageID: rHdr.MessageID,
		}
		if err := servers[i].Send(&resp, rPayload); err != nil {
			t.Fatalf("server[%d] Send: %v", i, err)
		}
	}

	// Each client receives its echo
	for i := 0; i < numClients; i++ {
		rHdr, rPayload, err := clients[i].Receive(buf)
		if err != nil {
			t.Fatalf("client[%d] Receive: %v", i, err)
		}
		if rHdr.MessageID != uint64(100+i) {
			t.Errorf("client[%d] message_id = %d, want %d", i, rHdr.MessageID, 100+i)
		}
		expected := []byte(fmt.Sprintf("client_%d", i))
		if !bytes.Equal(rPayload, expected) {
			t.Errorf("client[%d] response payload = %q, want %q", i, rPayload, expected)
		}
	}
}

// ---------------------------------------------------------------------------
//  Test: Pipelining (3 requests, match by message_id)
// ---------------------------------------------------------------------------

func TestPipelining(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Client sends 3 requests without waiting
	messageIDs := []uint64{10, 20, 30}
	for _, mid := range messageIDs {
		hdr := protocol.Header{
			Kind:      protocol.KindRequest,
			Code:      protocol.MethodIncrement,
			ItemCount: 1,
			MessageID: mid,
		}
		payload := []byte(fmt.Sprintf("req_%d", mid))
		if err := client.Send(&hdr, payload); err != nil {
			t.Fatalf("client Send(%d): %v", mid, err)
		}
	}

	// Server receives all 3 and responds in reverse order
	buf := make([]byte, 4096)
	type reqInfo struct {
		hdr     protocol.Header
		payload []byte
	}
	reqs := make([]reqInfo, 0, 3)

	for i := 0; i < 3; i++ {
		rHdr, rPayload, err := server.Receive(buf)
		if err != nil {
			t.Fatalf("server Receive[%d]: %v", i, err)
		}
		reqs = append(reqs, reqInfo{rHdr, rPayload})
	}

	// Respond in reverse order
	for i := len(reqs) - 1; i >= 0; i-- {
		resp := protocol.Header{
			Kind:      protocol.KindResponse,
			Code:      reqs[i].hdr.Code,
			ItemCount: 1,
			MessageID: reqs[i].hdr.MessageID,
		}
		respPayload := append([]byte("resp_"), reqs[i].payload...)
		if err := server.Send(&resp, respPayload); err != nil {
			t.Fatalf("server Send[%d]: %v", i, err)
		}
	}

	// Client receives 3 responses (should arrive in reverse order)
	received := make(map[uint64][]byte)
	for i := 0; i < 3; i++ {
		rHdr, rPayload, err := client.Receive(buf)
		if err != nil {
			t.Fatalf("client Receive[%d]: %v", i, err)
		}
		if rHdr.Kind != protocol.KindResponse {
			t.Errorf("received kind=%d, want RESPONSE", rHdr.Kind)
		}
		received[rHdr.MessageID] = rPayload
	}

	// Verify all messages received with correct data
	for _, mid := range messageIDs {
		payload, ok := received[mid]
		if !ok {
			t.Errorf("missing response for message_id %d", mid)
			continue
		}
		expected := []byte(fmt.Sprintf("resp_req_%d", mid))
		if !bytes.Equal(payload, expected) {
			t.Errorf("message_id %d: payload = %q, want %q", mid, payload, expected)
		}
	}
}

// ---------------------------------------------------------------------------
//  Test: Chunking (large message, small packet_size)
// ---------------------------------------------------------------------------

func TestChunking(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	// Use a small packet_size to force chunking
	const forcedPacketSize = 128

	sCfg := defaultServerConfig()
	sCfg.PacketSize = forcedPacketSize
	sCfg.MaxRequestPayloadBytes = 65536
	sCfg.MaxResponsePayloadBytes = 65536
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.PacketSize = forcedPacketSize
	cCfg.MaxRequestPayloadBytes = 65536
	cCfg.MaxResponsePayloadBytes = 65536
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Verify small packet size was negotiated
	if client.PacketSize > forcedPacketSize {
		t.Errorf("client packet_size = %d, want <= %d", client.PacketSize, forcedPacketSize)
	}

	// Build a large payload (much larger than packet_size)
	largePayload := make([]byte, 2000)
	for i := range largePayload {
		largePayload[i] = byte(i & 0xFF)
	}

	// Client sends large message (will be chunked)
	hdr := protocol.Header{
		Kind:      protocol.KindRequest,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 99,
	}
	if err := client.Send(&hdr, largePayload); err != nil {
		t.Fatalf("client Send (chunked): %v", err)
	}

	// Server receives (reassembles chunks)
	recvBuf := make([]byte, forcedPacketSize)
	rHdr, rPayload, err := server.Receive(recvBuf)
	if err != nil {
		t.Fatalf("server Receive (chunked): %v", err)
	}

	if rHdr.MessageID != 99 {
		t.Errorf("message_id = %d, want 99", rHdr.MessageID)
	}
	if !bytes.Equal(rPayload, largePayload) {
		t.Errorf("chunked payload mismatch: got %d bytes, want %d", len(rPayload), len(largePayload))
	}

	// Server echoes back (also chunked)
	resp := protocol.Header{
		Kind:      protocol.KindResponse,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 99,
	}
	if err := server.Send(&resp, rPayload); err != nil {
		t.Fatalf("server Send (chunked echo): %v", err)
	}

	// Client receives
	rHdr, rPayload, err = client.Receive(recvBuf)
	if err != nil {
		t.Fatalf("client Receive (chunked): %v", err)
	}
	if !bytes.Equal(rPayload, largePayload) {
		t.Errorf("client chunked payload mismatch: got %d bytes, want %d", len(rPayload), len(largePayload))
	}
}

// ---------------------------------------------------------------------------
//  Test: Handshake failures - bad auth
// ---------------------------------------------------------------------------

func TestHandshakeBadAuth(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	sCfg.AuthToken = 0x1111111111111111
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.AuthToken = 0x2222222222222222 // wrong token
	_, err := Connect(runDir, service, &cCfg)
	if err == nil {
		t.Fatal("expected auth failure, got nil")
	}

	if err != ErrAuthFailed {
		t.Errorf("error = %v, want ErrAuthFailed", err)
	}

	// Server side should also fail
	sr := <-acceptCh
	if sr.err == nil {
		sr.session.Close()
		t.Fatal("server expected auth failure")
	}
	if sr.err != ErrAuthFailed {
		t.Errorf("server error = %v, want ErrAuthFailed", sr.err)
	}
}

// ---------------------------------------------------------------------------
//  Test: Handshake failures - profile mismatch
// ---------------------------------------------------------------------------

func TestHandshakeProfileMismatch(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	sCfg.SupportedProfiles = protocol.ProfileSHMFutex // only SHM
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.SupportedProfiles = protocol.ProfileBaseline // only baseline
	_, err := Connect(runDir, service, &cCfg)
	if err == nil {
		t.Fatal("expected profile mismatch, got nil")
	}

	if err != ErrNoProfile {
		t.Errorf("error = %v, want ErrNoProfile", err)
	}

	sr := <-acceptCh
	if sr.err == nil {
		sr.session.Close()
		t.Fatal("server expected profile mismatch")
	}
}

// ---------------------------------------------------------------------------
//  Test: Stale socket recovery
// ---------------------------------------------------------------------------

func TestStaleRecovery(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	sockPath := filepath.Join(runDir, service+".sock")
	defer os.Remove(sockPath)

	// Create a stale socket file (not a real listener)
	f, err := os.Create(sockPath)
	if err != nil {
		t.Fatalf("cannot create stale file: %v", err)
	}
	f.Close()

	// Listen should recover and succeed
	sCfg := defaultServerConfig()
	listener, err := Listen(runDir, service, sCfg)
	if err != nil {
		t.Fatalf("Listen (stale recovery) failed: %v", err)
	}
	defer listener.Close()

	// Now try to listen again — should get AddrInUse because
	// the first listener is alive
	_, err = Listen(runDir, service, sCfg)
	if err != ErrAddrInUse {
		t.Errorf("second Listen: error = %v, want ErrAddrInUse", err)
	}
}

// ---------------------------------------------------------------------------
//  Test: Disconnect detection
// ---------------------------------------------------------------------------

func TestDisconnectDetection(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Close client
	client.Close()

	// Server should get an error on Receive
	buf := make([]byte, 4096)
	_, _, err = server.Receive(buf)
	if err == nil {
		t.Fatal("expected error on Receive after client disconnect, got nil")
	}
}

// ---------------------------------------------------------------------------
//  Test: Listener Fd
// ---------------------------------------------------------------------------

func TestListenerFd(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	if listener.Fd() < 0 {
		t.Errorf("listener fd = %d, want >= 0", listener.Fd())
	}
}

// ---------------------------------------------------------------------------
//  Test: Session Fd and Role
// ---------------------------------------------------------------------------

func TestSessionFdAndRole(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	if client.Fd() < 0 {
		t.Errorf("client fd = %d, want >= 0", client.Fd())
	}
	if server.Fd() < 0 {
		t.Errorf("server fd = %d, want >= 0", server.Fd())
	}
	if client.Role() != RoleClient {
		t.Errorf("client role = %d, want RoleClient", client.Role())
	}
	if server.Role() != RoleServer {
		t.Errorf("server role = %d, want RoleServer", server.Role())
	}
}

// ---------------------------------------------------------------------------
//  Test: Directional limit negotiation (min of client/server)
// ---------------------------------------------------------------------------

func TestDirectionalLimitNegotiation(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	sCfg.MaxRequestPayloadBytes = 2048
	sCfg.MaxRequestBatchItems = 8
	sCfg.MaxResponsePayloadBytes = 8192
	sCfg.MaxResponseBatchItems = 32
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.MaxRequestPayloadBytes = 4096
	cCfg.MaxRequestBatchItems = 16
	cCfg.MaxResponsePayloadBytes = 4096
	cCfg.MaxResponseBatchItems = 16
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// min(client, server) per direction
	if client.MaxRequestPayloadBytes != 2048 {
		t.Errorf("req_payload = %d, want 2048", client.MaxRequestPayloadBytes)
	}
	if client.MaxRequestBatchItems != 8 {
		t.Errorf("req_batch = %d, want 8", client.MaxRequestBatchItems)
	}
	if client.MaxResponsePayloadBytes != 4096 {
		t.Errorf("resp_payload = %d, want 4096", client.MaxResponsePayloadBytes)
	}
	if client.MaxResponseBatchItems != 16 {
		t.Errorf("resp_batch = %d, want 16", client.MaxResponseBatchItems)
	}

	// Server should have the same negotiated values
	if server.MaxRequestPayloadBytes != client.MaxRequestPayloadBytes {
		t.Errorf("server req_payload = %d, want %d", server.MaxRequestPayloadBytes, client.MaxRequestPayloadBytes)
	}
	if server.MaxResponsePayloadBytes != client.MaxResponsePayloadBytes {
		t.Errorf("server resp_payload = %d, want %d", server.MaxResponsePayloadBytes, client.MaxResponsePayloadBytes)
	}
}

// ---------------------------------------------------------------------------
//  Test: Profile selection (highest bit in preferred_intersection)
// ---------------------------------------------------------------------------

func TestProfileSelection(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	sCfg.SupportedProfiles = protocol.ProfileBaseline | protocol.ProfileSHMHybrid | protocol.ProfileSHMFutex
	sCfg.PreferredProfiles = protocol.ProfileSHMFutex
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.SupportedProfiles = protocol.ProfileBaseline | protocol.ProfileSHMHybrid | protocol.ProfileSHMFutex
	cCfg.PreferredProfiles = protocol.ProfileSHMFutex | protocol.ProfileSHMHybrid
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Preferred intersection = SHMFutex (0x04), highest bit = 0x04
	if client.SelectedProfile != protocol.ProfileSHMFutex {
		t.Errorf("selected = 0x%x, want 0x%x (SHMFutex)", client.SelectedProfile, protocol.ProfileSHMFutex)
	}
}

// ---------------------------------------------------------------------------
//  Test: Empty payload
// ---------------------------------------------------------------------------

func TestEmptyPayload(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Send empty payload
	hdr := protocol.Header{
		Kind:      protocol.KindRequest,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 1,
	}
	if err := client.Send(&hdr, nil); err != nil {
		t.Fatalf("Send empty: %v", err)
	}

	buf := make([]byte, 4096)
	rHdr, rPayload, err := server.Receive(buf)
	if err != nil {
		t.Fatalf("Receive empty: %v", err)
	}
	if rHdr.MessageID != 1 {
		t.Errorf("message_id = %d, want 1", rHdr.MessageID)
	}
	if len(rPayload) != 0 {
		t.Errorf("payload len = %d, want 0", len(rPayload))
	}
}

// ---------------------------------------------------------------------------
//  Test: Concurrent send/receive (stress test)
// ---------------------------------------------------------------------------

func TestConcurrentSendReceive(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	sCfg.MaxRequestPayloadBytes = 65536
	sCfg.MaxResponsePayloadBytes = 65536
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.MaxRequestPayloadBytes = 65536
	cCfg.MaxResponsePayloadBytes = 65536
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	const numMessages = 20
	var wg sync.WaitGroup

	// Server goroutine: receive and echo
	wg.Add(1)
	go func() {
		defer wg.Done()
		buf := make([]byte, 65600)
		for i := 0; i < numMessages; i++ {
			rHdr, rPayload, err := server.Receive(buf)
			if err != nil {
				t.Errorf("server Receive[%d]: %v", i, err)
				return
			}
			resp := protocol.Header{
				Kind:      protocol.KindResponse,
				Code:      rHdr.Code,
				ItemCount: 1,
				MessageID: rHdr.MessageID,
			}
			if err := server.Send(&resp, rPayload); err != nil {
				t.Errorf("server Send[%d]: %v", i, err)
				return
			}
		}
	}()

	// Client: send all, then receive all
	for i := 0; i < numMessages; i++ {
		payload := []byte(fmt.Sprintf("message_%d", i))
		hdr := protocol.Header{
			Kind:      protocol.KindRequest,
			Code:      protocol.MethodIncrement,
			ItemCount: 1,
			MessageID: uint64(i),
		}
		if err := client.Send(&hdr, payload); err != nil {
			t.Fatalf("client Send[%d]: %v", i, err)
		}
	}

	received := make(map[uint64]bool)
	buf := make([]byte, 65600)
	for i := 0; i < numMessages; i++ {
		rHdr, _, err := client.Receive(buf)
		if err != nil {
			t.Fatalf("client Receive[%d]: %v", i, err)
		}
		received[rHdr.MessageID] = true
	}

	wg.Wait()

	for i := 0; i < numMessages; i++ {
		if !received[uint64(i)] {
			t.Errorf("missing response for message_id %d", i)
		}
	}
}

// ---------------------------------------------------------------------------
//  Test: Close then Send/Receive returns error
// ---------------------------------------------------------------------------

func TestClosedSessionErrors(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	sr.session.Close()

	client.Close()

	// Send on closed session
	hdr := protocol.Header{
		Kind:      protocol.KindRequest,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 1,
	}
	if err := client.Send(&hdr, []byte("x")); err == nil {
		t.Error("Send on closed session should fail")
	}

	// Receive on closed session
	buf := make([]byte, 4096)
	_, _, err = client.Receive(buf)
	if err == nil {
		t.Error("Receive on closed session should fail")
	}
}

// ---------------------------------------------------------------------------
//  Test: Path too long
// ---------------------------------------------------------------------------

func TestPathTooLong(t *testing.T) {
	longDir := "/tmp/" + string(make([]byte, 200))
	_, err := Connect(longDir, "test", &ClientConfig{})
	if err != ErrPathTooLong {
		t.Errorf("expected ErrPathTooLong, got %v", err)
	}

	_, err = Listen(longDir, "test", ServerConfig{})
	if err != ErrPathTooLong {
		t.Errorf("expected ErrPathTooLong on Listen, got %v", err)
	}
}

// ---------------------------------------------------------------------------
//  Test: Listener close prevents new accepts
// ---------------------------------------------------------------------------

func TestListenerClose(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	listener.Close()

	// Connect should fail after listener close
	cCfg := defaultClientConfig()
	_, err := Connect(runDir, service, &cCfg)
	if err == nil {
		t.Error("Connect after listener close should fail")
	}
}

// ---------------------------------------------------------------------------
//  Test: Defaults applied when config values are 0
// ---------------------------------------------------------------------------

func TestDefaultsApplied(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	// Zero config = all defaults
	sCfg := ServerConfig{
		AuthToken: testAuthToken,
	}
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := ClientConfig{
		AuthToken: testAuthToken,
	}
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Defaults: MaxPayloadDefault=1024, batch=1
	if client.MaxRequestPayloadBytes != protocol.MaxPayloadDefault {
		t.Errorf("req_payload = %d, want %d", client.MaxRequestPayloadBytes, protocol.MaxPayloadDefault)
	}
	if client.MaxRequestBatchItems != 1 {
		t.Errorf("req_batch = %d, want 1", client.MaxRequestBatchItems)
	}
	if client.MaxResponsePayloadBytes != protocol.MaxPayloadDefault {
		t.Errorf("resp_payload = %d, want %d", client.MaxResponsePayloadBytes, protocol.MaxPayloadDefault)
	}
	if client.MaxResponseBatchItems != 1 {
		t.Errorf("resp_batch = %d, want 1", client.MaxResponseBatchItems)
	}
	// PacketSize should be > 0 (auto-detected from SO_SNDBUF)
	if client.PacketSize == 0 {
		t.Error("packet_size should be auto-detected, got 0")
	}
	// Profile should be baseline
	if client.SelectedProfile != protocol.ProfileBaseline {
		t.Errorf("selected_profile = 0x%x, want 0x%x", client.SelectedProfile, protocol.ProfileBaseline)
	}
}

// ---------------------------------------------------------------------------
//  Test: highestBit helper
// ---------------------------------------------------------------------------

func TestHighestBit(t *testing.T) {
	tests := []struct {
		in   uint32
		want uint32
	}{
		{0, 0},
		{1, 1},
		{0x03, 0x02},
		{0x07, 0x04},
		{0x80000000, 0x80000000},
		{0xFF, 0x80},
	}
	for _, tc := range tests {
		got := highestBit(tc.in)
		if got != tc.want {
			t.Errorf("highestBit(0x%x) = 0x%x, want 0x%x", tc.in, got, tc.want)
		}
	}
}

// ---------------------------------------------------------------------------
//  Test: Multiple sequential chunk messages on same session
// ---------------------------------------------------------------------------

func TestMultipleChunkedMessages(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	const forcedPacketSize = 96

	sCfg := defaultServerConfig()
	sCfg.PacketSize = forcedPacketSize
	sCfg.MaxRequestPayloadBytes = 65536
	sCfg.MaxResponsePayloadBytes = 65536
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	acceptCh := acceptAsync(listener)

	cCfg := defaultClientConfig()
	cCfg.PacketSize = forcedPacketSize
	cCfg.MaxRequestPayloadBytes = 65536
	cCfg.MaxResponsePayloadBytes = 65536
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect failed: %v", err)
	}
	defer client.Close()

	sr := <-acceptCh
	if sr.err != nil {
		t.Fatalf("Accept failed: %v", sr.err)
	}
	server := sr.session
	defer server.Close()

	// Send 3 chunked messages sequentially
	for i := 0; i < 3; i++ {
		size := 500 + i*200
		payload := make([]byte, size)
		for j := range payload {
			payload[j] = byte((i*31 + j) & 0xFF)
		}

		hdr := protocol.Header{
			Kind:      protocol.KindRequest,
			Code:      protocol.MethodIncrement,
			ItemCount: 1,
			MessageID: uint64(i + 1),
		}
		if err := client.Send(&hdr, payload); err != nil {
			t.Fatalf("Send[%d]: %v", i, err)
		}

		buf := make([]byte, forcedPacketSize)
		rHdr, rPayload, err := server.Receive(buf)
		if err != nil {
			t.Fatalf("Receive[%d]: %v", i, err)
		}
		if rHdr.MessageID != uint64(i+1) {
			t.Errorf("msg[%d] message_id = %d, want %d", i, rHdr.MessageID, i+1)
		}
		if !bytes.Equal(rPayload, payload) {
			t.Errorf("msg[%d] payload mismatch (%d vs %d bytes)", i, len(rPayload), len(payload))
		}
	}
}

// ---------------------------------------------------------------------------
//  Test: Connect to non-existent socket
// ---------------------------------------------------------------------------

func TestConnectNoServer(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)

	cCfg := defaultClientConfig()
	_, err := Connect(runDir, service, &cCfg)
	if err == nil {
		t.Fatal("expected error connecting to non-existent socket")
	}
}

// ---------------------------------------------------------------------------
//  Test: Timeout guard for Accept (server should not block forever in test)
// ---------------------------------------------------------------------------

func TestAcceptWithTimeout(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	defer os.Remove(filepath.Join(runDir, service+".sock"))

	sCfg := defaultServerConfig()
	listener := startListener(t, runDir, service, sCfg)
	defer listener.Close()

	// Start accept in background, connect after a short delay
	done := make(chan bool, 1)
	go func() {
		acceptCh := acceptAsync(listener)
		select {
		case sr := <-acceptCh:
			if sr.err != nil {
				done <- false
			} else {
				sr.session.Close()
				done <- true
			}
		case <-time.After(5 * time.Second):
			done <- false
		}
	}()

	// Short delay then connect
	time.Sleep(50 * time.Millisecond)
	cCfg := defaultClientConfig()
	client, err := Connect(runDir, service, &cCfg)
	if err != nil {
		t.Fatalf("Connect: %v", err)
	}
	client.Close()

	if ok := <-done; !ok {
		t.Error("accept did not complete successfully")
	}
}
