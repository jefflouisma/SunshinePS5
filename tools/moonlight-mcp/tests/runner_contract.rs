//! T039: real CLI and stdio agent integration; all recordings and observations are synthetic.
//! Passing these checks establishes software exit/status behavior, never reference-host acceptance.
use moonlight_mcp::reference::{write_index, RecordingIndex};
use serde_json::{json, Value};
use std::path::PathBuf;
use std::process::{Command, Output, Stdio};
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let root = std::env::temp_dir()
            .join("sunshine-t039")
            .join(uuid::Uuid::new_v4().to_string());
        std::fs::create_dir_all(&root).unwrap();
        Self(root)
    }
    fn write(&self, name: &str, value: &Value) {
        std::fs::write(self.0.join(name), serde_json::to_vec(value).unwrap()).unwrap();
    }
    fn read(&self, name: &str) -> Value {
        serde_json::from_slice(&std::fs::read(self.0.join(name)).unwrap()).unwrap()
    }
    fn invoke(&self, args: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_moonlight-mcp"))
            .current_dir(&self.0)
            .arg("--config-dir")
            .arg(self.0.join("identity"))
            .args(args)
            .output()
            .unwrap()
    }
    fn scenario(&self) -> Value {
        let mut value: Value =
            serde_json::from_str(include_str!("../../../tests/scenarios/reference-host.json"))
                .unwrap();
        value["id"] = json!("synthetic-runner-integration");
        value["host"] = json!("127.0.0.1");
        // A declared but unbound provider fails before any network operation.
        value["pairing"] = json!({"pin_delivery":"wolf","provider_ref":"unbound-fixture"});
        value
    }
    fn pictures(&self, unresolved: bool) {
        for name in ["stream", "reference"] {
            let dir = self.0.join(name);
            std::fs::create_dir(&dir).unwrap();
            write_index(
                &dir,
                &RecordingIndex {
                    source: "synthetic T039 fixture".into(),
                    width: 32,
                    height: 32,
                    timestamps: vec![1.0, 1.02, 1.04, 1.06],
                },
            )
            .unwrap();
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
            let mut metadata = self.read(&format!("{name}/recording.json"));
            metadata["calibration_id"] = json!("synthetic-fixture-calibration");
            metadata["calibration_reason"] = Value::Null;
            self.write(&format!("{name}/recording.json"), &metadata);
        }
        let mut criteria = vec![
            json!({"id":"synthetic-floor","metric":"ssim_floor_fraction",
            "threshold":{"operator":"gte","value":0.99,"units":"ratio"},"uncertainty":0.0}),
        ];
        if unresolved {
            criteria.push(json!({"id":"unresolved-median","metric":"ssim_median",
                "threshold":{"operator":"gte","value":null,"units":"ratio"},"uncertainty":null}));
        }
        self.write(
            "profile.json",
            &json!({"schema_version":"1.0.0","population":"all_stream_frames",
            "width":32,"height":32,"bit_depth":8,"color_space":"BT.709",
            "calibration_id":"synthetic-fixture-calibration",
            "options":{"min_delay_ms":0.0,"max_delay_ms":0.0},"criteria":criteria}),
        );
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

#[test]
fn cli_invalid_declaration_exits_64_without_report_or_identity() {
    let scratch = Scratch::new();
    let mut scenario = scratch.scenario();
    scenario["required_criteria"]["ids"] = json!(["REF-stream"]);
    scratch.write("scenario.json", &scenario);
    let output = scratch.invoke(&[
        "run",
        "--scenario",
        "scenario.json",
        "--report",
        "report.json",
    ]);
    assert_eq!(output.status.code(), Some(64));
    assert!(!output.stderr.is_empty());
    assert!(!scratch.0.join("report.json").exists());
    assert!(!scratch.0.join("identity").exists());
}

#[test]
fn cli_unavailable_provider_finalizes_all_criteria_and_exits_2() {
    let scratch = Scratch::new();
    let scenario = scratch.scenario();
    scratch.write("scenario.json", &scenario);
    let output = scratch.invoke(&[
        "run",
        "--scenario",
        "scenario.json",
        "--report",
        "report.json",
    ]);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    let summary: Value = serde_json::from_slice(&output.stdout).unwrap();
    let report = scratch.read("report.json");
    assert_eq!(summary["overall_status"], "Incomplete");
    assert_eq!(report["overall_status"], "Incomplete");
    assert_eq!(report["scenario"], scenario);
    assert_eq!(report["criteria"].as_array().unwrap().len(), 6);
    assert!(report["chosen"].is_null());
    assert!(!report["limitations"].as_array().unwrap().is_empty());
    let typed: moonlight_mcp::report::Report = serde_json::from_value(report).unwrap();
    typed.validate().unwrap();
}

#[test]
fn cli_reference_lifecycle_binding_cannot_promote_an_unstarted_run() {
    let scratch = Scratch::new();
    scratch.write("scenario.json", &scratch.scenario());
    let output = scratch.invoke(&[
        "run",
        "--scenario",
        "scenario.json",
        "--report",
        "report.json",
        "--reference-lifecycle",
        "absent-observer.json",
    ]);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    let report = scratch.read("report.json");
    assert!(report["criteria"]
        .as_array()
        .unwrap()
        .iter()
        .all(|c| c["status"] == "Incomplete"));
    assert_eq!(report["overall_status"], "Incomplete");
}

#[test]
fn cli_reference_recording_binding_cannot_promote_an_unstarted_run() {
    let scratch = Scratch::new();
    scratch.write("scenario.json", &scratch.scenario());
    let output = scratch.invoke(&[
        "run",
        "--scenario",
        "scenario.json",
        "--report",
        "report.json",
        "--reference-recording",
        "absent-observer.json",
    ]);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    let report = scratch.read("report.json");
    assert!(report["criteria"]
        .as_array()
        .unwrap()
        .iter()
        .all(|c| c["status"] == "Incomplete"));
    assert_eq!(report["overall_status"], "Incomplete");
}

#[test]
fn cli_comparison_failure_is_nonzero_and_survives_incomplete_precedence() {
    for unresolved in [false, true] {
        let scratch = Scratch::new();
        scratch.pictures(unresolved);
        let output = scratch.invoke(&[
            "compare",
            "--stream",
            "stream",
            "--reference",
            "reference",
            "--out",
            "comparison",
            "--profile",
            "profile.json",
            "--report",
            "report.json",
        ]);
        assert_eq!(
            output.status.code(),
            Some(if unresolved { 2 } else { 1 }),
            "{output:?}"
        );
        let report = scratch.read("report.json");
        assert_eq!(
            report["overall_status"],
            if unresolved { "Incomplete" } else { "Fail" }
        );
        assert_eq!(report["criteria"][0]["status"], "Fail");
        assert_eq!(report["comparison"]["stream_frames"], 4);
        assert_eq!(report["comparison"]["excluded_frames"], 0);
        if unresolved {
            assert_eq!(report["criteria"][1]["status"], "Incomplete");
        }
    }
}

async fn rpc(
    input: &mut tokio::process::ChildStdin,
    output: &mut BufReader<tokio::process::ChildStdout>,
    request: Value,
) -> Value {
    input
        .write_all(format!("{request}\n").as_bytes())
        .await
        .unwrap();
    input.flush().await.unwrap();
    let mut line = String::new();
    let size = tokio::time::timeout(Duration::from_secs(20), output.read_line(&mut line))
        .await
        .unwrap()
        .unwrap();
    assert!(size > 0, "MCP ended without a response");
    let response: Value = serde_json::from_str(&line).unwrap();
    assert_eq!(response["id"], request["id"]);
    response
}

#[tokio::test]
async fn agent_run_and_compare_keep_failure_and_incomplete_typed_over_real_stdio() {
    for mode in ["run", "fail", "incomplete"] {
        let scratch = Scratch::new();
        if mode == "run" {
            scratch.write("scenario.json", &scratch.scenario());
        } else {
            scratch.pictures(mode == "incomplete");
        }
        let mut child = tokio::process::Command::new(env!("CARGO_BIN_EXE_moonlight-mcp"))
            .current_dir(&scratch.0)
            .arg("--config-dir")
            .arg(scratch.0.join("identity"))
            .args(["mcp", "--output-root"])
            .arg(&scratch.0)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true)
            .spawn()
            .unwrap();
        let mut input = child.stdin.take().unwrap();
        let mut output = BufReader::new(child.stdout.take().unwrap());
        let initialized = rpc(
            &mut input,
            &mut output,
            json!({"jsonrpc":"2.0","id":1,"method":"initialize",
            "params":{"protocolVersion":"2025-06-18"}}),
        )
        .await;
        assert_eq!(initialized["result"]["protocolVersion"], "2025-06-18");
        input
            .write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
            .await
            .unwrap();
        let params = if mode == "run" {
            json!({"name":"run_scenario","arguments":{"scenario":"scenario.json","report":"report.json"}})
        } else {
            json!({"name":"compare_recordings","arguments":{"stream":"stream","reference":"reference",
                "out":"comparison","profile":"profile.json","report":"report.json"}})
        };
        let reply = rpc(
            &mut input,
            &mut output,
            json!({"jsonrpc":"2.0","id":2,"method":"tools/call","params":params}),
        )
        .await;
        assert!(reply.get("error").is_none(), "{reply}");
        assert_eq!(reply["result"]["isError"], true);
        let result = &reply["result"]["structuredContent"];
        assert_eq!(
            result["overall_status"],
            if mode == "fail" { "Fail" } else { "Incomplete" }
        );
        if mode != "run" {
            assert_eq!(result["criteria"][0]["status"], "Fail");
        }
        let persisted = scratch.read("report.json");
        assert_eq!(persisted["overall_status"], result["overall_status"]);
        let read = rpc(
            &mut input,
            &mut output,
            json!({"jsonrpc":"2.0","id":3,"method":"tools/call",
            "params":{"name":"read_report","arguments":{"report":"report.json"}}}),
        )
        .await;
        assert_eq!(
            read["result"]["structuredContent"]["overall_status"],
            result["overall_status"]
        );
        assert_eq!(read["result"]["isError"], true);
        input.shutdown().await.unwrap();
        drop(input);
        assert!(tokio::time::timeout(Duration::from_secs(10), child.wait())
            .await
            .unwrap()
            .unwrap()
            .success());
    }
}

