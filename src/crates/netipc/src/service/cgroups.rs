//! L2 cgroups snapshot service: client context and managed server.
//!
//! Pure composition of L1 (UDS/SHM) + Codec. No direct socket/mmap calls.
//! Client manages connection lifecycle with at-least-once retry.
//! Server handles accept, read, dispatch, respond.

use crate::protocol::{
    self, CgroupsRequest, CgroupsResponseView, Header, NipcError, HEADER_SIZE, KIND_REQUEST,
    KIND_RESPONSE, MAGIC_MSG, METHOD_CGROUPS_SNAPSHOT, PROFILE_SHM_FUTEX,
    PROFILE_SHM_HYBRID, STATUS_INTERNAL_ERROR, STATUS_OK, VERSION,
};
use crate::transport::posix::{ClientConfig, ServerConfig, UdsListener, UdsSession};

#[cfg(target_os = "linux")]
use crate::transport::shm::ShmContext;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

// ---------------------------------------------------------------------------
//  Client state
// ---------------------------------------------------------------------------

/// Client connection state machine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClientState {
    Disconnected,
    Connecting,
    Ready,
    NotFound,
    AuthFailed,
    Incompatible,
    Broken,
}

/// Diagnostic counters snapshot.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ClientStatus {
    pub state: ClientState,
    pub connect_count: u32,
    pub reconnect_count: u32,
    pub call_count: u32,
    pub error_count: u32,
}

// ---------------------------------------------------------------------------
//  Client context
// ---------------------------------------------------------------------------

/// L2 client context for the cgroups snapshot service.
///
/// Manages connection lifecycle and provides typed blocking calls
/// with at-least-once retry semantics.
pub struct CgroupsClient {
    state: ClientState,
    run_dir: String,
    service_name: String,
    transport_config: ClientConfig,

    // Connection (managed internally)
    session: Option<UdsSession>,
    #[cfg(target_os = "linux")]
    shm: Option<ShmContext>,

    // Stats
    connect_count: u32,
    reconnect_count: u32,
    call_count: u32,
    error_count: u32,
}

impl CgroupsClient {
    /// Create a new client context. Does NOT connect. Does NOT require
    /// the server to be running.
    pub fn new(run_dir: &str, service_name: &str, config: ClientConfig) -> Self {
        CgroupsClient {
            state: ClientState::Disconnected,
            run_dir: run_dir.to_string(),
            service_name: service_name.to_string(),
            transport_config: config,
            session: None,
            #[cfg(target_os = "linux")]
            shm: None,
            connect_count: 0,
            reconnect_count: 0,
            call_count: 0,
            error_count: 0,
        }
    }

    /// Attempt connect if DISCONNECTED/NOT_FOUND, reconnect if BROKEN.
    /// Returns true if the state changed.
    pub fn refresh(&mut self) -> bool {
        let old_state = self.state;

        match self.state {
            ClientState::Disconnected | ClientState::NotFound => {
                self.state = ClientState::Connecting;
                self.state = self.try_connect();
                if self.state == ClientState::Ready {
                    self.connect_count += 1;
                }
            }
            ClientState::Broken => {
                self.disconnect();
                self.state = ClientState::Connecting;
                self.state = self.try_connect();
                if self.state == ClientState::Ready {
                    self.reconnect_count += 1;
                }
            }
            ClientState::Ready
            | ClientState::Connecting
            | ClientState::AuthFailed
            | ClientState::Incompatible => {}
        }

        self.state != old_state
    }

    /// Cheap cached boolean. No I/O, no syscalls.
    #[inline]
    pub fn ready(&self) -> bool {
        self.state == ClientState::Ready
    }

    /// Detailed status snapshot for diagnostics.
    pub fn status(&self) -> ClientStatus {
        ClientStatus {
            state: self.state,
            connect_count: self.connect_count,
            reconnect_count: self.reconnect_count,
            call_count: self.call_count,
            error_count: self.error_count,
        }
    }

