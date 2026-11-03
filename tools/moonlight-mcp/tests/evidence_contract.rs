//! T014: report/status contracts. All observations here are synthetic, never acceptance evidence.
//! Activated: T016 (1 absent-value case), T017 (3 report cases), T019 (1 declaration case).
//! Activated: T035 (3 verdict cases), T036 (3 CLI outcome cases), plus the large-declaration regression.
//! Every adapter delegates to the production API; fixtures never establish hardware acceptance.
//! Sources: data-model.md Test Report; contracts/test-client-cli.md Report guarantees and Status
//! and exit semantics; contracts/measurement-evidence.md Verdict completeness and precedence;
//! contracts/implementation-decisions.md D1/D3; tests/scenarios/profiles/acceptance.json v1.0.0.

use serde_json::{json, Value};
use std::path::PathBuf;
use std::process::{Command, Output};

const REQUIRED_FIELDS: &[&str] = &[
    "schema_version",
    "run_id",
    "session_id",
    "scenario",
    "environment",
    "requested",
    "chosen",
    "bitrate_adjustment",
    "video",
    "audio",
    "input",
    "hdr_events",
    "fidelity",
    "criteria",
    "overall_status",
    "limitations",
    "artifact_refs",
];

fn criteria() -> Value {
    json!({"profile_id":"reference_host_compatibility", "profile_version":"1.0.0",
        "ids":["REF-pairing","REF-stream","REF-controller","REF-teardown","REF-recording","REF-report"]})
}

fn declared_scenario() -> Value {
    json!({
        "schema_version":"1.0.0", "id":"synthetic-contract", "host":"127.0.0.1",
        "pairing":{"pin_delivery":"already_paired"},
        "requested":{"width":1920,"height":1080,"fps":60,"bitrate_kbps":20000,
            "codec":"h264","dynamic_range":"sdr"},
        "duration_seconds":1, "controllers":[{"id":0,"actions":[
            {"event_id":"connect-0","at_seconds":0,"type":"connect"},
            {"event_id":"cross-0","at_seconds":0.1,"type":"button","button":"cross","pressed":true},
            {"event_id":"disconnect-0","at_seconds":0.5,"type":"disconnect"}]}],
        "reference":{"required":false}, "required_criteria":criteria(),
        "evidence":{"host_metrics":true,"input_delivery":true,"reference_recording":true,
            "official_client_observations":true,"operator_observations":true},
        "output_dir":"artifacts", "write_policy":{"max_bytes":1048576,"max_files":100,"on_limit":"incomplete"}
    })
}

fn unavailable(id: &str) -> Value {
    json!({"schema_version":"1.0.0","id":id,"session_id":"synthetic-session",
        "provenance":"synthetic_fixture","kind":"client_receipt","source":"client_receipt",
        "interval":null,"clock_domain":"fixture-monotonic","source_clock":null,
        "units":"milliseconds","resolution":0.001,"uncertainty":null,
        "uncertainty_reason":"no calibrated source in synthetic fixture",
        "population":{"expected":60,"observed":0,"represented":0,"excluded":0,
            "missing":60,"overflow":0,"failed_delivery":0},
        "coverage":{"state":"unavailable","enabled_intervals":[],
            "gaps":[{"start":null,"end":null,"reason":"synthetic source absent"}],
            "reason":"synthetic source absent"},
        "value":null,"lower_bound":null,"upper_bound":null,
        "validity":"unavailable","reason":"synthetic source absent","artifact_refs":[]})
}