/// T038 REF-controller evaluation. Every record below is a SYNTHETIC test fixture: the
/// `reference_host` provenance on some of them only exercises the evaluation logic and is never
/// written to evidence; real records come solely from the trusted --host-observations binding.
mod controller_outcome {
    use moonlight_mcp::measurements::HostObservation;
    use moonlight_mcp::report::Status;
    use moonlight_mcp::runner::controller_outcome;
    use serde_json::json;

    fn record(event: &str, boundary: &str, provenance: &str) -> HostObservation {
        let mut value = json!({
            "schema_version":"1.0.0", "id":format!("{event}-{boundary}-{provenance}"),
            "session_id":"session", "provenance":provenance, "source":"host_pipeline",
            "kind":"input", "event_id":event, "controller_id":0, "boundary":boundary,
            "authenticated":true, "delivery_boundary":"synthetic documented boundary",
            "clock_domain":"host-clock", "source_clock":{
                "id":"host-clock", "type":"monotonic", "epoch":"boot", "native_resolution_ns":1,
                "conversion":"floor(ns/1000)", "quantization_bound_ns":1000,
                "uncertainty_ns":null, "uncertainty_reason":"synthetic clock",
                "validity":"valid", "reason":null
            }, "observed_at":100,
            "population":{"expected":1,"observed":1,"represented":1,"excluded":0,"missing":0,
                "overflow":0,"failed_delivery":0},
            "coverage":{"state":"complete","enabled_intervals":[{"start":1,"end":10000}],
                "gaps":[],"reason":null},
            "validity":"valid","reason":null,"artifact_refs":[],"measurements":[]
        });
        if boundary == "delivery_failed" {
            value["reason"] = json!("synthetic failure");
        }
        let observation: HostObservation = serde_json::from_value(value).unwrap();
        observation.validate("session").unwrap();
        observation
    }

