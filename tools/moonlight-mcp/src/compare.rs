//! Native-code-value fidelity and HDMI-relative temporal alignment, version 2.
//!
//! Every stream frame receives a local JSONL record (score or explicit exclusion).
//! Alignment thumbnails and exact percentile runs use disk, bounded working memory.
//! PQ/BT.2020 stays in ten-bit signal space: no tone mapping or eight-bit conversion.
//! An uncalibrated capture delay is never an absolute display-latency measurement.

use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use crate::decode::write_png;
use crate::recording::{ColorSpace, PixelFormat};
use crate::reference::Recording;
use crate::stats::{DiskSummary, Summary};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct CompareOptions {
    pub min_delay_ms: f64,
    pub max_delay_ms: f64,
    /// Motion and blank thresholds use the normalized 0..255 scale for both precisions.
    pub min_motion: f64,
    pub min_confidence: f64,
    pub blank_std_dev: f64,
}
impl Default for CompareOptions {
    fn default() -> Self {
        Self {
            min_delay_ms: -100.0,
            max_delay_ms: 500.0,
            min_motion: 1.0,
            min_confidence: 0.3,
            blank_std_dev: 2.0,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct Alignment {
    pub delay_ms: Option<f64>,
    pub reliable: bool,
    pub reason: Option<String>,
    pub confidence: Option<f64>,
    pub best_cost: Option<f64>,
    pub median_cost: Option<f64>,
    pub reference_motion: f64,
    /// Reference cadence quantization only; not the total measurement uncertainty.
    pub resolution_ms: f64,
    /// Unknown until independently calibrated, including capture/clock/scheduling error.
    pub uncertainty_ms: Option<f64>,
    pub uncertainty_reason: String,
    /// Cost uses the same interior frame population for every candidate delay.
    pub search_frames: usize,
    pub search_excluded_frames: usize,
    pub search_step_ms: f64,
    pub best_plateau_width_ms: f64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Fidelity {
    pub frames_scored: usize,
    pub ssim: Summary,
    /// These summaries cover finite PSNR only. Perfect planes are positive infinity,
    /// counted separately rather than silently clipped to 100 dB or encoded as zero.
    pub psnr_y_db: Option<Summary>,
    pub psnr_u_db: Option<Summary>,
    pub psnr_v_db: Option<Summary>,
    pub perfect_psnr_frames_yuv: [usize; 3],
    pub frames_ssim_at_least_070: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct SamplePair {
    pub label: String,
    pub stream_frame: usize,
    pub reference_frame: usize,
    pub ssim: f64,
    pub stream_png: Option<String>,
    pub reference_png: Option<String>,
    /// Native packed bytes; HDR pairs preserve the input pixel formats and ten bits.
    pub stream_native: String,
    pub reference_native: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct CompareReport {
    pub method: String,
    pub options: CompareOptions,
    pub color_space: ColorSpace,
    pub bit_depth: u8,
    pub pixel_formats: [PixelFormat; 2],
    pub stream_frames: usize,
    pub reference_frames: usize,
    pub overlap_seconds: f64,
    pub blank_stream_frames: usize,
    pub alignment: Alignment,
    pub fidelity: Option<Fidelity>,
    pub excluded_frames: usize,
    pub exclusion_reasons: Vec<String>,
    /// False for partial populations or unreliable moving-scene alignment.
    /// This is comparison validity, not a calibrated acceptance verdict.
    pub fidelity_population_complete: bool,
    pub calibration_ids: [Option<String>; 2],
    pub frame_results: String,
    pub samples: Vec<SamplePair>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FrameResult {
    pub stream_frame: usize,
    pub stream_time_seconds: f64,
    pub reference_frame: Option<usize>,
    pub reference_time_seconds: Option<f64>,
    pub timestamp_residual_ms: Option<f64>,
    pub ssim: Option<f64>,
    /// IEEE-754 binary64 hex bits preserve exact percentile sample identity across
    /// JSON parsers (and avoid integer precision loss in JavaScript consumers).
    pub ssim_bits: Option<String>,
    /// None with perfect_psnr=true means positive infinity; with an exclusion it is absent.
    pub psnr_db_yuv: [Option<f64>; 3],
    pub perfect_psnr_yuv: [bool; 3],
    pub exclusion_reason: Option<String>,
}

/// SSIM variant matches imgcmp's uniform 8x8 / stride-4 population-variance windows.
/// Peak is explicitly 255 (SDR) or 1023 (PQ/BT.2020); sample units stay native.
#[derive(Debug, Clone, Serialize)]
pub struct PlaneScores {
    pub ssim: f64,
    pub psnr_db: Option<f64>,
    pub perfect: bool,
    pub color_space: ColorSpace,
}
pub fn score_plane(
    a: &[u16],
    b: &[u16],
    width: usize,
    height: usize,
    color_space: ColorSpace,
) -> Result<PlaneScores> {
    let peak = if color_space == ColorSpace::PqBt2020 {
        1023.0
    } else {
        255.0
    };
    ensure!(
        width >= 8
            && height >= 8
            && width.checked_mul(height) == Some(a.len())
            && a.len() == b.len(),
        "invalid plane geometry"
    );
    ensure!(
        a.iter().chain(b).all(|&v| f64::from(v) <= peak),
        "out-of-range code value"
    );
    let (c1, c2) = ((0.01_f64 * peak).powi(2), (0.03_f64 * peak).powi(2));
    let (mut total, mut windows) = (0.0, 0);
    for y in (0..=height - 8).step_by(4) {
        for x in (0..=width - 8).step_by(4) {
            let (mut sa, mut sb, mut saa, mut sbb, mut sab) = (0.0, 0.0, 0.0, 0.0, 0.0);
            for row in y..y + 8 {
                for col in x..x + 8 {
                    let (va, vb) = (
                        f64::from(a[row * width + col]),
                        f64::from(b[row * width + col]),
                    );
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                }
            }
            let (ma, mb) = (sa / 64.0, sb / 64.0);
            total += ((2.0 * ma * mb + c1) * (2.0 * (sab / 64.0 - ma * mb) + c2))
                / ((ma * ma + mb * mb + c1) * (saa / 64.0 - ma * ma + sbb / 64.0 - mb * mb + c2));
            windows += 1;
        }
    }
    let mse = a
        .iter()
        .zip(b)
        .map(|(&a, &b)| (f64::from(a) - f64::from(b)).powi(2))
        .sum::<f64>()
        / a.len() as f64;
    Ok(PlaneScores {
        ssim: total / windows as f64,
        psnr_db: (mse > 0.0).then(|| 10.0 * (peak * peak / mse).log10()),
        perfect: mse == 0.0,
        color_space,
    })
}

fn native_planes(rec: &Recording, i: usize) -> Result<[Vec<u16>; 3]> {
    let bytes = rec.read_frame(i)?;
    native_planes_bytes(&rec.metadata, &bytes)
}

pub(crate) fn native_planes_bytes(
    rec: &crate::recording::Recording,
    bytes: &[u8],
) -> Result<[Vec<u16>; 3]> {
    ensure!(
        bytes.len() as u64 == rec.checked_frame_bytes()?,
        "frame/geometry mismatch"
    );
    let y = rec.capture_mode.width as usize * rec.capture_mode.height as usize;
    let codes: Vec<u16> = match rec.pixel_format {
        PixelFormat::Yuv420p => bytes.iter().copied().map(u16::from).collect(),
        PixelFormat::P010le | PixelFormat::Yuv420p10le => {
            let shift = if rec.pixel_format == PixelFormat::P010le {
                6
            } else {
                0
            };
            ensure!(bytes.len().is_multiple_of(2), "truncated ten-bit sample");
            let values: Vec<_> = bytes
                .chunks_exact(2)
                .map(|b| u16::from_le_bytes([b[0], b[1]]))
                .collect();
            ensure!(
                values
                    .iter()
                    .all(|v| if shift == 6 { v & 63 == 0 } else { *v <= 1023 }),
                "invalid ten-bit packing"
            );
            values.into_iter().map(|v| v >> shift).collect()
        }
    };
    ensure!(codes.len() == y * 3 / 2, "frame/geometry mismatch");
    if rec.pixel_format == PixelFormat::P010le {
        Ok([
            codes[..y].to_vec(),
            codes[y..].iter().step_by(2).copied().collect(),
            codes[y + 1..].iter().step_by(2).copied().collect(),
        ])
    } else {
        Ok([
            codes[..y].to_vec(),
            codes[y..y + y / 4].to_vec(),
            codes[y + y / 4..].to_vec(),
        ])
    }
}

fn private_directory(path: &Path) -> Result<()> {
    let mut builder = std::fs::DirBuilder::new();
    builder.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        builder.mode(0o700);
    }
    builder.create(path)?;
    let metadata = std::fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_dir() && !metadata.file_type().is_symlink(),
        "comparison evidence directory cannot be a symlink"
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

fn private_file(path: &Path) -> Result<File> {
    let mut options = OpenOptions::new();
    options.read(true).write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    Ok(options.open(path)?)
}

/// RAII scratch lives only below the configured local temporary destination.
struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Result<Self> {
        let root = std::env::temp_dir().join(format!("moonlight-compare-{}", uuid::Uuid::new_v4()));
        private_directory(&root)?;
        Ok(Self(root))
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if let Err(error) = std::fs::remove_dir_all(&self.0) {
            eprintln!("comparison scratch cleanup failed: {error}");
        }
    }
}

struct Thumbnails {
    file: File,
    pixels: usize,
    motion: f64,
    blank: usize,
}
impl Thumbnails {
    fn build(rec: &Recording, path: &Path, blank_threshold: f64) -> Result<Self> {
        let (w, h) = (rec.index.width, rec.index.height);
        let factor = (w / 120).max(8).min(h).min(w);
        let (tw, th) = (w / factor, h / factor);
        let mut file = private_file(path)?;
        let mut output = BufWriter::new(&mut file);
        let (mut previous, mut motion, mut blank) = (None::<Vec<f64>>, 0.0, 0);
        let scale = if rec.metadata.bit_depth == 10 {
            255.0 / 1023.0
        } else {
            1.0
        };
        for i in 0..rec.frame_count() {
            let planes = native_planes(rec, i)?;
            let mut thumb = Vec::with_capacity(tw * th);
            for y in 0..th {
                for x in 0..tw {
                    let sum: u64 = (0..factor)
                        .map(|dy| {
                            planes[0][(y * factor + dy) * w + x * factor
                                ..(y * factor + dy) * w + (x + 1) * factor]
                                .iter()
                                .map(|v| u64::from(*v))
                                .sum::<u64>()
                        })
                        .sum();
                    let value = sum as f64 / (factor * factor) as f64 * scale;
                    output.write_all(&value.to_le_bytes())?;
                    thumb.push(value);
                }
            }
            let mean = thumb.iter().sum::<f64>() / thumb.len() as f64;
            if (thumb.iter().map(|v| (v - mean).powi(2)).sum::<f64>() / thumb.len() as f64).sqrt()
                < blank_threshold
            {
                blank += 1;
            }
            if let Some(previous) = &previous {
                motion += mad(previous, &thumb);
            }
            previous = Some(thumb);
        }
        output.flush()?;
        drop(output);
        Ok(Self {
            file,
            pixels: tw * th,
            motion: motion / (rec.frame_count() - 1).max(1) as f64,
            blank,
        })
    }
    fn read(&mut self, i: usize) -> Result<Vec<f64>> {
        self.file
            .seek(SeekFrom::Start((i * self.pixels * 8) as u64))?;
        let mut bytes = vec![0; self.pixels * 8];
        self.file.read_exact(&mut bytes)?;
        Ok(bytes
            .chunks_exact(8)
            .map(|v| f64::from_le_bytes(v.try_into().expect("eight bytes")))
            .collect())
    }
}
fn mad(a: &[f64], b: &[f64]) -> f64 {
    a.iter().zip(b).map(|(a, b)| (a - b).abs()).sum::<f64>() / a.len() as f64
}
fn median_interval(times: &[f64]) -> f64 {
    let mut deltas: Vec<_> = times.windows(2).map(|v| v[1] - v[0]).collect();
    deltas.sort_by(f64::total_cmp);
    deltas[deltas.len() / 2]
}
fn nearest(times: &[f64], time: f64) -> Option<usize> {
    if time < times[0] || time > times[times.len() - 1] {
        return None;
    }
    let i = times.partition_point(|&t| t < time);
    Some(if i == 0 {
        0
    } else if i == times.len() || time - times[i - 1] <= times[i] - time {
        i - 1
    } else {
        i
    })
}

fn align(
    stream: &Recording,
    reference: &Recording,
    s: &mut Thumbnails,
    r: &mut Thumbnails,
    opts: &CompareOptions,
) -> Result<Alignment> {
    let (st, rt) = (&stream.index.timestamps, &reference.index.timestamps);
    let interval = median_interval(rt);
    let step_ms = 1.0;
    let candidates = ((opts.max_delay_ms - opts.min_delay_ms) / step_ms).floor() as usize + 1;
    ensure!(
        candidates <= 10001,
        "alignment search exceeds 10001 candidate budget"
    );
    let still = r.motion < opts.min_motion;
    let mut costs = vec![0.0; candidates];
    let mut count = 0;
    for (i, &t) in st.iter().enumerate() {
        // Equal interior populations prevent a short boundary match from winning.
        if t - opts.max_delay_ms / 1000.0 < rt[0]
            || t - opts.min_delay_ms / 1000.0 > rt[rt.len() - 1]
        {
            continue;
        }
        let a = s.read(i)?;
        for (k, cost) in costs.iter_mut().enumerate() {
            let delay = (opts.min_delay_ms + k as f64 * step_ms) / 1000.0;
            let j = nearest(rt, t - delay).context("interior alignment pair missing")?;
            *cost += mad(&a, &r.read(j)?);
        }
        count += 1;
    }
    if count > 0 {
        for cost in &mut costs {
            *cost /= count as f64;
        }
    }
    let best = costs
        .iter()
        .enumerate()
        .min_by(|a, b| a.1.total_cmp(b.1))
        .map(|(i, _)| i)
        .unwrap_or(0);
    let best_cost = costs[best];
    let mut sorted = costs.clone();
    sorted.sort_by(f64::total_cmp);
    let median = sorted[sorted.len() / 2];
    let confidence = if median > 0.0 {
        (1.0 - best_cost / median).clamp(0.0, 1.0)
    } else {
        0.0
    };
    let tolerance = 1e-9_f64.max(best_cost.abs() * 1e-9);
    let tied: Vec<_> = costs
        .iter()
        .enumerate()
        .filter(|(_, c)| (**c - best_cost).abs() <= tolerance)
        .map(|(i, _)| i)
        .collect();
    let left = *tied.first().unwrap_or(&best);
    let right = *tied.last().unwrap_or(&best);
    let plateau = (right - left) as f64 * step_ms;
    let boundary = left == 0 || right + 1 == candidates;
    let ambiguous = plateau > interval * 1000.0 + step_ms;
    let reason = if still {
        Some("still reference; relative delay indeterminate")
    } else if count == 0 {
        Some("no common interior population for the declared delay search")
    } else if boundary {
        Some("alignment optimum touches search boundary")
    } else if ambiguous {
        Some("multiple separated delay optima; scene is temporally ambiguous")
    } else if confidence < opts.min_confidence {
        Some("alignment confidence below configured minimum")
    } else {
        None
    };
    Ok(Alignment {
        delay_ms: (!still && count > 0)
            .then_some(opts.min_delay_ms + (left + right) as f64 / 2.0 * step_ms),
        reliable: reason.is_none(),
        reason: reason.map(str::to_owned),
        confidence: (count > 0).then_some(confidence),
        best_cost: (count > 0).then_some(best_cost),
        median_cost: (count > 0).then_some(median),
        reference_motion: r.motion,
        resolution_ms: interval * 500.0,
        uncertainty_ms: None,
        uncertainty_reason: concat!(
            "capture-chain latency, timestamp delivery/scheduling and clock accuracy are ",
            "uncalibrated; cadence resolution is not an accuracy bound"
        )
        .into(),
        search_frames: count,
        search_excluded_frames: st.len() - count,
        search_step_ms: step_ms,
        best_plateau_width_ms: plateau,
    })
}

pub async fn compare(
    stream: &Recording,
    reference: &Recording,
    opts: &CompareOptions,
    sample_dir: &Path,
) -> Result<CompareReport> {
    stream.metadata.validate()?;
    reference.metadata.validate()?;
    ensure!(
        stream.index.width == reference.index.width
            && stream.index.height == reference.index.height
            && stream.index.width >= 16
            && stream.index.height >= 16,
        "incompatible or too-small native geometry"
    );
    ensure!(
        stream.metadata.bit_depth == reference.metadata.bit_depth
            && stream.metadata.color_space == reference.metadata.color_space,
        "incompatible color interpretation; no SDR/HDR substitution"
    );
    ensure!(
        stream.metadata.clock_domain == reference.metadata.clock_domain,
        "unestablished cross-process clock domain/epoch"
    );
    ensure!(
        reference.frame_count() >= 2,
        "reference needs two temporal observations"
    );
    ensure!(
        [
            opts.min_delay_ms,
            opts.max_delay_ms,
            opts.min_motion,
            opts.min_confidence,
            opts.blank_std_dev
        ]
        .iter()
        .all(|v| v.is_finite())
            && opts.min_delay_ms <= opts.max_delay_ms
            && opts.min_delay_ms >= -60_000.0
            && opts.max_delay_ms <= 60_000.0
            && opts.max_delay_ms - opts.min_delay_ms <= 10_000.0
            && opts.min_motion >= 0.0
            && opts.blank_std_dev >= 0.0
            && (0.0..=1.0).contains(&opts.min_confidence),
        "invalid comparison options"
    );
    private_directory(sample_dir)?;
    let frame_results = sample_dir.join("frame-results.jsonl");
    let file = private_file(&frame_results)?;
    let mut results = BufWriter::new(file);
    let scratch = Scratch::new()?;
    let mut s = Thumbnails::build(stream, &scratch.0.join("stream-luma"), opts.blank_std_dev)?;
    let mut r = Thumbnails::build(
        reference,
        &scratch.0.join("reference-luma"),
        opts.blank_std_dev,
    )?;
    let alignment = align(stream, reference, &mut s, &mut r, opts)?;
    let delay = alignment.delay_ms.unwrap_or(0.0) / 1000.0;
    let interval = median_interval(&reference.index.timestamps);
    let moving_unreliable = !alignment.reliable && alignment.reference_motion >= opts.min_motion;
    let mut distributions = (0..4)
        .map(|_| DiskSummary::new(&scratch.0))
        .collect::<Result<Vec<_>>>()?;
    let (mut scored, mut excluded, mut perfect, mut floor) = (0, 0, [0; 3], 0);
    let (w, h) = (stream.index.width, stream.index.height);
    for (i, &t) in stream.index.timestamps.iter().enumerate() {
        let j = nearest(&reference.index.timestamps, t - delay);
        let residual = j.map(|j| (t - delay - reference.index.timestamps[j]) * 1000.0);
        let reason = if moving_unreliable {
            Some("unreliable_temporal_alignment")
        } else if j.is_none() {
            Some("outside_reference_interval")
        } else if residual.is_some_and(|v| v.abs() > interval * 500.0 + 0.001) {
            Some("reference_timestamp_gap")
        } else {
            None
        };
        let mut result = FrameResult {
            stream_frame: i,
            stream_time_seconds: t,
            reference_frame: j,
            reference_time_seconds: j.map(|j| reference.index.timestamps[j]),
            timestamp_residual_ms: residual,
            ssim: None,
            ssim_bits: None,
            psnr_db_yuv: [None; 3],
            perfect_psnr_yuv: [false; 3],
            exclusion_reason: reason.map(str::to_owned),
        };
        if reason.is_none() {
            let (a, b) = (
                native_planes(stream, i)?,
                native_planes(reference, j.context("paired frame missing")?)?,
            );
            for p in 0..3 {
                let (pw, ph) = if p == 0 { (w, h) } else { (w / 2, h / 2) };
                let score = score_plane(&a[p], &b[p], pw, ph, stream.metadata.color_space)?;
                if p == 0 {
                    result.ssim = Some(score.ssim);
                    result.ssim_bits = Some(format!("{:016x}", score.ssim.to_bits()));
                    distributions[0].push(score.ssim)?;
                    if score.ssim >= 0.70 {
                        floor += 1;
                    }
                }
                result.psnr_db_yuv[p] = score.psnr_db;
                result.perfect_psnr_yuv[p] = score.perfect;
                if let Some(db) = score.psnr_db {
                    distributions[p + 1].push(db)?;
                } else {
                    perfect[p] += 1;
                }
            }
            scored += 1;
        } else {
            excluded += 1;
        }
        serde_json::to_writer(&mut results, &result)?;
        results.write_all(b"\n")?;
    }
    results.flush()?;
    results.get_ref().sync_all()?;
    drop(results);
    let mut summaries = distributions
        .into_iter()
        .map(DiskSummary::finish)
        .collect::<Result<Vec<_>>>()?
        .into_iter();
    let fidelity = summaries.next().flatten().map(|ssim| Fidelity {
        frames_scored: scored,
        ssim,
        psnr_y_db: summaries.next().flatten(),
        psnr_u_db: summaries.next().flatten(),
        psnr_v_db: summaries.next().flatten(),
        perfect_psnr_frames_yuv: perfect,
        frames_ssim_at_least_070: floor,
    });
    let mut samples = Vec::new();
    if let Some(fidelity) = &fidelity {
        for (label, score) in [
            ("worst", fidelity.ssim.min),
            ("median", fidelity.ssim.p50),
            ("best", fidelity.ssim.max),
        ] {
            let score_bits = format!("{:016x}", score.to_bits());
            for line in BufReader::new(File::open(&frame_results)?).lines() {
                let frame: FrameResult = serde_json::from_str(&line?)?;
                if frame.ssim_bits.as_deref() != Some(score_bits.as_str()) {
                    continue;
                }
                let i = frame.stream_frame;
                let j = frame.reference_frame.context("sample pair missing")?;
                let a = stream.read_frame(i)?;
                let b = reference.read_frame(j)?;
                let stream_native = sample_dir.join(format!("{label}-stream.raw"));
                let reference_native = sample_dir.join(format!("{label}-reference.raw"));
                private_file(&stream_native)?.write_all(&a)?;
                private_file(&reference_native)?.write_all(&b)?;
                let (mut stream_png, mut reference_png) = (None, None);
                if stream.metadata.bit_depth == 8 {
                    let sp = sample_dir.join(format!("{label}-stream.png"));
                    let rp = sample_dir.join(format!("{label}-reference.png"));
                    // Reserve regular private files before FFmpeg opens them with -y.
                    drop(private_file(&sp)?);
                    drop(private_file(&rp)?);
                    write_png(&a, w, h, &sp).await?;
                    write_png(&b, w, h, &rp).await?;
                    stream_png = Some(sp.display().to_string());
                    reference_png = Some(rp.display().to_string());
                }
                samples.push(SamplePair {
                    label: label.into(),
                    stream_frame: i,
                    reference_frame: j,
                    ssim: score,
                    stream_png,
                    reference_png,
                    stream_native: stream_native.display().to_string(),
                    reference_native: reference_native.display().to_string(),
                });
                break;
            }
        }
        ensure!(
            samples.len() == 3,
            "required percentile sample pair missing from local frame results"
        );
    }
    let st = &stream.index.timestamps;
    let rt = &reference.index.timestamps;
    let overlap_seconds = (st[st.len() - 1].min(rt[rt.len() - 1]) - st[0].max(rt[0])).max(0.0);
    let mut exclusion_reasons = Vec::new();
    if excluded > 0 {
        exclusion_reasons
            .push("see every excluded stream frame and reason in frame-results.jsonl".into());
    }
    Ok(CompareReport {method:"native-code-values-v2: uniform-8x8-stride4 SSIM; exact finite PSNR; 1ms MAD alignment on disk thumbnails".into(),
        options:opts.clone(),color_space:stream.metadata.color_space,bit_depth:stream.metadata.bit_depth,
        pixel_formats:[stream.metadata.pixel_format,reference.metadata.pixel_format],stream_frames:stream.frame_count(),reference_frames:reference.frame_count(),
        overlap_seconds,blank_stream_frames:s.blank,alignment,fidelity,excluded_frames:excluded,exclusion_reasons,
        fidelity_population_complete:excluded==0,calibration_ids:[stream.metadata.calibration_id.clone(),reference.metadata.calibration_id.clone()],
        frame_results:frame_results.display().to_string(),samples})
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn nearest_picks_closest_and_rejects_outside() {
        let times = [0.0, 1.0, 2.0];
        assert_eq!(nearest(&times, 0.4), Some(0));
        assert_eq!(nearest(&times, 0.6), Some(1));
        assert_eq!(nearest(&times, 2.0), Some(2));
        assert_eq!(nearest(&times, -0.1), None);
        assert_eq!(nearest(&times, 2.1), None);
    }
    #[test]
    fn ten_bit_peak_and_perfect_psnr_are_explicit() {
        let a = vec![512; 64];
        let b = vec![513; 64];
        let identical = score_plane(&a, &a, 8, 8, ColorSpace::PqBt2020).unwrap();
        assert_eq!(identical.ssim, 1.0);
        assert!(identical.perfect);
        assert!(identical.psnr_db.is_none());
        let changed = score_plane(&a, &b, 8, 8, ColorSpace::PqBt2020).unwrap();
        assert!(changed.ssim < 1.0);
        assert!((changed.psnr_db.unwrap() - 10.0 * 1023.0_f64.powi(2).log10()).abs() < 1e-9);
        assert!(score_plane(&a, &b, 8, 8, ColorSpace::Bt709).is_err());
    }

    fn fixture(root: &Path, name: &str, times: &[f64], hdr: bool, code: u16) -> Recording {
        let dir = root.join(name);
        std::fs::create_dir(&dir).unwrap();
        crate::reference::write_index(
            &dir,
            &crate::reference::RecordingIndex {
                source: "synthetic comparison fixture".into(),
                width: 16,
                height: 16,
                timestamps: times.to_vec(),
            },
        )
        .unwrap();
        let mut metadata: crate::recording::Recording =
            serde_json::from_slice(&std::fs::read(dir.join("recording.json")).unwrap()).unwrap();
        let mut frame = vec![128_u8; 16 * 16 * 3 / 2];
        frame[..16 * 16].fill(code as u8);
        if hdr {
            metadata.bit_depth = 10;
            metadata.color_space = ColorSpace::PqBt2020;
            metadata.pixel_format = PixelFormat::P010le;
            frame = (0..16 * 16 * 3 / 2)
                .flat_map(|i| (if i < 256 { code } else { 512 } << 6).to_le_bytes())
                .collect();
        }
        let mut output = File::create(dir.join("frames.yuv")).unwrap();
        for _ in times {
            output.write_all(&frame).unwrap();
        }
        std::fs::write(
            dir.join("recording.json"),
            serde_json::to_vec(&metadata).unwrap(),
        )
        .unwrap();
        Recording::load(&dir).unwrap()
    }

    #[tokio::test]
    async fn hdr_disk_comparison_keeps_low_bits_population_and_native_pairs() {
        let scratch = Scratch::new().unwrap();
        let times: Vec<_> = (0..40).map(|i| 100.0 + f64::from(i) / 60.0).collect();
        let stream = fixture(&scratch.0, "stream", &times, true, 513);
        let reference = fixture(&scratch.0, "reference", &times, true, 512);
        let report = compare(
            &stream,
            &reference,
            &CompareOptions::default(),
            &scratch.0.join("samples"),
        )
        .await
        .unwrap();
        assert!(report.fidelity_population_complete);
        assert_eq!(
            report.samples.len(),
            3,
            "worst/median/best native pairs required"
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(scratch.0.join("samples"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o700
            );
            assert_eq!(
                std::fs::metadata(&report.frame_results)
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
            assert_eq!(
                std::fs::metadata(&report.samples[0].stream_native)
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
        assert_eq!(report.excluded_frames, 0);
        let fidelity = report.fidelity.unwrap();
        assert_eq!(fidelity.frames_scored, 40);
        assert_eq!(fidelity.ssim.count, 40);
        assert!(fidelity.ssim.max < 1.0);
        assert!((fidelity.psnr_y_db.unwrap().min - 10.0 * 1023.0_f64.powi(2).log10()).abs() < 1e-9);
        assert_eq!(fidelity.perfect_psnr_frames_yuv, [0, 40, 40]);
        assert!(fidelity.psnr_u_db.is_none());
        assert!(report.alignment.delay_ms.is_none());
        assert!(report.alignment.uncertainty_ms.is_none());
        let records: Vec<FrameResult> = BufReader::new(File::open(&report.frame_results).unwrap())
            .lines()
            .map(|l| serde_json::from_str(&l.unwrap()).unwrap())
            .collect();
        assert_eq!(records.len(), 40);
        assert!(records
            .iter()
            .all(|r| r.psnr_db_yuv[0].is_some() && r.perfect_psnr_yuv[1]));
        for pair in report.samples {
            assert!(pair.stream_png.is_none());
            assert_eq!(
                std::fs::read(pair.stream_native).unwrap(),
                stream.read_frame(pair.stream_frame).unwrap()
            );
        }
    }

    #[tokio::test]
    async fn unmatched_and_gap_frames_are_recorded_not_silently_removed() {
        let scratch = Scratch::new().unwrap();
        let rt = [
            100.0,
            100.0 + 1.0 / 60.0,
            100.0 + 10.0 / 60.0,
            100.0 + 11.0 / 60.0,
        ];
        let st = [99.0, 100.0, 100.0 + 5.0 / 60.0, 100.0 + 10.0 / 60.0, 102.0];
        let stream = fixture(&scratch.0, "stream", &st, false, 32);
        let reference = fixture(&scratch.0, "reference", &rt, false, 32);
        let options = CompareOptions {
            min_delay_ms: 0.0,
            max_delay_ms: 0.0,
            ..Default::default()
        };
        let report = compare(&stream, &reference, &options, &scratch.0.join("samples"))
            .await
            .unwrap();
        assert_eq!(report.excluded_frames, 3);
        assert!(!report.fidelity_population_complete);
        assert_eq!(report.fidelity.unwrap().frames_scored, 2);
        let records: Vec<FrameResult> = BufReader::new(File::open(report.frame_results).unwrap())
            .lines()
            .map(|l| serde_json::from_str(&l.unwrap()).unwrap())
            .collect();
        assert_eq!(records.len(), 5);
        assert_eq!(
            records[0].exclusion_reason.as_deref(),
            Some("outside_reference_interval")
        );
        assert_eq!(
            records[2].exclusion_reason.as_deref(),
            Some("reference_timestamp_gap")
        );
    }

    #[tokio::test]
    async fn unrelated_clocks_color_and_invalid_search_are_rejected_before_outputs() {
        let scratch = Scratch::new().unwrap();
        let times = [100.0, 101.0];
        let mut stream = fixture(&scratch.0, "stream", &times, false, 32);
        let reference = fixture(&scratch.0, "reference", &times, false, 32);
        stream.metadata.clock_domain = "different-boot-epoch".into();
        assert!(compare(
            &stream,
            &reference,
            &CompareOptions::default(),
            &scratch.0.join("bad-clock")
        )
        .await
        .is_err());
        stream.metadata.clock_domain = reference.metadata.clock_domain.clone();
        for maximum in [f64::NAN, f64::MAX, -200.0] {
            let options = CompareOptions {
                max_delay_ms: maximum,
                ..Default::default()
            };
            assert!(
                compare(&stream, &reference, &options, &scratch.0.join("bad-range"))
                    .await
                    .is_err()
            );
        }
        let hdr = fixture(&scratch.0, "hdr", &times, true, 512);
        assert!(compare(
            &stream,
            &hdr,
            &CompareOptions::default(),
            &scratch.0.join("bad-color")
        )
        .await
        .is_err());
        assert!(!scratch.0.join("bad-range").exists());
    }

    #[tokio::test]
    async fn empty_alignment_search_reports_absence_instead_of_zero_cost() {
        let scratch = Scratch::new().unwrap();
        let times = [100.0, 100.1];
        let stream = fixture(&scratch.0, "stream", &times, false, 32);
        let reference = fixture(&scratch.0, "reference", &times, false, 32);
        let report = compare(
            &stream,
            &reference,
            &CompareOptions::default(),
            &scratch.0.join("samples"),
        )
        .await
        .unwrap();
        assert_eq!(report.alignment.search_frames, 0);
        assert!(report.alignment.best_cost.is_none());
        assert!(report.alignment.median_cost.is_none());
        assert!(report.alignment.confidence.is_none());
        assert_eq!(report.fidelity.unwrap().frames_scored, 2);
    }
}
