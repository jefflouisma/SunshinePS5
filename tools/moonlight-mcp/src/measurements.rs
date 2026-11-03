//! Separate observation boundaries, bounded host import, and exact duration populations.

use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::io::{BufRead, Read};

use crate::evidence::{
    self, Clock, Coverage, CoverageState, Measurement, Population, Provenance, Source, Validity,
};

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct Percentile {
    pub value_us: Option<u64>,
    pub lower_bound_us: Option<u64>,
    pub observed: u64,
    pub missing: u64,
    pub overflow: u64,
    pub reason: Option<String>,
}

/// Fixed integer bins retain exact recorded microseconds, including overflow in ranks.
pub struct DurationPopulation {
    bins: Box<[u64]>,
    observed: u64,
    missing: u64,
    overflow: u64,
    faulted: bool,
}
impl Default for DurationPopulation {
    fn default() -> Self {
        Self::new()
    }
}
impl DurationPopulation {
    pub fn new() -> Self {
        Self {
            bins: vec![0; 65_536].into_boxed_slice(),
            observed: 0,
            missing: 0,
            overflow: 0,
            faulted: false,
        }
    }
    pub fn record(&mut self, us: u64) -> Result<()> {
        let count = if us < 65_536 {
            &mut self.bins[us as usize]
        } else {
            &mut self.overflow
        };
        let (Some(next), Some(observed)) = (count.checked_add(1), self.observed.checked_add(1))
        else {
            self.faulted = true;
            anyhow::bail!("histogram/population counter overflow");
        };
        self.observed = observed;
        *count = next;
        Ok(())
    }
    pub fn mark_missing(&mut self, count: u64) -> Result<()> {
        let Some(missing) = self.missing.checked_add(count) else {
            self.faulted = true;
            anyhow::bail!("missing counter overflow");
        };
        self.missing = missing;
        Ok(())
    }
    pub fn percentile(&self, percent: u8) -> Result<Percentile> {
        ensure!((1..=100).contains(&percent), "percentile must be 1..100");
        let mut result = Percentile {
            value_us: None,
            lower_bound_us: None,
            observed: self.observed,
            missing: self.missing,
            overflow: self.overflow,
            reason: None,
        };
        if self.faulted || self.missing > 0 || self.observed == 0 {
            result.reason =
                Some("empty, incomplete, or counter-overflowed observation population".into());
            return Ok(result);
        }
        let rank = (u128::from(self.observed) * u128::from(percent)).div_ceil(100);
        let mut cumulative = 0u128;
        for (value, count) in self.bins.iter().enumerate() {
            cumulative += u128::from(*count);
            if cumulative >= rank {
                result.value_us = Some(value as u64);
                return Ok(result);
            }
        }
        result.lower_bound_us = Some(65_536);
        result.reason = Some("selected rank is outside exact histogram range".into());
        Ok(result)
    }
}

/// Bytes mean received wire bytes in the explicitly supplied observation interval, never targets.
pub struct ReceiptMeasurements {
    pub intervals: DurationPopulation,
    domain: String,
    previous: Option<(u64, u64)>,
    first_us: Option<u64>,
    bytes: u64,
    capacity: u64,
    observed: u64,
    valid: bool,
}
impl ReceiptMeasurements {
    pub fn new(domain: &str, max_frames: u64) -> Result<Self> {
        evidence::validate_id(domain)?;
        ensure!(max_frames > 0, "empty receipt capacity");
        Ok(Self {
            intervals: DurationPopulation::new(),
            domain: domain.into(),
            previous: None,
            first_us: None,
            bytes: 0,
            capacity: max_frames,
            observed: 0,
            valid: true,
        })
    }
    pub fn observe(
        &mut self,
        domain: &str,
        frame_id: u64,
        received_us: u64,
        bytes: u64,
    ) -> Result<()> {
        let valid = domain == self.domain
            && self.observed < self.capacity
            && self
                .previous
                .is_none_or(|(id, time)| frame_id > id && received_us > time);
        if !valid {
            self.valid = false;
            self.intervals.mark_missing(1)?;
            anyhow::bail!("receipt clock/order/capacity fault");
        }
        let Some(total) = self.bytes.checked_add(bytes) else {
            self.valid = false;
            self.intervals.mark_missing(1)?;
            anyhow::bail!("received byte counter overflow");
        };
        if let Some((id, previous)) = self.previous {
            if frame_id - id > 1 {
                self.intervals.mark_missing(frame_id - id - 1)?;
            }
            self.intervals.record(received_us - previous)?;
        }
        self.first_us.get_or_insert(received_us);
        self.previous = Some((frame_id, received_us));
        self.bytes = total;
        self.observed += 1;
        Ok(())
    }
    pub fn measured_bitrate(&self, start_us: u64, end_us: u64) -> Result<f64> {
        ensure!(
            self.valid
                && end_us > start_us
                && self.first_us.is_some_and(|v| v >= start_us)
                && self.previous.is_some_and(|(_, v)| v <= end_us),
            "invalid bitrate observation window"
        );
        Ok(self.bytes as f64 * 8_000_000.0 / (end_us - start_us) as f64)
    }
}