fn report_fixture() -> Value {
    let scenario = declared_scenario();
    let results: Vec<Value> = scenario["required_criteria"]["ids"]
        .as_array()
        .unwrap()
        .iter()
        .map(|id| {
            json!({"id":id,"threshold":null,"evidence_ids":[],"status":"Incomplete",
            "reason":"synthetic evidence unavailable"})
        })
        .collect();
    json!({
        "schema_version":"1.0.0","run_id":"synthetic-run","session_id":"synthetic-session",
        "scenario":scenario,"environment":{"firmware":null,"host_version":null,
            "client_version":env!("CARGO_PKG_VERSION"),"network":"loopback synthetic fixture",
            "devices":[],"reference_chain":null,"clocks":[],"calibration_ids":[]},
        "requested":declared_scenario()["requested"],"chosen":null,"bitrate_adjustment":null,
        "video":{"frame_rate":unavailable("fps"),"frame_intervals":unavailable("intervals"),
            "protocol_processing_latency":unavailable("protocol"),"full_host_pipeline":unavailable("pipeline"),
            "missing_frames":null,"incomplete_frames":null,"recoverable_with_fec":null,
            "host_drops":null,"measured_bitrate":unavailable("bitrate")},
        "audio":{"packet_loss":unavailable("audio-loss"),"non_silent":null},
        "input":{"client_sent":[],"transport_acknowledged":[],"host_received":[],
            "console_delivered":[],"delivery_latency":unavailable("input-latency")},
        "hdr_events":[],"fidelity":{"per_frame_scores":[],"sample_pairs":[],
            "ssim":null,"psnr":null,"relative_delay":unavailable("delay"),
            "alignment_reliable":false,"compared_frames":0,"excluded_frames":60},
        "criteria":results,"overall_status":"Incomplete",
        "limitations":["synthetic fixtures do not establish hardware or reference-host acceptance"],
        "artifact_refs":[]
    })
}

// Contract adapters call production models/evaluation rather than implementing test-local policy.
fn emit_report(declared: &Value, observed: &Value) -> Option<Value> {
    let report = moonlight_mcp::report::Report::from_observed(declared, observed.clone()).ok()?;
    let mut bytes = Vec::new();
    report.write_to(&mut bytes).ok()?;
    serde_json::from_slice(&bytes).ok()
}
fn emit_measurement(observed: &Value) -> Option<Value> {
    let evidence: moonlight_mcp::evidence::Measurement =
        serde_json::from_value(observed.clone()).ok()?;
    evidence.validate().ok()?;
    serde_json::to_value(evidence).ok()
}
fn accept_declaration(before: &Value, after: &Value) -> Option<bool> {
    let scenario = moonlight_mcp::scenario::Scenario::from_value(before.clone()).ok()?;
    Some(scenario.matches_declaration(after))
}
fn runner_exit_from_client(status: &str) -> Option<i32> {
    moonlight_mcp::runner::exit_code_name(status).ok()
}
fn evaluate(required: &[&str], results: &Value) -> Option<Value> {
    moonlight_mcp::verdict::evaluate(required, results).ok()
}

#[test]
fn expected_report_fixture_covers_the_declared_data_model() {
    // Fixture-integrity check only; this does not claim the client emits this report.
    let fixture = report_fixture();
    for field in REQUIRED_FIELDS {
        assert!(fixture.get(field).is_some(), "fixture lacks {field}");
    }
    assert_eq!(fixture["criteria"].as_array().unwrap().len(), 6);
    assert_eq!(fixture["scenario"]["required_criteria"], criteria());
    assert!(fixture["chosen"].is_null());
    let measurement = unavailable("schema-integrity");
    let schema: Value =
        serde_json::from_str(include_str!("../../../tests/schemas/evidence.schema.json")).unwrap();
    for key in schema["required"].as_array().unwrap() {
        assert!(
            measurement.get(key.as_str().unwrap()).is_some(),
            "missing schema field {key}"
        );
    }
    for key in ["kind", "source", "units", "validity"] {
        assert!(schema["properties"][key]["enum"]
            .as_array()
            .unwrap()
            .contains(&measurement[key]));
    }
}

#[test]
fn acceptance_profile_retains_incomplete_precedence_and_exit_contract() {
    let profile: Value = serde_json::from_str(include_str!(
        "../../../tests/scenarios/profiles/acceptance.json"
    ))
    .unwrap();
    assert_eq!(
        profile["verdict_policy"]["precedence"],
        json!(["Incomplete", "Fail", "Pass"])
    );
    assert_eq!(
        profile["verdict_policy"]["exit_codes"],
        json!({"Pass":0,"Fail":1,"Incomplete":2,"invalid_invocation":64})
    );
    assert_eq!(
        profile["verdict_policy"]["missing_numeric_value"],
        Value::Null
    );
    assert_eq!(
        profile["profiles"]["reference_host_compatibility"]["required_criteria"],
        criteria()["ids"]
    );
}

#[test]
fn missing_numeric_evidence_never_becomes_zero_or_exact_bounds() {
    let missing = unavailable("missing-input-delivery");
    assert_eq!(emit_measurement(&missing), Some(missing));
}

