//! Synthetic records exercise the importer; they are never hardware acceptance evidence.
use moonlight_mcp::reference_lifecycle::{load, MAX_MANIFEST_BYTES};
use moonlight_mcp::report::Status;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::path::PathBuf;

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let root =
            std::env::temp_dir().join(format!("reference-lifecycle-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&root).unwrap();
        std::fs::write(root.join("observer.txt"), b"SYNTHETIC TEST OBSERVER\n").unwrap();
        Self(root)
    }
    fn manifest(&self) -> Value {
        json!({
            "schema_version":"1.0.0", "session_id":"session", "scenario_id":"scenario",
            "host_version":"host-version", "test_client_version":env!("CARGO_PKG_VERSION"),
            "test_client_revision":"synthetic-revision", "official_client_version":"synthetic-official-version",
            "provenance":"reference_host",
            "artifacts":[{"path":"observer.txt", "sha256":hex::encode(Sha256::digest(b"SYNTHETIC TEST OBSERVER\n"))}],
            "observations":[
                {"check":"pairing_proof_flow", "satisfied":true, "validity":"valid", "reason":null, "artifacts":["observer.txt"]},
                {"check":"pairing_invalid_proofs_rejected", "satisfied":true, "validity":"valid", "reason":null, "artifacts":["observer.txt"]},
                {"check":"pairing_official_client_cross_check", "satisfied":true, "validity":"valid", "reason":null, "artifacts":["observer.txt"]},
                {"check":"teardown_session_terminated", "satisfied":true, "validity":"valid", "reason":null, "artifacts":["observer.txt"]},
                {"check":"teardown_controllers_released", "satisfied":true, "validity":"valid", "reason":null, "artifacts":["observer.txt"]},
                {"check":"teardown_resources_reclaimed", "satisfied":true, "validity":"valid", "reason":null, "artifacts":["observer.txt"]}
            ]
        })
    }
    fn write(&self, value: &Value) -> PathBuf {
        let path = self.0.join("observations.json");
        std::fs::write(&path, serde_json::to_vec(value).unwrap()).unwrap();
        path
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

#[test]
fn complete_correlated_observations_require_every_check_and_retain_verified_bytes() {
    let fixture = Fixture::new();
    let mut manifest = fixture.manifest();
    let imported = load(
        &fixture.write(&manifest),
        "session",
        "scenario",
        Some("host-version"),
    )
    .unwrap();
    assert_eq!(imported.outcome("REF-pairing").0, Status::Pass);
    assert_eq!(imported.outcome("REF-teardown").0, Status::Pass);
    assert_eq!(imported.artifacts[0].1, b"SYNTHETIC TEST OBSERVER\n");
    manifest["observations"].as_array_mut().unwrap().pop();
    let partial = load(
        &fixture.write(&manifest),
        "session",
        "scenario",
        Some("host-version"),
    )
    .unwrap();
    assert_eq!(partial.outcome("REF-pairing").0, Status::Pass);
    assert_eq!(partial.outcome("REF-teardown").0, Status::Incomplete);
    assert_eq!(partial.outcome("REF-recording").0, Status::Incomplete);
    assert_eq!(partial.outcome("REF-report").0, Status::Incomplete);
}

#[test]
fn observed_failure_survives_missing_checks_and_foreign_provenance_cannot_establish_acceptance() {
    let fixture = Fixture::new();
    let mut manifest = fixture.manifest();
    manifest["observations"].as_array_mut().unwrap().truncate(1);
    manifest["observations"][0]["satisfied"] = json!(false);
    let imported = load(
        &fixture.write(&manifest),
        "session",
        "scenario",
        Some("host-version"),
    )
    .unwrap();
    assert_eq!(imported.outcome("REF-pairing").0, Status::Fail);
    for provenance in [
        "synthetic_fixture",
        "console_hardware",
        "operator_observation",
    ] {
        manifest["provenance"] = json!(provenance);
        let imported = load(
            &fixture.write(&manifest),
            "session",
            "scenario",
            Some("host-version"),
        )
        .unwrap();
        assert_eq!(imported.outcome("REF-pairing").0, Status::Incomplete);
    }
}

#[test]
fn absent_invalid_or_uncorrelated_observations_cannot_pass() {
    let fixture = Fixture::new();
    for field in [
        "session_id",
        "scenario_id",
        "host_version",
        "test_client_version",
    ] {
        let mut manifest = fixture.manifest();
        manifest[field] = json!("other");
        assert!(
            load(
                &fixture.write(&manifest),
                "session",
                "scenario",
                Some("host-version")
            )
            .is_err(),
            "{field}"
        );
    }
    assert!(load(
        &fixture.write(&fixture.manifest()),
        "session",
        "scenario",
        None
    )
    .is_err());
    for validity in ["invalid", "unavailable"] {
        let mut manifest = fixture.manifest();
        manifest["observations"][0]["satisfied"] = Value::Null;
        manifest["observations"][0]["validity"] = json!(validity);
        manifest["observations"][0]["reason"] = json!("synthetic missing evidence");
        let imported = load(
            &fixture.write(&manifest),
            "session",
            "scenario",
            Some("host-version"),
        )
        .unwrap();
        assert_eq!(imported.outcome("REF-pairing").0, Status::Incomplete);
        manifest["observations"][0]["satisfied"] = json!(true);
        assert!(load(
            &fixture.write(&manifest),
            "session",
            "scenario",
            Some("host-version")
        )
        .is_err());
    }
}

#[test]
fn corrupt_missing_unbound_or_duplicate_artifacts_are_rejected() {
    let fixture = Fixture::new();
    for mutate in 0..6 {
        let mut manifest = fixture.manifest();
        match mutate {
            0 => manifest["artifacts"][0]["sha256"] = json!("0".repeat(64)),
            1 => manifest["artifacts"][0]["path"] = json!("absent.txt"),
            2 => manifest["observations"][0]["artifacts"] = json!(["unbound.txt"]),
            3 => {
                let duplicate = manifest["observations"][0].clone();
                manifest["observations"]
                    .as_array_mut()
                    .unwrap()
                    .push(duplicate);
            }
            4 => {
                let duplicate = manifest["artifacts"][0].clone();
                manifest["artifacts"]
                    .as_array_mut()
                    .unwrap()
                    .push(duplicate);
            }
            _ => manifest["observations"][0]["artifacts"] = json!([]),
        }
        assert!(
            load(
                &fixture.write(&manifest),
                "session",
                "scenario",
                Some("host-version")
            )
            .is_err(),
            "mutation {mutate}"
        );
    }
}

#[test]
fn manifest_artifacts_and_paths_are_bounded() {
    let fixture = Fixture::new();
    let path = fixture.write(&fixture.manifest());
    std::fs::write(&path, vec![b' '; MAX_MANIFEST_BYTES + 1]).unwrap();
    assert!(load(&path, "session", "scenario", Some("host-version")).is_err());
    for path in ["../observer.txt", "/etc/passwd", ""] {
        let mut manifest = fixture.manifest();
        manifest["artifacts"][0]["path"] = json!(path);
        assert!(load(
            &fixture.write(&manifest),
            "session",
            "scenario",
            Some("host-version")
        )
        .is_err());
    }
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink("observer.txt", fixture.0.join("linked.txt")).unwrap();
        let mut manifest = fixture.manifest();
        manifest["artifacts"][0]["path"] = json!("linked.txt");
        assert!(load(
            &fixture.write(&manifest),
            "session",
            "scenario",
            Some("host-version")
        )
        .is_err());
    }
}
