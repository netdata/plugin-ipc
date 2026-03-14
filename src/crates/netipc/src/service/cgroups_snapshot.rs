//! Cache-backed helper for the fake cgroups snapshot service.

use crate::protocol::{
    decode_cgroups_snapshot_view, encode_cgroups_snapshot_request_payload, encode_message_header,
    max_batch_total_size, message_total_size, CgroupsSnapshotRequest, CgroupsSnapshotView,
    MessageHeader, CGROUPS_SNAPSHOT_ITEM_HEADER_LEN, CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN,
    MESSAGE_FLAG_BATCH, MESSAGE_HEADER_LEN, MESSAGE_KIND_REQUEST, MESSAGE_KIND_RESPONSE,
    MESSAGE_MAGIC, MESSAGE_VERSION, METHOD_CGROUPS_SNAPSHOT, TRANSPORT_STATUS_AUTH_FAILED,
    TRANSPORT_STATUS_BAD_ENVELOPE, TRANSPORT_STATUS_INCOMPATIBLE, TRANSPORT_STATUS_INTERNAL_ERROR,
    TRANSPORT_STATUS_LIMIT_EXCEEDED, TRANSPORT_STATUS_OK, TRANSPORT_STATUS_UNSUPPORTED,
};
use std::io;
use std::path::PathBuf;
use std::time::Duration;

#[cfg(unix)]
use crate::transport::posix::{
    UdsSeqpacketClient as TransportClient, UdsSeqpacketConfig as TransportConfig,
};
#[cfg(windows)]
use crate::transport::windows::{
    NamedPipeClient as TransportClient, NamedPipeConfig as TransportConfig, PROFILE_NAMED_PIPE,
};

pub const CGROUPS_SNAPSHOT_NAME_MAX_BYTES: u32 = 255;
pub const CGROUPS_SNAPSHOT_PATH_MAX_BYTES: u32 = 4096;
pub const CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES: u32 =
    CGROUPS_SNAPSHOT_ITEM_HEADER_LEN as u32
        + CGROUPS_SNAPSHOT_NAME_MAX_BYTES
        + 1
        + CGROUPS_SNAPSHOT_PATH_MAX_BYTES
        + 1;
pub const CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS: u32 = 1000;

#[derive(Clone, Debug)]
pub struct CgroupsSnapshotClientConfig {
    pub service_namespace: PathBuf,
    pub service_name: String,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub auth_token: u64,
}

