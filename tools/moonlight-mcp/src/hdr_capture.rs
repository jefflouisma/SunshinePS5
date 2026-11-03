//! Safe consumer for the isolated P010 USB helper; no USB FFI or privilege escalation.
//! Acquisition success is distinct from calibration and independent source HDR metadata.

use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt, BufWriter};
use tokio::process::{Child, ChildStdin, Command};

use crate::capture_server::{CaptureBackend, CaptureFuture, DeviceLease};
use crate::clock;
use crate::evidence::{ArtifactRef, Validity};
use crate::recording::{CaptureMode, ColorSpace, PixelFormat, Recording};
use crate::reference::{CaptureLimits, RecordingIndex};
use crate::report::Status;

pub const FRAME_BYTES: usize = 1920 * 1080 * 3;
const INTERVAL: u32 = 166_666;
const MAX_FRAMES: u64 = 36_000;
const MAX_BYTES: u64 = 128 * 1024 * 1024 * 1024;
const DEVICE_ID: &str = "usb-0fd9-009c";
pub(crate) const SUMMARY_FILE: &str = "capture-summary.json";

/// Operator-established HDMI color context, independent of USB sample packing.
/// This does not assert access to source mastering-display/light-level metadata.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HdrSource {
    pub description: String,
    pub evidence_id: String,
}
impl HdrSource {
    fn validate(&self) -> Result<()> {
        crate::evidence::validate_reason(&self.description)?;
        crate::evidence::validate_id(&self.evidence_id)
    }
}

pub fn validate_p010_frame(recording: &Recording, bytes: &[u8]) -> Result<()> {
    recording.validate()?;
    ensure!(
        recording.pixel_format == PixelFormat::P010le
            && recording.bit_depth == 10
            && recording.color_space == ColorSpace::PqBt2020,
        "required native P010 PQ/BT.2020 context unavailable"
    );
    validate_samples(bytes, usize::try_from(recording.checked_frame_bytes()?)?)
}
fn validate_samples(bytes: &[u8], expected: usize) -> Result<()> {
    ensure!(bytes.len() == expected, "truncated/oversized P010 frame");
    // Validate four packed samples per iteration; this keeps the native-rate reader
    // bounded without a scalar iterator for each of 3,110,400 samples per frame.
    let mut words = bytes.chunks_exact(8);
    ensure!(
        words.all(
            |word| u64::from_le_bytes(word.try_into().expect("eight-byte chunk"))
                & 0x003f_003f_003f_003f
                == 0
        ) && words
            .remainder()
            .chunks_exact(2)
            .all(|sample| sample[0] & 0x3f == 0),
        "P010 low six padding bits must be zero; no eight-bit conversion permitted"
    );
    Ok(())
}
fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("fixed header"))
}
fn u64_at(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().expect("fixed header"))
}
async fn read_exact(
    input: &mut (impl AsyncRead + Unpin),
    bytes: &mut [u8],
    timeout: Duration,
) -> Result<()> {
    tokio::time::timeout(timeout, input.read_exact(bytes))
        .await
        .context("HDR helper read timed out")??;
    Ok(())
}

#[derive(Debug, Serialize, Deserialize)]
pub struct HdrSummary {
    pub expected_frames: u64,
    pub recorded_frames: u64,
    pub observed_usb_frames: u64,
    pub rejected_usb_frames: u64,
    pub usb_loss_reason: String,
    pub descriptor_interval_100ns: u32,
    pub source: HdrSource,
    pub clock: crate::evidence::Clock,
    pub timestamp_boundary: String,
    pub started_us: u64,
    pub ended_us: u64,
    pub ownership_restored: bool,
}