    const EXPECTED: [(u8, &str); 2] = [(0, "stick-left-0"), (0, "stick-neutral-0")];

    #[test]
    fn independent_delivery_of_every_declared_action_is_required_to_pass() {
        let all = [
            record("stick-left-0", "console_delivered", "reference_host"),
            record("stick-neutral-0", "console_delivered", "reference_host"),
        ];
        assert_eq!(controller_outcome(&EXPECTED, &all).0, Status::Pass);
        assert_eq!(
            controller_outcome(&EXPECTED, &all[..1]).0,
            Status::Incomplete
        );
        assert_eq!(controller_outcome(&[], &all).0, Status::Incomplete);
        // A receipt or another controller's delivery never substitutes for this one.
        let receipt = [
            record("stick-left-0", "console_delivered", "reference_host"),
            record("stick-neutral-0", "host_received", "reference_host"),
        ];
        assert_eq!(
            controller_outcome(&EXPECTED, &receipt).0,
            Status::Incomplete
        );
        let other = [(1, "stick-left-0"), (0, "stick-neutral-0")];
        assert_eq!(controller_outcome(&other, &all).0, Status::Incomplete);
    }

    #[test]
    fn synthetic_provenance_never_passes_and_failure_outranks_incomplete() {
        let synthetic = [
            record("stick-left-0", "console_delivered", "synthetic_fixture"),
            record("stick-neutral-0", "console_delivered", "synthetic_fixture"),
        ];
        let (status, reason) = controller_outcome(&EXPECTED, &synthetic);
        assert_eq!(status, Status::Incomplete);
        assert!(reason.contains("synthetic"));
        // One action failed while the other is unobserved: Fail takes precedence.
        let failed = [record(
            "stick-neutral-0",
            "delivery_failed",
            "reference_host",
        )];
        assert_eq!(controller_outcome(&EXPECTED, &failed).0, Status::Fail);
        // A synthetic failure is not reference evidence either way.
        let fixture_failure = [
            record("stick-left-0", "console_delivered", "reference_host"),
            record("stick-neutral-0", "delivery_failed", "synthetic_fixture"),
        ];
        assert_eq!(
            controller_outcome(&EXPECTED, &fixture_failure).0,
            Status::Incomplete
        );
    }

    #[test]
    fn only_reference_host_provenance_establishes_reference_acceptance() {
        // Console-side or operator records are not observations by the reference host itself,
        // so they can neither pass nor fail REF-controller.
        for provenance in ["console_hardware", "operator_observation"] {
            let foreign = [
                record("stick-left-0", "console_delivered", provenance),
                record("stick-neutral-0", "console_delivered", provenance),
            ];
            let (status, reason) = controller_outcome(&EXPECTED, &foreign);
            assert_eq!(status, Status::Incomplete, "{provenance}");
            assert!(reason.contains("reference host"), "{provenance}: {reason}");
            let mixed = [
                record("stick-left-0", "console_delivered", "reference_host"),
                record("stick-neutral-0", "console_delivered", provenance),
            ];
            assert_eq!(
                controller_outcome(&EXPECTED, &mixed).0,
                Status::Incomplete,
                "{provenance}"
            );
            let foreign_failure = [
                record("stick-left-0", "console_delivered", "reference_host"),
                record("stick-neutral-0", "delivery_failed", provenance),
            ];
            assert_eq!(
                controller_outcome(&EXPECTED, &foreign_failure).0,
                Status::Incomplete,
                "{provenance}"
            );
        }
    }
}
