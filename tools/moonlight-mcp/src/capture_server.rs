//! Bounded local capture transport and exclusive device ownership.

use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::future::Future;
use std::net::SocketAddr;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Semaphore;
use tokio::task::JoinSet;

use crate::clock;
use crate::reference::{CaptureDevice, RecordingIndex};

const MAX_HEADER: usize = 8192;
const MAX_RESPONSE: usize = 4 * 1024 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_CONNECTIONS: usize = 8;

pub fn validate_loopback(address: SocketAddr) -> Result<()> {
    ensure!(
        address.ip().is_loopback(),
        "capture endpoints must be loopback-only"
    );
    Ok(())
}

#[derive(Debug)]
enum Request {
    Health,
    Record { seconds: f64, name: String },
}
impl Request {
    fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(
            bytes.len() <= MAX_HEADER && bytes.ends_with(b"\r\n\r\n"),
            "incomplete/oversize request"
        );
        let text = std::str::from_utf8(bytes)?;
        let mut lines = text.split("\r\n");
        let fields: Vec<_> = lines.next().unwrap_or_default().split(' ').collect();
        ensure!(
            fields.len() == 3 && fields[0] == "GET" && matches!(fields[2], "HTTP/1.0" | "HTTP/1.1"),
            "unsupported request line"
        );
        let mut authorized = false;
        let mut headers = std::collections::BTreeSet::new();
        for line in lines.filter(|line| !line.is_empty()) {
            let (key, value) = line.split_once(':').context("malformed header")?;
            let key = key.to_ascii_lowercase();
            ensure!(headers.insert(key.clone()), "duplicate header");
            let value = value.trim();
            match key.as_str() {
                "origin" | "transfer-encoding" => bail!("cross-origin/body requests forbidden"),
                "content-length" => ensure!(value == "0", "request bodies forbidden"),
                "sec-fetch-site" => ensure!(
                    matches!(value, "none" | "same-origin"),
                    "cross-site request forbidden"
                ),
                "x-moonlight-capture" => authorized = value == "1",
                _ => (),
            }
        }
        if fields[1] == "/health" {
            return Ok(Self::Health);
        }
        ensure!(authorized, "local capture request header required");
        let query = fields[1]
            .strip_prefix("/record?")
            .context("unknown capture route")?;
        let (mut seconds, mut name) = (None, None);
        for item in query.split('&') {
            let (key, value) = item.split_once('=').context("invalid query")?;
            match key {
                "seconds" if seconds.is_none() => seconds = Some(value.parse::<f64>()?),
                "dir" if name.is_none() => name = Some(value.to_owned()),
                _ => bail!("duplicate/unknown query parameter"),
            }
        }
        let seconds = seconds.context("missing seconds")?;
        validate_seconds(seconds)?;
        let name = name.context("missing dir")?;
        validate_name(&name)?;
        Ok(Self::Record { seconds, name })
    }
}

pub fn validate_seconds(seconds: f64) -> Result<()> {
    ensure!(
        seconds.is_finite() && (0.5..=600.0).contains(&seconds),
        "capture duration must be 0.5..600 seconds"
    );
    Ok(())
}
fn validate_name(name: &str) -> Result<()> {
    ensure!(
        !name.is_empty()
            && name.len() <= 96
            && name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_'),
        "output must be a single bounded name"
    );
    Ok(())
}

/// Reject symlink ancestors before opening local artifacts. Roots must be user-controlled.
pub fn confined_root(root: &Path) -> Result<PathBuf> {
    let absolute = std::path::absolute(root)?;
    let mut ancestor = PathBuf::new();
    for component in absolute.components() {
        ensure!(
            !matches!(component, std::path::Component::ParentDir),
            "parent traversal forbidden"
        );
        ancestor.push(component);
        if let Ok(metadata) = std::fs::symlink_metadata(&ancestor) {
            ensure!(
                !metadata.file_type().is_symlink(),
                "output symlink forbidden"
            );
        }
    }
    std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(&absolute)?;
    Ok(absolute.canonicalize()?)
}

