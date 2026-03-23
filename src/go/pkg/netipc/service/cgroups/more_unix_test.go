//go:build unix

package cgroups

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sync/atomic"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

var unixServiceCounter atomic.Uint64

func uniqueUnixService(prefix string) string {
	return fmt.Sprintf("%s_%d", prefix, unixServiceCounter.Add(1))
}

type unixRawSessionServer struct {
	doneCh chan error
}

func startRawPosixSessionServerN(
	t *testing.T,
	service string,
	cfg posix.ServerConfig,
	accepts int,
	handler func(*posix.Session, protocol.Header, []byte) error,
) *unixRawSessionServer {
	t.Helper()
	ensureRunDir()
	cleanupAll(service)

	listener, err := posix.Listen(testRunDir, service, cfg)
	if err != nil {
		t.Fatalf("Listen failed: %v", err)
	}

	srv := &unixRawSessionServer{doneCh: make(chan error, 1)}
	go func() {
		defer listener.Close()

		for i := 0; i < accepts; i++ {
			session, err := listener.Accept()
			if err != nil {
				srv.doneCh <- err
				return
			}

			recvBuf := make([]byte, protocol.HeaderSize+int(cfg.MaxRequestPayloadBytes))
			hdr, payload, err := session.Receive(recvBuf)
			if err != nil {
				session.Close()
				srv.doneCh <- err
				return
			}

			if err := handler(session, hdr, payload); err != nil {
				session.Close()
				srv.doneCh <- err
				return
			}

			session.Close()
		}

		srv.doneCh <- nil
	}()

	time.Sleep(200 * time.Millisecond)
	return srv
}

func startRawPosixSessionServer(
	t *testing.T,
	service string,
	cfg posix.ServerConfig,
	handler func(*posix.Session, protocol.Header, []byte) error,
) *unixRawSessionServer {
	t.Helper()
	return startRawPosixSessionServerN(t, service, cfg, 1, handler)
}

func (s *unixRawSessionServer) wait(t *testing.T) {
	t.Helper()
	if err := <-s.doneCh; err != nil {
		t.Fatalf("raw unix session server failed: %v", err)
	}
}

func refreshUnixClientReady(t *testing.T, client *Client) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		client.Refresh()
		if client.Ready() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("client not ready, state=%d", client.state)
}

func newRawPosixShmClient(t *testing.T) (*Client, *posix.ShmContext) {
	t.Helper()

	runDir := t.TempDir()
	service := uniqueUnixService("go_unix_raw_shm")
	sessionID := unixServiceCounter.Add(1)
	reqCap := uint32(protocol.HeaderSize + 4096)
	respCap := uint32(protocol.HeaderSize + responseBufSize)

	server, err := posix.ShmServerCreate(runDir, service, sessionID, reqCap, respCap)
	if err != nil {
		t.Fatalf("ShmServerCreate failed: %v", err)
	}

	clientShm, err := posix.ShmClientAttach(runDir, service, sessionID)
	if err != nil {
		server.ShmDestroy()
		t.Fatalf("ShmClientAttach failed: %v", err)
	}

	client := NewClient(runDir, service, testClientConfig())
	client.state = StateReady
	client.shm = clientShm

	t.Cleanup(func() {
		client.Close()
		server.ShmDestroy()
	})

	return client, server
}

func TestUnixServerStopWhileIdle(t *testing.T) {
	svc := uniqueUnixService("go_unix_stop_idle")
	cleanupAll(svc)
	server := NewServer(testRunDir, svc, testServerConfig(), testHandlers())

	done := make(chan error, 1)
	go func() {
		done <- server.Run()
	}()

	time.Sleep(100 * time.Millisecond)
	server.Stop()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run after Stop = %v, want nil", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Run did not exit after Stop")
	}

	cleanupAll(svc)
}

func TestUnixRefreshFromBrokenReconnects(t *testing.T) {
	svc := uniqueUnixService("go_unix_refresh_broken")
	ts := startTestServer(svc, testHandlers())
	defer ts.stop()

	client := NewClient(testRunDir, svc, testClientConfig())
	defer client.Close()

	refreshUnixClientReady(t, client)

	client.session.Close()
	client.state = StateBroken

	changed := client.Refresh()
	if !changed {
		t.Fatal("refresh from BROKEN should change state")
	}
	if client.state != StateReady {
		t.Fatalf("expected READY after reconnect, got %d", client.state)
	}
	if client.reconnectCount < 1 {
		t.Fatalf("expected reconnect_count >= 1, got %d", client.reconnectCount)
	}
}