#[test]
fn evidence_rejects_fabricated_values_bad_counts_and_clock_mixing() {
    use moonlight_mcp::evidence::{input_delivery_duration, Measurement};
    for (field, value) in [("value", json!(0)), ("kind", json!("full_pipeline"))] {
        let mut invalid = unavailable("invalid");
        invalid[field] = value;
        assert!(serde_json::from_value::<Measurement>(invalid)
            .unwrap()
            .validate()
            .is_err());
    }
    let mut bounded = unavailable("bounded");
    bounded["validity"] = json!("bounded");
    bounded["lower_bound"] = json!(65535);
    let parsed: Measurement = serde_json::from_value(bounded.clone()).unwrap();
    parsed.validate().unwrap();
    assert!(serde_json::to_value(parsed).unwrap()["value"].is_null());
    bounded["upper_bound"] = json!(1);
    assert!(serde_json::from_value::<Measurement>(bounded)
        .unwrap()
        .validate()
        .is_err());
    let mut received = unavailable("received");
    received["source"] = json!("host_pipeline");
    received["kind"] = json!("host_received");
    received["event_id"] = json!("event-1");
    received["controller_id"] = json!(0);
    received["interval"] = json!({"start":100,"end":100});
    received["source_clock"] = json!({"id":"fixture-monotonic","type":"monotonic","epoch":"boot-1",
        "native_resolution_ns":1000,"uncertainty_ns":null,"uncertainty_reason":"synthetic clock",
        "conversion":"integer microseconds rounded down","quantization_bound_ns":999,
        "validity":"valid","reason":null});
    received["validity"] = json!("valid");
    received["reason"] = Value::Null;
    received["value"] = json!(100);
    received["population"] = json!({"expected":1,"observed":1,"represented":1,"excluded":0,"missing":0,"overflow":0,"failed_delivery":0});
    received["coverage"] = json!({"state":"complete","enabled_intervals":[{"start":100,"end":200}],"gaps":[],"reason":null});
    let a: Measurement = serde_json::from_value(received.clone()).unwrap();
    a.validate().unwrap();
    let mut delivered = received.clone();
    delivered["id"] = json!("delivered");
    delivered["kind"] = json!("console_delivered");
    delivered["authenticated"] = json!(true);
    delivered["delivery_boundary"] = json!("synthetic platform completion, not hardware evidence");
    delivered["interval"] = json!({"start":150,"end":150});
    let b: Measurement = serde_json::from_value(delivered.clone()).unwrap();
    assert_eq!(input_delivery_duration(&a, &b).unwrap(), 50);
    for pointer in ["/event_id", "/source_clock/epoch", "/kind"] {
        let mut invalid = delivered.clone();
        *invalid.pointer_mut(pointer).unwrap() = json!(if pointer == "/kind" {
            "transport_ack"
        } else {
            "different"
        });
        let b: Measurement = serde_json::from_value(invalid).unwrap();
        assert!(input_delivery_duration(&a, &b).is_err(), "{pointer}");
    }
    received["population"]["missing"] = json!(1);
    assert!(serde_json::from_value::<Measurement>(received)
        .unwrap()
        .validate()
        .is_err());
}

#[test]
fn new_report_preserves_declaration_interruption_and_local_references() {
    use moonlight_mcp::evidence::ArtifactRef;
    use moonlight_mcp::report::{Environment, Report, Status};
    let fixture = report_fixture();
    let environment: Environment = serde_json::from_value(fixture["environment"].clone()).unwrap();
    let mut report = Report::new(
        declared_scenario(),
        environment,
        "run-1".into(),
        "synthetic-session".into(),
    )
    .unwrap();
    assert_eq!(report.scenario(), &declared_scenario());
    assert_eq!(report.criteria.len(), 6);
    report.criteria[0].status = Status::Fail;
    report.criteria[0].reason = "observed synthetic failure".into();
    report.finalize(Some("run interrupted")).unwrap();
    assert_eq!(report.overall_status, Status::Incomplete);
    assert_eq!(report.criteria[0].status, Status::Fail);
    for path in [
        "../capture",
        "/absolute",
        "https://example.com/file",
        "symlink/../escape",
    ] {
        assert!(ArtifactRef(path.into()).validate().is_err());
    }
    assert!(ArtifactRef("frames/native.p010".into()).validate().is_ok());
    let mut changed = declared_scenario();
    changed["controllers"] = json!([]);
    assert!(Report::from_observed(&changed, fixture).is_err());
}

