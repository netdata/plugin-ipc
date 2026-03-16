//! L2 cgroups service: client context, managed server, and L3 cache.
//!
//! Pure composition of L1 (UDS/SHM on POSIX, Named Pipe/Win SHM on Windows)
//! + Codec. No direct socket/mmap calls.
//! Client manages connection lifecycle with at-least-once retry.
//! Server handles accept, read, dispatch, respond.

use crate::protocol::{
    self, CgroupsRequest, CgroupsResponseView, Header, NipcError, HEADER_SIZE,
    KIND_REQUEST, KIND_RESPONSE, MAGIC_MSG, METHOD_CGROUPS_SNAPSHOT, METHOD_INCREMENT,
    METHOD_STRING_REVERSE, STATUS_INTERNAL_ERROR, STATUS_OK, VERSION, FLAG_BATCH,
    increment_decode, increment_encode, INCREMENT_PAYLOAD_SIZE,
    string_reverse_decode, string_reverse_encode, STRING_REVERSE_HDR_SIZE,
    BatchBuilder, batch_item_get,
};

#[cfg(unix)]
use crate::protocol::{PROFILE_SHM_FUTEX, PROFILE_SHM_HYBRID};

#[cfg(unix)]
use crate::transport::posix::{ClientConfig, ServerConfig, UdsListener, UdsSession};

#[cfg(target_os = "linux")]
use crate::transport::shm::ShmContext;

#[cfg(windows)]
use crate::transport::windows::{
    ClientConfig, ServerConfig, NpSession, NpListener, NpError,
};

#[cfg(windows)]
use crate::transport::win_shm::{
    WinShmContext, PROFILE_HYBRID as WIN_SHM_PROFILE_HYBRID,
    PROFILE_BUSYWAIT as WIN_SHM_PROFILE_BUSYWAIT,
};

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

