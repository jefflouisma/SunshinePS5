use moonlight_mcp::reference_recording::{load, profile_digest};
use moonlight_mcp::report::Status;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::path::PathBuf;

struct Fixture {
    root: PathBuf,
    value: Value,
}
impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!("ref-recording-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&root).unwrap();
        Self {
            root,
            value: manifest(),
        }
    }
    fn artifact(&mut self, name: &str, bytes: &[u8]) {
        std::fs::write(self.root.join(name), bytes).unwrap();
        let entries = self.value["artifacts"].as_array_mut().unwrap();
        entries.retain(|a| a["path"] != name);
        entries.push(json!({"path":name,"sha256":hex::encode(Sha256::digest(bytes))}));
    }
    fn load(&self) -> anyhow::Result<moonlight_mcp::reference_recording::Imported> {
        let path = self.root.join("manifest.json");
        std::fs::write(&path, serde_json::to_vec(&self.value).unwrap()).unwrap();
        load(&path, "session", "scenario", Some("host"))
    }
    fn populations(&mut self) {
        self.artifact(
            "oracle.txt",
            b"SYNTHETIC observer oracle for importer tests only",
        );
        for (index, kind) in ["sdr", "hdr", "sdr_known_difference", "hdr_known_difference"]
            .iter()
            .enumerate()
        {
            let hdr = index % 2 == 1;
            let different = index >= 2;
            let a: Vec<u16> = (0..384).map(|i| (i % 200 + 20) as u16).collect();
            let b: Vec<u16> = a
                .iter()
                .map(|n| n + if different { 20 } else { 0 })
                .collect();
            let packed = |codes: &[u16]| -> Vec<u8> {
                if hdr {
                    codes.iter().flat_map(|n| n.to_le_bytes()).collect()
                } else {
                    codes.iter().map(|n| *n as u8).collect()
                }
            };
            let sa = format!("stream-{index}.raw");
            let sb = format!("reference-{index}.raw");
            self.artifact(&sa, &packed(&a));
            self.artifact(&sb, &packed(&b));
            let color = if hdr {
                moonlight_mcp::recording::ColorSpace::PqBt2020
            } else {
                moonlight_mcp::recording::ColorSpace::Bt709
            };
            let scores = [(0, 256, 16), (256, 320, 8), (320, 384, 8)].map(|(lo, hi, w)| {
                moonlight_mcp::compare::score_plane(&a[lo..hi], &b[lo..hi], w, w, color).unwrap()
            });
            let row = json!({"stream_frame":0,"stream_time_seconds":1.0,"reference_frame":0,"reference_time_seconds":1.0,"timestamp_residual_ms":0.0,
                "ssim":scores[0].ssim,"ssim_bits":format!("{:016x}",scores[0].ssim.to_bits()),
                "psnr_db_yuv":scores.iter().map(|s|s.psnr_db).collect::<Vec<_>>(),"perfect_psnr_yuv":scores.iter().map(|s|s.perfect).collect::<Vec<_>>(),"exclusion_reason":null});
            let score_path = format!("scores-{index}.json");
            self.artifact(&score_path, &serde_json::to_vec(&row).unwrap());
            let native = |path: &str| {
                json!({"source":"independent observer test fixture","capture_mode":{"width":16,"height":16,"fps":60},"calibration_id":"calibration","calibration_reason":null,
                "frames":path,"pixel_format":if hdr {"yuv420p10le"} else {"yuv420p"},"bit_depth":if hdr {10} else {8},"color_space":if hdr {"PQ/BT.2020"} else {"BT.709"},
                "clock_domain":"shared-test-epoch","frame_count":1,"timestamp_count":1,"timestamps":[1000000],"validity":"valid","metadata_evidence":if hdr {json!("hdr-epoch")} else {Value::Null}})
            };
            let metadata = if hdr {
                json!({"epoch":"hdr-epoch","provenance":"reference_host","source":"oracle.txt","mastering_max_nits":1000.0,"mastering_min_nits":0.01,"max_cll_nits":800.0,"max_fall_nits":200.0,
                "primaries_xy":[[0.708,0.292],[0.17,0.797],[0.131,0.046]],"white_xy":[0.3127,0.329]})
            } else {
                Value::Null
            };
            self.value["populations"].as_array_mut().unwrap().push(json!({"kind":kind,"stream":native(&sa),"reference":native(&sb),"calibration":{"id":"calibration","provenance":"reference_host","source":"oracle.txt","uncertainty_ms":0.0},
                "source_hdr":metadata,"stream_hdr":metadata,"delay_ms":0.0,"expected_delay_ms":[0.0,0.0],"expected_ssim":if different {json!([scores[0].ssim-0.001,scores[0].ssim+0.001])} else {json!([1.0,1.0])},"oracle":"oracle.txt","scores":score_path}));
        }
    }
    fn reports(&mut self) {
        use moonlight_mcp::report::{Environment, Report};
        self.artifact(
            "oracle.txt",
            b"SYNTHETIC observer oracle for importer tests only",
        );
        for (index, kind) in ["pass", "fail", "incomplete", "fail_under_incomplete"]
            .iter()
            .enumerate()
        {
            let mut r = Report::new(json!({"id":"report-case","requested":{"width":16,"height":16,"fps":60,"bitrate_kbps":1000,"codec":"h264","dynamic_range":"sdr"},"required_criteria":{"profile_id":"reference_host_compatibility","profile_version":"1.0.0","ids":["REF-pairing","REF-stream","REF-controller","REF-teardown","REF-recording","REF-report"]}}),
                Environment {firmware:None,host_version:Some("host".into()),client_version:env!("CARGO_PKG_VERSION").into(),network:"synthetic".into(),devices:vec![],reference_chain:None,clocks:vec![],calibration_ids:vec![]},format!("case-{index}"),format!("case-{index}")).unwrap();
            for c in &mut r.criteria {
                c.status = match index {
                    0 => Status::Pass,
                    1 => Status::Fail,
                    _ => Status::Incomplete,
                };
                c.evidence_ids = vec!["oracle".into()];
            }
            if index == 3 {
                r.criteria[0].status = Status::Fail;
            }
            r.overall_status = match index {
                0 => Status::Pass,
                1 => Status::Fail,
                _ => Status::Incomplete,
            };
            r.validate().unwrap();
            let expected = format!("expected-{index}.json");
            let emitted = format!("emitted-{index}.json");
            let bytes = serde_json::to_vec(&r).unwrap();
            self.artifact(&expected, &bytes);
            self.artifact(&emitted, &bytes);
            self.value["reports"].as_array_mut().unwrap().push(
                json!({"kind":kind,"expected":expected,"emitted":emitted,"oracle":"oracle.txt"}),
            );
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.root).unwrap();
    }
}

