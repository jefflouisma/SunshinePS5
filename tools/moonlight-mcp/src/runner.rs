//! Scenario lifecycle shared by CLI and local agent tools. Reports start before network work.
//! All fixture results exercise software only; host/console delivery requires separate evidence.

use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::net::UdpSocket;
use tokio::sync::watch;

use crate::evidence::{
    ArtifactRef, Coverage, CoverageState, Interval, Measurement, Provenance, Source, Units,
    Validity,
};
use crate::measurements::{self, HostObservation};
use crate::report::{Environment, Report, Status};
use crate::scenario::{DynamicRange, Scenario};
use crate::{clock, control, decode, http, input, pairing, reference, rtsp, video};

pub const INVALID_INVOCATION: i32 = 64;
pub const MAX_RUN_SECONDS: f64 = 86_400.0;
const MAX_FRAMES: u64 = 1_000_000;
const OBSERVER_WAIT: ObserverWait = ObserverWait {
    timeout: Duration::from_secs(10),
    poll_interval: Duration::from_millis(100),
};

#[derive(Clone, Copy)]
struct ObserverWait {
    timeout: Duration,
    poll_interval: Duration,
}

pub fn exit_code(status: Status) -> i32 {
    match status {
        Status::Pass => 0,
        Status::Fail => 1,
        Status::Incomplete => 2,
    }
}
pub fn exit_code_name(status: &str) -> Result<i32> {
    Ok(match status {
        "invalid_invocation" => INVALID_INVOCATION,
        name => exit_code(serde_json::from_value(json!(name))?),
    })
}

pub enum ReferenceBinding {
    Sdr(reference::ReferenceSource),
    Hdr(crate::hdr_capture::HdrCapture),
}
impl Clone for ReferenceBinding {
    fn clone(&self) -> Self {
        match self {
            Self::Sdr(source) => Self::Sdr(source.clone()),
            Self::Hdr(source) => Self::Hdr(crate::hdr_capture::HdrCapture {
                executable: source.executable.clone(),
                source: source.source.clone(),
                max_bytes: source.max_bytes,
            }),
        }
    }
}
impl ReferenceBinding {
    async fn record(&self, seconds: f64, out: &Path, hdr: bool) -> Result<()> {
        match (self, hdr) {
            (Self::Sdr(source), false) => {
                source.record(seconds, out).await?;
            }
            (Self::Hdr(source), true) => {
                let result = source.acquire(seconds, out).await;
                ensure!(
                    result.recording.is_some(),
                    "{}",
                    result
                        .reason
                        .unwrap_or_else(|| "HDR capture unavailable".into())
                );
            }
            _ => bail!("reference binding cannot provide required dynamic range"),
        }
        Ok(())
    }
}

/// Bind secrets and device capabilities outside immutable, retained scenario text.
#[derive(Clone)]
pub struct RunOptions {
    pub root: PathBuf,
    pub report: PathBuf,
    pub config_dir: Option<PathBuf>,
    pub http_port: u16,
    pub app_id: Option<u32>,
    pub providers: HashMap<String, pairing::PinDelivery>,
    pub references: HashMap<String, ReferenceBinding>,
    /// Trusted startup binding (CLI only, never agent input): JSON-lines host observations.
    /// With reference_lifecycle, this must name one of that manifest's bound artifacts.
    pub host_observations: Option<PathBuf>,
    /// Read-only console endpoint fetched after teardown and imported as
    /// session-bound host input observations. CLI/trusted binding only.
    pub host_observations_url: Option<String>,
    /// Trusted independent reference-host lifecycle snapshot, read after cancellation.
    pub reference_lifecycle: Option<PathBuf>,
    /// Trusted recording/report observer snapshot, awaited after cancellation.
    pub reference_recording: Option<PathBuf>,
    /// Optional trusted startup READY file. Never accepted from an agent tool call.
    pub observer_ready: Option<PathBuf>,
    pub observer_attachment: Option<PathBuf>,
    pub observer_finish_signal: bool,
}
impl RunOptions {
    pub fn new(root: PathBuf, report: PathBuf) -> Self {
        Self {
            root,
            report,
            config_dir: None,
            http_port: 47989,
            app_id: None,
            providers: HashMap::new(),
            references: HashMap::new(),
            host_observations: None,
            host_observations_url: None,
            reference_lifecycle: None,
            reference_recording: None,
            observer_ready: None,
            observer_attachment: None,
            observer_finish_signal: false,
        }
    }
    /// Trusted startup bindings are reused by agent calls; tool input never contains secrets.
    pub fn with_report(&self, report: PathBuf) -> Self {
        Self {
            root: self.root.clone(),
            report,
            config_dir: self.config_dir.clone(),
            http_port: self.http_port,
            app_id: self.app_id,
            providers: self.providers.clone(),
            references: self.references.clone(),
            host_observations: self.host_observations.clone(),
            host_observations_url: self.host_observations_url.clone(),
            reference_lifecycle: self.reference_lifecycle.clone(),
            reference_recording: self.reference_recording.clone(),
            observer_ready: self.observer_ready.clone(),
            observer_attachment: self.observer_attachment.clone(),
            observer_finish_signal: self.observer_finish_signal,
        }
    }
}

/// REF-controller (acceptance.json reference_host_compatibility): every action declaring
/// `expected_delivery` must be observed applied by the reference host itself, for the same
/// controller and event. Sends, ACKs and host receipts never count, only `reference_host`
/// provenance can establish (or refute) reference acceptance -- synthetic fixtures, console-side
/// and operator records never do -- and a failure the reference host observed wins.
pub fn controller_outcome(
    expected: &[(u8, &str)],
    observations: &[HostObservation],
) -> (Status, &'static str) {
    if expected.is_empty() {
        return (
            Status::Incomplete,
            "no controller action declares an expected delivery",
        );
    }
    let mut missing = false;
    for (controller, event) in expected {
        let independent = observations.iter().filter(|o| {
            o.kind == "input"
                && o.provenance == Provenance::ReferenceHost
                && o.event_id.as_deref() == Some(*event)
                && o.controller_id == Some(*controller)
        });
        let mut delivered = false;
        for observation in independent {
            if observation.boundary == "delivery_failed" {
                return (
                    Status::Fail,
                    "the reference host observed a declared controller action fail to apply",
                );
            }
            delivered |= observation.boundary == "console_delivered"
                && observation.validity == Validity::Valid
                && observation.authenticated == Some(true);
        }
        missing |= !delivered;
    }
    if missing {
        let synthetic = observations
            .iter()
            .any(|o| o.provenance == Provenance::SyntheticFixture);
        let foreign = observations.iter().any(|o| {
            !matches!(
                o.provenance,
                Provenance::ReferenceHost | Provenance::SyntheticFixture
            )
        });
        return (
            Status::Incomplete,
            if synthetic {
                "synthetic fixture host observations cannot establish reference acceptance"
            } else if foreign {
                "console or operator records are not reference host observations and cannot establish reference acceptance"
            } else {
                "an independent host observation is missing for a declared controller action"
            },
        );
    }
    (
        Status::Pass,
        "the reference host independently observed every declared controller action applied",
    )
}

/// Resolve the explicit controller binding lexically, without reopening any validated source.
/// Aliased paths or parent traversal are rejected rather than guessed from a filename.
fn controller_records(
    o: &RunOptions,
    session_id: &str,
    snapshot: Option<&crate::reference_lifecycle::Imported>,
) -> Result<Vec<HostObservation>> {
    let path = o
        .host_observations
        .as_ref()
        .context("no controller binding")?;
    if let Some(manifest_path) = &o.reference_lifecycle {
        let snapshot = snapshot.context("controller manifest was not validated")?;
        let cwd = std::env::current_dir()?;
        let absolute_manifest = cwd.join(manifest_path);
        let absolute_source = cwd.join(path);
        let relative = ArtifactRef(
            absolute_source
                .strip_prefix(
                    absolute_manifest
                        .parent()
                        .context("manifest parent missing")?,
                )?
                .to_path_buf(),
        );
        relative.validate()?;
        let bytes = snapshot
            .artifacts
            .iter()
            .find(|(name, _)| name == &relative)
            .map(|(_, bytes)| bytes)
            .context("controller binding is not a manifest artifact")?;
        let mut records = measurements::import_host(bytes.as_slice(), session_id, 1024)?;
        ensure!(
            records
                .iter()
                .all(|record| record.provenance == snapshot.manifest.provenance),
            "controller and manifest provenance mismatch"
        );
        // The retained JSONL is a normalized copy; its evidence links must point
        // at the same verified sources retained by the lifecycle importer.
        for record in &mut records {
            for reference in record.artifact_refs.iter_mut().chain(
                record
                    .measurements
                    .iter_mut()
                    .flat_map(|m| m.artifact_refs.iter_mut()),
            ) {
                let index = snapshot
                    .artifacts
                    .iter()
                    .position(|(name, _)| name == reference)
                    .context("controller record references an unbound source")?;
                *reference = ref_path(&lifecycle_source_name(index));
            }
        }
        Ok(records)
    } else {
        // Legacy standalone import has no publication wait or manifest checksum binding.
        measurements::import_host(std::io::BufReader::new(File::open(path)?), session_id, 1024)
    }
}

/// Evaluate only after cancellation and, when bound, final manifest verification/retention.
fn console_input_criterion(
    s: &Scenario,
    imported: Option<&Result<Vec<HostObservation>>>,
    r: &mut Report,
) {
    let expected = s
        .controllers()
        .iter()
        .flat_map(|c| c.actions.iter())
        .filter(|a| a.expected_delivery == Some(true))
        .count();
    let Some(imported) = imported else {
        criterion(
            r,
            "CON-input",
            Status::Incomplete,
            "console host observations were not imported",
            &[],
        );
        return;
    };
    let Ok(records) = imported else {
        criterion(
            r,
            "CON-input",
            Status::Incomplete,
            "console host observations were unreadable or for another session",
            &[],
        );
        return;
    };
    let failed = records
        .iter()
        .any(|o| o.boundary == "delivery_failed" && o.validity == Validity::Valid);
    let received = records
        .iter()
        .filter(|o| {
            o.kind == "input"
                && o.provenance == Provenance::ConsoleHardware
                && o.boundary == "host_received"
                && o.authenticated == Some(true)
                && o.validity == Validity::Valid
        })
        .count();
    if failed {
        criterion(
            r,
            "CON-input",
            Status::Fail,
            "the console host observed a declared input delivery failure",
            &["host-observations"],
        );
    } else if expected > 0 && received >= expected {
        criterion(
            r,
            "CON-input",
            Status::Pass,
            "the console host received every declared input action; console delivery remains unconfirmed",
            &["host-observations"],
        );
    } else {
        criterion(
            r,
            "CON-input",
            Status::Incomplete,
            "console host receipt is missing for one or more declared input actions",
            &["host-observations"],
        );
    }
}

fn controller_criterion(
    s: &Scenario,
    imported: Option<Result<Vec<HostObservation>>>,
    out: &Path,
    budget: &Budget,
    r: &mut Report,
) {
    let Some(imported) = imported else {
        criterion(
            r,
            "REF-controller",
            Status::Incomplete,
            "console delivery and functional response require independent host observations",
            &[],
        );
        return;
    };
    let expected: Vec<(u8, &str)> = s
        .controllers()
        .iter()
        .flat_map(|c| {
            c.actions
                .iter()
                .filter(|a| a.expected_delivery == Some(true))
                .map(move |a| (c.id, a.event_id.as_str()))
        })
        .collect();
    let imported = imported.and_then(|records| {
        let mut copy = budget.file(out, "host-observations.jsonl")?;
        for record in &records {
            copy.json(record)?;
        }
        copy.flush()?;
        Ok(records)
    });
    match imported {
        Ok(records) => {
            r.artifact_refs.push(ref_path("host-observations.jsonl"));
            let (status, reason) = controller_outcome(&expected, &records);
            criterion(r, "REF-controller", status, reason, &["host-observations"]);
        }
        Err(error) => {
            limitation(
                r,
                &format!("bound host observations were unreadable, invalid or for another session: {error}"),
            );
            criterion(
                r,
                "REF-controller",
                Status::Incomplete,
                "bound host observations could not be validated for this session",
                &[],
            );
        }
    }
}

/// Child operations observe cancellation without discarding the report/cleanup future.
async fn cancelled(cancel: &mut watch::Receiver<bool>) {
    loop {
        if *cancel.borrow() {
            return;
        }
        if cancel.changed().await.is_err() {
            return;
        }
    }
}

#[derive(Clone)]
struct Budget(Arc<Mutex<(u64, u32)>>);
impl Budget {
    fn new(bytes: u64, files: u32) -> Self {
        Self(Arc::new(Mutex::new((bytes, files))))
    }
    fn charge(&self, bytes: u64, files: u32) -> Result<()> {
        let mut left = self
            .0
            .lock()
            .map_err(|_| anyhow::anyhow!("artifact budget poisoned"))?;
        ensure!(
            bytes <= left.0 && files <= left.1,
            "declared artifact quota reached"
        );
        left.0 -= bytes;
        left.1 -= files;
        Ok(())
    }
    fn file(&self, root: &Path, name: &str) -> Result<ArtifactWriter> {
        self.charge(0, 1)?;
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(root.join(name))?;
        Ok(ArtifactWriter {
            file,
            budget: self.clone(),
        })
    }
}
struct ArtifactWriter {
    file: File,
    budget: Budget,
}
impl Write for ArtifactWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.budget
            .charge(bytes.len() as u64, 0)
            .map_err(std::io::Error::other)?;
        self.file.write(bytes)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.file.flush()
    }
}
impl ArtifactWriter {
    fn json(&mut self, value: &impl Serialize) -> Result<()> {
        serde_json::to_writer(&mut *self, value)?;
        self.write_all(b"\n")?;
        Ok(())
    }
}

fn ref_path(name: &str) -> ArtifactRef {
    ArtifactRef(PathBuf::from(name))
}

fn criterion(report: &mut Report, id: &str, status: Status, reason: &str, evidence: &[&str]) {
    if let Some(result) = report.criteria.iter_mut().find(|c| c.id == id) {
        // Preserve demonstrated failures when a later stage loses evidence.
        if result.status == Status::Fail && status != Status::Fail {
            return;
        }
        result.status = status;
        result.reason = reason.into();
        result.evidence_ids = evidence.iter().map(|s| (*s).into()).collect();
    }
}

fn limitation(report: &mut Report, reason: &str) {
    if report.limitations.len() < 120 && !report.limitations.iter().any(|r| r == reason) {
        report.limitations.push(reason.chars().take(1024).collect());
    }
}

/// Retry only the top-level manifest's read/parse. Once it is complete, consume
/// that exact parsed publication once; every validation/source error is terminal,
/// including missing files and JSON EOF in nested artifacts.
async fn await_observer_snapshot<M, T>(
    mut read_manifest: impl FnMut() -> Result<M>,
    load_manifest: impl FnOnce(M) -> Result<T>,
    deadline: tokio::time::Instant,
    poll_interval: Duration,
) -> Result<T> {
    loop {
        let error = match read_manifest() {
            Ok(manifest) => return load_manifest(manifest),
            Err(error) => error,
        };
        let unpublished = error.chain().any(|cause| {
            cause
                .downcast_ref::<std::io::Error>()
                .is_some_and(|e| e.kind() == std::io::ErrorKind::NotFound)
                || cause
                    .downcast_ref::<serde_json::Error>()
                    .is_some_and(serde_json::Error::is_eof)
        });
        let now = tokio::time::Instant::now();
        if !unpublished || now >= deadline {
            return Err(error);
        }
        tokio::time::sleep(poll_interval.min(deadline - now)).await;
        if tokio::time::Instant::now() >= deadline {
            return Err(error.context("reference observer publication deadline expired"));
        }
    }
}

async fn fetch_host_observations(
    base: &str,
    session_id: &str,
    out: &Path,
    budget: &Budget,
) -> Result<PathBuf> {
    ensure!(
        !base.is_empty() && !session_id.is_empty(),
        "empty host observation binding"
    );
    let separator = if base.contains('?') { '&' } else { '?' };
    let url = format!("{base}{separator}session={session_id}");
    let response = reqwest::Client::new()
        .get(&url)
        .timeout(Duration::from_secs(10))
        .send()
        .await?;
    ensure!(
        response.status() == reqwest::StatusCode::OK,
        "host observation endpoint returned {}",
        response.status()
    );
    let bytes = response.bytes().await?;
    ensure!(
        !bytes.is_empty() && bytes.len() <= 65_536,
        "host observation endpoint violated its bounded size"
    );
    let mut file = budget.file(out, "host-observations-source.jsonl")?;
    file.write_all(&bytes)?;
    file.flush()?;
    Ok(out.join("host-observations-source.jsonl"))
}

#[cfg(test)]
async fn reference_observer_criteria(
    s: &Scenario,
    o: &RunOptions,
    out: &Path,
    budget: &Budget,
    r: &mut Report,
    cancellation_succeeded: bool,
    wait: ObserverWait,
) {
    // Both bindings share one deadline; two absent observers must not double the
    // wait. Failed cancellation still imports available evidence once, as before.
    let deadline = tokio::time::Instant::now()
        + if cancellation_succeeded {
            wait.timeout
        } else {
            Duration::ZERO
        };
    reference_observer_criteria_until(s, o, out, budget, r, deadline, wait).await;
}

async fn reference_observer_criteria_until(
    s: &Scenario,
    o: &RunOptions,
    out: &Path,
    budget: &Budget,
    r: &mut Report,
    deadline: tokio::time::Instant,
    wait: ObserverWait,
) {
    let (lifecycle, recording) = tokio::join!(
        async {
            match &o.reference_lifecycle {
                Some(path) => Some(
                    await_observer_snapshot(
                        || crate::reference_lifecycle::read_manifest(path),
                        |manifest| {
                            crate::reference_lifecycle::load_manifest(
                                path,
                                manifest,
                                &r.session_id,
                                s.id(),
                                r.environment.host_version.as_deref(),
                            )
                        },
                        deadline,
                        wait.poll_interval,
                    )
                    .await,
                ),
                None => None,
            }
        },
        async {
            match &o.reference_recording {
                Some(path) => Some(
                    await_observer_snapshot(
                        || crate::reference_recording::read_manifest(path),
                        |manifest| {
                            crate::reference_recording::load_manifest(
                                path,
                                manifest,
                                &r.session_id,
                                s.id(),
                                r.environment.host_version.as_deref(),
                            )
                        },
                        deadline,
                        wait.poll_interval,
                    )
                    .await,
                ),
                None => None,
            }
        }
    );
    let mut controller = o.host_observations.as_ref().map(|_| {
        controller_records(
            o,
            &r.session_id,
            lifecycle.as_ref().and_then(|value| value.as_ref().ok()),
        )
    });
    if let Some(imported) = lifecycle {
        if !retain_reference_lifecycle(imported, out, budget, r) && controller.is_some() {
            controller = Some(Err(anyhow::anyhow!(
                "controller manifest could not be retained"
            )));
        }
    }
    console_input_criterion(s, controller.as_ref(), r);
    controller_criterion(s, controller, out, budget, r);
    if let Some(imported) = recording {
        retain_reference_recording(imported, out, budget, r);
    }
}

fn lifecycle_source_name(index: usize) -> String {
    format!("reference-lifecycle-source-{index:03}.dat")
}

fn retain_reference_lifecycle(
    imported: Result<crate::reference_lifecycle::Imported>,
    out: &Path,
    budget: &Budget,
    r: &mut Report,
) -> bool {
    let imported = (|| -> Result<_> {
        let mut imported = imported?;
        let pairing = imported.outcome("REF-pairing");
        let teardown = imported.outcome("REF-teardown");
        let mut retained = Vec::new();
        let mut destinations = HashMap::new();
        // Copy the already checked bytes and normalize retained paths into this run's quota.
        // Never reread an observer file after validating its digest.
        for (index, (source, data)) in imported.artifacts.iter().enumerate() {
            let name = lifecycle_source_name(index);
            let mut file = budget.file(out, &name)?;
            file.write_all(data)?;
            file.flush()?;
            let destination = ref_path(&name);
            destinations.insert(source.0.clone(), destination.clone());
            retained.push(destination);
        }
        for artifact in &mut imported.manifest.artifacts {
            artifact.path = destinations[&artifact.path.0].clone();
        }
        for observation in &mut imported.manifest.observations {
            for artifact in &mut observation.artifacts {
                *artifact = destinations[&artifact.0].clone();
            }
        }
        let mut manifest = budget.file(out, "reference-lifecycle.json")?;
        manifest.json(&imported.manifest)?;
        manifest.flush()?;
        retained.push(ref_path("reference-lifecycle.json"));
        Ok((pairing, teardown, retained))
    })();
    match imported {
        Ok((pairing, teardown, retained)) => {
            r.artifact_refs.extend(retained);
            for (id, outcome) in [("REF-pairing", pairing), ("REF-teardown", teardown)] {
                criterion(r, id, outcome.0, outcome.1, &["reference-lifecycle"]);
            }
            true
        }
        Err(_) => {
            limitation(r, "reference lifecycle observations or source artifacts were unavailable, invalid, uncorrelated or exceeded the run quota");
            for id in ["REF-pairing", "REF-teardown"] {
                criterion(
                    r,
                    id,
                    Status::Incomplete,
                    "independent reference lifecycle evidence could not be retained and verified",
                    &[],
                );
            }
            false
        }
    }
}