/// An incomplete/cancelled request removes only the freshly reserved directory it owns.
pub struct OutputReservation {
    pub path: PathBuf,
    committed: bool,
}
impl OutputReservation {
    pub fn create(root: &Path, name: &str) -> Result<Self> {
        validate_name(name)?;
        let root = confined_root(root)?;
        let path = root.join(name);
        std::fs::DirBuilder::new()
            .mode(0o700)
            .create(&path)
            .context("output already exists or cannot be reserved")?;
        Ok(Self {
            path,
            committed: false,
        })
    }
    pub fn commit(mut self) -> PathBuf {
        self.committed = true;
        self.path.clone()
    }
}
impl Drop for OutputReservation {
    fn drop(&mut self) {
        if !self.committed {
            if let Err(error) = std::fs::remove_dir_all(&self.path) {
                eprintln!("capture cleanup {}: {error}", self.path.display());
            }
        }
    }
}

/// A cooperative cross-process lease, held until capture and child cleanup finish.
/// A crashed process leaves the lock for explicit operator inspection, never blind takeover.
pub struct DeviceLease {
    path: PathBuf,
}
impl DeviceLease {
    pub fn acquire(identity: &str) -> Result<Self> {
        ensure!(
            !identity.is_empty() && identity.len() <= 256,
            "invalid device identity"
        );
        let root = confined_root(&std::env::temp_dir().join("moonlight-capture-device-locks"))?;
        let path = root.join(format!(
            "{}.lock",
            hex::encode(Sha256::digest(identity.as_bytes()))
        ));
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
            .context(
                "capture device is owned by another operation; inspect stale locks explicitly",
            )?;
        use std::io::Write;
        if let Err(error) = writeln!(file, "pid={} device={identity}", std::process::id()) {
            std::fs::remove_file(&path)?;
            return Err(error.into());
        }
        Ok(Self { path })
    }
}
impl Drop for DeviceLease {
    fn drop(&mut self) {
        if let Err(error) = std::fs::remove_file(&self.path) {
            eprintln!("device lease cleanup: {error}");
        }
    }
}

pub type CaptureFuture<'a> = Pin<Box<dyn Future<Output = Result<RecordingIndex>> + Send + 'a>>;
pub trait CaptureBackend: Send + Sync + 'static {
    fn identity(&self) -> &str;
    /// Async helper cleanup may outlive the request; such backends retain their own lease.
    fn manages_lease(&self) -> bool {
        false
    }
    /// Upper bound including native frames and metadata, checked before starting capture.
    fn reservation_bytes(&self, seconds: f64) -> Result<u64> {
        validate_seconds(seconds)?;
        Ok(((seconds * 60.0).ceil() as u64 + 2) * (1920 * 1080 * 3 / 2) + 8 * 1024 * 1024)
    }
    /// Writes to a fresh private directory; dropping the future must release child resources.
    fn record<'a>(&'a self, seconds: f64, output: &'a Path) -> CaptureFuture<'a>;
}
impl CaptureBackend for CaptureDevice {
    fn identity(&self) -> &str {
        &self.name
    }
    fn record<'a>(&'a self, seconds: f64, output: &'a Path) -> CaptureFuture<'a> {
        Box::pin(crate::reference::record_device(self, seconds, output))
    }
}

#[derive(Debug, Serialize, Deserialize)]
pub struct ServerRecording {
    pub dir: PathBuf,
    pub index: RecordingIndex,
    pub clock: crate::evidence::Clock,
}

pub async fn serve(listen: SocketAddr, output_root: PathBuf, device: CaptureDevice) -> Result<()> {
    validate_loopback(listen)?;
    let root = confined_root(&output_root)?;
    let listener = TcpListener::bind(listen).await?;
    serve_with(listener, root, Arc::new(device), async {
        let _ = tokio::signal::ctrl_c().await;
    })
    .await
}

