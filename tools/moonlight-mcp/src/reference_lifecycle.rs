//! Trusted observer records for the reference host's pairing and teardown boundaries.
//! This validates and retains an independent observer's evidence, not its truth by inspection.
//! A hash binds source bytes; it does not turn an operator assertion into an observation.

use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::File;
use std::io::Read;
use std::path::Path;

use crate::evidence::{validate_id, validate_reason, ArtifactRef, Provenance, Validity};
use crate::report::Status;

pub const MAX_MANIFEST_BYTES: usize = 65_536;
const MAX_ARTIFACT_BYTES: usize = 1_048_576;
const MAX_TOTAL_BYTES: usize = 4_194_304;

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "snake_case")]
pub enum Check {
    PairingProofFlow,
    PairingInvalidProofsRejected,
    PairingOfficialClientCrossCheck,
    TeardownSessionTerminated,
    TeardownControllersReleased,
    TeardownResourcesReclaimed,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Observation {
    pub check: Check,
    pub satisfied: Option<bool>,
    pub validity: Validity,
    pub reason: Option<String>,
    pub artifacts: Vec<ArtifactRef>,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Artifact {
    pub path: ArtifactRef,
    pub sha256: String,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub schema_version: String,
    pub session_id: String,
    pub scenario_id: String,
    pub host_version: String,
    pub test_client_version: String,
    pub test_client_revision: String,
    pub official_client_version: String,
    pub provenance: Provenance,
    pub artifacts: Vec<Artifact>,
    pub observations: Vec<Observation>,
}

pub struct Imported {
    pub manifest: Manifest,
    /// Bounded, checksum-verified source bytes. Retain these exact bytes, not a later reread.
    pub artifacts: Vec<(ArtifactRef, Vec<u8>)>,
}

impl Imported {
    pub fn outcome(&self, criterion: &str) -> (Status, &'static str) {
        let required = match criterion {
            "REF-pairing" => &[
                Check::PairingProofFlow,
                Check::PairingInvalidProofsRejected,
                Check::PairingOfficialClientCrossCheck,
            ],
            "REF-teardown" => &[
                Check::TeardownSessionTerminated,
                Check::TeardownControllersReleased,
                Check::TeardownResourcesReclaimed,
            ],
            _ => {
                return (
                    Status::Incomplete,
                    "no lifecycle evaluator for this criterion",
                )
            }
        };
        if self.manifest.provenance != Provenance::ReferenceHost {
            return (
                Status::Incomplete,
                "only independently collected reference-host observations establish this criterion",
            );
        }
        let mut missing = false;
        for check in required {
            match self
                .manifest
                .observations
                .iter()
                .find(|o| o.check == *check)
            {
                Some(observation) if observation.validity == Validity::Valid => {
                    if observation.satisfied == Some(false) {
                        return (
                            Status::Fail,
                            "the reference observer demonstrated a required check failed",
                        );
                    }
                    missing |= observation.satisfied != Some(true);
                }
                _ => missing = true,
            }
        }
        if missing {
            (
                Status::Incomplete,
                "a required reference observation is missing or invalid",
            )
        } else {
            (Status::Pass, "the independent reference observer recorded every required check with retained source evidence")
        }
    }
}

fn read_bounded(path: &Path, limit: usize) -> Result<Vec<u8>> {
    let metadata = std::fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && metadata.len() <= limit as u64,
        "nonregular or oversized evidence file"
    );
    let file = File::open(path)?;
    ensure!(file.metadata()?.is_file(), "nonregular evidence file");
    let mut bytes = Vec::new();
    file.take((limit + 1) as u64).read_to_end(&mut bytes)?;
    ensure!(
        !bytes.is_empty() && bytes.len() <= limit,
        "empty or oversized evidence file"
    );
    Ok(bytes)
}

/// Read only a separately bound local observer snapshot; never accept agent tool input.
/// All files must be regular files beneath its directory, and all references are validated
/// before any criterion can change. Missing or malformed evidence is an import error.
pub fn load(
    path: &Path,
    session_id: &str,
    scenario_id: &str,
    host_version: Option<&str>,
) -> Result<Imported> {
    load_manifest(
        path,
        read_manifest(path)?,
        session_id,
        scenario_id,
        host_version,
    )
}

/// Only this top-level read/parse may be retried while an observer publishes.
pub(crate) fn read_manifest(path: &Path) -> Result<Manifest> {
    let bytes = read_bounded(path, MAX_MANIFEST_BYTES)?;
    Ok(serde_json::from_slice(&bytes)?)
}

/// Consume the exact parsed publication without rereading its manifest path.
pub(crate) fn load_manifest(
    path: &Path,
    manifest: Manifest,
    session_id: &str,
    scenario_id: &str,
    host_version: Option<&str>,
) -> Result<Imported> {
    ensure!(
        manifest.schema_version == "1.0.0"
            && manifest.session_id == session_id
            && manifest.scenario_id == scenario_id
            && Some(manifest.host_version.as_str()) == host_version
            && manifest.test_client_version == env!("CARGO_PKG_VERSION"),
        "reference observation schema/session/scenario/version mismatch"
    );
    validate_id(&manifest.session_id)?;
    validate_id(&manifest.scenario_id)?;
    for value in [
        &manifest.host_version,
        &manifest.test_client_revision,
        &manifest.official_client_version,
    ] {
        validate_reason(value)?;
    }
    ensure!(
        manifest.artifacts.len() <= 32 && manifest.observations.len() <= 6,
        "reference evidence capacity exceeded"
    );
    let root = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."))
        .canonicalize()?;
    let mut artifacts = Vec::new();
    let mut names = BTreeSet::new();
    let mut total = 0usize;
    for artifact in &manifest.artifacts {
        artifact.path.validate()?;
        ensure!(
            names.insert(artifact.path.0.clone()),
            "duplicate reference artifact"
        );
        ensure!(
            artifact.sha256.len() == 64 && artifact.sha256.bytes().all(|b| b.is_ascii_hexdigit()),
            "invalid artifact checksum"
        );
        // Reject symlinks in every component, even if they currently resolve inside the root.
        let mut source = root.clone();
        for component in artifact.path.0.components() {
            source.push(component);
            ensure!(
                !std::fs::symlink_metadata(&source)?.file_type().is_symlink(),
                "reference artifact symlink"
            );
        }
        let resolved = artifact.path.resolve(&root)?;
        let data = read_bounded(&resolved, MAX_ARTIFACT_BYTES)?;
        total += data.len();
        ensure!(
            total <= MAX_TOTAL_BYTES,
            "reference artifacts exceed total byte bound"
        );
        ensure!(
            hex::encode(Sha256::digest(&data)).eq_ignore_ascii_case(&artifact.sha256),
            "reference artifact checksum mismatch"
        );
        artifacts.push((artifact.path.clone(), data));
    }
    let mut checks = BTreeSet::new();
    for observation in &manifest.observations {
        ensure!(
            checks.insert(observation.check),
            "duplicate reference check"
        );
        crate::evidence::validate_refs(&observation.artifacts)?;
        ensure!(
            !observation.artifacts.is_empty()
                && observation.artifacts.iter().all(|r| names.contains(&r.0)),
            "reference check lacks bound source artifacts"
        );
        if observation.validity == Validity::Valid {
            ensure!(
                observation.satisfied.is_some() && observation.reason.is_none(),
                "invalid observed check"
            );
        } else {
            ensure!(
                matches!(
                    observation.validity,
                    Validity::Invalid | Validity::Unavailable
                ) && observation.satisfied.is_none(),
                "unavailable check must have absent value"
            );
            validate_reason(observation.reason.as_deref().unwrap_or(""))?;
        }
    }
    Ok(Imported {
        manifest,
        artifacts,
    })
}