fn retain_reference_recording(
    imported: Result<crate::reference_recording::Imported>,
    out: &Path,
    budget: &Budget,
    r: &mut Report,
) {
    let imported = match imported {
        Ok(value) => value,
        Err(error) => {
            let missing = error.chain().any(|e| {
                e.downcast_ref::<std::io::Error>().is_some_and(|e| {
                    matches!(
                        e.kind(),
                        std::io::ErrorKind::NotFound | std::io::ErrorKind::PermissionDenied
                    )
                })
            });
            for id in ["REF-recording", "REF-report"] {
                criterion(
                    r,
                    id,
                    if missing {
                        Status::Incomplete
                    } else {
                        Status::Fail
                    },
                    "reference recording snapshot missing or invalid; see retained limitation",
                    &[],
                );
            }
            limitation(r,"reference recording snapshot could not be verified; missing sources are Incomplete and malformed or contradictory evidence fails validation");
            return;
        }
    };
    let retained = (|| -> Result<Vec<ArtifactRef>> {
        // Preserve original paths and bytes beneath a fresh private subtree, so nested
        // recording and report references remain independently inspectable.
        let root = out.join("reference-recording");
        std::fs::create_dir(&root)?;
        let mut refs = Vec::new();
        for (path, bytes) in &imported.artifacts {
            if let Some(parent) = path.0.parent() {
                std::fs::create_dir_all(root.join(parent))?;
            }
            let name = path.0.to_str().context("non-UTF8 reference artifact")?;
            let mut file = budget.file(&root, name)?;
            file.write_all(bytes)?;
            file.flush()?;
            refs.push(ArtifactRef(
                PathBuf::from("reference-recording").join(&path.0),
            ));
        }
        let mut manifest = budget.file(&root, "manifest.json")?;
        manifest.json(&imported.manifest)?;
        manifest.flush()?;
        refs.push(ref_path("reference-recording/manifest.json"));
        Ok(refs)
    })();
    match retained {
        Ok(refs) => {
            r.artifact_refs.extend(refs);
            for (id, status) in [
                ("REF-recording", imported.recording),
                ("REF-report", imported.report),
            ] {
                criterion(r,id,status,"strict independent recording/report snapshot evaluated; missing calibration or cases remain Incomplete",&["reference-recording"]);
            }
        }
        Err(_) => {
            limitation(
                r,
                "reference recording sources could not be retained within the run quota",
            );
            for (id, evaluated) in [
                ("REF-recording", imported.recording),
                ("REF-report", imported.report),
            ] {
                criterion(
                    r,
                    id,
                    if evaluated == Status::Fail {
                        Status::Fail
                    } else {
                        Status::Incomplete
                    },
                    if evaluated == Status::Fail {
                        "evaluated reference failure preserved; source retention failed"
                    } else {
                        "recording evidence retention failed; acceptance cannot be established"
                    },
                    &[],
                );
            }
        }
    }
}

fn local_report_path(root: &Path, path: &Path) -> Result<PathBuf> {
    ensure!(!path.as_os_str().is_empty(), "empty report path");
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        root.join(path)
    };
    ensure!(
        absolute.starts_with(root),
        "report must be within selected local root"
    );
    ensure!(
        absolute
            .components()
            .all(|p| !matches!(p, std::path::Component::ParentDir)),
        "report traversal forbidden"
    );
    let parent = absolute.parent().context("report parent missing")?;
    crate::capture_server::confined_root(parent)?;
    ensure!(
        !absolute.exists() && std::fs::symlink_metadata(&absolute).is_err(),
        "report path already exists"
    );
    Ok(absolute)
}

fn write_report(file: &mut File, report: &Report) -> Result<()> {
    use std::io::{Seek, SeekFrom};
    // Serialize first, so validation or encoding errors leave the prior valid journal intact.
    let mut bytes = Vec::new();
    report.write_to(&mut bytes)?;
    file.seek(SeekFrom::Start(0))?;
    file.write_all(&bytes)?;
    file.set_len(bytes.len() as u64)?;
    file.sync_all()?;
    Ok(())
}

const MAX_READY_BYTES: usize = 16_384;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct ObserverBinding {
    schema_version: String,
    run_id: String,
    session_id: String,
    scenario_id: String,
    scenario_sha256: String,
    host_version: String,
    test_client_version: String,
    nonce: String,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ObserverReady {
    schema_version: String,
    state: String,
    binding: ObserverBinding,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ObserverAttachment {
    schema_version: String,
    state: String,
    binding: ObserverBinding,
    launch_nonce: String,
    observer_id: String,
    association: String,
    authenticated: bool,
    runtime_attachment_sha256: String,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ObserverLaunch {
    schema_version: String,
    state: String,
    binding: ObserverBinding,
    launch_nonce: String,
}

struct LaunchReservation {
    envelope: ObserverLaunch,
    bytes: Vec<u8>,
    attachment_path: PathBuf,
}

impl LaunchReservation {
    fn new(path: &Path, out: &Path, budget: &Budget, binding: &ObserverBinding) -> Result<Self> {
        let absolute = std::env::current_dir()?.join(path);
        ensure!(
            !absolute
                .components()
                .any(|c| matches!(c, std::path::Component::ParentDir)),
            "attachment traversal forbidden"
        );
        let parent = absolute.parent().context("attachment parent missing")?;
        ensure!(
            parent.canonicalize()? == parent,
            "attachment parent must be canonical"
        );
        require_absent(&absolute)?;
        require_absent(&out.join("observer-launch.json"))?;
        let envelope = ObserverLaunch {
            schema_version: "1.0.0".into(),
            state: "launched".into(),
            binding: binding.clone(),
            launch_nonce: uuid::Uuid::new_v4().to_string(),
        };
        let bytes = serde_json::to_vec(&envelope)?;
        ensure!(bytes.len() <= MAX_READY_BYTES, "launch marker too large");
        budget.charge(bytes.len() as u64, 1)?;
        Ok(Self {
            envelope,
            bytes,
            attachment_path: absolute,
        })
    }
}

fn require_absent(path: &Path) -> Result<()> {
    ensure!(
        std::fs::symlink_metadata(path).is_err_and(|e| e.kind() == std::io::ErrorKind::NotFound),
        "observer destination must be absent before launch"
    );
    Ok(())
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ObserverCancel {
    schema_version: String,
    state: String,
    binding: ObserverBinding,
    request_succeeded: bool,
}

struct CancelReservation {
    succeeded: Vec<u8>,
    failed: Vec<u8>,
}

async fn cancel_with_observer_signal(
    cancellation: impl std::future::Future<Output = Result<()>>,
    timeout: Duration,
    wait: ObserverWait,
    signal: Option<CancelReservation>,
    out: &Path,
    report: &mut Report,
) -> (bool, tokio::time::Instant) {
    let succeeded = matches!(
        tokio::time::timeout(timeout, cancellation).await,
        Ok(Ok(()))
    );
    let deadline = tokio::time::Instant::now()
        + if succeeded {
            wait.timeout
        } else {
            Duration::ZERO
        };
    if let Some(signal) = signal {
        signal.publish(out, succeeded, report);
    }
    (succeeded, deadline)
}

impl CancelReservation {
    fn new(out: &Path, budget: &Budget, binding: &ObserverBinding) -> Result<Self> {
        ensure!(
            std::fs::symlink_metadata(out.join("observer-cancel.json"))
                .is_err_and(|e| e.kind() == std::io::ErrorKind::NotFound),
            "cancel destination must be absent"
        );
        let encode = |request_succeeded| {
            serde_json::to_vec(&ObserverCancel {
                schema_version: "1.0.0".into(),
                state: "host_cancel_completed".into(),
                binding: binding.clone(),
                request_succeeded,
            })
        };
        let succeeded = encode(true)?;
        let failed = encode(false)?;
        budget.charge(succeeded.len().max(failed.len()) as u64, 1)?;
        Ok(Self { succeeded, failed })
    }

    fn publish(self, out: &Path, succeeded: bool, report: &mut Report) {
        let bytes = if succeeded {
            self.succeeded
        } else {
            self.failed
        };
        match publish_observer_file(out, "observer-cancel.json", &bytes) {
            Ok(synced) => {
                report.artifact_refs.push(ref_path("observer-cancel.json"));
                if !synced {
                    limitation(
                        report,
                        "observer cancellation signal visible; directory durability unconfirmed",
                    );
                }
            }
            Err(_) => limitation(
                report,
                "observer cancellation signal could not be published",
            ),
        }
    }
}

#[cfg(any(target_os = "macos", target_os = "linux"))]
fn rename_observer_noreplace(source: &Path, destination: &Path) -> Result<()> {
    use rustix::fs::{renameat_with, RenameFlags, CWD};
    renameat_with(CWD, source, CWD, destination, RenameFlags::NOREPLACE)?;
    Ok(())
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn rename_observer_noreplace(_source: &Path, _destination: &Path) -> Result<()> {
    bail!("atomic observer publication unsupported")
}

fn publish_observer_file(out: &Path, name: &str, bytes: &[u8]) -> Result<bool> {
    publish_observer_file_with(
        out,
        name,
        bytes,
        || Ok(()),
        || File::open(out).and_then(|directory| directory.sync_all()),
    )
}

fn publish_observer_file_with(
    out: &Path,
    name: &str,
    bytes: &[u8],
    before_rename: impl FnOnce() -> Result<()>,
    directory_sync: impl FnOnce() -> std::io::Result<()>,
) -> Result<bool> {
    publish_observer_file_ops(
        out,
        name,
        bytes,
        |file, bytes| {
            file.write_all(bytes)?;
            file.sync_all()?;
            Ok(())
        },
        before_rename,
        directory_sync,
    )
}

fn publish_observer_file_ops(
    out: &Path,
    name: &str,
    bytes: &[u8],
    write_sync: impl FnOnce(&mut File, &[u8]) -> Result<()>,
    before_rename: impl FnOnce() -> Result<()>,
    directory_sync: impl FnOnce() -> std::io::Result<()>,
) -> Result<bool> {
    ensure!(
        out.canonicalize()? == out,
        "observer output must be canonical"
    );
    let temporary = out.join(format!(".observer-{}.pending", uuid::Uuid::new_v4()));
    let mut created = false;
    let operation = || -> Result<()> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .truncate(false)
            .open(&temporary)?;
        created = true;
        write_sync(&mut file, bytes)?;
        #[cfg(unix)]
        {
            use std::os::fd::IntoRawFd;
            nix::unistd::close(file.into_raw_fd())?;
        }
        #[cfg(not(unix))]
        drop(file);
        before_rename()?;
        rename_observer_noreplace(&temporary, &out.join(name))
    };
    if let Err(error) = operation() {
        if created {
            let _ = std::fs::remove_file(&temporary);
        }
        return Err(error);
    }
    // Visibility commits success. No inode mutation or rollback is allowed here.
    Ok(directory_sync().is_ok())
}

struct ReadinessContext<'a> {
    scenario: &'a Scenario,
    ready_path: Option<&'a Path>,
    out: &'a Path,
    budget: &'a Budget,
    report: &'a mut Report,
}

/// The trusted local publisher must use atomic publication. Reject links, special
/// files and replacement during acquisition; use this single byte snapshot thereafter.
#[cfg(unix)]
fn read_observer_ready(path: &Path, binding: &ObserverBinding) -> Result<Option<Vec<u8>>> {
    let Some(bytes) = read_observer_snapshot(path)? else {
        return Ok(None);
    };
    let ready: ObserverReady = serde_json::from_slice(&bytes)?;
    ensure!(
        ready.schema_version == "1.0.0" && ready.state == "ready" && ready.binding == *binding,
        "READY does not match this run"
    );
    Ok(Some(bytes))
}

#[cfg(unix)]
fn read_observer_snapshot(path: &Path) -> Result<Option<Vec<u8>>> {
    read_observer_snapshot_with(path, || {})
}

#[cfg(unix)]
fn read_observer_snapshot_with(path: &Path, after_read: impl FnOnce()) -> Result<Option<Vec<u8>>> {
    use std::io::Read;
    use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
    let identity = |m: &std::fs::Metadata| {
        (
            m.dev(),
            m.ino(),
            m.len(),
            m.mtime(),
            m.mtime_nsec(),
            m.ctime(),
            m.ctime_nsec(),
        )
    };
    let before = match std::fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        value => value?,
    };
    ensure!(
        before.is_file() && before.nlink() == 1 && before.len() <= MAX_READY_BYTES as u64,
        "invalid READY file"
    );
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(nix::libc::O_NOFOLLOW | nix::libc::O_NONBLOCK)
        .open(path)?;
    let opened = file.metadata()?;
    ensure!(
        opened.is_file() && opened.nlink() == 1 && identity(&before) == identity(&opened),
        "READY replaced before read"
    );
    let mut bytes = Vec::new();
    (&file)
        .take((MAX_READY_BYTES + 1) as u64)
        .read_to_end(&mut bytes)?;
    after_read();
    ensure!(
        bytes.len() <= MAX_READY_BYTES && !bytes.is_empty(),
        "invalid READY size"
    );
    ensure!(
        identity(&opened) == identity(&file.metadata()?)
            && identity(&opened) == identity(&std::fs::symlink_metadata(path)?),
        "READY changed during read"
    );
    Ok(Some(bytes))
}

#[cfg(not(unix))]
fn read_observer_snapshot(_path: &Path) -> Result<Option<Vec<u8>>> {
    bail!("observer snapshots require supported Unix regular-file semantics")
}

struct AttachmentContext<'a> {
    path: Option<&'a Path>,
    launch: Option<&'a LaunchReservation>,
    out: &'a Path,
    budget: &'a Budget,
    report: &'a mut Report,
}

async fn observer_before_rtsp<T>(
    context: AttachmentContext<'_>,
    cancel: &mut watch::Receiver<bool>,
    wait: ObserverWait,
    establish: impl FnOnce() -> T,
) -> Result<T> {
    let AttachmentContext {
        path,
        launch,
        out,
        budget,
        report,
    } = context;
    if let Some(path) = path {
        let operation = async {
            let launch = launch.context("attachment requires retained launch reservation")?;
            let binding = &launch.envelope.binding;
            let absolute = std::env::current_dir()?.join(path);
            ensure!(absolute == launch.attachment_path, "attachment path changed");
            let deadline = tokio::time::Instant::now() + wait.timeout;
            ensure!(!*cancel.borrow(), "cancelled before launch marker");
            if !publish_observer_file(out, "observer-launch.json", &launch.bytes)? {
                limitation(report, "observer launch marker visible; directory durability unconfirmed");
            }
            report.artifact_refs.push(ref_path("observer-launch.json"));
            let bytes = loop {
                ensure!(!*cancel.borrow(), "cancelled awaiting observer attachment");
                ensure!(tokio::time::Instant::now() < deadline, "observer attachment deadline expired");
                if let Some(bytes) = read_observer_snapshot(&absolute)? { break bytes; }
                tokio::select! {
                    biased;
                    _ = cancelled(cancel) => bail!("cancelled awaiting observer attachment"),
                    _ = tokio::time::sleep_until(deadline.min(tokio::time::Instant::now() + wait.poll_interval)) => (),
                }
            };
            let attachment: ObserverAttachment = serde_json::from_slice(&bytes)?;
            let observer = uuid::Uuid::parse_str(&attachment.observer_id)?;
            ensure!(attachment.schema_version == "1.0.0"
                && attachment.state == "attached" && attachment.binding == *binding
                && attachment.launch_nonce == launch.envelope.launch_nonce
                && observer.get_version_num() == 4 && observer.get_variant() == uuid::Variant::RFC4122 && observer.to_string() == attachment.observer_id
                && attachment.association == "exclusive_launch_candidate" && !attachment.authenticated
                && attachment.runtime_attachment_sha256.len() == 64
                && attachment.runtime_attachment_sha256.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
                "attachment does not match the coordination contract");
            let mut retained = budget.file(out, "observer-attachment.json")?;
            retained.write_all(&bytes)?;
            retained.file.sync_all()?;
            ensure!(!*cancel.borrow() && tokio::time::Instant::now() < deadline, "attachment retention exceeded deadline or was cancelled");
            report.artifact_refs.push(ref_path("observer-attachment.json"));
            Ok::<(), anyhow::Error>(())
        }.await;
        if operation.is_err() {
            limitation(report, "observer attachment unavailable, invalid, cancelled or outside quota; RTSP and input were not attempted");
        }
        operation?;
    }
    ensure!(!*cancel.borrow(), "cancelled before RTSP");
    Ok(establish())
}

#[cfg(not(unix))]
fn read_observer_ready(_path: &Path, _binding: &ObserverBinding) -> Result<Option<Vec<u8>>> {
    bail!("observer readiness requires supported Unix regular-file semantics")
}

/// Invoke the launch factory only after the optional readiness exchange is retained.
/// This is coordination, not evidence of delivery or authentication.
#[cfg(test)]
async fn observer_before_launch<T>(
    context: ReadinessContext<'_>,
    cancel: &mut watch::Receiver<bool>,
    wait: ObserverWait,
    launch: impl FnOnce() -> T,
) -> Result<T> {
    Ok(
        observer_before_launch_with_signal(context, cancel, wait, false, None, launch)
            .await?
            .0,
    )
}

async fn observer_before_launch_with_signal<T>(
    context: ReadinessContext<'_>,
    cancel: &mut watch::Receiver<bool>,
    wait: ObserverWait,
    finish_signal: bool,
    attachment_path: Option<&Path>,
    launch: impl FnOnce() -> T,
) -> Result<(T, Option<CancelReservation>, Option<LaunchReservation>)> {
    let ReadinessContext {
        scenario,
        ready_path,
        out,
        budget,
        report,
    } = context;
    ensure!(
        !finish_signal || ready_path.is_some(),
        "finish signal requires observer readiness"
    );
    let mut reservation = None;
    let mut launch_reservation = None;
    if let Some(path) = ready_path {
        let operation = async {
            use sha2::{Digest, Sha256};
            let absolute = std::env::current_dir()?.join(path);
            ensure!(!absolute.components().any(|c| matches!(c, std::path::Component::ParentDir)), "READY traversal forbidden");
            let parent = absolute.parent().context("READY parent missing")?;
            ensure!(parent.canonicalize()? == parent, "READY parent must not contain symlinks");
            match std::fs::symlink_metadata(&absolute) {
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
                _ => bail!("READY destination must be absent before binding publication"),
            }
            let mut scenario_digest = Sha256::new();
            serde_json::to_writer(&mut scenario_digest, scenario.as_value())?;
            let binding = ObserverBinding {
                schema_version: "1.0.0".into(),
                run_id: report.run_id.clone(),
                session_id: report.session_id.clone(),
                scenario_id: scenario.id().into(),
                scenario_sha256: hex::encode(scenario_digest.finalize()),
                host_version: report.environment.host_version.clone().context("host version unavailable")?,
                test_client_version: env!("CARGO_PKG_VERSION").into(),
                nonce: uuid::Uuid::new_v4().to_string(),
            };
            let bytes = serde_json::to_vec(&binding)?;
            ensure!(bytes.len() <= MAX_READY_BYTES / 2, "observer binding too large");
            if finish_signal { reservation = Some(CancelReservation::new(out, budget, &binding)?); }
            if let Some(path) = attachment_path { launch_reservation = Some(LaunchReservation::new(path, out, budget, &binding)?); }
            budget.charge(bytes.len() as u64, 1)?;
            if !publish_observer_file(out, "observer-binding.json", &bytes)? {
                limitation(report, "observer binding visible; directory durability unconfirmed");
            }
            report.artifact_refs.push(ref_path("observer-binding.json"));
            let deadline = tokio::time::Instant::now() + wait.timeout;
            let bytes = loop {
                ensure!(!*cancel.borrow(), "cancelled before observer readiness");
                ensure!(tokio::time::Instant::now() < deadline, "observer readiness deadline expired");
                match read_observer_ready(&absolute, &binding) {
                    Ok(Some(bytes)) => break bytes,
                    Ok(None) => (),
                    Err(error) => return Err(error),
                }
                tokio::select! {
                    biased;
                    _ = cancelled(cancel) => bail!("cancelled awaiting observer readiness"),
                    _ = tokio::time::sleep_until(deadline.min(tokio::time::Instant::now() + wait.poll_interval)) => (),
                }
            };
            let mut retained = budget.file(out, "observer-ready.json")?;
            retained.write_all(&bytes)?;
            retained.file.sync_all()?;
            report.artifact_refs.push(ref_path("observer-ready.json"));
            Ok::<(), anyhow::Error>(())
        }.await;
        if operation.is_err() {
            limitation(report, "observer readiness unavailable, invalid, cancelled or outside the run quota; host launch was not attempted");
        }
        operation?;
    }
    ensure!(!*cancel.borrow(), "cancelled before launch");
    if let Some(reservation) = &launch_reservation {
        require_absent(&reservation.attachment_path)?;
    }
    ensure!(
        attachment_path.is_none() || launch_reservation.is_some(),
        "attachment requires readiness"
    );
    Ok((launch(), reservation, launch_reservation))
}

/// Invalid declarations/destinations fail before contacting a host. Once admitted, operation
/// errors and cancellation become Incomplete reports with every declared criterion retained.
pub async fn run(
    scenario: &Scenario,
    options: &RunOptions,
    mut cancel: watch::Receiver<bool>,
) -> Result<Report> {
    ensure!(
        !options.observer_finish_signal || options.observer_ready.is_some(),
        "finish signal requires observer readiness"
    );
    ensure!(
        options.observer_attachment.is_none() || options.observer_ready.is_some(),
        "attachment requires observer readiness"
    );
    let root = crate::capture_server::confined_root(&options.root)?;
    let report_path = local_report_path(&root, &options.report)?;
    let environment = Environment {
        firmware: None,
        host_version: None,
        client_version: env!("CARGO_PKG_VERSION").into(),
        network: "client observations; topology unverified".into(),
        devices: Vec::new(),
        reference_chain: None,
        clocks: vec![clock::local_clock()?],
        calibration_ids: Vec::new(),
    };
    let mut report = Report::new_ref(
        scenario.as_value(),
        environment,
        uuid::Uuid::new_v4().to_string(),
        uuid::Uuid::new_v4().to_string(),
    )?;
    let mut journal = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&report_path)?;
    write_report(&mut journal, &report)?;
    let result = execute(scenario, options, &root, &mut report, &mut cancel).await;
    if result.is_err() {
        // Do not retain HTTP/provider error strings: they may contain pairing secrets.
        limitation(
            &mut report,
            "run could not complete; unavailable observations remain Incomplete",
        );
    }
    report.finalize(
        result
            .err()
            .map(|_| "session ended before all required evidence was available"),
    )?;
    write_report(&mut journal, &report)?;
    Ok(report)
}