/// Caller-owned shutdown is also used by deterministic loopback fixtures.
pub async fn serve_with<B: CaptureBackend>(
    listener: TcpListener,
    output_root: PathBuf,
    backend: Arc<B>,
    shutdown: impl Future<Output = ()>,
) -> Result<()> {
    validate_loopback(listener.local_addr()?)?;
    let root = confined_root(&output_root)?;
    let slots = Arc::new(Semaphore::new(MAX_CONNECTIONS));
    let mut handlers = JoinSet::new();
    tokio::pin!(shutdown);
    loop {
        tokio::select! {
            _ = &mut shutdown => break,
            result = handlers.join_next(), if !handlers.is_empty() => {
                if let Some(Err(error)) = result { eprintln!("capture connection task: {error}"); }
            }
            accepted = listener.accept() => {
                let (stream, peer) = accepted?;
                if !peer.ip().is_loopback() { continue; }
                let Ok(slot) = slots.clone().try_acquire_owned() else { drop(stream); continue; };
                let (root, backend) = (root.clone(), backend.clone());
                handlers.spawn(async move {
                    let _slot = slot;
                    if let Err(error) = handle(stream, &root, &*backend).await { eprintln!("capture request: {error:#}"); }
                });
            }
        }
    }
    handlers.abort_all();
    while handlers.join_next().await.is_some() {}
    Ok(())
}

async fn read_request(stream: &mut TcpStream) -> Result<Request> {
    let mut bytes = Vec::with_capacity(512);
    loop {
        let byte = stream.read_u8().await?;
        bytes.push(byte);
        ensure!(bytes.len() <= MAX_HEADER, "request too large");
        if bytes.ends_with(b"\r\n\r\n") {
            return Request::parse(&bytes);
        }
    }
}
fn check_retained_budget(root: &Path, reservation: u64) -> Result<()> {
    // Bound completed/unclaimed artifacts too; refuse rather than delete retained evidence.
    let mut bytes = 0u64;
    for (count, entry) in std::fs::read_dir(root)?.enumerate() {
        ensure!(count < 64, "capture retention directory limit reached");
        let entry = entry?;
        ensure!(entry.file_type()?.is_dir(), "unexpected capture-root entry");
        for (files, artifact) in std::fs::read_dir(entry.path())?.enumerate() {
            ensure!(files < 8, "capture artifact count limit reached");
            let artifact = artifact?;
            ensure!(
                artifact.file_type()?.is_file(),
                "unexpected capture artifact type"
            );
            bytes = bytes
                .checked_add(artifact.metadata()?.len())
                .context("capture retention size overflow")?;
        }
    }
    ensure!(
        bytes
            .checked_add(reservation)
            .is_some_and(|v| v <= 128 * 1024 * 1024 * 1024),
        "capture retention disk budget exhausted"
    );
    Ok(())
}

