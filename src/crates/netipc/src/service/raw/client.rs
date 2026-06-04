use super::common::{
    ensure_client_scratch, next_power_of_2_u32, CACHE_RESPONSE_BUF_SIZE,
    CLIENT_SHM_ATTACH_RETRY_INTERVAL_MS, CLIENT_SHM_ATTACH_RETRY_TIMEOUT_MS,
};
use crate::protocol::{
    self, Header, NipcError, HEADER_SIZE, KIND_REQUEST, KIND_RESPONSE, MAGIC_MSG, MAX_PAYLOAD_CAP,
    STATUS_LIMIT_EXCEEDED, STATUS_OK, VERSION,
};

#[cfg(unix)]
pub(super) use crate::transport::posix::ClientConfig;

#[cfg(unix)]
use crate::protocol::{PROFILE_SHM_FUTEX, PROFILE_SHM_HYBRID};

#[cfg(unix)]
use crate::transport::posix::UdsSession;

#[cfg(target_os = "linux")]
use crate::transport::shm::ShmContext;

#[cfg(windows)]
pub(super) use crate::transport::windows::ClientConfig;

#[cfg(windows)]
use crate::transport::windows::{NpError, NpSession};

#[cfg(windows)]
use crate::transport::win_shm::{
    WinShmContext, PROFILE_BUSYWAIT as WIN_SHM_PROFILE_BUSYWAIT,
    PROFILE_HYBRID as WIN_SHM_PROFILE_HYBRID,
};

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

/// L2 client context bound to one service kind.
///
/// Manages connection lifecycle and provides typed blocking calls with
/// at-least-once retry semantics. The outer request code remains only for
/// validation; each client instance is bound to one expected request kind.
pub struct RawClient {
    pub(super) state: ClientState,
    pub(super) run_dir: String,
    pub(super) service_name: String,
    pub(super) expected_method_code: u16,
    pub(super) transport_config: ClientConfig,

    #[cfg(unix)]
    pub(super) session: Option<UdsSession>,
    #[cfg(target_os = "linux")]
    pub(super) shm: Option<ShmContext>,

    #[cfg(windows)]
    pub(super) session: Option<NpSession>,
    #[cfg(windows)]
    pub(super) shm: Option<WinShmContext>,

    pub(super) request_buf: Vec<u8>,
    pub(super) send_buf: Vec<u8>,
    pub(super) transport_buf: Vec<u8>,

    pub(super) connect_count: u32,
    pub(super) reconnect_count: u32,
    pub(super) call_count: u32,
    pub(super) error_count: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct RawCallKind {
    flags: u16,
    item_count: u32,
    check_item_count: bool,
}

impl RawCallKind {
    pub(super) fn single() -> Self {
        Self {
            flags: 0,
            item_count: 1,
            check_item_count: false,
        }
    }