    /// Blocking typed call: encode request, send, receive, check
    /// transport_status, decode response.
    ///
    /// `response_buf` must be large enough for the expected snapshot.
    ///
    /// Retry policy (per spec): if the call fails and the context was
    /// previously READY, disconnect, reconnect (full handshake), retry ONCE.
    pub fn call_snapshot<'a>(
        &mut self,
        response_buf: &'a mut [u8],
    ) -> Result<CgroupsResponseView<'a>, NipcError> {
        // Fail fast if not READY
        if self.state != ClientState::Ready {
            self.error_count += 1;
            return Err(NipcError::BadLayout);
        }

        // First attempt: send/receive into response_buf, get payload length
        let first_result = self.do_cgroups_call_raw(response_buf);

        match first_result {
            Ok(payload_len) => {
                // Decode from response_buf
                let view = CgroupsResponseView::decode(&response_buf[..payload_len])?;
                self.call_count += 1;
                return Ok(view);
            }
            Err(first_err) => {
                // Call failed. Was previously READY: disconnect, reconnect, retry ONCE.
                self.disconnect();
                self.state = ClientState::Broken;

                // Reconnect (full handshake)
                self.state = self.try_connect();
                if self.state != ClientState::Ready {
                    self.error_count += 1;
                    return Err(first_err);
                }
                self.reconnect_count += 1;

                // Retry once
                match self.do_cgroups_call_raw(response_buf) {
                    Ok(payload_len) => {
                        let view = CgroupsResponseView::decode(&response_buf[..payload_len])?;
                        self.call_count += 1;
                        Ok(view)
                    }
                    Err(retry_err) => {
                        self.disconnect();
                        self.state = ClientState::Broken;
                        self.error_count += 1;
                        Err(retry_err)
                    }
                }
            }
        }
    }

    /// Tear down connection and release resources.
    pub fn close(&mut self) {
        self.disconnect();
        self.state = ClientState::Disconnected;
    }

    // ------------------------------------------------------------------
    //  Internal helpers
    // ------------------------------------------------------------------

    /// Tear down the current connection (UDS session + SHM if any).
    fn disconnect(&mut self) {
        #[cfg(target_os = "linux")]
        {
            if let Some(mut shm) = self.shm.take() {
                shm.close();
            }
        }

        // Drop the session (closes fd via Drop impl)
        self.session.take();
    }

    /// Attempt a full connection: UDS connect + handshake, then SHM upgrade
    /// if negotiated.
    fn try_connect(&mut self) -> ClientState {
        match UdsSession::connect(&self.run_dir, &self.service_name, &self.transport_config) {
            Ok(session) => {
                #[cfg(target_os = "linux")]
                let selected_profile = session.selected_profile;

                // SHM upgrade if negotiated
                #[cfg(target_os = "linux")]
                {
                    if selected_profile == PROFILE_SHM_HYBRID
                        || selected_profile == PROFILE_SHM_FUTEX
                    {
                        // Retry attach: server creates the SHM region after
                        // the UDS handshake, so it may not exist yet.
                        let mut shm_ok = false;
                        for _ in 0..200 {
                            match ShmContext::client_attach(&self.run_dir, &self.service_name) {
                                Ok(ctx) => {
                                    self.shm = Some(ctx);
                                    shm_ok = true;
                                    break;
                                }
                                Err(_) => {
                                    std::thread::sleep(std::time::Duration::from_millis(5));
                                }
                            }
                        }
                        // If SHM attach failed, fall back to UDS only.
                        if !shm_ok {
                            self.shm = None;
                        }
                    }
                }

                self.session = Some(session);
                ClientState::Ready
            }
            Err(e) => {
                use crate::transport::posix::UdsError;
                match e {
                    UdsError::Connect(_) => ClientState::NotFound,
                    UdsError::AuthFailed => ClientState::AuthFailed,
                    UdsError::NoProfile => ClientState::Incompatible,
                    _ => ClientState::Disconnected,
                }
            }
        }
    }

    /// Single attempt at a cgroups snapshot call.
    /// Returns the payload length on success. The payload bytes are in
    /// response_buf[..payload_len]. Caller decodes after this returns.
    fn do_cgroups_call_raw(
        &mut self,
        response_buf: &mut [u8],
    ) -> Result<usize, NipcError> {
        // 1. Encode request using Codec
        let req = CgroupsRequest {
            layout_version: 1,
            flags: 0,
        };
        let mut req_buf = [0u8; 4];
        let req_len = req.encode(&mut req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        // 2. Build outer header
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: METHOD_CGROUPS_SNAPSHOT,
            flags: 0,
            item_count: 1,
            message_id: (self.call_count as u64) + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        // 3. Send via L1 (SHM or UDS)
        self.transport_send(&mut hdr, &req_buf[..req_len])?;

        // 4. Receive via L1 (payload written into response_buf)
        let (resp_hdr, payload_len) = self.transport_receive(response_buf)?;

        // 5. Check transport_status BEFORE decode (spec requirement)
        if resp_hdr.transport_status != STATUS_OK {
            return Err(NipcError::BadLayout);
        }

        Ok(payload_len)
    }

    /// Send via the active transport (SHM if available, UDS otherwise).
    fn transport_send(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), NipcError> {
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm) = self.shm {
                let msg_len = HEADER_SIZE + payload.len();
                let mut msg = vec![0u8; msg_len];

                hdr.magic = MAGIC_MSG;
                hdr.version = VERSION;
                hdr.header_len = protocol::HEADER_LEN;
                hdr.payload_len = payload.len() as u32;

                hdr.encode(&mut msg[..HEADER_SIZE]);
                if !payload.is_empty() {
                    msg[HEADER_SIZE..].copy_from_slice(payload);
                }

                return shm.send(&msg).map_err(|_| NipcError::Overflow);
            }
        }

        // UDS path
        let session = self.session.as_mut().ok_or(NipcError::Truncated)?;
        session.send(hdr, payload).map_err(|_| NipcError::Overflow)
    }

    /// Receive via the active transport. Returns (header, payload_len).
    /// Payload bytes are written into response_buf[..payload_len].
    fn transport_receive(
        &mut self,
        response_buf: &mut [u8],
    ) -> Result<(Header, usize), NipcError> {
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm) = self.shm {
                let msg = shm.receive(30000).map_err(|_| NipcError::Truncated)?;

                if msg.len() < HEADER_SIZE {
                    return Err(NipcError::Truncated);
                }

                let hdr = Header::decode(msg)?;
                let payload_len = msg.len() - HEADER_SIZE;

                if payload_len > response_buf.len() {
                    return Err(NipcError::Overflow);
                }

                response_buf[..payload_len].copy_from_slice(&msg[HEADER_SIZE..]);
                return Ok((hdr, payload_len));
            }
        }

        // UDS path: receive returns (Header, Vec<u8>)
        let session = self.session.as_mut().ok_or(NipcError::Truncated)?;
        let mut scratch = vec![0u8; response_buf.len() + HEADER_SIZE];
        let (hdr, payload_vec) = session
            .receive(&mut scratch)
            .map_err(|_| NipcError::Truncated)?;

        let payload_len = payload_vec.len();
        if payload_len > response_buf.len() {
            return Err(NipcError::Overflow);
        }
        response_buf[..payload_len].copy_from_slice(&payload_vec);

        Ok((hdr, payload_len))
    }
}

