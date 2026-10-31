//go:build windows

package raw

import (
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

// Server is an internal managed server bound to one expected request kind.
type Server struct {
	runDir                       string
	serviceName                  string
	config                       windows.ServerConfig
	expectedMethodCode           uint16
	handler                      DispatchHandler
	running                      atomic.Bool
	learnedRequestPayloadBytes   atomic.Uint32
	learnedResponsePayloadBytes  atomic.Uint32
	requestPayloadGrowthCeiling  uint32
	responsePayloadGrowthCeiling uint32
	nextSessionID                atomic.Uint64
	workerCount                  int
	wg                           sync.WaitGroup
	listenerMu                   sync.Mutex
	listener                     *windows.Listener // stored so Stop() can wake it
	stopRequested                atomic.Bool
	stopInit                     sync.Once
	stopClose                    sync.Once
	stopCh                       chan struct{}
	timings                      serverTimings
	listening                    chan struct{} // closed once the listener accepts
	listeningOnce                sync.Once
	sessionExited                chan struct{} // test hook, nil in production: signalled after a worker frees its slot
	handshakeStarted             chan struct{} // test hook, nil in production: signalled before a worker waits for HELLO
	workerThreadsMu              sync.Mutex
	workerThreads                map[syscall.Handle]struct{}
}

// NewServer creates a new managed server.
func NewServer(
	runDir, serviceName string,
	config windows.ServerConfig,
	expectedMethodCode uint16,
	handler DispatchHandler,
) *Server {
	return NewServerWithWorkers(runDir, serviceName, config, expectedMethodCode, handler, defaultServerWorkerCount)
}

// NewServerWithWorkers creates a server with an explicit worker count limit.
func NewServerWithWorkers(
	runDir, serviceName string,
	config windows.ServerConfig,
	expectedMethodCode uint16,
	handler DispatchHandler,
	workerCount int,
) *Server {
	s := &Server{config: config}
	s.initCommon(
		runDir,
		serviceName,
		expectedMethodCode,
		handler,
		workerCount,
		config.MaxRequestPayloadBytes,
		config.MaxResponsePayloadBytes,
	)
	return s
}

type preparedWinShm struct {
	hybrid   *windows.WinShmContext
	busywait *windows.WinShmContext
}

func (p *preparedWinShm) take(profile uint32) *windows.WinShmContext {
	if p == nil {
		return nil
	}
	switch profile {
	case windows.WinShmProfileHybrid:
		ctx := p.hybrid
		p.hybrid = nil
		return ctx
	case windows.WinShmProfileBusywait:
		ctx := p.busywait
		p.busywait = nil
		return ctx
	default:
		return nil
	}
}

func (p *preparedWinShm) destroyAll() {
	if p == nil {
		return
	}
	if p.hybrid != nil {
		p.hybrid.WinShmDestroy()
		p.hybrid = nil
	}
	if p.busywait != nil {
		p.busywait.WinShmDestroy()
		p.busywait = nil
	}
}

const winShmProfiles = windows.WinShmProfileHybrid | windows.WinShmProfileBusywait

func (s *Server) prepareAcceptConfig() (uint64, windows.ServerConfig, *preparedWinShm, bool) {
	sessionID := s.nextSessionID.Add(1)
	cfg := s.config
	cfg.MaxRequestPayloadBytes = s.learnedRequestPayloadBytes.Load()
	cfg.MaxResponsePayloadBytes = s.learnedResponsePayloadBytes.Load()

	if cfg.SupportedProfiles&winShmProfiles == 0 {
		return sessionID, cfg, nil, true
	}

	prepared := &preparedWinShm{}
	for _, profile := range []uint32{windows.WinShmProfileHybrid, windows.WinShmProfileBusywait} {
		if cfg.SupportedProfiles&profile == 0 {
			continue
		}
		shm, err := windows.WinShmServerCreate(
			s.runDir, s.serviceName,
			cfg.AuthToken,
			sessionID,
			profile,
			cfg.MaxRequestPayloadBytes+uint32(protocol.HeaderSize),
			cfg.MaxResponsePayloadBytes+uint32(protocol.HeaderSize),
		)
		if err != nil {
			cfg.SupportedProfiles &^= profile
			cfg.PreferredProfiles &^= profile
			continue
		}
		if profile == windows.WinShmProfileHybrid {
			prepared.hybrid = shm
		} else {
			prepared.busywait = shm
		}
	}

	if cfg.SupportedProfiles == 0 {
		prepared.destroyAll()
		return sessionID, cfg, nil, false
	}

	if prepared.hybrid == nil && prepared.busywait == nil {
		return sessionID, cfg, nil, true
	}

	return sessionID, cfg, prepared, true
}

// Run starts the acceptor loop. Blocking. Accepts clients and runs each
// connection, handshake included, in its own goroutine (up to workerCount
// concurrently). The server is single-use: a Stop before Run makes Run
// return at once.
func (s *Server) Run() error {
	if s.stopRequested.Load() {
		return nil
	}

	listener, err := windows.Listen(s.runDir, s.serviceName, s.config)
	if err != nil {
		return err
	}
	s.setListener(listener)

	s.running.Store(true)
	// Stop may have run before the listener was published, so it could not
	// wake the accept below.
	if s.stopRequested.Load() {
		s.running.Store(false)
	}
	s.markListening()
	sem := make(chan struct{}, s.workerCount)

	for s.running.Load() {
		pending, err := listener.AcceptPending()
		if err != nil {
			if !s.retryAcceptAfter(nil) {
				break
			}
			continue
		}

		// The stop wake connection, or a client racing the stop.
		if !s.running.Load() {
			pending.Close()
			break
		}

		// Over capacity: close before HELLO_ACK so the client sees a
		// failed connect instead of a session that dies on first use.
		if !s.acquireWorkerSlot(sem, nil, pending.Close) {
			continue
		}

		s.startSessionWorker(sem, func() {
			s.serveConnection(pending)
		})
	}

	s.markStopped()
	s.setListener(nil)
	listener.Close()
	s.stopSessionWorkers()
	return nil
}

// Stop signals the server to stop and wakes its accept. Run owns the
// listener and closes it on exit, so Stop never closes a handle Run may
// still be using.
func (s *Server) Stop() {
	s.markStopped()

	s.listenerMu.Lock()
	listener := s.listener
	s.listenerMu.Unlock()
	if listener == nil {
		return
	}
	// One wake can be lost if it lands between accepts; repeat until Run
	// unpublishes the listener.
	go func() {
		for {
			listener.Wake()
			time.Sleep(serverStopRetry)
			s.listenerMu.Lock()
			current := s.listener
			s.listenerMu.Unlock()
			if current != listener {
				return
			}
		}
	}()
}

func (s *Server) setListener(listener *windows.Listener) {
	s.listenerMu.Lock()
	s.listener = listener
	s.listenerMu.Unlock()
}

func (s *Server) serveConnection(pending *windows.PendingConn) {
	// CancelSynchronousIo targets a thread, so the worker keeps one thread
	// for its whole life and publishes it to the stopper.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	thread := s.trackWorkerThread()
	defer s.untrackWorkerThread(thread)

	sessionID, acceptCfg, preparedShm, ok := s.prepareAcceptConfig()
	if !ok {
		pending.Close()
		return
	}
	s.notifyTestHook(s.handshakeStarted)

	session, err := pending.Handshake(
		sessionID, acceptCfg, s.timings.handshakeMs, s.stopChan())
	if err != nil {
		if preparedShm != nil {
			preparedShm.destroyAll()
		}
		return
	}

	var shm *windows.WinShmContext
	if session.SelectedProfile == windows.WinShmProfileHybrid ||
		session.SelectedProfile == windows.WinShmProfileBusywait {
		shm = preparedShm.take(session.SelectedProfile)
		if shm == nil {
			if preparedShm != nil {
				preparedShm.destroyAll()
			}
			session.Close()
			return
		}
	}
	if preparedShm != nil {
		preparedShm.destroyAll()
	}

	s.handleSession(session, shm)
}

var (
	serverKernel32          = syscall.NewLazyDLL("kernel32.dll")
	procOpenThread          = serverKernel32.NewProc("OpenThread")
	procCancelSynchronousIo = serverKernel32.NewProc("CancelSynchronousIo")
	procGetCurrentThreadID  = serverKernel32.NewProc("GetCurrentThreadId")
	threadTerminateAccess   = uintptr(0x0001)
)

// trackWorkerThread opens a handle to the calling (locked) OS thread and
// registers it for cancellation. Returns 0 if the handle cannot be opened.
func (s *Server) trackWorkerThread() syscall.Handle {
	tid, _, _ := procGetCurrentThreadID.Call()
	h, _, _ := procOpenThread.Call(threadTerminateAccess, 0, tid)
	thread := syscall.Handle(h)
	if thread == 0 {
		return 0
	}
	s.workerThreadsMu.Lock()
	if s.workerThreads == nil {
		s.workerThreads = make(map[syscall.Handle]struct{})
	}
	s.workerThreads[thread] = struct{}{}
	s.workerThreadsMu.Unlock()
	return thread
}

// untrackWorkerThread runs under the stopper's lock, so a cancel never
// reaches a thread that went back to the Go scheduler.
func (s *Server) untrackWorkerThread(thread syscall.Handle) {
	if thread == 0 {
		return
	}
	s.workerThreadsMu.Lock()
	delete(s.workerThreads, thread)
	syscall.CloseHandle(thread)
	s.workerThreadsMu.Unlock()
}

// stopSessionWorkers gives workers a grace period to finish on their own,
// then cancels their blocking pipe I/O until they exit. A single cancel can
// land before a worker enters its next blocking call, so it repeats.
func (s *Server) stopSessionWorkers() {
	done := make(chan struct{})
	go func() {
		s.wg.Wait()
		close(done)
	}()

	select {
	case <-done:
		return
	case <-time.After(s.timings.stopGrace):
	}

	ticker := time.NewTicker(serverStopRetry)
	defer ticker.Stop()
	for {
		s.workerThreadsMu.Lock()
		for thread := range s.workerThreads {
			_, _, _ = procCancelSynchronousIo.Call(uintptr(thread))
		}
		s.workerThreadsMu.Unlock()

		select {
		case <-done:
			return
		case <-ticker.C:
		}
	}
}

func (s *Server) handleSession(session *windows.Session, shm *windows.WinShmContext) {
	s.handleServerSession(serverSessionOps{
		maxRequestPayloadBytes:  session.MaxRequestPayloadBytes,
		maxResponsePayloadBytes: session.MaxResponsePayloadBytes,
		receive: func(recvBuf []byte) (protocol.Header, []byte, serverReceiveAction) {
			if shm != nil {
				mlen, err := shm.WinShmReceive(recvBuf, s.timings.pollMs)
				if err != nil {
					if err == windows.ErrWinShmTimeout {
						// A crashed client never sets the close flag; its
						// broken pipe is the only sign it left.
						if pipePeerDisconnected(session) {
							return protocol.Header{}, nil, serverReceiveStop
						}
						return protocol.Header{}, nil, serverReceiveContinue
					}
					return protocol.Header{}, nil, serverReceiveStop
				}
				if mlen < protocol.HeaderSize {
					return protocol.Header{}, nil, serverReceiveStop
				}
				hdr, err := protocol.DecodeHeader(recvBuf[:mlen])
				if err != nil {
					return protocol.Header{}, nil, serverReceiveStop
				}
				return hdr, recvBuf[protocol.HeaderSize:mlen], serverReceiveOK
			}

			ready, waitErr := session.WaitReadable(s.timings.pollMs)
			if waitErr != nil {
				return protocol.Header{}, nil, serverReceiveStop
			}
			if !ready {
				return protocol.Header{}, nil, serverReceiveContinue
			}

			hdr, payload, err := session.Receive(recvBuf)
			if err != nil {
				return protocol.Header{}, nil, serverReceiveStop
			}
			return hdr, payload, serverReceiveOK
		},
		send: func(respHdr *protocol.Header, payload []byte, msgBuf *[]byte) error {
			if shm == nil {
				return session.Send(respHdr, payload)
			}
			msg, err := serverEncodeSharedResponse(respHdr, payload, msgBuf)
			if err != nil {
				return err
			}
			return shm.WinShmSend(msg)
		},
		close: func() {
			if shm != nil {
				shm.WinShmDestroy()
			}
			session.Close()
		},
	})
}
