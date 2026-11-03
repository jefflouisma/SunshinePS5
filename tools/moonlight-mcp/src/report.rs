//! Bounded report records. Per-frame/event data lives in local streamed artifacts.

use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::io::Write;

use crate::evidence::{
    validate_id, validate_reason, validate_refs, ArtifactRef, Clock, Measurement, SCHEMA_VERSION,
};

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum Status {
    Pass,
    Fail,
    Incomplete,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum DynamicRange {
    Sdr,
    Hdr,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Codec {
    H264,
    Hevc,
    Av1,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Mode {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate_kbps: u32,
    pub codec: Codec,
    pub dynamic_range: DynamicRange,
}

impl Mode {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.width > 0 && self.height > 0 && self.fps > 0 && self.bitrate_kbps > 0,
            "zero mode parameter"
        );
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum BitrateAdjustmentReason {
    None,
    RaisedToMinimum,
    LoweredToMaximum,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct BitrateAdjustment {
    pub requested_kbps: u32,
    pub chosen_kbps: u32,
    pub minimum_kbps: u32,
    pub maximum_kbps: u32,
    pub reason: BitrateAdjustmentReason,
}

impl BitrateAdjustment {
    /// Bounds are supplied by validated capabilities, never guessed by the report.
    pub fn new(requested_kbps: u32, minimum_kbps: u32, maximum_kbps: u32) -> Result<Self> {
        ensure!(
            requested_kbps > 0 && minimum_kbps > 0 && minimum_kbps <= maximum_kbps,
            "invalid bitrate or bounds"
        );
        let chosen_kbps = requested_kbps.clamp(minimum_kbps, maximum_kbps);
        let reason = if requested_kbps < minimum_kbps {
            BitrateAdjustmentReason::RaisedToMinimum
        } else if requested_kbps > maximum_kbps {
            BitrateAdjustmentReason::LoweredToMaximum
        } else {
            BitrateAdjustmentReason::None
        };
        Ok(Self {
            requested_kbps,
            chosen_kbps,
            minimum_kbps,
            maximum_kbps,
            reason,
        })
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Environment {
    pub firmware: Option<String>,
    pub host_version: Option<String>,
    pub client_version: String,
    pub network: String,
    pub devices: Vec<String>,
    pub reference_chain: Option<String>,
    pub clocks: Vec<Clock>,
    pub calibration_ids: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Video {
    pub frame_rate: Measurement,
    pub frame_intervals: Measurement,
    pub protocol_processing_latency: Measurement,
    pub full_host_pipeline: Measurement,
    pub missing_frames: Option<u64>,
    pub incomplete_frames: Option<u64>,
    pub recoverable_with_fec: Option<u64>,
    pub host_drops: Option<u64>,
    pub measured_bitrate: Measurement,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Audio {
    pub packet_loss: Measurement,
    pub non_silent: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Input {
    pub client_sent: Vec<ArtifactRef>,
    pub transport_acknowledged: Vec<ArtifactRef>,
    pub host_received: Vec<ArtifactRef>,
    pub console_delivered: Vec<ArtifactRef>,
    pub delivery_latency: Measurement,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Fidelity {
    pub per_frame_scores: Vec<ArtifactRef>,
    pub sample_pairs: Vec<ArtifactRef>,
    pub ssim: Option<Measurement>,
    pub psnr: Option<Measurement>,
    pub relative_delay: Measurement,
    pub alignment_reliable: bool,
    pub compared_frames: u64,
    pub excluded_frames: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Criterion {
    pub id: String,
    pub threshold: Option<Value>,
    pub evidence_ids: Vec<String>,
    pub status: Status,
    pub reason: String,
}

/// The declaration is retained verbatim and exposed read-only. Parsing belongs to T019.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Report {
    pub schema_version: String,
    pub run_id: String,
    pub session_id: String,
    scenario: Value,
    pub environment: Environment,
    pub requested: Mode,
    pub chosen: Option<Mode>,
    pub bitrate_adjustment: Option<BitrateAdjustment>,
    pub video: Video,
    pub audio: Audio,
    pub input: Input,
    pub hdr_events: Vec<ArtifactRef>,
    pub fidelity: Fidelity,
    pub criteria: Vec<Criterion>,
    pub overall_status: Status,
    pub limitations: Vec<String>,
    pub artifact_refs: Vec<ArtifactRef>,
}

impl Report {
    pub fn scenario(&self) -> &Value {
        &self.scenario
    }

    /// Start a valid declared run with explicit absent observations and every required result.
    pub fn new(
        declared: Value,
        environment: Environment,
        run_id: String,
        session_id: String,
    ) -> Result<Self> {
        Self::new_ref(&declared, environment, run_id, session_id)
    }

    /// Large controller workloads remain in a streamed local declaration artifact, with an
    /// immutable digest and bounded inline summary. The report never clones bulk action data.
    pub fn new_ref(
        declared: &Value,
        environment: Environment,
        run_id: String,
        session_id: String,
    ) -> Result<Self> {
        let declared = bounded_declaration(declared)?;
        let absent =
            |id| Measurement::unavailable(id, &session_id, "required evidence unavailable");
        let mut report = Self {
            schema_version: SCHEMA_VERSION.into(),
            run_id,
            requested: serde_json::from_value(declared["requested"].clone())?,
            scenario: declared,
            environment,
            chosen: None,
            bitrate_adjustment: None,
            video: Video {
                frame_rate: absent("frame-rate"),
                frame_intervals: absent("frame-intervals"),
                protocol_processing_latency: absent("protocol-latency"),
                full_host_pipeline: absent("host-pipeline"),
                missing_frames: None,
                incomplete_frames: None,
                recoverable_with_fec: None,
                host_drops: None,
                measured_bitrate: absent("measured-bitrate"),
            },
            audio: Audio {
                packet_loss: absent("audio-loss"),
                non_silent: None,
            },
            input: Input {
                client_sent: Vec::new(),
                transport_acknowledged: Vec::new(),
                host_received: Vec::new(),
                console_delivered: Vec::new(),
                delivery_latency: absent("input-delivery"),
            },
            hdr_events: Vec::new(),
            fidelity: Fidelity {
                per_frame_scores: Vec::new(),
                sample_pairs: Vec::new(),
                ssim: None,
                psnr: None,
                relative_delay: absent("relative-delay"),
                alignment_reliable: false,
                compared_frames: 0,
                excluded_frames: 0,
            },
            session_id,
            criteria: Vec::new(),
            overall_status: Status::Incomplete,
            limitations: Vec::new(),
            artifact_refs: Vec::new(),
        };
        if report.external_declaration() {
            report
                .artifact_refs
                .push(ArtifactRef("scenario.json".into()));
        }
        report.finalize(None)?;
        Ok(report)
    }

    pub fn external_declaration(&self) -> bool {
        self.scenario.get("declaration").is_some()
    }

    /// Ingest bounded observations only after checking the original declaration.
    /// Missing criterion results are retained as Incomplete, including on interruption.
    pub fn from_observed(declared: &Value, observed: Value) -> Result<Self> {
        let mut report: Self = serde_json::from_value(observed)?;
        ensure!(
            report.scenario == bounded_declaration(declared)?,
            "run declaration changed"
        );
        report.finalize(None)?;
        Ok(report)
    }

    pub fn finalize(&mut self, interruption: Option<&str>) -> Result<()> {
        if let Some(reason) = interruption {
            validate_reason(reason)?;
            ensure!(self.limitations.len() < 128, "limitation capacity exceeded");
            self.limitations.push(reason.into());
        }
        let ids = self
            .scenario
            .pointer("/required_criteria/ids")
            .and_then(Value::as_array)
            .ok_or_else(|| anyhow::anyhow!("missing declared criteria"))?;
        ensure!(
            !ids.is_empty() && ids.len() <= 128 && self.criteria.len() <= 128,
            "criterion capacity"
        );
        let mut required = Vec::with_capacity(ids.len());
        for id in ids {
            let id = id
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("invalid criterion ID"))?;
            validate_id(id)?;
            ensure!(!required.contains(&id), "duplicate declared criterion");
            required.push(id);
        }
        for (index, criterion) in self.criteria.iter().enumerate() {
            ensure!(
                required.contains(&criterion.id.as_str())
                    && !self.criteria[..index].iter().any(|c| c.id == criterion.id),
                "unexpected or duplicate criterion"
            );
        }
        for id in required {
            if !self.criteria.iter().any(|c| c.id == id) {
                self.criteria.push(Criterion {
                    id: id.into(),
                    threshold: None,
                    evidence_ids: Vec::new(),
                    status: Status::Incomplete,
                    reason: "required evidence unavailable".into(),
                });
            }
        }
        self.overall_status = if self.criteria.iter().any(|c| c.status == Status::Incomplete) {
            Status::Incomplete
        } else if self.criteria.iter().any(|c| c.status == Status::Fail) {
            Status::Fail
        } else {
            Status::Pass
        };
        // An interrupted observation population cannot become a complete passing run.
        if interruption.is_some() {
            self.overall_status = Status::Incomplete;
        }
        self.validate()
    }

    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.schema_version == SCHEMA_VERSION,
            "unsupported report schema"
        );
        validate_id(&self.run_id)?;
        validate_id(&self.session_id)?;
        ensure!(
            serde_json::to_vec(&self.scenario)?.len() <= 1_048_576,
            "scenario metadata too large"
        );
        if let Some(declaration) = self.scenario.get("declaration") {
            ensure!(
                declaration["artifact_ref"] == "scenario.json"
                    && declaration["encoding"] == "serde_json_canonical_utf8"
                    && declaration["size_bytes"]
                        .as_u64()
                        .is_some_and(|n| n > 1_048_576 && n <= crate::scenario::MAX_SCENARIO_BYTES)
                    && declaration["sha256"]
                        .as_str()
                        .is_some_and(|s| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
                    && self
                        .artifact_refs
                        .contains(&ArtifactRef("scenario.json".into())),
                "invalid external declaration reference"
            );
        }
        self.requested.validate()?;
        ensure!(
            serde_json::from_value::<Mode>(self.scenario["requested"].clone())? == self.requested,
            "requested mode changed"
        );
        match (&self.chosen, &self.bitrate_adjustment) {
            (Some(mode), Some(adjustment)) => {
                mode.validate()?;
                ensure!(
                    *adjustment
                        == BitrateAdjustment::new(
                            self.requested.bitrate_kbps,
                            adjustment.minimum_kbps,
                            adjustment.maximum_kbps
                        )?
                        && mode.bitrate_kbps == adjustment.chosen_kbps,
                    "inconsistent bitrate adjustment"
                );
            }
            (None, None) => (),
            _ => anyhow::bail!("chosen mode and bitrate adjustment must be recorded together"),
        }
        for measurement in [
            &self.video.frame_rate,
            &self.video.frame_intervals,
            &self.video.protocol_processing_latency,
            &self.video.full_host_pipeline,
            &self.video.measured_bitrate,
            &self.audio.packet_loss,
            &self.input.delivery_latency,
            &self.fidelity.relative_delay,
        ]
        .into_iter()
        .chain(self.fidelity.ssim.iter())
        .chain(self.fidelity.psnr.iter())
        {
            measurement.validate()?;
            ensure!(
                measurement.session_id == self.session_id,
                "cross-session evidence"
            );
        }
        for refs in [
            &self.artifact_refs,
            &self.hdr_events,
            &self.input.client_sent,
            &self.input.transport_acknowledged,
            &self.input.host_received,
            &self.input.console_delivered,
            &self.fidelity.per_frame_scores,
            &self.fidelity.sample_pairs,
        ] {
            validate_refs(refs)?;
        }
        ensure!(
            self.limitations.len() <= 128
                && self.criteria.len() <= 128
                && self.environment.devices.len() <= 128
                && self.environment.clocks.len() <= 128
                && self.environment.calibration_ids.len() <= 128,
            "report metadata capacity exceeded"
        );
        for reason in &self.limitations {
            validate_reason(reason)?;
        }
        let required = self
            .scenario
            .pointer("/required_criteria/ids")
            .and_then(Value::as_array)
            .ok_or_else(|| anyhow::anyhow!("missing declared criteria"))?;
        ensure!(
            !required.is_empty() && required.len() == self.criteria.len(),
            "required criterion missing"
        );
        for (index, id) in required.iter().enumerate() {
            let id = id
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("invalid criterion ID"))?;
            ensure!(
                !required[..index]
                    .iter()
                    .any(|other| other.as_str() == Some(id))
                    && self.criteria.iter().filter(|c| c.id == id).count() == 1,
                "required criterion missing or duplicated"
            );
        }
        let expected_status = if self.criteria.iter().any(|c| c.status == Status::Incomplete) {
            Status::Incomplete
        } else if self.criteria.iter().any(|c| c.status == Status::Fail) {
            Status::Fail
        } else {
            Status::Pass
        };
        ensure!(
            self.overall_status == expected_status
                || (self.overall_status == Status::Incomplete && !self.limitations.is_empty()),
            "overall status contradicts required evidence"
        );
        for criterion in &self.criteria {
            validate_id(&criterion.id)?;
            validate_reason(&criterion.reason)?;
            ensure!(
                criterion.evidence_ids.len() <= 128
                    && serde_json::to_vec(&criterion.threshold)?.len() <= 4096,
                "criterion metadata capacity"
            );
            for id in &criterion.evidence_ids {
                validate_id(id)?;
            }
            ensure!(
                criterion.status != Status::Pass || !criterion.evidence_ids.is_empty(),
                "Pass requires evidence references"
            );
        }
        ensure!(
            !self.fidelity.alignment_reliable
                || self.fidelity.relative_delay.validity == crate::evidence::Validity::Valid,
            "reliable alignment needs valid delay"
        );
        ensure!(
            serde_json::to_vec(&self.environment)?.len() <= 65_536,
            "environment metadata capacity"
        );
        Ok(())
    }

    /// Serialization writes incrementally; frame/event payloads are never loaded here.
    pub fn write_to(&self, mut writer: impl Write) -> Result<()> {
        self.validate()?;
        serde_json::to_writer(&mut writer, self)?;
        writer.flush()?;
        Ok(())
    }
}

struct DeclarationDigest {
    hasher: Sha256,
    bytes: u64,
}
impl Write for DeclarationDigest {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.bytes = self
            .bytes
            .checked_add(bytes.len() as u64)
            .ok_or_else(|| std::io::Error::other("declaration length overflow"))?;
        if self.bytes > crate::scenario::MAX_SCENARIO_BYTES {
            return Err(std::io::Error::other("declaration exceeds scenario bound"));
        }
        self.hasher.update(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn bounded_declaration(declared: &Value) -> Result<Value> {
    let mut digest = DeclarationDigest {
        hasher: Sha256::new(),
        bytes: 0,
    };
    serde_json::to_writer(&mut digest, declared)?;
    if digest.bytes <= 1_048_576 {
        return Ok(declared.clone());
    }
    let mut summary = serde_json::Map::new();
    for (key, value) in declared
        .as_object()
        .ok_or_else(|| anyhow::anyhow!("declaration must be an object"))?
    {
        if key == "controllers" {
            let controllers = value
                .as_array()
                .ok_or_else(|| anyhow::anyhow!("invalid controllers"))?;
            summary.insert(key.clone(), Value::Array(controllers.iter().map(|controller| serde_json::json!({
                "id":controller["id"],"action_count":controller["actions"].as_array().map_or(0,Vec::len)
            })).collect()));
        } else {
            summary.insert(key.clone(), value.clone());
        }
    }
    summary.insert("declaration".into(),serde_json::json!({"artifact_ref":"scenario.json",
        "encoding":"serde_json_canonical_utf8","size_bytes":digest.bytes,"sha256":hex::encode(digest.hasher.finalize())}));
    let summary = Value::Object(summary);
    ensure!(
        serde_json::to_vec(&summary)?.len() <= 1_048_576,
        "non-controller declaration metadata too large"
    );
    Ok(summary)
}
