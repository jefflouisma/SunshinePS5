//! T015: generated pictures/timestamps only. No USB, HDMI, PS5, official-client or calibration
//! measurement is made by these tests. A fixture result cannot establish hardware readiness.
//! Activated: T018 (3 metadata/validity cases), T027 (1 clock mapping case),
//! T031 (1 P010 precision case), T033 (2 comparison cases). Replace absent test-only adapters
//! with production API calls and remove the corresponding ignore when the named task lands.
//! Sources: data-model.md Reference Recording; contracts/measurement-evidence.md Clock domains
//! and Fidelity/HDR integrity; FR-057/FR-061; research.md R4 (acquisition remains unverified).
//! Alignment uses small synthetic frames to keep fixtures bounded; the separate native SDR
//! and P010 fixtures explicitly use 1920×1080 without pretending to measure a real device.

use moonlight_mcp::compare::{compare, CompareOptions};
use moonlight_mcp::decode::yuv420_frame_size;
use moonlight_mcp::imgcmp::{psnr, ssim, Plane};
use moonlight_mcp::reference::{frames_path, write_index, Recording, RecordingIndex};
use serde_json::{json, Value};
use std::io::Write;
use std::path::{Path, PathBuf};

const WIDTH: usize = 1920;
const HEIGHT: usize = 1080;

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let dir = std::env::temp_dir()
            .join("moonlight-contract-fixtures")
            .join(format!("capture-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        Self(dir)
    }
    fn dir(&self, name: &str) -> PathBuf {
        let dir = self.0.join(name);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

fn frame(width: usize, height: usize, phase: u8) -> Vec<u8> {
    let mut pixels = vec![128; yuv420_frame_size(width, height)];
    for (i, value) in pixels[..width * height].iter_mut().enumerate() {
        *value = 16 + ((i % width * 3 + i / width * 5) % 32) as u8 + phase;
    }
    pixels
}

fn recording(dir: &Path, width: usize, height: usize, phases: &[u8], times: &[f64]) -> Recording {
    let mut file = std::fs::File::create(frames_path(dir)).unwrap();
    for &phase in phases {
        file.write_all(&frame(width, height, phase)).unwrap();
    }
    write_index(
        dir,
        &RecordingIndex {
            source: "synthetic fixture; no hardware evidence".into(),
            width,
            height,
            timestamps: times.to_vec(),
        },
    )
    .unwrap();
    Recording::load(dir).unwrap()
}

#[test]
fn native_sdr_geometry_and_pixels_survive_disk_roundtrip() {
    let scratch = Scratch::new();
    let rec = recording(
        &scratch.dir("native-sdr"),
        WIDTH,
        HEIGHT,
        &[0, 4],
        &[100.0, 100.0 + 1.0 / 60.0],
    );
    assert_eq!((rec.index.width, rec.index.height), (1920, 1080));
    assert_eq!(rec.frame_count(), 2);
    assert_eq!(rec.read_frame(0).unwrap(), frame(WIDTH, HEIGHT, 0));
    assert_eq!(rec.read_frame(1).unwrap().len(), WIDTH * HEIGHT * 3 / 2);
}

#[test]
fn native_sdr_identical_and_altered_pictures_have_expected_scores() {
    let a = frame(WIDTH, HEIGHT, 0);
    let b = frame(WIDTH, HEIGHT, 10);
    let a = Plane::new(&a[..WIDTH * HEIGHT], WIDTH, HEIGHT);
    let b = Plane::new(&b[..WIDTH * HEIGHT], WIDTH, HEIGHT);
    assert!((ssim(a, a) - 1.0).abs() < 1e-12);
    assert_eq!(psnr(a, a), f64::INFINITY);
    assert!(ssim(a, b) < 1.0);
    let expected = 10.0 * (255.0f64.powi(2) / 100.0).log10();
    assert!((psnr(a, b) - expected).abs() < 1e-9);
}

#[test]
fn missing_frame_or_timestamp_invalidates_recording() {
    let scratch = Scratch::new();
    for (frames, timestamps) in [(2, 1), (1, 2)] {
        let dir = scratch.dir(&format!("mismatch-{frames}-{timestamps}"));
        let mut file = std::fs::File::create(frames_path(&dir)).unwrap();
        for _ in 0..frames {
            file.write_all(&frame(64, 64, 0)).unwrap();
        }
        write_index(
            &dir,
            &RecordingIndex {
                source: "synthetic".into(),
                width: 64,
                height: 64,
                timestamps: (0..timestamps)
                    .map(|i| 100.0 + f64::from(i) / 60.0)
                    .collect(),
            },
        )
        .unwrap();
        assert!(
            Recording::load(&dir).is_err(),
            "{frames} frames / {timestamps} timestamps"
        );
    }
}

#[tokio::test]
async fn moving_scene_recovers_a_known_fifty_millisecond_shift() {
    let scratch = Scratch::new();
    let phases: Vec<u8> = (0..40).map(|i| i * 4).collect();
    let reference_times: Vec<f64> = (0..40).map(|i| 100.0 + f64::from(i) / 60.0).collect();
    let stream_times: Vec<f64> = reference_times[6..30].iter().map(|t| t + 0.050).collect();
    let reference = recording(&scratch.dir("reference"), 64, 64, &phases, &reference_times);
    let stream = recording(
        &scratch.dir("stream"),
        64,
        64,
        &phases[6..30],
        &stream_times,
    );
    let report = compare(
        &stream,
        &reference,
        &CompareOptions::default(),
        &scratch.dir("samples"),
    )
    .await
    .unwrap();
    assert!(report.alignment.reliable, "{:?}", report.alignment);
    assert!((report.alignment.delay_ms.unwrap() - 50.0).abs() <= 1000.0 / 120.0);
    assert_eq!(report.fidelity.as_ref().unwrap().frames_scored, 24);
    assert!((report.fidelity.unwrap().ssim.min - 1.0).abs() < 1e-12);
}

#[tokio::test]
async fn still_scene_has_valid_pixel_comparison_and_indeterminate_delay() {
    let scratch = Scratch::new();
    let times: Vec<f64> = (0..12).map(|i| 100.0 + f64::from(i) / 60.0).collect();
    let reference = recording(&scratch.dir("reference"), 64, 64, &[0; 12], &times);
    let stream = recording(&scratch.dir("stream"), 64, 64, &[0; 12], &times);
    let report = compare(
        &stream,
        &reference,
        &CompareOptions::default(),
        &scratch.dir("samples"),
    )
    .await
    .unwrap();
    assert_eq!(report.alignment.delay_ms, None);
    assert!(!report.alignment.reliable);
    assert_eq!(report.fidelity.as_ref().unwrap().frames_scored, 12);
    assert!((report.fidelity.unwrap().ssim.min - 1.0).abs() < 1e-12);
    // T035's required-delay criterion must be Incomplete even though these pictures match.
}

fn native_metadata(hdr: bool) -> Value {
    json!({"source":"synthetic-fixture","capture_mode":{"width":WIDTH,"height":HEIGHT,"fps":60},
        "calibration_id":null,"calibration_reason":"synthetic fixtures are not calibrated hardware",
        "pixel_format":if hdr {"p010le"} else {"yuv420p"},"bit_depth":if hdr {10} else {8},
        "color_space":if hdr {"PQ/BT.2020"} else {"BT.709"},
        "clock_domain":"test-machine-monotonic-fixture","frame_count":2,"timestamp_count":2,
        "timestamps":[100000000,100016667],"validity":"valid",
        "metadata_evidence":null})
}

// Deliberate test-only absent adapters approved for the pre-implementation TDD backlog.
// No production API exists yet; activating tasks must wire real models/helpers/comparison.
fn recording_metadata_from_client(metadata: &Value) -> Option<Value> {
    let recording: moonlight_mcp::recording::Recording =
        serde_json::from_value(metadata.clone()).ok()?;
    recording.validate().ok()?;
    serde_json::to_value(recording).ok()
}
fn recording_validity_from_client(metadata: &Value) -> Option<bool> {
    let recording: moonlight_mcp::recording::Recording =
        serde_json::from_value(metadata.clone()).ok()?;
    Some(recording.validate().is_ok())
}
fn clock_alignment_from_client(stream: &Value, reference: &Value) -> Option<Value> {
    let a = serde_json::from_value(stream.clone()).ok()?;
    let b = serde_json::from_value(reference.clone()).ok()?;
    serde_json::to_value(moonlight_mcp::clock::alignment_validity(&a, &b)).ok()
}
fn hdr_roundtrip_from_client(metadata: &Value, bytes: &[u8]) -> Option<Vec<u8>> {
    let mut recording: moonlight_mcp::recording::Recording =
        serde_json::from_value(metadata.clone()).ok()?;
    moonlight_mcp::hdr_capture::validate_p010_frame(&recording, bytes).ok()?;
    let scratch = Scratch::new();
    let dir = scratch.dir("native-hdr");
    recording.frames = Some(moonlight_mcp::evidence::ArtifactRef("frames.p010".into()));
    std::fs::write(dir.join("frames.p010"), bytes).ok()?;
    std::fs::write(
        dir.join("recording.json"),
        serde_json::to_vec(&recording).ok()?,
    )
    .ok()?;
    let index = RecordingIndex {
        source: recording.source.clone(),
        width: WIDTH,
        height: HEIGHT,
        timestamps: recording
            .timestamps
            .as_ref()?
            .iter()
            .map(|us| *us as f64 / 1_000_000.0)
            .collect(),
    };
    std::fs::write(dir.join("frames.json"), serde_json::to_vec(&index).ok()?).ok()?;
    Recording::load(&dir).ok()?.read_frame(0).ok()
}
fn hdr_scores_from_client(a: &[u16], b: &[u16]) -> Option<Value> {
    serde_json::to_value(
        moonlight_mcp::compare::score_plane(
            a,
            b,
            WIDTH,
            HEIGHT,
            moonlight_mcp::recording::ColorSpace::PqBt2020,
        )
        .ok()?,
    )
    .ok()
}

#[test]
fn native_recording_metadata_does_not_infer_source_hdr_metadata_or_calibration() {
    for hdr in [false, true] {
        let expected = native_metadata(hdr);
        assert_eq!(recording_metadata_from_client(&expected), Some(expected));
    }
}

#[test]
fn declared_count_mismatches_and_absent_timestamps_are_invalid() {
    for count in [0, 1, 3] {
        let mut metadata = native_metadata(false);
        metadata["timestamp_count"] = json!(count);
        metadata["timestamps"] = json!((0..count)
            .map(|i| 100000000 + i * 16667)
            .collect::<Vec<_>>());
        assert_eq!(recording_validity_from_client(&metadata), Some(false));
    }
    let mut metadata = native_metadata(true);
    metadata["timestamps"] = Value::Null;
    assert_eq!(recording_validity_from_client(&metadata), Some(false));
}

#[test]
fn reversed_timestamps_cannot_be_used_as_elapsed_time() {
    // The native model and T029 reference-loader regression both reject reversed timestamps.
    let mut metadata = native_metadata(false);
    metadata["timestamps"] = json!([100000000, 99000000]);
    assert_eq!(
        recording_validity_from_client(&metadata),
        Some(false),
        "reversed timestamps must invalidate alignment"
    );
}

#[test]
fn native_validity_does_not_establish_calibration_metadata_or_clock_mapping() {
    use moonlight_mcp::evidence::ArtifactRef;
    use moonlight_mcp::recording::Recording as NativeRecording;
    let mut hdr: NativeRecording = serde_json::from_value(native_metadata(true)).unwrap();
    hdr.frames = Some(ArtifactRef("frames.p010".into()));
    hdr.validate().unwrap();
    assert!(hdr.metadata_evidence.is_none());
    assert_eq!(hdr.checked_frame_bytes().unwrap(), 1920 * 1080 * 3);
    assert!(hdr
        .validate_for(&hdr.capture_mode, true, &hdr.clock_domain, false)
        .is_ok());
    assert!(hdr
        .validate_for(&hdr.capture_mode, true, &hdr.clock_domain, true)
        .is_err());
    assert!(hdr
        .validate_for(&hdr.capture_mode, true, "helper-private-epoch", false)
        .is_err());
    assert!(hdr
        .validate_for(&hdr.capture_mode, false, &hdr.clock_domain, false)
        .is_err());
    hdr.bit_depth = 8;
    assert!(hdr.validate().is_err());
}

#[test]
fn cross_process_clock_mapping_never_subtracts_unrelated_epochs() {
    let stream =
        json!({"domain":"machine-monotonic","epoch":"boot-A","timestamps_us":[100000,116667]});
    let matching = stream.clone();
    assert_eq!(
        clock_alignment_from_client(&stream, &matching),
        Some(json!({"valid":true,"reason":null}))
    );
    for reference in [
        json!({"domain":"wall-clock","epoch":"unix","timestamps_us":[100000,116667]}),
        json!({"domain":"helper-private-clock","epoch":"helper-start","timestamps_us":[100000,116667]}),
        json!({"domain":"machine-monotonic","epoch":"boot-B","timestamps_us":[100000,116667]}),
        json!({"domain":"machine-monotonic","epoch":"boot-A","timestamps_us":[100000,99999]}),
    ] {
        let result = clock_alignment_from_client(&stream, &reference);
        assert_eq!(
            result.as_ref().and_then(|v| v.get("valid")),
            Some(&json!(false))
        );
        assert!(result.unwrap()["reason"]
            .as_str()
            .is_some_and(|s| !s.is_empty()));
    }
}

#[test]
fn native_p010_keeps_low_two_signal_bits() {
    let mut metadata = native_metadata(true);
    metadata["frame_count"] = json!(1);
    metadata["timestamp_count"] = json!(1);
    metadata["timestamps"] = json!([100000000]);
    let samples: Vec<u16> = (0..WIDTH * HEIGHT * 3 / 2)
        .map(|i| ((i % 1024) as u16) << 6)
        .collect();
    let bytes: Vec<u8> = samples.iter().flat_map(|v| v.to_le_bytes()).collect();
    assert_eq!(bytes.len(), WIDTH * HEIGHT * 3);
    // Signal values 512 and 513 differ in ten-bit data but collapse to the same eight-bit code.
    assert_ne!(samples[512], samples[513]);
    assert_eq!(samples[512] >> 8, samples[513] >> 8);
    let actual = hdr_roundtrip_from_client(&metadata, &bytes)
        .expect("native P010 recording is not implemented");
    assert_eq!(actual.len(), bytes.len());
    assert!(
        actual == bytes,
        "P010 samples or their low two signal bits changed"
    );
}

#[test]
fn hdr_identical_and_low_bit_altered_pictures_are_distinguishable() {
    let a = vec![512u16; WIDTH * HEIGHT];
    let b = vec![513u16; WIDTH * HEIGHT];
    let identical = hdr_scores_from_client(&a, &a);
    assert_eq!(
        identical.as_ref().and_then(|v| v.get("ssim")),
        Some(&json!(1.0))
    );
    let changed = hdr_scores_from_client(&a, &b).expect("ten-bit comparison must exist");
    assert!(changed["ssim"].as_f64().unwrap() < 1.0);
    let expected = 10.0 * (1023.0f64.powi(2)).log10();
    assert!((changed["psnr_db"].as_f64().unwrap() - expected).abs() < 1e-9);
    assert_eq!(changed["color_space"], "PQ/BT.2020");
}

#[tokio::test]
async fn full_population_has_no_default_three_hundred_frame_sampling_cap() {
    let scratch = Scratch::new();
    let times: Vec<f64> = (0..304).map(|i| 100.0 + f64::from(i) / 60.0).collect();
    let reference = recording(&scratch.dir("reference"), 64, 64, &[0; 304], &times);
    let stream = recording(&scratch.dir("stream"), 64, 64, &[0; 304], &times);
    let report = compare(
        &stream,
        &reference,
        &CompareOptions::default(),
        &scratch.dir("samples"),
    )
    .await
    .unwrap();
    assert_eq!(report.fidelity.unwrap().frames_scored, 304);
}