#[test]
fn native_populations_recompute_scores_and_require_calibration_hdr_and_difference_oracles() {
    let mut f = Fixture::new();
    f.populations();
    assert_eq!(f.load().unwrap().recording, Status::Pass);
    f.value["populations"][0]["calibration"] = Value::Null;
    assert_eq!(f.load().unwrap().recording, Status::Incomplete);
    f.value["populations"][2]["expected_ssim"] = json!([-1.0, 0.0]);
    assert_eq!(f.load().unwrap().recording, Status::Fail);
    f.value["provenance"] = json!("synthetic_fixture");
    assert_eq!(f.load().unwrap().recording, Status::Incomplete);
}

#[test]
fn report_matrix_requires_all_four_outcomes_and_full_equality() {
    let mut f = Fixture::new();
    f.reports();
    assert_eq!(f.load().unwrap().report, Status::Pass);
    f.value["reports"].as_array_mut().unwrap().pop();
    assert_eq!(f.load().unwrap().report, Status::Incomplete);
    let mut emitted: Value =
        serde_json::from_slice(&std::fs::read(f.root.join("emitted-0.json")).unwrap()).unwrap();
    emitted["audio"]["non_silent"] = json!(true);
    f.artifact("emitted-0.json", &serde_json::to_vec(&emitted).unwrap());
    assert_eq!(f.load().unwrap().report, Status::Fail);
}

#[test]
fn independently_declared_delay_is_checked_against_every_timestamp() {
    let mut f = Fixture::new();
    f.populations();
    f.value["populations"][0]["stream"]["timestamps"] = json!([1050000]);
    f.value["populations"][0]["delay_ms"] = json!(50.0);
    f.value["populations"][0]["expected_delay_ms"] = json!([50.0, 50.0]);
    let mut row: Value =
        serde_json::from_slice(&std::fs::read(f.root.join("scores-0.json")).unwrap()).unwrap();
    row["stream_time_seconds"] = json!(1.05);
    f.artifact("scores-0.json", &serde_json::to_vec(&row).unwrap());
    assert_eq!(f.load().unwrap().recording, Status::Pass);
    f.value["populations"][0]["expected_delay_ms"] = json!([0.0, 10.0]);
    assert_eq!(f.load().unwrap().recording, Status::Fail);
}

#[test]
fn calibrated_delay_containment_disjointness_and_overlap_have_distinct_verdicts() {
    for (expected, status) in [
        ([-1.0, 1.0], Status::Pass),
        ([-2.0, 2.0], Status::Pass),
        ([2.0, 3.0], Status::Fail),
        ([-3.0, -2.0], Status::Fail),
        ([0.0, 0.0], Status::Incomplete),
        ([1.0, 2.0], Status::Incomplete),
        ([-2.0, -1.0], Status::Incomplete),
    ] {
        let mut f = Fixture::new();
        f.populations();
        f.value["populations"][0]["calibration"]["uncertainty_ms"] = json!(1.0);
        f.value["populations"][0]["expected_delay_ms"] = json!(expected);
        assert_eq!(f.load().unwrap().recording, status, "bounds {expected:?}");
    }
    let mut f = Fixture::new();
    f.populations();
    f.value["populations"][0]["calibration"]["uncertainty_ms"] = json!(1.0);
    f.value["populations"][0]["expected_ssim"] = json!([-1.0, 0.5]);
    assert_eq!(
        f.load().unwrap().recording,
        Status::Fail,
        "score failure survives delay overlap"
    );
}

