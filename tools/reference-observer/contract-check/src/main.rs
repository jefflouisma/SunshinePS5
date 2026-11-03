//! Synthetic Python publisher output must remain Incomplete in the real consumers.
use moonlight_mcp::measurements::import_host;
use moonlight_mcp::reference_lifecycle;
use moonlight_mcp::report::Status;
use moonlight_mcp::runner::controller_outcome;
use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(std::env::args().nth(1).expect("fixture directory"));
    let bytes = std::fs::read(root.join("host.jsonl")).unwrap();
    let rows = import_host(std::io::Cursor::new(bytes), "session", 128).unwrap();
    for row in &rows {
        row.validate("session").unwrap();
        assert!(row.validate("foreign-session").is_err());
        assert_eq!(row.authenticated, Some(false));
    }
    let expected = [
        (0, "connect"),
        (0, "deflect"),
        (0, "neutral"),
        (0, "disconnect"),
    ];
    assert_eq!(controller_outcome(&expected, &rows).0, Status::Incomplete);
    let manifest = root.join("manifest.json");
    let imported =
        reference_lifecycle::load(&manifest, "session", "scenario", Some("fixture-host")).unwrap();
    assert_eq!(imported.outcome("REF-pairing").0, Status::Incomplete);
    assert_eq!(imported.outcome("REF-teardown").0, Status::Incomplete);
    assert_eq!(imported.artifacts.len(), 2);
    for name in ["raw.json", "host.jsonl"] {
        let path = root.join(name);
        let original = std::fs::read(&path).unwrap();
        let retained = imported
            .artifacts
            .iter()
            .find(|(r, _)| r.0 == PathBuf::from(name))
            .unwrap();
        assert_eq!(retained.1, original);
        let mut modified = original.clone();
        modified.push(b'\n');
        std::fs::write(&path, modified).unwrap();
        let rejected =
            reference_lifecycle::load(&manifest, "session", "scenario", Some("fixture-host"));
        std::fs::write(&path, &original).unwrap();
        let error = match rejected {
            Err(error) => error,
            Ok(_) => panic!("tampered artifact was accepted: {name}"),
        };
        assert!(error.to_string().contains("checksum mismatch"));
        assert_eq!(retained.1, original);
    }
    reference_lifecycle::load(&manifest, "session", "scenario", Some("fixture-host")).unwrap();
    assert!(
        reference_lifecycle::load(&manifest, "foreign", "scenario", Some("fixture-host")).is_err()
    );
    println!("PASS: real Rust HostObservation/lifecycle consumers accept fixture; controller, pairing and teardown remain Incomplete; wrong session and tampering of either bound artifact rejected; original files restored and revalidated");
}