async fn handle<B: CaptureBackend>(mut stream: TcpStream, root: &Path, backend: &B) -> Result<()> {
    let request = tokio::time::timeout(REQUEST_TIMEOUT, read_request(&mut stream)).await?;
    let request = match request {
        Ok(request) => request,
        Err(error) => {
            respond(&mut stream, 400, error.to_string().as_bytes()).await?;
            return Ok(());
        }
    };
    match request {
        Request::Health => respond(&mut stream, 200, b"ok").await?,
        Request::Record { seconds, name } => {
            let operation = async {
                let bytes = backend.reservation_bytes(seconds)?;
                ensure!(bytes > 0, "empty capture reservation");
                check_retained_budget(root, bytes)?;
                let _lease = if backend.manages_lease() {
                    None
                } else {
                    Some(DeviceLease::acquire(backend.identity())?)
                };
                let reservation = OutputReservation::create(root, &name)?;
                let index = tokio::time::timeout(
                    Duration::from_secs_f64(seconds + 15.0),
                    backend.record(seconds, &reservation.path),
                )
                .await??;
                let verified = crate::reference::Recording::load(&reservation.path)?;
                ensure!(
                    verified.index == index,
                    "backend result differs from recorded sidecars"
                );
                let clock = clock::local_clock()?;
                let result = ServerRecording {
                    dir: reservation.path.clone(),
                    index,
                    clock,
                };
                let body = serde_json::to_vec(&result)?;
                ensure!(
                    body.len() <= MAX_RESPONSE,
                    "capture response exceeded limit"
                );
                Ok::<_, anyhow::Error>((reservation, body))
            };
            // Any extra request data or EOF cancels capture and drops its fresh output/lease.
            let mut unexpected = [0; 1];
            let result = tokio::select! {
                result = operation => result,
                _ = stream.read(&mut unexpected) => return Ok(()),
            };
            match result {
                Ok((reservation, body)) => {
                    tokio::time::timeout(REQUEST_TIMEOUT, respond(&mut stream, 200, &body))
                        .await??;
                    reservation.commit();
                }
                Err(error) => respond(&mut stream, 409, error.to_string().as_bytes()).await?,
            }
        }
    }
    Ok(())
}
async fn respond(stream: &mut TcpStream, status: u16, body: &[u8]) -> Result<()> {
    ensure!(body.len() <= MAX_RESPONSE, "response too large");
    stream
        .write_all(
            format!(
                "HTTP/1.0 {status} Capture\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            )
            .as_bytes(),
        )
        .await?;
    stream.write_all(body).await?;
    stream.shutdown().await?;
    Ok(())
}

/// In-process recording shares service admission, ownership and rollback semantics.
pub async fn record_local<B: CaptureBackend>(
    backend: &B,
    seconds: f64,
    output: &Path,
) -> Result<RecordingIndex> {
    validate_seconds(seconds)?;
    let parent = output
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let name = output
        .file_name()
        .and_then(|name| name.to_str())
        .context("output directory name required")?;
    let bytes = backend.reservation_bytes(seconds)?;
    ensure!(
        bytes > 0 && bytes <= 128 * 1024 * 1024 * 1024,
        "capture disk budget exceeded"
    );
    let _lease = if backend.manages_lease() {
        None
    } else {
        Some(DeviceLease::acquire(backend.identity())?)
    };
    let reservation = OutputReservation::create(parent, name)?;
    let index = tokio::time::timeout(
        Duration::from_secs_f64(seconds + 15.0),
        backend.record(seconds, &reservation.path),
    )
    .await??;
    crate::reference::Recording::load(&reservation.path)?;
    reservation.commit();
    Ok(index)
}

pub fn server_output_root() -> PathBuf {
    std::env::temp_dir().join("sunshineps5-capture-server")
}

/// Local artifact movement only. Redirects/proxies and non-loopback destinations are refused.
pub async fn record_via_server(
    addr: SocketAddr,
    seconds: f64,
    out_dir: &Path,
) -> Result<RecordingIndex> {
    validate_loopback(addr)?;
    validate_seconds(seconds)?;
    let parent = out_dir
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let name = out_dir
        .file_name()
        .and_then(|v| v.to_str())
        .context("output name required")?;
    // Reserve before requesting so an invalid destination cannot leave an orphan capture.
    let output = OutputReservation::create(parent, name)?;
    let name = format!("rec-{}", uuid::Uuid::new_v4().simple());
    let mut response = reqwest::Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .timeout(Duration::from_secs_f64(seconds + 30.0))
        .build()?
        .get(format!("http://{addr}/record?seconds={seconds}&dir={name}"))
        .header("X-Moonlight-Capture", "1")
        .send()
        .await?;
    ensure!(
        response.status().is_success(),
        "capture service returned {}",
        response.status()
    );
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await? {
        ensure!(
            bytes.len() + chunk.len() <= MAX_RESPONSE,
            "capture response too large"
        );
        bytes.extend_from_slice(&chunk);
    }
    let result: ServerRecording = serde_json::from_slice(&bytes)?;
    ensure!(
        result.clock == clock::local_clock()?,
        "capture service clock does not match this machine boot"
    );
    ensure!(
        result.dir.is_absolute() && result.dir.file_name().and_then(|v| v.to_str()) == Some(&name),
        "unconfined server artifact directory"
    );
    ensure!(
        std::fs::symlink_metadata(&result.dir)?.is_dir(),
        "server artifact directory missing"
    );
    let source = confined_root(&result.dir)?;
    let mut copied = Vec::new();
    let native = crate::reference::Recording::load(&source)?;
    let files = if native.metadata.pixel_format == crate::recording::PixelFormat::P010le {
        ensure!(
            native
                .metadata
                .frames
                .as_ref()
                .is_some_and(|path| path.0.as_path() == Path::new("frames.p010")),
            "HDR artifact name outside transfer allowlist"
        );
        [
            "frames.p010",
            "frames.json",
            "recording.json",
            "capture-summary.json",
        ]
    } else {
        ensure!(
            native
                .metadata
                .frames
                .as_ref()
                .is_some_and(|path| path.0.as_path() == Path::new("frames.yuv")),
            "SDR artifact name outside transfer allowlist"
        );
        [
            "frames.yuv",
            "frames.json",
            "recording.json",
            "capture.json",
        ]
    };
    for file in files {
        let from = source.join(file);
        let metadata = match std::fs::symlink_metadata(&from) {
            Ok(metadata) => metadata,
            Err(error)
                if error.kind() == std::io::ErrorKind::NotFound
                    && matches!(file, "recording.json" | "capture.json") =>
            {
                continue
            }
            Err(error) => return Err(error.into()),
        };
        ensure!(
            metadata.is_file()
                && !metadata.file_type().is_symlink()
                && metadata.len() <= 128 * 1024 * 1024 * 1024,
            "invalid capture artifact"
        );
        let mut input = tokio::fs::File::open(&from).await?;
        let mut destination = tokio::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(output.path.join(file))
            .await?;
        let mut buffer = vec![0; 64 * 1024];
        let mut written = 0u64;
        loop {
            let count =
                tokio::time::timeout(Duration::from_secs(2), input.read(&mut buffer)).await??;
            if count == 0 {
                break;
            }
            written = written
                .checked_add(count as u64)
                .context("artifact byte count overflow")?;
            ensure!(written <= metadata.len(), "artifact changed during copy");
            tokio::time::timeout(
                Duration::from_secs(2),
                destination.write_all(&buffer[..count]),
            )
            .await??;
        }
        ensure!(written == metadata.len(), "artifact truncated during copy");
        copied.push(from);
    }
    // A bad/incomplete response is never published to the requested destination.
    let verified = crate::reference::Recording::load(&output.path)?;
    ensure!(
        verified.index == result.index,
        "capture response differs from artifact index"
    );
    for file in copied {
        std::fs::remove_file(file)?;
    }
    std::fs::remove_dir(source)?;
    output.commit();
    Ok(result.index)
}

#[cfg(test)]
mod tests {
    use super::*;
    struct SyntheticBackend {
        identity: String,
    }
    impl CaptureBackend for SyntheticBackend {
        fn identity(&self) -> &str {
            &self.identity
        }
        fn record<'a>(&'a self, _: f64, output: &'a Path) -> CaptureFuture<'a> {
            Box::pin(async move {
                let bytes = vec![128; 1920 * 1080 * 3];
                crate::reference::record_sdr_stream(
                    std::io::Cursor::new(bytes),
                    &crate::recording::CaptureMode {
                        width: 1920,
                        height: 1080,
                        fps: 60,
                    },
                    "synthetic capture backend; no HDMI/device evidence",
                    output,
                    crate::reference::CaptureLimits {
                        expected_frames: 30,
                        max_frames: 32,
                        max_bytes: 10 * 1024 * 1024,
                        io_timeout: Duration::from_secs(2),
                    },
                )
                .await
            })
        }
    }

    struct SyntheticHdrBackend {
        identity: String,
    }
    impl CaptureBackend for SyntheticHdrBackend {
        fn identity(&self) -> &str {
            &self.identity
        }
        fn reservation_bytes(&self, _: f64) -> Result<u64> {
            Ok(32 * crate::hdr_capture::FRAME_BYTES as u64 + 8 * 1024 * 1024)
        }
        fn record<'a>(&'a self, _: f64, output: &'a Path) -> CaptureFuture<'a> {
            Box::pin(async move {
                let stamp = clock::monotonic_us()?;
                let index = RecordingIndex {
                    source: "synthetic native HDR service fixture; no hardware evidence".into(),
                    width: 1920,
                    height: 1080,
                    timestamps: vec![
                        (stamp - 16_667) as f64 / 1_000_000.0,
                        stamp as f64 / 1_000_000.0,
                    ],
                };
                let metadata = crate::recording::Recording {
                    source: index.source.clone(),
                    capture_mode: crate::recording::CaptureMode {
                        width: 1920,
                        height: 1080,
                        fps: 60,
                    },
                    calibration_id: None,
                    calibration_reason: Some("synthetic fixture".into()),
                    frames: Some(crate::evidence::ArtifactRef("frames.p010".into())),
                    pixel_format: crate::recording::PixelFormat::P010le,
                    bit_depth: 10,
                    color_space: crate::recording::ColorSpace::PqBt2020,
                    clock_domain: clock::local_clock()?.id,
                    frame_count: 2,
                    timestamp_count: 2,
                    timestamps: Some(vec![stamp - 16_667, stamp]),
                    validity: crate::evidence::Validity::Valid,
                    metadata_evidence: None,
                };
                let bytes = [0x40, 0x80].repeat(crate::hdr_capture::FRAME_BYTES);
                tokio::fs::write(output.join("frames.p010"), bytes).await?;
                tokio::fs::write(output.join("frames.json"), serde_json::to_vec(&index)?).await?;
                tokio::fs::write(
                    output.join("recording.json"),
                    serde_json::to_vec(&metadata)?,
                )
                .await?;
                tokio::fs::write(
                    output.join("capture-summary.json"),
                    b"{\"fixture\":\"synthetic transfer only; no physical device\"}",
                )
                .await?;
                Ok(index)
            })
        }
    }
    #[tokio::test]
    async fn native_hdr_roundtrip_through_service_keeps_p010_and_summary() {
        let root = std::env::temp_dir().join(format!("hdr-service-{}", uuid::Uuid::new_v4()));
        let output = std::env::temp_dir().join(format!("hdr-client-{}", uuid::Uuid::new_v4()));
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let (stop, stopped) = tokio::sync::oneshot::channel();
        let service = tokio::spawn(serve_with(
            listener,
            root.clone(),
            Arc::new(SyntheticHdrBackend {
                identity: format!("synthetic-hdr-{}", uuid::Uuid::new_v4()),
            }),
            async {
                let _ = stopped.await;
            },
        ));
        let result = record_via_server(address, 0.5, &output).await;
        stop.send(()).unwrap();
        service.await.unwrap().unwrap();
        result.unwrap();
        let recording = crate::reference::Recording::load(&output).unwrap();
        assert_eq!(recording.metadata.bit_depth, 10);
        assert_eq!(
            recording.metadata.color_space,
            crate::recording::ColorSpace::PqBt2020
        );
        assert_eq!(
            recording.read_frame(0).unwrap(),
            [0x40, 0x80].repeat(crate::hdr_capture::FRAME_BYTES / 2)
        );
        assert!(output.join("capture-summary.json").is_file());
        assert!(!output.join("frames.yuv").exists());
        assert_eq!(std::fs::read_dir(&root).unwrap().count(), 0);
        std::fs::remove_dir_all(root).unwrap();
        std::fs::remove_dir_all(output).unwrap();
    }

    struct ManagedBackend {
        inner: SyntheticBackend,
        bytes: u64,
    }
    impl CaptureBackend for ManagedBackend {
        fn identity(&self) -> &str {
            self.inner.identity()
        }
        fn manages_lease(&self) -> bool {
            true
        }
        fn reservation_bytes(&self, _: f64) -> Result<u64> {
            Ok(self.bytes)
        }
        fn record<'a>(&'a self, seconds: f64, output: &'a Path) -> CaptureFuture<'a> {
            Box::pin(async move {
                let _lease = DeviceLease::acquire(self.identity())?;
                self.inner.record(seconds, output).await
            })
        }
    }

    #[tokio::test]
    async fn backend_managed_lease_and_retention_apply_to_local_and_service_capture() {
        let root = std::env::temp_dir().join(format!("managed-service-{}", uuid::Uuid::new_v4()));
        let backend = Arc::new(ManagedBackend {
            inner: SyntheticBackend {
                identity: format!("managed-{}", uuid::Uuid::new_v4()),
            },
            bytes: 12 * 1024 * 1024,
        });
        let default = SyntheticBackend {
            identity: format!("default-{}", uuid::Uuid::new_v4()),
        };
        assert!(!default.manages_lease());
        assert_eq!(
            default.reservation_bytes(0.5).unwrap(),
            32 * (1920 * 1080 * 3 / 2) + 8 * 1024 * 1024
        );
        assert!(default.reservation_bytes(f64::NAN).is_err());
        record_local(&default, 0.5, &root.join("sdr-local"))
            .await
            .unwrap();
        record_local(backend.as_ref(), 0.5, &root.join("managed-local"))
            .await
            .unwrap();
        drop(DeviceLease::acquire(backend.identity()).unwrap());
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let (stop, stopped) = tokio::sync::oneshot::channel();
        let service = tokio::spawn(serve_with(listener, root.clone(), backend.clone(), async {
            let _ = stopped.await;
        }));
        let response = reqwest::Client::new()
            .get(format!("http://{address}/record?seconds=0.5&dir=managed"))
            .header("X-Moonlight-Capture", "1")
            .send()
            .await
            .unwrap();
        assert_eq!(response.status(), 200);
        let value: ServerRecording = response.json().await.unwrap();
        assert_eq!(value.index.timestamps.len(), 2);
        drop(DeviceLease::acquire(backend.identity()).unwrap());
        stop.send(()).unwrap();
        service.await.unwrap().unwrap();
        std::fs::remove_dir_all(root).unwrap();
    }

    #[tokio::test]
    async fn backend_advertised_oversize_budget_is_rejected_before_output_and_lease() {
        let root = std::env::temp_dir().join(format!("oversize-service-{}", uuid::Uuid::new_v4()));
        let backend = Arc::new(ManagedBackend {
            inner: SyntheticBackend {
                identity: format!("oversize-{}", uuid::Uuid::new_v4()),
            },
            bytes: 128 * 1024 * 1024 * 1024 + 1,
        });
        assert!(record_local(backend.as_ref(), 0.5, &root.join("local"))
            .await
            .is_err());
        assert!(!root.join("local").exists());
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let (stop, stopped) = tokio::sync::oneshot::channel();
        let service = tokio::spawn(serve_with(listener, root.clone(), backend.clone(), async {
            let _ = stopped.await;
        }));
        let response = reqwest::Client::new()
            .get(format!("http://{address}/record?seconds=0.5&dir=remote"))
            .header("X-Moonlight-Capture", "1")
            .send()
            .await
            .unwrap();
        assert_eq!(response.status(), 409);
        assert!(!root.join("remote").exists());
        drop(DeviceLease::acquire(backend.identity()).unwrap());
        stop.send(()).unwrap();
        service.await.unwrap().unwrap();
        std::fs::remove_dir_all(root).unwrap();
    }

    #[tokio::test]
    async fn native_sdr_roundtrip_through_service_keeps_metadata_and_clears_server_artifacts() {
        let root = std::env::temp_dir().join(format!("sdr-service-{}", uuid::Uuid::new_v4()));
        let output = std::env::temp_dir().join(format!("sdr-client-{}", uuid::Uuid::new_v4()));
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let backend = Arc::new(SyntheticBackend {
            identity: format!("synthetic-{}", uuid::Uuid::new_v4()),
        });
        let (stop, stopped) = tokio::sync::oneshot::channel();
        let service = tokio::spawn(serve_with(listener, root.clone(), backend, async {
            let _ = stopped.await;
        }));
        record_via_server(address, 0.5, &output).await.unwrap();
        let recording = crate::reference::Recording::load(&output).unwrap();
        assert_eq!(recording.metadata.frame_count, 2);
        assert_eq!(recording.metadata.capture_mode.width, 1920);
        assert_eq!(
            recording.metadata.clock_domain,
            clock::local_clock().unwrap().id
        );
        assert!(recording.metadata.source.contains("synthetic"));
        assert!(output.join("capture.json").is_file());
        assert_eq!(std::fs::read_dir(&root).unwrap().count(), 0);
        stop.send(()).unwrap();
        service.await.unwrap().unwrap();
        std::fs::remove_dir_all(root).unwrap();
        std::fs::remove_dir_all(output).unwrap();
    }

    struct PendingBackend {
        identity: String,
        entered: Arc<tokio::sync::Notify>,
        dropped: Arc<std::sync::atomic::AtomicBool>,
    }
    impl CaptureBackend for PendingBackend {
        fn identity(&self) -> &str {
            &self.identity
        }
        fn record<'a>(&'a self, _: f64, output: &'a Path) -> CaptureFuture<'a> {
            Box::pin(async move {
                struct Release(Arc<std::sync::atomic::AtomicBool>);
                impl Drop for Release {
                    fn drop(&mut self) {
                        self.0.store(true, std::sync::atomic::Ordering::SeqCst);
                    }
                }
                let _release = Release(self.dropped.clone());
                std::fs::write(output.join("partial"), b"synthetic").unwrap();
                self.entered.notify_one();
                std::future::pending().await
            })
        }
    }

    #[tokio::test]
    async fn loopback_disconnect_and_shutdown_cancel_capture_and_release_ownership() {
        let root = std::env::temp_dir().join(format!("capture-loopback-{}", uuid::Uuid::new_v4()));
        let entered = Arc::new(tokio::sync::Notify::new());
        let dropped = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let backend = Arc::new(PendingBackend {
            identity: format!("fake-{}", uuid::Uuid::new_v4()),
            entered: entered.clone(),
            dropped: dropped.clone(),
        });
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let (stop, stopped) = tokio::sync::oneshot::channel();
        let service_root = root.clone();
        let task = tokio::spawn(serve_with(listener, service_root, backend.clone(), async {
            let _ = stopped.await;
        }));
        let mut first = TcpStream::connect(address).await.unwrap();
        first
            .write_all(b"GET /record?seconds=1&dir=one HTTP/1.0\r\nX-Moonlight-Capture: 1\r\n\r\n")
            .await
            .unwrap();
        tokio::time::timeout(Duration::from_secs(2), entered.notified())
            .await
            .unwrap();
        let response = reqwest::Client::new()
            .get(format!("http://{address}/record?seconds=1&dir=two"))
            .header("X-Moonlight-Capture", "1")
            .send()
            .await
            .unwrap();
        assert_eq!(response.status(), 409);
        assert!(!root.join("two").exists());
        drop(first);
        tokio::time::timeout(Duration::from_secs(2), async {
            while root.join("one").exists() {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        assert!(dropped.load(std::sync::atomic::Ordering::SeqCst));
        drop(DeviceLease::acquire(backend.identity()).unwrap());
        let mut second = TcpStream::connect(address).await.unwrap();
        second
            .write_all(
                b"GET /record?seconds=1&dir=shutdown HTTP/1.0\r\nX-Moonlight-Capture: 1\r\n\r\n",
            )
            .await
            .unwrap();
        tokio::time::timeout(Duration::from_secs(2), entered.notified())
            .await
            .unwrap();
        stop.send(()).unwrap();
        task.await.unwrap().unwrap();
        assert!(!root.join("shutdown").exists());
        drop(DeviceLease::acquire(backend.identity()).unwrap());
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn requests_reject_nonlocal_unbounded_duplicate_and_traversal_inputs() {
        let valid =
            b"GET /record?seconds=0.5&dir=rec-test HTTP/1.0\r\nX-Moonlight-Capture: 1\r\n\r\n";
        assert!(Request::parse(valid).is_ok());
        for target in [
            "seconds=NaN&dir=a",
            "seconds=1&seconds=2&dir=a",
            "seconds=1&dir=../x",
            "seconds=601&dir=a",
            "seconds=1&dir=a%2fb",
        ] {
            let data = format!("GET /record?{target} HTTP/1.0\r\nX-Moonlight-Capture: 1\r\n\r\n");
            assert!(Request::parse(data.as_bytes()).is_err());
        }
        assert!(Request::parse(b"GET /record?seconds=1&dir=a HTTP/1.0\r\n\r\n").is_err());
        assert!(
            Request::parse(b"GET /health HTTP/1.0\r\nOrigin: https://evil.test\r\n\r\n").is_err()
        );
        assert!(validate_loopback("192.168.1.1:9000".parse().unwrap()).is_err());
    }
    #[test]
    fn directories_and_device_leases_are_exclusive_and_cleaned_on_drop() {
        let root = std::env::temp_dir().join(format!("capture-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&root).unwrap();
        let output = OutputReservation::create(&root, "one").unwrap();
        assert!(OutputReservation::create(&root, "one").is_err());
        let identity = format!("synthetic-{}", uuid::Uuid::new_v4());
        let lease = DeviceLease::acquire(&identity).unwrap();
        assert!(DeviceLease::acquire(&identity).is_err());
        drop(lease);
        drop(DeviceLease::acquire(&identity).unwrap());
        drop(output);
        assert!(!root.join("one").exists());
        std::os::unix::fs::symlink(&root, root.join("escape")).unwrap();
        assert!(OutputReservation::create(&root, "escape").is_err());
        std::fs::remove_dir_all(root).unwrap();
    }
}