impl CgroupsSnapshotClientConfig {
    pub fn new(service_namespace: impl Into<PathBuf>, service_name: impl Into<String>) -> Self {
        Self {
            service_namespace: service_namespace.into(),
            service_name: service_name.into(),
            supported_profiles: 0,
            preferred_profiles: 0,
            max_request_payload_bytes: CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32,
            max_request_batch_items: 1,
            max_response_payload_bytes: 0,
            max_response_batch_items: 0,
            auth_token: 0,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CgroupsSnapshotCacheItem {
    pub hash: u32,
    pub options: u32,
    pub enabled: bool,
    pub name: Vec<u8>,
    pub path: Vec<u8>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CgroupsSnapshotCache {
    pub generation: u64,
    pub systemd_enabled: bool,
    pub items: Vec<CgroupsSnapshotCacheItem>,
}

pub struct CgroupsSnapshotClient {
    config: CgroupsSnapshotClientConfig,
    transport: Option<TransportClient>,
    next_message_id: u64,
    response_message: Vec<u8>,
    cache: CgroupsSnapshotCache,
}

impl CgroupsSnapshotClient {
    pub fn new(config: CgroupsSnapshotClientConfig) -> io::Result<Self> {
        if config.service_name.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "service_name must not be empty",
            ));
        }
        if config.supported_profiles == 0
            || config.preferred_profiles == 0
            || config.max_response_payload_bytes == 0
            || config.max_response_batch_items == 0
            || config.auth_token == 0
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "auth_token, supported_profiles, preferred_profiles, max_response_payload_bytes, and max_response_batch_items must be set explicitly",
            ));
        }

        let response_capacity = max_batch_total_size(
            effective_response_payload_limit(config.max_response_payload_bytes),
            effective_response_batch_limit(config.max_response_batch_items),
        )?;

        Ok(Self {
            config,
            transport: None,
            next_message_id: 1,
            response_message: vec![0u8; response_capacity],
            cache: CgroupsSnapshotCache::default(),
        })
    }

    pub fn refresh(&mut self, timeout: Option<Duration>) -> io::Result<()> {
        self.ensure_transport(timeout)?;

        let message_id = self.next_message_id;
        self.next_message_id += 1;

        let request_payload =
            encode_cgroups_snapshot_request_payload(&CgroupsSnapshotRequest { flags: 0 });
        let request_header = MessageHeader {
            magic: MESSAGE_MAGIC,
            version: MESSAGE_VERSION,
            header_len: MESSAGE_HEADER_LEN as u16,
            kind: MESSAGE_KIND_REQUEST,
            flags: 0,
            code: METHOD_CGROUPS_SNAPSHOT,
            transport_status: TRANSPORT_STATUS_OK,
            payload_len: CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32,
            item_count: 1,
            message_id,
        };
        let encoded_header = encode_message_header(&request_header)?;
        let mut request_message = [0u8; MESSAGE_HEADER_LEN + CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN];
        request_message[..MESSAGE_HEADER_LEN].copy_from_slice(&encoded_header);
        request_message[MESSAGE_HEADER_LEN..].copy_from_slice(&request_payload);

        let response_len = match self
            .transport
            .as_mut()
            .expect("transport must exist after ensure_transport")
            .call_message(&request_message, &mut self.response_message, timeout)
        {
            Ok(len) => len,
            Err(err) => {
                self.disconnect_transport();
                return Err(err);
            }
        };

        let header =
            match crate::protocol::decode_message_header(&self.response_message[..response_len]) {
                Ok(header) => header,
                Err(err) => {
                    self.disconnect_transport();
                    return Err(err);
                }
            };

        if header.kind != MESSAGE_KIND_RESPONSE
            || header.code != METHOD_CGROUPS_SNAPSHOT
            || header.message_id != message_id
            || header.flags != MESSAGE_FLAG_BATCH
        {
            self.disconnect_transport();
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid cgroups snapshot response envelope",
            ));
        }

        if header.transport_status != TRANSPORT_STATUS_OK {
            self.disconnect_transport();
            return Err(io::Error::from_raw_os_error(transport_status_errno(
                header.transport_status,
            )));
        }

        let total = message_total_size(&header)?;
        if total != response_len {
            self.disconnect_transport();
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "response length does not match envelope",
            ));
        }

        let view = match decode_cgroups_snapshot_view(
            &self.response_message[MESSAGE_HEADER_LEN..response_len],
            header.item_count,
        ) {
            Ok(view) => view,
            Err(err) => {
                self.disconnect_transport();
                return Err(err);
            }
        };

        self.cache = materialize_cache(view)?;
        Ok(())
    }

    pub fn cache(&self) -> &CgroupsSnapshotCache {
        &self.cache
    }

    pub fn lookup(&self, hash: u32, name: &[u8]) -> Option<&CgroupsSnapshotCacheItem> {
        self.cache
            .items
            .iter()
            .find(|item| item.hash == hash && item.name.as_slice() == name)
    }

    fn ensure_transport(&mut self, timeout: Option<Duration>) -> io::Result<()> {
        if self.transport.is_some() {
            return Ok(());
        }

        let transport = connect_transport(&self.config, timeout)?;
        self.transport = Some(transport);
        Ok(())
    }

    fn disconnect_transport(&mut self) {
        self.transport = None;
    }
}