func TestUnixServerRunRejectsInvalidServiceName(t *testing.T) {
	server := NewServer(testRunDir, "bad/service", testServerConfig(), testHandlers())
	if err := server.Run(); !errors.Is(err, posix.ErrBadParam) {
		t.Fatalf("Run() invalid service name = %v, want %v", err, posix.ErrBadParam)
	}
}

func TestUnixClientTransportWithoutSession(t *testing.T) {
	client := NewClient(testRunDir, uniqueUnixService("go_unix_transport"), testClientConfig())
	defer client.Close()

	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            protocol.MethodIncrement,
		ItemCount:       1,
		MessageID:       1,
		TransportStatus: protocol.StatusOK,
	}

	if err := client.transportSend(&hdr, nil); !errors.Is(err, protocol.ErrTruncated) {
		t.Fatalf("transportSend without session = %v, want ErrTruncated", err)
	}
	if _, _, err := client.transportReceive(); !errors.Is(err, protocol.ErrTruncated) {
		t.Fatalf("transportReceive without session = %v, want ErrTruncated", err)
	}
}

func TestUnixClientTransportReceiveShmError(t *testing.T) {
	client := NewClient(testRunDir, uniqueUnixService("go_unix_transport_shm_err"), testClientConfig())
	defer client.Close()

	client.state = StateReady
	client.shm = &posix.ShmContext{}

	if _, _, err := client.transportReceive(); !errors.Is(err, protocol.ErrTruncated) {
		t.Fatalf("transportReceive with invalid SHM context = %v, want %v", err, protocol.ErrTruncated)
	}
}

func TestUnixClientTransportReceiveShmRejectsShortMessage(t *testing.T) {
	client, serverShm := newRawPosixShmClient(t)

	if err := serverShm.ShmSend([]byte{1, 2, 3, 4}); err != nil {
		t.Fatalf("ShmSend failed: %v", err)
	}

	if _, _, err := client.transportReceive(); !errors.Is(err, protocol.ErrTruncated) {
		t.Fatalf("transportReceive short SHM message = %v, want %v", err, protocol.ErrTruncated)
	}
}

func TestUnixClientTransportReceiveShmRejectsBadHeader(t *testing.T) {
	client, serverShm := newRawPosixShmClient(t)

	msg := make([]byte, protocol.HeaderSize)
	if err := serverShm.ShmSend(msg); err != nil {
		t.Fatalf("ShmSend failed: %v", err)
	}

	if _, _, err := client.transportReceive(); !errors.Is(err, protocol.ErrBadMagic) {
		t.Fatalf("transportReceive bad SHM header = %v, want %v", err, protocol.ErrBadMagic)
	}
}

func TestUnixDispatchSingleUnsupportedMethods(t *testing.T) {
	server := NewServer(testRunDir, uniqueUnixService("go_unix_dispatch"), testServerConfig(), Handlers{})
	responseBuf := make([]byte, 128)

	for _, methodCode := range []uint16{
		protocol.MethodIncrement,
		protocol.MethodStringReverse,
		protocol.MethodCgroupsSnapshot,
		0xffff,
	} {
		if n, ok := server.dispatchSingle(methodCode, nil, responseBuf); ok || n != 0 {
			t.Fatalf("dispatchSingle(%d) = (%d, %v), want (0, false)", methodCode, n, ok)
		}
	}

	server = NewServer(testRunDir, uniqueUnixService("go_unix_snapshot_dispatch"), testServerConfig(), testHandlers())
	if n, ok := server.dispatchSingle(protocol.MethodCgroupsSnapshot, nil, nil); ok || n != 0 {
		t.Fatalf("dispatchSingle snapshot with zero response buffer = (%d, %v), want (0, false)", n, ok)
	}
}

func TestUnixCallSnapshotWithMalformedTransportState(t *testing.T) {
	svc := uniqueUnixService("go_unix_malformed_state")
	ts := startTestServer(svc, testHandlers())
	defer ts.stop()

	client := NewClient(testRunDir, svc, testClientConfig())
	defer client.Close()

	refreshUnixClientReady(t, client)

	client.session.Close()
	client.session = nil

	view, err := client.CallSnapshot()
	if err != nil {
		t.Fatalf("expected reconnect to recover from nil session, got %v", err)
	}
	if view.ItemCount != 3 {
		t.Fatalf("expected recovered snapshot, got %d items", view.ItemCount)
	}
}

