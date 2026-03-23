//! L2 cgroups service: client context, managed server, and L3 cache.
//!
//! Pure composition of L1 (UDS/SHM on POSIX, Named Pipe/Win SHM on Windows)
//! + Codec. No direct socket/mmap calls.
//! Client manages connection lifecycle with at-least-once retry.
//! Server handles accept, read, dispatch, respond.

use crate::protocol::{
    self, batch_item_get, increment_decode, increment_encode, string_reverse_decode,
    string_reverse_encode, BatchBuilder, CgroupsRequest, CgroupsResponseView, Header, NipcError,
    FLAG_BATCH, HEADER_SIZE, INCREMENT_PAYLOAD_SIZE, KIND_REQUEST, KIND_RESPONSE, MAGIC_MSG,
    METHOD_CGROUPS_SNAPSHOT, METHOD_INCREMENT, METHOD_STRING_REVERSE, STATUS_INTERNAL_ERROR,
    STATUS_OK, STRING_REVERSE_HDR_SIZE, VERSION,
};

#[cfg(unix)]
use crate::protocol::{PROFILE_SHM_FUTEX, PROFILE_SHM_HYBRID};

#[cfg(unix)]
use crate::transport::posix::{ClientConfig, ServerConfig, UdsListener, UdsSession};

#[cfg(target_os = "linux")]
use crate::transport::shm::ShmContext;

#[cfg(windows)]
use crate::transport::windows::{ClientConfig, NpError, NpListener, NpSession, ServerConfig};

