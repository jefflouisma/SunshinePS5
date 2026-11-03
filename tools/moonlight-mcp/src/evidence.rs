//! Typed, local evidence. Validation never promotes transport success into console delivery.

use anyhow::{bail, ensure, Result};
use serde::{Deserialize, Serialize};
use std::path::{Component, Path, PathBuf};

pub const SCHEMA_VERSION: &str = "1.0.0";
pub const MAX_REFERENCES: usize = 128;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(transparent)]
pub struct ArtifactRef(pub PathBuf);

impl ArtifactRef {
    pub fn validate(&self) -> Result<()> {
        let text = self
            .0
            .to_str()
            .ok_or_else(|| anyhow::anyhow!("non-UTF8 artifact path"))?;
        ensure!(
            !text.is_empty() && text.len() <= 1024,
            "artifact path length"
        );
        ensure!(
            self.0
                .components()
                .all(|c| matches!(c, Component::Normal(_))),
            "artifact path must be relative without traversal"
        );
        ensure!(
            text.bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"/_.-".contains(&b)),
            "invalid artifact path"
        );
        Ok(())
    }

    /// Resolve existing files without allowing symlinks to escape the run directory.
    pub fn resolve(&self, root: &Path) -> Result<PathBuf> {
        self.validate()?;
        let root = root.canonicalize()?;
        let path = root.join(&self.0).canonicalize()?;
        ensure!(path.starts_with(root), "artifact escapes run directory");
        Ok(path)
    }
}

pub fn validate_refs(refs: &[ArtifactRef]) -> Result<()> {
    ensure!(refs.len() <= MAX_REFERENCES, "too many artifact references");
    for (index, item) in refs.iter().enumerate() {
        item.validate()?;
        ensure!(
            !refs[..index].contains(item),
            "duplicate artifact reference"
        );
    }
    Ok(())
}

pub fn validate_id(id: &str) -> Result<()> {
    ensure!(
        !id.is_empty()
            && id.len() <= 128
            && id.as_bytes()[0].is_ascii_alphanumeric()
            && id
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b)),
        "invalid identifier"
    );
    Ok(())
}