/// Parses a bounded helper stream into the existing native recording layout.
/// The caller owns a fresh OutputReservation; no metadata is committed on failure.
/// One frame plus fixed writer storage; timestamp/sequence vectors have a fixed cap.
struct PendingAcquisition {
    index: RecordingIndex,
    recording: Recording,
    summary: HdrSummary,
}
fn checked_acquisition(
    result: Result<PendingAcquisition>,
    cleanup: Result<()>,
) -> Result<PendingAcquisition> {
    match (result, cleanup) {
        (Err(error), Err(cleanup)) => {
            Err(error.context(format!("HDR helper termination: {cleanup:#}")))
        }
        (Err(error), Ok(())) => Err(error),
        (Ok(_), Err(error)) => Err(error),
        (Ok(pending), Ok(())) => Ok(pending),
    }
}
impl PendingAcquisition {
    async fn commit(self, output: &Path, timeout: Duration) -> Result<RecordingIndex> {
        for (name, bytes) in [
            ("frames.json", serde_json::to_vec(&self.index)?),
            (SUMMARY_FILE, serde_json::to_vec(&self.summary)?),
            ("recording.json", serde_json::to_vec(&self.recording)?),
        ] {
            let mut file = tokio::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(output.join(name))
                .await?;
            tokio::time::timeout(timeout, file.write_all(&bytes)).await??;
            // Tokio writes may still be queued after write_all returns. Commit requires the
            // sidecar to be readable by the synchronous recording loader before returning.
            tokio::time::timeout(timeout, file.flush()).await??;
        }
        Ok(self.index)
    }
}

async fn ingest(
    mut input: impl AsyncRead + Unpin,
    output: &Path,
    source: &HdrSource,
    limits: CaptureLimits,
) -> Result<PendingAcquisition> {
    source.validate()?;
    ensure!(
        limits.expected_frames > 0
            && limits.expected_frames <= limits.max_frames
            && limits.max_frames <= MAX_FRAMES
            && limits.max_bytes <= MAX_BYTES
            && limits.max_bytes >= limits.expected_frames * FRAME_BYTES as u64
            && !limits.io_timeout.is_zero(),
        "invalid HDR acquisition bounds"
    );
    let clock = clock::local_clock()?;
    let started = clock::monotonic_us()?;
    let mut session = [0; 128];
    read_exact(&mut input, &mut session, limits.io_timeout).await?;
    ensure!(
        &session[..4] == b"SP5H"
            && u32_at(&session, 4) == 1
            && u32_at(&session, 8) == FRAME_BYTES as u32
            && u32_at(&session, 12) == INTERVAL
            && u32_at(&session, 16) == 1920
            && u32_at(&session, 20) == 1080
            && &session[24..28] == b"P010"
            && session[28..32] == [0; 4],
        "helper rejected native 1080p60 P010 contract"
    );
    let id_length = session[32..]
        .iter()
        .position(|v| *v == 0)
        .context("unterminated clock ID")?;
    ensure!(
        &session[32..32 + id_length] == clock.id.as_bytes()
            && session[32 + id_length..].iter().all(|v| *v == 0),
        "helper clock epoch mismatch"
    );
    let file = tokio::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output.join("frames.p010"))
        .await?;
    let mut writer = BufWriter::with_capacity(64 * 1024, file);
    let mut frame = vec![0; FRAME_BYTES];
    let mut timestamps = Vec::with_capacity(limits.max_frames as usize);
    let mut sequence = 0;
    let mut bad = 0;
    loop {
        let mut header = [0; 48];
        read_exact(&mut input, &mut header, limits.io_timeout).await?;
        ensure!(
            header[40..].iter().all(|v| *v == 0),
            "nonzero reserved fields"
        );
        let stamp = u64_at(&header, 8);
        let observed = clock::monotonic_us()?;
        ensure!(
            stamp >= started && stamp <= observed,
            "unrelated/future helper timestamp"
        );
        let next_sequence = u64_at(&header, 16);
        let next_bad = u64_at(&header, 24);
        if &header[..4] == b"DONE" {
            ensure!(
                u32_at(&header, 4) == 0
                    && u32_at(&header, 32) == 1
                    && u32_at(&header, 36) == 0
                    && next_sequence >= sequence
                    && next_bad >= bad
                    && next_sequence.checked_sub(next_bad) == Some(timestamps.len() as u64)
                    && timestamps.len() as u64 == limits.expected_frames
                    && timestamps.last().is_some_and(|previous| stamp >= *previous),
                "HDR acquisition incomplete or device restoration unverified: frames={}/{}, observed={}, rejected={}, restored={}, reason={}",
                timestamps.len(), limits.expected_frames, next_sequence, next_bad,
                u32_at(&header, 32), u32_at(&header, 36)
            );
            sequence = next_sequence;
            bad = next_bad;
            break;
        }
        ensure!(
            &header[..4] == b"FRAM"
                && u32_at(&header, 4) == FRAME_BYTES as u32
                && u32_at(&header, 32) == 0
                && u32_at(&header, 36) == 0
                && next_sequence > sequence
                && next_bad >= bad
                && next_sequence.checked_sub(next_bad) == Some(timestamps.len() as u64 + 1)
                && (timestamps.len() as u64) < limits.max_frames
                && (timestamps.len() as u64 + 1) * FRAME_BYTES as u64 <= limits.max_bytes
                && timestamps.last().is_none_or(|previous| stamp > *previous),
            "invalid/reversed/oversize HDR frame record"
        );
        read_exact(&mut input, &mut frame, limits.io_timeout).await?;
        validate_samples(&frame, FRAME_BYTES)?;
        tokio::time::timeout(limits.io_timeout, writer.write_all(&frame)).await??;
        timestamps.push(stamp);
        sequence = next_sequence;
        bad = next_bad;
    }
    let mut extra = [0];
    ensure!(
        tokio::time::timeout(limits.io_timeout, input.read(&mut extra)).await?? == 0,
        "trailing helper bytes"
    );
    tokio::time::timeout(limits.io_timeout, writer.flush()).await??;
    let recording = Recording {
        source: format!("Elgato 4K X direct USB P010; {}", source.description),
        capture_mode: CaptureMode {
            width: 1920,
            height: 1080,
            fps: 60,
        },
        calibration_id: None,
        calibration_reason: Some("USB acquisition is not capture-chain calibration".into()),
        frames: Some(ArtifactRef("frames.p010".into())),
        pixel_format: PixelFormat::P010le,
        bit_depth: 10,
        color_space: ColorSpace::PqBt2020,
        clock_domain: clock.id.clone(),
        frame_count: timestamps.len() as u64,
        timestamp_count: timestamps.len() as u64,
        timestamps: Some(timestamps.clone()),
        validity: Validity::Valid,
        metadata_evidence: None,
    };
    recording.validate_files(output)?;
    let index = RecordingIndex {
        source: recording.source.clone(),
        width: 1920,
        height: 1080,
        timestamps: timestamps
            .iter()
            .map(|us| *us as f64 / 1_000_000.0)
            .collect(),
    };
    let summary = HdrSummary {
        expected_frames: limits.expected_frames,
        recorded_frames: timestamps.len() as u64,
        observed_usb_frames: sequence,
        rejected_usb_frames: bad,
        usb_loss_reason: "Rejected frames count observed malformed/short/error USB frames; missing HDMI frames without a USB boundary are unobservable. Cadence gaps are separate, not host drops.".into(),
        descriptor_interval_100ns: INTERVAL,
        source: source.clone(),
        clock,
        timestamp_boundary: "CLOCK_MONOTONIC microseconds at complete validated UVC frame, before pipe write; not HDMI exposure or source PTS".into(),
        started_us: started,
        ended_us: clock::monotonic_us()?,
        ownership_restored: true,
    };
    Ok(PendingAcquisition {
        index,
        recording,
        summary,
    })
}