func TestUnixCallIncrementBatchWithMalformedTransportState(t *testing.T) {
	svc := uniqueUnixService("go_unix_batch_malformed_state")

	scfg := testServerConfig()
	scfg.MaxRequestBatchItems = 16
	scfg.MaxResponseBatchItems = 16
	s := NewServer(testRunDir, svc, scfg, pingPongHandlers())
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		s.Run()
	}()
	time.Sleep(100 * time.Millisecond)
	defer func() {
		s.Stop()
		<-doneCh
		cleanupAll(svc)
	}()

	ccfg := testClientConfig()
	ccfg.MaxRequestBatchItems = 16
	ccfg.MaxResponseBatchItems = 16
	client := NewClient(testRunDir, svc, ccfg)
	defer client.Close()

	refreshUnixClientReady(t, client)

	client.session.Close()
	client.session = nil

	got, err := client.CallIncrementBatch([]uint64{1, 41})
	if err != nil {
		t.Fatalf("expected batch reconnect to recover from nil session, got %v", err)
	}

	want := []uint64{2, 42}
	if len(got) != len(want) {
		t.Fatalf("batch result len = %d, want %d", len(got), len(want))
	}
	for i, v := range want {
		if got[i] != v {
			t.Fatalf("batch[%d] = %d, want %d", i, got[i], v)
		}
	}
}

func TestUnixCallIncrementRejectsMalformedResponseEnvelope(t *testing.T) {
	cases := []struct {
		name   string
		want   error
		mutate func(*protocol.Header)
	}{
		{
			name: "bad kind",
			want: protocol.ErrBadKind,
			mutate: func(h *protocol.Header) {
				h.Kind = protocol.KindRequest
			},
		},
		{
			name: "bad code",
			want: protocol.ErrBadLayout,
			mutate: func(h *protocol.Header) {
				h.Code = protocol.MethodStringReverse
			},
		},
		{
			name: "bad status",
			want: protocol.ErrBadLayout,
			mutate: func(h *protocol.Header) {
				h.TransportStatus = protocol.StatusInternalError
			},
		},
		{
			name: "bad message_id",
			want: protocol.ErrTruncated,
			mutate: func(h *protocol.Header) {
				h.MessageID++
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			svc := uniqueUnixService("go_unix_bad_incr_resp")
			srv := startRawPosixSessionServer(t, svc, testServerConfig(),
				func(session *posix.Session, hdr protocol.Header, payload []byte) error {
					if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodIncrement {
						return fmt.Errorf("unexpected request header: %+v", hdr)
					}

					var respPayload [protocol.IncrementPayloadSize]byte
					protocol.IncrementEncode(42, respPayload[:])

					respHdr := protocol.Header{
						Kind:            protocol.KindResponse,
						Code:            protocol.MethodIncrement,
						ItemCount:       1,
						MessageID:       hdr.MessageID,
						TransportStatus: protocol.StatusOK,
					}
					tc.mutate(&respHdr)
					return session.Send(&respHdr, respPayload[:])
				})

			client := NewClient(testRunDir, svc, testClientConfig())
			defer client.Close()

			refreshUnixClientReady(t, client)

			_, err := client.CallIncrement(41)
			if !errors.Is(err, tc.want) {
				t.Fatalf("CallIncrement error = %v, want %v", err, tc.want)
			}

			srv.wait(t)
			cleanupAll(svc)
		})
	}
}

func TestUnixCallIncrementRejectsMalformedPayload(t *testing.T) {
	svc := uniqueUnixService("go_unix_bad_incr_payload")
	srv := startRawPosixSessionServer(t, svc, testServerConfig(),
		func(session *posix.Session, hdr protocol.Header, payload []byte) error {
			if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodIncrement {
				return fmt.Errorf("unexpected request header: %+v", hdr)
			}

			respHdr := protocol.Header{
				Kind:            protocol.KindResponse,
				Code:            protocol.MethodIncrement,
				ItemCount:       1,
				MessageID:       hdr.MessageID,
				TransportStatus: protocol.StatusOK,
			}
			return session.Send(&respHdr, []byte{1, 2, 3, 4})
		})

	client := NewClient(testRunDir, svc, testClientConfig())
	defer client.Close()

	refreshUnixClientReady(t, client)

	_, err := client.CallIncrement(41)
	if !errors.Is(err, protocol.ErrTruncated) {
		t.Fatalf("CallIncrement error = %v, want %v", err, protocol.ErrTruncated)
	}

	srv.wait(t)
	cleanupAll(svc)
}