impl Drop for CgroupsClient {
    fn drop(&mut self) {
        self.close();
    }
}

// ---------------------------------------------------------------------------
//  Managed server
// ---------------------------------------------------------------------------

/// Handler function type. Receives (method_code, request_payload).
/// Returns Some(response_payload) on success, None on failure
/// (which maps to INTERNAL_ERROR + empty payload).
pub type HandlerFn = Box<dyn FnMut(u16, &[u8]) -> Option<Vec<u8>> + Send>;

/// L2 managed server for the cgroups snapshot service.
///
/// Handles accept, read, dispatch to handler, respond.
/// Single-threaded acceptor loop (mirrors C implementation).
pub struct CgroupsServer {
    run_dir: String,
    service_name: String,
    server_config: ServerConfig,
    handler: HandlerFn,
    running: Arc<AtomicBool>,
}

impl CgroupsServer {
    /// Create a new managed server. Does NOT start listening yet.
    pub fn new(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        _response_buf_size: usize,
        handler: HandlerFn,
    ) -> Self {
        CgroupsServer {
            run_dir: run_dir.to_string(),
            service_name: service_name.to_string(),
            server_config: config,
            handler,
            running: Arc::new(AtomicBool::new(false)),
        }
    }

    /// Run the acceptor loop. Blocking. Accepts clients, reads requests,
    /// dispatches to the handler, sends responses.
    ///
    /// Returns when `stop()` is called or on fatal error.
    pub fn run(&mut self) -> Result<(), NipcError> {
        let listener = UdsListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        self.running.store(true, Ordering::Release);

        while self.running.load(Ordering::Acquire) {
            // Poll the listener fd before blocking on accept
            let ready = poll_fd(listener.fd(), 500);
            if ready < 0 {
                break;
            }
            if ready == 0 {
                continue; // timeout, check running flag
            }

            // Accept one client via L1
            let session = match listener.accept() {
                Ok(s) => s,
                Err(_) => {
                    if !self.running.load(Ordering::Acquire) {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                    continue;
                }
            };

            // SHM upgrade if negotiated
            #[cfg(target_os = "linux")]
            let shm = self.try_shm_upgrade(&session);
            #[cfg(not(target_os = "linux"))]
            let shm: Option<()> = None;

            // Handle this session (blocking, single-threaded)
            self.handle_session(
                session,
                #[cfg(target_os = "linux")]
                shm,
                #[cfg(not(target_os = "linux"))]
                shm,
            );
        }

        Ok(())
    }

    /// Signal shutdown.
    pub fn stop(&self) {
        self.running.store(false, Ordering::Release);
    }

    /// Returns an Arc to the running flag for external stop signaling.
    pub fn running_flag(&self) -> Arc<AtomicBool> {
        self.running.clone()
    }

    // ------------------------------------------------------------------
    //  Internal helpers
    // ------------------------------------------------------------------

    #[cfg(target_os = "linux")]
    fn try_shm_upgrade(&self, session: &UdsSession) -> Option<ShmContext> {
        let profile = session.selected_profile;
        if profile != PROFILE_SHM_HYBRID && profile != PROFILE_SHM_FUTEX {
            return None;
        }

        match ShmContext::server_create(
            &self.run_dir,
            &self.service_name,
            session.max_request_payload_bytes + HEADER_SIZE as u32,
            session.max_response_payload_bytes + HEADER_SIZE as u32,
        ) {
            Ok(ctx) => Some(ctx),
            Err(_) => None,
        }
    }

    /// Handle one client session.
    fn handle_session(
        &mut self,
        mut session: UdsSession,
        #[cfg(target_os = "linux")] mut shm: Option<ShmContext>,
        #[cfg(not(target_os = "linux"))] _shm: Option<()>,
    ) {
        let mut recv_buf = vec![0u8; 65536];

        while self.running.load(Ordering::Acquire) {
            // Receive request via the active transport
            let (hdr, payload) = {
                #[cfg(target_os = "linux")]
                {
                    if let Some(ref mut shm_ctx) = shm {
                        match shm_ctx.receive(500) {
                            Ok(msg) => {
                                if msg.len() < HEADER_SIZE {
                                    break;
                                }
                                let hdr = match Header::decode(msg) {
                                    Ok(h) => h,
                                    Err(_) => break,
                                };
                                let payload = msg[HEADER_SIZE..].to_vec();
                                (hdr, payload)
                            }
                            Err(crate::transport::shm::ShmError::Timeout) => continue,
                            Err(_) => break,
                        }
                    } else {
                        // UDS path with poll
                        let ready = poll_fd(session.fd(), 500);
                        if ready < 0 {
                            break;
                        }
                        if ready == 0 {
                            continue;
                        }

                        match session.receive(&mut recv_buf) {
                            Ok((hdr, payload)) => (hdr, payload.to_vec()),
                            Err(_) => break,
                        }
                    }
                }

                #[cfg(not(target_os = "linux"))]
                {
                    let ready = poll_fd(session.fd(), 500);
                    if ready < 0 {
                        break;
                    }
                    if ready == 0 {
                        continue;
                    }

                    match session.receive(&mut recv_buf) {
                        Ok((hdr, payload)) => (hdr, payload.to_vec()),
                        Err(_) => break,
                    }
                }
            };

            // Skip non-request messages
            if hdr.kind != KIND_REQUEST {
                continue;
            }

            // Dispatch to handler
            let handler_result = (self.handler)(hdr.code, &payload);

            // Build response header
            let mut resp_hdr = Header {
                kind: KIND_RESPONSE,
                code: hdr.code,
                message_id: hdr.message_id,
                item_count: 1,
                flags: 0,
                ..Header::default()
            };

            let response_payload = match handler_result {
                Some(data) => {
                    resp_hdr.transport_status = STATUS_OK;
                    data
                }
                None => {
                    // Handler failure: INTERNAL_ERROR + empty payload
                    resp_hdr.transport_status = STATUS_INTERNAL_ERROR;
                    Vec::new()
                }
            };

            // Send response via the active transport
            #[cfg(target_os = "linux")]
            {
                if let Some(ref mut shm_ctx) = shm {
                    let msg_len = HEADER_SIZE + response_payload.len();
                    let mut msg = vec![0u8; msg_len];

                    resp_hdr.magic = MAGIC_MSG;
                    resp_hdr.version = VERSION;
                    resp_hdr.header_len = protocol::HEADER_LEN;
                    resp_hdr.payload_len = response_payload.len() as u32;

                    resp_hdr.encode(&mut msg[..HEADER_SIZE]);
                    if !response_payload.is_empty() {
                        msg[HEADER_SIZE..].copy_from_slice(&response_payload);
                    }

                    if shm_ctx.send(&msg).is_err() {
                        break;
                    }
                    continue;
                }
            }

            // UDS path
            if session.send(&mut resp_hdr, &response_payload).is_err() {
                break;
            }
        }

        // Cleanup
        #[cfg(target_os = "linux")]
        {
            if let Some(mut shm_ctx) = shm {
                shm_ctx.destroy();
            }
        }
        drop(session);
    }
}

// ---------------------------------------------------------------------------
//  Internal: poll helper
// ---------------------------------------------------------------------------

/// Poll a file descriptor for readability with a timeout in milliseconds.
/// Returns: 1 = data ready, 0 = timeout, -1 = error/hangup.
fn poll_fd(fd: i32, timeout_ms: i32) -> i32 {
    let mut pfd = libc::pollfd {
        fd,
        events: libc::POLLIN,
        revents: 0,
    };

    let ret = unsafe { libc::poll(&mut pfd, 1, timeout_ms) };

    if ret < 0 {
        let errno = unsafe { *libc::__errno_location() };
        if errno == libc::EINTR {
            return 0;
        }
        return -1;
    }

    if ret == 0 {
        return 0;
    }

    if pfd.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
        return -1;
    }

