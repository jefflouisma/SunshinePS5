//! Native SDR reference acquisition through the bounded local capture service.
//!
//! Frames retain 1920×1080 eight-bit limited-range BT.709 samples. Timestamps
//! describe receipt of each complete raw frame on this machine, including child
//! pipe/scheduling uncertainty; they do not claim device exposure timestamps.
//! Calibration and independently observed HDMI/source metadata remain absent.

use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::io::{Read, Seek, SeekFrom, Write};
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt, BufWriter};
use tokio::process::Command;

use crate::clock;
use crate::evidence::{ArtifactRef, Validity};
use crate::recording::{CaptureMode, ColorSpace, PixelFormat, Recording as NativeRecording};

const FRAMES_FILE: &str = "frames.yuv";
pub(crate) const INDEX_FILE: &str = "frames.json";
pub(crate) const NATIVE_FILE: &str = "recording.json";
const MAX_METADATA_BYTES: u64 = 4 * 1024 * 1024;
pub(crate) const MAX_FRAMES: u64 = 36_002;
const MAX_DISK_BYTES: u64 = 128 * 1024 * 1024 * 1024;

/// Seconds in the metadata's explicit monotonic domain, retained for existing comparison APIs.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct RecordingIndex {
    pub source: String,
    pub width: usize,
    pub height: usize,
    pub timestamps: Vec<f64>,
}

pub struct Recording {
    pub index: RecordingIndex,
    pub metadata: NativeRecording,
    frames_path: PathBuf,
}
impl Recording {
    pub fn load(dir: &Path) -> Result<Self> {
        let index: RecordingIndex = read_metadata(&dir.join(INDEX_FILE))?;
        validate_index(&index)?;
        // Legacy Unix-time sidecars are not silently upgraded to monotonic evidence.
        let metadata: NativeRecording = read_metadata(&dir.join(NATIVE_FILE))
            .context("native monotonic recording metadata required; legacy wall-clock captures must be reacquired")?;
        metadata.validate_files(dir)?;
        ensure!(
            metadata.capture_mode.width as usize == index.width
                && metadata.capture_mode.height as usize == index.height
                && metadata.source == index.source
                && metadata.frame_count == index.timestamps.len() as u64,
            "recording index/native metadata mismatch"
        );
        let times = metadata
            .timestamps
            .as_ref()
            .context("native timestamps missing")?;
        ensure!(
            times
                .iter()
                .zip(&index.timestamps)
                .all(|(us, sec)| (*us as f64 / 1_000_000.0 - sec).abs()
                    <= 0.000_001 + f64::EPSILON * sec.abs()),
            "timestamp sidecars disagree"
        );
        let frames_path = metadata
            .frames
            .as_ref()
            .context("frames missing")?
            .resolve(dir)?;
        ensure!(
            !std::fs::symlink_metadata(&frames_path)?
                .file_type()
                .is_symlink(),
            "frame symlink forbidden"
        );
        Ok(Self {
            index,
            metadata,
            frames_path,
        })
    }
    pub fn frame_count(&self) -> usize {
        self.index.timestamps.len()
    }
    pub fn read_frame(&self, i: usize) -> Result<Vec<u8>> {
        ensure!(
            i < self.frame_count(),
            "frame index outside recorded population"
        );
        let size = self.metadata.checked_frame_bytes()?;
        ensure!(size <= 64 * 1024 * 1024, "frame read exceeds memory limit");
        let offset = (i as u64)
            .checked_mul(size)
            .context("frame offset overflow")?;
        let mut file = std::fs::File::open(&self.frames_path)?;
        file.seek(SeekFrom::Start(offset))?;
        let mut bytes = vec![0; usize::try_from(size)?];
        file.read_exact(&mut bytes)?;
        Ok(bytes)
    }
}
/// Bounded, symlink-refusing sidecar reader shared by every consumer of a capture directory.
pub(crate) fn read_metadata<T: serde::de::DeserializeOwned>(path: &Path) -> Result<T> {
    let metadata = std::fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file()
            && !metadata.file_type().is_symlink()
            && metadata.len() <= MAX_METADATA_BYTES,
        "invalid/oversize recording metadata"
    );
    Ok(serde_json::from_reader(
        std::fs::File::open(path)?.take(MAX_METADATA_BYTES + 1),
    )?)
}
fn validate_index(index: &RecordingIndex) -> Result<()> {
    ensure!(
        !index.timestamps.is_empty()
            && index.timestamps.len() <= MAX_FRAMES as usize
            && index
                .timestamps
                .iter()
                .all(|v| v.is_finite() && *v >= 0.0 && *v < u64::MAX as f64 / 1_000_000.0)
            && index.timestamps.windows(2).all(|w| w[1] > w[0]),
        "missing/reversed/discontinuous recording timestamps"
    );
    Ok(())
}
fn native_metadata(
    index: &RecordingIndex,
    fps: u32,
    domain: String,
    timestamps: Vec<u64>,
) -> Result<NativeRecording> {
    let recording = NativeRecording {
        source: index.source.clone(),
        capture_mode: CaptureMode {
            width: u32::try_from(index.width)?,
            height: u32::try_from(index.height)?,
            fps,
        },
        calibration_id: None,
        calibration_reason: Some("capture-chain calibration not established by acquisition".into()),
        frames: Some(ArtifactRef(FRAMES_FILE.into())),
        pixel_format: PixelFormat::Yuv420p,
        bit_depth: 8,
        color_space: ColorSpace::Bt709,
        clock_domain: domain,
        frame_count: timestamps.len() as u64,
        timestamp_count: timestamps.len() as u64,
        timestamps: Some(timestamps),
        validity: Validity::Valid,
        metadata_evidence: None,
    };
    recording.validate()?;
    Ok(recording)
}

