//! Offline capture-cadence analysis over already-recorded frame timestamps.
//!
//! This module never opens a device, never contacts a console or any network peer, and never
//! reads the frame pictures. It loads the existing capture sidecars from a capture output
//! directory and reports what the recorded timestamps show.
//!
//! It exists so cadence review is repeatable and reviewable instead of an ad-hoc script, and so
//! a probe run that degrades the captured output cadence is visible immediately. It is a
//! diagnostic, not acceptance evidence: a timestamp marks a complete UVC frame assembled before
//! pipe output, which is not HDMI exposure, not console presentation/PTS, and not calibrated
//! optical latency. USB carries no HDMI-source frame counter, so whole source frames absent from
//! USB are unobservable; a cadence gap can never be attributed to the console rather than the
//! USB/host capture path, and is never a SunshinePS5 host drop.

use anyhow::{ensure, Context, Result};
use serde::Serialize;
use std::path::Path;

use crate::evidence::Validity;
use crate::hdr_capture::HdrSummary;
use crate::recording::Recording;
use crate::reference::RecordingIndex;
use crate::stats::Summary;

/// Which metadata established the nominal frame interval.
#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum NominalSource {
    /// `capture-summary.json` `descriptor_interval_100ns`, the negotiated USB frame interval.
    HdrDescriptorInterval,
    /// `recording.json` `capture_mode.fps`.
    RecordingCaptureModeFps,
    /// Operator-supplied override.
    CliOverride,
}