#[test]
fn report_writer_rejects_forged_status_or_removed_required_results() {
    use moonlight_mcp::report::{Report, Status};
    let mut report = Report::from_observed(&declared_scenario(), report_fixture()).unwrap();
    report.overall_status = Status::Pass;
    assert!(
        report.write_to(Vec::new()).is_err(),
        "missing evidence cannot pass"
    );
    report.overall_status = Status::Incomplete;
    report.criteria.pop();
    assert!(
        report.write_to(Vec::new()).is_err(),
        "required criteria cannot disappear"
    );
}

#[test]
fn every_report_field_survives_serialization() {
    let expected = report_fixture();
    assert_eq!(emit_report(&declared_scenario(), &expected), Some(expected));
}

#[test]
fn interrupted_report_keeps_failed_criteria_and_missing_evidence() {
    let mut expected = report_fixture();
    expected["criteria"][0]["status"] = json!("Fail");
    expected["criteria"][0]["reason"] = json!("synthetic pairing proof rejected");
    expected["limitations"]
        .as_array_mut()
        .unwrap()
        .push(json!("run interrupted"));
    assert_eq!(emit_report(&declared_scenario(), &expected), Some(expected));
}

#[test]
fn bitrate_clamp_is_explicit_and_measured_throughput_is_separate() {
    // Bounds are synthetic test data; no supported PS5 bitrate range is asserted.
    for (request, chosen, reason) in [
        (1, 1000, "raised_to_minimum"),
        (20000, 20000, "none"),
        (100001, 100000, "lowered_to_maximum"),
    ] {
        let mut scenario = declared_scenario();
        scenario["requested"]["bitrate_kbps"] = json!(request);
        let mut expected = report_fixture();
        expected["scenario"] = scenario.clone();
        expected["requested"] = scenario["requested"].clone();
        expected["chosen"] = scenario["requested"].clone();
        expected["chosen"]["bitrate_kbps"] = json!(chosen);
        expected["bitrate_adjustment"] = json!({"requested_kbps":request,"chosen_kbps":chosen,
            "minimum_kbps":1000,"maximum_kbps":100000,"reason":reason});
        expected["video"]["measured_bitrate"] = unavailable("measured-bitrate");
        assert_eq!(emit_report(&scenario, &expected), Some(expected));
    }
}

#[test]
fn criterion_set_profile_and_controller_declaration_are_immutable() {
    let before = declared_scenario();
    assert_eq!(accept_declaration(&before, &before), Some(true));
    for pointer in [
        "/required_criteria/ids",
        "/required_criteria/profile_version",
        "/controllers",
    ] {
        let mut changed = before.clone();
        *changed.pointer_mut(pointer).unwrap() = json!([]);
        assert_eq!(
            accept_declaration(&before, &changed),
            Some(false),
            "{pointer}"
        );
    }
}

#[test]
fn large_valid_declaration_uses_a_bounded_digest_and_local_artifact() {
    use moonlight_mcp::report::{Environment, Report};
    let mut declared = declared_scenario();
    let mut actions = vec![json!({"event_id":"connect","at_seconds":0,"type":"connect"})];
    for i in 0..9000 {
        actions.push(json!({"event_id":format!("action-{i:05}-{}","x".repeat(90)),"at_seconds":(i+1) as f64/10000.0,
            "type":"button","button":"cross","pressed":i%2==0}));
    }
    declared["controllers"][0]["actions"] = json!(actions);
    moonlight_mcp::scenario::Scenario::from_value(declared.clone()).unwrap();
    assert!(serde_json::to_vec(&declared).unwrap().len() > 1048576);
    let environment: Environment =
        serde_json::from_value(report_fixture()["environment"].clone()).unwrap();
    let report = Report::new_ref(
        &declared,
        environment,
        "large-fixture".into(),
        "synthetic-session".into(),
    )
    .unwrap();
    assert!(report.external_declaration());
    assert!(serde_json::to_vec(report.scenario()).unwrap().len() < 16384);
    assert_eq!(
        report.scenario()["declaration"]["artifact_ref"],
        "scenario.json"
    );
    assert_eq!(report.scenario()["controllers"][0]["action_count"], 9001);
    let observed = serde_json::to_value(&report).unwrap();
    Report::from_observed(&declared, observed.clone()).unwrap();
    declared["controllers"][0]["actions"][1]["pressed"] = json!(false);
    assert!(
        Report::from_observed(&declared, observed).is_err(),
        "digest must preserve exact controller declaration"
    );
}