async fn execute(
    s: &Scenario,
    o: &RunOptions,
    root: &Path,
    r: &mut Report,
    cancel: &mut watch::Receiver<bool>,
) -> Result<()> {
    ensure!(
        s.duration_seconds() <= MAX_RUN_SECONDS,
        "run exceeds bounded observation duration"
    );
    let output = s.resolve_output(root)?;
    ensure!(
        !output.exists(),
        "scenario artifact destination already exists"
    );
    let output = crate::capture_server::confined_root(&output)?;
    // Report refs are relative to the scenario output directory.
    let budget = Budget::new(s.write_policy().max_bytes, s.write_policy().max_files);
    if r.external_declaration() {
        let mut declaration = budget.file(&output, "scenario.json")?;
        serde_json::to_writer(&mut declaration, s.as_value())?;
        declaration.flush()?;
    }
    let mut lifecycle = budget.file(&output, "lifecycle.jsonl")?;
    r.artifact_refs.push(ref_path("lifecycle.jsonl"));
    lifecycle.json(&json!({"event":"declared","scenario_id":s.id(),"session_id":r.session_id,"time_us":clock::monotonic_us()?}))?;
    let paths = crate::config::Paths::resolve(o.config_dir.as_deref())?;
    let identity = crate::identity::ClientIdentity::load_or_create(&paths)?;
    let host = http::Host {
        address: s.host().into(),
        http_port: o.http_port,
    };
    let delivery = pairing::scenario_delivery(s.pairing(), &o.providers)?;
    let prepare = async {
        if let Some(delivery) = &delivery {
            pairing::pair_unattended(&paths, &identity, &host, &pairing::random_pin(), delivery)
                .await?;
        }
        let paired = http::PairedHost::connect(&paths, &identity, &host).await?;
        let info = paired.server_info().await?;
        let apps = paired.app_list().await?;
        let app_id = match o.app_id {
            Some(id) if apps.iter().any(|a| a.id == id) => id,
            Some(_) => bail!("declared app unavailable"),
            None if apps.len() == 1 => apps[0].id,
            None => bail!("explicit app ID required for a multi-app host"),
        };
        Ok::<_, anyhow::Error>((paired, info, app_id))
    };
    let (paired, info, app_id) = tokio::select! {
        result = tokio::time::timeout(Duration::from_secs(350),prepare) => result??,
        _ = cancelled(cancel) => bail!("run cancelled before launch"),
    };
    r.environment.host_version = Some(info.app_version.clone());
    lifecycle
        .json(&json!({"event":"paired_connection_verified","time_us":clock::monotonic_us()?}))?;
    // A stored pairing and successful HTTPS do not exercise first-time pairing rejection cases.
    limitation(
        r,
        "pairing connection does not establish the complete official-client acceptance matrix",
    );
    let m = s.requested();
    let codec = match m.codec {
        crate::scenario::Codec::H264 => crate::codec::Codec::H264,
        crate::scenario::Codec::Hevc => crate::codec::Codec::Hevc,
        crate::scenario::Codec::Av1 => crate::codec::Codec::Av1,
    };
    let mut request = http::LaunchRequest::with_random_keys(
        app_id,
        m.width,
        m.height,
        m.fps,
        m.dynamic_range == DynamicRange::Hdr,
    );
    request.session_id = r.session_id.clone();
    let settings = rtsp::StreamSettings {
        width: m.width,
        height: m.height,
        fps: m.fps,
        bitrate_kbps: m.bitrate_kbps,
        codec,
        hdr: request.hdr,
        packet_size: rtsp::DEFAULT_PACKET_SIZE,
        // Moonlight Qt f786e94c session.cpp:648-658 opts into audio encryption on Mac.
        encrypt_av: true,
    };
    settings.validate()?;
    let (launch_future, cancel_signal, observer_launch) = observer_before_launch_with_signal(
        ReadinessContext {
            scenario: s,
            ready_path: o.observer_ready.as_deref(),
            out: &output,
            budget: &budget,
            report: r,
        },
        cancel,
        OBSERVER_WAIT,
        o.observer_finish_signal,
        o.observer_attachment.as_deref(),
        || paired.launch(&request),
    )
    .await?;
    // A cancelled launch may have reached the host: always send cancel after attempting it.
    let launch = tokio::select! {
        result = launch_future => result,
        _ = cancelled(cancel) => Err(anyhow::anyhow!("cancelled during launch")),
    };
    let result = match launch {
        Ok(url) => {
            let established = async {
                let establish = observer_before_rtsp(
                    AttachmentContext {
                        path: o.observer_attachment.as_deref(),
                        launch: observer_launch.as_ref(),
                        out: &output,
                        budget: &budget,
                        report: r,
                    },
                    cancel,
                    OBSERVER_WAIT,
                    || rtsp::establish(s.host(), &url, request.rikey, &settings),
                )
                .await?;
                tokio::select! {
                result = establish => result,
                _ = cancelled(cancel) => Err(anyhow::anyhow!("cancelled during negotiation")),
                }
            }
            .await;
            match established {
                Ok(mut session) => {
                    limitation(r,"RTSP acceptance is not evidence of the encoder chosen bitrate or supported bounds");
                    let result = async {
                        lifecycle.json(&json!({"event":"active","time_us":clock::monotonic_us()?,"negotiation":session.negotiation}))?;
                        observe(ObserveContext {scenario:s,options:o,out:&output,budget:&budget,request:&request},r,&mut session,cancel).await
                    }.await;
                    // Input release is the first operation inside observe's cleanup, before media drain.
                    let teardown = session.teardown().await;
                    if teardown.is_err() {
                        limitation(r, "control disconnect could not be confirmed");
                    }
                    result.and(teardown)
                }
                Err(error) => Err(error),
            }
        }
        Err(error) => Err(error),
    };
    let (cancellation_succeeded, observer_deadline) = cancel_with_observer_signal(
        paired.cancel(),
        Duration::from_secs(10),
        OBSERVER_WAIT,
        cancel_signal,
        &output,
        r,
    )
    .await;
    let log_result = clock::monotonic_us().and_then(|time_us| lifecycle.json(&json!({"event":"host_cancel_attempted","time_us":time_us,"request_succeeded":cancellation_succeeded})));
    if log_result.is_err() {
        limitation(
            r,
            "host cancellation journal unavailable; observer import still attempted",
        );
    }
    if !cancellation_succeeded {
        limitation(r, "host cancellation could not be confirmed");
    }
    limitation(
        r,
        "transport teardown is not proof of host controller release or resource cleanup",
    );
    let mut controller_options = o.clone();
    if let Some(base) = &o.host_observations_url {
        match fetch_host_observations(base, &r.session_id, &output, &budget).await {
            Ok(path) => {
                controller_options.host_observations = Some(path);
                r.input
                    .host_received
                    .push(ref_path("host-observations.jsonl"));
            }
            Err(error) => {
                limitation(
                    r,
                    &format!("session-bound host observations unavailable: {error}"),
                );
            }
        }
    }
    reference_observer_criteria_until(
        s,
        &controller_options,
        &output,
        &budget,
        r,
        observer_deadline,
        OBSERVER_WAIT,
    )
    .await;
    result?;
    ensure!(cancellation_succeeded, "host cancel failed");
    Ok(())
}

struct MediaResult {
    video: video::VideoReport,
    audio: crate::audio::AudioPopulation,
    stamps: Vec<decode::FrameStamp>,
    codec: crate::codec::Codec,
    interval: Interval,
    bytes: u64,
}

/// Aborting an outer request cannot leave a media receiver detached from its owner.
struct MediaTask(tokio::task::JoinHandle<Result<MediaResult>>);
impl Drop for MediaTask {
    fn drop(&mut self) {
        self.0.abort();
    }
}

struct ObserveContext<'a> {
    scenario: &'a Scenario,
    options: &'a RunOptions,
    out: &'a Path,
    budget: &'a Budget,
    request: &'a http::LaunchRequest,
}

async fn observe(
    context: ObserveContext<'_>,
    r: &mut Report,
    session: &mut rtsp::ActiveSession,
    cancel: &mut watch::Receiver<bool>,
) -> Result<()> {
    let ObserveContext {
        scenario: s,
        options: o,
        out,
        budget,
        request,
    } = context;
    let peer = tokio::net::lookup_host((s.host(), session.negotiation.video_port))
        .await?
        .next()
        .context("no host endpoint")?
        .ip();
    let local = if peer.is_ipv4() {
        "0.0.0.0:0"
    } else {
        "[::]:0"
    };
    let video_socket = UdpSocket::bind(local).await?;
    let audio_socket = UdpSocket::bind(local).await?;
    let video_file = budget.file(out, "video.jsonl")?;
    let audio_file = budget.file(out, "audio.jsonl")?;
    let elementary = budget.file(out, "elementary.bin")?;
    let mut controls = budget.file(out, "control.jsonl")?;
    let mut inputs = budget.file(out, "input.jsonl")?;
    r.artifact_refs.extend([
        ref_path("video.jsonl"),
        ref_path("audio.jsonl"),
        ref_path("elementary.bin"),
        ref_path("control.jsonl"),
        ref_path("input.jsonl"),
    ]);
    r.hdr_events.push(ref_path("control.jsonl"));
    r.input.client_sent.push(ref_path("input.jsonl"));
    let (stop, stop_rx) = watch::channel(false);
    let (hdr_tx, hdr_rx) = watch::channel(None);
    let worker = MediaWorker {
        video_socket,
        audio_socket,
        peer,
        negotiation: session.negotiation.clone(),
        request: request.clone(),
        video_file,
        audio_file,
        elementary,
    };
    let media = worker.receive(stop_rx, hdr_rx);
    // Media gets its own runtime worker; input scheduling never awaits media filesystem writes.
    let mut media_task = MediaTask(tokio::spawn(media));
    let reference_path = out.join("reference");
    let mut reference_task = None;
    if s.reference().required || s.evidence().reference_recording {
        let binding = s
            .reference()
            .device_ref
            .as_ref()
            .and_then(|id| o.references.get(id));
        if let Some(binding) = binding {
            let bytes_per_frame =
                u64::from(s.requested().width) * u64::from(s.requested().height) * 3
                    / if request.hdr { 1 } else { 2 };
            let reserved = (s.duration_seconds() * f64::from(s.requested().fps)).ceil() as u64;
            if budget
                .charge(
                    reserved
                        .saturating_add(2)
                        .saturating_mul(bytes_per_frame)
                        .saturating_add(4_194_304),
                    4,
                )
                .is_ok()
            {
                // Scoped future: dropping it cancels the helper and releases capture ownership.
                reference_task = Some(Box::pin(binding.record(
                    s.duration_seconds(),
                    &reference_path,
                    request.hdr,
                )));
            } else {
                limitation(r, "reference capture exceeds declared artifact quota");
            }
        } else {
            limitation(r, "required reference device binding unavailable");
        }
    }
    let mut pads = input::ControllerSession::new(session.negotiation.describe.feature_flags);
    let mut next = [0usize; 4];
    let start = tokio::time::Instant::now();
    let mut last_input_time = 0;
    let mut ticks = tokio::time::interval(Duration::from_millis(1));
    ticks.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut captured = false;
    let mut failure = None;
    loop {
        tokio::select! {
            biased;
            _ = cancelled(cancel) => { failure = Some("run cancelled"); break; }
            _ = tokio::time::sleep_until(start + Duration::from_secs_f64(s.duration_seconds())) => break,
            result = &mut media_task.0 => {
                // An early media failure stops the safe session; no producer can keep growing.
                let _ = result;
                failure = Some("media receive or artifact quota failed"); break;
            }
            result = async { reference_task.as_mut().unwrap().await }, if reference_task.is_some() => {
                captured = result.is_ok();
                if !captured { limitation(r,"reference acquisition unavailable or invalid"); }
                reference_task = None;
            }
            _ = ticks.tick() => {
                let elapsed = start.elapsed().as_secs_f64();
                let now = match clock::monotonic_us() {Ok(now)=>now,Err(_)=>{failure=Some("input clock unavailable");break;}};
                last_input_time=now;
                for c in s.controllers() {
                    while let Some(action) = c.actions.get(next[c.id as usize]).filter(|a| a.at_seconds <= elapsed) {
                        let outcome = pads.apply(c.id,action,now,|p|session.control.send_input(&p.bytes,p.channel,p.reliable));
                        next[c.id as usize] += 1;
                        match outcome {
                            Ok(outcome) => {
                                if outcome.unavailable.is_some() || !outcome.transport_errors.is_empty() { limitation(r,"controller action lacked supported transport evidence"); }
                                if inputs.json(&outcome).is_err() { failure = Some("input artifact quota reached"); break; }
                            }
                            Err(_) => { failure = Some("controller action failed"); break; }
                        }
                    }
                }
                match session.control.poll() {
                    Ok(Some(event)) => {
                        let now = match clock::monotonic_us() {Ok(now)=>now,Err(_)=>{failure=Some("control clock unavailable");break;}};
                        let _ = pads.observe_feedback(&event.event,now);
                        if let Some(hdr) = decode::HdrSignal::from_control(&event.event,now) { let _ = hdr_tx.send(Some(hdr)); }
                        let terminated = matches!(event.event,control::ControlEvent::Termination{..}|control::ControlEvent::Disconnected{..});
                        if controls.json(&event).is_err() { failure=Some("control artifact quota reached"); }
                        if terminated { failure=Some("host terminated session"); }
                    }
                    Ok(None) => (), Err(_) => failure=Some("control transport lost"),
                }
                let feedback_time=clock::monotonic_us().unwrap_or(now);
                for c in s.controllers() {
                    if let Ok(Some(result))=pads.rumble_result(c.id,feedback_time) {
                        if result!=input::FeedbackResult::Pending {
                            if inputs.json(&json!({"controller":c.id,"feedback":result,"time_us":feedback_time})).is_err() {failure=Some("feedback artifact quota reached");}
                            if result==input::FeedbackResult::TimedOut {
                                criterion(r,"REF-controller",Status::Fail,"required rumble feedback timed out",&["controller-feedback"]);
                            }
                        }
                    }
                }
                if failure.is_some() { break; }
            }
        }
    }
    // Unconditional input-first cleanup, including cancellation and quota errors.
    let cleanup = pads.release_all(clock::monotonic_us().unwrap_or(last_input_time), |p| {
        session.control.send_input(&p.bytes, p.channel, p.reliable)
    });
    if let Ok(outcomes) = &cleanup {
        for outcome in outcomes {
            let _ = inputs.json(outcome);
        }
    }
    let _ = stop.send(true);
    if media_task.0.is_finished() {
        if let Some(reason) = failure {
            return Err(anyhow::anyhow!(reason));
        }
    }
    let media = tokio::time::timeout(Duration::from_secs(5), &mut media_task.0).await;
    let media = match media {
        Ok(result) => result??,
        Err(_) => {
            bail!("media shutdown deadline");
        }
    };
    if let Some(mut pending) = reference_task {
        // Recording may finish a little after observation. Cancellation does not wait for it.
        tokio::select! {
            result = tokio::time::timeout(Duration::from_secs(5),&mut pending) => captured=matches!(result,Ok(Ok(()))),
            _ = cancelled(cancel) => (),
        }
    }
    let mut summary = budget.file(out, "receipt-summary.json")?;
    summary.json(&json!({"video":media.video,"audio":media.audio,"session_id":r.session_id}))?;
    r.artifact_refs.push(ref_path("receipt-summary.json"));
    let sequence_observed = media.video.frames_complete > 0
        || media.video.frames_incomplete > 0
        || media.video.frames_at_session_boundary > 0;
    r.video.missing_frames = sequence_observed.then_some(media.video.frames_missing);
    r.video.incomplete_frames = sequence_observed.then_some(media.video.frames_incomplete as u64);
    r.video.recoverable_with_fec =
        sequence_observed.then_some(media.video.frames_recoverable_with_fec as u64);
    r.audio.non_silent = media.audio.non_silent();
    // Client receipt values keep their declared observation boundary, unknown accuracy and source population.
    let frame_population = media.video.frames_complete as u64
        + media.video.frames_incomplete as u64
        + media.video.frames_missing;
    r.video.frame_rate = receipt_measurement(
        "frame-rate",
        r,
        media.video.fps,
        Units::FramesPerSecond,
        (frame_population, media.video.frames_complete as u64),
        &media.interval,
        "video.jsonl",
    )?;
    let span = media.interval.end.saturating_sub(media.interval.start);
    let bitrate = (span > 0).then(|| media.bytes as f64 * 8_000_000.0 / span as f64);
    r.video.measured_bitrate = receipt_measurement(
        "measured-bitrate",
        r,
        bitrate,
        Units::BitsPerSecond,
        (media.video.packets, media.video.packets),
        &media.interval,
        "video.jsonl",
    )?;
    r.audio.packet_loss = receipt_measurement(
        "audio-loss",
        r,
        media.audio.loss_fraction(),
        Units::Ratio,
        (media.audio.expected_packets, media.audio.expected_packets),
        &media.interval,
        "audio.jsonl",
    )?;
    if let Some(interval) = &media.video.frame_interval_ms {
        r.video.frame_intervals = receipt_measurement(
            "frame-intervals",
            r,
            Some(interval.p99),
            Units::Milliseconds,
            (
                frame_population.saturating_sub(1),
                media.video.frames_complete.saturating_sub(1) as u64,
            ),
            &media.interval,
            "video.jsonl",
        )?;
    }
    r.video
        .protocol_processing_latency
        .artifact_refs
        .push(ref_path("video.jsonl"));
    r.video.protocol_processing_latency.reason=Some("per-frame pre-send header values retained; required host clock/population validity unavailable".into());
    if let Some(reason) = failure {
        limitation(r, reason);
        bail!("{reason}");
    }
    cleanup?;
    if media.stamps.is_empty() || media.audio.decoded_packets == 0 {
        criterion(
            r,
            "REF-stream",
            Status::Fail,
            "declared stream interval had no complete video or decoded stereo audio",
            &["receipt-summary"],
        );
    }
    if !media.stamps.is_empty() {
        let decoded = tokio::select! {
            result=decode_recording(out,budget,request,&media,s.requested().fps)=>result,
            _=cancelled(cancel)=>Err(anyhow::anyhow!("cancelled during native decode")),
        };
        if decoded.is_err() {
            limitation(r, "native stream decode or format verification unavailable");
        }
        if decoded.is_ok() && media.audio.decoded_packets > 0 {
            criterion(r,"REF-stream",Status::Pass,"declared native video decoded with stereo audio; host-internal evidence is separate",&["receipt-summary"]);
        }
        if captured && decoded.is_ok() {
            // Full comparison is local; calibrated acceptance still belongs to the frozen profile.
            let comparison = tokio::select! {
                result=compare_recordings(out,budget)=>result,
                _=cancelled(cancel)=>Err(anyhow::anyhow!("cancelled during comparison")),
            };
            match comparison {
                Ok(report) => {
                    r.fidelity.compared_frames = report
                        .fidelity
                        .as_ref()
                        .map_or(0, |f| f.frames_scored as u64);
                    r.fidelity.excluded_frames = report.excluded_frames as u64;
                    r.fidelity
                        .per_frame_scores
                        .push(ref_path("comparison/frame-results.jsonl"));
                    r.artifact_refs.push(ref_path("comparison/report.json"));
                    limitation(r,"comparison calibration and HDMI delay uncertainty must be established before acceptance");
                }
                Err(_) => limitation(r, "required native comparison unavailable"),
            }
        }
    }
    Ok(())
}

fn receipt_measurement(
    id: &str,
    r: &Report,
    value: Option<f64>,
    units: Units,
    population: (u64, u64),
    interval: &Interval,
    artifact: &str,
) -> Result<Measurement> {
    let (expected, count) = population;
    let mut m =
        Measurement::unavailable(id, &r.session_id, "client receipt accuracy is uncalibrated");
    m.units = units;
    m.provenance = Provenance::OperatorObservation;
    m.source = Source::ClientReceipt;
    m.artifact_refs.push(ref_path(artifact));
    m.population.observed = Some(count);
    if let Some(value) = value.filter(|_| count > 0 && expected == count) {
        m.value = Some(value);
        m.validity = Validity::Valid;
        m.reason = None;
        m.interval = Some(interval.clone());
        let clock = clock::local_clock()?;
        m.clock_domain = Some(clock.id.clone());
        m.source_clock = Some(clock);
        m.resolution = Some(0.000001);
        m.population.expected = Some(count);
        m.population.represented = Some(count);
        m.population.excluded = Some(0);
        m.population.missing = Some(0);
        m.population.overflow = Some(0);
        m.coverage = Coverage {
            state: CoverageState::Complete,
            enabled_intervals: vec![interval.clone()],
            gaps: Vec::new(),
            reason: None,
        };
    }
    m.validate()?;
    Ok(m)
}