/// Poll/receive timeout for server loops (ms). Controls shutdown detection latency.
const SERVER_POLL_TIMEOUT_MS: u32 = 100;

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
    #[cfg(unix)]
    session: Option<UdsSession>,
    #[cfg(target_os = "linux")]
    shm: Option<ShmContext>,

    #[cfg(windows)]
    session: Option<NpSession>,
    #[cfg(windows)]
    shm: Option<WinShmContext>,

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
            #[cfg(windows)]
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
        let req = CgroupsRequest {
            layout_version: 1,
            flags: 0,
        };
        let mut req_buf = [0u8; 4];
        let req_len = req.encode(&mut req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        let payload_len = self.call_with_retry(|client| {
            client.do_raw_call(
                METHOD_CGROUPS_SNAPSHOT,
                &req_buf[..req_len],
                response_buf,
            )
        })?;

        CgroupsResponseView::decode(&response_buf[..payload_len])
    }

    /// Blocking typed call: INCREMENT method.
    /// Sends a u64 value, receives the incremented u64 back.
    pub fn call_increment(&mut self, value: u64) -> Result<u64, NipcError> {
        let mut req_buf = [0u8; INCREMENT_PAYLOAD_SIZE];
        let req_len = increment_encode(value, &mut req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        let mut response_buf = [0u8; INCREMENT_PAYLOAD_SIZE];
        let payload_len = self.call_with_retry(|client| {
            client.do_raw_call(
                METHOD_INCREMENT,
                &req_buf[..req_len],
                &mut response_buf,
            )
        })?;

        increment_decode(&response_buf[..payload_len])
    }

    /// Blocking typed call: STRING_REVERSE method.
    /// Sends a string, receives the reversed string back.
    /// Returns the reversed string as an owned `String`.
    pub fn call_string_reverse(&mut self, s: &str) -> Result<String, NipcError> {
        let req_size = STRING_REVERSE_HDR_SIZE + s.len() + 1;
        let mut req_buf = vec![0u8; req_size];
        let req_len = string_reverse_encode(s.as_bytes(), &mut req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        let resp_size = STRING_REVERSE_HDR_SIZE + s.len() + 1;
        let mut response_buf = vec![0u8; resp_size];
        let payload_len = self.call_with_retry(|client| {
            client.do_raw_call(
                METHOD_STRING_REVERSE,
                &req_buf[..req_len],
                &mut response_buf,
            )
        })?;

        let view = string_reverse_decode(&response_buf[..payload_len])?;
        Ok(view.as_str().to_string())
    }

    /// Blocking typed batch call: INCREMENT method.
    /// Sends multiple u64 values, receives the incremented u64s back.
    pub fn call_increment_batch(&mut self, values: &[u64]) -> Result<Vec<u64>, NipcError> {
        if values.is_empty() {
            return Ok(Vec::new());
        }

        // Single value: use the non-batch path
        if values.len() == 1 {
            let r = self.call_increment(values[0])?;
            return Ok(vec![r]);
        }

        let count = values.len() as u32;

        // Pre-encode all items
        let mut encoded_items: Vec<[u8; INCREMENT_PAYLOAD_SIZE]> = Vec::with_capacity(values.len());
        for &v in values {
            let mut item_buf = [0u8; INCREMENT_PAYLOAD_SIZE];
            let n = increment_encode(v, &mut item_buf);
            if n == 0 {
                return Err(NipcError::Truncated);
            }
            encoded_items.push(item_buf);
        }

        // Response buffer large enough for batch directory + packed items
        let resp_buf_size = protocol::align8(count as usize * 8)
            + count as usize * protocol::align8(INCREMENT_PAYLOAD_SIZE)
            + 64;
        let mut response_buf = vec![0u8; resp_buf_size];

        let payload_len = self.call_with_retry(|client| {
            // Build batch request payload
            let req_buf_size = protocol::align8(count as usize * 8)
                + count as usize * protocol::align8(INCREMENT_PAYLOAD_SIZE)
                + 64;
            let mut req_buf = vec![0u8; req_buf_size];
            let mut bb = BatchBuilder::new(&mut req_buf, count);

            for item in &encoded_items {
                bb.add(item).map_err(|_| NipcError::Overflow)?;
            }

            let (req_len, _out_count) = bb.finish();

            client.do_raw_batch_call(
                METHOD_INCREMENT,
                &req_buf[..req_len],
                count,
                &mut response_buf,
            )
        })?;

        // Extract and decode each response item
        let resp_payload = &response_buf[..payload_len];
        let mut results = Vec::with_capacity(values.len());
        for i in 0..count {
            let (item_data, _item_len) = batch_item_get(resp_payload, count, i)?;
            let val = increment_decode(item_data)?;
            results.push(val);
        }

        Ok(results)
    }

    // ------------------------------------------------------------------
    //  Generic retry wrapper
    // ------------------------------------------------------------------

    /// Execute `attempt` with at-least-once retry semantics.
    /// If the first attempt fails and the client was READY, disconnect,
    /// reconnect (full handshake), and retry ONCE.
    fn call_with_retry<F, T>(&mut self, mut attempt: F) -> Result<T, NipcError>
    where
        F: FnMut(&mut Self) -> Result<T, NipcError>,
    {
        if self.state != ClientState::Ready {
            self.error_count += 1;
            return Err(NipcError::BadLayout);
        }

        match attempt(self) {
            Ok(val) => {
                self.call_count += 1;
                Ok(val)
            }
            Err(first_err) => {
                self.disconnect();
                self.state = ClientState::Broken;

                self.state = self.try_connect();
                if self.state != ClientState::Ready {
                    self.error_count += 1;
                    return Err(first_err);
                }
                self.reconnect_count += 1;

                match attempt(self) {
                    Ok(val) => {
                        self.call_count += 1;
                        Ok(val)
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

    /// Tear down the current connection.
    fn disconnect(&mut self) {
        #[cfg(target_os = "linux")]
        {
            if let Some(mut shm) = self.shm.take() {
                shm.close();
            }
        }

        #[cfg(windows)]
        {
            if let Some(mut shm) = self.shm.take() {
                shm.close();
            }
        }

        // Drop the session (closes handle/fd via Drop impl)
        self.session.take();
    }

    /// Attempt a full connection: transport connect + handshake, then SHM
    /// upgrade if negotiated.
    #[cfg(unix)]
    fn try_connect(&mut self) -> ClientState {
        match UdsSession::connect(&self.run_dir, &self.service_name, &self.transport_config) {
            Ok(session) => {
                #[cfg(target_os = "linux")]
                let selected_profile = session.selected_profile;
                #[cfg(target_os = "linux")]
                let session_id = session.session_id;

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
                            match ShmContext::client_attach(&self.run_dir, &self.service_name, session_id) {
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
                        if !shm_ok {
                            // SHM attach failed. Fail the session to avoid transport desync.
                            self.session.take();
                            return ClientState::Disconnected;
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

    /// Windows: attempt a full Named Pipe connection + Win SHM upgrade.
    #[cfg(windows)]
    fn try_connect(&mut self) -> ClientState {
        match NpSession::connect(&self.run_dir, &self.service_name, &self.transport_config) {
            Ok(session) => {
                let selected_profile = session.selected_profile;

                // Win SHM upgrade if negotiated
                if selected_profile == WIN_SHM_PROFILE_HYBRID
                    || selected_profile == WIN_SHM_PROFILE_BUSYWAIT
                {
                    let mut shm_ok = false;
                    for _ in 0..200 {
                        match WinShmContext::client_attach(
                            &self.run_dir,
                            &self.service_name,
                            self.transport_config.auth_token,
                            session.session_id,
                            selected_profile,
                        ) {
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
                    if !shm_ok {
                        // SHM attach failed. Fail the session to avoid transport desync.
                        self.session.take();
                        return ClientState::Disconnected;
                    }
                }

                self.session = Some(session);
                ClientState::Ready
            }
            Err(e) => match e {
                NpError::Connect(_) => ClientState::NotFound,
                NpError::AuthFailed => ClientState::AuthFailed,
                NpError::NoProfile => ClientState::Incompatible,
                _ => ClientState::Disconnected,
            },
        }
    }

    /// Single attempt at a raw call for any method.
    /// `method_code` identifies the method. `request_payload` is the
    /// already-encoded request payload. Returns the response payload
    /// length on success. The payload bytes are in
    /// response_buf[..payload_len].
    fn do_raw_call(
        &mut self,
        method_code: u16,
        request_payload: &[u8],
        response_buf: &mut [u8],
    ) -> Result<usize, NipcError> {
        // 1. Build outer header
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: method_code,
            flags: 0,
            item_count: 1,
            message_id: (self.call_count as u64) + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        // 2. Send via L1 (SHM or UDS)
        self.transport_send(&mut hdr, request_payload)?;

        // 3. Receive via L1 (payload written into response_buf)
        let (resp_hdr, payload_len) = self.transport_receive(response_buf)?;

        // 4. Verify response envelope fields before decode
        if resp_hdr.kind != KIND_RESPONSE {
            return Err(NipcError::BadKind);
        }
        if resp_hdr.code != method_code {
            return Err(NipcError::BadLayout);
        }
        if resp_hdr.message_id != hdr.message_id {
            return Err(NipcError::BadLayout);
        }

        // 5. Check transport_status BEFORE decode (spec requirement)
        if resp_hdr.transport_status != STATUS_OK {
            return Err(NipcError::BadLayout);
        }

        Ok(payload_len)
    }

    /// Single attempt at a raw batch call. Like `do_raw_call` but sets
    /// FLAG_BATCH and item_count, and validates the response matches.
    fn do_raw_batch_call(
        &mut self,
        method_code: u16,
        request_payload: &[u8],
        item_count: u32,
        response_buf: &mut [u8],
    ) -> Result<usize, NipcError> {
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: method_code,
            flags: FLAG_BATCH,
            item_count,
            message_id: (self.call_count as u64) + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        self.transport_send(&mut hdr, request_payload)?;

        let (resp_hdr, payload_len) = self.transport_receive(response_buf)?;

        if resp_hdr.kind != KIND_RESPONSE {
            return Err(NipcError::BadKind);
        }
        if resp_hdr.code != method_code {
            return Err(NipcError::BadLayout);
        }
        if resp_hdr.message_id != hdr.message_id {
            return Err(NipcError::BadLayout);
        }
        if resp_hdr.transport_status != STATUS_OK {
            return Err(NipcError::BadLayout);
        }
        if resp_hdr.item_count != item_count {
            return Err(NipcError::BadItemCount);
        }

        Ok(payload_len)
    }

    /// Send via the active transport (SHM if available, baseline otherwise).
    fn transport_send(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), NipcError> {
        // SHM path (POSIX or Windows)
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

        #[cfg(windows)]
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

        // Baseline transport path
        let session = self.session.as_mut().ok_or(NipcError::Truncated)?;
        session.send(hdr, payload).map_err(|_| NipcError::Overflow)
    }

    /// Receive via the active transport. Returns (header, payload_len).
    /// Payload bytes are written into response_buf[..payload_len].
    fn transport_receive(
        &mut self,
        response_buf: &mut [u8],
    ) -> Result<(Header, usize), NipcError> {
        // SHM path (POSIX or Windows)
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm) = self.shm {
                let mut shm_buf = vec![0u8; response_buf.len() + HEADER_SIZE];
                let mlen = shm.receive(&mut shm_buf, 30000).map_err(|_| NipcError::Truncated)?;

                if mlen < HEADER_SIZE {
                    return Err(NipcError::Truncated);
                }

                let hdr = Header::decode(&shm_buf[..mlen])?;
                let payload_len = mlen - HEADER_SIZE;

                if payload_len > response_buf.len() {
                    return Err(NipcError::Overflow);
                }

                response_buf[..payload_len].copy_from_slice(&shm_buf[HEADER_SIZE..mlen]);
                return Ok((hdr, payload_len));
            }
        }

        #[cfg(windows)]
        {
            if let Some(ref mut shm) = self.shm {
                let mut shm_buf = vec![0u8; response_buf.len() + HEADER_SIZE];
                let mlen = shm.receive(&mut shm_buf, 30000).map_err(|_| NipcError::Truncated)?;

                if mlen < HEADER_SIZE {
                    return Err(NipcError::Truncated);
                }

                let hdr = Header::decode(&shm_buf[..mlen])?;
                let payload_len = mlen - HEADER_SIZE;

                if payload_len > response_buf.len() {
                    return Err(NipcError::Overflow);
                }

                response_buf[..payload_len].copy_from_slice(&shm_buf[HEADER_SIZE..mlen]);
                return Ok((hdr, payload_len));
            }
        }

        // Baseline transport: UDS on POSIX, Named Pipe on Windows
        let session = self.session.as_mut().ok_or(NipcError::Truncated)?;

        #[cfg(unix)]
        {
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

        #[cfg(windows)]
        {
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
///
/// Must be Fn (not FnMut) + Send + Sync for multi-client concurrency.
pub type HandlerFn = Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync>;

/// L2 managed server. Generic request/response dispatcher.
///
/// Handles accept, spawns a thread per session (up to worker_count),
/// reads requests, dispatches to handler, sends responses.
pub struct ManagedServer {
    run_dir: String,
    service_name: String,
    server_config: ServerConfig,
    handler: HandlerFn,
    running: Arc<AtomicBool>,
    worker_count: usize,
    /// Windows: stored listener handle so stop() can close it to unblock Accept.
    #[cfg(windows)]
    listener_handle: Arc<std::sync::Mutex<Option<usize>>>,
}

impl ManagedServer {
    /// Create a new managed server. Does NOT start listening yet.
    pub fn new(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        _response_buf_size: usize,
        handler: HandlerFn,
    ) -> Self {
        Self::with_workers(run_dir, service_name, config, _response_buf_size, handler, 8)
    }

    /// Create a server with an explicit worker count limit.
    pub fn with_workers(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        _response_buf_size: usize,
        handler: HandlerFn,
        worker_count: usize,
    ) -> Self {
        ManagedServer {
            run_dir: run_dir.to_string(),
            service_name: service_name.to_string(),
            server_config: config,
            handler,
            running: Arc::new(AtomicBool::new(false)),
            worker_count: if worker_count < 1 { 1 } else { worker_count },
            #[cfg(windows)]
            listener_handle: Arc::new(std::sync::Mutex::new(None)),
        }
    }

    /// Run the acceptor loop. Blocking. Accepts clients, spawns a
    /// thread per session (up to worker_count concurrent sessions).
    ///
    /// Returns when `stop()` is called or on fatal error.
    #[cfg(unix)]
    pub fn run(&mut self) -> Result<(), NipcError> {
        #[cfg(target_os = "linux")]
        crate::transport::shm::cleanup_stale(&self.run_dir, &self.service_name);

        let listener = UdsListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        self.running.store(true, Ordering::Release);

        let mut session_threads: Vec<std::thread::JoinHandle<()>> = Vec::new();

        while self.running.load(Ordering::Acquire) {
            let ready = poll_fd(listener.fd(), SERVER_POLL_TIMEOUT_MS as i32);
            if ready < 0 {
                break;
            }
            if ready == 0 {
                // Reap finished threads periodically
                session_threads.retain(|t| !t.is_finished());
                continue;
            }

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

            // Check worker count limit (non-blocking)
            // Reap finished threads first
            session_threads.retain(|t| !t.is_finished());
            if session_threads.len() >= self.worker_count {
                // At capacity: reject client
                drop(session);
                continue;
            }

            #[cfg(target_os = "linux")]
            let shm = self.try_shm_upgrade(&session);
            #[cfg(not(target_os = "linux"))]
            let shm: Option<()> = None;

            // If SHM was negotiated but create failed, reject the session
            #[cfg(target_os = "linux")]
            {
                if shm.is_none()
                    && (session.selected_profile == PROFILE_SHM_HYBRID
                        || session.selected_profile == PROFILE_SHM_FUTEX)
                {
                    drop(session);
                    continue;
                }
            }

            // Spawn a handler thread for this session
            let handler = self.handler.clone();
            let running = self.running.clone();

            let t = std::thread::spawn(move || {
                handle_session_threaded(
                    session,
                    #[cfg(target_os = "linux")]
                    shm,
                    #[cfg(not(target_os = "linux"))]
                    shm,
                    handler,
                    running,
                );
            });
            session_threads.push(t);
        }

        // Wait for all active session threads
        for t in session_threads {
            let _ = t.join();
        }

        Ok(())
    }

    /// Windows: run the acceptor loop over Named Pipes.
    #[cfg(windows)]
    pub fn run(&mut self) -> Result<(), NipcError> {
        // Win SHM cleanup is a no-op: kernel objects auto-clean on handle close.

        let mut listener = NpListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        // Store listener handle so stop() can close it to unblock Accept
        *self.listener_handle.lock().unwrap() = Some(listener.handle() as usize);

        self.running.store(true, Ordering::Release);

        let mut session_threads: Vec<std::thread::JoinHandle<()>> = Vec::new();

        while self.running.load(Ordering::Acquire) {
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

            // Reap finished threads
            session_threads.retain(|t| !t.is_finished());
            if session_threads.len() >= self.worker_count {
                drop(session);
                continue;
            }

            let shm = self.try_win_shm_upgrade(&session);

            // If SHM was negotiated but create failed, reject the session
            if shm.is_none()
                && (session.selected_profile == WIN_SHM_PROFILE_HYBRID
                    || session.selected_profile == WIN_SHM_PROFILE_BUSYWAIT)
            {
                drop(session);
                continue;
            }

            let handler = self.handler.clone();
            let running = self.running.clone();

            let t = std::thread::spawn(move || {
                handle_session_win_threaded(session, shm, handler, running);
            });
            session_threads.push(t);
        }

        for t in session_threads {
            let _ = t.join();
        }

        Ok(())
    }

    /// Signal shutdown. On Windows, also closes the listener pipe to
    /// unblock ConnectNamedPipe in the accept loop.
    pub fn stop(&self) {
        self.running.store(false, Ordering::Release);

        #[cfg(windows)]
        {
            let mut guard = self.listener_handle.lock().unwrap();
            if let Some(h) = guard.take() {
                // Close the listener pipe to unblock ConnectNamedPipe
                extern "system" { fn CloseHandle(h: usize) -> i32; }
                unsafe { CloseHandle(h); }
            }
        }
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
            session.session_id,
            session.max_request_payload_bytes + HEADER_SIZE as u32,
            session.max_response_payload_bytes + HEADER_SIZE as u32,
        ) {
            Ok(ctx) => Some(ctx),
            Err(_) => None,
        }
    }

    /// Windows: SHM upgrade helper.
    #[cfg(windows)]
    fn try_win_shm_upgrade(&self, session: &NpSession) -> Option<WinShmContext> {
        let profile = session.selected_profile;
        if profile != WIN_SHM_PROFILE_HYBRID && profile != WIN_SHM_PROFILE_BUSYWAIT {
            return None;
        }

        match WinShmContext::server_create(
            &self.run_dir,
            &self.service_name,
            self.server_config.auth_token,
            session.session_id,
            profile,
            session.max_request_payload_bytes + HEADER_SIZE as u32,
            session.max_response_payload_bytes + HEADER_SIZE as u32,
        ) {
            Ok(ctx) => Some(ctx),
            Err(_) => None,
        }
    }

}

/// Windows: handle one client session over Named Pipe + optional Win SHM.
/// Standalone function for use in per-session threads.
#[cfg(windows)]
fn handle_session_win_threaded(
    mut session: NpSession,
    mut shm: Option<WinShmContext>,
    handler: HandlerFn,
    running: Arc<AtomicBool>,
) {
    let mut recv_buf = vec![0u8; HEADER_SIZE + session.max_request_payload_bytes as usize];

    while running.load(Ordering::Acquire) {
        let (hdr, payload) = {
            if let Some(ref mut shm_ctx) = shm {
                match shm_ctx.receive(&mut recv_buf, SERVER_POLL_TIMEOUT_MS) {
                    Ok(mlen) => {
                        if mlen < HEADER_SIZE {
                            break;
                        }
                        let hdr = match Header::decode(&recv_buf[..mlen]) {
                            Ok(h) => h,
                            Err(_) => break,
                        };
                        let payload = recv_buf[HEADER_SIZE..mlen].to_vec();
                        (hdr, payload)
                    }
                    Err(crate::transport::win_shm::WinShmError::Timeout) => continue,
                    Err(_) => break,
                }
            } else {
                // Named Pipe path
                match session.receive(&mut recv_buf) {
                    Ok((hdr, payload)) => (hdr, payload),
                    Err(_) => break,
                }
            }
        };

        // Protocol violation: unexpected message kind terminates session
        if hdr.kind != KIND_REQUEST {
            break;
        }

        // Dispatch: single-item or batch
        let is_batch = (hdr.flags & FLAG_BATCH) != 0 && hdr.item_count > 1;

        let (response_payload, resp_ok, resp_flags, resp_item_count) = if !is_batch {
            match handler(hdr.code, &payload) {
                Some(data) => (data, true, 0u16, 1u32),
                None => (Vec::new(), false, 0u16, 1u32),
            }
        } else {
            let batch_count = hdr.item_count;
            let mut ok = true;
            let mut item_responses: Vec<Vec<u8>> = Vec::with_capacity(batch_count as usize);

            for i in 0..batch_count {
                match batch_item_get(&payload, batch_count, i) {
                    Ok((item_data, _item_len)) => {
                        match handler(hdr.code, item_data) {
                            Some(resp_data) => item_responses.push(resp_data),
                            None => { ok = false; break; }
                        }
                    }
                    Err(_) => { ok = false; break; }
                }
            }

            if ok {
                let total_data: usize = item_responses.iter()
                    .map(|r| protocol::align8(r.len()))
                    .sum();
                let buf_size = protocol::align8(batch_count as usize * 8) + total_data + 64;
                let mut batch_buf = vec![0u8; buf_size];
                let mut bb = BatchBuilder::new(&mut batch_buf, batch_count);

                for resp in &item_responses {
                    if bb.add(resp).is_err() {
                        ok = false;
                        break;
                    }
                }

                if ok {
                    let (total_len, _out_count) = bb.finish();
                    (batch_buf[..total_len].to_vec(), true, FLAG_BATCH, batch_count)
                } else {
                    (Vec::new(), false, 0u16, 1u32)
                }
            } else {
                (Vec::new(), false, 0u16, 1u32)
            }
        };

        let mut resp_hdr = Header {
            kind: KIND_RESPONSE,
            code: hdr.code,
            message_id: hdr.message_id,
            ..Header::default()
        };

        if resp_ok {
            resp_hdr.transport_status = STATUS_OK;
            resp_hdr.flags = resp_flags;
            resp_hdr.item_count = resp_item_count;
        } else {
            resp_hdr.transport_status = STATUS_INTERNAL_ERROR;
            resp_hdr.item_count = 1;
            resp_hdr.flags = 0;
        }

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

        if session.send(&mut resp_hdr, &response_payload).is_err() {
            break;
        }
    }

    if let Some(mut shm_ctx) = shm {
        shm_ctx.destroy();
    }
    session.close();
}

/// POSIX: Handle one client session in its own thread.
#[cfg(unix)]
fn handle_session_threaded(
    mut session: UdsSession,
    #[cfg(target_os = "linux")] mut shm: Option<ShmContext>,
    #[cfg(not(target_os = "linux"))] _shm: Option<()>,
    handler: HandlerFn,
    running: Arc<AtomicBool>,
) {
    let mut recv_buf = vec![0u8; HEADER_SIZE + session.max_request_payload_bytes as usize];

    while running.load(Ordering::Acquire) {
        // Receive request via the active transport
        let (hdr, payload) = {
            #[cfg(target_os = "linux")]
            {
                if let Some(ref mut shm_ctx) = shm {
                    match shm_ctx.receive(&mut recv_buf, SERVER_POLL_TIMEOUT_MS) {
                        Ok(mlen) => {
                            if mlen < HEADER_SIZE {
                                break;
                            }
                            let hdr = match Header::decode(&recv_buf[..mlen]) {
                                Ok(h) => h,
                                Err(_) => break,
                            };
                            let payload = recv_buf[HEADER_SIZE..mlen].to_vec();
                            (hdr, payload)
                        }
                        Err(crate::transport::shm::ShmError::Timeout) => continue,
                        Err(_) => break,
                    }
                } else {
                    // UDS path with poll
                    let ready = poll_fd(session.fd(), SERVER_POLL_TIMEOUT_MS as i32);
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
                let ready = poll_fd(session.fd(), SERVER_POLL_TIMEOUT_MS as i32);
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

        // Protocol violation: unexpected message kind terminates session
        if hdr.kind != KIND_REQUEST {
            break;
        }

        // Dispatch: single-item or batch
        let is_batch = (hdr.flags & FLAG_BATCH) != 0 && hdr.item_count > 1;

        let (response_payload, resp_ok, resp_flags, resp_item_count) = if !is_batch {
            // Single-item dispatch
            match handler(hdr.code, &payload) {
                Some(data) => (data, true, 0u16, 1u32),
                None => (Vec::new(), false, 0u16, 1u32),
            }
        } else {
            // Batch dispatch: extract each item, call handler per item,
            // reassemble responses using BatchBuilder.
            let batch_count = hdr.item_count;
            let mut ok = true;
            let mut item_responses: Vec<Vec<u8>> = Vec::with_capacity(batch_count as usize);

            for i in 0..batch_count {
                match batch_item_get(&payload, batch_count, i) {
                    Ok((item_data, _item_len)) => {
                        match handler(hdr.code, item_data) {
                            Some(resp_data) => item_responses.push(resp_data),
                            None => { ok = false; break; }
                        }
                    }
                    Err(_) => { ok = false; break; }
                }
            }

            if ok {
                // Assemble batch response
                let total_data: usize = item_responses.iter()
                    .map(|r| protocol::align8(r.len()))
                    .sum();
                let buf_size = protocol::align8(batch_count as usize * 8) + total_data + 64;
                let mut batch_buf = vec![0u8; buf_size];
                let mut bb = BatchBuilder::new(&mut batch_buf, batch_count);

                for resp in &item_responses {
                    if bb.add(resp).is_err() {
                        ok = false;
                        break;
                    }
                }

                if ok {
                    let (total_len, _out_count) = bb.finish();
                    (batch_buf[..total_len].to_vec(), true, FLAG_BATCH, batch_count)
                } else {
                    (Vec::new(), false, 0u16, 1u32)
                }
            } else {
                (Vec::new(), false, 0u16, 1u32)
            }
        };

        // Build response header
        let mut resp_hdr = Header {
            kind: KIND_RESPONSE,
            code: hdr.code,
            message_id: hdr.message_id,
            ..Header::default()
        };

        if resp_ok {
            resp_hdr.transport_status = STATUS_OK;
            resp_hdr.flags = resp_flags;
            resp_hdr.item_count = resp_item_count;
        } else {
            resp_hdr.transport_status = STATUS_INTERNAL_ERROR;
            resp_hdr.item_count = 1;
            resp_hdr.flags = 0;
        }

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

// ---------------------------------------------------------------------------
//  Internal: poll helper
// ---------------------------------------------------------------------------

/// Poll a file descriptor for readability with a timeout in milliseconds.
/// Returns: 1 = data ready, 0 = timeout, -1 = error/hangup.
#[cfg(unix)]
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
//  L3: Client-side cgroups snapshot cache
// ---------------------------------------------------------------------------

/// Cached copy of a single cgroup item. Owns its strings.
/// Built from ephemeral L2 views during cache construction.
#[derive(Debug, Clone)]
pub struct CgroupsCacheItem {
    pub hash: u32,
    pub options: u32,
    pub enabled: u32,
    pub name: String,
    pub path: String,
}

/// L3 cache status snapshot (for diagnostics, not hot path).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CgroupsCacheStatus {
    pub populated: bool,
    pub item_count: u32,
    pub systemd_enabled: u32,
    pub generation: u64,
    pub refresh_success_count: u32,
    pub refresh_failure_count: u32,
    pub connection_state: ClientState,
    /// Monotonic milliseconds of last successful refresh (0 if never).
    pub last_refresh_ts: u64,
}

/// Default response buffer size for L3 cache refresh.
const CACHE_RESPONSE_BUF_SIZE: usize = 65536;

/// L3 client-side cgroups snapshot cache.
///
/// Wraps an L2 client and maintains a local owned copy of the most
/// recent successful snapshot. Lookup by hash+name is O(1) via HashMap.
///
/// On refresh failure, the previous cache is preserved. The cache
/// is empty only if no successful refresh has ever occurred.
pub struct CgroupsCache {
    client: CgroupsClient,
    items: Vec<CgroupsCacheItem>,
    /// Hash table: (hash, name) -> index into items vec
    lookup_index: std::collections::HashMap<(u32, String), usize>,
    systemd_enabled: u32,
    generation: u64,
    populated: bool,
    refresh_success_count: u32,
    refresh_failure_count: u32,
    response_buf: Vec<u8>,
    /// Monotonic reference point for timestamp calculation
    epoch: std::time::Instant,
    /// Monotonic ms of last successful refresh (0 if never)
    pub last_refresh_ts: u64,
}

impl CgroupsCache {
    /// Create a new L3 cache. Creates the underlying L2 client context.
    /// Does NOT connect. Does NOT require the server to be running.
    /// Cache starts empty (populated == false).
    pub fn new(run_dir: &str, service_name: &str, config: ClientConfig) -> Self {
        let buf_size = if config.max_response_payload_bytes > 0 {
            HEADER_SIZE + config.max_response_payload_bytes as usize
        } else {
            CACHE_RESPONSE_BUF_SIZE
        };
        CgroupsCache {
            client: CgroupsClient::new(run_dir, service_name, config),
            items: Vec::new(),
            lookup_index: std::collections::HashMap::new(),
            systemd_enabled: 0,
            generation: 0,
            populated: false,
            refresh_success_count: 0,
            refresh_failure_count: 0,
            response_buf: vec![0u8; buf_size],
            epoch: std::time::Instant::now(),
            last_refresh_ts: 0,
        }
    }

    /// Refresh the cache. Drives the L2 client (connect/reconnect as
    /// needed) and requests a fresh snapshot. On success, rebuilds the
    /// local cache. On failure, preserves the previous cache.
    ///
    /// Returns true if the cache was updated.
    pub fn refresh(&mut self) -> bool {
        // Drive L2 connection lifecycle
        self.client.refresh();

        // Attempt snapshot call
        match self.client.call_snapshot(&mut self.response_buf) {
            Ok(view) => {
                // Build new cache from snapshot view
                let mut new_items = Vec::with_capacity(view.item_count as usize);
                for i in 0..view.item_count {
                    match view.item(i) {
                        Ok(iv) => {
                            let name = match iv.name.as_str() {
                                Ok(s) => s.to_string(),
                                Err(_) => {
                                    // Non-UTF8 name: use lossy conversion
                                    String::from_utf8_lossy(iv.name.as_bytes()).into_owned()
                                }
                            };
                            let path = match iv.path.as_str() {
                                Ok(s) => s.to_string(),
                                Err(_) => {
                                    String::from_utf8_lossy(iv.path.as_bytes()).into_owned()
                                }
                            };
                            new_items.push(CgroupsCacheItem {
                                hash: iv.hash,
                                options: iv.options,
                                enabled: iv.enabled,
                                name,
                                path,
                            });
                        }
                        Err(_) => {
                            // Malformed item: abort, preserve old cache
                            self.refresh_failure_count += 1;
                            return false;
                        }
                    }
                }

                // Rebuild lookup index
                let mut idx = std::collections::HashMap::with_capacity(new_items.len());
                for (i, item) in new_items.iter().enumerate() {
                    idx.insert((item.hash, item.name.clone()), i);
                }

                // Replace old cache
                self.items = new_items;
                self.lookup_index = idx;
                self.systemd_enabled = view.systemd_enabled;
                self.generation = view.generation;
                self.populated = true;
                self.refresh_success_count += 1;
                self.last_refresh_ts = self.epoch.elapsed().as_millis() as u64;
                true
            }
            Err(_) => {
                // Refresh failed: preserve previous cache
                self.refresh_failure_count += 1;
                false
            }
        }
    }

    /// Returns true if at least one successful refresh has occurred.
    /// Cheap cached boolean. No I/O, no syscalls.
    ///
    /// Note: ready means "has cached data", not "is connected."
    #[inline]
    pub fn ready(&self) -> bool {
        self.populated
    }

    /// Look up a cached item by hash + name. O(1) via HashMap. No I/O.
    pub fn lookup(&self, hash: u32, name: &str) -> Option<&CgroupsCacheItem> {
        if !self.populated {
            return None;
        }
        self.lookup_index
            .get(&(hash, name.to_string()))
            .map(|&idx| &self.items[idx])
    }

    /// Fill a status snapshot for diagnostics.
    pub fn status(&self) -> CgroupsCacheStatus {
        CgroupsCacheStatus {
            populated: self.populated,
            item_count: self.items.len() as u32,
            systemd_enabled: self.systemd_enabled,
            generation: self.generation,
            refresh_success_count: self.refresh_success_count,
            refresh_failure_count: self.refresh_failure_count,
            connection_state: self.client.state,
            last_refresh_ts: self.last_refresh_ts,
        }
    }

    /// Close the cache: free all cached items, close the L2 client.
    pub fn close(&mut self) {
        self.items.clear();
        self.lookup_index.clear();
        self.populated = false;
        self.client.close();
    }
}

impl Drop for CgroupsCache {
    fn drop(&mut self) {
        self.close();
    }
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

#[cfg(all(test, unix))]
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
            Self::start_with_workers(service, handler, 8)
        }

        fn start_with_workers(
            service: &str,
            handler: fn(u16, &[u8]) -> Option<Vec<u8>>,
            worker_count: usize,
        ) -> Self {
            ensure_run_dir();
            cleanup_all(service);

            let svc = service.to_string();
            let ready_flag = Arc::new(AtomicBool::new(false));
            let ready_clone = ready_flag.clone();

            let mut server = ManagedServer::with_workers(
                TEST_RUN_DIR,
                &svc,
                server_config(),
                RESPONSE_BUF_SIZE,
                Arc::new(move |code, payload| handler(code, payload)),
                worker_count,
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

        fn start_with_resp_size(
            service: &str,
            handler: fn(u16, &[u8]) -> Option<Vec<u8>>,
            resp_buf_size: usize,
        ) -> Self {
            ensure_run_dir();
            cleanup_all(service);

            let svc = service.to_string();
            let ready_flag = Arc::new(AtomicBool::new(false));
            let ready_clone = ready_flag.clone();

            let mut scfg = server_config();
            scfg.max_response_payload_bytes = resp_buf_size as u32;

            let mut server = ManagedServer::new(
                TEST_RUN_DIR,
                &svc,
                scfg,
                resp_buf_size,
                Arc::new(move |code, payload| handler(code, payload)),
            );
            let stop_flag = server.running_flag();

            let thread = thread::spawn(move || {
                ready_clone.store(true, Ordering::Release);
                let _ = server.run();
            });

            for _ in 0..2000 {
                if ready_flag.load(Ordering::Acquire) {
                    break;
                }
                thread::sleep(Duration::from_micros(500));
            }
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

        // Now multi-client: keep client 1 open, connect client 2
        let mut client2 = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client2.refresh();
        assert!(client2.ready());

        let mut resp_buf2 = vec![0u8; RESPONSE_BUF_SIZE];
        let view2 = client2.call_snapshot(&mut resp_buf2).expect("client 2 call");
        assert_eq!(view2.item_count, 3);

        client1.close();
        client2.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_concurrent_clients() {
        let svc = "rs_svc_concurrent";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        const NUM_CLIENTS: usize = 5;
        const REQUESTS_PER: usize = 10;

        let mut handles = Vec::new();

        for _ in 0..NUM_CLIENTS {
            let svc_name = svc.to_string();
            let handle = thread::spawn(move || {
                let mut client = CgroupsClient::new(TEST_RUN_DIR, &svc_name, client_config());

                // Connect with retry
                for _ in 0..100 {
                    client.refresh();
                    if client.ready() {
                        break;
                    }
                    thread::sleep(Duration::from_millis(10));
                }

                assert!(client.ready(), "client must be ready");

                let mut successes = 0usize;
                for _ in 0..REQUESTS_PER {
                    let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
                    match client.call_snapshot(&mut resp_buf) {
                        Ok(view) => {
                            assert_eq!(view.item_count, 3);
                            assert_eq!(view.generation, 42);

                            // Verify first item content
                            let item0 = view.item(0).expect("item 0");
                            assert_eq!(item0.hash, 1001);
                            assert_eq!(
                                std::str::from_utf8(item0.name.as_bytes()).unwrap(),
                                "docker-abc123"
                            );

                            successes += 1;
                        }
                        Err(e) => panic!("call failed: {:?}", e),
                    }
                }
                client.close();
                successes
            });
            handles.push(handle);
        }

        let mut total = 0usize;
        for h in handles {
            total += h.join().expect("client thread panicked");
        }

        assert_eq!(total, NUM_CLIENTS * REQUESTS_PER);

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

    #[test]
    fn test_non_request_terminates_session() {
        // Send a RESPONSE message to a server; the server must terminate
        // the session (protocol violation), so subsequent requests fail.
        let svc = "rs_svc_nonreq";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        // Connect via raw UDS session
        let mut session = UdsSession::connect(
            TEST_RUN_DIR, svc, &client_config(),
        ).expect("connect");

        // Send a RESPONSE (not REQUEST) - protocol violation
        let mut hdr = Header {
            kind: KIND_RESPONSE,
            code: METHOD_CGROUPS_SNAPSHOT,
            flags: 0,
            item_count: 0,
            message_id: 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };
        let send_result = session.send(&mut hdr, &[]);
        // Send may succeed (the bytes go out)
        if send_result.is_ok() {
            // But subsequent communication should fail because the
            // server terminated the session
            thread::sleep(Duration::from_millis(100));
            let mut recv_buf = vec![0u8; 4096];
            // Try to send a valid request and receive - should fail
            let mut hdr2 = Header {
                kind: KIND_REQUEST,
                code: METHOD_CGROUPS_SNAPSHOT,
                flags: 0,
                item_count: 1,
                message_id: 2,
                transport_status: STATUS_OK,
                ..Header::default()
            };
            let req = CgroupsRequest { layout_version: 1, flags: 0 };
            let mut req_buf = [0u8; 4];
            req.encode(&mut req_buf);
            let _ = session.send(&mut hdr2, &req_buf);
            let recv = session.receive(&mut recv_buf);
            assert!(recv.is_err(), "server should have terminated session after non-request message");
        }

        drop(session);

        // Verify server is still alive: connect a new client and do a normal call
        let mut verify_client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        verify_client.refresh();
        assert!(verify_client.ready(), "server should still be alive after bad client");

        let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
        let view = verify_client.call_snapshot(&mut resp_buf)
            .expect("normal call should succeed after bad client");
        assert_eq!(view.item_count, 3, "response should be correct after bad client");

        verify_client.close();
        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  L3 Cache tests
    // ---------------------------------------------------------------

    #[test]
    fn test_cache_full_round_trip() {
        let svc = "rs_cache_rt";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());
        assert!(!cache.ready());

        // Ensure monotonic epoch advances past 0ms before refresh
        thread::sleep(Duration::from_millis(2));

        // Refresh populates the cache
        let updated = cache.refresh();
        assert!(updated);
        assert!(cache.ready());

        // Lookup by hash + name
        let item = cache.lookup(1001, "docker-abc123");
        assert!(item.is_some());
        let item = item.unwrap();
        assert_eq!(item.hash, 1001);
        assert_eq!(item.options, 0);
        assert_eq!(item.enabled, 1);
        assert_eq!(item.name, "docker-abc123");
        assert_eq!(item.path, "/sys/fs/cgroup/docker/abc123");

        let item2 = cache.lookup(3003, "systemd-user");
        assert!(item2.is_some());
        assert_eq!(item2.unwrap().enabled, 0);

        // Status
        let status = cache.status();
        assert!(status.populated);
        assert_eq!(status.item_count, 3);
        assert_eq!(status.systemd_enabled, 1);
        assert_eq!(status.generation, 42);
        assert_eq!(status.refresh_success_count, 1);
        assert_eq!(status.refresh_failure_count, 0);
        assert_eq!(status.connection_state, ClientState::Ready);
        assert!(status.last_refresh_ts > 0);

        cache.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_cache_refresh_failure_preserves() {
        let svc = "rs_cache_preserve";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());

        // First refresh populates cache
        assert!(cache.refresh());
        assert!(cache.ready());
        assert!(cache.lookup(1001, "docker-abc123").is_some());

        // Kill server
        server.stop();
        cleanup_all(svc);
        thread::sleep(Duration::from_millis(50));

        // Refresh fails, but old cache is preserved
        let updated = cache.refresh();
        assert!(!updated);
        assert!(cache.ready()); // still has cached data
        assert!(cache.lookup(1001, "docker-abc123").is_some());

        let status = cache.status();
        assert_eq!(status.refresh_success_count, 1);
        assert!(status.refresh_failure_count >= 1);

        cache.close();
        cleanup_all(svc);
    }

    #[test]
    fn test_cache_reconnect_rebuilds() {
        let svc = "rs_cache_reconn";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server1 = TestServer::start(svc, test_cgroups_handler);

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());
        assert!(cache.refresh());
        assert_eq!(cache.status().item_count, 3);

        // Kill and restart server
        server1.stop();
        cleanup_all(svc);
        thread::sleep(Duration::from_millis(50));

        let mut server2 = TestServer::start(svc, test_cgroups_handler);

        // Refresh should reconnect and rebuild cache
        let updated = cache.refresh();
        assert!(updated);
        assert!(cache.ready());
        assert_eq!(cache.status().item_count, 3);
        assert_eq!(cache.status().refresh_success_count, 2);

        cache.close();
        server2.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_cache_lookup_not_found() {
        let svc = "rs_cache_notfound";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());
        assert!(cache.refresh());

        // Non-existent hash
        assert!(cache.lookup(9999, "nonexistent").is_none());

        // Correct hash, wrong name
        assert!(cache.lookup(1001, "wrong-name").is_none());

        // Correct name, wrong hash
        assert!(cache.lookup(9999, "docker-abc123").is_none());

        cache.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_cache_empty() {
        let svc = "rs_cache_empty";
        ensure_run_dir();
        cleanup_all(svc);

        let cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());

        // Not ready before any refresh
        assert!(!cache.ready());

        // Lookup on empty cache returns None
        assert!(cache.lookup(1001, "docker-abc123").is_none());

        let status = cache.status();
        assert!(!status.populated);
        assert_eq!(status.item_count, 0);
        assert_eq!(status.refresh_success_count, 0);
        assert_eq!(status.refresh_failure_count, 0);

        cleanup_all(svc);
    }

    #[test]
    fn test_cache_large_dataset() {
        let svc = "rs_cache_large";
        ensure_run_dir();
        cleanup_all(svc);

        const N: u32 = 1000;

        // Handler that builds N items
        fn large_handler(method_code: u16, request_payload: &[u8]) -> Option<Vec<u8>> {
            if method_code != METHOD_CGROUPS_SNAPSHOT {
                return None;
            }
            if CgroupsRequest::decode(request_payload).is_err() {
                return None;
            }

            let buf_size = 256 * N as usize;
            let mut buf = vec![0u8; buf_size];
            let mut builder = CgroupsBuilder::new(&mut buf, N, 1, 100);

            for i in 0..N {
                let name = format!("cgroup-{i}");
                let path = format!("/sys/fs/cgroup/test/{i}");
                if builder
                    .add(i + 1000, 0, if i % 3 == 0 { 0 } else { 1 },
                         name.as_bytes(), path.as_bytes())
                    .is_err()
                {
                    return None;
                }
            }

            let total = builder.finish();
            buf.truncate(total);
            Some(buf)
        }

        // Use a larger response buf size
        let mut cfg = client_config();
        cfg.max_response_payload_bytes = 256 * N;

        let mut server = TestServer::start_with_resp_size(svc, large_handler, 256 * N as usize);

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, cfg);

        // Need larger response buf
        cache.response_buf = vec![0u8; 256 * N as usize];

        assert!(cache.refresh());
        assert_eq!(cache.status().item_count, N);

        // Verify all lookups
        for i in 0..N {
            let name = format!("cgroup-{i}");
            let item = cache.lookup(i + 1000, &name);
            assert!(item.is_some(), "item {i} not found");
            let item = item.unwrap();
            assert_eq!(item.hash, i + 1000);
            let expected_path = format!("/sys/fs/cgroup/test/{i}");
            assert_eq!(item.path, expected_path);
        }

        cache.close();
        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Stress tests (Phase H4)
    // ---------------------------------------------------------------

    /// djb2 hash matching the C implementation
    fn simple_hash(s: &str) -> u32 {
        let mut hash: u32 = 5381;
        for c in s.bytes() {
            hash = hash.wrapping_shl(5).wrapping_add(hash).wrapping_add(c as u32);
        }
        hash
    }

    struct StressTestServer {
        stop_flag: Arc<AtomicBool>,
        thread: Option<thread::JoinHandle<()>>,
    }

    impl StressTestServer {
        fn start(service: &str, n: u32, resp_buf_size: usize) -> Self {
            ensure_run_dir();
            cleanup_all(service);

            let svc = service.to_string();
            let ready_flag = Arc::new(AtomicBool::new(false));
            let ready_clone = ready_flag.clone();

            let mut scfg = server_config();
            scfg.max_response_payload_bytes = resp_buf_size as u32;
            scfg.packet_size = 65536; // force smaller packets for chunked transport

            let handler: Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync> =
                Arc::new(move |method_code, request_payload| {
                    if method_code != METHOD_CGROUPS_SNAPSHOT {
                        return None;
                    }
                    if CgroupsRequest::decode(request_payload).is_err() {
                        return None;
                    }

                    let mut buf = vec![0u8; resp_buf_size];
                    let mut builder = CgroupsBuilder::new(&mut buf, n, 1, 42);

                    for i in 0..n {
                        let name = format!("container-{i:04}");
                        let path = format!("/sys/fs/cgroup/docker/{i:04}");
                        let hash = simple_hash(&name);
                        let enabled = if i % 5 == 0 { 0 } else { 1 };
                        if builder.add(hash, 0x10, enabled,
                                       name.as_bytes(), path.as_bytes()).is_err() {
                            return None;
                        }
                    }

                    let total = builder.finish();
                    buf.truncate(total);
                    Some(buf)
                });

            let mut server = ManagedServer::new(
                TEST_RUN_DIR,
                &svc,
                scfg,
                resp_buf_size,
                handler,
            );
            let stop_flag = server.running_flag();

            let thread = thread::spawn(move || {
                ready_clone.store(true, Ordering::Release);
                let _ = server.run();
            });

            for _ in 0..2000 {
                if ready_flag.load(Ordering::Acquire) {
                    break;
                }
                thread::sleep(Duration::from_micros(500));
            }
            thread::sleep(Duration::from_millis(50));

            StressTestServer {
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

    impl Drop for StressTestServer {
        fn drop(&mut self) {
            self.stop();
        }
    }

    #[test]
    fn test_stress_1000_items() {
        let svc = "rs_stress_1k";

        const N: u32 = 1000;
        const BUF_SIZE: usize = 300 * N as usize;

        let mut server = StressTestServer::start(svc, N, BUF_SIZE);

        let mut cfg = client_config();
        cfg.max_response_payload_bytes = BUF_SIZE as u32;
        cfg.packet_size = 65536;

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, cfg);
        client.refresh();
        assert!(client.ready(), "client not ready");

        let start = std::time::Instant::now();
        let mut resp_buf = vec![0u8; BUF_SIZE];
        let view = client.call_snapshot(&mut resp_buf).expect("call should succeed");
        let elapsed = start.elapsed();

        eprintln!("  1000 items: {:?}", elapsed);

        assert_eq!(view.item_count, N);
        assert_eq!(view.systemd_enabled, 1);
        assert_eq!(view.generation, 42);

        // Verify ALL items
        for i in 0..N {
            let item = view.item(i).unwrap_or_else(|_| panic!("item {i} decode failed"));
            let expected_name = format!("container-{i:04}");
            let expected_path = format!("/sys/fs/cgroup/docker/{i:04}");
            let expected_hash = simple_hash(&expected_name);
            let expected_enabled = if i % 5 == 0 { 0 } else { 1 };

            assert_eq!(item.hash, expected_hash, "item {i} hash mismatch");
            assert_eq!(
                std::str::from_utf8(item.name.as_bytes()).unwrap(),
                expected_name,
                "item {i} name mismatch"
            );
            assert_eq!(
                std::str::from_utf8(item.path.as_bytes()).unwrap(),
                expected_path,
                "item {i} path mismatch"
            );
            assert_eq!(item.enabled, expected_enabled, "item {i} enabled mismatch");
            assert_eq!(item.options, 0x10, "item {i} options mismatch");
        }

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_stress_5000_items() {
        let svc = "rs_stress_5k";

        const N: u32 = 5000;
        const BUF_SIZE: usize = 300 * N as usize;

        let mut server = StressTestServer::start(svc, N, BUF_SIZE);

        let mut cfg = client_config();
        cfg.max_response_payload_bytes = BUF_SIZE as u32;
        cfg.packet_size = 65536;

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, cfg);
        client.refresh();
        assert!(client.ready(), "client not ready");

        let start = std::time::Instant::now();
        let mut resp_buf = vec![0u8; BUF_SIZE];
        let view = client.call_snapshot(&mut resp_buf).expect("call should succeed");
        let elapsed = start.elapsed();

        eprintln!("  5000 items: {:?}", elapsed);

        assert_eq!(view.item_count, N);

        // Spot-check first, middle, last
        for idx in [0, N / 2, N - 1] {
            let item = view.item(idx).unwrap();
            let expected_name = format!("container-{idx:04}");
            let expected_hash = simple_hash(&expected_name);
            assert_eq!(item.hash, expected_hash);
            assert_eq!(
                std::str::from_utf8(item.name.as_bytes()).unwrap(),
                expected_name
            );
        }

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_stress_concurrent_clients() {
        let svc = "rs_stress_concurrent";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start_with_workers(svc, test_cgroups_handler, 64);

        const NUM_CLIENTS: usize = 50;
        const REQUESTS_PER: usize = 10;

        let start = std::time::Instant::now();

        let mut handles = Vec::new();
        for client_id in 0..NUM_CLIENTS {
            let svc_name = svc.to_string();
            let handle = thread::spawn(move || {
                let mut client = CgroupsClient::new(TEST_RUN_DIR, &svc_name, client_config());

                for _ in 0..200 {
                    client.refresh();
                    if client.ready() {
                        break;
                    }
                    thread::sleep(Duration::from_millis(5));
                }

                assert!(client.ready(), "client {client_id} not ready");

                let mut successes = 0usize;
                for _ in 0..REQUESTS_PER {
                    let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
                    match client.call_snapshot(&mut resp_buf) {
                        Ok(view) => {
                            assert_eq!(view.item_count, 3);
                            assert_eq!(view.generation, 42);
                            let item0 = view.item(0).expect("item 0");
                            assert_eq!(item0.hash, 1001);
                            assert_eq!(
                                std::str::from_utf8(item0.name.as_bytes()).unwrap(),
                                "docker-abc123"
                            );
                            let item2 = view.item(2).expect("item 2");
                            assert_eq!(item2.hash, 3003);
                            successes += 1;
                        }
                        Err(e) => panic!("client {client_id} call failed: {:?}", e),
                    }
                }
                client.close();
                successes
            });
            handles.push(handle);
        }

        let mut total = 0usize;
        for h in handles {
            total += h.join().expect("client thread panicked");
        }

        let elapsed = start.elapsed();
        eprintln!(
            "  {NUM_CLIENTS} clients x {REQUESTS_PER} req: {total}/{} in {:?}",
            NUM_CLIENTS * REQUESTS_PER,
            elapsed
        );

        assert_eq!(total, NUM_CLIENTS * REQUESTS_PER);

        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_stress_rapid_connect_disconnect() {
        let svc = "rs_stress_rapid";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        const CYCLES: usize = 1000;
        let mut successes = 0usize;
        let mut failures = 0usize;

        let start = std::time::Instant::now();

        for _ in 0..CYCLES {
            let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());

            let mut connected = false;
            for _ in 0..50 {
                client.refresh();
                if client.ready() {
                    connected = true;
                    break;
                }
                thread::sleep(Duration::from_millis(2));
            }

            if !connected {
                failures += 1;
                client.close();
                continue;
            }

            let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
            match client.call_snapshot(&mut resp_buf) {
                Ok(view) => {
                    if view.item_count == 3 && view.generation == 42 {
                        successes += 1;
                    } else {
                        failures += 1;
                    }
                }
                Err(_) => failures += 1,
            }

            client.close();
        }

        let elapsed = start.elapsed();
        eprintln!("  {CYCLES} rapid cycles: {successes} ok, {failures} fail, {:?}", elapsed);

        assert_eq!(successes, CYCLES, "all cycles should succeed");
        assert_eq!(failures, 0, "no failures expected");

        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_stress_cache_concurrent() {
        let svc = "rs_stress_cache";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start_with_workers(svc, test_cgroups_handler, 16);

        const NUM_CLIENTS: usize = 10;
        const REQUESTS_PER: usize = 100;

        let start = std::time::Instant::now();

        let mut handles = Vec::new();
        for _ in 0..NUM_CLIENTS {
            let svc_name = svc.to_string();
            let handle = thread::spawn(move || {
                let mut cache = CgroupsCache::new(TEST_RUN_DIR, &svc_name, client_config());
                let mut successes = 0usize;

                for _ in 0..REQUESTS_PER {
                    let updated = cache.refresh();
                    if updated || cache.ready() {
                        let status = cache.status();
                        if status.item_count != 3 {
                            continue;
                        }
                        let item = cache.lookup(1001, "docker-abc123");
                        if item.is_some() && item.unwrap().hash == 1001 {
                            successes += 1;
                        }
                    }
                }
                cache.close();
                successes
            });
            handles.push(handle);
        }

        let mut total = 0usize;
        for h in handles {
            total += h.join().expect("cache thread panicked");
        }

        let elapsed = start.elapsed();
        eprintln!(
            "  {NUM_CLIENTS} cache clients x {REQUESTS_PER} req: {total}/{} in {:?}",
            NUM_CLIENTS * REQUESTS_PER,
            elapsed
        );

        assert_eq!(total, NUM_CLIENTS * REQUESTS_PER);

        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_stress_long_running() {
        let svc = "rs_stress_long";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handler);

        const NUM_CLIENTS: usize = 5;
        let run_duration = Duration::from_secs(30);

        let running = Arc::new(AtomicBool::new(true));
        let mut handles = Vec::new();

        for _ in 0..NUM_CLIENTS {
            let svc_name = svc.to_string();
            let r = running.clone();
            let handle = thread::spawn(move || {
                let mut cache = CgroupsCache::new(TEST_RUN_DIR, &svc_name, client_config());
                let mut refreshes = 0u64;
                let mut errors = 0u64;

                while r.load(Ordering::Acquire) {
                    let updated = cache.refresh();
                    if updated || cache.ready() {
                        let status = cache.status();
                        if status.item_count == 3 {
                            refreshes += 1;
                        } else {
                            errors += 1;
                        }
                    } else {
                        errors += 1;
                    }
                    thread::sleep(Duration::from_millis(1));
                }

                cache.close();
                (refreshes, errors)
            });
            handles.push(handle);
        }

        thread::sleep(run_duration);
        running.store(false, Ordering::Release);

        let mut total_refreshes = 0u64;
        let mut total_errors = 0u64;
        for h in handles {
            let (r, e) = h.join().expect("client thread panicked");
            total_refreshes += r;
            total_errors += e;
        }

        eprintln!("  30s run: {total_refreshes} refreshes, {total_errors} errors");

        assert!(total_refreshes > 0, "expected some refreshes");
        assert_eq!(total_errors, 0, "expected zero errors in 60s run");

        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Ping-pong tests (INCREMENT + STRING_REVERSE)
    // ---------------------------------------------------------------

    /// Multi-method handler: INCREMENT (method 1) and STRING_REVERSE (method 3).
    /// Uses typed dispatch helpers from protocol.
    fn pingpong_handler(method_code: u16, request_payload: &[u8]) -> Option<Vec<u8>> {
        match method_code {
            METHOD_INCREMENT => {
                let mut buf = [0u8; INCREMENT_PAYLOAD_SIZE];
                let len = protocol::dispatch_increment(request_payload, &mut buf, |v| Some(v + 1))?;
                Some(buf[..len].to_vec())
            }
            METHOD_STRING_REVERSE => {
                // Need a buffer large enough for the response
                let resp_size = STRING_REVERSE_HDR_SIZE + request_payload.len() + 1;
                let mut buf = vec![0u8; resp_size];
                let len = protocol::dispatch_string_reverse(request_payload, &mut buf, |data| {
                    Some(data.iter().rev().copied().collect())
                })?;
                Some(buf[..len].to_vec())
            }
            _ => None,
        }
    }

    #[test]
    fn test_increment_ping_pong() {
        let svc = "rs_pp_incr";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready(), "client not ready");

        // Ping-pong: send 0 -> get 1 -> send 1 -> get 2 -> ... -> 10
        let mut value = 0u64;
        let mut responses_received = 0u64;
        for round in 0..10 {
            let sent = value;
            let result = client.call_increment(sent)
                .unwrap_or_else(|e| panic!("round {round}: call_increment({sent}) failed: {e:?}"));
            assert_eq!(result, sent + 1, "round {round}: expected {} got {result}", sent + 1);
            responses_received += 1;
            value = result;
        }
        assert_eq!(responses_received, 10, "expected 10 responses, got {responses_received}");
        assert_eq!(value, 10, "final value after 10 rounds");

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_string_reverse_ping_pong() {
        let svc = "rs_pp_strrev";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready(), "client not ready");

        let original = "abcdefghijklmnopqrstuvwxyz";
        let mut current = original.to_string();
        let mut responses_received = 0u64;

        // 6 rounds: feed each response back as next request
        for round in 0..6 {
            let sent = current.clone();
            let expected: String = sent.chars().rev().collect();
            let result = client.call_string_reverse(&sent)
                .unwrap_or_else(|e| panic!("round {round}: call_string_reverse({sent:?}) failed: {e:?}"));
            assert_eq!(result, expected, "round {round}: reverse of {sent:?} should be {expected:?}, got {result:?}");
            responses_received += 1;
            current = result;
        }
        assert_eq!(responses_received, 6, "expected 6 responses, got {responses_received}");
        // even number of reversals = identity
        assert_eq!(current, original, "6 reversals should restore original string");

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_mixed_methods() {
        let svc = "rs_pp_mixed";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handler);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready(), "client not ready");

        // Interleave increment and string_reverse calls
        let inc_input_1 = 100u64;
        let v1 = client.call_increment(inc_input_1).expect("increment(100)");
        assert_eq!(v1, inc_input_1 + 1, "increment({inc_input_1}) should be {}", inc_input_1 + 1);

        let str_input_1 = "hello";
        let expected_s1: String = str_input_1.chars().rev().collect();
        let s1 = client.call_string_reverse(str_input_1).expect("reverse(hello)");
        assert_eq!(s1, expected_s1, "reverse of {str_input_1:?} should be {expected_s1:?}");

        let inc_input_2 = v1;
        let v2 = client.call_increment(inc_input_2).expect("increment(101)");
        assert_eq!(v2, inc_input_2 + 1, "increment({inc_input_2}) should be {}", inc_input_2 + 1);

        let str_input_2 = "world";
        let expected_s2: String = str_input_2.chars().rev().collect();
        let s2 = client.call_string_reverse(str_input_2).expect("reverse(world)");
        assert_eq!(s2, expected_s2, "reverse of {str_input_2:?} should be {expected_s2:?}");

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_increment_batch() {
        let svc = "rs_pp_batch";
        ensure_run_dir();
        cleanup_all(svc);

        // Need batch items > 1 for both client and server configs
        fn batch_server_config() -> ServerConfig {
            ServerConfig {
                supported_profiles: PROFILE_BASELINE,
                max_request_payload_bytes: 4096,
                max_request_batch_items: 16,
                max_response_payload_bytes: 4096,
                max_response_batch_items: 16,
                auth_token: AUTH_TOKEN,
                backlog: 4,
                ..ServerConfig::default()
            }
        }

        fn batch_client_config() -> ClientConfig {
            ClientConfig {
                supported_profiles: PROFILE_BASELINE,
                max_request_payload_bytes: 4096,
                max_request_batch_items: 16,
                max_response_payload_bytes: 4096,
                max_response_batch_items: 16,
                auth_token: AUTH_TOKEN,
                ..ClientConfig::default()
            }
        }

        // Start server with batch-capable config
        let svc_name = svc.to_string();
        let ready_flag = Arc::new(AtomicBool::new(false));
        let ready_clone = ready_flag.clone();

        let mut server_obj = ManagedServer::with_workers(
            TEST_RUN_DIR,
            &svc_name,
            batch_server_config(),
            RESPONSE_BUF_SIZE,
            Arc::new(move |code, payload| pingpong_handler(code, payload)),
            8,
        );
        let stop_flag = server_obj.running_flag();

        let thread_handle = thread::spawn(move || {
            ready_clone.store(true, Ordering::Release);
            let _ = server_obj.run();
        });

        for _ in 0..2000 {
            if ready_flag.load(Ordering::Acquire) {
                break;
            }
            thread::sleep(Duration::from_micros(500));
        }
        thread::sleep(Duration::from_millis(50));

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, batch_client_config());
        client.refresh();
        assert!(client.ready(), "client not ready");

        // Send batch of [10, 20, 30, 40, 50]
        let values = vec![10u64, 20, 30, 40, 50];
        let results = client.call_increment_batch(&values).expect("batch call");

        assert_eq!(results.len(), 5);
        for (i, (&input, &output)) in values.iter().zip(results.iter()).enumerate() {
            assert_eq!(output, input + 1, "batch item {i}: expected {}, got {output}", input + 1);
        }

        // Single item batch
        let single = client.call_increment_batch(&[99]).expect("single-item batch");
        assert_eq!(single, vec![100]);

        // Empty batch
        let empty = client.call_increment_batch(&[]).expect("empty batch");
        assert!(empty.is_empty());

        client.close();
        stop_flag.store(false, Ordering::Release);
        let _ = thread_handle.join();
        cleanup_all(svc);
    }
}