#[test]
fn contradictory_scores_metadata_partial_populations_and_unsafe_sources_are_rejected() {
    for mutation in 0..8 {
        let mut f = Fixture::new();
        f.populations();
        match mutation {
            0 => f.value["populations"][1]["stream_hdr"]["max_cll_nits"] = json!(900.0),
            1 => f.value["populations"][0]["stream"]["frame_count"] = json!(2),
            2 => f.value["populations"][0]["reference"]["clock_domain"] = json!("other"),
            3 => f.value["populations"][2]["expected_ssim"] = json!([0.0, 1.0]),
            4 => f.value["artifacts"][0]["sha256"] = json!("0".repeat(64)),
            5 => f.value["artifacts"][0]["path"] = json!("../escape"),
            6 => f.artifact("scores-0.json", b"[]"),
            _ => {
                let mut rows: Value =
                    serde_json::from_slice(&std::fs::read(f.root.join("scores-0.json")).unwrap())
                        .unwrap();
                rows["ssim"] = json!(0.5);
                f.artifact("scores-0.json", &serde_json::to_vec(&rows).unwrap());
            }
        }
        assert!(f.load().is_err(), "mutation {mutation}");
    }
    let mut f = Fixture::new();
    f.populations();
    f.value["populations"][1]["source_hdr"] = Value::Null;
    assert_eq!(f.load().unwrap().recording, Status::Incomplete);
}

fn manifest() -> Value {
    json!({"schema_version":"1.0.0", "session_id":"session", "scenario_id":"scenario",
        "host_version":"host", "test_client_version":env!("CARGO_PKG_VERSION"),
        "test_client_revision":"test-revision", "official_client_version":"test-official",
        "profile_id":"reference_host_compatibility", "profile_version":"1.0.0",
        "profile_sha256":profile_digest(), "provenance":"reference_host",
        "artifacts":[], "populations":[], "reports":[]})
}

#[test]
fn missing_populations_and_reports_remain_incomplete() {
    let root = std::env::temp_dir().join(format!("ref-recording-{}", uuid::Uuid::new_v4()));
    std::fs::create_dir(&root).unwrap();
    let path = root.join("manifest.json");
    std::fs::write(&path, serde_json::to_vec(&manifest()).unwrap()).unwrap();
    let imported = load(&path, "session", "scenario", Some("host")).unwrap();
    assert_eq!(imported.recording, Status::Incomplete);
    assert_eq!(imported.report, Status::Incomplete);
    let mut wrong = manifest();
    wrong["profile_sha256"] = json!("0".repeat(64));
    std::fs::write(&path, serde_json::to_vec(&wrong).unwrap()).unwrap();
    assert!(load(&path, "session", "scenario", Some("host")).is_err());
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn source_integrity_symlinks_capacity_and_cross_run_binding_are_rejected() {
    let mut f = Fixture::new();
    f.populations();
    let path = f.root.join("manifest.json");
    f.load().unwrap();
    assert!(load(&path, "other", "scenario", Some("host")).is_err());
    assert!(load(&path, "session", "scenario", Some("other")).is_err());
    let bytes = std::fs::read(f.root.join("stream-0.raw")).unwrap();
    std::fs::write(f.root.join("stream-0.raw"), b"changed").unwrap();
    assert!(f.load().is_err());
    std::fs::write(f.root.join("stream-0.raw"), bytes).unwrap();
    #[cfg(unix)]
    {
        std::fs::rename(f.root.join("oracle.txt"), f.root.join("original.txt")).unwrap();
        std::os::unix::fs::symlink("original.txt", f.root.join("oracle.txt")).unwrap();
        assert!(f.load().is_err());
    }
    std::fs::write(
        &path,
        vec![b' '; moonlight_mcp::reference_recording::MAX_MANIFEST_BYTES + 1],
    )
    .unwrap();
    assert!(load(&path, "session", "scenario", Some("host")).is_err());
}

#[test]
fn cli_exposes_recording_binding_for_run_and_mcp_startup() {
    for command in ["run", "mcp"] {
        let output = std::process::Command::new(env!("CARGO_BIN_EXE_moonlight-mcp"))
            .args([command, "--help"])
            .output()
            .unwrap();
        assert!(output.status.success());
        assert!(String::from_utf8(output.stdout)
            .unwrap()
            .contains("--reference-recording"));
    }
}