#[cfg(windows)]
use crate::transport::win_shm::{
    WinShmContext, PROFILE_BUSYWAIT as WIN_SHM_PROFILE_BUSYWAIT,
    PROFILE_HYBRID as WIN_SHM_PROFILE_HYBRID,
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

    // Reusable scratch buffers owned by the client for hot request paths.
    request_buf: Vec<u8>,
    send_buf: Vec<u8>,
    transport_buf: Vec<u8>,

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
            request_buf: Vec::new(),
            send_buf: Vec::new(),
            transport_buf: Vec::new(),
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
    /// The returned view is valid until the next typed call on this client.
    ///
    /// Retry policy (per spec): if the call fails and the context was
    /// previously READY, disconnect, reconnect (full handshake), retry ONCE.
    pub fn call_snapshot(&mut self) -> Result<CgroupsResponseView<'_>, NipcError> {
        let req = CgroupsRequest {
            layout_version: 1,
            flags: 0,
        };
        let mut req_buf = [0u8; 4];
        let req_len = req.encode(&mut req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        let response = self.raw_call_with_retry(METHOD_CGROUPS_SNAPSHOT, &req_buf[..req_len])?;
        CgroupsResponseView::decode(self.response_payload(response)?)
    }

    /// Blocking typed call: INCREMENT method.
    /// Sends a u64 value, receives the incremented u64 back.
    pub fn call_increment(&mut self, value: u64) -> Result<u64, NipcError> {
        let mut req_buf = [0u8; INCREMENT_PAYLOAD_SIZE];
        let req_len = increment_encode(value, &mut req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        let response = self.raw_call_with_retry(METHOD_INCREMENT, &req_buf[..req_len])?;
        increment_decode(self.response_payload(response)?)
    }

    /// Blocking typed call: STRING_REVERSE method.
    /// Sends a string, receives the reversed string back.
    ///
    /// The returned view is valid until the next typed call on this client.
    pub fn call_string_reverse(
        &mut self,
        s: &str,
    ) -> Result<protocol::StringReverseView<'_>, NipcError> {
        let req_size = STRING_REVERSE_HDR_SIZE + s.len() + 1;
        let req_buf = ensure_client_scratch(&mut self.request_buf, req_size);
        let req_len = string_reverse_encode(s.as_bytes(), req_buf);
        if req_len == 0 {
            return Err(NipcError::Truncated);
        }

        let response = self.raw_call_with_retry_request_buf(METHOD_STRING_REVERSE, req_len)?;
        string_reverse_decode(self.response_payload(response)?)
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

        let req_buf_size = protocol::align8(count as usize * 8)
            + count as usize * protocol::align8(INCREMENT_PAYLOAD_SIZE)
            + 64;
        let req_buf = ensure_client_scratch(&mut self.request_buf, req_buf_size);
        let req_len = {
            let mut bb = BatchBuilder::new(req_buf, count);
            for &v in values {
                let mut item_buf = [0u8; INCREMENT_PAYLOAD_SIZE];
                if increment_encode(v, &mut item_buf) == 0 {
                    return Err(NipcError::Truncated);
                }
                bb.add(&item_buf).map_err(|_| NipcError::Overflow)?;
            }
            let (req_len, _out_count) = bb.finish();
            req_len
        };

        let response =
            self.raw_batch_call_with_retry_request_buf(METHOD_INCREMENT, req_len, count)?;
        let resp_payload = self.response_payload(response)?;
        let mut results = Vec::with_capacity(values.len());
        for i in 0..count {
            let (item_data, _item_len) = batch_item_get(resp_payload, count, i)?;
            let val = increment_decode(item_data)?;
            results.push(val);
        }

        Ok(results)
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
                            match ShmContext::client_attach(
                                &self.run_dir,
                                &self.service_name,
                                session_id,
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

    /// Retry a single-item raw call once after a reconnect if the first
    /// attempt fails while the client was READY.
    fn raw_call_with_retry<'a>(
        &mut self,
        method_code: u16,
        request_payload: &[u8],
    ) -> Result<ClientResponseRef, NipcError> {
        if self.state != ClientState::Ready {
            self.error_count += 1;
            return Err(NipcError::BadLayout);
        }

        match self.do_raw_call(method_code, request_payload) {
            Ok(payload) => Ok(payload),
            Err(first_err) => {
                self.disconnect();
                self.state = ClientState::Broken;
                self.state = self.try_connect();
                if self.state != ClientState::Ready {
                    self.error_count += 1;
                    return Err(first_err);
                }
                self.reconnect_count += 1;

                match self.do_raw_call(method_code, request_payload) {
                    Ok(payload) => Ok(payload),
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

    fn raw_call_with_retry_request_buf<'a>(
        &mut self,
        method_code: u16,
        req_len: usize,
    ) -> Result<ClientResponseRef, NipcError> {
        if self.state != ClientState::Ready {
            self.error_count += 1;
            return Err(NipcError::BadLayout);
        }

        match self.do_raw_call_from_request_buf(method_code, req_len) {
            Ok(payload) => Ok(payload),
            Err(first_err) => {
                self.disconnect();
                self.state = ClientState::Broken;
                self.state = self.try_connect();
                if self.state != ClientState::Ready {
                    self.error_count += 1;
                    return Err(first_err);
                }
                self.reconnect_count += 1;

                match self.do_raw_call_from_request_buf(method_code, req_len) {
                    Ok(payload) => Ok(payload),
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

    fn raw_batch_call_with_retry_request_buf<'a>(
        &mut self,
        method_code: u16,
        req_len: usize,
        item_count: u32,
    ) -> Result<ClientResponseRef, NipcError> {
        if self.state != ClientState::Ready {
            self.error_count += 1;
            return Err(NipcError::BadLayout);
        }

        match self.do_raw_batch_call_from_request_buf(method_code, req_len, item_count) {
            Ok(payload) => Ok(payload),
            Err(first_err) => {
                self.disconnect();
                self.state = ClientState::Broken;
                self.state = self.try_connect();
                if self.state != ClientState::Ready {
                    self.error_count += 1;
                    return Err(first_err);
                }
                self.reconnect_count += 1;

                match self.do_raw_batch_call_from_request_buf(method_code, req_len, item_count) {
                    Ok(payload) => Ok(payload),
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

    /// Single attempt at a raw call for any method.
    fn do_raw_call(
        &mut self,
        method_code: u16,
        request_payload: &[u8],
    ) -> Result<ClientResponseRef, NipcError> {
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

        // 3. Receive via L1
        let (resp_hdr, response) = self.transport_receive()?;

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

        self.call_count += 1;
        Ok(response)
    }

    fn do_raw_call_from_request_buf(
        &mut self,
        method_code: u16,
        req_len: usize,
    ) -> Result<ClientResponseRef, NipcError> {
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: method_code,
            flags: 0,
            item_count: 1,
            message_id: (self.call_count as u64) + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        self.transport_send_request_buf(&mut hdr, req_len)?;
        let (resp_hdr, response) = self.transport_receive()?;

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

        self.call_count += 1;
        Ok(response)
    }

    /// Single attempt at a raw batch call. Like `do_raw_call` but sets
    /// FLAG_BATCH and item_count, and validates the response matches.
    fn do_raw_batch_call_from_request_buf(
        &mut self,
        method_code: u16,
        req_len: usize,
        item_count: u32,
    ) -> Result<ClientResponseRef, NipcError> {
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: method_code,
            flags: FLAG_BATCH,
            item_count,
            message_id: (self.call_count as u64) + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        self.transport_send_request_buf(&mut hdr, req_len)?;

        let (resp_hdr, response) = self.transport_receive()?;

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

        self.call_count += 1;
        Ok(response)
    }

    /// Send via the active transport (SHM if available, baseline otherwise).
    fn transport_send(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), NipcError> {
        // SHM path (POSIX or Windows)
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm) = self.shm {
                let msg_len = HEADER_SIZE + payload.len();
                let msg = ensure_client_scratch(&mut self.send_buf, msg_len);

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
                let msg = ensure_client_scratch(&mut self.send_buf, msg_len);

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

    fn transport_send_request_buf(
        &mut self,
        hdr: &mut Header,
        req_len: usize,
    ) -> Result<(), NipcError> {
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm) = self.shm {
                let msg_len = HEADER_SIZE + req_len;
                let msg = ensure_client_scratch(&mut self.send_buf, msg_len);

                hdr.magic = MAGIC_MSG;
                hdr.version = VERSION;
                hdr.header_len = protocol::HEADER_LEN;
                hdr.payload_len = req_len as u32;

                hdr.encode(&mut msg[..HEADER_SIZE]);
                if req_len > 0 {
                    msg[HEADER_SIZE..HEADER_SIZE + req_len]
                        .copy_from_slice(&self.request_buf[..req_len]);
                }

                return shm.send(&msg[..msg_len]).map_err(|_| NipcError::Overflow);
            }
        }

        #[cfg(windows)]
        {
            if let Some(ref mut shm) = self.shm {
                let msg_len = HEADER_SIZE + req_len;
                let msg = ensure_client_scratch(&mut self.send_buf, msg_len);

                hdr.magic = MAGIC_MSG;
                hdr.version = VERSION;
                hdr.header_len = protocol::HEADER_LEN;
                hdr.payload_len = req_len as u32;

                hdr.encode(&mut msg[..HEADER_SIZE]);
                if req_len > 0 {
                    msg[HEADER_SIZE..HEADER_SIZE + req_len]
                        .copy_from_slice(&self.request_buf[..req_len]);
                }

                return shm.send(&msg[..msg_len]).map_err(|_| NipcError::Overflow);
            }
        }

        let session = self.session.as_mut().ok_or(NipcError::Truncated)?;
        session
            .send(hdr, &self.request_buf[..req_len])
            .map_err(|_| NipcError::Overflow)
    }

    /// Receive via the active transport. Returns (header, payload_view).
    fn transport_receive(&mut self) -> Result<(Header, ClientResponseRef), NipcError> {
        let needed = self.max_receive_message_bytes();
        let scratch = ensure_client_scratch(&mut self.transport_buf, needed);

        // SHM path (POSIX or Windows)
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm) = self.shm {
                let mlen = shm
                    .receive(scratch, 30000)
                    .map_err(|_| NipcError::Truncated)?;

                if mlen < HEADER_SIZE {
                    return Err(NipcError::Truncated);
                }

                let hdr = Header::decode(&scratch[..mlen])?;
                return Ok((
                    hdr,
                    ClientResponseRef {
                        source: ClientResponseSource::TransportBuf,
                        len: mlen - HEADER_SIZE,
                    },
                ));
            }
        }

        #[cfg(windows)]
        {
            if let Some(ref mut shm) = self.shm {
                let mlen = shm
                    .receive(scratch, 30000)
                    .map_err(|_| NipcError::Truncated)?;

                if mlen < HEADER_SIZE {
                    return Err(NipcError::Truncated);
                }

                let hdr = Header::decode(&scratch[..mlen])?;
                return Ok((
                    hdr,
                    ClientResponseRef {
                        source: ClientResponseSource::TransportBuf,
                        len: mlen - HEADER_SIZE,
                    },
                ));
            }
        }

        // Baseline transport: UDS on POSIX, Named Pipe on Windows
        let session = self.session.as_mut().ok_or(NipcError::Truncated)?;

        #[cfg(unix)]
        {
            let scratch_payload_ptr = unsafe { scratch.as_ptr().add(HEADER_SIZE) };
            let (hdr, payload) = session.receive(scratch).map_err(|_| NipcError::Truncated)?;
            let source = if payload.as_ptr() == scratch_payload_ptr {
                ClientResponseSource::TransportBuf
            } else {
                ClientResponseSource::SessionBuf
            };
            Ok((
                hdr,
                ClientResponseRef {
                    source,
                    len: payload.len(),
                },
            ))
        }

        #[cfg(windows)]
        {
            let scratch_payload_ptr = unsafe { scratch.as_ptr().add(HEADER_SIZE) };
            let (hdr, payload) = session.receive(scratch).map_err(|_| NipcError::Truncated)?;
            let source = if payload.as_ptr() == scratch_payload_ptr {
                ClientResponseSource::TransportBuf
            } else {
                ClientResponseSource::SessionBuf
            };
            Ok((
                hdr,
                ClientResponseRef {
                    source,
                    len: payload.len(),
                },
            ))
        }
    }

    fn response_payload(&self, response: ClientResponseRef) -> Result<&[u8], NipcError> {
        match response.source {
            ClientResponseSource::TransportBuf => {
                let start = HEADER_SIZE;
                let end = HEADER_SIZE + response.len;
                if end > self.transport_buf.len() {
                    return Err(NipcError::Truncated);
                }
                Ok(&self.transport_buf[start..end])
            }
            ClientResponseSource::SessionBuf => {
                #[cfg(unix)]
                {
                    let session = self.session.as_ref().ok_or(NipcError::Truncated)?;
                    return Ok(session.received_payload(response.len));
                }
                #[cfg(windows)]
                {
                    let session = self.session.as_ref().ok_or(NipcError::Truncated)?;
                    return Ok(session.received_payload(response.len));
                }
                #[allow(unreachable_code)]
                Err(NipcError::Truncated)
            }
        }
    }

    fn max_receive_message_bytes(&self) -> usize {
        let mut max_payload = self.transport_config.max_response_payload_bytes as usize;
        #[cfg(unix)]
        if let Some(ref session) = self.session {
            if session.max_response_payload_bytes > 0 {
                max_payload = session.max_response_payload_bytes as usize;
            }
        }
        #[cfg(windows)]
        if let Some(ref session) = self.session {
            if session.max_response_payload_bytes > 0 {
                max_payload = session.max_response_payload_bytes as usize;
            }
        }
        if max_payload == 0 {
            max_payload = CACHE_RESPONSE_BUF_SIZE;
        }
        HEADER_SIZE + max_payload
    }
}

impl Drop for CgroupsClient {
    fn drop(&mut self) {
        self.close();
    }
}

fn ensure_client_scratch(buf: &mut Vec<u8>, needed: usize) -> &mut [u8] {
    if buf.len() < needed {
        buf.resize(needed, 0);
    }
    &mut buf[..needed]
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ClientResponseSource {
    TransportBuf,
    SessionBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ClientResponseRef {
    source: ClientResponseSource,
    len: usize,
}

fn dispatch_single(
    handlers: &Handlers,
    method_code: u16,
    request: &[u8],
    response_buf: &mut [u8],
) -> (usize, bool) {
    match method_code {
        METHOD_INCREMENT => {
            let Some(handler) = handlers.on_increment.as_ref() else {
                return (0, false);
            };
            match protocol::dispatch_increment(request, response_buf, |value| handler(value)) {
                Some(n) => (n, true),
                None => (0, false),
            }
        }
        METHOD_STRING_REVERSE => {
            let Some(handler) = handlers.on_string_reverse.as_ref() else {
                return (0, false);
            };
            match protocol::dispatch_string_reverse(request, response_buf, |data| {
                let s = std::str::from_utf8(data).ok()?;
                handler(s).map(String::into_bytes)
            }) {
                Some(n) => (n, true),
                None => (0, false),
            }
        }
        METHOD_CGROUPS_SNAPSHOT => {
            let Some(handler) = handlers.on_snapshot.as_ref() else {
                return (0, false);
            };
            let max_items = handlers.snapshot_max_items(response_buf.len());
            if max_items == 0 {
                return (0, false);
            }
            match protocol::dispatch_cgroups_snapshot(
                request,
                response_buf,
                max_items,
                |req, builder| handler(req, builder),
            ) {
                Some(n) => (n, true),
                None => (0, false),
            }
        }
        _ => (0, false),
    }
}

// ---------------------------------------------------------------------------
//  Managed server
// ---------------------------------------------------------------------------

pub type IncrementHandler = Arc<dyn Fn(u64) -> Option<u64> + Send + Sync>;
pub type StringReverseHandler = Arc<dyn Fn(&str) -> Option<String> + Send + Sync>;
pub type SnapshotHandler =
    Arc<dyn for<'a> Fn(&CgroupsRequest, &mut protocol::CgroupsBuilder<'a>) -> bool + Send + Sync>;

/// Typed server handler surface for the cgroups service.
#[derive(Clone, Default)]
pub struct Handlers {
    pub on_increment: Option<IncrementHandler>,
    pub on_string_reverse: Option<StringReverseHandler>,
    pub on_snapshot: Option<SnapshotHandler>,
    pub snapshot_max_items: u32,
}

impl Handlers {
    fn snapshot_max_items(&self, response_buf_size: usize) -> u32 {
        if self.snapshot_max_items != 0 {
            return self.snapshot_max_items;
        }
        protocol::estimate_cgroups_max_items(response_buf_size)
    }
}

/// L2 managed server. Typed request/response dispatcher.
///
/// Handles accept, spawns a thread per session (up to worker_count),
/// reads requests, dispatches to handler, sends responses.
pub struct ManagedServer {
    run_dir: String,
    service_name: String,
    server_config: ServerConfig,
    handlers: Handlers,
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
        handlers: Handlers,
    ) -> Self {
        Self::with_workers(run_dir, service_name, config, handlers, 8)
    }

    /// Create a server with an explicit worker count limit.
    pub fn with_workers(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        handlers: Handlers,
        worker_count: usize,
    ) -> Self {
        ManagedServer {
            run_dir: run_dir.to_string(),
            service_name: service_name.to_string(),
            server_config: config,
            handlers,
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
            let handlers = self.handlers.clone();
            let running = self.running.clone();

            let t = std::thread::spawn(move || {
                handle_session_threaded(
                    session,
                    #[cfg(target_os = "linux")]
                    shm,
                    #[cfg(not(target_os = "linux"))]
                    shm,
                    handlers,
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

            let handlers = self.handlers.clone();
            let running = self.running.clone();

            let t = std::thread::spawn(move || {
                handle_session_win_threaded(session, shm, handlers, running);
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
                extern "system" {
                    fn CloseHandle(h: isize) -> i32;
                }
                unsafe {
                    CloseHandle(h as isize);
                }
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
    handlers: Handlers,
    running: Arc<AtomicBool>,
) {
    let mut recv_buf = vec![0u8; HEADER_SIZE + session.max_request_payload_bytes as usize];
    let mut resp_buf = vec![0u8; session.max_response_payload_bytes as usize];
    let mut item_resp_buf = vec![0u8; session.max_response_payload_bytes as usize];
    let mut msg_buf = vec![0u8; HEADER_SIZE + session.max_response_payload_bytes as usize];

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
                        let payload = &recv_buf[HEADER_SIZE..mlen];
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
        let is_batch = (hdr.flags & FLAG_BATCH) != 0 && hdr.item_count >= 1;
        let mut response_len = 0usize;
        let mut ok = true;

        if !is_batch {
            let (n, dispatch_ok) = dispatch_single(&handlers, hdr.code, payload, &mut resp_buf);
            ok = dispatch_ok && n <= resp_buf.len();
            if ok {
                response_len = n;
            }
        } else {
            let mut bb = BatchBuilder::new(&mut resp_buf, hdr.item_count);
            for i in 0..hdr.item_count {
                let (item_data, _item_len) = match batch_item_get(payload, hdr.item_count, i) {
                    Ok(v) => v,
                    Err(_) => {
                        ok = false;
                        break;
                    }
                };
                let (item_len, item_ok) =
                    dispatch_single(&handlers, hdr.code, item_data, &mut item_resp_buf);
                if !item_ok || item_len > item_resp_buf.len() {
                    ok = false;
                    break;
                }
                if bb.add(&item_resp_buf[..item_len]).is_err() {
                    ok = false;
                    break;
                }
            }
            if ok {
                let (n, _) = bb.finish();
                response_len = n;
            }
        }

        let mut resp_hdr = Header {
            kind: KIND_RESPONSE,
            code: hdr.code,
            message_id: hdr.message_id,
            ..Header::default()
        };

        if ok {
            resp_hdr.transport_status = STATUS_OK;
            if is_batch {
                resp_hdr.flags = FLAG_BATCH;
                resp_hdr.item_count = hdr.item_count;
            } else {
                resp_hdr.flags = 0;
                resp_hdr.item_count = 1;
            }
        } else {
            resp_hdr.transport_status = STATUS_INTERNAL_ERROR;
            resp_hdr.item_count = 1;
            resp_hdr.flags = 0;
            response_len = 0;
        }

        if let Some(ref mut shm_ctx) = shm {
            let msg_len = HEADER_SIZE + response_len;
            let msg = ensure_client_scratch(&mut msg_buf, msg_len);

            resp_hdr.magic = MAGIC_MSG;
            resp_hdr.version = VERSION;
            resp_hdr.header_len = protocol::HEADER_LEN;
            resp_hdr.payload_len = response_len as u32;

            resp_hdr.encode(&mut msg[..HEADER_SIZE]);
            if response_len > 0 {
                msg[HEADER_SIZE..].copy_from_slice(&resp_buf[..response_len]);
            }

            if shm_ctx.send(msg).is_err() {
                break;
            }
            continue;
        }

        if session
            .send(&mut resp_hdr, &resp_buf[..response_len])
            .is_err()
        {
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
    handlers: Handlers,
    running: Arc<AtomicBool>,
) {
    let mut recv_buf = vec![0u8; HEADER_SIZE + session.max_request_payload_bytes as usize];
    let mut resp_buf = vec![0u8; session.max_response_payload_bytes as usize];
    let mut item_resp_buf = vec![0u8; session.max_response_payload_bytes as usize];
    let mut msg_buf = vec![0u8; HEADER_SIZE + session.max_response_payload_bytes as usize];

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
                            let payload = &recv_buf[HEADER_SIZE..mlen];
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
                        Ok((hdr, payload)) => (hdr, payload),
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
        let is_batch = (hdr.flags & FLAG_BATCH) != 0 && hdr.item_count >= 1;
        let mut response_len = 0usize;
        let mut ok = true;

        if !is_batch {
            let (n, dispatch_ok) = dispatch_single(&handlers, hdr.code, payload, &mut resp_buf);
            ok = dispatch_ok && n <= resp_buf.len();
            if ok {
                response_len = n;
            }
        } else {
            let mut bb = BatchBuilder::new(&mut resp_buf, hdr.item_count);
            for i in 0..hdr.item_count {
                let (item_data, _item_len) = match batch_item_get(payload, hdr.item_count, i) {
                    Ok(v) => v,
                    Err(_) => {
                        ok = false;
                        break;
                    }
                };
                let (item_len, item_ok) =
                    dispatch_single(&handlers, hdr.code, item_data, &mut item_resp_buf);
                if !item_ok || item_len > item_resp_buf.len() {
                    ok = false;
                    break;
                }
                if bb.add(&item_resp_buf[..item_len]).is_err() {
                    ok = false;
                    break;
                }
            }
            if ok {
                let (n, _) = bb.finish();
                response_len = n;
            }
        }

        // Build response header
        let mut resp_hdr = Header {
            kind: KIND_RESPONSE,
            code: hdr.code,
            message_id: hdr.message_id,
            ..Header::default()
        };

        if ok {
            resp_hdr.transport_status = STATUS_OK;
            if is_batch {
                resp_hdr.flags = FLAG_BATCH;
                resp_hdr.item_count = hdr.item_count;
            } else {
                resp_hdr.flags = 0;
                resp_hdr.item_count = 1;
            }
        } else {
            resp_hdr.transport_status = STATUS_INTERNAL_ERROR;
            resp_hdr.item_count = 1;
            resp_hdr.flags = 0;
            response_len = 0;
        }

        // Send response via the active transport
        #[cfg(target_os = "linux")]
        {
            if let Some(ref mut shm_ctx) = shm {
                let msg_len = HEADER_SIZE + response_len;
                let msg = ensure_client_scratch(&mut msg_buf, msg_len);

                resp_hdr.magic = MAGIC_MSG;
                resp_hdr.version = VERSION;
                resp_hdr.header_len = protocol::HEADER_LEN;
                resp_hdr.payload_len = response_len as u32;

                resp_hdr.encode(&mut msg[..HEADER_SIZE]);
                if response_len > 0 {
                    msg[HEADER_SIZE..].copy_from_slice(&resp_buf[..response_len]);
                }

                if shm_ctx.send(msg).is_err() {
                    break;
                }
                continue;
            }
        }

        // UDS path
        if session
            .send(&mut resp_hdr, &resp_buf[..response_len])
            .is_err()
        {
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
        CgroupsCache {
            client: CgroupsClient::new(run_dir, service_name, config),
            items: Vec::new(),
            lookup_index: std::collections::HashMap::new(),
            systemd_enabled: 0,
            generation: 0,
            populated: false,
            refresh_success_count: 0,
            refresh_failure_count: 0,
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
        match self.client.call_snapshot() {
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
                                Err(_) => String::from_utf8_lossy(iv.path.as_bytes()).into_owned(),
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
    #[cfg(target_os = "linux")]
    use crate::protocol::PROFILE_SHM_FUTEX;
    use crate::protocol::{increment_encode, BatchBuilder, CgroupsBuilder, PROFILE_BASELINE};
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
        #[cfg(target_os = "linux")]
        crate::transport::shm::cleanup_stale(TEST_RUN_DIR, service);
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

    #[cfg(target_os = "linux")]
    fn shm_server_config() -> ServerConfig {
        ServerConfig {
            supported_profiles: PROFILE_BASELINE | PROFILE_SHM_FUTEX,
            preferred_profiles: PROFILE_SHM_FUTEX,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            backlog: 4,
            ..ServerConfig::default()
        }
    }

    #[cfg(target_os = "linux")]
    fn shm_client_config() -> ClientConfig {
        ClientConfig {
            supported_profiles: PROFILE_BASELINE | PROFILE_SHM_FUTEX,
            preferred_profiles: PROFILE_SHM_FUTEX,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            ..ClientConfig::default()
        }
    }

    fn batch_server_config() -> ServerConfig {
        ServerConfig {
            supported_profiles: PROFILE_BASELINE,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
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
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            ..ClientConfig::default()
        }
    }

    fn connect_ready(client: &mut CgroupsClient) {
        for _ in 0..200 {
            client.refresh();
            if client.ready() {
                return;
            }
            thread::sleep(Duration::from_millis(10));
        }

        panic!("client did not reach READY state");
    }

    fn fill_test_cgroups_snapshot(builder: &mut CgroupsBuilder<'_>) -> bool {
        let items = [
            (
                1001u32,
                0u32,
                1u32,
                b"docker-abc123" as &[u8],
                b"/sys/fs/cgroup/docker/abc123" as &[u8],
            ),
            (2002, 0, 1, b"k8s-pod-xyz", b"/sys/fs/cgroup/kubepods/xyz"),
            (
                3003,
                0,
                0,
                b"systemd-user",
                b"/sys/fs/cgroup/user.slice/user-1000",
            ),
        ];

        for (hash, options, enabled, name, path) in &items {
            if builder.add(*hash, *options, *enabled, name, path).is_err() {
                return false;
            }
        }

        true
    }

    fn test_cgroups_handlers() -> Handlers {
        Handlers {
            on_snapshot: Some(Arc::new(|req, builder| {
                if req.layout_version != 1 || req.flags != 0 {
                    return false;
                }
                builder.set_header(1, 42);
                fill_test_cgroups_snapshot(builder)
            })),
            snapshot_max_items: 3,
            ..Handlers::default()
        }
    }

    fn pingpong_handlers() -> Handlers {
        Handlers {
            on_increment: Some(Arc::new(|value| Some(value + 1))),
            on_string_reverse: Some(Arc::new(|s| Some(s.chars().rev().collect()))),
            ..Handlers::default()
        }
    }

    struct TestServer {
        stop_flag: Arc<AtomicBool>,
        thread: Option<thread::JoinHandle<()>>,
    }

    impl TestServer {
        fn start(service: &str, handlers: Handlers) -> Self {
            Self::start_with(service, server_config(), handlers, 8)
        }

        #[cfg(target_os = "linux")]
        fn start_shm(service: &str, handlers: Handlers) -> Self {
            Self::start_with(service, shm_server_config(), handlers, 8)
        }

        fn start_with_workers(service: &str, handlers: Handlers, worker_count: usize) -> Self {
            Self::start_with(service, server_config(), handlers, worker_count)
        }

        fn start_with(
            service: &str,
            config: ServerConfig,
            handlers: Handlers,
            worker_count: usize,
        ) -> Self {
            ensure_run_dir();
            cleanup_all(service);

            let svc = service.to_string();
            let ready_flag = Arc::new(AtomicBool::new(false));
            let ready_clone = ready_flag.clone();

            let mut server =
                ManagedServer::with_workers(TEST_RUN_DIR, &svc, config, handlers, worker_count);
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

        fn start_with_resp_size(service: &str, handlers: Handlers, resp_buf_size: usize) -> Self {
            ensure_run_dir();
            cleanup_all(service);

            let svc = service.to_string();
            let ready_flag = Arc::new(AtomicBool::new(false));
            let ready_clone = ready_flag.clone();

            let mut scfg = server_config();
            scfg.max_response_payload_bytes = resp_buf_size as u32;

            let mut server = ManagedServer::new(TEST_RUN_DIR, &svc, scfg, handlers);
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

    struct RawSessionServer {
        thread: Option<thread::JoinHandle<Result<(), String>>>,
    }

    fn start_raw_session_server<F>(service: &str, cfg: ServerConfig, handler: F) -> RawSessionServer
    where
        F: FnOnce(&mut UdsSession, Header, &[u8]) -> Result<(), String> + Send + 'static,
    {
        ensure_run_dir();
        cleanup_all(service);

        let svc = service.to_string();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let thread = thread::spawn(move || {
            let listener =
                UdsListener::bind(TEST_RUN_DIR, &svc, cfg).map_err(|e| format!("bind: {e}"))?;
            ready_clone.store(true, Ordering::Release);
            let mut session = listener.accept().map_err(|e| format!("accept: {e}"))?;

            let (hdr, payload) = {
                let mut recv_buf = vec![0u8; RESPONSE_BUF_SIZE];
                let (hdr, payload) = session
                    .receive(&mut recv_buf)
                    .map_err(|e| format!("receive: {e}"))?;
                (hdr, payload.to_vec())
            };

            handler(&mut session, hdr, &payload)
        });

        for _ in 0..2000 {
            if ready.load(Ordering::Acquire) {
                break;
            }
            thread::sleep(Duration::from_micros(500));
        }
        thread::sleep(Duration::from_millis(50));

        RawSessionServer {
            thread: Some(thread),
        }
    }

    impl RawSessionServer {
        fn wait(&mut self) {
            if let Some(thread) = self.thread.take() {
                match thread.join() {
                    Ok(Ok(())) => {}
                    Ok(Err(err)) => panic!("raw unix session server failed: {err}"),
                    Err(_) => panic!("raw unix session server panicked"),
                }
            }
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
        let mut server = TestServer::start(svc, test_cgroups_handlers());

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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        let view = client.call_snapshot().expect("call should succeed");

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

    #[cfg(target_os = "linux")]
    #[test]
    fn test_cgroups_call_shm() {
        let svc = "rs_svc_cgroups_shm";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start_shm(svc, test_cgroups_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, shm_client_config());
        connect_ready(&mut client);
        assert!(client.shm.is_some(), "expected SHM to be negotiated");
        assert_eq!(
            client.session.as_ref().map(|s| s.selected_profile),
            Some(PROFILE_SHM_FUTEX)
        );

        let view = client.call_snapshot().expect("snapshot over SHM");
        assert_eq!(view.item_count, 3);
        assert_eq!(view.generation, 42);
        assert_eq!(view.item(0).expect("item 0").hash, 1001);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_client_call_string_reverse_shm_success() {
        let svc = "rs_svc_strrev_shm";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start_shm(svc, pingpong_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, shm_client_config());
        connect_ready(&mut client);
        assert!(client.shm.is_some(), "expected SHM to be negotiated");

        let result = client
            .call_string_reverse("hello")
            .expect("string reverse over SHM");
        assert_eq!(result.as_str(), "olleh");

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_increment_batch_shm() {
        let svc = "rs_pp_batch_shm";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start_shm(svc, pingpong_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, shm_client_config());
        connect_ready(&mut client);
        assert!(client.shm.is_some(), "expected SHM to be negotiated");

        let values = vec![10u64, 20, 30, 40];
        let results = client
            .call_increment_batch(&values)
            .expect("batch over SHM");
        assert_eq!(results, vec![11, 21, 31, 41]);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_retry_on_failure() {
        let svc = "rs_svc_retry";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server1 = TestServer::start(svc, test_cgroups_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        // First call succeeds
        let view = client.call_snapshot().expect("first call");
        assert_eq!(view.item_count, 3);

        // Kill server
        server1.stop();
        cleanup_all(svc);
        thread::sleep(Duration::from_millis(50));

        // Restart server
        let mut server2 = TestServer::start(svc, test_cgroups_handlers());

        // Next call triggers reconnect + retry
        let view2 = client.call_snapshot().expect("retry call");
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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

        // Create and connect client 1
        let mut client1 = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client1.refresh();
        assert!(client1.ready());

        let view1 = client1.call_snapshot().expect("client 1 call");
        assert_eq!(view1.item_count, 3);

        // Now multi-client: keep client 1 open, connect client 2
        let mut client2 = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client2.refresh();
        assert!(client2.ready());

        let view2 = client2.call_snapshot().expect("client 2 call");
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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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
                    match client.call_snapshot() {
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

        let mut server = TestServer::start(svc, Handlers::default());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        // Call should fail (handler returns None -> INTERNAL_ERROR)
        let err = client.call_snapshot();
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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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
            client.call_snapshot().expect("call ok");
        }

        let s1 = client.status();
        assert_eq!(s1.call_count, 3);
        assert_eq!(s1.error_count, 0);

        // Call on disconnected client
        client.close();
        let err = client.call_snapshot();
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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

        // Connect via raw UDS session
        let mut session =
            UdsSession::connect(TEST_RUN_DIR, svc, &client_config()).expect("connect");

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
            let req = CgroupsRequest {
                layout_version: 1,
                flags: 0,
            };
            let mut req_buf = [0u8; 4];
            req.encode(&mut req_buf);
            let _ = session.send(&mut hdr2, &req_buf);
            let recv = session.receive(&mut recv_buf);
            assert!(
                recv.is_err(),
                "server should have terminated session after non-request message"
            );
        }

        drop(session);

        // Verify server is still alive: connect a new client and do a normal call
        let mut verify_client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        verify_client.refresh();
        assert!(
            verify_client.ready(),
            "server should still be alive after bad client"
        );

        let view = verify_client
            .call_snapshot()
            .expect("normal call should succeed after bad client");
        assert_eq!(
            view.item_count, 3,
            "response should be correct after bad client"
        );

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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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

        let mut server1 = TestServer::start(svc, test_cgroups_handlers());

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());
        assert!(cache.refresh());
        assert_eq!(cache.status().item_count, 3);

        // Kill and restart server
        server1.stop();
        cleanup_all(svc);
        thread::sleep(Duration::from_millis(50));

        let mut server2 = TestServer::start(svc, test_cgroups_handlers());

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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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
        fn large_handlers() -> Handlers {
            Handlers {
                on_snapshot: Some(Arc::new(|req, builder| {
                    if req.layout_version != 1 || req.flags != 0 {
                        return false;
                    }
                    builder.set_header(1, 100);

                    for i in 0..N {
                        let name = format!("cgroup-{i}");
                        let path = format!("/sys/fs/cgroup/test/{i}");
                        if builder
                            .add(
                                i + 1000,
                                0,
                                if i % 3 == 0 { 0 } else { 1 },
                                name.as_bytes(),
                                path.as_bytes(),
                            )
                            .is_err()
                        {
                            return false;
                        }
                    }

                    true
                })),
                snapshot_max_items: N,
                ..Handlers::default()
            }
        }

        // Use a larger response buf size
        let mut cfg = client_config();
        cfg.max_response_payload_bytes = 256 * N;

        let mut server = TestServer::start_with_resp_size(svc, large_handlers(), 256 * N as usize);

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, cfg);

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

    #[test]
    fn test_cache_refresh_lossy_utf8() {
        let svc = "rs_cache_lossy";
        ensure_run_dir();
        cleanup_all(svc);

        let handlers = Handlers {
            on_snapshot: Some(Arc::new(|_, builder| {
                builder.set_header(1, 7);
                builder
                    .add(1001, 0, 1, b"bad-\xFF-name", b"/bad/\xFF/path")
                    .is_ok()
            })),
            snapshot_max_items: 1,
            ..Handlers::default()
        };

        let mut server = TestServer::start(svc, handlers);
        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());

        assert!(cache.refresh(), "cache refresh should succeed");
        let item = cache
            .lookup(1001, "bad-\u{FFFD}-name")
            .expect("lossy lookup");
        assert_eq!(item.name, "bad-\u{FFFD}-name");
        assert_eq!(item.path, "/bad/\u{FFFD}/path");

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
            hash = hash
                .wrapping_shl(5)
                .wrapping_add(hash)
                .wrapping_add(c as u32);
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

            let handlers = Handlers {
                on_snapshot: Some(Arc::new(move |req, builder| {
                    if req.layout_version != 1 || req.flags != 0 {
                        return false;
                    }
                    builder.set_header(1, 42);

                    for i in 0..n {
                        let name = format!("container-{i:04}");
                        let path = format!("/sys/fs/cgroup/docker/{i:04}");
                        let hash = simple_hash(&name);
                        let enabled = if i % 5 == 0 { 0 } else { 1 };
                        if builder
                            .add(hash, 0x10, enabled, name.as_bytes(), path.as_bytes())
                            .is_err()
                        {
                            return false;
                        }
                    }

                    true
                })),
                snapshot_max_items: n,
                ..Handlers::default()
            };

            let mut server = ManagedServer::new(TEST_RUN_DIR, &svc, scfg, handlers);
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
        let view = client.call_snapshot().expect("call should succeed");
        let elapsed = start.elapsed();

        eprintln!("  1000 items: {:?}", elapsed);

        assert_eq!(view.item_count, N);
        assert_eq!(view.systemd_enabled, 1);
        assert_eq!(view.generation, 42);

        // Verify ALL items
        for i in 0..N {
            let item = view
                .item(i)
                .unwrap_or_else(|_| panic!("item {i} decode failed"));
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
        let view = client.call_snapshot().expect("call should succeed");
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

        let mut server = TestServer::start_with_workers(svc, test_cgroups_handlers(), 64);

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
                    match client.call_snapshot() {
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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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

            match client.call_snapshot() {
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
        eprintln!(
            "  {CYCLES} rapid cycles: {successes} ok, {failures} fail, {:?}",
            elapsed
        );

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

        let mut server = TestServer::start_with_workers(svc, test_cgroups_handlers(), 16);

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

        let mut server = TestServer::start(svc, test_cgroups_handlers());

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

    #[test]
    fn test_increment_ping_pong() {
        let svc = "rs_pp_incr";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready(), "client not ready");

        // Ping-pong: send 0 -> get 1 -> send 1 -> get 2 -> ... -> 10
        let mut value = 0u64;
        let mut responses_received = 0u64;
        for round in 0..10 {
            let sent = value;
            let result = client
                .call_increment(sent)
                .unwrap_or_else(|e| panic!("round {round}: call_increment({sent}) failed: {e:?}"));
            assert_eq!(
                result,
                sent + 1,
                "round {round}: expected {} got {result}",
                sent + 1
            );
            responses_received += 1;
            value = result;
        }
        assert_eq!(
            responses_received, 10,
            "expected 10 responses, got {responses_received}"
        );
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

        let mut server = TestServer::start(svc, pingpong_handlers());

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
            let result = client.call_string_reverse(&sent).unwrap_or_else(|e| {
                panic!("round {round}: call_string_reverse({sent:?}) failed: {e:?}")
            });
            assert_eq!(
                result.as_str(),
                expected,
                "round {round}: reverse of {sent:?} should be {expected:?}, got {result:?}"
            );
            responses_received += 1;
            current = result.as_str().to_string();
        }
        assert_eq!(
            responses_received, 6,
            "expected 6 responses, got {responses_received}"
        );
        // even number of reversals = identity
        assert_eq!(
            current, original,
            "6 reversals should restore original string"
        );

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_mixed_methods() {
        let svc = "rs_pp_mixed";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready(), "client not ready");

        // Interleave increment and string_reverse calls
        let inc_input_1 = 100u64;
        let v1 = client.call_increment(inc_input_1).expect("increment(100)");
        assert_eq!(
            v1,
            inc_input_1 + 1,
            "increment({inc_input_1}) should be {}",
            inc_input_1 + 1
        );

        let str_input_1 = "hello";
        let expected_s1: String = str_input_1.chars().rev().collect();
        let s1 = client
            .call_string_reverse(str_input_1)
            .expect("reverse(hello)");
        assert_eq!(
            s1.as_str(),
            expected_s1,
            "reverse of {str_input_1:?} should be {expected_s1:?}"
        );

        let inc_input_2 = v1;
        let v2 = client.call_increment(inc_input_2).expect("increment(101)");
        assert_eq!(
            v2,
            inc_input_2 + 1,
            "increment({inc_input_2}) should be {}",
            inc_input_2 + 1
        );

        let str_input_2 = "world";
        let expected_s2: String = str_input_2.chars().rev().collect();
        let s2 = client
            .call_string_reverse(str_input_2)
            .expect("reverse(world)");
        assert_eq!(
            s2.as_str(),
            expected_s2,
            "reverse of {str_input_2:?} should be {expected_s2:?}"
        );

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
            pingpong_handlers(),
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
            assert_eq!(
                output,
                input + 1,
                "batch item {i}: expected {}, got {output}",
                input + 1
            );
        }

        // Single item batch
        let single = client
            .call_increment_batch(&[99])
            .expect("single-item batch");
        assert_eq!(single, vec![100]);

        // Empty batch
        let empty = client.call_increment_batch(&[]).expect("empty batch");
        assert!(empty.is_empty());

        client.close();
        stop_flag.store(false, Ordering::Release);
        let _ = thread_handle.join();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Client state machine: auth failure (lines 438-440)
    // ---------------------------------------------------------------

    #[test]
    fn test_client_auth_failure() {
        let svc = "rs_svc_authfail";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handlers());

        // Client with wrong auth token
        let mut bad_cfg = client_config();
        bad_cfg.auth_token = 0xBAD_BAD_BAD;

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, bad_cfg);
        client.refresh();
        assert_eq!(client.state, ClientState::AuthFailed);
        assert!(!client.ready());

        // Subsequent refresh stays stuck in AuthFailed
        client.refresh();
        assert_eq!(client.state, ClientState::AuthFailed);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Client state machine: incompatible (lines 439-440)
    // ---------------------------------------------------------------

    #[test]
    fn test_client_incompatible() {
        let svc = "rs_svc_incompat";
        ensure_run_dir();
        cleanup_all(svc);

        // Server supports only PROFILE_BASELINE, but start it first
        let mut server = TestServer::start(svc, test_cgroups_handlers());

        // Client requires SHM_FUTEX only (no baseline)
        let mut bad_cfg = client_config();
        #[cfg(target_os = "linux")]
        {
            bad_cfg.supported_profiles = crate::protocol::PROFILE_SHM_FUTEX;
        }
        #[cfg(not(target_os = "linux"))]
        {
            // On non-Linux, use a profile bit that won't match
            bad_cfg.supported_profiles = 0x80000000;
        }

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, bad_cfg);
        client.refresh();
        assert_eq!(client.state, ClientState::Incompatible);
        assert!(!client.ready());

        // Stays stuck
        client.refresh();
        assert_eq!(client.state, ClientState::Incompatible);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Client: call_snapshot when not ready (line 324-326)
    // ---------------------------------------------------------------

    #[test]
    fn test_call_when_not_ready() {
        let svc = "rs_svc_noready";
        ensure_run_dir();
        cleanup_all(svc);

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        assert_eq!(client.state, ClientState::Disconnected);

        let result = client.call_snapshot();
        assert!(result.is_err());
        assert_eq!(client.status().error_count, 1);

        // Increment when not ready
        let result = client.call_increment(42);
        assert!(result.is_err());

        // String reverse when not ready
        let result = client.call_string_reverse("test");
        assert!(result.is_err());

        client.close();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Client: broken -> reconnect cycle (lines 136-141)
    // ---------------------------------------------------------------

    #[test]
    fn test_broken_reconnect() {
        let svc = "rs_svc_broken";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert_eq!(client.state, ClientState::Ready);

        // Force broken state
        client.state = ClientState::Broken;

        // refresh from Broken should disconnect, reconnect
        let changed = client.refresh();
        assert!(changed);
        assert_eq!(client.state, ClientState::Ready);
        assert!(client.status().reconnect_count >= 1);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Cache: close resets everything (line 1561-1566)
    // ---------------------------------------------------------------

    #[test]
    fn test_cache_close_resets() {
        let svc = "rs_cache_close";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, test_cgroups_handlers());

        let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());
        assert!(cache.refresh());
        assert!(cache.ready());
        assert_eq!(cache.status().item_count, 3);

        cache.close();
        assert!(!cache.ready());
        assert!(cache.lookup(1001, "docker-abc123").is_none());
        assert_eq!(cache.status().item_count, 0);

        server.stop();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Cache: with max_response_payload_bytes = 0 (line 1437-1440)
    // ---------------------------------------------------------------

    #[test]
    fn test_cache_default_buf_size() {
        let svc = "rs_cache_defbuf";
        ensure_run_dir();
        cleanup_all(svc);

        // Config with max_response_payload_bytes = 0 triggers default buf
        let mut cfg = client_config();
        cfg.max_response_payload_bytes = 0;

        let cache = CgroupsCache::new(TEST_RUN_DIR, svc, cfg);
        assert_eq!(
            cache.client.max_receive_message_bytes(),
            HEADER_SIZE + CACHE_RESPONSE_BUF_SIZE
        );

        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  ManagedServer: worker_count = 0 -> clamped to 1 (line 778)
    // ---------------------------------------------------------------

    #[test]
    fn test_server_worker_count_clamped() {
        let svc = "rs_svc_w0";
        ensure_run_dir();
        cleanup_all(svc);

        let server =
            ManagedServer::with_workers(TEST_RUN_DIR, svc, server_config(), Handlers::default(), 0);
        assert_eq!(server.worker_count, 1);

        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  ManagedServer: stop flag (line 946)
    // ---------------------------------------------------------------

    #[test]
    fn test_server_stop_flag() {
        let svc = "rs_svc_stopflag";
        ensure_run_dir();
        cleanup_all(svc);

        let server = ManagedServer::new(TEST_RUN_DIR, svc, server_config(), Handlers::default());
        let flag = server.running_flag();
        assert!(!flag.load(Ordering::Acquire));

        // stop sets running to false
        server.stop();
        assert!(!flag.load(Ordering::Acquire));

        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  ClientStatus / CgroupsCacheStatus fields
    // ---------------------------------------------------------------

    #[test]
    fn test_client_status_fields() {
        let svc = "rs_svc_csf";
        ensure_run_dir();
        cleanup_all(svc);

        let client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        let status = client.status();
        assert_eq!(status.state, ClientState::Disconnected);
        assert_eq!(status.connect_count, 0);
        assert_eq!(status.reconnect_count, 0);
        assert_eq!(status.call_count, 0);
        assert_eq!(status.error_count, 0);
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  call_increment and call_string_reverse success paths
    // ---------------------------------------------------------------

    #[test]
    fn test_client_call_increment_success() {
        let svc = "rs_svc_incr_ok";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        let result = client.call_increment(99).expect("increment");
        assert_eq!(result, 100);

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_client_call_string_reverse_success() {
        let svc = "rs_svc_strrev_ok";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server = TestServer::start(svc, pingpong_handlers());

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
        client.refresh();
        assert!(client.ready());

        let result = client.call_string_reverse("hello").expect("reverse");
        assert_eq!(result.as_str(), "olleh");

        client.close();
        server.stop();
        cleanup_all(svc);
    }

    #[test]
    fn test_dispatch_single_helper_paths() {
        let mut response_buf = [0u8; 128];

        let (n, ok) = dispatch_single(
            &Handlers::default(),
            METHOD_INCREMENT,
            &[0; 8],
            &mut response_buf,
        );
        assert_eq!((n, ok), (0, false));

        let (n, ok) = dispatch_single(
            &Handlers::default(),
            METHOD_STRING_REVERSE,
            &[0; 8],
            &mut response_buf,
        );
        assert_eq!((n, ok), (0, false));

        let snapshot_handlers = Handlers {
            on_snapshot: Some(Arc::new(|_, _| true)),
            snapshot_max_items: 0,
            ..Handlers::default()
        };
        let (n, ok) = dispatch_single(
            &snapshot_handlers,
            METHOD_CGROUPS_SNAPSHOT,
            &[1, 0, 0, 0],
            &mut [],
        );
        assert_eq!((n, ok), (0, false));

        let (n, ok) = dispatch_single(&Handlers::default(), 0xFFFF, &[], &mut response_buf);
        assert_eq!((n, ok), (0, false));

        assert!(
            Handlers {
                snapshot_max_items: 0,
                ..Handlers::default()
            }
            .snapshot_max_items(4096)
                > 0,
            "default snapshot item estimate should be positive for a non-empty buffer"
        );
    }

    #[test]
    fn test_response_payload_transport_buf_bounds() {
        let mut client = CgroupsClient::new(TEST_RUN_DIR, "rs_payload_bounds", client_config());
        client.transport_buf.resize(HEADER_SIZE + 8, 0);

        let response = ClientResponseRef {
            source: ClientResponseSource::TransportBuf,
            len: 16,
        };

        assert_eq!(client.response_payload(response), Err(NipcError::Truncated));
    }

    #[test]
    fn test_call_increment_rejects_malformed_response_envelope_unix() {
        struct Case {
            name: &'static str,
            kind: u16,
            code: u16,
            status: u16,
            want: NipcError,
        }

        let cases = [
            Case {
                name: "bad kind",
                kind: KIND_REQUEST,
                code: METHOD_INCREMENT,
                status: STATUS_OK,
                want: NipcError::BadKind,
            },
            Case {
                name: "bad code",
                kind: KIND_RESPONSE,
                code: METHOD_STRING_REVERSE,
                status: STATUS_OK,
                want: NipcError::BadLayout,
            },
            Case {
                name: "bad status",
                kind: KIND_RESPONSE,
                code: METHOD_INCREMENT,
                status: STATUS_INTERNAL_ERROR,
                want: NipcError::BadLayout,
            },
        ];

        for tc in cases {
            let svc = format!("rs_unix_inc_env_{}", tc.name.replace(' ', "_"));
            let mut server =
                start_raw_session_server(&svc, server_config(), move |session, req_hdr, _| {
                    let mut payload = [0u8; INCREMENT_PAYLOAD_SIZE];
                    let n = increment_encode(43, &mut payload);
                    if n != INCREMENT_PAYLOAD_SIZE {
                        return Err(format!("increment_encode returned {n}"));
                    }

                    let mut resp_hdr = Header {
                        kind: tc.kind,
                        code: tc.code,
                        flags: 0,
                        item_count: 1,
                        message_id: req_hdr.message_id,
                        transport_status: tc.status,
                        ..Header::default()
                    };
                    session
                        .send(&mut resp_hdr, &payload)
                        .map_err(|e| format!("send: {e}"))
                });

            let mut client = CgroupsClient::new(TEST_RUN_DIR, &svc, client_config());
            connect_ready(&mut client);

            let err = client.call_increment(42).expect_err(tc.name);
            assert_eq!(err, tc.want, "{}", tc.name);

            client.close();
            server.wait();
            cleanup_all(&svc);
        }
    }

    #[test]
    fn test_call_increment_batch_rejects_wrong_item_count_unix() {
        let svc = "rs_unix_batch_count";
        ensure_run_dir();
        cleanup_all(svc);

        let mut server =
            start_raw_session_server(svc, batch_server_config(), move |session, req_hdr, _| {
                let mut encoded = [0u8; INCREMENT_PAYLOAD_SIZE];
                let n = increment_encode(11, &mut encoded);
                if n != INCREMENT_PAYLOAD_SIZE {
                    return Err(format!("increment_encode returned {n}"));
                }

                let mut response_buf = vec![0u8; 128];
                let resp_len = {
                    let mut batch = BatchBuilder::new(&mut response_buf, 2);
                    batch
                        .add(&encoded)
                        .map_err(|e| format!("batch add 1: {e:?}"))?;
                    batch
                        .add(&encoded)
                        .map_err(|e| format!("batch add 2: {e:?}"))?;
                    let (len, _count) = batch.finish();
                    len
                };

                let mut resp_hdr = Header {
                    kind: KIND_RESPONSE,
                    code: METHOD_INCREMENT,
                    flags: FLAG_BATCH,
                    item_count: 1,
                    message_id: req_hdr.message_id,
                    transport_status: STATUS_OK,
                    ..Header::default()
                };
                session
                    .send(&mut resp_hdr, &response_buf[..resp_len])
                    .map_err(|e| format!("send: {e}"))
            });

        let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, batch_client_config());
        connect_ready(&mut client);

        let err = client
            .call_increment_batch(&[10, 20])
            .expect_err("wrong batch item_count");
        assert_eq!(err, NipcError::BadItemCount);

        client.close();
        server.wait();
        cleanup_all(svc);
    }

    // ---------------------------------------------------------------
    //  Batch dispatch: handler failure returns INTERNAL_ERROR (lines 1244-1248)
    // ---------------------------------------------------------------

    #[test]
    fn test_batch_dispatch_handler_failure() {
        let svc = "rs_svc_batchfail";
        ensure_run_dir();
        cleanup_all(svc);

        // Handler that fails on the 2nd item
        fn fail_second_increment_handler() -> IncrementHandler {
            static CALL_COUNT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            Arc::new(move |value| {
                let n = CALL_COUNT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                if n % 3 == 1 {
                    return None;
                }
                Some(value + 1)
            })
        }

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

        let svc_name = svc.to_string();
        let ready_flag = Arc::new(AtomicBool::new(false));
        let ready_clone = ready_flag.clone();

        let mut server_obj = ManagedServer::with_workers(
            TEST_RUN_DIR,
            &svc_name,
            batch_server_config(),
            Handlers {
                on_increment: Some(fail_second_increment_handler()),
                ..Handlers::default()
            },
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
        assert!(client.ready());

        // Batch of 3: handler fails on the 2nd -> server returns INTERNAL_ERROR
        let values = vec![10u64, 20, 30];
        let result = client.call_increment_batch(&values);
        // The batch should fail because the handler returned None for item 2
        assert!(result.is_err());

        client.close();
        stop_flag.store(false, Ordering::Release);
        let _ = thread_handle.join();
        cleanup_all(svc);
    }
}

#[cfg(all(test, windows))]
#[path = "cgroups_windows_tests.rs"]
mod windows_tests;