struct MediaWorker {
    video_socket: UdpSocket,
    audio_socket: UdpSocket,
    peer: std::net::IpAddr,
    negotiation: rtsp::RtspSession,
    request: http::LaunchRequest,
    video_file: ArtifactWriter,
    audio_file: ArtifactWriter,
    elementary: ArtifactWriter,
}
impl MediaWorker {
    async fn receive(
        self,
        mut stop: watch::Receiver<bool>,
        hdr: watch::Receiver<Option<decode::HdrSignal>>,
    ) -> Result<MediaResult> {
        let Self {
            video_socket,
            audio_socket,
            peer,
            negotiation: n,
            request,
            mut video_file,
            mut audio_file,
            mut elementary,
        } = self;
        // Configuration is copied before spawning; no borrowed session owner enters this worker.
        let video_remote = std::net::SocketAddr::new(peer, n.video_port);
        let audio_remote = std::net::SocketAddr::new(peer, n.audio_port);
        ensure!(
            n.encryption_enabled & rtsp::ENC_VIDEO == 0,
            "encrypted video acquisition unavailable"
        );
        let mut audio = crate::audio::AudioReceiver::new(
            5,
            64,
            (n.requested.encrypt_av || n.encryption_enabled & rtsp::ENC_AUDIO != 0)
                .then_some(request.rikey),
            request.rikeyid,
        )?;
        let mut assembler = video::FrameReassembler::default();
        let mut video_buf = vec![0; 65536];
        let mut audio_buf = vec![0; 2048];
        let mut timer = tokio::time::interval(Duration::from_millis(500));
        let mut sequence = 0u32;
        let mut frames = Vec::new();
        let mut stamps = Vec::new();
        let mut signals = Vec::new();
        let started = clock::monotonic_us()?;
        let mut bytes = 0u64;
        loop {
            tokio::select! {
                _ = cancelled(&mut stop) => break,
                _ = timer.tick() => {
                    sequence=sequence.wrapping_add(1);
                    for (socket,remote,payload) in [(&video_socket,video_remote,&n.video_ping_payload),(&audio_socket,audio_remote,&n.ping_payload)] {
                        let mut ping=payload.as_bytes().to_vec();
                        if ping.is_empty() {ping.extend_from_slice(b"PING");} else {ping.extend_from_slice(&sequence.to_be_bytes());}
                        socket.send_to(&ping,remote).await?;
                    }
                }
                received=video_socket.recv_from(&mut video_buf) => {
                    let (size,from)=received?;
                    if from != video_remote {continue;}
                    bytes=bytes.checked_add(size as u64).context("received byte overflow")?;
                    if let Some(done)=assembler.push(&video_buf[..size],clock::monotonic_seconds()?) {
                        store_frame(done,&mut video_file,&mut elementary,&mut frames,&mut stamps,&hdr,&mut signals)?;
                    }
                }
                received=audio_socket.recv_from(&mut audio_buf) => {
                    let (size,from)=received?;
                    if from != audio_remote {continue;}
                    for observed in audio.push(&audio_buf[..size],clock::monotonic_us()?)? {audio_file.json(&observed)?;}
                }
            }
        }
        if let Some(done) = assembler.finish() {
            store_frame(
                done,
                &mut video_file,
                &mut elementary,
                &mut frames,
                &mut stamps,
                &hdr,
                &mut signals,
            )?;
        }
        for observed in audio.finish()? {
            audio_file.json(&observed)?;
        }
        elementary.flush()?;
        let report = video::summarize(
            &frames,
            assembler.packets,
            assembler.parity_packets,
            assembler.frames_missing,
            None,
        );
        Ok(MediaResult {
            video: report,
            audio: audio.population().clone(),
            stamps,
            codec: n.announced.codec,
            interval: Interval {
                start: started,
                end: clock::monotonic_us()?,
            },
            bytes,
        })
    }
}

fn store_frame(
    done: (video::FrameRecord, Option<Vec<u8>>),
    log: &mut ArtifactWriter,
    elementary: &mut ArtifactWriter,
    frames: &mut Vec<video::FrameRecord>,
    stamps: &mut Vec<decode::FrameStamp>,
    hdr: &watch::Receiver<Option<decode::HdrSignal>>,
    signals: &mut Vec<decode::HdrSignal>,
) -> Result<()> {
    ensure!(
        (frames.len() as u64) < MAX_FRAMES,
        "frame observation capacity reached"
    );
    let (record, bytes) = done;
    log.json(&record)?;
    if let Some(bytes) = bytes {
        elementary.write_all(&bytes)?;
        let signal = hdr.borrow().clone();
        if let Some(signal) = &signal {
            if signals
                .last()
                .is_none_or(|last| last.observed_us != signal.observed_us)
            {
                signals.push(signal.clone());
            }
        }
        stamps.push(decode::FrameStamp {
            frame_id: u64::from(record.frame_index),
            received_us: (record.last_packet_at * 1_000_000.0).floor() as u64,
            clock_domain: clock::DOMAIN.into(),
            hdr_signal: signal,
        });
    }
    frames.push(record);
    Ok(())
}

async fn decode_recording(
    out: &Path,
    budget: &Budget,
    request: &http::LaunchRequest,
    media: &MediaResult,
    fps: u32,
) -> Result<()> {
    let codec = media.codec;
    // The inspector, not a desired label, supplies native precision and color interpretation.
    let info = decode::probe(&out.join("elementary.bin"), codec).await?;
    let format =
        decode::NativeFormat::from_info(&info, request.width, request.height, request.hdr)?;
    let stream = crate::capture_server::confined_root(&out.join("stream"))?;
    let mut raw = budget.file(&stream, "frames.yuv")?;
    let mut times = Vec::new();
    let limits = decode::DecodeLimits {
        max_frames: MAX_FRAMES,
        max_output_bytes: budget
            .0
            .lock()
            .map_err(|_| anyhow::anyhow!("quota poisoned"))?
            .0,
        max_frame_bytes: format.frame_bytes()?,
        timeout: Duration::from_secs(60),
    };
    decode::decode_native(
        &out.join("elementary.bin"),
        codec,
        &format,
        media.stamps.clone(),
        limits,
        |frame| {
            raw.write_all(&frame.pixels)?;
            times.push(frame.stamp.received_us);
            Ok(())
        },
    )
    .await?;
    let metadata = crate::recording::Recording {
        source: "native received stream".into(),
        capture_mode: crate::recording::CaptureMode {
            width: request.width,
            height: request.height,
            fps,
        },
        calibration_id: None,
        calibration_reason: Some("capture chain uncalibrated".into()),
        frames: Some(ref_path("frames.yuv")),
        pixel_format: if request.hdr {
            crate::recording::PixelFormat::Yuv420p10le
        } else {
            crate::recording::PixelFormat::Yuv420p
        },
        bit_depth: format.bit_depth,
        color_space: if request.hdr {
            crate::recording::ColorSpace::PqBt2020
        } else {
            crate::recording::ColorSpace::Bt709
        },
        clock_domain: clock::DOMAIN.into(),
        frame_count: times.len() as u64,
        timestamp_count: times.len() as u64,
        timestamps: Some(times.clone()),
        validity: Validity::Valid,
        metadata_evidence: None,
    };
    metadata.validate()?;
    let index = reference::RecordingIndex {
        source: metadata.source.clone(),
        width: request.width as usize,
        height: request.height as usize,
        timestamps: times.iter().map(|v| *v as f64 / 1_000_000.0).collect(),
    };
    budget.file(&stream, "recording.json")?.json(&metadata)?;
    budget.file(&stream, "frames.json")?.json(&index)?;
    Ok(())
}

async fn compare_recordings(out: &Path, budget: &Budget) -> Result<crate::compare::CompareReport> {
    // Comparison scratch/files are bounded by its implementation; reserve its worst-case output
    // before entering it rather than letting the scenario quota silently grow.
    let stream = reference::Recording::load(&out.join("stream"))?;
    let reference = reference::Recording::load(&out.join("reference"))?;
    let pixels = (stream.index.width as u64)
        .checked_mul(stream.index.height as u64)
        .context("comparison geometry overflow")?;
    // JSONL, six native samples, worst-case PNG payloads and metadata. Reserve conservatively
    // before the comparator writes; its scratch population has a separate bounded lifetime.
    let bytes = (stream.frame_count() as u64)
        .checked_mul(4096)
        .and_then(|v| {
            pixels
                .checked_mul(48)
                .and_then(|samples| v.checked_add(samples))
        })
        .and_then(|v| v.checked_add(4 * 1024 * 1024))
        .context("comparison reservation overflow")?;
    budget.charge(bytes, 32)?;
    let comparison = out.join("comparison");
    let report = crate::compare::compare(
        &stream,
        &reference,
        &crate::compare::CompareOptions::default(),
        &comparison,
    )
    .await?;
    let mut file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(comparison.join("report.json"))?;
    serde_json::to_writer(&mut file, &report)?;
    Ok(report)
}

/// A comparison profile freezes format, full population, calibration identity and numeric rules.
/// Null thresholds or unknown calibration uncertainty are supported declarations of Incomplete.
#[derive(Debug, Clone, serde::Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ComparisonProfile {
    pub schema_version: String,
    pub population: String,
    pub width: u32,
    pub height: u32,
    pub bit_depth: u8,
    pub color_space: crate::recording::ColorSpace,
    pub calibration_id: Option<String>,
    pub options: crate::compare::CompareOptions,
    pub criteria: Vec<PictureCriterion>,
}
#[derive(Debug, Clone, serde::Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PictureCriterion {
    pub id: String,
    pub metric: PictureMetric,
    pub threshold: crate::verdict::Threshold,
    pub uncertainty: Option<f64>,
}
#[derive(Debug, Clone, Copy, serde::Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PictureMetric {
    SsimFloorFraction,
    SsimMedian,
    RelativeDelayMs,
}
impl ComparisonProfile {
    pub(crate) fn validate(&self) -> Result<()> {
        ensure!(
            self.schema_version == "1.0.0" && self.population == "all_stream_frames",
            "unsupported comparison profile version or population"
        );
        ensure!(
            self.width > 0 && self.height > 0 && [8, 10].contains(&self.bit_depth),
            "invalid comparison format"
        );
        ensure!(
            !self.criteria.is_empty() && self.criteria.len() <= 128,
            "comparison criterion capacity"
        );
        let o = &self.options;
        ensure!(
            [
                o.min_delay_ms,
                o.max_delay_ms,
                o.min_motion,
                o.min_confidence,
                o.blank_std_dev
            ]
            .iter()
            .all(|v| v.is_finite())
                && o.min_delay_ms <= o.max_delay_ms
                && o.min_delay_ms >= -60_000.0
                && o.max_delay_ms <= 60_000.0
                && o.max_delay_ms - o.min_delay_ms <= 10_000.0
                && o.min_motion >= 0.0
                && o.blank_std_dev >= 0.0
                && (0.0..=1.0).contains(&o.min_confidence),
            "invalid comparison options"
        );
        if let Some(id) = &self.calibration_id {
            crate::evidence::validate_id(id)?;
        }
        for (i, c) in self.criteria.iter().enumerate() {
            crate::evidence::validate_id(&c.id)?;
            ensure!(
                !((c.id.starts_with("SC-") && c.id.len() == 6) || c.id.starts_with("REF-")),
                "picture subchecks cannot stand in for complete release or reference-host criteria"
            );
            ensure!(
                !self.criteria[..i].iter().any(|other| other.id == c.id),
                "duplicate comparison criterion"
            );
            ensure!(
                c.uncertainty.is_none_or(|v| v.is_finite() && v >= 0.0),
                "invalid comparison uncertainty"
            );
            let expected = match c.metric {
                PictureMetric::RelativeDelayMs => "ms",
                _ => "ratio",
            };
            ensure!(
                c.threshold.units.as_deref() == Some(expected),
                "comparison threshold unit mismatch"
            );
        }
        Ok(())
    }
}

fn picture_results(
    profile: &ComparisonProfile,
    comparison: &crate::compare::CompareReport,
    calibrated: bool,
) -> Value {
    let mut criteria = Vec::new();
    for c in &profile.criteria {
        let value = match c.metric {
            PictureMetric::SsimFloorFraction => comparison
                .fidelity
                .as_ref()
                .filter(|f| f.frames_scored > 0)
                .map(|f| f.frames_ssim_at_least_070 as f64 / f.frames_scored as f64),
            PictureMetric::SsimMedian => comparison.fidelity.as_ref().map(|f| f.ssim.p50),
            PictureMetric::RelativeDelayMs => comparison
                .alignment
                .delay_ms
                .filter(|_| comparison.alignment.reliable),
        };
        let error = match c.metric {
            PictureMetric::RelativeDelayMs => comparison.alignment.uncertainty_ms,
            _ => c.uncertainty,
        };
        let status = if calibrated && comparison.fidelity_population_complete {
            match value.zip(error) {
                Some((v, e)) => {
                    crate::verdict::compare_bounds(&c.threshold, Some(v - e), Some(v + e))
                }
                None => Status::Incomplete,
            }
        } else {
            Status::Incomplete
        };
        criteria.push(json!({"id":c.id,"threshold":c.threshold,"value":value,"uncertainty":error,
            "status":status,"evidence_ids":["frame-results"],"reason":match status {
                Status::Pass=>"complete calibrated comparison meets threshold",Status::Fail=>"complete calibrated comparison fails threshold",
                Status::Incomplete=>"required population, calibrated format, threshold or uncertainty unavailable"}}));
    }
    let statuses = criteria
        .iter()
        .map(|c| serde_json::from_value(c["status"].clone()).expect("serialized status"));
    json!({"overall_status":crate::verdict::overall(statuses),"criteria":criteria})
}

/// Local comparison retains its declaration and an Incomplete report even when a required
/// recording is missing. Invalid profiles/paths fail before report admission.
pub async fn compare_with_profile(
    stream: &Path,
    reference_path: &Path,
    out: &Path,
    profile_path: Option<&Path>,
    report_path: Option<&Path>,
) -> Result<Value> {
    let (_stop, cancel) = watch::channel(false);
    compare_in_root(
        &std::env::current_dir()?,
        stream,
        reference_path,
        out,
        profile_path,
        report_path,
        cancel,
    )
    .await
}