/// Holds ownership until the helper exits, including cancellation of the Rust future.
/// Closing stdin asks the helper to stop; it performs its normal rollback, without SIGKILL.
struct HelperGuard {
    child: Option<Child>,
    control: Option<ChildStdin>,
    lease: Option<DeviceLease>,
}
impl HelperGuard {
    async fn finish(&mut self) -> Result<()> {
        self.control.take();
        let child = self.child.as_mut().context("helper already reaped")?;
        let status = tokio::time::timeout(Duration::from_secs(10), child.wait())
            .await
            .context("helper cleanup unverified; inspect device ownership")??;
        self.child.take();
        if !matches!(status.code(), Some(0 | 1 | 64)) {
            if let Some(lease) = self.lease.take() {
                std::mem::forget(lease);
            }
        } else {
            self.lease.take();
        }
        ensure!(
            status.success(),
            "HDR helper failed; required capture is Incomplete"
        );
        Ok(())
    }
}
impl Drop for HelperGuard {
    fn drop(&mut self) {
        self.control.take();
        if let Some(mut child) = self.child.take() {
            let lease = self.lease.take();
            // The helper's own input/output/USB deadlines still apply if runtime shuts down.
            if let Ok(runtime) = tokio::runtime::Handle::try_current() {
                runtime.spawn(async move {
                    match tokio::time::timeout(Duration::from_secs(10), child.wait()).await {
                        Ok(Ok(status)) if matches!(status.code(), Some(0 | 1 | 64)) => drop(lease),
                        _ => {
                            eprintln!("HDR helper cleanup unverified; device lease retained for inspection");
                            if let Some(lease) = lease { std::mem::forget(lease); }
                        }
                    }
                });
            } else if let Some(lease) = lease {
                std::mem::forget(lease);
            }
        }
    }
}