    if pfd.revents & libc::POLLIN != 0 {
        return 1;
    }

    0
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{CgroupsBuilder, PROFILE_BASELINE};
    use std::thread;
    use std::time::Duration;

    const TEST_RUN_DIR: &str = "/tmp/nipc_svc_rust_test";
    const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
    const RESPONSE_BUF_SIZE: usize = 65536;

    fn ensure_run_dir() {
        let _ = std::fs::create_dir_all(TEST_RUN_DIR);
    }

    fn cleanup_all(service: &str) {
        let _ = std::fs::remove_file(format!("{TEST_RUN_DIR}/{service}.sock"));
        let _ = std::fs::remove_file(format!("{TEST_RUN_DIR}/{service}.ipcshm"));
    }

    fn server_config() -> ServerConfig {
        ServerConfig {
            supported_profiles: PROFILE_BASELINE,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 1,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 1,
            auth_token: AUTH_TOKEN,
            backlog: 4,
            ..ServerConfig::default()
        }
    }

    fn client_config() -> ClientConfig {
        ClientConfig {
            supported_profiles: PROFILE_BASELINE,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 1,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 1,
            auth_token: AUTH_TOKEN,
            ..ClientConfig::default()
        }
    }

    /// Test handler: build a snapshot with 3 items.
    fn test_cgroups_handler(method_code: u16, request_payload: &[u8]) -> Option<Vec<u8>> {
        if method_code != METHOD_CGROUPS_SNAPSHOT {
            return None;
        }

        // Validate request
        if CgroupsRequest::decode(request_payload).is_err() {
            return None;
        }

        let mut buf = vec![0u8; RESPONSE_BUF_SIZE];
        let mut builder = CgroupsBuilder::new(&mut buf, 3, 1, 42);

        let items = [
            (1001u32, 0u32, 1u32, b"docker-abc123" as &[u8], b"/sys/fs/cgroup/docker/abc123" as &[u8]),
            (2002, 0, 1, b"k8s-pod-xyz", b"/sys/fs/cgroup/kubepods/xyz"),
            (3003, 0, 0, b"systemd-user", b"/sys/fs/cgroup/user.slice/user-1000"),
        ];

        for (hash, options, enabled, name, path) in &items {
            if builder.add(*hash, *options, *enabled, name, path).is_err() {
                return None;
            }
        }

        let total = builder.finish();
        buf.truncate(total);
        Some(buf)
    }