fn materialize_cache(view: CgroupsSnapshotView<'_>) -> io::Result<CgroupsSnapshotCache> {
    let mut items = Vec::with_capacity(view.item_count as usize);
    for index in 0..view.item_count {
        let item = view.item_view_at(index)?;
        items.push(CgroupsSnapshotCacheItem {
            hash: item.hash,
            options: item.options,
            enabled: item.enabled,
            name: item.name_view.as_bytes().to_vec(),
            path: item.path_view.as_bytes().to_vec(),
        });
    }

    Ok(CgroupsSnapshotCache {
        generation: view.generation,
        systemd_enabled: view.systemd_enabled,
        items,
    })
}

fn effective_response_payload_limit(configured: u32) -> u32 {
    if configured != 0 {
        configured
    } else {
        CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES
    }
}

fn effective_response_batch_limit(configured: u32) -> u32 {
    if configured != 0 {
        configured
    } else {
        CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS
    }
}

fn transport_status_errno(status: u16) -> i32 {
    match status {
        TRANSPORT_STATUS_OK => 0,
        TRANSPORT_STATUS_AUTH_FAILED => libc::EACCES,
        TRANSPORT_STATUS_LIMIT_EXCEEDED => libc::EMSGSIZE,
        TRANSPORT_STATUS_UNSUPPORTED => libc::ENOTSUP,
        TRANSPORT_STATUS_INTERNAL_ERROR => libc::EIO,
        TRANSPORT_STATUS_BAD_ENVELOPE | TRANSPORT_STATUS_INCOMPATIBLE | _ => libc::EPROTO,
    }
}

#[cfg(unix)]
fn connect_transport(
    config: &CgroupsSnapshotClientConfig,
    timeout: Option<Duration>,
) -> io::Result<TransportClient> {
    let mut transport = TransportConfig::new(
        config.service_namespace.clone(),
        config.service_name.clone(),
    );
    transport.supported_profiles = config.supported_profiles;
    transport.preferred_profiles = config.preferred_profiles;
    transport.max_request_payload_bytes = config.max_request_payload_bytes;
    transport.max_request_batch_items = config.max_request_batch_items;
    transport.max_response_payload_bytes = config.max_response_payload_bytes;
    transport.max_response_batch_items = config.max_response_batch_items;
    transport.auth_token = config.auth_token;
    TransportClient::connect(&transport, timeout)
}

