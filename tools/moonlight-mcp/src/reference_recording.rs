//! Strict reference-client recording/report validation from a trusted observer snapshot.
//! Provenance and calibration are observer attestations, backed by retained source bytes.
//! Numeric frame scores and full report equality are checked locally.
use crate::{
    evidence::{validate_id, validate_reason, ArtifactRef, Provenance},
    recording::{ColorSpace, Recording},
    report::{Report, Status},
};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::File,
    io::Read,
    path::{Path, PathBuf},
};

const PROFILE: &[u8] = include_bytes!("../../../tests/scenarios/profiles/acceptance.json");
pub const MAX_MANIFEST_BYTES: usize = 4 * 1024 * 1024;
const MAX_ARTIFACT_BYTES: usize = 64 * 1024 * 1024;
const MAX_TOTAL_BYTES: usize = 256 * 1024 * 1024;
pub fn profile_digest() -> String {
    hex::encode(Sha256::digest(PROFILE))
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Artifact {
    pub path: ArtifactRef,
    pub sha256: String,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PopulationKind {
    Sdr,
    Hdr,
    SdrKnownDifference,
    HdrKnownDifference,
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Calibration {
    pub id: String,
    pub provenance: Provenance,
    pub source: ArtifactRef,
    pub uncertainty_ms: f64,
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceHdr {
    pub epoch: String,
    pub provenance: Provenance,
    pub source: ArtifactRef,
    pub mastering_max_nits: f64,
    pub mastering_min_nits: f64,
    pub max_cll_nits: f64,
    pub max_fall_nits: f64,
    pub primaries_xy: [[f64; 2]; 3],
    pub white_xy: [f64; 2],
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Population {
    pub kind: PopulationKind,
    pub stream: Recording,
    pub reference: Recording,
    pub calibration: Option<Calibration>,
    pub source_hdr: Option<SourceHdr>,
    pub stream_hdr: Option<SourceHdr>,
    pub delay_ms: f64,
    /// Independently known delay interval, frozen by the observer before comparison.
    pub expected_delay_ms: [f64; 2],
    /// Independent known-scene score bounds; differences must exclude a perfect score.
    pub expected_ssim: [f64; 2],
    pub oracle: ArtifactRef,
    pub scores: ArtifactRef,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ReportKind {
    Pass,
    Fail,
    Incomplete,
    FailUnderIncomplete,
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReportCase {
    pub kind: ReportKind,
    pub expected: ArtifactRef,
    pub emitted: ArtifactRef,
    pub oracle: ArtifactRef,
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub schema_version: String,
    pub session_id: String,
    pub scenario_id: String,
    pub host_version: String,
    pub test_client_version: String,
    pub test_client_revision: String,
    pub official_client_version: String,
    pub profile_id: String,
    pub profile_version: String,
    pub profile_sha256: String,
    pub provenance: Provenance,
    pub artifacts: Vec<Artifact>,
    pub populations: Vec<Population>,
    pub reports: Vec<ReportCase>,
}
pub struct Imported {
    pub manifest: Manifest,
    pub artifacts: Vec<(ArtifactRef, Vec<u8>)>,
    pub recording: Status,
    pub report: Status,
}
fn read(path: &Path, limit: usize) -> Result<Vec<u8>> {
    let metadata = std::fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && metadata.len() <= limit as u64,
        "nonregular/oversized evidence"
    );
    let mut bytes = Vec::new();
    File::open(path)?
        .take(limit as u64 + 1)
        .read_to_end(&mut bytes)?;
    ensure!(
        !bytes.is_empty() && bytes.len() <= limit,
        "empty/oversized evidence"
    );
    Ok(bytes)
}
fn source<'a>(data: &'a BTreeMap<PathBuf, Vec<u8>>, path: &ArtifactRef) -> Result<&'a [u8]> {
    path.validate()?;
    data.get(&path.0)
        .map(Vec::as_slice)
        .context("unbound evidence reference")
}
fn hdr(h: &SourceHdr, data: &BTreeMap<PathBuf, Vec<u8>>) -> Result<bool> {
    validate_id(&h.epoch)?;
    source(data, &h.source)?;
    ensure!(
        [
            h.mastering_min_nits,
            h.mastering_max_nits,
            h.max_cll_nits,
            h.max_fall_nits
        ]
        .iter()
        .all(|n| n.is_finite() && *n >= 0.0 && *n <= 10000.0)
            && h.mastering_max_nits > h.mastering_min_nits
            && h.max_cll_nits >= h.max_fall_nits,
        "invalid HDR luminance metadata"
    );
    ensure!(
        h.primaries_xy
            .iter()
            .chain(std::iter::once(&h.white_xy))
            .all(
                |xy| xy.iter().all(|n| n.is_finite() && *n > 0.0 && *n < 1.0)
                    && xy[0] + xy[1] <= 1.0
            ),
        "invalid HDR chromaticity"
    );
    Ok(h.provenance == Provenance::ReferenceHost)
}
fn population(p: &Population, data: &BTreeMap<PathBuf, Vec<u8>>) -> Result<Status> {
    p.stream.validate()?;
    p.reference.validate()?;
    ensure!(
        p.stream.frames != p.reference.frames,
        "independent recordings require distinct source paths"
    );
    source(data, &p.oracle)?;
    let hdr_mode = matches!(
        p.kind,
        PopulationKind::Hdr | PopulationKind::HdrKnownDifference
    );
    ensure!(
        p.stream.capture_mode == p.reference.capture_mode
            && p.stream.clock_domain == p.reference.clock_domain
            && p.stream.color_space == p.reference.color_space
            && (p.stream.color_space == ColorSpace::PqBt2020) == hdr_mode,
        "recording mode/clock disagreement"
    );
    if p.stream.capture_mode.width < 16 || p.stream.capture_mode.height < 16 {
        return Ok(Status::Incomplete);
    }
    ensure!(
        p.delay_ms.is_finite()
            && p.delay_ms.abs() <= 60000.0
            && p.expected_delay_ms
                .iter()
                .all(|n| n.is_finite() && n.abs() <= 60000.0)
            && p.expected_delay_ms[0] <= p.expected_delay_ms[1]
            && p.expected_ssim
                .iter()
                .all(|n| n.is_finite() && (-1.0..=1.0).contains(n))
            && p.expected_ssim[0] <= p.expected_ssim[1],
        "invalid oracle bounds"
    );
    if matches!(
        p.kind,
        PopulationKind::SdrKnownDifference | PopulationKind::HdrKnownDifference
    ) {
        ensure!(
            p.expected_ssim[1] < 1.0,
            "known difference must exclude perfect score"
        );
    }
    let mut complete = false;
    let mut uncertainty = 0.0;
    if let Some(c) = &p.calibration {
        validate_id(&c.id)?;
        source(data, &c.source)?;
        ensure!(
            c.uncertainty_ms.is_finite() && (0.0..=60000.0).contains(&c.uncertainty_ms),
            "invalid calibration uncertainty"
        );
        ensure!(
            p.stream
                .calibration_id
                .as_ref()
                .is_none_or(|id| id == &c.id)
                && p.reference
                    .calibration_id
                    .as_ref()
                    .is_none_or(|id| id == &c.id),
            "calibration identity disagreement"
        );
        complete = c.provenance == Provenance::ReferenceHost
            && p.stream.calibration_id.as_deref() == Some(&c.id)
            && p.reference.calibration_id.as_deref() == Some(&c.id);
        uncertainty = c.uncertainty_ms;
    }
    if hdr_mode {
        match (&p.source_hdr, &p.stream_hdr) {
            (Some(a), Some(b)) => {
                complete &= hdr(a, data)? && hdr(b, data)?;
                let mut aa = serde_json::to_value(a)?;
                let mut bb = serde_json::to_value(b)?;
                aa.as_object_mut().unwrap().remove("source");
                bb.as_object_mut().unwrap().remove("source");
                ensure!(aa == bb, "source/signaled HDR metadata disagree");
                complete &= p.stream.metadata_evidence.as_deref() == Some(&a.epoch)
                    && p.reference.metadata_evidence.as_deref() == Some(&a.epoch);
            }
            _ => complete = false,
        }
    } else {
        ensure!(
            p.source_hdr.is_none() && p.stream_hdr.is_none(),
            "HDR metadata in SDR population"
        );
    }
    let stream = source(
        data,
        p.stream.frames.as_ref().context("stream frames absent")?,
    )?;
    let reference = source(
        data,
        p.reference
            .frames
            .as_ref()
            .context("reference frames absent")?,
    )?;
    let frame_bytes = usize::try_from(p.stream.checked_frame_bytes()?)?;
    let reference_bytes = usize::try_from(p.reference.checked_frame_bytes()?)?;
    ensure!(
        frame_bytes.checked_mul(p.stream.frame_count as usize) == Some(stream.len())
            && reference_bytes.checked_mul(p.reference.frame_count as usize)
                == Some(reference.len()),
        "frame byte population mismatch"
    );
    let scores: Vec<crate::compare::FrameResult> =
        serde_json::Deserializer::from_slice(source(data, &p.scores)?)
            .into_iter()
            .collect::<std::result::Result<_, _>>()?;
    ensure!(
        scores.len() == p.stream.frame_count as usize,
        "partial score population"
    );
    let delay_lower = p.delay_ms - uncertainty;
    let delay_upper = p.delay_ms + uncertainty;
    let delay_status =
        if delay_lower >= p.expected_delay_ms[0] && delay_upper <= p.expected_delay_ms[1] {
            Status::Pass
        } else if delay_upper < p.expected_delay_ms[0] || delay_lower > p.expected_delay_ms[1] {
            Status::Fail
        } else {
            Status::Incomplete
        };
    let mut pass = true;
    let mut paired = BTreeSet::new();
    for (i, row) in scores.iter().enumerate() {
        ensure!(
            row.stream_frame == i && row.exclusion_reason.is_none(),
            "missing/duplicate/excluded score"
        );
        let j = row.reference_frame.context("unpaired frame")?;
        ensure!(
            j < p.reference.frame_count as usize && paired.insert(j),
            "invalid/reused reference frame"
        );
        let ts = p.stream.timestamps.as_ref().unwrap()[i] as f64 / 1000.0;
        let tr = p.reference.timestamps.as_ref().unwrap()[j] as f64 / 1000.0;
        ensure!(
            (row.stream_time_seconds * 1000.0 - ts).abs() < 0.000001
                && row
                    .reference_time_seconds
                    .is_some_and(|v| (v * 1000.0 - tr).abs() < 0.000001)
                && row
                    .timestamp_residual_ms
                    .is_some_and(|v| (v - (ts - tr - p.delay_ms)).abs() < 0.000001),
            "alignment timestamp mismatch"
        );
        pass &= (ts - tr - p.delay_ms).abs() <= uncertainty;
        let a = crate::compare::native_planes_bytes(
            &p.stream,
            &stream[i * frame_bytes..(i + 1) * frame_bytes],
        )?;
        let b = crate::compare::native_planes_bytes(
            &p.reference,
            &reference[j * reference_bytes..(j + 1) * reference_bytes],
        )?;
        for plane in 0..3 {
            let div = if plane == 0 { 1 } else { 2 };
            let score = crate::compare::score_plane(
                &a[plane],
                &b[plane],
                p.stream.capture_mode.width as usize / div,
                p.stream.capture_mode.height as usize / div,
                p.stream.color_space,
            )?;
            ensure!(
                row.perfect_psnr_yuv[plane] == score.perfect
                    && match (row.psnr_db_yuv[plane], score.psnr_db) {
                        (None, None) => true,
                        (Some(x), Some(y)) => x.is_finite() && (x - y).abs() < 1e-9,
                        _ => false,
                    },
                "PSNR report differs from native samples"
            );
            if plane == 0 {
                ensure!(
                    row.ssim
                        .is_some_and(|v| v.is_finite() && (v - score.ssim).abs() < 1e-12)
                        && row.ssim_bits.as_deref()
                            == Some(&format!("{:016x}", score.ssim.to_bits())),
                    "SSIM report differs from native samples"
                );
                pass &= score.ssim >= p.expected_ssim[0] && score.ssim <= p.expected_ssim[1];
            }
        }
    }
    Ok(if !complete {
        Status::Incomplete
    } else if !pass || delay_status == Status::Fail {
        Status::Fail
    } else {
        delay_status
    })
}

pub fn load(path: &Path, session: &str, scenario: &str, host: Option<&str>) -> Result<Imported> {
    load_manifest(path, read_manifest(path)?, session, scenario, host)
}

/// Only this top-level read/parse may be retried while an observer publishes.
pub(crate) fn read_manifest(path: &Path) -> Result<Manifest> {
    Ok(serde_json::from_slice(&read(path, MAX_MANIFEST_BYTES)?)?)
}

/// Consume the exact parsed publication without rereading its manifest path.
pub(crate) fn load_manifest(
    path: &Path,
    manifest: Manifest,
    session: &str,
    scenario: &str,
    host: Option<&str>,
) -> Result<Imported> {
    ensure!(
        manifest.schema_version == "1.0.0"
            && manifest.session_id == session
            && manifest.scenario_id == scenario
            && Some(manifest.host_version.as_str()) == host
            && manifest.test_client_version == env!("CARGO_PKG_VERSION")
            && manifest.profile_id == "reference_host_compatibility"
            && manifest.profile_version == "1.0.0"
            && manifest.profile_sha256 == profile_digest(),
        "recording manifest binding mismatch"
    );
    validate_id(session)?;
    validate_id(scenario)?;
    for value in [
        &manifest.host_version,
        &manifest.test_client_revision,
        &manifest.official_client_version,
    ] {
        validate_reason(value)?;
    }
    ensure!(
        manifest.artifacts.len() <= 64
            && manifest.populations.len() <= 4
            && manifest.reports.len() <= 4,
        "evidence capacity exceeded"
    );
    let root = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."))
        .canonicalize()?;
    let mut data = BTreeMap::new();
    let mut total = 0;
    for artifact in &manifest.artifacts {
        artifact.path.validate()?;
        ensure!(
            artifact.path.0 != Path::new("manifest.json"),
            "reserved manifest path"
        );
        ensure!(!data.contains_key(&artifact.path.0), "duplicate artifact");
        let mut file = root.clone();
        for component in artifact.path.0.components() {
            file.push(component);
            ensure!(
                !std::fs::symlink_metadata(&file)?.file_type().is_symlink(),
                "artifact symlink"
            );
        }
        let bytes = read(&file, MAX_ARTIFACT_BYTES)?;
        total += bytes.len();
        ensure!(total <= MAX_TOTAL_BYTES, "total evidence capacity exceeded");
        ensure!(
            hex::encode(Sha256::digest(&bytes)) == artifact.sha256,
            "artifact checksum mismatch"
        );
        data.insert(artifact.path.0.clone(), bytes);
    }
    let mut kinds = BTreeSet::new();
    let mut statuses = Vec::new();
    for p in &manifest.populations {
        ensure!(kinds.insert(p.kind), "duplicate population");
        statuses.push(population(p, &data)?);
    }
    let recording = combine(&statuses, 4);
    let mut report_kinds = BTreeSet::new();
    let mut reports = Vec::new();
    for case in &manifest.reports {
        ensure!(report_kinds.insert(case.kind), "duplicate report case");
        source(&data, &case.oracle)?;
        ensure!(
            case.expected != case.emitted && case.oracle != case.emitted,
            "report oracle must be independent"
        );
        let expected: Report = serde_json::from_slice(source(&data, &case.expected)?)?;
        let emitted: Report = serde_json::from_slice(source(&data, &case.emitted)?)?;
        expected.validate()?;
        emitted.validate()?;
        let required: BTreeSet<_> = [
            "REF-pairing",
            "REF-stream",
            "REF-controller",
            "REF-teardown",
            "REF-recording",
            "REF-report",
        ]
        .into_iter()
        .collect();
        ensure!(
            expected.scenario()["required_criteria"]["profile_id"]
                == "reference_host_compatibility"
                && expected.scenario()["required_criteria"]["profile_version"] == "1.0.0"
                && expected
                    .criteria
                    .iter()
                    .map(|c| c.id.as_str())
                    .collect::<BTreeSet<_>>()
                    == required,
            "report oracle must cover the complete reference profile"
        );
        ensure!(
            expected.environment.host_version.as_deref() == host
                && expected.environment.client_version == manifest.test_client_version,
            "report oracle host/client mismatch"
        );
        for report in [&expected, &emitted] {
            for refs in [
                &report.artifact_refs,
                &report.hdr_events,
                &report.input.client_sent,
                &report.input.transport_acknowledged,
                &report.input.host_received,
                &report.input.console_delivered,
                &report.fidelity.per_frame_scores,
                &report.fidelity.sample_pairs,
            ] {
                for path in refs {
                    source(&data, path)?;
                }
            }
            for measurement in [
                &report.video.frame_rate,
                &report.video.frame_intervals,
                &report.video.protocol_processing_latency,
                &report.video.full_host_pipeline,
                &report.video.measured_bitrate,
                &report.audio.packet_loss,
                &report.input.delivery_latency,
                &report.fidelity.relative_delay,
            ]
            .into_iter()
            .chain(report.fidelity.ssim.iter())
            .chain(report.fidelity.psnr.iter())
            {
                for path in &measurement.artifact_refs {
                    source(&data, path)?;
                }
            }
        }
        let status = match case.kind {
            ReportKind::Pass => Status::Pass,
            ReportKind::Fail => Status::Fail,
            _ => Status::Incomplete,
        };
        ensure!(
            expected.overall_status == status,
            "oracle case status mismatch"
        );
        if case.kind == ReportKind::FailUnderIncomplete {
            ensure!(
                expected.criteria.iter().any(|c| c.status == Status::Fail)
                    && expected
                        .criteria
                        .iter()
                        .any(|c| c.status == Status::Incomplete),
                "oracle lacks preserved failure"
            );
        }
        reports.push(if expected == emitted {
            Status::Pass
        } else {
            Status::Fail
        });
    }
    let trusted = manifest.provenance == Provenance::ReferenceHost;
    Ok(Imported {
        manifest,
        artifacts: data.into_iter().map(|(p, b)| (ArtifactRef(p), b)).collect(),
        recording: if trusted {
            recording
        } else {
            Status::Incomplete
        },
        report: if trusted {
            combine(&reports, 4)
        } else {
            Status::Incomplete
        },
    })
}
fn combine(statuses: &[Status], required: usize) -> Status {
    if statuses.contains(&Status::Fail) {
        Status::Fail
    } else if statuses.len() != required || statuses.contains(&Status::Incomplete) {
        Status::Incomplete
    } else {
        Status::Pass
    }
}