    /// Handler that always fails.
    fn failing_handler(_method_code: u16, _request_payload: &[u8]) -> Option<Vec<u8>> {
        None
    }

    struct TestServer {
        stop_flag: Arc<AtomicBool>,
        thread: Option<thread::JoinHandle<()>>,
    }

    impl TestServer {
        fn start(
            service: &str,
            handler: fn(u16, &[u8]) -> Option<Vec<u8>>,
        ) -> Self {
            ensure_run_dir();
            cleanup_all(service);

            let svc = service.to_string();
            let ready_flag = Arc::new(AtomicBool::new(false));
            let ready_clone = ready_flag.clone();

            let mut server = CgroupsServer::new(
                TEST_RUN_DIR,
                &svc,
                server_config(),
                RESPONSE_BUF_SIZE,
                Box::new(move |code, payload| handler(code, payload)),
            );
            let stop_flag = server.running_flag();

            let thread = thread::spawn(move || {
                // We need to signal readiness after bind but before accept loop.
                // The run() method binds internally, so we signal immediately
                // after it starts (it blocks on accept).
                ready_clone.store(true, Ordering::Release);
                let _ = server.run();
            });

            // Wait for server to be ready
            for _ in 0..2000 {
                if ready_flag.load(Ordering::Acquire) {
                    break;
                }
                thread::sleep(Duration::from_micros(500));
            }
            // Extra settle time for listener bind
            thread::sleep(Duration::from_millis(50));

            TestServer {
                stop_flag,
                thread: Some(thread),
            }
        }