#[derive(Debug, Clone, Serialize)]
pub struct NominalCandidate {
    pub source: NominalSource,
    pub interval_ms: f64,
    pub fps: f64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Nominal {
    pub source: NominalSource,
    pub interval_ms: f64,
    pub fps: f64,
    /// Every nominal rate the inputs offered, so a displaced candidate stays visible.
    pub candidates: Vec<NominalCandidate>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Population {
    pub frames: usize,
    pub intervals: usize,
    pub span_ms: f64,
    pub effective_fps: f64,
    pub effective_fps_basis: String,
    pub interval_ms: Summary,
}

#[derive(Debug, Clone, Serialize)]
pub struct Gap {
    pub interval_index: usize,
    pub from_frame: usize,
    pub to_frame: usize,
    pub duration_ms: f64,
    pub nominal_intervals: f64,
    pub implied_missing_frames: u64,
    pub attribution: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct SteadyState {
    pub interval_ms: Summary,
    pub max_abs_deviation_from_nominal_ms: f64,
    pub effective_fps: f64,
    /// True when the selected population's rate sits above the nominal rate, which selection
    /// alone can produce. Not a sign the source ran fast.
    pub exceeds_nominal: bool,
    pub basis: String,
}

#[derive(Debug, Clone)]
pub struct StartupExclusion {
    pub frames: usize,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct ExclusionReport {
    pub declared_startup_frames: usize,
    pub reason: String,
    pub excluded_frames: usize,
    pub excluded_intervals: usize,
    pub retained: Population,
    pub retained_gaps: Vec<Gap>,
    pub retained_gaps_omitted: usize,
    pub retained_implied_missing_frames: u64,
    pub retained_steady_state: Option<SteadyState>,
    pub note: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct HdrCrossCheck {
    pub descriptor_interval_100ns: u32,
    pub expected_frames: u64,
    pub summary_recorded_frames: u64,
    pub timestamp_count: u64,
    pub recorded_frames_agree: bool,
    /// True when the capture recorded at least the frame count it requested. Implied-missing
    /// frames can be nonzero while this is true: an arrival gap is not a recording shortfall.
    pub met_requested_frame_budget: bool,
    pub observed_usb_frames: u64,
    pub rejected_usb_frames: u64,
    pub implied_missing_frames: u64,
    pub rejected_matches_implied_missing: bool,
    pub ownership_restored: bool,
    pub usb_loss_reason: String,
    pub note: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct CadenceReport {
    pub kind: String,
    pub acceptance: String,
    pub capture_dir: String,
    pub source: String,
    pub width: usize,
    pub height: usize,
    pub recording_validity: Validity,
    pub clock_domain: String,
    pub timestamp_boundary: String,
    pub nominal: Nominal,
    pub gap_threshold_nominal: f64,
    pub population: Population,
    pub gaps: Vec<Gap>,
    pub gaps_omitted: usize,
    pub total_implied_missing_frames: u64,
    pub implied_missing_basis: String,
    pub steady_state: Option<SteadyState>,
    pub startup_exclusion: Option<ExclusionReport>,
    pub hdr_cross_check: Option<HdrCrossCheck>,
    pub limitations: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct CadenceOptions {
    /// Classify an interval as a gap when it strictly exceeds this multiple of nominal.
    pub gap_threshold_nominal: f64,
    pub nominal_fps_override: Option<f64>,
    pub declared_startup_exclusion: Option<StartupExclusion>,
}

impl Default for CadenceOptions {
    fn default() -> Self {
        Self {
            gap_threshold_nominal: 1.5,
            nominal_fps_override: None,
            declared_startup_exclusion: None,
        }
    }
}

/// Reported gaps are bounded; the counts and totals always cover every classified gap.
const MAX_GAPS_REPORTED: usize = 512;

const TIMESTAMP_BOUNDARY: &str = "Each timestamp marks one complete capture frame assembled \
     before pipe output. It is not HDMI exposure time, not console presentation/PTS, and not \
     calibrated optical latency.";

/// Counts nominal arrival slots that hold no recorded frame, summed over gaps. It is NOT a
/// count of frames lost from the recording: a capture can record every frame it requested and
/// still show arrival gaps, so read it beside `hdr_cross_check.met_requested_frame_budget`.
const IMPLIED_MISSING_BASIS: &str = "Nominal arrival slots holding no recorded frame, summed \
    over classified gaps. This is not a count of frames lost from the recording: a capture that \
    recorded its full requested frame budget can still show arrival gaps. Whether such frames \
    were never emitted by the source, never reached USB, or were rejected on this host is not \
    decidable from timestamps.";

const GAP_ATTRIBUTION: &str = "Unattributable. USB provides no HDMI-source frame counter, so \
     whole source frames absent from USB are unobservable. This gap cannot be attributed to the \
     console rather than the USB/host capture path, and is not a SunshinePS5 host drop.";

fn limitations() -> Vec<String> {
    [
        TIMESTAMP_BOUNDARY,
        "USB provides no HDMI-source frame counter. Whole source frames absent from USB are \
         unobservable, so no gap reported here can be attributed to the console rather than the \
         USB/host capture path, and no gap is a SunshinePS5 host drop.",
        "This is a diagnostic/research report. It is not acceptance evidence, it claims no \
         SC-003, SC-004, SC-012, SC-013 or SC-014, and it emits no Pass/Fail acceptance verdict.",
        "This analysis sees recorded capture cadence only. It cannot measure input-to-display \
         latency, on-console resource contention, encoder or network behaviour, or image \
         fidelity.",
        "A short capture cannot characterize a session. Read the reported frame count and span \
         before generalizing from these figures.",
        "Agreement between an implied missing-frame count and a recorded USB reject count is \
         consistency, not causation. Neither establishes where a frame was lost.",
        "A declared startup exclusion is reported beside the full population; excluded frames \
         and intervals are declared with a reason and never silently dropped.",
        "Recording validity and calibration status are carried through from the capture \
         metadata. This analysis neither requires nor confers either of them.",
        "Percentiles use the crate's nearest-rank convention, so every reported percentile is a \
         value that was actually observed.",
    ]
    .iter()
    .map(|text| (*text).to_string())
    .collect()
}

struct Loaded {
    index: RecordingIndex,
    native: Recording,
    summary: Option<HdrSummary>,
}

/// Loads the existing capture sidecars. Frame pictures are never opened or read.
///
/// Deliberately calls neither `Recording::validate` nor `Recording::validate_files`: a capture
/// whose metadata was later marked invalid (for example because its source colour context was
/// withdrawn) still carries real transport timestamps worth analyzing, and the frame pictures are
/// never opened. The checks this analysis does need are performed inline below: index/metadata
/// agreement, the seconds timestamps' finiteness and strict monotonicity (`validate_timestamps`),
/// and agreement between the seconds and native-microsecond sidecars, which together establish
/// the native-microsecond ordering this report relies on. The recording's own validity is
/// reported as-is.
fn load(dir: &Path) -> Result<Loaded> {
    let index: RecordingIndex =
        crate::reference::read_metadata(&dir.join(crate::reference::INDEX_FILE))
            .with_context(|| format!("reading frame index in {}", dir.display()))?;
    validate_timestamps(&index.timestamps)?;
    let native: Recording =
        crate::reference::read_metadata(&dir.join(crate::reference::NATIVE_FILE)).context(
            "native monotonic recording metadata required; legacy wall-clock captures must be \
             reacquired",
        )?;
    // The same index/metadata agreement the recording loader enforces, minus the frame-file and
    // validity requirements this read-only analysis does not need.
    ensure!(
        native.capture_mode.width as usize == index.width
            && native.capture_mode.height as usize == index.height
            && native.source == index.source
            && native.frame_count == index.timestamps.len() as u64
            && native.timestamp_count == index.timestamps.len() as u64,
        "recording index/native metadata mismatch"
    );
    let micros = native
        .timestamps
        .as_ref()
        .context("native timestamps missing")?;
    ensure!(
        micros.len() == index.timestamps.len(),
        "timestamp index count mismatch"
    );
    ensure!(
        micros
            .iter()
            .zip(&index.timestamps)
            .all(|(us, sec)| (*us as f64 / 1_000_000.0 - sec).abs()
                <= 0.000_001 + f64::EPSILON * sec.abs()),
        "timestamp sidecars disagree"
    );
    let summary_path = dir.join(crate::hdr_capture::SUMMARY_FILE);
    let summary = match std::fs::symlink_metadata(&summary_path) {
        Ok(_) => Some(
            crate::reference::read_metadata(&summary_path)
                .context("reading capture summary metadata")?,
        ),
        Err(_) => None,
    };
    Ok(Loaded {
        index,
        native,
        summary,
    })
}

fn validate_timestamps(timestamps: &[f64]) -> Result<()> {
    ensure!(
        timestamps.len() >= 2,
        "cadence analysis requires at least two frame timestamps; this capture has {}",
        timestamps.len()
    );
    ensure!(
        timestamps.len() as u64 <= crate::reference::MAX_FRAMES,
        "recorded frame population exceeds the bounded reader limit"
    );
    for (i, value) in timestamps.iter().enumerate() {
        ensure!(
            value.is_finite() && *value >= 0.0 && *value < u64::MAX as f64 / 1_000_000.0,
            "non-finite or negative frame timestamp at index {i}: {value}"
        );
    }
    for (i, pair) in timestamps.windows(2).enumerate() {
        ensure!(
            pair[1] != pair[0],
            "duplicate frame timestamp at indices {i} and {}: {}",
            i + 1,
            pair[0]
        );
        ensure!(
            pair[1] > pair[0],
            "frame timestamps must strictly increase: index {} ({}) precedes index {i} ({})",
            i + 1,
            pair[1],
            pair[0]
        );
    }
    Ok(())
}

fn nominal(loaded: &Loaded, override_fps: Option<f64>) -> Result<Nominal> {
    let mut candidates = Vec::new();
    if let Some(summary) = &loaded.summary {
        let ticks = summary.descriptor_interval_100ns;
        ensure!(
            ticks > 0,
            "capture summary descriptor interval must be positive"
        );
        // 100 ns units: one tick is 1e-4 ms.
        let interval_ms = f64::from(ticks) * 1e-4;
        candidates.push(NominalCandidate {
            source: NominalSource::HdrDescriptorInterval,
            interval_ms,
            fps: 1000.0 / interval_ms,
        });
    }
    let fps = loaded.native.capture_mode.fps;
    ensure!(fps > 0, "recording capture mode declares no frame rate");
    candidates.push(NominalCandidate {
        source: NominalSource::RecordingCaptureModeFps,
        interval_ms: 1000.0 / f64::from(fps),
        fps: f64::from(fps),
    });
    if let Some(fps) = override_fps {
        ensure!(
            fps.is_finite() && fps > 0.0 && fps <= 1000.0,
            "nominal fps override must be finite and within (0, 1000]; got {fps}"
        );
        candidates.push(NominalCandidate {
            source: NominalSource::CliOverride,
            interval_ms: 1000.0 / fps,
            fps,
        });
    }
    // An explicit operator override wins when supplied. Otherwise the negotiated USB descriptor
    // interval wins over the recording's declared capture mode, because it is the rate the
    // capture path actually agreed to. Displaced candidates stay in the report.
    let selected = if override_fps.is_some() {
        NominalSource::CliOverride
    } else if loaded.summary.is_some() {
        NominalSource::HdrDescriptorInterval
    } else {
        NominalSource::RecordingCaptureModeFps
    };
    let chosen = candidates
        .iter()
        .find(|candidate| candidate.source == selected)
        .context("no nominal frame interval could be established")?;
    Ok(Nominal {
        source: chosen.source,
        interval_ms: chosen.interval_ms,
        fps: chosen.fps,
        candidates,
    })
}

fn intervals_ms(timestamps: &[f64]) -> Vec<f64> {
    timestamps
        .windows(2)
        .map(|pair| (pair[1] - pair[0]) * 1000.0)
        .collect()
}

fn population_of(timestamps: &[f64], intervals: &[f64]) -> Result<Population> {
    let span_ms = (timestamps[timestamps.len() - 1] - timestamps[0]) * 1000.0;
    ensure!(
        span_ms.is_finite() && span_ms > 0.0,
        "recorded capture span is not positive"
    );
    Ok(Population {
        frames: timestamps.len(),
        intervals: intervals.len(),
        span_ms,
        effective_fps: intervals.len() as f64 / (span_ms / 1000.0),
        effective_fps_basis: "Recorded intervals divided by observed span: (frames - 1) / span. \
                              frames / span would overstate a short capture."
            .into(),
        interval_ms: Summary::of(intervals).context("interval distribution unavailable")?,
    })
}

/// Splits intervals into classified gaps and the remaining steady-state intervals.
///
/// `frame_offset` keeps frame and interval indices in the original recorded numbering even when
/// a declared startup window has been removed from the analyzed slice.
fn classify(
    intervals: &[f64],
    nominal_ms: f64,
    threshold: f64,
    frame_offset: usize,
) -> (Vec<Gap>, u64, Vec<f64>) {
    let mut gaps = Vec::new();
    let mut steady = Vec::with_capacity(intervals.len());
    let mut implied_total = 0_u64;
    for (i, duration_ms) in intervals.iter().copied().enumerate() {
        if duration_ms <= threshold * nominal_ms {
            steady.push(duration_ms);
            continue;
        }
        let nominal_intervals = duration_ms / nominal_ms;
        // Implied missing frames are derived, not guessed. Two consecutive recorded frames whose
        // timestamps are `d` apart occupy k = round(d / nominal) nominal frame periods, clamped
        // to at least one. A back-to-back pair has k == 1 with nothing between it, so the number
        // of nominal frame slots holding no recorded frame is exactly k - 1. Rounding to nearest
        // rather than truncating charges sub-period timing noise to jitter instead of inventing
        // a fractional frame: 99.774 ms against a 16.6666 ms nominal is 5.9865 periods, so k = 6
        // and 5 frames are implied missing. Whether those frames were never emitted by the
        // source, never reached USB, or were rejected on this host is not decidable from
        // timestamps.
        let periods = nominal_intervals.round().max(1.0);
        let implied = (periods - 1.0) as u64;
        implied_total = implied_total.saturating_add(implied);
        gaps.push(Gap {
            interval_index: frame_offset + i,
            from_frame: frame_offset + i,
            to_frame: frame_offset + i + 1,
            duration_ms,
            nominal_intervals,
            implied_missing_frames: implied,
            attribution: GAP_ATTRIBUTION.into(),
        });
    }
    (gaps, implied_total, steady)
}

fn steady_state(intervals: &[f64], nominal_ms: f64) -> Option<SteadyState> {
    let interval_ms = Summary::of(intervals)?;
    Some(SteadyState {
        interval_ms,
        max_abs_deviation_from_nominal_ms: intervals
            .iter()
            .map(|value| (value - nominal_ms).abs())
            .fold(0.0, f64::max),
        effective_fps: 1000.0 / interval_ms.mean,
        exceeds_nominal: 1000.0 / interval_ms.mean > 1000.0 / nominal_ms,
        basis: "Non-gap intervals only: intervals at or below the declared gap threshold. \
                effective_fps is 1000 / mean non-gap interval and describes cadence between \
                consecutively recorded frames, not sustained session throughput. This figure \
                is selection-biased by construction: the intervals that exceeded the threshold \
                were removed to form this population, so it cannot fall far below nominal and \
                may exceed it. Read it only against the full-population figures."
            .into(),
    })
}

/// Analyzes the capture output directory `dir` offline. Reads only the JSON sidecars.
pub fn analyze(dir: &Path, options: &CadenceOptions) -> Result<CadenceReport> {
    let threshold = options.gap_threshold_nominal;
    ensure!(
        threshold.is_finite() && threshold > 1.0 && threshold <= 1000.0,
        "gap threshold must be finite and within (1, 1000] multiples of nominal; got {threshold}"
    );
    let loaded = load(dir)?;
    let nominal = nominal(&loaded, options.nominal_fps_override)?;
    let nominal_ms = nominal.interval_ms;
    ensure!(
        nominal_ms.is_finite() && nominal_ms > 0.0,
        "nominal frame interval is not positive"
    );
    let timestamps = &loaded.index.timestamps;
    let intervals = intervals_ms(timestamps);
    let population = population_of(timestamps, &intervals)?;
    let (mut gaps, total_implied_missing_frames, steady) =
        classify(&intervals, nominal_ms, threshold, 0);
    let gaps_omitted = gaps.len().saturating_sub(MAX_GAPS_REPORTED);
    gaps.truncate(MAX_GAPS_REPORTED);

    let startup_exclusion = match &options.declared_startup_exclusion {
        None => None,
        Some(declared) => {
            crate::evidence::validate_reason(&declared.reason)
                .context("a declared startup exclusion requires a stated reason")?;
            ensure!(
                declared.frames >= 1,
                "a declared startup exclusion must exclude at least one frame"
            );
            ensure!(
                declared.frames + 2 <= timestamps.len(),
                "declared startup exclusion of {} frames leaves fewer than two of {} recorded \
                 frames",
                declared.frames,
                timestamps.len()
            );
            let retained_timestamps = &timestamps[declared.frames..];
            let retained_intervals = intervals_ms(retained_timestamps);
            let retained = population_of(retained_timestamps, &retained_intervals)?;
            let (mut retained_gaps, retained_implied_missing_frames, retained_steady) =
                classify(&retained_intervals, nominal_ms, threshold, declared.frames);
            let retained_gaps_omitted = retained_gaps.len().saturating_sub(MAX_GAPS_REPORTED);
            retained_gaps.truncate(MAX_GAPS_REPORTED);
            Some(ExclusionReport {
                declared_startup_frames: declared.frames,
                reason: declared.reason.clone(),
                excluded_frames: declared.frames,
                excluded_intervals: intervals.len() - retained_intervals.len(),
                retained,
                retained_gaps,
                retained_gaps_omitted,
                retained_implied_missing_frames,
                retained_steady_state: steady_state(&retained_steady, nominal_ms),
                note: "Declared exclusion only. The full-population figures in this report are \
                       unchanged and remain the primary figures."
                    .into(),
            })
        }
    };

    let hdr_cross_check = loaded.summary.as_ref().map(|summary| {
        let timestamp_count = timestamps.len() as u64;
        HdrCrossCheck {
            descriptor_interval_100ns: summary.descriptor_interval_100ns,
            expected_frames: summary.expected_frames,
            summary_recorded_frames: summary.recorded_frames,
            timestamp_count,
            recorded_frames_agree: summary.recorded_frames == timestamp_count,
            met_requested_frame_budget: summary.recorded_frames >= summary.expected_frames,
            observed_usb_frames: summary.observed_usb_frames,
            rejected_usb_frames: summary.rejected_usb_frames,
            implied_missing_frames: total_implied_missing_frames,
            rejected_matches_implied_missing: summary.rejected_usb_frames
                == total_implied_missing_frames,
            ownership_restored: summary.ownership_restored,
            usb_loss_reason: summary.usb_loss_reason.clone(),
            note: "Cross-check only: equal counts are consistency, not causation, and a \
                   disagreement is reported rather than reconciled. Rejected USB frames are \
                   observed malformed/short/error USB frames; source frames that never produced \
                   a USB boundary remain unobservable."
                .into(),
        }
    });

    Ok(CadenceReport {
        kind: "capture-cadence-diagnostic".into(),
        acceptance: "none: diagnostic-only output. This report claims no SC-003, SC-004, SC-012, \
                     SC-013 or SC-014, emits no acceptance verdict, and is not acceptance \
                     evidence."
            .into(),
        capture_dir: dir.display().to_string(),
        source: loaded.index.source.clone(),
        width: loaded.index.width,
        height: loaded.index.height,
        recording_validity: loaded.native.validity,
        clock_domain: loaded.native.clock_domain.clone(),
        timestamp_boundary: TIMESTAMP_BOUNDARY.into(),
        nominal,
        gap_threshold_nominal: threshold,
        population,
        gaps,
        gaps_omitted,
        total_implied_missing_frames,
        implied_missing_basis: IMPLIED_MISSING_BASIS.into(),
        steady_state: steady_state(&steady, nominal_ms),
        startup_exclusion,
        hdr_cross_check,
        limitations: limitations(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::evidence::{Clock, Validity};
    use crate::hdr_capture::HdrSource;
    use crate::recording::{CaptureMode, ColorSpace, PixelFormat};
    use std::path::PathBuf;

    const SOURCE: &str = "synthetic cadence fixture; no device, console or network involved";
    const NOMINAL_60: f64 = 1.0 / 60.0;

    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!("cadence-{}", uuid::Uuid::new_v4()));
            std::fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn index_of(timestamps: Vec<f64>) -> RecordingIndex {
        RecordingIndex {
            source: SOURCE.into(),
            width: 1920,
            height: 1080,
            timestamps,
        }
    }

    fn native_of(index: &RecordingIndex, fps: u32) -> Recording {
        let micros: Vec<u64> = index
            .timestamps
            .iter()
            .map(|v| (v * 1_000_000.0).floor().max(0.0) as u64)
            .collect();
        Recording {
            source: index.source.clone(),
            capture_mode: CaptureMode {
                width: index.width as u32,
                height: index.height as u32,
                fps,
            },
            calibration_id: None,
            calibration_reason: Some("synthetic fixture is not capture-chain calibration".into()),
            frames: None,
            pixel_format: PixelFormat::Yuv420p,
            bit_depth: 8,
            color_space: ColorSpace::Bt709,
            clock_domain: "synthetic-fixture-monotonic".into(),
            frame_count: micros.len() as u64,
            timestamp_count: micros.len() as u64,
            timestamps: Some(micros),
            validity: Validity::Valid,
            metadata_evidence: None,
        }
    }

    fn write_capture(dir: &Path, index: &RecordingIndex, native: &Recording) {
        std::fs::write(dir.join("frames.json"), serde_json::to_vec(index).unwrap()).unwrap();
        std::fs::write(
            dir.join("recording.json"),
            serde_json::to_vec(native).unwrap(),
        )
        .unwrap();
    }

    /// Writes a capture directory whose frame timestamps are exactly `timestamps` seconds.
    fn fixture(dir: &Path, timestamps: Vec<f64>, fps: u32) {
        let index = index_of(timestamps);
        let native = native_of(&index, fps);
        write_capture(dir, &index, &native);
    }

    fn write_summary_budget(
        dir: &Path,
        expected: u64,
        recorded: u64,
        rejected: u64,
        interval_100ns: u32,
    ) {
        write_summary(dir, recorded, rejected, interval_100ns);
        let path = dir.join(crate::hdr_capture::SUMMARY_FILE);
        let mut value: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        value["expected_frames"] = serde_json::json!(expected);
        std::fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
    }

    fn write_summary(dir: &Path, recorded: u64, rejected: u64, interval_100ns: u32) {
        let summary = HdrSummary {
            expected_frames: recorded,
            recorded_frames: recorded,
            observed_usb_frames: recorded + rejected,
            rejected_usb_frames: rejected,
            usb_loss_reason: "synthetic fixture reject counter".into(),
            descriptor_interval_100ns: interval_100ns,
            source: HdrSource {
                description: "synthetic fixture source context; not an HDR assertion".into(),
                evidence_id: "synthetic-cadence-fixture".into(),
            },
            clock: Clock {
                id: "synthetic-fixture-monotonic".into(),
                clock_type: "monotonic".into(),
                epoch: None,
                native_resolution_ns: Some(1000.0),
                conversion: Some("synthetic".into()),
                quantization_bound_ns: Some(1000.0),
                uncertainty_ns: None,
                uncertainty_reason: Some("synthetic fixture; no calibration".into()),
                validity: Validity::Valid,
                reason: None,
            },
            timestamp_boundary: "synthetic fixture boundary".into(),
            started_us: 0,
            ended_us: 1,
            ownership_restored: true,
        };
        std::fs::write(
            dir.join("capture-summary.json"),
            serde_json::to_vec(&summary).unwrap(),
        )
        .unwrap();
    }

    /// `count` frames at exactly the nominal 60 Hz interval, starting at 100 s.
    fn nominal_series(count: usize) -> Vec<f64> {
        (0..count).map(|i| 100.0 + i as f64 * NOMINAL_60).collect()
    }

    /// A series where interval `at` spans `multiple` nominal periods instead of one.
    fn series_with_gap(count: usize, at: usize, multiple: f64) -> Vec<f64> {
        let mut timestamps = Vec::with_capacity(count);
        let mut now = 100.0;
        timestamps.push(now);
        for i in 0..count - 1 {
            now += if i == at { multiple } else { 1.0 } * NOMINAL_60;
            timestamps.push(now);
        }
        timestamps
    }

    #[test]
    fn exact_nominal_cadence_has_no_gaps() {
        let scratch = Scratch::new();
        fixture(scratch.path(), nominal_series(10), 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(
            report.nominal.source,
            NominalSource::RecordingCaptureModeFps
        );
        assert!((report.nominal.interval_ms - 1000.0 / 60.0).abs() < 1e-9);
        assert_eq!(report.population.frames, 10);
        assert_eq!(report.population.intervals, 9);
        assert!(report.gaps.is_empty());
        assert_eq!(report.total_implied_missing_frames, 0);
        assert!((report.population.effective_fps - 60.0).abs() < 1e-6);
        let steady = report.steady_state.unwrap();
        assert_eq!(steady.interval_ms.count, 9);
        assert!(steady.max_abs_deviation_from_nominal_ms < 1e-6);
        assert!((steady.effective_fps - 60.0).abs() < 1e-6);
        assert!(report.hdr_cross_check.is_none());
        assert!(report.startup_exclusion.is_none());
    }

    #[test]
    fn single_startup_gap_is_classified_with_implied_missing_frames() {
        let scratch = Scratch::new();
        fixture(scratch.path(), series_with_gap(12, 1, 6.0), 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(report.gaps.len(), 1);
        let gap = &report.gaps[0];
        assert_eq!(gap.interval_index, 1);
        assert_eq!((gap.from_frame, gap.to_frame), (1, 2));
        assert!((gap.nominal_intervals - 6.0).abs() < 1e-6);
        assert!((gap.duration_ms - 100.0).abs() < 1e-6);
        assert_eq!(gap.implied_missing_frames, 5);
        assert_eq!(report.total_implied_missing_frames, 5);
        assert!(gap.attribution.contains("unobservable"));
        assert!(gap.attribution.contains("not a SunshinePS5 host drop"));
        // The whole-population average is dragged below nominal by the one gap, while the
        // steady-state population stays at nominal. That separation is the point of the tool.
        assert!(report.population.effective_fps < 59.0);
        let steady = report.steady_state.unwrap();
        assert_eq!(steady.interval_ms.count, 10);
        assert!((steady.effective_fps - 60.0).abs() < 1e-6);
    }

    #[test]
    fn mid_stream_gap_is_classified() {
        let scratch = Scratch::new();
        fixture(scratch.path(), series_with_gap(14, 7, 3.0), 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(report.gaps.len(), 1);
        let gap = &report.gaps[0];
        assert_eq!(gap.interval_index, 7);
        assert_eq!((gap.from_frame, gap.to_frame), (7, 8));
        assert_eq!(gap.implied_missing_frames, 2);
        assert!((gap.nominal_intervals - 3.0).abs() < 1e-6);
        assert_eq!(report.steady_state.unwrap().interval_ms.count, 12);
    }

    #[test]
    fn sub_threshold_jitter_is_not_classified_as_a_gap() {
        let scratch = Scratch::new();
        // 1.49x nominal is jitter under the default 1.5x threshold; 1.51x is a gap.
        let mut timestamps = vec![100.0];
        for multiple in [1.0, 1.45, 0.8, 1.49, 1.2, 1.0] {
            timestamps.push(timestamps.last().unwrap() + multiple * NOMINAL_60);
        }
        fixture(scratch.path(), timestamps.clone(), 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert!(report.gaps.is_empty(), "{:?}", report.gaps);
        assert_eq!(report.total_implied_missing_frames, 0);
        let steady = report.steady_state.unwrap();
        assert_eq!(steady.interval_ms.count, 6);
        assert!((steady.max_abs_deviation_from_nominal_ms - 0.49 * 1000.0 / 60.0).abs() < 1e-6);

        let strict = Scratch::new();
        fixture(strict.path(), timestamps, 60);
        let tightened = analyze(
            strict.path(),
            &CadenceOptions {
                gap_threshold_nominal: 1.4,
                ..CadenceOptions::default()
            },
        )
        .unwrap();
        assert_eq!(tightened.gaps.len(), 2);
        // Round-to-nearest: 1.45 and 1.49 nominal periods are one period, so no frame is implied
        // missing even once the threshold calls them gaps.
        assert_eq!(tightened.total_implied_missing_frames, 0);
    }

    #[test]
    fn declared_startup_exclusion_reports_both_populations() {
        let scratch = Scratch::new();
        fixture(scratch.path(), series_with_gap(12, 1, 6.0), 60);
        let report = analyze(
            scratch.path(),
            &CadenceOptions {
                declared_startup_exclusion: Some(StartupExclusion {
                    frames: 2,
                    reason: "declared startup window: first frames follow stream negotiation"
                        .into(),
                }),
                ..CadenceOptions::default()
            },
        )
        .unwrap();
        // Full population is still reported, with its gap.
        assert_eq!(report.population.frames, 12);
        assert_eq!(report.gaps.len(), 1);
        let exclusion = report.startup_exclusion.unwrap();
        assert_eq!(exclusion.declared_startup_frames, 2);
        assert_eq!(exclusion.excluded_frames, 2);
        assert_eq!(exclusion.excluded_intervals, 2);
        assert!(exclusion.reason.contains("declared startup window"));
        assert_eq!(exclusion.retained.frames, 10);
        assert_eq!(exclusion.retained.intervals, 9);
        assert!(exclusion.retained_gaps.is_empty());
        assert_eq!(exclusion.retained_gaps_omitted, 0);
        assert_eq!(exclusion.retained_implied_missing_frames, 0);
        assert!((exclusion.retained.effective_fps - 60.0).abs() < 1e-6);
        assert_eq!(
            exclusion.retained_steady_state.unwrap().interval_ms.count,
            9
        );

        // A window that leaves fewer than two frames cannot be analyzed.
        let short = Scratch::new();
        fixture(short.path(), nominal_series(4), 60);
        let error = analyze(
            short.path(),
            &CadenceOptions {
                declared_startup_exclusion: Some(StartupExclusion {
                    frames: 3,
                    reason: "too wide".into(),
                }),
                ..CadenceOptions::default()
            },
        )
        .unwrap_err();
        assert!(format!("{error:#}").contains("startup exclusion"));
    }

    #[test]
    fn hdr_summary_agreement_is_reported() {
        let scratch = Scratch::new();
        fixture(scratch.path(), series_with_gap(12, 1, 6.0), 60);
        write_summary(scratch.path(), 12, 5, 166_666);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(report.nominal.source, NominalSource::HdrDescriptorInterval);
        assert!((report.nominal.interval_ms - 16.6666).abs() < 1e-9);
        assert_eq!(report.nominal.candidates.len(), 2);
        let cross = report.hdr_cross_check.unwrap();
        assert!(cross.recorded_frames_agree);
        assert_eq!(cross.timestamp_count, 12);
        assert_eq!(cross.rejected_usb_frames, 5);
        assert_eq!(cross.implied_missing_frames, 5);
        assert!(cross.rejected_matches_implied_missing);
        assert!(cross.note.contains("not causation"));
    }

    #[test]
    fn hdr_summary_disagreement_is_reported_not_assumed() {
        let scratch = Scratch::new();
        fixture(scratch.path(), series_with_gap(12, 1, 6.0), 60);
        write_summary(scratch.path(), 99, 2, 166_666);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        let cross = report.hdr_cross_check.unwrap();
        assert_eq!(cross.summary_recorded_frames, 99);
        assert_eq!(cross.timestamp_count, 12);
        assert!(!cross.recorded_frames_agree);
        assert_eq!(cross.rejected_usb_frames, 2);
        assert_eq!(cross.implied_missing_frames, 5);
        assert!(!cross.rejected_matches_implied_missing);
    }

    #[test]
    fn rejects_fewer_than_two_timestamps() {
        for count in [0, 1] {
            let scratch = Scratch::new();
            fixture(scratch.path(), nominal_series(count), 60);
            let error = analyze(scratch.path(), &CadenceOptions::default()).unwrap_err();
            assert!(
                format!("{error:#}").contains("at least two"),
                "{count}: {error:#}"
            );
        }
    }

    #[test]
    fn rejects_non_monotonic_and_duplicate_timestamps() {
        let scratch = Scratch::new();
        let mut timestamps = nominal_series(6);
        timestamps[3] = timestamps[2];
        fixture(scratch.path(), timestamps, 60);
        let error = analyze(scratch.path(), &CadenceOptions::default()).unwrap_err();
        assert!(format!("{error:#}").contains("duplicate"), "{error:#}");

        let reversed = Scratch::new();
        let mut timestamps = nominal_series(6);
        timestamps.swap(2, 4);
        fixture(reversed.path(), timestamps, 60);
        let error = analyze(reversed.path(), &CadenceOptions::default()).unwrap_err();
        assert!(
            format!("{error:#}").contains("strictly increase"),
            "{error:#}"
        );
    }

    #[test]
    fn rejects_non_finite_or_negative_timestamp() {
        // JSON cannot carry NaN or an infinity, so the non-finite guard is asserted directly at
        // the validator boundary it defends: any in-process caller of this module.
        for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -1.0] {
            let error = validate_timestamps(&[100.0, bad, 100.1]).unwrap_err();
            assert!(
                format!("{error:#}").contains("non-finite or negative"),
                "{bad}: {error:#}"
            );
        }
        // A negative timestamp does survive JSON, and must be rejected end to end. The native
        // sidecar cannot represent it, so it is built from a clean series: the analysis must
        // reject the index rather than trust whichever sidecar looks healthier.
        let scratch = Scratch::new();
        let index = index_of({
            let mut timestamps = nominal_series(6);
            timestamps[2] = -1.0;
            timestamps
        });
        let native = native_of(&index_of(nominal_series(6)), 60);
        write_capture(scratch.path(), &index, &native);
        let error = analyze(scratch.path(), &CadenceOptions::default()).unwrap_err();
        assert!(
            format!("{error:#}").contains("non-finite or negative"),
            "{error:#}"
        );
        // An out-of-range literal must fail loudly rather than silently become an infinity.
        let overflowed = Scratch::new();
        fixture(overflowed.path(), nominal_series(6), 60);
        std::fs::write(
            overflowed.path().join("frames.json"),
            format!(
                r#"{{"source":"{SOURCE}","width":1920,"height":1080,"timestamps":[1.0,1e400,3.0]}}"#
            ),
        )
        .unwrap();
        assert!(analyze(overflowed.path(), &CadenceOptions::default()).is_err());
    }

    #[test]
    fn rejects_index_metadata_mismatch() {
        let geometry = Scratch::new();
        let index = index_of(nominal_series(6));
        let mut native = native_of(&index, 60);
        native.capture_mode.width = 1280;
        write_capture(geometry.path(), &index, &native);
        let error = analyze(geometry.path(), &CadenceOptions::default()).unwrap_err();
        assert!(format!("{error:#}").contains("mismatch"), "{error:#}");

        let counted = Scratch::new();
        let mut native = native_of(&index, 60);
        native.frame_count = 5;
        write_capture(counted.path(), &index, &native);
        let error = analyze(counted.path(), &CadenceOptions::default()).unwrap_err();
        assert!(format!("{error:#}").contains("mismatch"), "{error:#}");

        let drifted = Scratch::new();
        let mut native = native_of(&index, 60);
        native.timestamps.as_mut().unwrap()[4] += 50_000;
        write_capture(drifted.path(), &index, &native);
        let error = analyze(drifted.path(), &CadenceOptions::default()).unwrap_err();
        assert!(format!("{error:#}").contains("disagree"), "{error:#}");

        let missing = Scratch::new();
        fixture(missing.path(), nominal_series(6), 60);
        std::fs::remove_file(missing.path().join("recording.json")).unwrap();
        assert!(analyze(missing.path(), &CadenceOptions::default()).is_err());
    }

    #[test]
    fn rejects_invalid_threshold_or_nominal_override() {
        let scratch = Scratch::new();
        fixture(scratch.path(), nominal_series(6), 60);
        for threshold in [1.0, 0.5, f64::NAN, f64::INFINITY] {
            let error = analyze(
                scratch.path(),
                &CadenceOptions {
                    gap_threshold_nominal: threshold,
                    ..CadenceOptions::default()
                },
            )
            .unwrap_err();
            assert!(
                format!("{error:#}").contains("gap threshold"),
                "{threshold}: {error:#}"
            );
        }
        for fps in [0.0, -60.0, f64::NAN, 100_000.0] {
            let error = analyze(
                scratch.path(),
                &CadenceOptions {
                    nominal_fps_override: Some(fps),
                    ..CadenceOptions::default()
                },
            )
            .unwrap_err();
            assert!(
                format!("{error:#}").contains("nominal fps"),
                "{fps}: {error:#}"
            );
        }
    }

    #[test]
    fn nominal_override_is_declared_and_keeps_displaced_candidates() {
        let scratch = Scratch::new();
        fixture(scratch.path(), nominal_series(6), 50);
        let metadata = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(
            metadata.nominal.source,
            NominalSource::RecordingCaptureModeFps
        );
        assert!((metadata.nominal.interval_ms - 20.0).abs() < 1e-9);

        let overridden = analyze(
            scratch.path(),
            &CadenceOptions {
                nominal_fps_override: Some(30.0),
                ..CadenceOptions::default()
            },
        )
        .unwrap();
        assert_eq!(overridden.nominal.source, NominalSource::CliOverride);
        assert!((overridden.nominal.interval_ms - 1000.0 / 30.0).abs() < 1e-9);
        assert!(overridden
            .nominal
            .candidates
            .iter()
            .any(|c| c.source == NominalSource::RecordingCaptureModeFps && c.fps == 50.0));
    }

    #[test]
    fn report_is_labelled_diagnostic_only_and_carries_limitations() {
        let scratch = Scratch::new();
        fixture(scratch.path(), nominal_series(10), 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert!(report.kind.contains("diagnostic"));
        let text = serde_json::to_string(&report).unwrap();
        for required in [
            "not acceptance evidence",
            "SC-013",
            "unobservable",
            "not HDMI exposure",
            "input-to-display",
        ] {
            assert!(text.contains(required), "missing {required}");
        }
        for forbidden in ["\"Pass\"", "\"Fail\"", "overall_status"] {
            assert!(!text.contains(forbidden), "unexpected {forbidden}");
        }
        assert!(report.limitations.len() >= 6);
        assert!(report.population.span_ms > 0.0);
        assert_eq!(report.recording_validity, Validity::Valid);
    }

    /// A met frame budget must not read as lost frames. Implied-missing counts nominal arrival
    /// slots without a recorded frame; it is not a recording shortfall. The R4 capture recorded
    /// its full 120-frame budget while still showing a six-period startup arrival gap.
    #[test]
    fn implied_missing_frames_are_reconciled_against_the_frame_budget() {
        let scratch = Scratch::new();
        let mut timestamps = vec![0.0, NOMINAL_60];
        let mut at = NOMINAL_60 + NOMINAL_60 * 6.0;
        for _ in 0..10 {
            timestamps.push(at);
            at += NOMINAL_60;
        }
        fixture(scratch.path(), timestamps, 60);
        write_summary_budget(scratch.path(), 12, 12, 5, 166_666);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(report.total_implied_missing_frames, 5);
        // The basis must deny the "frames lost from the recording" reading outright.
        let basis = report.implied_missing_basis.to_lowercase();
        assert!(
            basis.contains("arrival"),
            "basis omits arrival framing: {basis}"
        );
        assert!(
            basis.contains("not") && basis.contains("recording"),
            "basis does not deny a recording shortfall: {basis}"
        );
        let cross = report.hdr_cross_check.as_ref().unwrap();
        assert!(
            cross.met_requested_frame_budget,
            "12 recorded of 12 requested must report a met budget despite 5 implied missing"
        );
        // A genuine shortfall is reported as one.
        let short = Scratch::new();
        let mut t2 = vec![0.0, NOMINAL_60];
        let mut a2 = NOMINAL_60 + NOMINAL_60 * 6.0;
        for _ in 0..10 {
            t2.push(a2);
            a2 += NOMINAL_60;
        }
        fixture(short.path(), t2, 60);
        write_summary_budget(short.path(), 20, 12, 5, 166_666);
        let shortfall = analyze(short.path(), &CadenceOptions::default()).unwrap();
        assert!(
            !shortfall
                .hdr_cross_check
                .as_ref()
                .unwrap()
                .met_requested_frame_budget
        );
    }

    /// Steady-state fps is selection-biased by construction: the slow intervals were removed to
    /// build the population. It must say so and flag exceeding nominal rather than reading as a
    /// clean rate the console sustained.
    #[test]
    fn steady_state_fps_declares_its_selection_bias() {
        let scratch = Scratch::new();
        // Non-gap intervals deliberately faster than nominal, plus one real gap.
        let fast = NOMINAL_60 * 0.96;
        let mut timestamps = vec![0.0];
        let mut at = 0.0;
        for _ in 0..6 {
            at += fast;
            timestamps.push(at);
        }
        at += NOMINAL_60 * 5.0;
        timestamps.push(at);
        for _ in 0..6 {
            at += fast;
            timestamps.push(at);
        }
        fixture(scratch.path(), timestamps, 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        let steady = report.steady_state.as_ref().unwrap();
        assert!(steady.effective_fps > 60.0, "fixture must exceed nominal");
        assert!(
            steady.exceeds_nominal,
            "a steady-state rate above nominal must be flagged"
        );
        let basis = steady.basis.to_lowercase();
        assert!(
            basis.contains("selection") || basis.contains("selected"),
            "basis must disclose selection bias: {basis}"
        );
    }

    /// The derivation's hard case: an over-threshold interval coexisting with compensating
    /// sub-nominal jitter, so the span stays on schedule while one arrival slot is empty.
    #[test]
    fn over_threshold_interval_with_compensating_jitter() {
        let scratch = Scratch::new();
        // Two nominal periods spent as one 1.8x interval followed by a 0.2x catch-up.
        let timestamps = vec![
            0.0,
            NOMINAL_60,
            NOMINAL_60 * 2.8,
            NOMINAL_60 * 3.0,
            NOMINAL_60 * 4.0,
            NOMINAL_60 * 5.0,
        ];
        fixture(scratch.path(), timestamps, 60);
        let report = analyze(scratch.path(), &CadenceOptions::default()).unwrap();
        assert_eq!(report.gaps.len(), 1);
        let gap = &report.gaps[0];
        assert_eq!(gap.interval_index, 1);
        // 1.8 periods rounds to 2, so exactly one nominal slot held no recorded frame.
        assert_eq!(gap.implied_missing_frames, 1);
        assert_eq!(report.total_implied_missing_frames, 1);
        // The catch-up interval is below nominal and must not become a negative correction.
        let steady = report.steady_state.as_ref().unwrap();
        assert!(steady.interval_ms.min < 1000.0 / 60.0);
    }
}