pub fn validate_reason(reason: &str) -> Result<()> {
    ensure!(
        !reason.trim().is_empty() && reason.len() <= 1024,
        "invalid reason"
    );
    Ok(())
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    Capture,
    Encode,
    Packetize,
    Send,
    FullPipeline,
    ProtocolLatency,
    InputDelivery,
    ClientSent,
    TransportAck,
    HostReceived,
    ConsoleDelivered,
    ClientReceipt,
    HostDrops,
    ClientLoss,
    AudioLoss,
    AudioNonSilence,
    Bitrate,
    FrameInterval,
    FidelitySsim,
    FidelityPsnr,
    HdmiRelativeDelay,
    HdrMetadata,
    OperatorObservation,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Source {
    HostPipeline,
    ProtocolHeader,
    ClientReceipt,
    ReferenceRecording,
    OperatorObservation,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Provenance {
    SyntheticFixture,
    ConsoleHardware,
    ReferenceHost,
    OperatorObservation,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Validity {
    Valid,
    Bounded,
    Unavailable,
    Invalid,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Units {
    Microseconds,
    #[serde(rename = "0.1_milliseconds")]
    ProtocolTicks,
    Milliseconds,
    Seconds,
    Frames,
    FramesPerSecond,
    Packets,
    Events,
    BitsPerSecond,
    Ratio,
    #[serde(rename = "dB")]
    Decibels,
    Boolean,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Interval {
    pub start: u64,
    pub end: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Clock {
    pub id: String,
    #[serde(rename = "type")]
    pub clock_type: String,
    pub epoch: Option<String>,
    pub native_resolution_ns: Option<f64>,
    pub conversion: Option<String>,
    pub quantization_bound_ns: Option<f64>,
    pub uncertainty_ns: Option<f64>,
    pub uncertainty_reason: Option<String>,
    pub validity: Validity,
    pub reason: Option<String>,
}

impl Clock {
    pub fn validate(&self) -> Result<()> {
        validate_id(&self.id)?;
        ensure!(
            self.clock_type == "monotonic",
            "elapsed measurements require monotonic clocks"
        );
        ensure!(
            self.native_resolution_ns
                .is_none_or(|v| v.is_finite() && v > 0.0),
            "invalid clock resolution"
        );
        for value in [self.quantization_bound_ns, self.uncertainty_ns]
            .into_iter()
            .flatten()
        {
            ensure!(
                value.is_finite() && value >= 0.0,
                "invalid clock uncertainty"
            );
        }
        if self.uncertainty_ns.is_none() {
            validate_reason(self.uncertainty_reason.as_deref().unwrap_or(""))?;
        }
        if self.validity == Validity::Valid {
            validate_id(self.epoch.as_deref().unwrap_or(""))?;
            validate_reason(self.conversion.as_deref().unwrap_or(""))?;
            ensure!(
                self.native_resolution_ns.is_some()
                    && self.quantization_bound_ns.is_some()
                    && self.reason.is_none(),
                "valid clock needs resolution and conversion uncertainty"
            );
        } else {
            ensure!(
                self.validity != Validity::Bounded,
                "clock validity cannot be bounded"
            );
            validate_reason(self.reason.as_deref().unwrap_or(""))?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Population {
    pub expected: Option<u64>,
    pub observed: Option<u64>,
    pub represented: Option<u64>,
    pub excluded: Option<u64>,
    pub missing: Option<u64>,
    pub overflow: Option<u64>,
    pub failed_delivery: Option<u64>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum CoverageState {
    Complete,
    Partial,
    Disabled,
    Unavailable,
    Invalid,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Gap {
    pub start: Option<u64>,
    pub end: Option<u64>,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Coverage {
    pub state: CoverageState,
    pub enabled_intervals: Vec<Interval>,
    pub gaps: Vec<Gap>,
    pub reason: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Measurement {
    pub schema_version: String,
    pub id: String,
    pub session_id: String,
    pub provenance: Provenance,
    pub kind: Kind,
    pub source: Source,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frame_id: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub event_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub controller_id: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub authenticated: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub delivery_boundary: Option<String>,
    pub interval: Option<Interval>,
    pub clock_domain: Option<String>,
    pub source_clock: Option<Clock>,
    pub units: Units,
    pub resolution: Option<f64>,
    pub uncertainty: Option<f64>,
    pub uncertainty_reason: Option<String>,
    pub population: Population,
    pub coverage: Coverage,
    pub value: Option<f64>,
    pub lower_bound: Option<f64>,
    pub upper_bound: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub raw_protocol_latency: Option<u16>,
    pub validity: Validity,
    pub reason: Option<String>,
    pub artifact_refs: Vec<ArtifactRef>,
}

impl Measurement {
    /// Unknown observations remain null; zero is reserved for an actually observed zero.
    pub fn unavailable(id: &str, session_id: &str, reason: &str) -> Self {
        Self {
            schema_version: SCHEMA_VERSION.into(),
            id: id.into(),
            session_id: session_id.into(),
            provenance: Provenance::OperatorObservation,
            kind: Kind::ClientReceipt,
            source: Source::ClientReceipt,
            frame_id: None,
            event_id: None,
            controller_id: None,
            authenticated: None,
            delivery_boundary: None,
            interval: None,
            clock_domain: None,
            source_clock: None,
            units: Units::Events,
            resolution: None,
            uncertainty: None,
            uncertainty_reason: Some(reason.into()),
            population: Population {
                expected: None,
                observed: None,
                represented: None,
                excluded: None,
                missing: None,
                overflow: None,
                failed_delivery: None,
            },
            coverage: Coverage {
                state: CoverageState::Unavailable,
                enabled_intervals: Vec::new(),
                gaps: Vec::new(),
                reason: Some(reason.into()),
            },
            value: None,
            lower_bound: None,
            upper_bound: None,
            raw_protocol_latency: None,
            validity: Validity::Unavailable,
            reason: Some(reason.into()),
            artifact_refs: Vec::new(),
        }
    }

    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.schema_version == SCHEMA_VERSION,
            "unsupported evidence schema"
        );
        validate_id(&self.id)?;
        validate_id(&self.session_id)?;
        validate_refs(&self.artifact_refs)?;
        for value in [
            self.value,
            self.lower_bound,
            self.upper_bound,
            self.resolution,
            self.uncertainty,
        ]
        .into_iter()
        .flatten()
        {
            ensure!(value.is_finite(), "nonfinite measurement");
        }
        ensure!(
            self.resolution.is_none_or(|v| v > 0.0) && self.uncertainty.is_none_or(|v| v >= 0.0),
            "invalid measurement precision"
        );
        if self.uncertainty.is_none() {
            validate_reason(self.uncertainty_reason.as_deref().unwrap_or(""))?;
        }
        if let Some(interval) = &self.interval {
            ensure!(interval.end >= interval.start, "reversed interval");
        }
        if let Some(domain) = &self.clock_domain {
            validate_id(domain)?;
        }
        if let Some(clock) = &self.source_clock {
            ensure!(
                Some(&clock.id) == self.clock_domain.as_ref(),
                "clock domain mismatch"
            );
            clock.validate()?;
        }
        let p = &self.population;
        if let (Some(represented), Some(excluded), Some(overflow), Some(observed)) =
            (p.represented, p.excluded, p.overflow, p.observed)
        {
            ensure!(
                represented
                    .checked_add(excluded)
                    .and_then(|v| v.checked_add(overflow))
                    == Some(observed),
                "population accounting mismatch"
            );
        }
        if let (Some(failed), Some(observed)) = (p.failed_delivery, p.observed) {
            ensure!(failed <= observed, "delivery count exceeds observations");
        }
        if let (Some(expected), Some(observed), Some(missing)) = (p.expected, p.observed, p.missing)
        {
            ensure!(
                observed.checked_add(missing) == Some(expected),
                "expected population mismatch"
            );
        }
        ensure!(
            self.coverage.enabled_intervals.len() <= 128 && self.coverage.gaps.len() <= 128,
            "coverage capacity exceeded"
        );
        for interval in &self.coverage.enabled_intervals {
            ensure!(interval.end >= interval.start, "reversed coverage");
        }
        for gap in &self.coverage.gaps {
            validate_reason(&gap.reason)?;
            ensure!(
                gap.start.zip(gap.end).is_none_or(|(a, b)| b >= a),
                "reversed gap"
            );
        }
        if self.coverage.state == CoverageState::Complete {
            ensure!(
                !self.coverage.enabled_intervals.is_empty()
                    && self.coverage.gaps.is_empty()
                    && self.coverage.reason.is_none(),
                "complete coverage requires intervals without gaps"
            );
        } else {
            validate_reason(self.coverage.reason.as_deref().unwrap_or(""))?;
        }
        if let (Some(lo), Some(hi)) = (self.lower_bound, self.upper_bound) {
            ensure!(lo <= hi, "reversed bounds");
        }
        match self.validity {
            Validity::Valid => {
                ensure!(
                    self.value.is_some()
                        && self.lower_bound.is_none()
                        && self.upper_bound.is_none()
                        && self.reason.is_none(),
                    "exact evidence requires value without bounds"
                );
                ensure!(
                    self.interval.is_some()
                        && self.resolution.is_some()
                        && self
                            .source_clock
                            .as_ref()
                            .is_some_and(|c| c.validity == Validity::Valid
                                && c.native_resolution_ns
                                    .is_some_and(|v| v.is_finite() && v > 0.0)),
                    "valid evidence requires a valid clock and interval"
                );
                ensure!(
                    p.observed.is_some_and(|v| v > 0)
                        && p.missing == Some(0)
                        && p.expected.is_some_and(|v| v > 0)
                        && self.coverage.state == CoverageState::Complete
                        && self.coverage.gaps.is_empty(),
                    "valid evidence needs complete population"
                );
            }
            _ => {
                ensure!(
                    self.value.is_none(),
                    "missing or bounded evidence cannot have exact value"
                );
                validate_reason(self.reason.as_deref().unwrap_or(""))?;
                if self.validity == Validity::Bounded {
                    ensure!(
                        self.lower_bound.is_some() || self.upper_bound.is_some(),
                        "bounded evidence needs a bound"
                    );
                }
            }
        }
        let expected_source = match self.kind {
            Kind::Capture
            | Kind::Encode
            | Kind::Packetize
            | Kind::Send
            | Kind::FullPipeline
            | Kind::HostDrops
            | Kind::HostReceived
            | Kind::ConsoleDelivered
            | Kind::InputDelivery => Some(Source::HostPipeline),
            Kind::ClientSent
            | Kind::TransportAck
            | Kind::ClientReceipt
            | Kind::ClientLoss
            | Kind::AudioLoss
            | Kind::AudioNonSilence => Some(Source::ClientReceipt),
            Kind::FidelitySsim | Kind::FidelityPsnr | Kind::HdmiRelativeDelay => {
                Some(Source::ReferenceRecording)
            }
            Kind::ProtocolLatency => Some(Source::ProtocolHeader),
            Kind::OperatorObservation => Some(Source::OperatorObservation),
            _ => None,
        };
        ensure!(
            expected_source.is_none_or(|s| s == self.source),
            "evidence source does not establish this boundary"
        );
        if matches!(
            self.kind,
            Kind::Capture
                | Kind::Encode
                | Kind::Packetize
                | Kind::Send
                | Kind::FullPipeline
                | Kind::ProtocolLatency
                | Kind::FrameInterval
                | Kind::FidelitySsim
                | Kind::FidelityPsnr
        ) {
            ensure!(self.frame_id.is_some(), "frame identifier required");
        }
        if matches!(
            self.kind,
            Kind::ClientSent
                | Kind::TransportAck
                | Kind::HostReceived
                | Kind::ConsoleDelivered
                | Kind::InputDelivery
        ) {
            validate_id(self.event_id.as_deref().unwrap_or(""))?;
            ensure!(
                self.controller_id.is_some_and(|id| id < 4) && self.frame_id.is_none(),
                "controller-only event required"
            );
        }
        if self.validity == Validity::Valid
            && matches!(self.kind, Kind::InputDelivery | Kind::ConsoleDelivered)
        {
            ensure!(self.authenticated == Some(true), "unauthenticated delivery");
            validate_reason(self.delivery_boundary.as_deref().unwrap_or(""))?;
        }
        if matches!(
            self.kind,
            Kind::Capture
                | Kind::Encode
                | Kind::Packetize
                | Kind::Send
                | Kind::FullPipeline
                | Kind::InputDelivery
        ) {
            ensure!(
                self.units == Units::Microseconds
                    && [self.value, self.lower_bound, self.upper_bound]
                        .into_iter()
                        .flatten()
                        .all(|v| v >= 0.0),
                "invalid duration units or value"
            );
        }
        if self.kind == Kind::ProtocolLatency {
            ensure!(self.units == Units::ProtocolTicks, "protocol latency units");
            match self.raw_protocol_latency {
                Some(0) => ensure!(
                    self.validity == Validity::Unavailable,
                    "zero protocol field means absent"
                ),
                Some(u16::MAX) => ensure!(
                    self.validity == Validity::Bounded
                        && self.lower_bound == Some(f64::from(u16::MAX)),
                    "saturated protocol field is only a bound"
                ),
                Some(raw) => ensure!(
                    self.validity != Validity::Valid || self.value == Some(f64::from(raw)),
                    "protocol field value mismatch"
                ),
                None => bail!("missing protocol field"),
            }
        } else {
            ensure!(
                self.raw_protocol_latency.is_none(),
                "protocol field on unrelated evidence"
            );
        }
        Ok(())
    }
}

/// Subtract only observations of the same event at documented host boundaries.
pub fn input_delivery_duration(received: &Measurement, delivered: &Measurement) -> Result<u64> {
    received.validate()?;
    delivered.validate()?;
    ensure!(
        received.kind == Kind::HostReceived && delivered.kind == Kind::ConsoleDelivered,
        "ACK is not console delivery"
    );
    ensure!(
        received.validity == Validity::Valid && delivered.validity == Validity::Valid,
        "unavailable delivery observations"
    );
    ensure!(
        received.session_id == delivered.session_id
            && received.event_id == delivered.event_id
            && received.controller_id == delivered.controller_id
            && received.provenance == delivered.provenance,
        "uncorrelated input event"
    );
    ensure!(
        received.source_clock == delivered.source_clock
            && received.clock_domain.is_some()
            && received.clock_domain == delivered.clock_domain,
        "unrelated clocks"
    );
    let start = received
        .interval
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("missing receipt timestamp"))?
        .end;
    let end = delivered
        .interval
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("missing delivery timestamp"))?
        .end;
    end.checked_sub(start)
        .ok_or_else(|| anyhow::anyhow!("reversed input timestamps"))
}