    pub(super) fn batch(item_count: u32) -> Self {
        Self {
            flags: protocol::FLAG_BATCH,
            item_count,
            check_item_count: true,
        }
    }
}

impl RawClient {
    pub(super) fn new_bound(
        run_dir: &str,
        service_name: &str,
        expected_method_code: u16,
        config: ClientConfig,
    ) -> Self {
        RawClient {
            state: ClientState::Disconnected,
            run_dir: run_dir.to_string(),
            service_name: service_name.to_string(),
            expected_method_code,
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

    pub(super) fn request_scratch(&mut self, needed: usize) -> &mut [u8] {
        ensure_client_scratch(&mut self.request_buf, needed)
    }

    pub(super) fn validate_method(&self, method_code: u16) -> Result<(), NipcError> {
        if self.expected_method_code == method_code {
            Ok(())
        } else {
            Err(NipcError::BadLayout)
        }
    }

    pub(super) fn session_max_request_payload_bytes(&self) -> u32 {
        #[cfg(unix)]
        if let Some(ref session) = self.session {
            return session.max_request_payload_bytes;
        }

        #[cfg(windows)]
        if let Some(ref session) = self.session {
            return session.max_request_payload_bytes;
        }

        self.transport_config.max_request_payload_bytes
    }

    pub(super) fn session_max_response_payload_bytes(&self) -> u32 {
        #[cfg(unix)]
        if let Some(ref session) = self.session {
            return session.max_response_payload_bytes;
        }

        #[cfg(windows)]
        if let Some(ref session) = self.session {
            return session.max_response_payload_bytes;
        }

        self.transport_config.max_response_payload_bytes
    }

    fn client_note_request_capacity(&mut self, payload_len: u32) {
        let grown = next_power_of_2_u32(payload_len).min(MAX_PAYLOAD_CAP);
        if grown > self.transport_config.max_request_payload_bytes {
            self.transport_config.max_request_payload_bytes = grown;
        }
    }

    fn client_note_response_capacity(&mut self, payload_len: u32) {
        let grown = next_power_of_2_u32(payload_len).min(MAX_PAYLOAD_CAP);
        if grown > self.transport_config.max_response_payload_bytes {
            self.transport_config.max_response_payload_bytes = grown;
        }
    }

    /// Tear down connection and release resources.
    pub fn close(&mut self) {
        self.disconnect();
        self.state = ClientState::Disconnected;
    }

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

                #[cfg(target_os = "linux")]
                {
                    if selected_profile == PROFILE_SHM_HYBRID
                        || selected_profile == PROFILE_SHM_FUTEX
                    {
                        let mut shm_ok = false;
                        let deadline = std::time::Instant::now()
                            + std::time::Duration::from_millis(CLIENT_SHM_ATTACH_RETRY_TIMEOUT_MS);
                        loop {
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
                                    if std::time::Instant::now() >= deadline {
                                        break;
                                    }
                                    std::thread::sleep(std::time::Duration::from_millis(
                                        CLIENT_SHM_ATTACH_RETRY_INTERVAL_MS,
                                    ));
                                }
                            }
                        }
                        if !shm_ok {
                            drop(session);
                            self.transport_config.supported_profiles &=
                                !(PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
                            self.transport_config.preferred_profiles &=
                                !(PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
                            if self.transport_config.supported_profiles == 0 {
                                return ClientState::Disconnected;
                            }
                            return self.try_connect();
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
                    UdsError::Incompatible(_) => ClientState::Incompatible,
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

                if selected_profile == WIN_SHM_PROFILE_HYBRID
                    || selected_profile == WIN_SHM_PROFILE_BUSYWAIT
                {
                    let mut shm_ok = false;
                    let deadline = std::time::Instant::now()
                        + std::time::Duration::from_millis(CLIENT_SHM_ATTACH_RETRY_TIMEOUT_MS);
                    loop {
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
                                if std::time::Instant::now() >= deadline {
                                    break;
                                }
                                std::thread::sleep(std::time::Duration::from_millis(
                                    CLIENT_SHM_ATTACH_RETRY_INTERVAL_MS,
                                ));
                            }
                        }
                    }
                    if !shm_ok {
                        drop(session);
                        self.transport_config.supported_profiles &=
                            !(WIN_SHM_PROFILE_HYBRID | WIN_SHM_PROFILE_BUSYWAIT);
                        self.transport_config.preferred_profiles &=
                            !(WIN_SHM_PROFILE_HYBRID | WIN_SHM_PROFILE_BUSYWAIT);
                        if self.transport_config.supported_profiles == 0 {
                            return ClientState::Disconnected;
                        }
                        return self.try_connect();
                    }
                }

                self.session = Some(session);
                ClientState::Ready
            }
            Err(e) => match e {
                NpError::Connect(_) => ClientState::NotFound,
                NpError::AuthFailed => ClientState::AuthFailed,
                NpError::NoProfile => ClientState::Incompatible,
                NpError::Incompatible(_) => ClientState::Incompatible,
                _ => ClientState::Disconnected,
            },
        }
    }

    /// Reconnect-driven recovery for raw calls.
    ///
    /// Ordinary failures retry once. Overflow-driven resize recovery may
    /// reconnect more than once while negotiated capacities grow.
    pub(super) fn raw_call_with_retry(
        &mut self,
        method_code: u16,
        req_len: usize,
        call: RawCallKind,
    ) -> Result<ClientResponseRef, NipcError> {
        if self.state != ClientState::Ready {
            self.error_count += 1;
            return Err(NipcError::BadLayout);
        }

        let mut overflow_retries = 0u32;
        loop {
            let prev_req = self.session_max_request_payload_bytes();
            let prev_resp = self.session_max_response_payload_bytes();
            let prev_cfg_req = self.transport_config.max_request_payload_bytes;
            let prev_cfg_resp = self.transport_config.max_response_payload_bytes;

            match self.do_raw_call(method_code, req_len, call) {
                Ok(payload) => {
                    self.call_count += 1;
                    return Ok(payload);
                }
                Err(first_err) => {
                    if first_err != NipcError::Overflow {
                        self.disconnect();
                        self.state = ClientState::Broken;
                        self.state = self.try_connect();
                        if self.state != ClientState::Ready {
                            self.error_count += 1;
                            return Err(first_err);
                        }
                        self.reconnect_count += 1;

                        match self.do_raw_call(method_code, req_len, call) {
                            Ok(payload) => {
                                self.call_count += 1;
                                return Ok(payload);
                            }
                            Err(retry_err) => {
                                self.disconnect();
                                self.state = ClientState::Broken;
                                self.error_count += 1;
                                return Err(retry_err);
                            }
                        }
                    }

                    self.disconnect();
                    self.state = ClientState::Broken;
                    self.state = self.try_connect();
                    if self.state != ClientState::Ready {
                        self.error_count += 1;
                        return Err(first_err);
                    }
                    self.reconnect_count += 1;

                    if self.session_max_request_payload_bytes() <= prev_req
                        && self.session_max_response_payload_bytes() <= prev_resp
                        && self.transport_config.max_request_payload_bytes <= prev_cfg_req
                        && self.transport_config.max_response_payload_bytes <= prev_cfg_resp
                    {
                        self.disconnect();
                        self.state = ClientState::Broken;
                        self.error_count += 1;
                        return Err(first_err);
                    }

                    overflow_retries += 1;
                    if overflow_retries >= 8 {
                        self.disconnect();
                        self.state = ClientState::Broken;
                        self.error_count += 1;
                        return Err(first_err);
                    }
                }
            }
        }
    }