func TestUnixCallStringReverseRejectsMalformedResponseEnvelope(t *testing.T) {
	cases := []struct {
		name   string
		want   error
		mutate func(*protocol.Header)
	}{
		{
			name: "bad kind",
			want: protocol.ErrBadKind,
			mutate: func(h *protocol.Header) {
				h.Kind = protocol.KindRequest
			},
		},
		{
			name: "bad code",
			want: protocol.ErrBadLayout,
			mutate: func(h *protocol.Header) {
				h.Code = protocol.MethodIncrement
			},
		},
		{
			name: "bad status",
			want: protocol.ErrBadLayout,
			mutate: func(h *protocol.Header) {
				h.TransportStatus = protocol.StatusInternalError
			},
		},
		{
			name: "bad message_id",
			want: protocol.ErrTruncated,
			mutate: func(h *protocol.Header) {
				h.MessageID++
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			svc := uniqueUnixService("go_unix_bad_str_resp")
			srv := startRawPosixSessionServer(t, svc, testServerConfig(),
				func(session *posix.Session, hdr protocol.Header, payload []byte) error {
					if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodStringReverse {
						return fmt.Errorf("unexpected request header: %+v", hdr)
					}

					respPayload := make([]byte, protocol.StringReverseHdrSize+len("olleh")+1)
					if protocol.StringReverseEncode("olleh", respPayload) == 0 {
						return fmt.Errorf("StringReverseEncode failed")
					}

					respHdr := protocol.Header{
						Kind:            protocol.KindResponse,
						Code:            protocol.MethodStringReverse,
						ItemCount:       1,
						MessageID:       hdr.MessageID,
						TransportStatus: protocol.StatusOK,
					}
					tc.mutate(&respHdr)
					return session.Send(&respHdr, respPayload)
				})

			client := NewClient(testRunDir, svc, testClientConfig())
			defer client.Close()

			refreshUnixClientReady(t, client)

			_, err := client.CallStringReverse("hello")
			if !errors.Is(err, tc.want) {
				t.Fatalf("CallStringReverse error = %v, want %v", err, tc.want)
			}

			srv.wait(t)
			cleanupAll(svc)
		})
	}
}

func TestUnixCallSnapshotRejectsMalformedPayload(t *testing.T) {
	svc := uniqueUnixService("go_unix_bad_snapshot_payload")
	srv := startRawPosixSessionServer(t, svc, testServerConfig(),
		func(session *posix.Session, hdr protocol.Header, payload []byte) error {
			if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodCgroupsSnapshot {
				return fmt.Errorf("unexpected request header: %+v", hdr)
			}

			respHdr := protocol.Header{
				Kind:            protocol.KindResponse,
				Code:            protocol.MethodCgroupsSnapshot,
				ItemCount:       1,
				MessageID:       hdr.MessageID,
				TransportStatus: protocol.StatusOK,
			}
			return session.Send(&respHdr, []byte{1, 2, 3, 4})
		})

	client := NewClient(testRunDir, svc, testClientConfig())
	defer client.Close()

	refreshUnixClientReady(t, client)

	_, err := client.CallSnapshot()
	if !errors.Is(err, protocol.ErrTruncated) {
		t.Fatalf("CallSnapshot error = %v, want %v", err, protocol.ErrTruncated)
	}

	srv.wait(t)
	cleanupAll(svc)
}

func TestUnixCallStringReverseRejectsMalformedPayload(t *testing.T) {
	svc := uniqueUnixService("go_unix_bad_str_payload")
	srv := startRawPosixSessionServer(t, svc, testServerConfig(),
		func(session *posix.Session, hdr protocol.Header, payload []byte) error {
			if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodStringReverse {
				return fmt.Errorf("unexpected request header: %+v", hdr)
			}

			respHdr := protocol.Header{
				Kind:            protocol.KindResponse,
				Code:            protocol.MethodStringReverse,
				ItemCount:       1,
				MessageID:       hdr.MessageID,
				TransportStatus: protocol.StatusOK,
			}

			respPayload := make([]byte, protocol.StringReverseHdrSize+3)
			binary.NativeEndian.PutUint32(respPayload[0:4], uint32(protocol.StringReverseHdrSize))
			binary.NativeEndian.PutUint32(respPayload[4:8], 2)
			copy(respPayload[8:], []byte{'o', 'k', 'x'})
			return session.Send(&respHdr, respPayload)
		})

	client := NewClient(testRunDir, svc, testClientConfig())
	defer client.Close()

	refreshUnixClientReady(t, client)

	_, err := client.CallStringReverse("hello")
	if !errors.Is(err, protocol.ErrMissingNul) {
		t.Fatalf("CallStringReverse error = %v, want %v", err, protocol.ErrMissingNul)
	}

	srv.wait(t)
	cleanupAll(svc)
}