        fn stop(&mut self) {
            self.stop_flag.store(false, Ordering::Release);
            if let Some(t) = self.thread.take() {
                let _ = t.join();
            }
        }
    }

    impl Drop for TestServer {
        fn drop(&mut self) {
            self.stop();
        }
    }

    #[test]
    fn test_client_lifecycle() {
        let svc = "rs_svc_lifecycle";
        ensure_run_dir();
        cleanup_all(svc);

        // Init without server running
        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        assert_eq!(client.state, ClientState::Disconnected);
        assert!(!client.ready());

        // Refresh without server -> NOT_FOUND
        let changed = client.refresh();
        assert!(changed);
        assert_eq!(client.state, ClientState::NotFound);

        // Start server
        let mut server = TestServer::start(svc, test_cgroups_handler);

        // Refresh -> READY
        let changed = client.refresh();
        assert!(changed);
        assert_eq!(client.state, ClientState::Ready);
        assert!(client.ready());

        // Status reporting
        let status = client.status();
        assert_eq!(status.connect_count, 1);
        assert_eq!(status.reconnect_count, 0);

        // Close
        client.close();
        assert_eq!(client.state, ClientState::Disconnected);
        assert!(!client.ready());

        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_cgroups_call() {
        let svc = "rs_svc_cgroups";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
        let view = client.call_snapshot(&mut resp_buf).expect("call should succeed");

        assert_eq!(view.item_count, 3);
        assert_eq!(view.systemd_enabled, 1);
        assert_eq!(view.generation, 42);

        // Verify first item
        let item0 = view.item(0).expect("item 0");
        assert_eq!(item0.hash, 1001);
        assert_eq!(item0.enabled, 1);
        assert_eq!(item0.name.as_bytes(), b"docker-abc123");
        assert_eq!(item0.path.as_bytes(), b"/sys/fs/cgroup/docker/abc123");

        // Verify third item
        let item2 = view.item(2).expect("item 2");
        assert_eq!(item2.hash, 3003);
        assert_eq!(item2.enabled, 0);
        assert_eq!(item2.name.as_bytes(), b"systemd-user");

        // Verify stats
        let status = client.status();
        assert_eq!(status.call_count, 1);
        assert_eq!(status.error_count, 0);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_retry_on_failure() {
        let svc = "rs_svc_retry";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server1 = TestServer::start(svc, test_cgroups_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        // First call succeeds
        let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
        let view = client.call_snapshot(&mut resp_buf).expect("first call");
        assert_eq!(view.item_count, 3);

        // Kill server
        server1.stop();
        cleanup_all(svc);
        thread::sleep(Duration::from_millis(50));

        // Restart server
        let mut server2 = TestServer::start(svc, test_cgroups_handler);

        // Next call triggers reconnect + retry
        let mut resp_buf2 = vec![0u8; RESPONSE_BUF_SIZE];
        let view2 = client.call_snapshot(&mut resp_buf2).expect("retry call");
        assert_eq!(view2.item_count, 3);

        // Verify reconnect happened
        let status = client.status();
        assert!(status.reconnect_count >= 1);

        client.close();
        server2.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_multiple_clients() {
        let svc = "rs_svc_multi";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        // Create and connect client 1
        let mut client1 = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client1.refresh();
        assert!(client1.ready());

        let mut resp_buf1 = vec![0u8; RESPONSE_BUF_SIZE];
        let view1 = client1.call_snapshot(&mut resp_buf1).expect("client 1 call");
        assert_eq!(view1.item_count, 3);

        // Close client 1 so server can accept client 2 (single-threaded)
        client1.close();

        // Create and connect client 2
        let mut client2 = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client2.refresh();
        assert!(client2.ready());

        let mut resp_buf2 = vec![0u8; RESPONSE_BUF_SIZE];
        let view2 = client2.call_snapshot(&mut resp_buf2).expect("client 2 call");
        assert_eq!(view2.item_count, 3);

        client2.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_handler_failure() {
        let svc = "rs_svc_hfail";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, failing_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        // Call should fail (handler returns None -> INTERNAL_ERROR)
        let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
        let err = client.call_snapshot(&mut resp_buf);
        assert!(err.is_err());

        let status = client.status();
        assert!(status.error_count >= 1);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_status_reporting() {
        let svc = "rs_svc_status";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        // Initial counters
        let s0 = client.status();
        assert_eq!(s0.connect_count, 1);
        assert_eq!(s0.call_count, 0);
        assert_eq!(s0.error_count, 0);

        // Make 3 successful calls
        for _ in 0..3 {
            let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
            client.call_snapshot(&mut resp_buf).expect("call ok");
        }

        let s1 = client.status();
        assert_eq!(s1.call_count, 3);
        assert_eq!(s1.error_count, 0);

        // Call on disconnected client
        client.close();
        let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
        let err = client.call_snapshot(&mut resp_buf);
        assert!(err.is_err());

        let s2 = client.status();
        assert_eq!(s2.error_count, 1);

        server.stop();
        cleanup_all(svc);
    }
}