    /// Single attempt at a raw call.
    fn do_raw_call(
        &mut self,
        method_code: u16,
        req_len: usize,
        call: RawCallKind,
    ) -> Result<ClientResponseRef, NipcError> {
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: method_code,
            flags: call.flags,
            item_count: call.item_count,
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

        match resp_hdr.transport_status {
            STATUS_OK => {}
            STATUS_LIMIT_EXCEEDED => {
                let current = self.session_max_response_payload_bytes();
                if current > 0 {
                    self.client_note_response_capacity(current.saturating_mul(2));
                }
                return Err(NipcError::Overflow);
            }
            _ => return Err(NipcError::BadLayout),
        }

        if call.check_item_count && resp_hdr.item_count != call.item_count {
            return Err(NipcError::BadItemCount);
        }

        Ok(response)
    }

    /// Compatibility test seam for sending a borrowed payload through the
    /// single shared request-buffer transport path.
    #[cfg(test)]
    #[allow(dead_code)]
    pub(super) fn transport_send(
        &mut self,
        hdr: &mut Header,
        payload: &[u8],
    ) -> Result<(), NipcError> {
        let req = self.request_scratch(payload.len());
        req.copy_from_slice(payload);
        self.transport_send_request_buf(hdr, payload.len())
    }

    /// Send via the active transport (SHM if available, baseline otherwise).
    pub(super) fn transport_send_request_buf(
        &mut self,
        hdr: &mut Header,
        req_len: usize,
    ) -> Result<(), NipcError> {
        let max_request_payload_bytes = self.session_max_request_payload_bytes();

        #[cfg(target_os = "linux")]
        {
            if self.shm.is_some() {
                if req_len > max_request_payload_bytes as usize {
                    self.client_note_request_capacity(req_len as u32);
                    return Err(NipcError::Overflow);
                }

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

                let send_result = self.shm.as_mut().unwrap().send(&msg[..msg_len]);
                return match send_result {
                    Ok(()) => Ok(()),
                    Err(crate::transport::shm::ShmError::MsgTooLarge) => {
                        self.client_note_request_capacity(req_len as u32);
                        Err(NipcError::Overflow)
                    }
                    Err(_) => Err(NipcError::Truncated),
                };
            }
        }

        #[cfg(windows)]
        {
            if self.shm.is_some() {
                if req_len > max_request_payload_bytes as usize {
                    self.client_note_request_capacity(req_len as u32);
                    return Err(NipcError::Overflow);
                }

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

                let send_result = self.shm.as_mut().unwrap().send(&msg[..msg_len]);
                return match send_result {
                    Ok(()) => Ok(()),
                    Err(crate::transport::win_shm::WinShmError::MsgTooLarge) => {
                        self.client_note_request_capacity(req_len as u32);
                        Err(NipcError::Overflow)
                    }
                    Err(_) => Err(NipcError::Truncated),
                };
            }
        }

        let send_result = {
            let session = self.session.as_mut().ok_or(NipcError::Truncated)?;
            session.send(hdr, &self.request_buf[..req_len])
        };
        match send_result {
            Ok(()) => Ok(()),
            #[cfg(unix)]
            Err(crate::transport::posix::UdsError::LimitExceeded) => {
                self.client_note_request_capacity(req_len as u32);
                Err(NipcError::Overflow)
            }
            #[cfg(windows)]
            Err(crate::transport::windows::NpError::LimitExceeded) => {
                self.client_note_request_capacity(req_len as u32);
                Err(NipcError::Overflow)
            }
            Err(_) => Err(NipcError::Truncated),
        }
    }

    /// Receive via the active transport. Returns (header, payload view).
    pub(super) fn transport_receive(&mut self) -> Result<(Header, ClientResponseRef), NipcError> {
        let needed = self.max_receive_message_bytes();
        let scratch = ensure_client_scratch(&mut self.transport_buf, needed);

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

    pub(super) fn response_payload(&self, response: ClientResponseRef) -> Result<&[u8], NipcError> {
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

    pub(super) fn max_receive_message_bytes(&self) -> usize {
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

impl Drop for RawClient {
    fn drop(&mut self) {
        self.close();
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ClientResponseSource {
    TransportBuf,
    SessionBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct ClientResponseRef {
    pub(super) source: ClientResponseSource,
    pub(super) len: usize,
}