pub async fn compare_in_root(
    root: &Path,
    stream: &Path,
    reference_path: &Path,
    out: &Path,
    profile_path: Option<&Path>,
    report_path: Option<&Path>,
    mut cancel: watch::Receiver<bool>,
) -> Result<Value> {
    let profile: Option<ComparisonProfile> = if let Some(path) = profile_path {
        let file = File::open(path)?;
        ensure!(
            file.metadata()?.len() <= 65536,
            "comparison profile too large"
        );
        let profile: ComparisonProfile = serde_json::from_reader(file)?;
        profile.validate()?;
        Some(profile)
    } else {
        None
    };
    let options = profile
        .as_ref()
        .map_or_else(crate::compare::CompareOptions::default, |p| {
            p.options.clone()
        });
    let mut result = json!({"schema_version":"1.0.0","profile":profile,"overall_status":"Incomplete",
        "criteria":[],"comparison":null,"reason":"required recording evidence unavailable"});
    if let Some(profile) = &profile {
        result["criteria"] = json!(profile
            .criteria
            .iter()
            .map(
                |c| json!({"id":c.id,"threshold":c.threshold,"status":"Incomplete",
            "evidence_ids":[],"reason":"required recording evidence unavailable"})
            )
            .collect::<Vec<_>>());
    }
    let root = root.canonicalize()?;
    let output = if out.is_absolute() {
        out.to_path_buf()
    } else {
        root.join(out)
    };
    ensure!(
        output.starts_with(&root)
            && !output.exists()
            && output
                .components()
                .all(|component| !matches!(component, std::path::Component::ParentDir)),
        "comparison output must be a fresh local directory"
    );
    crate::capture_server::confined_root(output.parent().context("missing comparison parent")?)?;
    let mut journal = if let Some(path) = report_path {
        let path = local_report_path(&root, path)?;
        let mut file = OpenOptions::new().create_new(true).write(true).open(path)?;
        serde_json::to_writer(&mut file, &result)?;
        file.sync_all()?;
        Some(file)
    } else {
        None
    };
    let operation = async {
        let stream = reference::Recording::load(stream)?;
        let reference = reference::Recording::load(reference_path)?;
        let calibrated = if let Some(p) = &profile {
            ensure!(
                stream.index.width == p.width as usize
                    && stream.index.height == p.height as usize
                    && stream.metadata.bit_depth == p.bit_depth
                    && stream.metadata.color_space == p.color_space,
                "profile and native format disagree"
            );
            p.calibration_id.as_ref().is_some_and(|id| {
                stream.metadata.calibration_id.as_ref() == Some(id)
                    && reference.metadata.calibration_id.as_ref() == Some(id)
            })
        } else {
            false
        };
        let comparison = crate::compare::compare(&stream, &reference, &options, &output).await?;
        Ok::<_, anyhow::Error>((comparison, calibrated))
    };
    let comparison = tokio::select! {
        result=tokio::time::timeout(Duration::from_secs(300),operation)=>match result {Ok(result)=>result,Err(_)=>Err(anyhow::anyhow!("comparison deadline exceeded"))},
        _=cancelled(&mut cancel)=>Err(anyhow::anyhow!("comparison cancelled")),
    };
    match comparison {
        Ok((comparison, calibrated)) => {
            if let Some(profile) = &profile {
                let verdict = picture_results(profile, &comparison, calibrated);
                result["criteria"] = verdict["criteria"].clone();
                result["overall_status"] = verdict["overall_status"].clone();
            }
            result["comparison"] = serde_json::to_value(comparison)?;
            result["reason"] = json!(
                "numeric comparison is separate from unresolved release acceptance requirements"
            );
        }
        Err(error) => {
            if journal.is_none() && profile.is_none() {
                return Err(error);
            }
        }
    }
    if let Some(file) = journal.as_mut() {
        use std::io::{Seek, SeekFrom};
        let bytes = serde_json::to_vec(&result)?;
        file.seek(SeekFrom::Start(0))?;
        file.write_all(&bytes)?;
        file.set_len(bytes.len() as u64)?;
        file.sync_all()?;
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn observer_attachment_prelaunch_refuses_stale_races_and_reserves_private_marker() {
        for case in ["stale", "race", "quota", "reserved"] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let ready = scratch.0.join("ready.json");
            let attachment = scratch.0.join("attachment.json");
            if case == "stale" {
                std::fs::write(&attachment, b"premature").unwrap();
            }
            let budget = Budget::new(if case == "quota" { 1 } else { 65536 }, 10);
            let (_tx, mut cancel) = watch::channel(false);
            let mut launched = false;
            let operation = observer_before_launch_with_signal(
                ReadinessContext {
                    scenario: &s,
                    ready_path: Some(&ready),
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r,
                },
                &mut cancel,
                OBSERVER_WAIT,
                false,
                Some(&attachment),
                || {
                    launched = true;
                },
            );
            let publisher = async {
                if ["stale", "quota"].contains(&case) {
                    return;
                }
                tokio::task::yield_now().await;
                publish_readiness(
                    &ready,
                    &serde_json::to_vec(&readiness_publication(&scratch.0)).unwrap(),
                );
                if case == "race" {
                    std::fs::write(&attachment, b"raced before launch").unwrap();
                }
            };
            let (result, ()) = tokio::join!(operation, publisher);
            assert_eq!(launched, case == "reserved");
            assert_eq!(result.is_ok(), case == "reserved");
            assert!(!scratch.0.join("observer-launch.json").exists());
            if let Ok(((), _, Some(reservation))) = result {
                let original =
                    std::fs::read_to_string(scratch.0.join("observer-binding.json")).unwrap();
                assert!(!original.contains(&reservation.envelope.launch_nonce));
                let remaining = *budget.0.lock().unwrap();
                budget.charge(remaining.0, remaining.1).unwrap();
                // Publication uses the already charged bytes/file even after later quota exhaustion.
                assert!(publish_observer_file(
                    &scratch.0,
                    "observer-launch.json",
                    &reservation.bytes
                )
                .is_ok());
            }
            retain_signal_case(
                &scratch.0,
                &format!("attachment-prelaunch-{case}"),
                json!({"launch_attempted":launched, "nonce_private_before_launch":true,"marker_reserved":case == "reserved"}),
            );
        }
    }
    #[test]
    fn observer_attachment_replacement_during_snapshot_is_terminal() {
        let scratch = Scratch::new();
        let path = scratch.0.join("attachment.json");
        std::fs::write(&path, b"original").unwrap();
        let result = read_observer_snapshot_with(&path, || {
            std::fs::write(scratch.0.join("replacement"), b"replaced").unwrap();
            std::fs::rename(scratch.0.join("replacement"), &path).unwrap();
        });
        assert!(result.is_err());
        retain_signal_case(
            &scratch.0,
            "attachment-replacement",
            json!({"replacement_rejected":true}),
        );
    }
    #[tokio::test]
    async fn observer_attachment_orders_launch_retention_rtsp_and_cancel() {
        for case in [
            "success",
            "delayed",
            "late",
            "missing",
            "cancelled",
            "foreign",
            "quota",
            "duplicate",
            "unknown",
            "bom",
            "hardlink",
            "symlink",
            "authenticated",
            "guessed-nonce",
            "marker-competitor",
        ] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            let binding = ObserverBinding {
                schema_version: "1.0.0".into(),
                run_id: r.run_id.clone(),
                session_id: r.session_id.clone(),
                scenario_id: s.id().into(),
                scenario_sha256: "a".repeat(64),
                host_version: "host".into(),
                test_client_version: "client".into(),
                nonce: uuid::Uuid::new_v4().to_string(),
            };
            let path = scratch.0.join("attachment.json");
            let launch =
                LaunchReservation::new(&path, &scratch.0, &Budget::new(65536, 10), &binding)
                    .unwrap();
            let mut envelope = json!({"schema_version":"1.0.0", "state":"attached", "binding":binding, "launch_nonce":uuid::Uuid::new_v4().to_string(), "observer_id":uuid::Uuid::new_v4().to_string(), "association":"exclusive_launch_candidate", "authenticated":false, "runtime_attachment_sha256":"b".repeat(64)});
            if case == "foreign" {
                envelope["binding"]["nonce"] = json!(uuid::Uuid::new_v4().to_string());
            }
            if case == "unknown" {
                envelope["extra"] = json!(true);
            }
            if case == "authenticated" {
                envelope["authenticated"] = json!(true);
            }
            if case == "guessed-nonce" {
                std::fs::write(&path, serde_json::to_vec(&envelope).unwrap()).unwrap();
            }
            if case == "marker-competitor" {
                std::fs::write(scratch.0.join("observer-launch.json"), b"competitor").unwrap();
            }
            let signal_budget = Budget::new(65536, 10);
            let signal = CancelReservation::new(&scratch.0, &signal_budget, &binding).unwrap();
            let budget = Budget::new(if case == "quota" { 1 } else { 65536 }, 10);
            let (_tx, mut cancel) = watch::channel(case == "cancelled");
            let mut events = vec!["launch"];
            let started = tokio::time::Instant::now();
            let operation = observer_before_rtsp(
                AttachmentContext {
                    path: Some(&path),
                    launch: Some(&launch),
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r,
                },
                &mut cancel,
                ObserverWait {
                    timeout: Duration::from_millis(if ["missing", "late"].contains(&case) {
                        30
                    } else {
                        1000
                    }),
                    poll_interval: Duration::from_millis(2),
                },
                || {
                    assert_eq!(
                        std::fs::read(scratch.0.join("observer-attachment.json")).unwrap(),
                        std::fs::read(&path).unwrap()
                    );
                    events.push("rtsp");
                },
            );
            let publisher = async {
                if ["missing", "cancelled", "guessed-nonce", "marker-competitor"].contains(&case) {
                    return;
                }
                tokio::task::yield_now().await;
                let marker: ObserverLaunch = serde_json::from_slice(
                    &std::fs::read(scratch.0.join("observer-launch.json")).unwrap(),
                )
                .unwrap();
                assert_eq!(marker.state, "launched");
                assert_eq!(marker.binding, binding);
                envelope["launch_nonce"] = json!(marker.launch_nonce);
                let mut bytes = serde_json::to_vec(&envelope).unwrap();
                if case == "duplicate" {
                    bytes = String::from_utf8(bytes)
                        .unwrap()
                        .replacen("{", "{\"state\":\"attached\",", 1)
                        .into_bytes();
                }
                if case == "bom" {
                    bytes.splice(0..0, [0xef, 0xbb, 0xbf]);
                }
                if ["delayed", "late"].contains(&case) {
                    tokio::time::sleep(Duration::from_millis(if case == "late" { 60 } else { 5 }))
                        .await;
                }
                publish_readiness(&path, &bytes);
                if case == "hardlink" {
                    std::fs::hard_link(&path, scratch.0.join("alias.json")).unwrap();
                }
                if case == "symlink" {
                    std::fs::rename(&path, scratch.0.join("original.json")).unwrap();
                    std::os::unix::fs::symlink(scratch.0.join("original.json"), &path).unwrap();
                }
            };
            let (result, ()) = tokio::join!(operation, publisher);
            let accepted = ["success", "delayed"].contains(&case);
            assert_eq!(result.is_ok(), accepted, "{case}: {result:?}");
            assert!(started.elapsed() < Duration::from_secs(2));
            let cancellation = async {
                events.push("cancel");
                Ok(())
            };
            let (success, _) = cancel_with_observer_signal(
                cancellation,
                Duration::from_secs(1),
                OBSERVER_WAIT,
                Some(signal),
                &scratch.0,
                &mut r,
            )
            .await;
            assert!(success);
            assert_eq!(
                events,
                if accepted {
                    vec!["launch", "rtsp", "cancel"]
                } else {
                    vec!["launch", "cancel"]
                }
            );
            r.finalize(Some("synthetic coordination only")).unwrap();
            assert_eq!(r.overall_status, Status::Incomplete);
            std::fs::write(
                scratch.0.join("report.json"),
                serde_json::to_vec_pretty(&r).unwrap(),
            )
            .unwrap();
            retain_signal_case(
                &scratch.0,
                &format!("attachment-{case}"),
                json!({"case":case,"events":events,"rtsp_attempted":accepted, "cancellation_attempted":true,"overall_status":r.overall_status}),
            );
        }
    }

    #[tokio::test]
    async fn observer_attachment_absent_option_preserves_legacy() {
        let scratch = Scratch::new();
        let s = scenario();
        let mut r = report(&s);
        let budget = Budget::new(0, 0);
        let (_tx, mut cancel) = watch::channel(false);
        assert_eq!(
            observer_before_rtsp(
                AttachmentContext {
                    path: None,
                    launch: None,
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r
                },
                &mut cancel,
                OBSERVER_WAIT,
                || 42
            )
            .await
            .unwrap(),
            42
        );
    }

    #[tokio::test]
    async fn observer_attachment_wait_observes_cancellation() {
        let scratch = Scratch::new();
        let s = scenario();
        let mut r = report(&s);
        let binding = ObserverBinding {
            schema_version: "1.0.0".into(),
            run_id: r.run_id.clone(),
            session_id: r.session_id.clone(),
            scenario_id: s.id().into(),
            scenario_sha256: "a".repeat(64),
            host_version: "host".into(),
            test_client_version: "client".into(),
            nonce: uuid::Uuid::new_v4().to_string(),
        };
        let budget = Budget::new(65536, 10);
        let (tx, mut cancel) = watch::channel(false);
        let path = scratch.0.join("missing.json");
        let launch =
            LaunchReservation::new(&path, &scratch.0, &Budget::new(65536, 10), &binding).unwrap();
        let operation = observer_before_rtsp(
            AttachmentContext {
                path: Some(&path),
                launch: Some(&launch),
                out: &scratch.0,
                budget: &budget,
                report: &mut r,
            },
            &mut cancel,
            OBSERVER_WAIT,
            || panic!("RTSP after cancellation"),
        );
        let cancelling = async {
            tokio::time::sleep(Duration::from_millis(5)).await;
            tx.send(true).unwrap();
        };
        let started = tokio::time::Instant::now();
        let (result, ()) = tokio::join!(operation, cancelling);
        assert!(result.is_err());
        assert!(started.elapsed() < Duration::from_secs(1));
        retain_signal_case(
            &scratch.0,
            "attachment-cancel-while-waiting",
            json!({"cancel_interrupts_wait":true,"rtsp_attempted":false}),
        );
    }
    fn retain_readiness_case(
        root: &Path,
        name: &str,
        r: &mut Report,
        launched: bool,
        result: &Result<impl Serialize>,
    ) {
        r.finalize(
            result
                .as_ref()
                .err()
                .map(|_| "synthetic readiness unavailable"),
        )
        .unwrap();
        r.validate().unwrap();
        if let Some(base) = std::env::var_os("MOONLIGHT_READY_TEST_EVIDENCE") {
            let destination = PathBuf::from(base).join(name);
            std::fs::create_dir(&destination).unwrap();
            for entry in std::fs::read_dir(root).unwrap() {
                let entry = entry.unwrap();
                if entry.file_type().unwrap().is_file() {
                    std::fs::copy(entry.path(), destination.join(entry.file_name())).unwrap();
                }
            }
            std::fs::write(
                destination.join("report.json"),
                serde_json::to_vec_pretty(r).unwrap(),
            )
            .unwrap();
            std::fs::write(destination.join("observable.json"), serde_json::to_vec_pretty(&json!({
                "provenance":"synthetic_fixture", "launch_factory_called":launched,
                "result":result.as_ref().ok(), "error":result.as_ref().err().map(ToString::to_string),
                "overall_status":r.overall_status,
            })).unwrap()).unwrap();
        }
    }

    fn readiness_publication(root: &Path) -> Value {
        let binding: Value =
            serde_json::from_slice(&std::fs::read(root.join("observer-binding.json")).unwrap())
                .unwrap();
        json!({"schema_version":"1.0.0","state":"ready","binding":binding})
    }

    fn retain_signal_case(root: &Path, name: &str, observable: Value) {
        if let Some(evidence) = std::env::var_os("MOONLIGHT_SIGNAL_TEST_EVIDENCE") {
            let destination = PathBuf::from(evidence).join(name);
            std::fs::create_dir_all(&destination).unwrap();
            for entry in std::fs::read_dir(root).unwrap() {
                let entry = entry.unwrap();
                if entry.file_type().unwrap().is_file() {
                    std::fs::copy(entry.path(), destination.join(entry.file_name())).unwrap();
                }
            }
            std::fs::write(
                destination.join("observable.json"),
                serde_json::to_vec_pretty(&observable).unwrap(),
            )
            .unwrap();
        }
    }

    #[test]
    fn observer_signal_publication_is_irrevocable_and_single_link() {
        use std::os::unix::fs::MetadataExt;
        let scratch = Scratch::new();
        let path = scratch.0.join("observer-binding.json");
        let mut visible_identity = None;
        let synced = publish_observer_file_with(
            &scratch.0,
            "observer-binding.json",
            b"complete",
            || Ok(()),
            || {
                let info = std::fs::metadata(&path).unwrap();
                assert_eq!(info.nlink(), 1);
                assert_eq!(std::fs::read(&path).unwrap(), b"complete");
                visible_identity = Some((info.ino(), info.ctime(), info.ctime_nsec()));
                Err(std::io::Error::other("injected directory sync failure"))
            },
        )
        .unwrap();
        assert!(!synced);
        let after = std::fs::metadata(&path).unwrap();
        assert_eq!(
            visible_identity,
            Some((after.ino(), after.ctime(), after.ctime_nsec()))
        );
        assert_eq!(std::fs::read_dir(&scratch.0).unwrap().count(), 1);
        retain_signal_case(
            &scratch.0,
            "irrevocable",
            json!({"directory_synced":synced,"nlink":after.nlink(),"stable_identity":true}),
        );
    }

    #[test]
    fn observer_signal_python_reads_binding_at_first_visibility() {
        let scratch = Scratch::new();
        let binding = ObserverBinding {
            schema_version: "1.0.0".into(),
            run_id: "run".into(),
            session_id: "session".into(),
            scenario_id: "scenario".into(),
            scenario_sha256: "a".repeat(64),
            host_version: "host".into(),
            test_client_version: "client".into(),
            nonce: uuid::Uuid::new_v4().to_string(),
        };
        let bytes = serde_json::to_vec(&binding).unwrap();
        let synced = publish_observer_file_with(&scratch.0, "observer-binding.json", &bytes, || Ok(()), || {
            let output = std::process::Command::new("python3")
                .current_dir(Path::new(env!("CARGO_MANIFEST_DIR")).join("../reference-observer"))
                .env("PYTHONDONTWRITEBYTECODE", "1")
                .args(["-c", "import sys,json; from observer import read_runner_binding; p=sys.argv[1]; b=json.load(open(p)); assert read_runner_binding(p,b)==b; print('binding accepted at visibility')"])
                .arg(scratch.0.join("observer-binding.json")).output().unwrap();
            std::fs::write(scratch.0.join("python-visibility.log"), &output.stdout).unwrap();
            assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
            Err(std::io::Error::other("directory sync fails after consumer read"))
        }).unwrap();
        assert!(!synced);
        assert_eq!(
            std::fs::read(scratch.0.join("observer-binding.json")).unwrap(),
            bytes
        );
        retain_signal_case(
            &scratch.0,
            "python-first-visibility",
            json!({"accepted":true,"directory_synced":false}),
        );
    }

    #[test]
    fn observer_signal_reservation_rejects_stale_and_quota_before_launch() {
        let scratch = Scratch::new();
        let binding = ObserverBinding {
            schema_version: "1.0.0".into(),
            run_id: "run".into(),
            session_id: "session".into(),
            scenario_id: "scenario".into(),
            scenario_sha256: "a".repeat(64),
            host_version: "host".into(),
            test_client_version: "client".into(),
            nonce: uuid::Uuid::new_v4().to_string(),
        };
        assert!(CancelReservation::new(&scratch.0, &Budget::new(0, 1), &binding).is_err());
        assert!(CancelReservation::new(&scratch.0, &Budget::new(65536, 0), &binding).is_err());
        std::fs::write(scratch.0.join("observer-cancel.json"), b"stale").unwrap();
        assert!(CancelReservation::new(&scratch.0, &Budget::new(65536, 10), &binding).is_err());
        retain_signal_case(
            &scratch.0,
            "reservation-refusal",
            json!({"stale_rejected":true,"bytes_rejected":true,"files_rejected":true}),
        );
    }

    #[tokio::test]
    async fn observer_signal_follows_cancellation_and_timeout_has_zero_wait() {
        for case in ["success", "error", "timeout"] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            let binding = ObserverBinding {
                schema_version: "1.0.0".into(),
                run_id: r.run_id.clone(),
                session_id: r.session_id.clone(),
                scenario_id: s.id().into(),
                scenario_sha256: "a".repeat(64),
                host_version: "host".into(),
                test_client_version: "client".into(),
                nonce: uuid::Uuid::new_v4().to_string(),
            };
            let signal =
                CancelReservation::new(&scratch.0, &Budget::new(65536, 1), &binding).unwrap();
            let cancellation = async {
                assert!(!scratch.0.join("observer-cancel.json").exists());
                if case == "timeout" {
                    std::future::pending::<()>().await;
                }
                tokio::time::sleep(Duration::from_millis(5)).await;
                assert!(!scratch.0.join("observer-cancel.json").exists());
                if case == "error" {
                    bail!("synthetic cancel failure");
                }
                Ok(())
            };
            let (succeeded, deadline) = cancel_with_observer_signal(
                cancellation,
                Duration::from_millis(20),
                // Only the returned deadline uses this; it is long enough that
                // a slow runner still observes it in the future on success.
                ObserverWait {
                    timeout: Duration::from_secs(5),
                    poll_interval: Duration::from_millis(1),
                },
                Some(signal),
                &scratch.0,
                &mut r,
            )
            .await;
            assert_eq!(succeeded, case == "success");
            let envelope: ObserverCancel = serde_json::from_slice(
                &std::fs::read(scratch.0.join("observer-cancel.json")).unwrap(),
            )
            .unwrap();
            assert_eq!(envelope.request_succeeded, succeeded);
            if succeeded {
                assert!(deadline > tokio::time::Instant::now());
            } else {
                assert!(deadline <= tokio::time::Instant::now());
            }
            retain_signal_case(
                &scratch.0,
                &format!("cancel-order-{case}"),
                json!({"signal_absent_during_cancel":true,"request_succeeded":succeeded,"zero_wait":!succeeded}),
            );
        }
    }

    #[tokio::test]
    async fn observer_signal_prelaunch_failure_produces_no_cancel_signal() {
        for case in ["cancelled", "quota", "stale"] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let ready = scratch.0.join("ready.json");
            if case == "stale" {
                std::fs::write(&ready, b"stale").unwrap();
            }
            let budget = Budget::new(if case == "quota" { 0 } else { 65536 }, 10);
            let (_stop, mut cancel) = watch::channel(case == "cancelled");
            let result = observer_before_launch_with_signal(
                ReadinessContext {
                    scenario: &s,
                    ready_path: Some(&ready),
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r,
                },
                &mut cancel,
                OBSERVER_WAIT,
                true,
                None,
                || panic!("prelaunch refusal must prevent launch"),
            )
            .await;
            assert!(result.is_err());
            assert!(!scratch.0.join("observer-cancel.json").exists());
            retain_signal_case(
                &scratch.0,
                &format!("prelaunch-{case}"),
                json!({"launch_prevented":true,"cancel_signal_absent":true}),
            );
        }
    }

    #[tokio::test]
    async fn observer_signal_import_uses_elapsed_absolute_deadline() {
        let mut fixture = ObserverFixture::new();
        let deadline = tokio::time::Instant::now();
        let started = tokio::time::Instant::now();
        reference_observer_criteria_until(
            &fixture.scenario,
            &fixture.options,
            &fixture.out,
            &Budget::new(65536, 10),
            &mut fixture.report,
            deadline,
            OBSERVER_WAIT,
        )
        .await;
        assert!(started.elapsed() < Duration::from_millis(100));
        assert!(fixture
            .report
            .criteria
            .iter()
            .all(|criterion| criterion.status == Status::Incomplete));
        retain_signal_case(
            &fixture.out,
            "expired-deadline",
            json!({"immediate_import":true,"all_incomplete":true}),
        );
    }

    #[test]
    fn observer_signal_competitor_and_previsibility_fault_are_safe() {
        for case in ["competitor", "fault", "symlink"] {
            let scratch = Scratch::new();
            let path = scratch.0.join("observer-cancel.json");
            let result = publish_observer_file_with(
                &scratch.0,
                "observer-cancel.json",
                b"ours",
                || {
                    match case {
                        "competitor" => std::fs::write(&path, b"winner")?,
                        "symlink" => std::os::unix::fs::symlink("missing", &path)?,
                        _ => bail!("injected previsibility fault"),
                    }
                    Ok(())
                },
                || panic!("cannot sync after rejected rename"),
            );
            assert!(result.is_err());
            if case == "competitor" {
                assert_eq!(std::fs::read(&path).unwrap(), b"winner");
            }
            if case == "fault" {
                assert!(!path.exists());
            }
            if case == "symlink" {
                assert!(std::fs::symlink_metadata(&path)
                    .unwrap()
                    .file_type()
                    .is_symlink());
            }
            assert!(!std::fs::read_dir(&scratch.0).unwrap().any(|entry| entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .ends_with(".pending")));
            retain_signal_case(
                &scratch.0,
                case,
                json!({"rejected":true,"competitor_preserved":case != "fault"}),
            );
        }
    }

    #[test]
    fn observer_signal_alias_and_late_competitor_fail_without_replacement() {
        let scratch = Scratch::new();
        let alias = scratch.0.join("alias");
        std::os::unix::fs::symlink(&scratch.0, &alias).unwrap();
        assert!(publish_observer_file(&alias, "signal.json", b"data").is_err());
        let s = scenario();
        let mut r = report(&s);
        let binding = ObserverBinding {
            schema_version: "1.0.0".into(),
            run_id: r.run_id.clone(),
            session_id: r.session_id.clone(),
            scenario_id: s.id().into(),
            scenario_sha256: "a".repeat(64),
            host_version: "host".into(),
            test_client_version: "client".into(),
            nonce: uuid::Uuid::new_v4().to_string(),
        };
        let reservation =
            CancelReservation::new(&scratch.0, &Budget::new(65536, 1), &binding).unwrap();
        std::fs::write(scratch.0.join("observer-cancel.json"), b"competitor").unwrap();
        reservation.publish(&scratch.0, true, &mut r);
        assert_eq!(
            std::fs::read(scratch.0.join("observer-cancel.json")).unwrap(),
            b"competitor"
        );
        assert!(r
            .limitations
            .iter()
            .any(|reason| reason.contains("could not be published")));
        retain_signal_case(
            &scratch.0,
            "alias-late-competitor",
            json!({"alias_rejected":true,"competitor_preserved":true,"limitation_retained":true}),
        );
    }

    #[test]
    fn observer_signal_write_and_sync_failures_leave_no_publication() {
        for case in ["write", "file-sync"] {
            let scratch = Scratch::new();
            let result = publish_observer_file_ops(
                &scratch.0,
                "observer-cancel.json",
                b"payload",
                |file, bytes| {
                    if case == "file-sync" {
                        file.write_all(bytes)?;
                    }
                    bail!("injected {case} failure")
                },
                || panic!("failed write/sync cannot rename"),
                || panic!("failed write/sync cannot directory-sync"),
            );
            assert!(result.is_err());
            assert_eq!(std::fs::read_dir(&scratch.0).unwrap().count(), 0);
            retain_signal_case(
                &scratch.0,
                case,
                json!({"publication_absent":true,"temporary_cleaned":true}),
            );
        }
    }

    #[tokio::test]
    async fn observer_signal_retains_binding_and_reserved_budget() {
        for succeeded in [true, false] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let ready = scratch.0.join("ready.json");
            let budget = Budget::new(65536, 10);
            let (_stop, mut cancel) = watch::channel(false);
            let operation = observer_before_launch_with_signal(
                ReadinessContext {
                    scenario: &s,
                    ready_path: Some(&ready),
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r,
                },
                &mut cancel,
                OBSERVER_WAIT,
                true,
                None,
                || 42,
            );
            let publisher = async {
                tokio::task::yield_now().await;
                publish_readiness(
                    &ready,
                    &serde_json::to_vec(&readiness_publication(&scratch.0)).unwrap(),
                );
            };
            let (result, ()) = tokio::join!(operation, publisher);
            let (value, signal, _) = result.unwrap();
            assert_eq!(value, 42);
            assert!(!scratch.0.join("observer-cancel.json").exists());
            let original: ObserverBinding = serde_json::from_slice(
                &std::fs::read(scratch.0.join("observer-binding.json")).unwrap(),
            )
            .unwrap();
            std::fs::write(
                scratch.0.join("observer-binding.json"),
                b"replaced after READY",
            )
            .unwrap();
            let remaining = *budget.0.lock().unwrap();
            budget.charge(remaining.0, remaining.1).unwrap();
            signal.unwrap().publish(&scratch.0, succeeded, &mut r);
            let envelope: ObserverCancel = serde_json::from_slice(
                &std::fs::read(scratch.0.join("observer-cancel.json")).unwrap(),
            )
            .unwrap();
            assert_eq!(envelope.binding, original);
            assert_eq!(envelope.request_succeeded, succeeded);
            assert!(r
                .criteria
                .iter()
                .all(|criterion| criterion.status == Status::Incomplete));
            let python = std::process::Command::new("python3")
                .current_dir(Path::new(env!("CARGO_MANIFEST_DIR")).join("../reference-observer"))
                .env("PYTHONDONTWRITEBYTECODE", "1")
                .args(["-c", r#"
import json, os, sys, threading, time
from pathlib import Path
from unittest.mock import patch
import observer
root = Path(sys.argv[1])
signal = json.loads((root / 'observer-cancel.json').read_bytes())
binding = signal['binding']
reader, writer = os.pipe()
os.set_blocking(reader, False)
ready = root / 'finish-ready.json'
errors = []
def finish():
    try:
        deadline = time.monotonic() + 2
        while not ready.exists():
            if time.monotonic() >= deadline: raise RuntimeError('no READY')
            time.sleep(.001)
        if signal['request_succeeded']:
            os.write(writer, json.dumps({'schema_version':'1.0.0','command':'finish','binding':binding}).encode() + b'\n')
    except BaseException as error: errors.append(str(error))
    finally: os.close(writer)
worker = threading.Thread(target=finish)
worker.start()
started = time.monotonic()
with patch.object(observer.sys, 'platform', 'linux'), patch.object(observer, 'inventory', return_value={}):
    records = observer.collect(binding['session_id'], 300, (ready,binding), reader)
worker.join(timeout=2)
assert not worker.is_alive() and not errors, errors
assert time.monotonic() - started < 2
reason = records[-1]['termination_reason']
assert reason == ('finish_command' if signal['request_succeeded'] else 'control_error'), records
(root / 'finish-records.json').write_text(json.dumps(records))
print(json.dumps({'finish_reason':reason,'bounded':True}))
"#])
                .arg(&scratch.0).output().unwrap();
            std::fs::write(scratch.0.join("finish-python.log"), &python.stdout).unwrap();
            assert!(
                python.status.success(),
                "{}",
                String::from_utf8_lossy(&python.stderr)
            );
            retain_signal_case(
                &scratch.0,
                if succeeded {
                    "cancel-success"
                } else {
                    "cancel-failure"
                },
                json!({"request_succeeded":succeeded,"reserved_budget":true,"binding_snapshot":true,"all_incomplete":true}),
            );
        }
    }

    fn publish_readiness(path: &Path, bytes: &[u8]) {
        let temporary = path.with_extension("pending");
        std::fs::write(&temporary, bytes).unwrap();
        std::fs::rename(temporary, path).unwrap();
    }

    #[tokio::test]
    async fn readiness_invalid_stale_and_foreign_records_prevent_launch() {
        for case in [
            "run_id",
            "session_id",
            "scenario_id",
            "scenario_sha256",
            "nonce",
            "host_version",
            "test_client_version",
            "schema_version",
            "state",
            "unknown_field",
            "malformed",
            "empty",
            "oversized",
            "symlink",
            "directory",
            "stale",
        ] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let path = scratch.0.join("ready.json");
            let (_stop, mut cancel) = watch::channel(false);
            let budget = Budget::new(65536, 10);
            let mut launched = false;
            if case == "stale" {
                std::fs::write(&path, b"stale previous run").unwrap();
            }
            let operation = observer_before_launch(
                ReadinessContext {
                    scenario: &s,
                    ready_path: Some(&path),
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r,
                },
                &mut cancel,
                OBSERVER_WAIT,
                || {
                    launched = true;
                },
            );
            let publisher = async {
                tokio::task::yield_now().await;
                if case == "stale" {
                    return;
                }
                let mut ready = readiness_publication(&scratch.0);
                match case {
                    "schema_version" | "state" => ready[case] = json!("invalid"),
                    "unknown_field" => ready["authenticated"] = json!(true),
                    "malformed" | "empty" | "oversized" | "symlink" | "directory" => (),
                    _ => ready["binding"][case] = json!("foreign"),
                }
                match case {
                    "malformed" => publish_readiness(&path, b"{"),
                    "empty" => publish_readiness(&path, b""),
                    "oversized" => publish_readiness(&path, &vec![b' '; MAX_READY_BYTES + 1]),
                    "symlink" => {
                        let source = scratch.0.join("foreign.json");
                        std::fs::write(&source, serde_json::to_vec(&ready).unwrap()).unwrap();
                        std::os::unix::fs::symlink(source, &path).unwrap();
                    }
                    "directory" => std::fs::create_dir(&path).unwrap(),
                    _ => publish_readiness(&path, &serde_json::to_vec(&ready).unwrap()),
                }
            };
            let (result, ()) = tokio::time::timeout(Duration::from_secs(2), async {
                tokio::join!(operation, publisher)
            })
            .await
            .expect("invalid readiness must be terminal, not retried");
            assert!(result.is_err(), "{case}");
            assert!(!launched, "{case}");
            assert!(!scratch.0.join("observer-ready.json").exists());
            retain_readiness_case(
                &scratch.0,
                &format!("rejected-{case}"),
                &mut r,
                launched,
                &result,
            );
        }
    }

    #[tokio::test]
    async fn readiness_timeout_cancellation_and_quota_prevent_launch_and_finalize() {
        for case in [
            "timeout",
            "cancel",
            "cancel_preexisting",
            "byte_quota",
            "file_quota",
            "ready_file_quota",
            "ready_byte_quota",
            "binding_exists",
        ] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let path = scratch.0.join("ready.json");
            let (stop, mut cancel) = watch::channel(case == "cancel_preexisting");
            let budget = match case {
                "byte_quota" => Budget::new(1, 10),
                "file_quota" => Budget::new(65536, 0),
                "ready_file_quota" => Budget::new(65536, 1),
                "ready_byte_quota" => Budget::new(600, 10),
                _ => Budget::new(65536, 10),
            };
            let mut launched = false;
            if case == "binding_exists" {
                std::fs::write(scratch.0.join("observer-binding.json"), b"do not replace").unwrap();
            }
            let operation = observer_before_launch(
                ReadinessContext {
                    scenario: &s,
                    ready_path: Some(&path),
                    out: &scratch.0,
                    budget: &budget,
                    report: &mut r,
                },
                &mut cancel,
                // Only the timeout case should expire; the others must not
                // race a slow runner's concurrent publication.
                ObserverWait {
                    timeout: if case == "timeout" {
                        Duration::from_millis(40)
                    } else {
                        Duration::from_secs(5)
                    },
                    poll_interval: Duration::from_millis(5),
                },
                || {
                    launched = true;
                },
            );
            let publisher = async {
                tokio::task::yield_now().await;
                if case == "cancel" {
                    stop.send(true).unwrap();
                }
                if matches!(case, "ready_file_quota" | "ready_byte_quota") {
                    publish_readiness(
                        &path,
                        &serde_json::to_vec(&readiness_publication(&scratch.0)).unwrap(),
                    );
                }
            };
            let (result, ()) = tokio::join!(operation, publisher);
            assert!(result.is_err(), "{case}");
            assert!(!launched, "{case}");
            if case == "binding_exists" {
                assert_eq!(
                    std::fs::read(scratch.0.join("observer-binding.json")).unwrap(),
                    b"do not replace"
                );
            }
            if case == "ready_byte_quota" {
                assert!(scratch.0.join("observer-binding.json").exists());
                assert!(scratch.0.join("ready.json").exists());
                assert_eq!(
                    std::fs::metadata(scratch.0.join("observer-ready.json"))
                        .unwrap()
                        .len(),
                    0
                );
            }
            retain_readiness_case(&scratch.0, case, &mut r, launched, &result);
            assert_eq!(r.overall_status, Status::Incomplete);
        }
    }

    #[tokio::test]
    async fn readiness_verified_snapshot_is_retained_before_launch_and_not_reopened() {
        let scratch = Scratch::new();
        let s = scenario();
        let mut r = report(&s);
        r.environment.host_version = Some("synthetic-host".into());
        let path = scratch.0.join("ready.json");
        let (_stop, mut cancel) = watch::channel(false);
        let budget = Budget::new(65536, 10);
        let mut launched = false;
        let operation = observer_before_launch(
            ReadinessContext {
                scenario: &s,
                ready_path: Some(&path),
                out: &scratch.0,
                budget: &budget,
                report: &mut r,
            },
            &mut cancel,
            OBSERVER_WAIT,
            || {
                assert!(scratch.0.join("observer-ready.json").exists());
                publish_readiness(&path, b"replacement after validation");
                launched = true;
            },
        );
        let publisher = async {
            tokio::task::yield_now().await;
            publish_readiness(
                &path,
                &serde_json::to_vec(&readiness_publication(&scratch.0)).unwrap(),
            );
        };
        let (result, ()) = tokio::join!(operation, publisher);
        assert!(result.is_ok());
        assert!(launched);
        let retained: Value =
            serde_json::from_slice(&std::fs::read(scratch.0.join("observer-ready.json")).unwrap())
                .unwrap();
        assert_eq!(retained, readiness_publication(&scratch.0));
        assert_eq!(
            std::fs::read(path).unwrap(),
            b"replacement after validation"
        );
        retain_readiness_case(
            &scratch.0,
            "snapshot-before-launch",
            &mut r,
            launched,
            &result,
        );
        assert!(r.criteria.iter().all(|c| c.status == Status::Incomplete));
    }

    #[tokio::test]
    async fn readiness_python_collector_publishes_accepted_envelope_without_promoting_criteria() {
        let mut scratch = Scratch::new();
        scratch.0 = scratch.0.canonicalize().unwrap();
        let s = scenario();
        let mut r = report(&s);
        r.environment.host_version = Some("synthetic-host".into());
        let path = scratch.0.join("ready.json");
        let (_stop, mut cancel) = watch::channel(false);
        let budget = Budget::new(65536, 10);
        let mut launched = false;
        let operation = observer_before_launch(
            ReadinessContext {
                scenario: &s,
                ready_path: Some(&path),
                out: &scratch.0,
                budget: &budget,
                report: &mut r,
            },
            &mut cancel,
            OBSERVER_WAIT,
            || {
                let retained = std::fs::read(scratch.0.join("observer-ready.json")).unwrap();
                assert_eq!(retained, std::fs::read(&path).unwrap());
                launched = true;
            },
        );
        let publisher = async {
            tokio::task::yield_now().await;
            let envelope = readiness_publication(&scratch.0);
            let binding = &envelope["binding"];
            std::fs::write(scratch.0.join("collector-binding.json"), serde_json::to_vec(&json!({
                "session_id": binding["session_id"], "scenario_id": binding["scenario_id"],
                "host_version": binding["host_version"], "test_client_version": binding["test_client_version"],
                "test_client_revision": "synthetic", "official_client_version": "synthetic",
                "runtime_identity": "synthetic acquisition harness; no live devices",
            })).unwrap()).unwrap();
            let output = std::process::Command::new("python3")
                .current_dir(Path::new(env!("CARGO_MANIFEST_DIR")).join("../reference-observer"))
                .env("PYTHONDONTWRITEBYTECODE", "1")
                .args(["-c", "import sys; from test_observer import readiness_contract; readiness_contract(sys.argv[1])"])
                .arg(&scratch.0)
                .output()
                .unwrap();
            std::fs::write(scratch.0.join("python-stdout.log"), &output.stdout).unwrap();
            std::fs::write(scratch.0.join("python-stderr.log"), &output.stderr).unwrap();
            assert!(
                output.status.success(),
                "{}",
                String::from_utf8_lossy(&output.stderr)
            );
            let stdout: Value = serde_json::from_slice(&output.stdout).unwrap();
            assert_eq!(stdout["state"], "READY");
            assert_eq!(stdout["initial_devices"], json!({}));
            let published: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
            assert_eq!(published, envelope);
        };
        let (result, ()) = tokio::join!(operation, publisher);
        assert!(result.is_ok());
        assert!(launched);
        retain_readiness_case(&scratch.0, "python-collector", &mut r, launched, &result);
        assert_eq!(r.overall_status, Status::Incomplete);
        assert!(!r.criteria.is_empty());
        assert!(r
            .criteria
            .iter()
            .all(|criterion| criterion.status == Status::Incomplete));
    }

    #[tokio::test]
    async fn readiness_unconfigured_preserves_launch_without_artifacts() {
        let scratch = Scratch::new();
        let s = scenario();
        let mut r = report(&s);
        let (_stop, mut cancel) = watch::channel(false);
        let budget = Budget::new(0, 0);
        let mut launched = false;
        let result = observer_before_launch(
            ReadinessContext {
                scenario: &s,
                ready_path: None,
                out: &scratch.0,
                budget: &budget,
                report: &mut r,
            },
            &mut cancel,
            OBSERVER_WAIT,
            || {
                launched = true;
            },
        )
        .await;
        assert!(result.is_ok());
        assert!(launched);
        assert_eq!(std::fs::read_dir(&scratch.0).unwrap().count(), 0);
        retain_readiness_case(&scratch.0, "unconfigured", &mut r, launched, &result);
        let mut options = RunOptions::new(scratch.0.clone(), "report.json".into());
        options.observer_ready = Some(scratch.0.join("ready.json"));
        assert_eq!(
            options.with_report("other.json".into()).observer_ready,
            options.observer_ready
        );
    }
    #[tokio::test]
    async fn readiness_delayed_ready_precedes_launch_and_never_promotes_acceptance() {
        let scratch = Scratch::new();
        let s = scenario();
        let mut r = report(&s);
        r.environment.host_version = Some("synthetic-host".into());
        let path = scratch.0.join("ready.json");
        let (_stop, mut cancel) = watch::channel(false);
        let mut launched = false;
        let budget = Budget::new(65536, 10);
        let operation = observer_before_launch(
            ReadinessContext {
                scenario: &s,
                ready_path: Some(&path),
                out: &scratch.0,
                budget: &budget,
                report: &mut r,
            },
            &mut cancel,
            ObserverWait {
                timeout: Duration::from_secs(1),
                poll_interval: Duration::from_millis(5),
            },
            || {
                launched = true;
                42
            },
        );
        let publisher = async {
            tokio::time::sleep(Duration::from_millis(20)).await;
            let binding: Value = serde_json::from_slice(
                &std::fs::read(scratch.0.join("observer-binding.json")).unwrap(),
            )
            .unwrap();
            std::fs::write(
                &path,
                serde_json::to_vec(
                    &json!({"schema_version":"1.0.0","state":"ready","binding":binding}),
                )
                .unwrap(),
            )
            .unwrap();
        };
        let (result, ()) = tokio::join!(operation, publisher);
        assert_eq!(result.as_ref().unwrap(), &42);
        assert!(launched);
        assert!(r.criteria.iter().all(|c| c.status == Status::Incomplete));
        assert_eq!(
            std::fs::read(path).unwrap(),
            std::fs::read(scratch.0.join("observer-ready.json")).unwrap()
        );
        retain_readiness_case(&scratch.0, "delayed-ready", &mut r, launched, &result);
    }
    // The retention tests below exercise the same post-validation path without
    // introducing a publication wait into quota and oracle tests.
    fn reference_lifecycle_criteria(
        s: &Scenario,
        o: &RunOptions,
        out: &Path,
        budget: &Budget,
        r: &mut Report,
    ) {
        if let Some(path) = &o.reference_lifecycle {
            let imported = crate::reference_lifecycle::load(
                path,
                &r.session_id,
                s.id(),
                r.environment.host_version.as_deref(),
            );
            retain_reference_lifecycle(imported, out, budget, r);
        }
    }
    fn reference_recording_criteria(
        s: &Scenario,
        o: &RunOptions,
        out: &Path,
        budget: &Budget,
        r: &mut Report,
    ) {
        if let Some(path) = &o.reference_recording {
            let imported = crate::reference_recording::load(
                path,
                &r.session_id,
                s.id(),
                r.environment.host_version.as_deref(),
            );
            retain_reference_recording(imported, out, budget, r);
        }
    }
    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Self {
            let path = std::env::temp_dir()
                .join("t035-t037")
                .join(uuid::Uuid::new_v4().to_string());
            std::fs::create_dir_all(&path).unwrap();
            Self(path.canonicalize().unwrap())
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            std::fs::remove_dir_all(&self.0).unwrap();
        }
    }
    fn scenario() -> Scenario {
        Scenario::from_value(json!({"schema_version":"1.0.0","id":"synthetic-runner","host":"127.0.0.1",
            "pairing":{"pin_delivery":"already_paired"},"requested":{"width":1920,"height":1080,"fps":60,"bitrate_kbps":20000,"codec":"h264","dynamic_range":"sdr"},
            "duration_seconds":0.05,"controllers":[{"id":0,"actions":[{"event_id":"connect","at_seconds":0,"type":"connect"},
                {"event_id":"cross","at_seconds":0.001,"type":"button","button":"cross","pressed":true}]}],
            "reference":{"required":false},"required_criteria":{"profile_id":"reference_host_compatibility","profile_version":"1.0.0",
                "ids":["REF-pairing","REF-stream","REF-controller","REF-teardown","REF-recording","REF-report"]},
            "evidence":{"host_metrics":true,"input_delivery":true,"reference_recording":false,"official_client_observations":true,"operator_observations":true},
            "output_dir":"artifacts","write_policy":{"max_bytes":1048576,"max_files":100,"on_limit":"incomplete"}})).unwrap()
    }
    fn report(s: &Scenario) -> Report {
        Report::new(
            s.as_value().clone(),
            Environment {
                firmware: None,
                host_version: None,
                client_version: "synthetic fixture".into(),
                network: "loopback fixture".into(),
                devices: vec![],
                reference_chain: None,
                clocks: vec![],
                calibration_ids: vec![],
            },
            "fixture-run".into(),
            "fixture-session".into(),
        )
        .unwrap()
    }

    struct ObserverFixture {
        scratch: Scratch,
        scenario: Scenario,
        report: Report,
        options: RunOptions,
        out: PathBuf,
    }
    impl ObserverFixture {
        fn new() -> Self {
            use sha2::{Digest, Sha256};
            let scratch = Scratch::new();
            let scenario = scenario();
            let mut report = report(&scenario);
            report.environment.host_version = Some("synthetic-host".into());
            let mut options = RunOptions::new(scratch.0.clone(), "report.json".into());
            options.reference_lifecycle = Some(scratch.0.join("lifecycle.json"));
            options.reference_recording = Some(scratch.0.join("recording.json"));
            let out = scratch.0.join("retained");
            std::fs::create_dir(&out).unwrap();
            let common = json!({"schema_version":"1.0.0","session_id":report.session_id,
                "scenario_id":scenario.id(),"host_version":"synthetic-host",
                "test_client_version":env!("CARGO_PKG_VERSION"),"test_client_revision":"synthetic-test",
                "official_client_version":"synthetic-test","provenance":"reference_host"});
            let mut lifecycle = common.clone();
            lifecycle["observations"] = json!([
                "pairing_proof_flow", "pairing_invalid_proofs_rejected", "pairing_official_client_cross_check",
                "teardown_session_terminated", "teardown_controllers_released", "teardown_resources_reclaimed"
            ].map(|check| json!({"check":check,"satisfied":true,"validity":"valid","reason":null,"artifacts":["lifecycle.txt"]})));
            lifecycle["artifacts"] = json!([{"path":"lifecycle.txt","sha256":hex::encode(Sha256::digest(b"synthetic lifecycle"))}]);
            let mut recording = common;
            recording["profile_id"] = json!("reference_host_compatibility");
            recording["profile_version"] = json!("1.0.0");
            recording["profile_sha256"] = json!(crate::reference_recording::profile_digest());
            recording["artifacts"] = json!([{"path":"recording.txt","sha256":hex::encode(Sha256::digest(b"synthetic recording"))}]);
            // A valid final snapshot with no populations is retained but must not
            // turn missing physical acquisition evidence into acceptance.
            recording["populations"] = json!([]);
            recording["reports"] = json!([]);
            for (name, bytes) in [
                ("lifecycle.txt", b"synthetic lifecycle".as_slice()),
                ("recording.txt", b"synthetic recording".as_slice()),
            ] {
                std::fs::write(scratch.0.join(name), bytes).unwrap();
            }
            for (name, value) in [
                ("lifecycle.pending", lifecycle),
                ("recording.pending", recording),
            ] {
                std::fs::write(scratch.0.join(name), serde_json::to_vec(&value).unwrap()).unwrap();
            }
            Self {
                scratch,
                scenario,
                report,
                options,
                out,
            }
        }
        fn status(&self, id: &str) -> Status {
            self.report
                .criteria
                .iter()
                .find(|c| c.id == id)
                .unwrap()
                .status
        }
        // Synthetic inputs deliberately exercise reference-host acceptance branches.
        // Captured reports from these tests are software fixtures, never live evidence.
        fn bind_controller(&mut self) {
            use sha2::{Digest, Sha256};
            let mut scenario = self.scenario.as_value().clone();
            scenario["controllers"][0]["actions"][1]["expected_delivery"] = json!(true);
            self.scenario = Scenario::from_value(scenario).unwrap();
            self.report = report(&self.scenario);
            self.report.environment.host_version = Some("synthetic-host".into());
            let row = json!({
                "schema_version":"1.0.0", "id":"synthetic-cross",
                "session_id":self.report.session_id, "provenance":"reference_host",
                "source":"host_pipeline", "kind":"input", "event_id":"cross",
                "controller_id":0, "boundary":"console_delivered", "authenticated":true,
                "delivery_boundary":"synthetic documented boundary",
                "clock_domain":"host-clock", "source_clock":{
                    "id":"host-clock", "type":"monotonic", "epoch":"boot",
                    "native_resolution_ns":1, "conversion":"floor(ns/1000)",
                    "quantization_bound_ns":1000, "uncertainty_ns":null,
                    "uncertainty_reason":"synthetic clock", "validity":"valid", "reason":null
                }, "observed_at":100,
                "population":{"expected":1,"observed":1,"represented":1,"excluded":0,
                    "missing":0,"overflow":0,"failed_delivery":0},
                "coverage":{"state":"complete","enabled_intervals":[{"start":1,"end":10000}],
                    "gaps":[],"reason":null},
                "validity":"valid", "reason":null,"artifact_refs":["lifecycle.txt"],"measurements":[]
            });
            let bytes = serde_json::to_vec(&row).unwrap();
            let path = self.scratch.0.join("controller.jsonl");
            std::fs::write(&path, &bytes).unwrap();
            self.options.host_observations = Some(path);
            let path = self.scratch.0.join("lifecycle.pending");
            let mut manifest: Value =
                serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
            manifest["artifacts"].as_array_mut().unwrap().push(
                json!({"path":"controller.jsonl","sha256":hex::encode(Sha256::digest(&bytes))}),
            );
            std::fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        }
        fn rewrite_controller(&self, bytes: &[u8]) {
            use sha2::{Digest, Sha256};
            std::fs::write(self.scratch.0.join("controller.jsonl"), bytes).unwrap();
            let path = self.scratch.0.join("lifecycle.pending");
            let mut manifest: Value =
                serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
            manifest["artifacts"][1]["sha256"] = json!(hex::encode(Sha256::digest(bytes)));
            std::fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
        }
        fn publish(&self) {
            for name in ["lifecycle", "recording"] {
                std::fs::rename(
                    self.scratch.0.join(format!("{name}.pending")),
                    self.scratch.0.join(format!("{name}.json")),
                )
                .unwrap();
            }
        }
        fn capture(&self, label: &str) {
            let Some(root) = std::env::var_os("MOONLIGHT_OBSERVER_TEST_EVIDENCE") else {
                return;
            };
            fn copy_tree(source: &Path, target: &Path) {
                std::fs::create_dir_all(target).unwrap();
                for entry in std::fs::read_dir(source).unwrap() {
                    let entry = entry.unwrap();
                    let target = target.join(entry.file_name());
                    if entry.file_type().unwrap().is_dir() {
                        copy_tree(&entry.path(), &target);
                    } else {
                        std::fs::copy(entry.path(), target).unwrap();
                    }
                }
            }
            let mut report = self.report.clone();
            report.finalize(None).unwrap();
            std::fs::write(
                self.out.join("report.json"),
                serde_json::to_vec_pretty(&report).unwrap(),
            )
            .unwrap();
            copy_tree(&self.scratch.0, &PathBuf::from(root).join(label));
        }
        async fn import(&mut self, cancellation_succeeded: bool, timeout: Duration) {
            reference_observer_criteria(
                &self.scenario,
                &self.options,
                &self.out,
                &Budget::new(1_048_576, 20),
                &mut self.report,
                cancellation_succeeded,
                ObserverWait {
                    timeout,
                    poll_interval: Duration::from_millis(2),
                },
            )
            .await;
        }
    }

    async fn delayed_observer_publication(partial: bool) {
        let mut fixture = ObserverFixture::new();
        let root = fixture.scratch.0.clone();
        if partial {
            for name in ["lifecycle.json", "recording.json"] {
                std::fs::write(root.join(name), b"{\"schema_version\":").unwrap();
            }
        }
        // The first branch reads missing/truncated files before its first await;
        // the publisher then commits complete files without a timing-based sleep.
        tokio::join!(biased; fixture.import(true, Duration::from_secs(1)), async {
            for name in ["lifecycle", "recording"] {
                std::fs::rename(root.join(format!("{name}.pending")), root.join(format!("{name}.json"))).unwrap();
            }
        });
        assert_eq!(fixture.status("REF-pairing"), Status::Pass);
        assert_eq!(fixture.status("REF-teardown"), Status::Pass);
        assert_eq!(fixture.status("REF-recording"), Status::Incomplete);
        assert_eq!(fixture.status("REF-report"), Status::Incomplete);
        assert_eq!(fixture.report.artifact_refs.len(), 4);
        assert_eq!(
            std::fs::read(fixture.out.join("reference-lifecycle-source-000.dat")).unwrap(),
            b"synthetic lifecycle"
        );
        assert_eq!(
            std::fs::read(fixture.out.join("reference-recording/recording.txt")).unwrap(),
            b"synthetic recording"
        );
        fixture.report.finalize(None).unwrap();
        fixture.report.validate().unwrap();
        fixture.capture(if partial {
            "partial-then-rename"
        } else {
            "delayed-atomic-publication"
        });
    }

    #[tokio::test]
    async fn controller_manifest_waits_for_publication_after_cancellation() {
        let mut fixture = ObserverFixture::new();
        fixture.bind_controller();
        let root = fixture.scratch.0.clone();
        tokio::join!(biased; fixture.import(true, Duration::from_secs(1)), async {
            for name in ["lifecycle", "recording"] {
                std::fs::rename(root.join(format!("{name}.pending")), root.join(format!("{name}.json"))).unwrap();
            }
        });
        assert_eq!(fixture.status("REF-controller"), Status::Pass);
        assert!(fixture.out.join("host-observations.jsonl").is_file());
        fixture.capture("controller-delayed-publication");
    }

    #[tokio::test]
    async fn controller_manifest_rejects_tampering_invalid_foreign_and_unbound_sources() {
        for case in [
            "tampered",
            "missing-source",
            "invalid-manifest",
            "foreign-manifest",
            "invalid-json",
            "foreign-session",
            "unbound",
            "unbound-reference",
            "outside",
            "provenance",
            "record-quota",
        ] {
            let mut fixture = ObserverFixture::new();
            fixture.bind_controller();
            let path = fixture.scratch.0.join("controller.jsonl");
            let original = std::fs::read(&path).unwrap();
            match case {
                "tampered" => std::fs::write(&path, b"changed after checksum").unwrap(),
                "missing-source" => std::fs::remove_file(&path).unwrap(),
                "invalid-manifest" | "foreign-manifest" => {
                    let path = fixture.scratch.0.join("lifecycle.pending");
                    let mut manifest: Value =
                        serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
                    if case == "invalid-manifest" {
                        manifest["artifacts"][1]["sha256"] = json!("invalid checksum");
                    } else {
                        manifest["session_id"] = json!("previous-session");
                    }
                    std::fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
                }
                "invalid-json" => fixture.rewrite_controller(b"{"),
                "foreign-session" | "provenance" | "unbound-reference" => {
                    let mut row: Value = serde_json::from_slice(&original).unwrap();
                    if case == "foreign-session" {
                        row["session_id"] = json!("another-session");
                    } else if case == "provenance" {
                        row["provenance"] = json!("synthetic_fixture");
                    } else {
                        row["artifact_refs"] = json!(["absent-source.txt"]);
                    }
                    fixture.rewrite_controller(&serde_json::to_vec(&row).unwrap());
                }
                "unbound" => {
                    let replacement = fixture.scratch.0.join("unbound.jsonl");
                    std::fs::write(&replacement, &original).unwrap();
                    fixture.options.host_observations = Some(replacement);
                }
                "outside" => {
                    fixture.options.host_observations =
                        Some(fixture.scratch.0.join("../controller.jsonl"))
                }
                "record-quota" => {
                    let mut rows = Vec::new();
                    let mut row: Value = serde_json::from_slice(&original).unwrap();
                    for id in 0..1025 {
                        row["id"] = json!(format!("synthetic-{id}"));
                        serde_json::to_writer(&mut rows, &row).unwrap();
                        rows.push(b'\n');
                    }
                    fixture.rewrite_controller(&rows);
                }
                _ => unreachable!(),
            }
            fixture.publish();
            tokio::time::timeout(
                Duration::from_millis(100),
                fixture.import(true, Duration::from_secs(10)),
            )
            .await
            .unwrap();
            assert_eq!(
                fixture.status("REF-controller"),
                Status::Incomplete,
                "{case}"
            );
            assert!(
                !fixture.out.join("host-observations.jsonl").exists(),
                "{case}"
            );
            fixture.capture(&format!("controller-rejected-{case}"));
        }
    }

    #[tokio::test]
    async fn controller_manifest_uses_snapshot_after_sources_are_replaced() {
        let mut fixture = ObserverFixture::new();
        fixture.bind_controller();
        let root = fixture.scratch.0.clone();
        let original = std::fs::read(root.join("controller.jsonl")).unwrap();
        std::fs::rename(root.join("lifecycle.pending"), root.join("lifecycle.json")).unwrap();
        tokio::join!(biased; fixture.import(true, Duration::from_secs(1)), async {
            std::fs::write(root.join("controller.jsonl"), b"invalid replacement").unwrap();
            std::fs::write(root.join("lifecycle.json"), b"invalid replacement").unwrap();
            std::fs::rename(root.join("recording.pending"), root.join("recording.json")).unwrap();
        });
        assert_eq!(fixture.status("REF-controller"), Status::Pass);
        assert_eq!(
            std::fs::read(fixture.out.join("reference-lifecycle-source-001.dat")).unwrap(),
            original
        );
        let retained = std::fs::read(fixture.out.join("host-observations.jsonl")).unwrap();
        let records =
            measurements::import_host(retained.as_slice(), &fixture.report.session_id, 1024)
                .unwrap();
        assert_eq!(
            controller_outcome(&[(0, "cross")], &records).0,
            Status::Pass
        );
        assert_eq!(
            records[0].artifact_refs,
            vec![ref_path("reference-lifecycle-source-000.dat")]
        );
        assert_eq!(
            std::fs::read(records[0].artifact_refs[0].resolve(&fixture.out).unwrap()).unwrap(),
            b"synthetic lifecycle"
        );
        fixture.capture("controller-validated-bytes-not-reopened");
    }

    #[tokio::test]
    async fn controller_manifest_unavailable_and_synthetic_observations_stay_incomplete() {
        for case in ["unavailable", "synthetic"] {
            let mut fixture = ObserverFixture::new();
            fixture.bind_controller();
            let mut row: Value = serde_json::from_slice(
                &std::fs::read(fixture.scratch.0.join("controller.jsonl")).unwrap(),
            )
            .unwrap();
            if case == "unavailable" {
                row["validity"] = json!("unavailable");
                row["reason"] = json!("synthetic missing ownership");
                row["observed_at"] = Value::Null;
                row["boundary"] = json!("host_received");
                row["authenticated"] = json!(false);
                row["delivery_boundary"] = Value::Null;
            } else {
                row["provenance"] = json!("synthetic_fixture");
                let path = fixture.scratch.0.join("lifecycle.pending");
                let mut manifest: Value =
                    serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
                manifest["provenance"] = json!("synthetic_fixture");
                std::fs::write(path, serde_json::to_vec(&manifest).unwrap()).unwrap();
            }
            fixture.rewrite_controller(&serde_json::to_vec(&row).unwrap());
            fixture.publish();
            fixture.import(true, Duration::from_secs(1)).await;
            assert_eq!(fixture.status("REF-controller"), Status::Incomplete);
            assert!(fixture.out.join("host-observations.jsonl").is_file());
            fixture.capture(&format!("controller-{case}"));
        }
    }

    #[tokio::test]
    async fn controller_manifest_missing_does_not_fall_back_to_jsonl() {
        for cancellation_succeeded in [true, false] {
            let mut fixture = ObserverFixture::new();
            fixture.bind_controller();
            let timeout = if cancellation_succeeded {
                Duration::from_millis(8)
            } else {
                Duration::from_secs(10)
            };
            tokio::time::timeout(
                Duration::from_millis(100),
                fixture.import(cancellation_succeeded, timeout),
            )
            .await
            .unwrap();
            assert_eq!(fixture.status("REF-controller"), Status::Incomplete);
            assert!(!fixture.out.join("host-observations.jsonl").exists());
            fixture.capture(if cancellation_succeeded {
                "controller-missing-deadline"
            } else {
                "controller-cancel-failed-no-wait"
            });
        }
    }

    #[tokio::test]
    async fn controller_manifest_and_recording_share_one_publication_deadline() {
        let mut fixture = ObserverFixture::new();
        fixture.bind_controller();
        // Three consumers still share the two manifest waits. A sequential second
        // 200 ms wait would exceed this outer bound, as would a separate controller wait.
        tokio::time::timeout(
            Duration::from_millis(350),
            fixture.import(true, Duration::from_millis(200)),
        )
        .await
        .unwrap();
        for id in [
            "REF-controller",
            "REF-pairing",
            "REF-teardown",
            "REF-recording",
            "REF-report",
        ] {
            assert_eq!(fixture.status(id), Status::Incomplete);
        }
        assert!(fixture.report.artifact_refs.is_empty());
        fixture.capture("controller-shared-publication-deadline");
    }

    #[tokio::test]
    async fn controller_manifest_failed_cancellation_consumes_available_snapshot_once() {
        let mut fixture = ObserverFixture::new();
        fixture.bind_controller();
        fixture.publish();
        tokio::time::timeout(
            Duration::from_millis(100),
            fixture.import(false, Duration::from_secs(10)),
        )
        .await
        .unwrap();
        assert_eq!(fixture.status("REF-controller"), Status::Pass);
        fixture.capture("controller-cancel-failed-available-snapshot");
    }

    #[tokio::test]
    async fn controller_manifest_quota_failure_prevents_acceptance() {
        for (bytes, files) in [(1, 20), (1_048_576, 2), (1_048_576, 3)] {
            let mut fixture = ObserverFixture::new();
            fixture.bind_controller();
            fixture.publish();
            reference_observer_criteria(
                &fixture.scenario,
                &fixture.options,
                &fixture.out,
                &Budget::new(bytes, files),
                &mut fixture.report,
                true,
                OBSERVER_WAIT,
            )
            .await;
            assert_eq!(fixture.status("REF-controller"), Status::Incomplete);
            fixture.capture(&format!("controller-quota-{bytes}-{files}"));
        }
    }

    #[tokio::test]
    async fn controller_standalone_compatibility_imports_once_after_cancellation() {
        let mut fixture = ObserverFixture::new();
        fixture.bind_controller();
        fixture.options.reference_lifecycle = None;
        fixture.options.reference_recording = None;
        fixture.import(false, Duration::from_secs(10)).await;
        assert_eq!(fixture.status("REF-controller"), Status::Pass);
        assert_eq!(fixture.status("REF-teardown"), Status::Incomplete);
        fixture.capture("controller-standalone-compatibility");
    }

    #[tokio::test]
    async fn observer_wait_accepts_delayed_atomic_publication_for_both_bindings() {
        delayed_observer_publication(false).await;
    }

    #[tokio::test]
    async fn observer_wait_accepts_partial_json_only_after_final_rename() {
        delayed_observer_publication(true).await;
    }

    #[tokio::test]
    async fn observer_wait_timeout_preserves_missing_partial_and_existing_fail_statuses() {
        for partial in [false, true] {
            let mut fixture = ObserverFixture::new();
            if partial {
                for name in ["lifecycle.json", "recording.json"] {
                    std::fs::write(fixture.scratch.0.join(name), b"{").unwrap();
                }
            }
            criterion(
                &mut fixture.report,
                "REF-pairing",
                Status::Fail,
                "prior verified failure",
                &[],
            );
            fixture.import(true, Duration::from_millis(8)).await;
            assert_eq!(fixture.status("REF-pairing"), Status::Fail);
            assert_eq!(fixture.status("REF-teardown"), Status::Incomplete);
            for id in ["REF-recording", "REF-report"] {
                assert_eq!(
                    fixture.status(id),
                    if partial {
                        Status::Fail
                    } else {
                        Status::Incomplete
                    }
                );
            }
            assert!(fixture.report.artifact_refs.is_empty());
            fixture.capture(if partial {
                "partial-timeout"
            } else {
                "missing-timeout"
            });
        }
    }

    #[tokio::test]
    async fn observer_wait_rejects_stale_foreign_and_invalid_snapshots_without_retry() {
        for (field, value) in [
            ("session_id", "previous-run"),
            ("scenario_id", "foreign-scenario"),
            ("host_version", "old-host"),
            ("test_client_version", "old-client"),
        ] {
            let mut fixture = ObserverFixture::new();
            for name in ["lifecycle", "recording"] {
                let mut value_json: Value = serde_json::from_slice(
                    &std::fs::read(fixture.scratch.0.join(format!("{name}.pending"))).unwrap(),
                )
                .unwrap();
                value_json[field] = json!(value);
                std::fs::write(
                    fixture.scratch.0.join(format!("{name}.json")),
                    serde_json::to_vec(&value_json).unwrap(),
                )
                .unwrap();
            }
            tokio::time::timeout(
                Duration::from_millis(100),
                fixture.import(true, Duration::from_secs(10)),
            )
            .await
            .unwrap();
            assert_eq!(fixture.status("REF-teardown"), Status::Incomplete);
            assert_eq!(fixture.status("REF-recording"), Status::Fail);
            assert_eq!(fixture.status("REF-report"), Status::Fail);
            assert!(fixture.report.artifact_refs.is_empty());
            fixture.capture(&format!("rejected-{field}"));
        }
    }

    #[tokio::test]
    async fn observer_wait_complete_manifests_with_missing_sources_are_terminal() {
        let mut fixture = ObserverFixture::new();
        let root = fixture.scratch.0.clone();
        for name in ["lifecycle", "recording"] {
            std::fs::copy(
                root.join(format!("{name}.pending")),
                root.join(format!("{name}.json")),
            )
            .unwrap();
            std::fs::remove_file(root.join(format!("{name}.txt"))).unwrap();
        }
        // Repair both sources and replace both manifests after the initial attempt.
        // A retry of nested NotFound would incorrectly accept these replacements.
        let (result, ()) = tokio::join!(biased;
            tokio::time::timeout(Duration::from_millis(100), fixture.import(true, Duration::from_secs(10))),
            async {
                for (name, bytes) in [("lifecycle", b"synthetic lifecycle".as_slice()), ("recording", b"synthetic recording".as_slice())] {
                    std::fs::write(root.join(format!("{name}.txt")), bytes).unwrap();
                    std::fs::rename(root.join(format!("{name}.pending")), root.join(format!("{name}.json"))).unwrap();
                }
            }
        );
        result.unwrap();
        for id in ["REF-pairing", "REF-teardown", "REF-recording", "REF-report"] {
            assert_eq!(fixture.status(id), Status::Incomplete);
        }
        assert!(fixture.report.artifact_refs.is_empty());
        fixture.capture("complete-manifests-missing-sources");
    }

    #[tokio::test]
    async fn observer_wait_complete_recording_with_nested_json_eof_is_terminal() {
        use sha2::{Digest, Sha256};
        let mut fixture = ObserverFixture::new();
        fixture.options.reference_lifecycle = None;
        let root = fixture.scratch.0.clone();
        let mut manifest: Value =
            serde_json::from_slice(&std::fs::read(root.join("recording.pending")).unwrap())
                .unwrap();
        for name in ["expected.json", "emitted.json"] {
            std::fs::write(root.join(name), b"{").unwrap();
            manifest["artifacts"]
                .as_array_mut()
                .unwrap()
                .push(json!({"path":name,"sha256":hex::encode(Sha256::digest(b"{"))}));
        }
        manifest["reports"] = json!([{"kind":"pass","expected":"expected.json","emitted":"emitted.json","oracle":"recording.txt"}]);
        std::fs::write(
            root.join("recording.json"),
            serde_json::to_vec(&manifest).unwrap(),
        )
        .unwrap();
        // Preserve the rejected final publication as evidence before replacement.
        std::fs::copy(
            root.join("recording.json"),
            root.join("rejected-recording.json"),
        )
        .unwrap();
        let (result, ()) = tokio::join!(biased;
            tokio::time::timeout(Duration::from_millis(100), fixture.import(true, Duration::from_secs(10))),
            async {
                std::fs::rename(root.join("recording.pending"), root.join("recording.json")).unwrap();
            }
        );
        result.unwrap();
        for id in ["REF-recording", "REF-report"] {
            assert_eq!(fixture.status(id), Status::Fail);
        }
        assert!(fixture.report.artifact_refs.is_empty());
        fixture.capture("complete-recording-nested-json-eof");
    }

    #[tokio::test]
    async fn observer_wait_consumes_exact_parsed_manifest_without_rereading_path() {
        let mut fixture = ObserverFixture::new();
        let budget = Budget::new(1_048_576, 20);
        for name in ["lifecycle", "recording"] {
            std::fs::rename(
                fixture.scratch.0.join(format!("{name}.pending")),
                fixture.scratch.0.join(format!("{name}.json")),
            )
            .unwrap();
        }
        let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
        let lifecycle = fixture.options.reference_lifecycle.as_ref().unwrap();
        let imported = await_observer_snapshot(
            || crate::reference_lifecycle::read_manifest(lifecycle),
            |manifest| {
                std::fs::write(lifecycle, b"invalid replacement").unwrap();
                crate::reference_lifecycle::load_manifest(
                    lifecycle,
                    manifest,
                    &fixture.report.session_id,
                    fixture.scenario.id(),
                    Some("synthetic-host"),
                )
            },
            deadline,
            Duration::from_millis(2),
        )
        .await
        .unwrap();
        assert_eq!(imported.outcome("REF-teardown").0, Status::Pass);
        retain_reference_lifecycle(Ok(imported), &fixture.out, &budget, &mut fixture.report);
        let recording = fixture.options.reference_recording.as_ref().unwrap();
        let imported = await_observer_snapshot(
            || crate::reference_recording::read_manifest(recording),
            |manifest| {
                std::fs::write(recording, b"invalid replacement").unwrap();
                crate::reference_recording::load_manifest(
                    recording,
                    manifest,
                    &fixture.report.session_id,
                    fixture.scenario.id(),
                    Some("synthetic-host"),
                )
            },
            deadline,
            Duration::from_millis(2),
        )
        .await
        .unwrap();
        assert_eq!(imported.artifacts[0].1, b"synthetic recording");
        retain_reference_recording(Ok(imported), &fixture.out, &budget, &mut fixture.report);
        assert_eq!(fixture.report.artifact_refs.len(), 4);
        fixture.capture("parsed-manifests-not-reread");
    }

    #[tokio::test]
    async fn observer_wait_failed_cancellation_does_not_wait_or_invent_cleanup() {
        let mut fixture = ObserverFixture::new();
        tokio::time::timeout(
            Duration::from_millis(100),
            fixture.import(false, Duration::from_secs(10)),
        )
        .await
        .unwrap();
        for id in ["REF-pairing", "REF-teardown", "REF-recording", "REF-report"] {
            assert_eq!(fixture.status(id), Status::Incomplete);
        }
        assert!(fixture.report.artifact_refs.is_empty());
        fixture.capture("cancellation-failed");
    }

    #[tokio::test]
    async fn observer_wait_retains_validated_bytes_while_other_binding_is_pending() {
        let mut fixture = ObserverFixture::new();
        let root = fixture.scratch.0.clone();
        std::fs::rename(root.join("lifecycle.pending"), root.join("lifecycle.json")).unwrap();
        tokio::join!(biased; fixture.import(true, Duration::from_secs(1)), async {
            std::fs::write(root.join("lifecycle.json"), b"invalid replacement").unwrap();
            std::fs::write(root.join("lifecycle.txt"), b"replaced bytes").unwrap();
            std::fs::rename(root.join("recording.pending"), root.join("recording.json")).unwrap();
        });
        assert_eq!(fixture.status("REF-teardown"), Status::Pass);
        assert_eq!(
            std::fs::read(fixture.out.join("reference-lifecycle-source-000.dat")).unwrap(),
            b"synthetic lifecycle"
        );
        crate::reference_lifecycle::load(
            &fixture.out.join("reference-lifecycle.json"),
            &fixture.report.session_id,
            fixture.scenario.id(),
            Some("synthetic-host"),
        )
        .unwrap();
        assert!(fixture
            .out
            .join("reference-recording/manifest.json")
            .is_file());
        fixture.capture("validated-bytes-retained");
    }
    #[test]
    fn shared_budget_never_overwrites_or_exceeds_limits() {
        let scratch = Scratch::new();
        let budget = Budget::new(3, 1);
        let mut file = budget.file(&scratch.0, "one").unwrap();
        file.write_all(b"abc").unwrap();
        assert!(file.write_all(b"d").is_err());
        assert!(budget.file(&scratch.0, "two").is_err());
        assert_eq!(std::fs::read(scratch.0.join("one")).unwrap(), b"abc");
    }
    #[test]
    fn reference_lifecycle_retains_verified_sources_before_promoting_criteria() {
        use sha2::{Digest, Sha256};
        for (bytes, files) in [(1_048_576, 10), (1, 10), (1_048_576, 1)] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let observer = scratch.0.join("observer");
            let out = scratch.0.join("retained");
            std::fs::create_dir(&observer).unwrap();
            std::fs::create_dir(&out).unwrap();
            let names = ["second.txt", "reference-lifecycle-source-000.dat"];
            let source = b"SYNTHETIC lifecycle evidence";
            for name in names {
                std::fs::write(observer.join(name), source).unwrap();
            }
            let observations: Vec<_> = [
                "pairing_proof_flow",
                "pairing_invalid_proofs_rejected",
                "pairing_official_client_cross_check",
                "teardown_session_terminated",
                "teardown_controllers_released",
                "teardown_resources_reclaimed",
            ].iter().map(|check| json!({"check":check,"satisfied":true,"validity":"valid","reason":null,"artifacts":names})).collect();
            let manifest = json!({
                "schema_version":"1.0.0","session_id":r.session_id,"scenario_id":s.id(),
                "host_version":"synthetic-host","test_client_version":env!("CARGO_PKG_VERSION"),
                "test_client_revision":"synthetic-revision","official_client_version":"synthetic-official",
                "provenance":"reference_host","observations":observations,
                "artifacts":names.iter().map(|name| json!({"path":name,"sha256":hex::encode(Sha256::digest(source))})).collect::<Vec<_>>()
            });
            let path = observer.join("snapshot.json");
            std::fs::write(&path, serde_json::to_vec(&manifest).unwrap()).unwrap();
            let mut options = RunOptions::new(scratch.0.clone(), "report.json".into());
            options.reference_lifecycle = Some(path);
            let options = options.with_report("other-report.json".into());
            reference_lifecycle_criteria(&s, &options, &out, &Budget::new(bytes, files), &mut r);
            let expected = if bytes > 1 && files > 1 {
                Status::Pass
            } else {
                Status::Incomplete
            };
            for id in ["REF-pairing", "REF-teardown"] {
                assert_eq!(
                    r.criteria.iter().find(|c| c.id == id).unwrap().status,
                    expected
                );
            }
            for id in ["REF-recording", "REF-report"] {
                assert_eq!(
                    r.criteria.iter().find(|c| c.id == id).unwrap().status,
                    Status::Incomplete
                );
            }
            if expected == Status::Pass {
                let retained = crate::reference_lifecycle::load(
                    &out.join("reference-lifecycle.json"),
                    &r.session_id,
                    s.id(),
                    Some("synthetic-host"),
                )
                .unwrap();
                assert_eq!(retained.artifacts.len(), 2);
                assert_ne!(
                    retained.manifest.observations[0].artifacts[0],
                    retained.manifest.observations[0].artifacts[1]
                );
                for artifact in &r.artifact_refs {
                    assert!(artifact.resolve(&out).unwrap().is_file());
                }
            } else {
                assert!(r.artifact_refs.is_empty());
            }
            r.finalize(None).unwrap();
            assert_eq!(r.overall_status, Status::Incomplete);
            r.validate().unwrap();
        }
    }
    #[test]
    fn reference_recording_retention_quota_and_invalid_snapshot_statuses() {
        use sha2::{Digest, Sha256};
        for (bytes, files) in [(1_048_576, 10), (1, 10), (1_048_576, 1)] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            r.environment.host_version = Some("synthetic-host".into());
            let out = scratch.0.join("retained");
            std::fs::create_dir(&out).unwrap();
            std::fs::write(scratch.0.join("source.txt"), b"observer").unwrap();
            let snapshot = json!({"schema_version":"1.0.0","session_id":r.session_id,"scenario_id":s.id(),
                "host_version":"synthetic-host","test_client_version":env!("CARGO_PKG_VERSION"),"test_client_revision":"test","official_client_version":"test",
                "profile_id":"reference_host_compatibility","profile_version":"1.0.0","profile_sha256":crate::reference_recording::profile_digest(),"provenance":"reference_host",
                "artifacts":[{"path":"source.txt","sha256":hex::encode(Sha256::digest(b"observer"))}],"populations":[],"reports":[]});
            let path = scratch.0.join("snapshot.json");
            std::fs::write(&path, serde_json::to_vec(&snapshot).unwrap()).unwrap();
            let mut options = RunOptions::new(scratch.0.clone(), "report.json".into());
            options.reference_recording = Some(path.clone());
            let options = options.with_report("other.json".into());
            reference_recording_criteria(&s, &options, &out, &Budget::new(bytes, files), &mut r);
            assert!(r
                .criteria
                .iter()
                .filter(|c| matches!(c.id.as_str(), "REF-recording" | "REF-report"))
                .all(|c| c.status == Status::Incomplete));
            if bytes > 1 && files > 1 {
                assert_eq!(r.artifact_refs.len(), 2);
                let imported = crate::reference_recording::load(
                    &out.join("reference-recording/manifest.json"),
                    &r.session_id,
                    s.id(),
                    Some("synthetic-host"),
                )
                .unwrap();
                assert_eq!(imported.artifacts[0].1, b"observer");
                assert_eq!(
                    std::fs::read(&path).unwrap(),
                    serde_json::to_vec(&snapshot).unwrap()
                );
            } else {
                assert!(r.artifact_refs.is_empty());
            }
            std::fs::write(&path, b"malformed").unwrap();
            reference_recording_criteria(&s, &options, &out, &Budget::new(1000, 10), &mut r);
            assert!(r
                .criteria
                .iter()
                .filter(|c| matches!(c.id.as_str(), "REF-recording" | "REF-report"))
                .all(|c| c.status == Status::Fail));
            std::fs::remove_file(&path).unwrap();
            reference_recording_criteria(&s, &options, &out, &Budget::new(1000, 10), &mut r);
            assert!(r
                .criteria
                .iter()
                .filter(|c| matches!(c.id.as_str(), "REF-recording" | "REF-report"))
                .all(|c| c.status == Status::Fail));
            r.finalize(None).unwrap();
            r.validate().unwrap();
        }
    }

    #[test]
    fn reference_recording_verified_oracle_failure_survives_byte_and_file_quota_exhaustion() {
        use sha2::{Digest, Sha256};
        for (mismatch, recording_failure) in
            [(false, false), (true, false), (false, true), (true, true)]
        {
            for (bytes, files) in [(1, 32), (1_048_576, 1)] {
                let scratch = Scratch::new();
                let s = scenario();
                let mut r = report(&s);
                r.environment.host_version = Some("synthetic-host".into());
                let out = scratch.0.join("retained");
                std::fs::create_dir(&out).unwrap();
                let mut artifacts = Vec::new();
                let mut cases = Vec::new();
                for (index, kind) in ["pass", "fail", "incomplete", "fail_under_incomplete"]
                    .iter()
                    .enumerate()
                {
                    let mut expected = report(&s);
                    expected.environment.host_version = Some("synthetic-host".into());
                    expected.environment.client_version = env!("CARGO_PKG_VERSION").into();
                    for c in &mut expected.criteria {
                        c.status = match index {
                            0 => Status::Pass,
                            1 => Status::Fail,
                            _ => Status::Incomplete,
                        };
                        c.evidence_ids = vec!["independent-oracle".into()];
                    }
                    if index == 3 {
                        expected.criteria[0].status = Status::Fail;
                    }
                    expected.finalize(None).unwrap();
                    expected.validate().unwrap();
                    let mut emitted = expected.clone();
                    if mismatch && index == 0 {
                        emitted.audio.non_silent = Some(true);
                    }
                    emitted.validate().unwrap();
                    let names = [
                        format!("expected-{index}.json"),
                        format!("emitted-{index}.json"),
                    ];
                    for (name, record) in names.iter().zip([&expected, &emitted]) {
                        let data = serde_json::to_vec(record).unwrap();
                        std::fs::write(scratch.0.join(name), &data).unwrap();
                        artifacts
                            .push(json!({"path":name,"sha256":hex::encode(Sha256::digest(&data))}));
                    }
                    cases.push(json!({"kind":kind,"expected":names[0],"emitted":names[1],"oracle":"oracle.txt"}));
                }
                std::fs::write(
                    scratch.0.join("oracle.txt"),
                    b"independent synthetic oracle",
                )
                .unwrap();
                artifacts.push(json!({"path":"oracle.txt","sha256":hex::encode(Sha256::digest(b"independent synthetic oracle"))}));
                let mut populations = Vec::new();
                if recording_failure {
                    let pixels: Vec<u8> = (0..384).map(|n| (n % 200 + 20) as u8).collect();
                    let scores=serde_json::to_vec(&json!({"stream_frame":0,"stream_time_seconds":1.0,
                        "reference_frame":0,"reference_time_seconds":1.0,"timestamp_residual_ms":0.0,
                        "ssim":1.0,"ssim_bits":"3ff0000000000000","psnr_db_yuv":[null,null,null],
                        "perfect_psnr_yuv":[true,true,true],"exclusion_reason":null})).unwrap();
                    for (name, data) in [
                        ("stream.raw", &pixels),
                        ("reference.raw", &pixels),
                        ("scores.jsonl", &scores),
                    ] {
                        std::fs::write(scratch.0.join(name), data).unwrap();
                        artifacts
                            .push(json!({"path":name,"sha256":hex::encode(Sha256::digest(data))}));
                    }
                    let native = |path: &str| {
                        json!({"source":"synthetic independent source","capture_mode":{"width":16,"height":16,"fps":60},
                        "calibration_id":"test-calibration","calibration_reason":null,"frames":path,"pixel_format":"yuv420p",
                        "bit_depth":8,"color_space":"BT.709","clock_domain":"test-clock","frame_count":1,"timestamp_count":1,
                        "timestamps":[1000000],"validity":"valid","metadata_evidence":null})
                    };
                    populations.push(json!({"kind":"sdr","stream":native("stream.raw"),"reference":native("reference.raw"),
                        "calibration":{"id":"test-calibration","provenance":"reference_host","source":"oracle.txt","uncertainty_ms":0.0},
                        "source_hdr":null,"stream_hdr":null,"delay_ms":0.0,"expected_delay_ms":[0.0,0.0],
                        "expected_ssim":[0.0,0.5],"oracle":"oracle.txt","scores":"scores.jsonl"}));
                }
                let snapshot = json!({"schema_version":"1.0.0","session_id":r.session_id,"scenario_id":s.id(),"host_version":"synthetic-host",
                    "test_client_version":env!("CARGO_PKG_VERSION"),"test_client_revision":"test","official_client_version":"test",
                    "profile_id":"reference_host_compatibility","profile_version":"1.0.0","profile_sha256":crate::reference_recording::profile_digest(),
                    "provenance":"reference_host","artifacts":artifacts,"populations":populations,"reports":cases});
                let path = scratch.0.join("snapshot.json");
                std::fs::write(&path, serde_json::to_vec(&snapshot).unwrap()).unwrap();
                let imported = crate::reference_recording::load(
                    &path,
                    &r.session_id,
                    s.id(),
                    Some("synthetic-host"),
                )
                .unwrap();
                assert_eq!(
                    imported.report,
                    if mismatch { Status::Fail } else { Status::Pass }
                );
                assert_eq!(
                    imported.recording,
                    if recording_failure {
                        Status::Fail
                    } else {
                        Status::Incomplete
                    }
                );
                let mut options = RunOptions::new(scratch.0.clone(), "report.json".into());
                options.reference_recording = Some(path);
                reference_recording_criteria(
                    &s,
                    &options,
                    &out,
                    &Budget::new(bytes, files),
                    &mut r,
                );
                let result = r.criteria.iter().find(|c| c.id == "REF-report").unwrap();
                assert_eq!(
                    result.status,
                    if mismatch {
                        Status::Fail
                    } else {
                        Status::Incomplete
                    }
                );
                assert!(result.evidence_ids.is_empty());
                assert!(r.artifact_refs.is_empty());
                assert_eq!(
                    r.criteria
                        .iter()
                        .find(|c| c.id == "REF-recording")
                        .unwrap()
                        .status,
                    if recording_failure {
                        Status::Fail
                    } else {
                        Status::Incomplete
                    }
                );
                assert!(r
                    .criteria
                    .iter()
                    .find(|c| c.id == "REF-recording")
                    .unwrap()
                    .evidence_ids
                    .is_empty());
                assert!(r.limitations.iter().any(|l| l.contains("quota")));
                r.finalize(None).unwrap();
                r.validate().unwrap();
                assert_eq!(r.overall_status, Status::Incomplete);
            }
        }
    }

    fn comparison_profile() -> ComparisonProfile {
        ComparisonProfile {
            schema_version: "1.0.0".into(),
            population: "all_stream_frames".into(),
            width: 32,
            height: 32,
            bit_depth: 8,
            color_space: crate::recording::ColorSpace::Bt709,
            calibration_id: Some("synthetic-fixture-calibration".into()),
            options: crate::compare::CompareOptions {
                min_delay_ms: 0.0,
                max_delay_ms: 0.0,
                ..Default::default()
            },
            criteria: vec![
                PictureCriterion {
                    id: "ssim-floor".into(),
                    metric: PictureMetric::SsimFloorFraction,
                    threshold: crate::verdict::Threshold {
                        operator: crate::verdict::Operator::Gte,
                        value: Some(0.99),
                        units: Some("ratio".into()),
                    },
                    uncertainty: Some(0.0),
                },
                PictureCriterion {
                    id: "required-calibrated-median".into(),
                    metric: PictureMetric::SsimMedian,
                    threshold: crate::verdict::Threshold {
                        operator: crate::verdict::Operator::Gte,
                        value: None,
                        units: Some("ratio".into()),
                    },
                    uncertainty: None,
                },
            ],
        }
    }
    #[test]
    fn received_frame_rate_uses_a_boot_scoped_clock_and_rate_unit() {
        let r = report(&scenario());
        let m = receipt_measurement(
            "observed-fps",
            &r,
            Some(60.0),
            Units::FramesPerSecond,
            (60, 60),
            &Interval {
                start: 1000,
                end: 1001000,
            },
            "video.jsonl",
        )
        .unwrap();
        assert_eq!(
            m.clock_domain.as_deref(),
            Some(m.source_clock.as_ref().unwrap().id.as_str())
        );
        m.validate().unwrap();
        assert!(serde_json::from_value::<Units>(json!("frames_per_second")).is_ok());
    }
    #[tokio::test]
    async fn missing_comparison_evidence_retains_frozen_subchecks_and_writes_report() {
        let scratch = Scratch::new();
        let profile = comparison_profile();
        let profile_path = scratch.0.join("profile.json");
        std::fs::write(&profile_path, serde_json::to_vec(&profile).unwrap()).unwrap();
        let (_stop, cancel) = watch::channel(false);
        let result = compare_in_root(
            &scratch.0,
            &scratch.0.join("absent-stream"),
            &scratch.0.join("absent-reference"),
            &scratch.0.join("comparison"),
            Some(&profile_path),
            Some(Path::new("comparison-report.json")),
            cancel,
        )
        .await
        .unwrap();
        assert_eq!(result["overall_status"], "Incomplete");
        assert_eq!(result["criteria"].as_array().unwrap().len(), 2);
        assert_eq!(result["profile"], serde_json::to_value(profile).unwrap());
        let persisted: Value = serde_json::from_slice(
            &std::fs::read(scratch.0.join("comparison-report.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(result, persisted);
    }
    #[tokio::test]
    async fn native_fixture_comparison_preserves_failures_under_unresolved_targets() {
        let scratch = Scratch::new();
        for name in ["stream", "reference"] {
            let dir = scratch.0.join(name);
            std::fs::create_dir(&dir).unwrap();
            let index = reference::RecordingIndex {
                source: "synthetic comparison fixture".into(),
                width: 32,
                height: 32,
                timestamps: vec![1.0, 1.02, 1.04, 1.06],
            };
            reference::write_index(&dir, &index).unwrap();
            let frame: Vec<u8> = (0..32 * 32 * 3 / 2)
                .map(|i| {
                    if name == "reference" {
                        (i % 170 + 40) as u8
                    } else {
                        (i % 70 + 10) as u8
                    }
                })
                .collect();
            std::fs::write(dir.join("frames.yuv"), frame.repeat(4)).unwrap();
            let mut metadata: Value =
                serde_json::from_slice(&std::fs::read(dir.join("recording.json")).unwrap())
                    .unwrap();
            metadata["calibration_id"] = json!("synthetic-fixture-calibration");
            metadata["calibration_reason"] = Value::Null;
            std::fs::write(
                dir.join("recording.json"),
                serde_json::to_vec(&metadata).unwrap(),
            )
            .unwrap();
        }
        let profile = comparison_profile();
        let profile_path = scratch.0.join("profile.json");
        std::fs::write(&profile_path, serde_json::to_vec(&profile).unwrap()).unwrap();
        let (_stop, cancel) = watch::channel(false);
        let result = compare_in_root(
            &scratch.0,
            &scratch.0.join("stream"),
            &scratch.0.join("reference"),
            &scratch.0.join("comparison"),
            Some(&profile_path),
            Some(Path::new("report.json")),
            cancel,
        )
        .await
        .unwrap();
        assert_eq!(result["overall_status"], "Incomplete");
        assert_eq!(result["criteria"][0]["status"], "Fail");
        assert_eq!(result["criteria"][1]["status"], "Incomplete");
        assert_eq!(result["comparison"]["stream_frames"], 4);
        assert_eq!(result["comparison"]["excluded_frames"], 0);
        assert_eq!(result["comparison"]["bit_depth"], 8);
    }
    #[tokio::test]
    async fn cancelled_prelaunch_run_still_finalizes_and_preserves_declaration() {
        let scratch = Scratch::new();
        let s = scenario();
        let mut options = RunOptions::new(scratch.0.clone(), "report.json".into());
        options.config_dir = Some(scratch.0.join("identity"));
        let (stop, cancel) = watch::channel(true);
        let report = run(&s, &options, cancel).await.unwrap();
        drop(stop);
        assert_eq!(report.overall_status, Status::Incomplete);
        assert_eq!(report.scenario(), s.as_value());
        assert_eq!(report.criteria.len(), 6);
        report.validate().unwrap();
        let bytes = std::fs::read(scratch.0.join("report.json")).unwrap();
        let _: Report = serde_json::from_slice(&bytes).unwrap();
        let (_stop, cancel) = watch::channel(false);
        assert!(
            run(&s, &options, cancel).await.is_err(),
            "must not overwrite report"
        );
    }
    #[cfg(unix)]
    #[test]
    fn report_destination_refuses_traversal_symlinks_and_external_paths() {
        let scratch = Scratch::new();
        let root = scratch.0.canonicalize().unwrap();
        std::os::unix::fs::symlink(&root, root.join("alias")).unwrap();
        for path in [
            PathBuf::from("../escape"),
            PathBuf::from("alias/report.json"),
            PathBuf::from("/outside/report.json"),
        ] {
            assert!(
                local_report_path(&root, &path).is_err(),
                "{}",
                path.display()
            );
        }
    }
    #[tokio::test]
    async fn loopback_input_cleanup_precedes_media_drain_and_disconnect() {
        for cancelled_run in [false, true] {
            let scratch = Scratch::new();
            let s = scenario();
            let mut r = report(&s);
            let options = RunOptions::new(scratch.0.clone(), "report.json".into());
            let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
            let remote = socket.local_addr().unwrap();
            let mut host = rusty_enet::Host::new(
                socket,
                rusty_enet::HostSettings {
                    peer_limit: 1,
                    channel_limit: 0x30,
                    ..Default::default()
                },
            )
            .unwrap();
            let server = tokio::spawn(async move {
                let until = tokio::time::Instant::now() + Duration::from_secs(5);
                let mut received = 0;
                loop {
                    assert!(
                        tokio::time::Instant::now() < until,
                        "cleanup must disconnect"
                    );
                    match host.service().unwrap() {
                        Some(rusty_enet::Event::Receive { .. }) => received += 1,
                        Some(rusty_enet::Event::Disconnect { .. }) => return received,
                        _ => tokio::time::sleep(Duration::from_millis(1)).await,
                    }
                }
            });
            let control = control::ControlConnection::connect(remote, 1, [1; 16], true)
                .await
                .unwrap();
            let request = http::LaunchRequest::with_random_keys(1, 1920, 1080, 60, false);
            let settings = rtsp::StreamSettings {
                width: 1920,
                height: 1080,
                fps: 60,
                bitrate_kbps: 20000,
                codec: crate::codec::Codec::H264,
                hdr: false,
                packet_size: 1392,
                encrypt_av: false,
            };
            let negotiation = rtsp::RtspSession {
                requested: settings.clone(),
                announced: settings,
                host_chosen_bitrate_kbps: None,
                session_id: "fixture".into(),
                describe: rtsp::Describe::default(),
                audio_port: remote.port(),
                video_port: remote.port(),
                control_port: remote.port(),
                ping_payload: "0123456789abcdef".into(),
                video_ping_payload: "0123456789abcdef".into(),
                connect_data: 1,
                encryption_enabled: 1,
                packet_size: 1392,
                encrypted_rtsp: false,
            };
            let mut session = rtsp::ActiveSession {
                negotiation,
                control,
            };
            let (stop, mut cancel) = watch::channel(false);
            let interrupter = tokio::spawn(async move {
                if cancelled_run {
                    tokio::time::sleep(Duration::from_millis(10)).await;
                    let _ = stop.send(true);
                } else {
                    tokio::time::sleep(Duration::from_secs(2)).await;
                }
            });
            let budget = Budget::new(1048576, 100);
            let result = observe(
                ObserveContext {
                    scenario: &s,
                    options: &options,
                    out: &scratch.0,
                    budget: &budget,
                    request: &request,
                },
                &mut r,
                &mut session,
                &mut cancel,
            )
            .await;
            assert_eq!(result.is_err(), cancelled_run);
            session.teardown().await.unwrap();
            interrupter.abort();
            assert!(server.await.unwrap() > 3);
            let log = std::fs::read_to_string(scratch.0.join("input.jsonl")).unwrap();
            let events: Vec<Value> = log
                .lines()
                .map(|line| serde_json::from_str(line).unwrap())
                .collect();
            assert_eq!(events[0]["event_id"], "connect");
            assert_eq!(events.last().unwrap()["event_id"], "cleanup-0");
            assert!(events.last().unwrap()["packets_queued"].as_u64().unwrap() > 0);
            assert!(
                r.input.console_delivered.is_empty(),
                "ACK cannot establish delivery"
            );
            assert!(r.video.host_drops.is_none());
            assert!(
                r.video.missing_frames.is_none(),
                "no frame sequence was observed; missing-frame counts are unknown"
            );
            assert!(r.video.incomplete_frames.is_none());
            assert!(r.video.recoverable_with_fec.is_none());
            r.finalize(cancelled_run.then_some("fixture cancellation"))
                .unwrap();
            assert_eq!(r.overall_status, Status::Incomplete);
            if !cancelled_run {
                assert_eq!(
                    r.criteria
                        .iter()
                        .find(|c| c.id == "REF-stream")
                        .unwrap()
                        .status,
                    Status::Fail
                );
            }
        }
    }
}