/// No sudo invocation: the operator provisions/authorizes the isolated helper separately.
#[derive(Debug, Clone)]
pub struct HdrCapture {
    pub executable: PathBuf,
    pub source: Option<HdrSource>,
    pub max_bytes: u64,
}
#[derive(Debug)]
pub struct HdrAcquisition {
    pub status: Status,
    pub recording: Option<RecordingIndex>,
    pub reason: Option<String>,
}
impl HdrCapture {
    async fn record_native(&self, seconds: f64, output: &Path) -> Result<RecordingIndex> {
        crate::capture_server::validate_seconds(seconds)?;
        let source = self
            .source
            .as_ref()
            .context("known HDR PQ/BT.2020 source evidence unavailable")?;
        source.validate()?;
        let count = (seconds * 60.0).ceil() as u64;
        ensure!(
            count <= MAX_FRAMES
                && self.max_bytes <= MAX_BYTES
                && self.max_bytes >= count * FRAME_BYTES as u64,
            "HDR disk/frame budget exceeded"
        );
        ensure!(
            self.executable.is_absolute(),
            "explicit absolute helper path required"
        );
        let lease = DeviceLease::acquire(DEVICE_ID)?;
        let clock = clock::local_clock()?;
        let mut child = Command::new(&self.executable)
            .args(["--count", &count.to_string(), "--clock-id", &clock.id])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .kill_on_drop(false)
            .spawn()
            .context("required HDR helper unavailable")?;
        let control = child.stdin.take();
        let stdout = child.stdout.take().context("helper output unavailable")?;
        let mut guard = HelperGuard {
            child: Some(child),
            control,
            lease: Some(lease),
        };
        let result = tokio::time::timeout(
            Duration::from_secs_f64(seconds + 15.0),
            ingest(
                stdout,
                output,
                source,
                CaptureLimits {
                    expected_frames: count,
                    max_frames: count,
                    max_bytes: self.max_bytes,
                    io_timeout: Duration::from_secs(7),
                },
            ),
        )
        .await
        .context("HDR acquisition deadline exceeded")
        .and_then(|result| result);
        let cleanup = guard.finish().await;
        checked_acquisition(result, cleanup)?
            .commit(output, Duration::from_secs(7))
            .await
    }
    /// Missing source/helper, permission/mode errors, loss of ownership and disconnects
    /// all preserve the required path as Incomplete; no SDR acquisition is attempted.
    pub async fn acquire(&self, seconds: f64, output: &Path) -> HdrAcquisition {
        match self.record_native(seconds, output).await {
            Ok(index) => HdrAcquisition {
                status: Status::Pass,
                recording: Some(index),
                reason: None,
            },
            Err(error) => HdrAcquisition {
                status: Status::Incomplete,
                recording: None,
                reason: Some(format!("required HDR acquisition unavailable: {error:#}")),
            },
        }
    }
}
impl CaptureBackend for HdrCapture {
    fn manages_lease(&self) -> bool {
        true
    }
    fn reservation_bytes(&self, seconds: f64) -> Result<u64> {
        crate::capture_server::validate_seconds(seconds)?;
        let count = (seconds * 60.0).ceil() as u64;
        ensure!(
            count <= MAX_FRAMES
                && self.max_bytes <= MAX_BYTES
                && self.max_bytes >= count * FRAME_BYTES as u64,
            "HDR disk/frame budget exceeded"
        );
        Ok((count + 2) * FRAME_BYTES as u64 + 8 * 1024 * 1024)
    }
    fn identity(&self) -> &str {
        DEVICE_ID
    }
    fn record<'a>(&'a self, seconds: f64, output: &'a Path) -> CaptureFuture<'a> {
        Box::pin(async move {
            let result = self.acquire(seconds, output).await;
            result.recording.context(
                result
                    .reason
                    .unwrap_or_else(|| "HDR capture Incomplete".into()),
            )
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::capture_server::OutputReservation;
    use tokio::io::DuplexStream;

    fn source() -> HdrSource {
        HdrSource {
            description: "synthetic PQ/BT.2020 fixture; no hardware evidence".into(),
            evidence_id: "synthetic-hdr-source".into(),
        }
    }
    fn limits() -> CaptureLimits {
        CaptureLimits {
            expected_frames: 1,
            max_frames: 1,
            max_bytes: FRAME_BYTES as u64,
            io_timeout: Duration::from_secs(2),
        }
    }
    fn reservation() -> OutputReservation {
        OutputReservation::create(
            &std::env::temp_dir().join("sunshine-hdr-tests"),
            &uuid::Uuid::new_v4().to_string(),
        )
        .unwrap()
    }
    #[derive(Clone, Copy)]
    enum Fault {
        None,
        Format,
        Clock,
        Size,
        Padding,
        Restore,
        Sequence,
        Truncate,
        Future,
        Trailing,
        ReverseDone,
    }
    async fn fixture(mut output: DuplexStream, fault: Fault) {
        let result: Result<()> = async {
            let mut header = [0u8; 128];
            header[..4].copy_from_slice(b"SP5H");
            for (offset, value) in [
                (4, 1u32),
                (8, FRAME_BYTES as u32),
                (12, INTERVAL),
                (16, 1920),
                (20, 1080),
            ] {
                header[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
            }
            header[24..28].copy_from_slice(if matches!(fault, Fault::Format) {
                b"YUY2"
            } else {
                b"P010"
            });
            let clock = clock::local_clock()?;
            header[32..32 + clock.id.len()].copy_from_slice(clock.id.as_bytes());
            if matches!(fault, Fault::Clock) {
                header[32] = b'x';
            }
            output.write_all(&header).await?;
            // Timestamp in producer after consumer began reading the session header.
            tokio::task::yield_now().await;
            let mut frame_header = [0u8; 48];
            frame_header[..4].copy_from_slice(b"FRAM");
            frame_header[4..8].copy_from_slice(
                &(if matches!(fault, Fault::Size) {
                    1
                } else {
                    FRAME_BYTES as u32
                })
                .to_le_bytes(),
            );
            let stamp = clock::monotonic_us()?
                + if matches!(fault, Fault::Future) {
                    1_000_000_000
                } else {
                    0
                };
            frame_header[8..16].copy_from_slice(&stamp.to_le_bytes());
            frame_header[16..24].copy_from_slice(
                &(if matches!(fault, Fault::Sequence) {
                    2u64
                } else {
                    1u64
                })
                .to_le_bytes(),
            );
            output.write_all(&frame_header).await?;
            let mut bytes: Vec<u8> = (0..FRAME_BYTES / 2)
                .flat_map(|i| (((i % 1024) as u16) << 6).to_le_bytes())
                .collect();
            if matches!(fault, Fault::Padding) {
                bytes[0] = 1;
            }
            if matches!(fault, Fault::Truncate) {
                bytes.truncate(16);
            }
            output.write_all(&bytes).await?;
            if matches!(fault, Fault::Truncate) {
                return Ok(());
            }
            let mut done = [0u8; 48];
            done[..4].copy_from_slice(b"DONE");
            done[8..16].copy_from_slice(
                &(if matches!(fault, Fault::ReverseDone) {
                    stamp - 1
                } else {
                    clock::monotonic_us()?
                })
                .to_le_bytes(),
            );
            done[16..24].copy_from_slice(&1u64.to_le_bytes());
            done[32..36].copy_from_slice(
                &(if matches!(fault, Fault::Restore) {
                    0u32
                } else {
                    1u32
                })
                .to_le_bytes(),
            );
            output.write_all(&done).await?;
            if matches!(fault, Fault::Trailing) {
                output.write_all(&[1]).await?;
            }
            Ok(())
        }
        .await;
        // Invalid fixtures intentionally close their consumer before the producer finishes.
        let _ = result;
    }
    #[tokio::test]
    async fn helper_stream_preserves_every_ten_bit_sample_and_existing_sidecars() {
        let output = reservation();
        let (reader, writer) = tokio::io::duplex(64 * 1024);
        let producer = tokio::spawn(fixture(writer, Fault::None));
        let index = ingest(reader, &output.path, &source(), limits())
            .await
            .unwrap()
            .commit(&output.path, Duration::from_secs(2))
            .await
            .unwrap();
        producer.await.unwrap();
        let loaded = crate::reference::Recording::load(&output.path).unwrap();
        assert_eq!(loaded.index, index);
        assert_eq!(loaded.metadata.bit_depth, 10);
        assert_eq!(loaded.metadata.color_space, ColorSpace::PqBt2020);
        assert!(loaded.metadata.calibration_id.is_none());
        assert!(loaded.metadata.metadata_evidence.is_none());
        let bytes = loaded.read_frame(0).unwrap();
        for (i, pair) in bytes.chunks_exact(2).enumerate() {
            assert_eq!(
                u16::from_le_bytes(pair.try_into().unwrap()),
                ((i % 1024) as u16) << 6
            );
        }
        let summary: HdrSummary = serde_json::from_slice(
            &std::fs::read(output.path.join("capture-summary.json")).unwrap(),
        )
        .unwrap();
        assert!(summary.ownership_restored);
        assert_eq!(summary.recorded_frames, 1);
        assert_eq!(summary.rejected_usb_frames, 0);
    }
    #[tokio::test]
    async fn malformed_unrelated_clock_partial_disconnect_and_unrestored_streams_fail_closed() {
        for fault in [
            Fault::Format,
            Fault::Clock,
            Fault::Size,
            Fault::Padding,
            Fault::Restore,
            Fault::Sequence,
            Fault::Truncate,
            Fault::Future,
            Fault::Trailing,
            Fault::ReverseDone,
        ] {
            let output = reservation();
            let (reader, writer) = tokio::io::duplex(64 * 1024);
            let producer = tokio::spawn(fixture(writer, fault));
            assert!(ingest(reader, &output.path, &source(), limits())
                .await
                .is_err());
            producer.await.unwrap();
            assert!(!output.path.join("recording.json").exists());
        }
        let output = reservation();
        let (reader, _writer) = tokio::io::duplex(1);
        let mut limits = limits();
        limits.io_timeout = Duration::from_millis(10);
        assert!(ingest(reader, &output.path, &source(), limits)
            .await
            .is_err());
    }
    #[test]
    fn packed_sample_validation_checks_each_lane_padding_bit_and_tail() {
        let bytes = [0x40, 0xff].repeat(5);
        validate_samples(&bytes, 10).unwrap();
        for lane in 0..5 {
            for bit in 0..6 {
                let mut invalid = bytes.clone();
                invalid[lane * 2] |= 1 << bit;
                assert!(validate_samples(&invalid, 10).is_err());
            }
        }
        assert!(validate_samples(&bytes[..9], 10).is_err());
    }
    #[test]
    fn helper_failure_preserves_the_primary_acquisition_error() {
        let result = checked_acquisition(
            Err(anyhow::anyhow!("native parse error")),
            Err(anyhow::anyhow!("helper exit failure")),
        );
        let diagnostic = format!("{:#}", result.err().unwrap());
        assert!(diagnostic.contains("native parse error"));
        assert!(diagnostic.contains("helper exit failure"));
    }
    #[tokio::test]
    async fn missing_source_or_helper_is_incomplete_without_an_sdr_substitute() {
        let output = reservation();
        let mut backend = HdrCapture {
            executable: output.path.join("nonexistent-helper"),
            source: None,
            max_bytes: 128 * 1024 * 1024 * 1024,
        };
        assert_eq!(
            backend.acquire(1.0, &output.path).await.status,
            Status::Incomplete
        );
        backend.source = Some(source());
        let unavailable = backend.acquire(1.0, &output.path).await;
        assert_eq!(unavailable.status, Status::Incomplete);
        assert!(unavailable.recording.is_none());
        assert!(unavailable.reason.unwrap().contains("helper unavailable"));
        assert!(!output.path.join("frames.yuv").exists());
        assert!(!output.path.join("recording.json").exists());
    }
    #[tokio::test]
    async fn cancellation_closes_control_and_reaps_before_releasing_ownership() {
        let identity = format!("synthetic-helper-{}", uuid::Uuid::new_v4());
        let lease = DeviceLease::acquire(&identity).unwrap();
        let mut child = Command::new("/bin/sh")
            .args(["-c", "cat >/dev/null"])
            .stdin(Stdio::piped())
            .spawn()
            .unwrap();
        let control = child.stdin.take();
        let guard = HelperGuard {
            child: Some(child),
            control,
            lease: Some(lease),
        };
        assert!(DeviceLease::acquire(&identity).is_err());
        drop(guard);
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if let Ok(lease) = DeviceLease::acquire(&identity) {
                    drop(lease);
                    break;
                }
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
    }
}