/// Optional version 1.1 portable metrics metadata. Old 1.0 records remain valid.
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetrics {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub context: Option<HostMetricsContext>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub summaries: Option<Vec<HostMetricsSummary>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub retained: Option<HostMetricsRetention>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub continuous: Option<HostMetricsContinuous>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub allocated_bytes: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub raw: Option<HostMetricsRaw>,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsContext {
    pub host_version: Option<String>,
    pub firmware_version: Option<String>,
    pub client_name: Option<String>,
    pub codec: Option<String>,
    pub delivery_boundary: Option<String>,
    pub width: Option<u64>,
    pub height: Option<u64>,
    pub fps_numerator: Option<u64>,
    pub fps_denominator: Option<u64>,
    pub requested_bitrate_kbps: Option<u64>,
    pub chosen_bitrate_kbps: Option<u64>,
    pub dynamic_range: String,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsQuantile {
    pub value_us: Option<u64>,
    pub lower_bound_us: Option<u64>,
    pub population: u64,
    pub validity: Validity,
    pub reason: Option<String>,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsPopulation {
    pub seen: u64,
    pub represented: u64,
    pub overflow: u64,
    pub missing: u64,
    pub failed_deliveries: u64,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsSummary {
    pub channel: String,
    pub window: String,
    pub population: HostMetricsPopulation,
    pub median: HostMetricsQuantile,
    pub p95: HostMetricsQuantile,
    pub p99: HostMetricsQuantile,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsRetained {
    pub first_us: Option<u64>,
    pub last_us: Option<u64>,
    pub records: u64,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsRetention {
    pub frames: HostMetricsRetained,
    pub inputs: HostMetricsRetained,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsContinuous {
    pub frame_outcomes: [u64; 5],
    pub encoded_bytes: u64,
    pub network_bytes: u64,
    pub client_lost_packets: u64,
    pub client_recovery_requests: u64,
    pub start_us: u64,
    pub end_us: u64,
    pub encoded_bitrate_bps: Option<u64>,
    pub network_throughput_bps: Option<u64>,
    pub frame_rate_millihz: Option<u64>,
    pub validity: Validity,
    pub reason: Option<String>,
}
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostMetricsRaw {
    pub observed_us: u64,
    pub durations_us: Vec<Option<u64>>,
    pub flags: u64,
}
impl HostMetrics {
    fn validate(&self) -> Result<()> {
        ensure!(
            self.allocated_bytes.is_none_or(|n| n <= 83_886_080),
            "metrics memory ceiling"
        );
        if let Some(raw) = &self.raw {
            ensure!(
                (1..=5).contains(&raw.durations_us.len()),
                "raw duration capacity"
            );
        }
        if let Some(context) = &self.context {
            ensure!(
                matches!(
                    context.dynamic_range.as_str(),
                    "unknown" | "sdr" | "hdr_pq_bt2020"
                ),
                "unknown dynamic range"
            );
            for value in [
                &context.host_version,
                &context.firmware_version,
                &context.client_name,
                &context.codec,
                &context.delivery_boundary,
            ]
            .into_iter()
            .flatten()
            {
                ensure!(
                    !value.is_empty() && value.len() <= 256,
                    "metrics context capacity"
                );
            }
        }
        if let Some(summaries) = &self.summaries {
            ensure!(
                summaries.len() == 12,
                "six channels and two windows required"
            );
            let mut channels = BTreeSet::new();
            for summary in summaries {
                ensure!(
                    matches!(
                        summary.channel.as_str(),
                        "capture"
                            | "encode"
                            | "packetize"
                            | "send"
                            | "full_pipeline"
                            | "input_delivery"
                    ) && matches!(summary.window.as_str(), "whole_session" | "last_10_seconds"),
                    "unknown metrics channel/window"
                );
                ensure!(
                    channels.insert((&summary.channel, &summary.window)),
                    "duplicate metrics summary"
                );
                let p = &summary.population;
                ensure!(
                    p.represented
                        .checked_add(p.overflow)
                        .and_then(|n| n.checked_add(p.missing))
                        == Some(p.seen)
                        && p.failed_deliveries <= p.missing,
                    "metrics population arithmetic"
                );
                for q in [&summary.median, &summary.p95, &summary.p99] {
                    ensure!(
                        q.population
                            == p.represented
                                .checked_add(p.overflow)
                                .context("metrics population overflow")?,
                        "quantile population mismatch"
                    );
                    if q.validity == Validity::Valid {
                        ensure!(
                            q.value_us.is_some_and(|v| v < 65_536)
                                && q.lower_bound_us.is_none()
                                && q.reason.is_none()
                                && p.missing == 0
                                && q.population > 0,
                            "invalid exact quantile"
                        );
                    } else {
                        ensure!(
                            matches!(q.validity, Validity::Invalid | Validity::Unavailable)
                                && q.value_us.is_none(),
                            "missing quantile must be absent"
                        );
                        evidence::validate_reason(q.reason.as_deref().unwrap_or(""))?;
                    }
                }
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HostObservation {
    pub schema_version: String,
    pub id: String,
    pub session_id: String,
    pub provenance: Provenance,
    pub source: Source,
    pub kind: String,
    pub frame_id: Option<u64>,
    pub event_id: Option<String>,
    pub controller_id: Option<u8>,
    pub boundary: String,
    pub drop_reason: Option<String>,
    pub authenticated: Option<bool>,
    pub delivery_boundary: Option<String>,
    pub clock_domain: Option<String>,
    pub source_clock: Option<Clock>,
    pub observed_at: Option<u64>,
    pub population: Population,
    pub coverage: Coverage,
    pub validity: Validity,
    pub reason: Option<String>,
    pub artifact_refs: Vec<evidence::ArtifactRef>,
    pub measurements: Vec<Measurement>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub metrics: Option<HostMetrics>,
}
impl HostObservation {
    pub fn validate(&self, session_id: &str) -> Result<()> {
        ensure!(
            matches!(self.schema_version.as_str(), "1.0.0" | "1.1.0")
                && self.session_id == session_id,
            "host observation schema/session mismatch"
        );
        if let Some(metrics) = &self.metrics {
            metrics.validate()?;
        }
        evidence::validate_id(&self.id)?;
        evidence::validate_id(&self.session_id)?;
        ensure!(
            self.source == Source::HostPipeline && self.measurements.len() <= 128,
            "invalid host source or measurement capacity"
        );
        evidence::validate_refs(&self.artifact_refs)?;
        if self.validity == Validity::Valid {
            let clock = self.source_clock.as_ref().context("host clock missing")?;
            clock.validate()?;
            ensure!(
                clock.validity == Validity::Valid
                    && Some(&clock.id) == self.clock_domain.as_ref()
                    && self.observed_at.is_some(),
                "invalid host clock boundary"
            );
        } else {
            ensure!(
                matches!(self.validity, Validity::Invalid | Validity::Unavailable)
                    && self.observed_at.is_none(),
                "invalid unavailable host boundary"
            );
            evidence::validate_reason(self.reason.as_deref().unwrap_or(""))?;
        }
        match self.kind.as_str() {
            "frame" => {
                ensure!(
                    self.frame_id.is_some()
                        && self.event_id.is_none()
                        && self.controller_id.is_none()
                        && self.authenticated.is_none()
                        && self.delivery_boundary.is_none()
                        && matches!(
                            self.boundary.as_str(),
                            "captured"
                                | "encode_accepted"
                                | "encode_completed"
                                | "network_submitted"
                                | "host_dropped"
                        ),
                    "invalid frame boundary"
                );
            }
            "input" => {
                evidence::validate_id(self.event_id.as_deref().unwrap_or(""))?;
                ensure!(
                    self.controller_id.is_some_and(|id| id < 4)
                        && self.frame_id.is_none()
                        && self.authenticated.is_some()
                        && self.drop_reason.is_none()
                        && matches!(
                            self.boundary.as_str(),
                            "host_received" | "console_delivered" | "delivery_failed"
                        ),
                    "invalid input boundary"
                );
                if self.boundary == "console_delivered" {
                    ensure!(self.authenticated == Some(true), "unauthenticated delivery");
                    evidence::validate_reason(self.delivery_boundary.as_deref().unwrap_or(""))?;
                }
                if self.boundary == "delivery_failed" {
                    evidence::validate_reason(self.reason.as_deref().unwrap_or(""))?;
                }
            }
            "metrics_coverage" => ensure!(
                matches!(
                    self.boundary.as_str(),
                    "metrics_enabled" | "metrics_disabled" | "coverage_invalidated"
                ) && self.frame_id.is_none()
                    && self.event_id.is_none()
                    && self.controller_id.is_none()
                    && self.drop_reason.is_none()
                    && self.authenticated.is_none()
                    && self.delivery_boundary.is_none(),
                "invalid coverage boundary"
            ),
            _ => anyhow::bail!("unknown host observation kind"),
        }
        if self.boundary == "host_dropped" {
            evidence::validate_reason(self.drop_reason.as_deref().unwrap_or(""))?;
        } else {
            ensure!(
                self.drop_reason.is_none(),
                "drop reason on unrelated boundary"
            );
        }
        ensure!(
            self.coverage.enabled_intervals.len() <= 128 && self.coverage.gaps.len() <= 128,
            "host coverage capacity exceeded"
        );
        if self.coverage.state != CoverageState::Complete {
            evidence::validate_reason(self.coverage.reason.as_deref().unwrap_or(""))?;
        }
        let mut structure = Measurement::unavailable(
            &self.id,
            &self.session_id,
            "host envelope structural validation",
        );
        structure.population = self.population.clone();
        structure.coverage = self.coverage.clone();
        structure.validate()?;
        for measurement in &self.measurements {
            measurement.validate()?;
            ensure!(
                measurement.session_id == self.session_id
                    && measurement.provenance == self.provenance
                    && measurement.clock_domain == self.clock_domain
                    && measurement.source_clock == self.source_clock
                    && measurement.frame_id == self.frame_id
                    && measurement.event_id == self.event_id
                    && measurement.controller_id == self.controller_id,
                "host nested evidence correlation mismatch"
            );
        }
        Ok(())
    }
}

/// JSON-lines import bounds each record before allocating; imported provenance remains explicit.
pub fn import_host(
    reader: impl BufRead,
    session_id: &str,
    max_records: usize,
) -> Result<Vec<HostObservation>> {
    ensure!(
        max_records <= 1024,
        "in-memory import limited to 1024 records; use streaming import"
    );
    let mut records = Vec::new();
    import_host_stream(reader, session_id, max_records, |record| {
        records.push(record);
        Ok(())
    })?;
    Ok(records)
}

/// Consume validated records one at a time, without retaining whole-session raw observations.
pub fn import_host_stream(
    mut reader: impl BufRead,
    session_id: &str,
    max_records: usize,
    mut sink: impl FnMut(HostObservation) -> Result<()>,
) -> Result<usize> {
    ensure!(
        (1..=1_000_000).contains(&max_records),
        "host import capacity invalid"
    );
    let mut count = 0;
    let mut ids = BTreeSet::new();
    loop {
        let mut line = Vec::new();
        let length = std::io::Read::by_ref(&mut reader)
            .take(65_537)
            .read_until(b'\n', &mut line)?;
        if length == 0 {
            break;
        }
        ensure!(
            length <= 65_536 && count < max_records,
            "host import record/capacity exceeded"
        );
        let observation: HostObservation = serde_json::from_slice(&line)?;
        observation.validate(session_id)?;
        ensure!(
            ids.insert(observation.id.clone()),
            "duplicate host observation id"
        );
        sink(observation)?;
        count += 1;
    }
    Ok(count)
}

/// Use only documented host receipt/delivery endpoints of the same authenticated controller event.
pub fn correlate_input(
    client_sent: &Measurement,
    received: &Measurement,
    delivered: &Measurement,
) -> Result<u64> {
    client_sent.validate()?;
    ensure!(
        client_sent.kind == evidence::Kind::ClientSent
            && client_sent.validity == Validity::Valid
            && client_sent.session_id == received.session_id
            && client_sent.event_id == received.event_id
            && client_sent.controller_id == received.controller_id,
        "uncorrelated client input transmission"
    );
    evidence::input_delivery_duration(received, delivered)
}

/// Exact same-clock host endpoints; protocol/client/HDMI boundaries cannot substitute.
pub fn host_duration(
    start: &HostObservation,
    end: &HostObservation,
    kind: evidence::Kind,
) -> Result<u64> {
    start.validate(&start.session_id)?;
    end.validate(&start.session_id)?;
    let boundaries = match kind {
        evidence::Kind::FullPipeline => ("captured", "network_submitted"),
        evidence::Kind::Encode => ("encode_accepted", "encode_completed"),
        evidence::Kind::InputDelivery => ("host_received", "console_delivered"),
        _ => anyhow::bail!("unsupported or unrelated host timing boundary"),
    };
    ensure!(
        start.boundary == boundaries.0
            && end.boundary == boundaries.1
            && start.validity == Validity::Valid
            && end.validity == Validity::Valid
            && start.frame_id == end.frame_id
            && start.event_id == end.event_id
            && start.controller_id == end.controller_id
            && start.provenance == end.provenance
            && start.source_clock == end.source_clock
            && start.clock_domain == end.clock_domain
            && start.coverage.state == CoverageState::Complete
            && end.coverage.state == CoverageState::Complete,
        "uncorrelated/unavailable host boundaries"
    );
    if kind == evidence::Kind::InputDelivery {
        ensure!(
            start.authenticated == Some(true) && end.authenticated == Some(true),
            "unauthenticated input"
        );
    }
    end.observed_at
        .context("missing end")?
        .checked_sub(start.observed_at.context("missing start")?)
        .context("reversed host boundary timestamps")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn host(boundary: &str, at: u64) -> HostObservation {
        serde_json::from_value(serde_json::json!({
            "schema_version":"1.0.0", "id":boundary, "session_id":"session", "provenance":"synthetic_fixture",
            "source":"host_pipeline", "kind":"input", "event_id":"event", "controller_id":0,
            "boundary":boundary, "authenticated":true, "delivery_boundary":"fake documented delivery only",
            "clock_domain":"host-clock", "source_clock":{
                "id":"host-clock", "type":"monotonic", "epoch":"boot", "native_resolution_ns":1,
                "conversion":"floor(ns/1000)", "quantization_bound_ns":1000,
                "uncertainty_ns":null, "uncertainty_reason":"synthetic clock", "validity":"valid", "reason":null
            }, "observed_at":at,
            "population":{"expected":1,"observed":1,"represented":1,"excluded":0,"missing":0,"overflow":0,"failed_delivery":0},
            "coverage":{"state":"complete","enabled_intervals":[{"start":1,"end":10000}],"gaps":[],"reason":null},
            "validity":"valid","reason":null,"artifact_refs":[],"measurements":[]
        })).unwrap()
    }

    #[test]
    fn host_import_accepts_typed_metrics_and_preserves_legacy_validation() {
        let legacy = host("host_received", 100);
        legacy.validate("session").unwrap();
        let mut document = serde_json::to_value(legacy).unwrap();
        document["schema_version"] = serde_json::json!("1.1.0");
        document["metrics"] = serde_json::json!({"allocated_bytes":83_886_080, "raw":{"observed_us":100,"durations_us":[0,null,65_536],"flags":5}});
        let bytes = serde_json::to_vec(&document).unwrap();
        let imported = import_host(std::io::Cursor::new(bytes), "session", 1).unwrap();
        assert_eq!(
            imported[0]
                .metrics
                .as_ref()
                .unwrap()
                .raw
                .as_ref()
                .unwrap()
                .durations_us[1],
            None
        );
        document["metrics"]["allocated_bytes"] = serde_json::json!(83_886_081u64);
        assert!(serde_json::from_value::<HostObservation>(document.clone())
            .unwrap()
            .validate("session")
            .is_err());
        document["metrics"]["allocated_bytes"] = serde_json::json!(1);
        document["metrics"]["unexpected"] = serde_json::json!(1);
        assert!(serde_json::from_value::<HostObservation>(document).is_err());
    }

    #[test]
    fn host_import_is_bounded_correlated_and_does_not_promote_ack_or_fixture() {
        let receipt = host("host_received", 100);
        let mut delivery = host("console_delivered", 1999);
        let line = serde_json::to_vec(&receipt).unwrap();
        let imported = import_host(std::io::Cursor::new(&line), "session", 1).unwrap();
        assert_eq!(imported[0].provenance, Provenance::SyntheticFixture);
        assert!(import_host(std::io::Cursor::new(&line), "other", 1).is_err());
        assert!(import_host(std::io::Cursor::new(vec![b' '; 65_537]), "session", 1).is_err());
        let duplicate = [line.as_slice(), b"\n", line.as_slice()].concat();
        assert!(import_host(std::io::Cursor::new(duplicate), "session", 2).is_err());
        assert_eq!(
            host_duration(&receipt, &delivery, evidence::Kind::InputDelivery).unwrap(),
            1899
        );
        assert!(host_duration(&receipt, &delivery, evidence::Kind::ProtocolLatency).is_err());
        delivery.event_id = Some("wrong".into());
        assert!(host_duration(&receipt, &delivery, evidence::Kind::InputDelivery).is_err());
        delivery.event_id = receipt.event_id.clone();
        delivery.boundary = "transport_ack".into();
        assert!(delivery.validate("session").is_err());
        delivery.boundary = "delivery_failed".into();
        delivery.reason = Some("fake delivery failed".into());
        assert!(host_duration(&receipt, &delivery, evidence::Kind::InputDelivery).is_err());
    }

    #[test]
    fn exact_nearest_rank_preserves_overflow_and_population_gaps() {
        let mut h = DurationPopulation::new();
        assert_eq!(h.percentile(99).unwrap().value_us, None);
        for n in 1..=100 {
            h.record(n).unwrap();
        }
        assert_eq!(h.percentile(99).unwrap().value_us, Some(99));
        h.record(100_000).unwrap();
        assert_eq!(h.percentile(100).unwrap().lower_bound_us, Some(65_536));
        h.mark_missing(1).unwrap();
        assert_eq!(h.percentile(50).unwrap().value_us, None);
        assert!(h.percentile(0).is_err());
        let mut overflow = DurationPopulation::new();
        overflow.observed = u64::MAX;
        assert!(overflow.record(1).is_err());
        assert_eq!(overflow.percentile(99).unwrap().lower_bound_us, None);
        assert_eq!(overflow.percentile(99).unwrap().value_us, None);
    }

    #[test]
    fn byte_counter_overflow_invalidates_bitrate_instead_of_retaining_a_partial_value() {
        let mut receipts = ReceiptMeasurements::new("boot-A", 10).unwrap();
        receipts.observe("boot-A", 1, 1, u64::MAX).unwrap();
        assert!(receipts.observe("boot-A", 2, 2, 1).is_err());
        assert!(receipts.measured_bitrate(1, 100).is_err());
    }

    #[test]
    fn intervals_and_bitrate_require_ordered_same_clock_observations() {
        let mut receipts = ReceiptMeasurements::new("boot-A", 10).unwrap();
        receipts.observe("boot-A", 1, 100, 1000).unwrap();
        receipts.observe("boot-A", 2, 16_766, 2000).unwrap();
        assert_eq!(
            receipts.intervals.percentile(99).unwrap().value_us,
            Some(16_666)
        );
        assert_eq!(receipts.measured_bitrate(100, 1_000_100).unwrap(), 24_000.0);
        assert!(receipts.observe("boot-B", 3, 20_000, 0).is_err());
        assert!(receipts.observe("boot-A", 3, 100, 0).is_err());
        assert!(receipts.measured_bitrate(100, 100).is_err());
        assert!(receipts.measured_bitrate(100, 1_000_100).is_err());
        let mut gap = ReceiptMeasurements::new("boot-A", 10).unwrap();
        gap.observe("boot-A", 1, 1, 1).unwrap();
        gap.observe("boot-A", 3, 100, 1).unwrap();
        assert_eq!(gap.intervals.percentile(99).unwrap().value_us, None);
    }
}