#[cfg(windows)]
fn connect_transport(
    config: &CgroupsSnapshotClientConfig,
    timeout: Option<Duration>,
) -> io::Result<TransportClient> {
    let mut transport = TransportConfig::new(
        config.service_namespace.clone(),
        config.service_name.clone(),
    );
    transport.supported_profiles = config.supported_profiles;
    transport.preferred_profiles = config.preferred_profiles;
    transport.max_request_payload_bytes = config.max_request_payload_bytes;
    transport.max_request_batch_items = config.max_request_batch_items;
    transport.max_response_payload_bytes = config.max_response_payload_bytes;
    transport.max_response_batch_items = config.max_response_batch_items;
    transport.auth_token = config.auth_token;
    TransportClient::connect(&transport, timeout)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::protocol::{
        decode_cgroups_snapshot_request_view, encode_message_header, CgroupsSnapshotItem,
        CgroupsSnapshotResponseBuilder, MessageHeader, CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN,
    };
    use crate::protocol::{max_batch_payload_len, max_batch_total_size};
    use crate::transport::posix::{
        UdsSeqpacketConfig, UdsSeqpacketServer, PROFILE_SHM_HYBRID, PROFILE_UDS_SEQPACKET,
    };
    use std::fs;
    use std::sync::mpsc;
    use std::thread;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    fn test_service_namespace(name: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        path.push(format!("netipc-{name}-{unique}-{}", std::process::id()));
        fs::create_dir_all(&path).unwrap();
        path
    }

    fn fixed_cgroups_items() -> Vec<CgroupsSnapshotItem<'static>> {
        vec![
            CgroupsSnapshotItem {
                hash: 123,
                options: 0x2,
                enabled: true,
                name: b"system.slice-nginx",
                path: b"/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 456,
                options: 0x4,
                enabled: false,
                name: b"docker-1234",
                path: b"",
            },
            CgroupsSnapshotItem {
                hash: 789,
                options: 0x6,
                enabled: true,
                name: b"kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice",
                path: b"/sys/fs/cgroup/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1001,
                options: 0x2,
                enabled: true,
                name: b"system.slice-sshd.service",
                path: b"/sys/fs/cgroup/system.slice/sshd.service/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1002,
                options: 0x2,
                enabled: true,
                name: b"system.slice-docker.service",
                path: b"/sys/fs/cgroup/system.slice/docker.service/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1003,
                options: 0x6,
                enabled: true,
                name: b"user.slice-user-1000.slice-session-3.scope",
                path: b"/sys/fs/cgroup/user.slice/user-1000.slice/session-3.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1004,
                options: 0x2,
                enabled: true,
                name: b"machine.slice-libvirt-qemu-5-win11.scope",
                path: b"/sys/fs/cgroup/machine.slice/libvirt-qemu-5-win11.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1005,
                options: 0x8,
                enabled: false,
                name: b"system.slice-telegraf.service",
                path: b"/sys/fs/cgroup/system.slice/telegraf.service/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1006,
                options: 0x6,
                enabled: true,
                name: b"podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope",
                path: b"/sys/fs/cgroup/system.slice/podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1007,
                options: 0x4,
                enabled: true,
                name: b"init.scope",
                path: b"/sys/fs/cgroup/init.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1008,
                options: 0x6,
                enabled: true,
                name: b"system.slice-containerd.service",
                path: b"/sys/fs/cgroup/system.slice/containerd.service/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1009,
                options: 0x4,
                enabled: true,
                name: b"machine.slice-systemd-nspawn-observability-lab.scope",
                path: b"/sys/fs/cgroup/machine.slice/systemd-nspawn-observability-lab.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1010,
                options: 0x6,
                enabled: true,
                name: b"user.slice-user-1001.slice-user@1001.service-app.slice-observability-frontend.scope",
                path: b"/sys/fs/cgroup/user.slice/user-1001.slice/user@1001.service/app.slice/observability-frontend.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1011,
                options: 0x1,
                enabled: false,
                name: b"crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope",
                path: b"/sys/fs/cgroup/kubepods.slice/kubepods-pod98765432_10fe_dcba_9876_543210fedcba.slice/crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1012,
                options: 0x2,
                enabled: true,
                name: b"system.slice-netdata.service",
                path: b"/sys/fs/cgroup/system.slice/netdata.service/cgroup.procs",
            },
            CgroupsSnapshotItem {
                hash: 1013,
                options: 0x6,
                enabled: true,
                name: b"system.slice-super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service",
                path: b"/sys/fs/cgroup/system.slice/super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service/cgroup.procs",
            },
        ]
    }

    fn fixed_response(message_id: u64) -> Vec<u8> {
        let items = fixed_cgroups_items();
        let payload_cap = max_batch_payload_len(
            CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES,
            items.len() as u32,
        )
        .unwrap();
        let mut payload = vec![0u8; payload_cap];
        let mut builder =
            CgroupsSnapshotResponseBuilder::new(&mut payload, 42, true, 3, items.len() as u32)
                .unwrap();
        for item in &items {
            builder.push_item(item).unwrap();
        }
        let payload_len = builder.finish().unwrap();
        let header = MessageHeader {
            magic: MESSAGE_MAGIC,
            version: MESSAGE_VERSION,
            header_len: MESSAGE_HEADER_LEN as u16,
            kind: MESSAGE_KIND_RESPONSE,
            flags: MESSAGE_FLAG_BATCH,
            code: METHOD_CGROUPS_SNAPSHOT,
            transport_status: TRANSPORT_STATUS_OK,
            payload_len: payload_len as u32,
            item_count: items.len() as u32,
            message_id,
        };
        let encoded = encode_message_header(&header).unwrap();
        let mut message = Vec::with_capacity(MESSAGE_HEADER_LEN + payload_len);
        message.extend_from_slice(&encoded);
        message.extend_from_slice(&payload[..payload_len]);
        message
    }

    fn malformed_but_transport_valid_response(message_id: u64) -> Vec<u8> {
        let mut message = fixed_response(message_id);
        let first_item_ref_offset = MESSAGE_HEADER_LEN + CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN;
        message[first_item_ref_offset..first_item_ref_offset + 4]
            .copy_from_slice(&1u32.to_le_bytes());
        message
    }

    fn spawn_snapshot_server(
        service_namespace: PathBuf,
        service_name: String,
    ) -> (mpsc::Receiver<()>, thread::JoinHandle<io::Result<()>>) {
        spawn_snapshot_server_with_profiles(
            service_namespace,
            service_name,
            PROFILE_UDS_SEQPACKET,
            PROFILE_UDS_SEQPACKET,
        )
    }

    fn spawn_snapshot_server_with_profiles(
        service_namespace: PathBuf,
        service_name: String,
        supported_profiles: u32,
        preferred_profiles: u32,
    ) -> (mpsc::Receiver<()>, thread::JoinHandle<io::Result<()>>) {
        let (ready_tx, ready_rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let mut config = UdsSeqpacketConfig::new(service_namespace.clone(), service_name);
            config.supported_profiles = supported_profiles;
            config.preferred_profiles = preferred_profiles;
            config.max_request_payload_bytes = CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32;
            config.max_request_batch_items = 1;
            config.max_response_payload_bytes = CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
            config.max_response_batch_items = CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;

            let mut server = UdsSeqpacketServer::bind(&config)?;
            ready_tx.send(()).unwrap();
            server.accept(Some(Duration::from_secs(5)))?;

            let request_capacity =
                max_batch_total_size(CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32, 1)?;
            let mut request = vec![0u8; request_capacity];
            let request_len = server.receive_message(&mut request, Some(Duration::from_secs(5)))?;
            let header = crate::protocol::decode_message_header(&request[..request_len])?;
            assert_eq!(header.kind, MESSAGE_KIND_REQUEST);
            assert_eq!(header.code, METHOD_CGROUPS_SNAPSHOT);
            assert_eq!(header.item_count, 1);
            decode_cgroups_snapshot_request_view(&request[MESSAGE_HEADER_LEN..request_len])?;

            let response = fixed_response(header.message_id);
            server.send_message(&response, Some(Duration::from_secs(5)))
        });
        (ready_rx, handle)
    }

    fn spawn_snapshot_response_sequence_server(
        service_namespace: PathBuf,
        service_name: String,
        supported_profiles: u32,
        preferred_profiles: u32,
        responses: Vec<Vec<u8>>,
    ) -> (mpsc::Receiver<()>, thread::JoinHandle<io::Result<()>>) {
        let (ready_tx, ready_rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let mut config = UdsSeqpacketConfig::new(service_namespace.clone(), service_name);
            config.supported_profiles = supported_profiles;
            config.preferred_profiles = preferred_profiles;
            config.max_request_payload_bytes = CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32;
            config.max_request_batch_items = 1;
            config.max_response_payload_bytes = CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
            config.max_response_batch_items = CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;

            let mut server = UdsSeqpacketServer::bind(&config)?;
            ready_tx.send(()).unwrap();
            server.accept(Some(Duration::from_secs(5)))?;

            let request_capacity =
                max_batch_total_size(CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32, 1)?;
            let mut request = vec![0u8; request_capacity];

            for response in responses {
                let request_len =
                    server.receive_message(&mut request, Some(Duration::from_secs(5)))?;
                let header = crate::protocol::decode_message_header(&request[..request_len])?;
                assert_eq!(header.kind, MESSAGE_KIND_REQUEST);
                assert_eq!(header.code, METHOD_CGROUPS_SNAPSHOT);
                assert_eq!(header.item_count, 1);
                decode_cgroups_snapshot_request_view(&request[MESSAGE_HEADER_LEN..request_len])?;
                server.send_message(&response, Some(Duration::from_secs(5)))?;
            }

            Ok(())
        });
        (ready_rx, handle)
    }

    fn strict_client_config(
        service_namespace: PathBuf,
        service_name: String,
    ) -> CgroupsSnapshotClientConfig {
        strict_client_config_with_profiles(
            service_namespace,
            service_name,
            PROFILE_UDS_SEQPACKET,
            PROFILE_UDS_SEQPACKET,
        )
    }

    fn strict_client_config_with_profiles(
        service_namespace: PathBuf,
        service_name: String,
        supported_profiles: u32,
        preferred_profiles: u32,
    ) -> CgroupsSnapshotClientConfig {
        let mut config = CgroupsSnapshotClientConfig::new(service_namespace, service_name);
        #[cfg(unix)]
        {
            config.supported_profiles = supported_profiles;
            config.preferred_profiles = preferred_profiles;
        }
        #[cfg(windows)]
        {
            config.supported_profiles = PROFILE_NAMED_PIPE;
            config.preferred_profiles = PROFILE_NAMED_PIPE;
        }
        config.max_response_payload_bytes = CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
        config.max_response_batch_items = CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
        config.auth_token = 1;
        config
    }

    #[test]
    fn refresh_populates_cache_and_lookup() {
        let service_namespace = test_service_namespace("cgroups-cache");
        let service_name = "cgroups-snapshot".to_string();
        let (ready_rx, handle) =
            spawn_snapshot_server(service_namespace.clone(), service_name.clone());
        ready_rx.recv().unwrap();

        let mut client = CgroupsSnapshotClient::new(strict_client_config(
            service_namespace.clone(),
            service_name,
        ))
        .unwrap();
        client.refresh(Some(Duration::from_secs(5))).unwrap();

        let cache = client.cache();
        assert_eq!(cache.generation, 42);
        assert!(cache.systemd_enabled);
        assert_eq!(cache.items.len(), 16);

        let item = client.lookup(123, b"system.slice-nginx").unwrap();
        assert_eq!(
            item.path,
            b"/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs"
        );
        assert!(item.enabled);

        handle.join().unwrap().unwrap();
        fs::remove_dir_all(service_namespace).unwrap();
    }

    #[test]
    fn refresh_failure_keeps_previous_cache() {
        let service_namespace = test_service_namespace("cgroups-cache-sticky");
        let service_name = "cgroups-snapshot".to_string();
        let (ready_rx, handle) =
            spawn_snapshot_server(service_namespace.clone(), service_name.clone());
        ready_rx.recv().unwrap();

        let mut client = CgroupsSnapshotClient::new(strict_client_config(
            service_namespace.clone(),
            service_name,
        ))
        .unwrap();
        client.refresh(Some(Duration::from_secs(5))).unwrap();
        handle.join().unwrap().unwrap();

        let err = client
            .refresh(Some(Duration::from_millis(200)))
            .unwrap_err();
        assert!(
            matches!(
                err.raw_os_error(),
                Some(libc::ECONNREFUSED)
                    | Some(libc::ENOENT)
                    | Some(libc::ECONNRESET)
                    | Some(libc::ENOTCONN)
                    | Some(libc::EPIPE)
                    | Some(libc::EPROTO)
            ) || matches!(
                err.kind(),
                io::ErrorKind::BrokenPipe | io::ErrorKind::UnexpectedEof | io::ErrorKind::TimedOut
            ),
            "unexpected refresh error: {err:?}"
        );

        let cache = client.cache();
        assert_eq!(cache.generation, 42);
        assert_eq!(cache.items.len(), 16);
        assert!(client.lookup(456, b"docker-1234").is_some());

        fs::remove_dir_all(service_namespace).unwrap();
    }

    #[test]
    fn refresh_failure_keeps_previous_cache_over_shm() {
        let service_namespace = test_service_namespace("cgroups-cache-sticky-shm");
        let service_name = "cgroups-snapshot".to_string();
        let supported = PROFILE_UDS_SEQPACKET | PROFILE_SHM_HYBRID;
        let preferred = PROFILE_SHM_HYBRID;
        let (ready_rx, handle) = spawn_snapshot_server_with_profiles(
            service_namespace.clone(),
            service_name.clone(),
            supported,
            preferred,
        );
        ready_rx.recv().unwrap();

        let mut client = CgroupsSnapshotClient::new(strict_client_config_with_profiles(
            service_namespace.clone(),
            service_name,
            supported,
            preferred,
        ))
        .unwrap();
        client.refresh(Some(Duration::from_secs(5))).unwrap();
        handle.join().unwrap().unwrap();

        let err = client
            .refresh(Some(Duration::from_millis(200)))
            .unwrap_err();
        assert!(
            matches!(
                err.raw_os_error(),
                Some(libc::ECONNREFUSED)
                    | Some(libc::ENOENT)
                    | Some(libc::ECONNRESET)
                    | Some(libc::ENOTCONN)
                    | Some(libc::EPIPE)
                    | Some(libc::EPROTO)
            ) || matches!(
                err.kind(),
                io::ErrorKind::BrokenPipe | io::ErrorKind::UnexpectedEof | io::ErrorKind::TimedOut
            ),
            "unexpected refresh error: {err:?}"
        );

        let cache = client.cache();
        assert_eq!(cache.generation, 42);
        assert_eq!(cache.items.len(), 16);
        assert!(client.lookup(456, b"docker-1234").is_some());

        fs::remove_dir_all(service_namespace).unwrap();
    }

    #[test]
    fn malformed_response_keeps_previous_cache_over_shm() {
        let service_namespace = test_service_namespace("cgroups-cache-malformed-shm");
        let service_name = "cgroups-snapshot".to_string();
        let supported = PROFILE_UDS_SEQPACKET | PROFILE_SHM_HYBRID;
        let preferred = PROFILE_SHM_HYBRID;
        let (ready_rx, handle) = spawn_snapshot_response_sequence_server(
            service_namespace.clone(),
            service_name.clone(),
            supported,
            preferred,
            vec![fixed_response(1), malformed_but_transport_valid_response(2)],
        );
        ready_rx.recv().unwrap();

        let mut client = CgroupsSnapshotClient::new(strict_client_config_with_profiles(
            service_namespace.clone(),
            service_name,
            supported,
            preferred,
        ))
        .unwrap();
        client.refresh(Some(Duration::from_secs(5))).unwrap();
        let err = client.refresh(Some(Duration::from_secs(5))).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
        handle.join().unwrap().unwrap();

        let cache = client.cache();
        assert_eq!(cache.generation, 42);
        assert_eq!(cache.items.len(), 16);
        assert!(client.lookup(123, b"system.slice-nginx").is_some());

        fs::remove_dir_all(service_namespace).unwrap();
    }

    #[test]
    fn new_rejects_implicit_insecure_defaults() {
        let err = match CgroupsSnapshotClient::new(CgroupsSnapshotClientConfig::new(
            test_service_namespace("cgroups-config-invalid"),
            "cgroups-snapshot".to_string(),
        )) {
            Ok(_) => panic!("expected client creation to fail"),
            Err(err) => err,
        };
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }
}