func TestUnixCallIncrementBatchRejectsMalformedResponseEnvelope(t *testing.T) {
	cases := []struct {
		name   string
		want   error
		mutate func(*protocol.Header)
	}{
		{
			name: "bad kind",
			want: protocol.ErrBadKind,
			mutate: func(h *protocol.Header) {
				h.Kind = protocol.KindRequest
			},
		},
		{
			name: "bad code",
			want: protocol.ErrBadLayout,
			mutate: func(h *protocol.Header) {
				h.Code = protocol.MethodStringReverse
			},
		},
		{
			name: "bad status",
			want: protocol.ErrBadLayout,
			mutate: func(h *protocol.Header) {
				h.TransportStatus = protocol.StatusInternalError
			},
		},
		{
			name: "bad message_id",
			want: protocol.ErrTruncated,
			mutate: func(h *protocol.Header) {
				h.MessageID++
			},
		},
		{
			name: "missing batch flag",
			want: protocol.ErrBadItemCount,
			mutate: func(h *protocol.Header) {
				h.Flags = 0
			},
		},
		{
			name: "wrong item count",
			want: protocol.ErrBadItemCount,
			mutate: func(h *protocol.Header) {
				h.Flags = protocol.FlagBatch
				h.ItemCount = 1
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cfg := testServerConfig()
			cfg.MaxRequestBatchItems = 16
			cfg.MaxResponseBatchItems = 16

			svc := uniqueUnixService("go_unix_bad_batch_resp")
			srv := startRawPosixSessionServer(t, svc, cfg,
				func(session *posix.Session, hdr protocol.Header, payload []byte) error {
					if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodIncrement {
						return fmt.Errorf("unexpected request header: %+v", hdr)
					}

					respHdr := protocol.Header{
						Kind:            protocol.KindResponse,
						Code:            protocol.MethodIncrement,
						Flags:           protocol.FlagBatch,
						ItemCount:       hdr.ItemCount,
						MessageID:       hdr.MessageID,
						TransportStatus: protocol.StatusOK,
					}
					tc.mutate(&respHdr)

					var itemA [protocol.IncrementPayloadSize]byte
					var itemB [protocol.IncrementPayloadSize]byte
					if protocol.IncrementEncode(2, itemA[:]) == 0 || protocol.IncrementEncode(3, itemB[:]) == 0 {
						return fmt.Errorf("IncrementEncode failed")
					}

					respBuf := make([]byte, 64)
					bb := protocol.NewBatchBuilder(respBuf, 2)
					if err := bb.Add(itemA[:]); err != nil {
						return err
					}
					if err := bb.Add(itemB[:]); err != nil {
						return err
					}
					n, _ := bb.Finish()
					return session.Send(&respHdr, respBuf[:n])
				})

			ccfg := testClientConfig()
			ccfg.MaxRequestBatchItems = 16
			ccfg.MaxResponseBatchItems = 16

			client := NewClient(testRunDir, svc, ccfg)
			defer client.Close()

			refreshUnixClientReady(t, client)

			_, err := client.CallIncrementBatch([]uint64{1, 2})
			if !errors.Is(err, tc.want) {
				t.Fatalf("CallIncrementBatch error = %v, want %v", err, tc.want)
			}

			srv.wait(t)
			cleanupAll(svc)
		})
	}
}