#[test]
fn all_status_combinations_preserve_individual_failures() {
    for a in ["Pass", "Fail", "Incomplete"] {
        for b in ["Pass", "Fail", "Incomplete"] {
            for c in ["Pass", "Fail", "Incomplete"] {
                let states = [a, b, c];
                let overall = if states.contains(&"Incomplete") {
                    "Incomplete"
                } else if states.contains(&"Fail") {
                    "Fail"
                } else {
                    "Pass"
                };
                let results =
                    json!([{"id":"a","status":a},{"id":"b","status":b},{"id":"c","status":c}]);
                let expected = json!({"overall_status":overall,"criteria":results});
                assert_eq!(
                    evaluate(&["a", "b", "c"], &results),
                    Some(expected),
                    "{states:?}"
                );
            }
        }
    }
}

#[test]
fn missing_required_criterion_cannot_be_removed_or_relabelled() {
    let results = json!([{"id":"a","status":"Fail","reason":"observed failure"}]);
    let expected = json!({"overall_status":"Incomplete","criteria":[
        {"id":"a","status":"Fail","reason":"observed failure"},
        {"id":"b","status":"Incomplete","reason":"required evidence unavailable"}]});
    assert_eq!(evaluate(&["a", "b"], &results), Some(expected));
}

#[test]
fn absent_values_and_uncertainty_do_not_round_into_pass() {
    for (value, lower, upper) in [
        (Value::Null, Value::Null, Value::Null),
        (json!(1.999), json!(1.998), json!(2.001)),
    ] {
        let results = json!([{"id":"SC-005","value":value,"lower_bound":lower,"upper_bound":upper,
            "threshold":{"operator":"lt","value":2.0,"units":"ms"}}]);
        let outcome = evaluate(&["SC-005"], &results);
        assert_eq!(
            outcome.as_ref().and_then(|v| v.get("overall_status")),
            Some(&json!("Incomplete"))
        );
    }
}

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let path = std::env::temp_dir()
            .join("moonlight-contract-fixtures")
            .join(format!("evidence-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn invoke(&self, args: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_moonlight-mcp"))
            .arg("--config-dir")
            .arg(self.0.join("identity"))
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap()
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

#[test]
fn invalid_invocations_have_a_distinct_non_acceptance_exit() {
    let scratch = Scratch::new();
    std::fs::write(scratch.0.join("invalid.json"), "{broken").unwrap();
    for args in [
        vec![],
        vec!["unknown-command"],
        vec!["run"],
        vec![
            "run",
            "--scenario",
            "invalid.json",
            "--report",
            "report.json",
        ],
        vec![
            "run",
            "--scenario",
            "missing.json",
            "--report",
            "report.json",
        ],
    ] {
        let output = scratch.invoke(&args);
        assert_eq!(
            output.status.code(),
            Some(64),
            "{args:?}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(!output.stderr.is_empty());
        assert!(!scratch.0.join("report.json").exists());
    }
}

#[test]
fn unavailable_run_emits_incomplete_report_with_all_declared_criteria() {
    let scratch = Scratch::new();
    std::fs::write(
        scratch.0.join("scenario.json"),
        serde_json::to_vec(&declared_scenario()).unwrap(),
    )
    .unwrap();
    let output = scratch.invoke(&[
        "run",
        "--scenario",
        "scenario.json",
        "--report",
        "report.json",
    ]);
    assert_eq!(output.status.code(), Some(2));
    let report: Value =
        serde_json::from_slice(&std::fs::read(scratch.0.join("report.json")).unwrap()).unwrap();
    for field in REQUIRED_FIELDS {
        assert!(report.get(field).is_some(), "missing {field}");
    }
    assert_eq!(report["overall_status"], "Incomplete");
    assert_eq!(report["scenario"]["required_criteria"], criteria());
    let results = report["criteria"].as_array().unwrap();
    assert_eq!(results.len(), 6);
    for id in criteria()["ids"].as_array().unwrap() {
        assert!(results.iter().any(|r| r["id"] == *id));
    }
}

#[test]
fn pass_fail_incomplete_and_invalid_invocation_have_distinct_exits() {
    for (status, code) in [
        ("Pass", 0),
        ("Fail", 1),
        ("Incomplete", 2),
        ("invalid_invocation", 64),
    ] {
        assert_eq!(runner_exit_from_client(status), Some(code));
    }
}