/// Compatibility writer for explicitly synthetic fixtures. It cannot invent a real clock epoch.
pub fn write_index(dir: &Path, index: &RecordingIndex) -> Result<()> {
    validate_index(index)?;
    ensure!(
        index.source.to_ascii_lowercase().contains("synthetic"),
        "legacy index-only writer is restricted to synthetic fixtures"
    );
    let timestamps = index
        .timestamps
        .iter()
        .map(|v| (v * 1_000_000.0).floor() as u64)
        .collect();
    let metadata = native_metadata(index, 60, "synthetic-fixture-monotonic".into(), timestamps)?;
    write_sidecars(dir, index, &metadata)
}
fn write_sidecars(dir: &Path, index: &RecordingIndex, metadata: &NativeRecording) -> Result<()> {
    for (name, bytes) in [
        (INDEX_FILE, serde_json::to_vec(index)?),
        (NATIVE_FILE, serde_json::to_vec(metadata)?),
    ] {
        ensure!(
            bytes.len() as u64 <= MAX_METADATA_BYTES,
            "recording metadata capacity exceeded"
        );
        std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(dir.join(name))?
            .write_all(&bytes)?;
    }
    Ok(())
}
pub fn frames_path(dir: &Path) -> PathBuf {
    dir.join(FRAMES_FILE)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CaptureDevice {
    pub name: String,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
}
impl Default for CaptureDevice {
    fn default() -> Self {
        Self {
            name: "Elgato 4K X".into(),
            width: 1920,
            height: 1080,
            fps: 60,
        }
    }
}
#[derive(Debug, Clone)]
pub enum ReferenceSource {
    Direct(CaptureDevice),
    Server(SocketAddr),
}
impl ReferenceSource {
    pub async fn record(&self, seconds: f64, out_dir: &Path) -> Result<RecordingIndex> {
        match self {
            Self::Direct(device) => record_direct(device, seconds, out_dir).await,
            Self::Server(address) => {
                crate::capture_server::record_via_server(*address, seconds, out_dir).await
            }
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct CaptureLimits {
    pub expected_frames: u64,
    pub max_frames: u64,
    pub max_bytes: u64,
    pub io_timeout: Duration,
}
#[derive(Debug, Serialize, Deserialize)]
pub struct CaptureSummary {
    pub expected_frames: u64,
    pub recorded_frames: u64,
    pub missing_from_requested_population: u64,
    pub excess_frames: u64,
    pub source_dropped_frames: Option<u64>,
    pub source_loss_reason: String,
    pub bytes_written: u64,
    pub started_us: u64,
    pub ended_us: u64,
    pub clock: crate::evidence::Clock,
    pub timestamp_boundary: String,
    pub color_authority: String,
}

/// One frame buffer and one fixed writer buffer; disk stalls backpressure the reader.
/// A fresh output reservation owned by the caller rolls back files on error/cancellation.
pub async fn record_sdr_stream(
    mut input: impl AsyncRead + Unpin,
    mode: &CaptureMode,
    source: &str,
    output: &Path,
    limits: CaptureLimits,
) -> Result<RecordingIndex> {
    ensure!(
        mode.width == 1920 && mode.height == 1080 && mode.fps == 60,
        "native SDR capture requires 1920x1080 at 60 fps"
    );
    ensure!(
        limits.expected_frames > 0
            && limits.expected_frames <= limits.max_frames
            && limits.max_frames <= MAX_FRAMES
            && limits.max_bytes > 0
            && limits.max_bytes <= MAX_DISK_BYTES
            && !limits.io_timeout.is_zero(),
        "invalid capture bounds"
    );
    let clock = clock::local_clock()?;
    let started_us = clock::monotonic_us()?;
    let size = usize::try_from(u64::from(mode.width) * u64::from(mode.height) * 3 / 2)?;
    let file = tokio::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(frames_path(output))
        .await?;
    let mut writer = BufWriter::with_capacity(64 * 1024, file);
    let mut frame = vec![0; size];
    let mut timestamps = Vec::with_capacity(limits.max_frames as usize);
    let mut bytes_written = 0u64;
    loop {
        if tokio::time::timeout(limits.io_timeout, input.read(&mut frame[..1])).await?? == 0 {
            break;
        }
        ensure!(
            (timestamps.len() as u64) < limits.max_frames,
            "capture frame limit exceeded"
        );
        tokio::time::timeout(limits.io_timeout, input.read_exact(&mut frame[1..])).await??;
        let observed = clock::monotonic_us()?;
        ensure!(
            timestamps
                .last()
                .is_none_or(|previous| observed > *previous),
            "capture timestamp resolution/discontinuity fault"
        );
        let total = bytes_written
            .checked_add(size as u64)
            .context("capture disk counter overflow")?;
        ensure!(total <= limits.max_bytes, "capture disk budget exhausted");
        tokio::time::timeout(limits.io_timeout, writer.write_all(&frame)).await??;
        timestamps.push(observed);
        bytes_written = total;
    }
    ensure!(
        !timestamps.is_empty(),
        "capture produced no complete frames"
    );
    tokio::time::timeout(limits.io_timeout, writer.flush()).await??;
    let ended_us = clock::monotonic_us()?;
    let index = RecordingIndex {
        source: source.into(),
        width: mode.width as usize,
        height: mode.height as usize,
        timestamps: timestamps
            .iter()
            .map(|time| *time as f64 / 1_000_000.0)
            .collect(),
    };
    let metadata = native_metadata(&index, mode.fps, clock.id.clone(), timestamps)?;
    metadata.validate_files(output)?;
    let count = metadata.frame_count;
    let summary = CaptureSummary { expected_frames: limits.expected_frames, recorded_frames: count,
        missing_from_requested_population: limits.expected_frames.saturating_sub(count),
        excess_frames: count.saturating_sub(limits.expected_frames), source_dropped_frames: None,
        source_loss_reason: "raw device output has no source frame IDs; requested-count deficit is not hardware drop attribution".into(),
        bytes_written, started_us, ended_us, clock,
        timestamp_boundary: "complete raw frame received from local capture input; pipe/scheduling latency uncalibrated".into(),
        color_authority: "configured 8-bit limited-range SDR BT.709 interpretation; HDMI source metadata not independently measured".into() };
    tokio::fs::write(output.join("capture.json"), serde_json::to_vec(&summary)?).await?;
    write_sidecars(output, &index, &metadata)?;
    Ok(index)
}

/// The direct API uses exactly the same reservation/lease/timeout service path as HTTP.
pub async fn record_direct(
    device: &CaptureDevice,
    seconds: f64,
    output: &Path,
) -> Result<RecordingIndex> {
    crate::capture_server::record_local(device, seconds, output).await
}

/// Called only while the capture service owns the device and fresh private output.
pub(crate) async fn record_device(
    device: &CaptureDevice,
    seconds: f64,
    output: &Path,
) -> Result<RecordingIndex> {
    crate::capture_server::validate_seconds(seconds)?;
    ensure!(
        !device.name.is_empty()
            && device.name.len() <= 256
            && device.width == 1920
            && device.height == 1080
            && device.fps == 60,
        "native SDR device mode required"
    );
    let mode = CaptureMode {
        width: device.width,
        height: device.height,
        fps: device.fps,
    };
    let expected = (seconds * f64::from(device.fps)).ceil() as u64;
    let limits = CaptureLimits {
        expected_frames: expected,
        max_frames: expected + 2,
        max_bytes: MAX_DISK_BYTES,
        io_timeout: Duration::from_secs(2),
    };
    let mut command = Command::new("ffmpeg");
    command.args(["-hide_banner", "-loglevel", "error", "-nostdin"]);
    #[cfg(target_os = "macos")]
    command.args([
        "-f",
        "avfoundation",
        "-framerate",
        "60",
        "-video_size",
        "1920x1080",
        "-i",
        &format!("{}:none", device.name),
    ]);
    #[cfg(target_os = "linux")]
    command.args([
        "-f",
        "v4l2",
        "-framerate",
        "60",
        "-video_size",
        "1920x1080",
        "-i",
        &device.name,
    ]);
    command
        .args([
            "-t",
            &seconds.to_string(),
            "-an",
            "-vf",
            "format=yuv420p",
            "-fps_mode",
            "passthrough",
            "-color_range",
            "tv",
            "-colorspace",
            "bt709",
            "-color_primaries",
            "bt709",
            "-color_trc",
            "bt709",
            "-f",
            "rawvideo",
            "pipe:1",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    let mut child = command
        .spawn()
        .context("starting native SDR FFmpeg capture")?;
    let stdout = child.stdout.take().context("capture stdout unavailable")?;
    let mut stderr = child.stderr.take().context("capture stderr unavailable")?;
    let drain_errors = async {
        let mut retained = Vec::new();
        let mut buffer = [0u8; 4096];
        loop {
            let count = stderr.read(&mut buffer).await?;
            if count == 0 {
                break;
            }
            let remaining = 65_536usize.saturating_sub(retained.len());
            retained.extend_from_slice(&buffer[..count.min(remaining)]);
        }
        Ok::<_, anyhow::Error>(retained)
    };
    let record = async {
        let index = record_sdr_stream(
            stdout,
            &mode,
            &format!("hdmi:{}", device.name),
            output,
            limits,
        )
        .await?;
        let status = child.wait().await?;
        ensure!(status.success(), "capture child failed: {status}");
        Ok::<_, anyhow::Error>(index)
    };
    let (index, _errors) = tokio::try_join!(record, drain_errors)?;
    Ok(index)
}

pub use crate::capture_server::{serve, server_output_root};

#[cfg(test)]
mod native_tests {
    use super::*;

    fn scratch() -> PathBuf {
        let path = std::env::temp_dir().join(format!("native-sdr-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&path).unwrap();
        path
    }

    #[tokio::test]
    async fn native_sdr_stream_preserves_pixels_counts_and_monotonic_metadata() {
        let output = scratch();
        let mode = crate::recording::CaptureMode {
            width: 1920,
            height: 1080,
            fps: 60,
        };
        let size = 1920 * 1080 * 3 / 2;
        let mut pixels = vec![16u8; size];
        pixels.extend(vec![128u8; size]);
        let limits = CaptureLimits {
            expected_frames: 3,
            max_frames: 3,
            max_bytes: (size * 3) as u64,
            io_timeout: std::time::Duration::from_secs(2),
        };
        let index = record_sdr_stream(
            std::io::Cursor::new(&pixels),
            &mode,
            "synthetic fake capture; no hardware",
            &output,
            limits,
        )
        .await
        .unwrap();
        assert_eq!(index.timestamps.len(), 2);
        let recording = Recording::load(&output).unwrap();
        assert_eq!(recording.read_frame(0).unwrap(), pixels[..size]);
        assert_eq!(recording.read_frame(1).unwrap(), pixels[size..]);
        assert!(recording.read_frame(2).is_err());
        assert_eq!(recording.metadata.capture_mode, mode);
        assert_eq!(
            recording.metadata.clock_domain,
            crate::clock::local_clock().unwrap().id
        );
        assert_eq!(recording.metadata.timestamps.as_ref().unwrap().len(), 2);
        assert_eq!(recording.metadata.bit_depth, 8);
        let summary: CaptureSummary =
            serde_json::from_slice(&std::fs::read(output.join("capture.json")).unwrap()).unwrap();
        assert_eq!(summary.missing_from_requested_population, 1);
        assert_eq!(summary.source_dropped_frames, None);
        assert!(recording.metadata.calibration_id.is_none());
        std::fs::remove_dir_all(output).unwrap();
    }

    #[tokio::test]
    async fn native_capture_refuses_truncation_disk_overflow_and_stalled_input() {
        let mode = crate::recording::CaptureMode {
            width: 1920,
            height: 1080,
            fps: 60,
        };
        let size = 1920 * 1080 * 3 / 2;
        for (data, max_bytes) in [
            (vec![1; size - 1], size as u64),
            (vec![1; size], size as u64 - 1),
        ] {
            let output = scratch();
            let limits = CaptureLimits {
                expected_frames: 1,
                max_frames: 1,
                max_bytes,
                io_timeout: std::time::Duration::from_secs(1),
            };
            assert!(record_sdr_stream(
                std::io::Cursor::new(data),
                &mode,
                "synthetic",
                &output,
                limits
            )
            .await
            .is_err());
            assert!(!output.join("recording.json").exists());
            std::fs::remove_dir_all(output).unwrap();
        }
        let output = scratch();
        let (input, _held) = tokio::io::duplex(1);
        let limits = CaptureLimits {
            expected_frames: 1,
            max_frames: 1,
            max_bytes: size as u64,
            io_timeout: std::time::Duration::from_millis(20),
        };
        assert!(
            record_sdr_stream(input, &mode, "synthetic", &output, limits)
                .await
                .is_err()
        );
        std::fs::remove_dir_all(output).unwrap();
    }

    #[test]
    fn legacy_loader_rejects_reversed_timestamps() {
        let output = scratch();
        std::fs::write(frames_path(&output), vec![0; 64 * 64 * 3]).unwrap();
        let index = RecordingIndex {
            source: "synthetic".into(),
            width: 64,
            height: 64,
            timestamps: vec![2.0, 1.0],
        };
        // Write raw to exercise the reader even when the writer rejects invalid metadata.
        std::fs::write(output.join(INDEX_FILE), serde_json::to_vec(&index).unwrap()).unwrap();
        assert!(Recording::load(&output).is_err());
        std::fs::remove_dir_all(output).unwrap();
    }
}