func TestUnixCallIncrementBatchRejectsMalformedPayload(t *testing.T) {
	t.Run("bad batch directory", func(t *testing.T) {
		cfg := testServerConfig()
		cfg.MaxRequestBatchItems = 16
		cfg.MaxResponseBatchItems = 16

		svc := uniqueUnixService("go_unix_bad_batch_dir")
		srv := startRawPosixSessionServerN(t, svc, cfg, 2,
			func(session *posix.Session, hdr protocol.Header, payload []byte) error {
				if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodIncrement {
					return fmt.Errorf("unexpected request header: %+v", hdr)
				}

				respPayload := make([]byte, 24)
				binary.NativeEndian.PutUint32(respPayload[0:4], 1)
				binary.NativeEndian.PutUint32(respPayload[4:8], 4)
				binary.NativeEndian.PutUint32(respPayload[8:12], 0)
				binary.NativeEndian.PutUint32(respPayload[12:16], 4)
				copy(respPayload[16:], []byte("payload!!"))

				respHdr := protocol.Header{
					Kind:            protocol.KindResponse,
					Code:            protocol.MethodIncrement,
					Flags:           protocol.FlagBatch,
					ItemCount:       2,
					MessageID:       hdr.MessageID,
					TransportStatus: protocol.StatusOK,
				}
				return session.Send(&respHdr, respPayload)
			})

		ccfg := testClientConfig()
		ccfg.MaxRequestBatchItems = 16
		ccfg.MaxResponseBatchItems = 16
		client := NewClient(testRunDir, svc, ccfg)
		defer client.Close()

		refreshUnixClientReady(t, client)

		_, err := client.CallIncrementBatch([]uint64{1, 2})
		if !errors.Is(err, protocol.ErrTruncated) {
			t.Fatalf("CallIncrementBatch error = %v, want %v", err, protocol.ErrTruncated)
		}

		srv.wait(t)
		cleanupAll(svc)
	})

	t.Run("truncated batch item", func(t *testing.T) {
		cfg := testServerConfig()
		cfg.MaxRequestBatchItems = 16
		cfg.MaxResponseBatchItems = 16

		svc := uniqueUnixService("go_unix_bad_batch_payload")
		srv := startRawPosixSessionServerN(t, svc, cfg, 1,
			func(session *posix.Session, hdr protocol.Header, payload []byte) error {
				if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodIncrement {
					return fmt.Errorf("unexpected request header: %+v", hdr)
				}

				respBuf := make([]byte, 64)
				bb := protocol.NewBatchBuilder(respBuf, 1)
				if err := bb.Add([]byte{1, 2, 3, 4}); err != nil {
					return err
				}
				n, _ := bb.Finish()

				respHdr := protocol.Header{
					Kind:            protocol.KindResponse,
					Code:            protocol.MethodIncrement,
					Flags:           protocol.FlagBatch,
					ItemCount:       1,
					MessageID:       hdr.MessageID,
					TransportStatus: protocol.StatusOK,
				}
				return session.Send(&respHdr, respBuf[:n])
			})

		ccfg := testClientConfig()
		ccfg.MaxRequestBatchItems = 16
		ccfg.MaxResponseBatchItems = 16
		client := NewClient(testRunDir, svc, ccfg)
		defer client.Close()

		refreshUnixClientReady(t, client)

		_, err := client.CallIncrementBatch([]uint64{1})
		if !errors.Is(err, protocol.ErrTruncated) {
			t.Fatalf("CallIncrementBatch error = %v, want %v", err, protocol.ErrTruncated)
		}

		srv.wait(t)
		cleanupAll(svc)
	})

	t.Run("missing batch body", func(t *testing.T) {
		cfg := testServerConfig()
		cfg.MaxRequestBatchItems = 16
		cfg.MaxResponseBatchItems = 16

		svc := uniqueUnixService("go_unix_bad_batch_body")
		srv := startRawPosixSessionServerN(t, svc, cfg, 1,
			func(session *posix.Session, hdr protocol.Header, payload []byte) error {
				if hdr.Kind != protocol.KindRequest || hdr.Code != protocol.MethodIncrement {
					return fmt.Errorf("unexpected request header: %+v", hdr)
				}

				respHdr := protocol.Header{
					Kind:            protocol.KindResponse,
					Code:            protocol.MethodIncrement,
					Flags:           protocol.FlagBatch,
					ItemCount:       hdr.ItemCount,
					MessageID:       hdr.MessageID,
					TransportStatus: protocol.StatusOK,
				}
				return session.Send(&respHdr, nil)
			})

		ccfg := testClientConfig()
		ccfg.MaxRequestBatchItems = 16
		ccfg.MaxResponseBatchItems = 16
		client := NewClient(testRunDir, svc, ccfg)
		defer client.Close()

		refreshUnixClientReady(t, client)

		_, err := client.CallIncrementBatch([]uint64{1, 2})
		if !errors.Is(err, protocol.ErrTruncated) {
			t.Fatalf("CallIncrementBatch error = %v, want %v", err, protocol.ErrTruncated)
		}

		srv.wait(t)
		cleanupAll(svc)
	})
}
