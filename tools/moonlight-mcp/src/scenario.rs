//! Immutable declarations for scenario.schema.json v1.0.0. Parsing never contacts a host,
//! resolves a secret, creates output, or removes a criterion whose evidence is unavailable.

use anyhow::{bail, ensure, Context, Result};
use serde::Deserialize;
use serde_json::Value;
use std::collections::HashSet;
use std::io::Read;
use std::path::{Component, Path, PathBuf};

/// Bounds input before serde allocation; four controllers can each declare 65,536 actions.
pub const MAX_SCENARIO_BYTES: u64 = 128 * 1024 * 1024;
const PROFILES: &str = include_str!("../../../tests/scenarios/profiles/acceptance.json");

#[derive(Debug, Clone, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Mode {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate_kbps: u32,
    pub codec: Codec,
    pub dynamic_range: DynamicRange,
}
#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Codec {
    H264,
    Hevc,
    Av1,
}
#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum DynamicRange {
    Sdr,
    Hdr,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Pairing {
    pub pin_delivery: PinStrategy,
    pub provider_ref: Option<String>,
}
#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum PinStrategy {
    AlreadyPaired,
    Command,
    Sunshine,
    Wolf,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Controller {
    pub id: u8,
    pub actions: Vec<Action>,
}
#[derive(Debug, Clone, Deserialize)]
pub struct Action {
    pub event_id: String,
    pub at_seconds: f64,
    pub expected_delivery: Option<bool>,
    pub expected_response: Option<String>,
    #[serde(flatten)]
    pub kind: ActionKind,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum ActionKind {
    Connect {},
    Disconnect {},
    Button {
        button: Button,
        pressed: bool,
    },
    Stick {
        stick: Side,
        x: f64,
        y: f64,
    },
    Trigger {
        trigger: Side,
        value: f64,
    },
    Motion {
        sensor: Sensor,
        x: f64,
        y: f64,
        z: f64,
    },
    Touchpad {
        contact_id: u8,
        phase: TouchPhase,
        x: f64,
        y: f64,
    },
    ExpectRumble {
        minimum_amplitude: f64,
        timeout_seconds: f64,
    },
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Side {
    Left,
    Right,
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Sensor {
    Accel,
    Gyro,
}
#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TouchPhase {
    Down,
    Move,
    Up,
    Cancel,
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Button {
    Cross,
    Circle,
    Square,
    Triangle,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    L1,
    R1,
    L3,
    R3,
    Options,
    Share,
    Home,
    Touchpad,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Reference {
    pub required: bool,
    pub device_ref: Option<String>,
    pub capture_mode: Option<RecordingMode>,
    pub calibration_id: Option<String>,
    pub calibration_reason: Option<String>,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecordingMode {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub dynamic_range: DynamicRange,
    pub pixel_format: String,
    pub bit_depth: u8,
    pub color_space: String,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RequiredCriteria {
    pub profile_id: String,
    pub profile_version: String,
    pub ids: Vec<String>,
    #[serde(default)]
    pub declared_exclusions: Vec<String>,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EvidenceSources {
    pub host_metrics: bool,
    pub input_delivery: bool,
    pub reference_recording: bool,
    pub official_client_observations: bool,
    pub operator_observations: bool,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WritePolicy {
    pub max_bytes: u64,
    pub max_files: u32,
    pub on_limit: String,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Declaration {
    schema_version: String,
    id: String,
    host: String,
    pairing: Pairing,
    requested: Mode,
    duration_seconds: f64,
    controllers: Vec<Controller>,
    reference: Reference,
    required_criteria: RequiredCriteria,
    evidence: EvidenceSources,
    output_dir: PathBuf,
    write_policy: WritePolicy,
}

/// Validated fields are exposed by shared reference so a run cannot mutate its declaration.
#[derive(Debug, Clone)]
pub struct Scenario {
    original: Value,
    declaration: Declaration,
}
impl Scenario {
    pub fn from_reader(reader: impl Read) -> Result<Self> {
        let mut bytes = Vec::new();
        reader
            .take(MAX_SCENARIO_BYTES + 1)
            .read_to_end(&mut bytes)?;
        ensure!(
            bytes.len() as u64 <= MAX_SCENARIO_BYTES,
            "scenario exceeds input limit"
        );
        Self::from_value(serde_json::from_slice(&bytes)?)
    }
    pub fn from_value(original: Value) -> Result<Self> {
        let declaration: Declaration = serde_json::from_value(original.clone())
            .context("scenario does not match schema v1.0.0")?;
        let scenario = Self {
            original,
            declaration,
        };
        scenario.validate()?;
        Ok(scenario)
    }
    pub fn as_value(&self) -> &Value {
        &self.original
    }
    pub fn matches_declaration(&self, candidate: &Value) -> bool {
        self.original == *candidate
    }
    pub fn id(&self) -> &str {
        &self.declaration.id
    }
    pub fn host(&self) -> &str {
        &self.declaration.host
    }
    pub fn pairing(&self) -> &Pairing {
        &self.declaration.pairing
    }
    pub fn requested(&self) -> &Mode {
        &self.declaration.requested
    }
    pub fn duration_seconds(&self) -> f64 {
        self.declaration.duration_seconds
    }
    pub fn controllers(&self) -> &[Controller] {
        &self.declaration.controllers
    }
    pub fn reference(&self) -> &Reference {
        &self.declaration.reference
    }
    pub fn required_criteria(&self) -> &RequiredCriteria {
        &self.declaration.required_criteria
    }
    pub fn evidence(&self) -> &EvidenceSources {
        &self.declaration.evidence
    }
    pub fn write_policy(&self) -> &WritePolicy {
        &self.declaration.write_policy
    }

    /// Resolve relative to an explicitly selected existing output root. Existing symlinks are
    /// rejected, including dangling ones; writers must recheck before creating files (T028).
    pub fn resolve_output(&self, root: &Path) -> Result<PathBuf> {
        let root = root
            .canonicalize()
            .context("output root must already exist")?;
        let mut output = root;
        for part in self.declaration.output_dir.components() {
            output.push(part);
            match std::fs::symlink_metadata(&output) {
                Ok(meta) => ensure!(
                    meta.is_dir() && !meta.file_type().is_symlink(),
                    "output path contains a symlink or non-directory"
                ),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
        }
        Ok(output)
    }
    fn validate(&self) -> Result<()> {
        let d = &self.declaration;
        reject_null_optional(&self.original["pairing"], &["provider_ref"])?;
        reject_null_optional(&self.original["reference"], &["device_ref", "capture_mode"])?;
        reject_null_optional(
            &self.original["required_criteria"],
            &["declared_exclusions"],
        )?;
        for controller in self.original["controllers"].as_array().unwrap() {
            for action in controller["actions"].as_array().unwrap() {
                reject_null_optional(action, &["expected_delivery", "expected_response"])?;
            }
        }
        ensure!(d.schema_version == "1.0.0", "unsupported scenario version");
        id(&d.id)?;
        ensure!(
            !d.host.is_empty()
                && d.host.len() <= 253
                && d.host
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b".:%_-".contains(&b)),
            "invalid host address"
        );
        match d.pairing.pin_delivery {
            PinStrategy::AlreadyPaired => ensure!(
                !self.original["pairing"]
                    .as_object()
                    .unwrap()
                    .contains_key("provider_ref"),
                "already_paired cannot declare a provider"
            ),
            _ => id(d
                .pairing
                .provider_ref
                .as_deref()
                .context("PIN strategy requires provider_ref")?)?,
        }
        let mode = &d.requested;
        ensure!(
            mode.width > 0 && mode.height > 0 && mode.fps > 0 && mode.bitrate_kbps > 0,
            "mode values must be positive u32 values"
        );
        ensure!(
            d.duration_seconds.is_finite()
                && d.duration_seconds > 0.0
                && d.duration_seconds <= u32::MAX as f64,
            "invalid duration"
        );
        ensure!(d.controllers.len() <= 4, "at most four controllers");
        let mut controller_ids = HashSet::new();
        let mut event_ids = HashSet::new();
        for controller in &d.controllers {
            ensure!(
                controller.id <= 3 && controller_ids.insert(controller.id),
                "invalid or duplicate controller ID"
            );
            ensure!(
                controller.actions.len() <= 65536,
                "too many controller actions"
            );
            let (mut previous, mut connected) = (0.0, false);
            let mut contacts = HashSet::new();
            for action in &controller.actions {
                id(&action.event_id)?;
                ensure!(event_ids.insert(&action.event_id), "duplicate event ID");
                ensure!(
                    action.at_seconds.is_finite()
                        && action.at_seconds >= previous
                        && action.at_seconds <= d.duration_seconds,
                    "actions must be ordered within duration"
                );
                previous = action.at_seconds;
                if let Some(response) = &action.expected_response {
                    reason(response)?;
                }
                match &action.kind {
                    ActionKind::Connect {} => {
                        ensure!(!connected, "controller already connected");
                        connected = true;
                    }
                    ActionKind::Disconnect {} => {
                        ensure!(connected, "controller is disconnected");
                        connected = false;
                        contacts.clear();
                    }
                    kind => {
                        ensure!(connected, "controller action before connect");
                        match kind {
                            ActionKind::Stick { x, y, .. } => {
                                range(*x, -1.0, 1.0)?;
                                range(*y, -1.0, 1.0)?;
                            }
                            ActionKind::Trigger { value, .. } => range(*value, 0.0, 1.0)?,
                            ActionKind::Motion { x, y, z, .. } => {
                                ensure!([x, y, z].iter().all(|n| n.is_finite()), "nonfinite motion")
                            }
                            ActionKind::Touchpad {
                                contact_id,
                                phase,
                                x,
                                y,
                            } => {
                                range(*x, 0.0, 1.0)?;
                                range(*y, 0.0, 1.0)?;
                                match phase {
                                    TouchPhase::Down => ensure!(
                                        contacts.insert(*contact_id),
                                        "touch contact already down"
                                    ),
                                    TouchPhase::Move => ensure!(
                                        contacts.contains(contact_id),
                                        "move without touch down"
                                    ),
                                    TouchPhase::Up | TouchPhase::Cancel => ensure!(
                                        contacts.remove(contact_id),
                                        "release without touch down"
                                    ),
                                }
                            }
                            ActionKind::ExpectRumble {
                                minimum_amplitude,
                                timeout_seconds,
                            } => {
                                range(*minimum_amplitude, 0.0, 1.0)?;
                                ensure!(
                                    timeout_seconds.is_finite()
                                        && *timeout_seconds > 0.0
                                        && *timeout_seconds
                                            <= d.duration_seconds - action.at_seconds,
                                    "rumble timeout outside run"
                                );
                            }
                            _ => {}
                        }
                    }
                }
            }
        }
        self.validate_reference()?;
        let profiles: Value = serde_json::from_str(PROFILES)?;
        let criteria = &d.required_criteria;
        ensure!(
            criteria.profile_version == profiles["profile_version"],
            "unsupported profile version"
        );
        let profile = profiles["profiles"]
            .get(&criteria.profile_id)
            .context("unknown profile")?;
        let expected: Vec<String> = serde_json::from_value(profile["required_criteria"].clone())?;
        let actual: HashSet<_> = criteria.ids.iter().collect();
        ensure!(
            actual.len() == criteria.ids.len() && actual == expected.iter().collect(),
            "required criterion set differs from fixed profile"
        );
        let exclusions: Vec<String> =
            serde_json::from_value(profile["declared_exclusions"].clone())?;
        ensure!(
            criteria.declared_exclusions == exclusions,
            "profile exclusions changed"
        );
        ensure!(
            d.write_policy.max_bytes > 0
                && d.write_policy.max_files > 0
                && d.write_policy.on_limit == "incomplete",
            "invalid write policy"
        );
        let path = d.output_dir.to_str().context("output path must be UTF-8")?;
        ensure!(
            !path.is_empty()
                && path.len() <= 1024
                && !path.contains(['\\', ':'])
                && path
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b"/_.-".contains(&b)),
            "invalid local output path"
        );
        ensure!(
            d.output_dir
                .components()
                .all(|part| matches!(part, Component::Normal(_))),
            "output path must be relative without traversal"
        );
        ensure!(
            !path
                .split('/')
                .any(|part| part.is_empty() || part == "." || part == ".."),
            "invalid output path component"
        );
        Ok(())
    }
    fn validate_reference(&self) -> Result<()> {
        let r = &self.declaration.reference;
        for value in [&r.device_ref, &r.calibration_id].into_iter().flatten() {
            id(value)?;
        }
        if let Some(reason_text) = &r.calibration_reason {
            reason(reason_text)?;
        }
        let raw = self.original["reference"].as_object().unwrap();
        if r.required {
            for field in [
                "device_ref",
                "capture_mode",
                "calibration_id",
                "calibration_reason",
            ] {
                ensure!(raw.contains_key(field), "required reference lacks {field}");
            }
            ensure!(
                r.device_ref.is_some() && r.capture_mode.is_some(),
                "required reference lacks device or mode"
            );
        }
        if raw.get("calibration_id").is_some_and(Value::is_null) {
            ensure!(
                r.calibration_reason.is_some(),
                "absent calibration requires a reason"
            );
        }
        if let Some(capture) = &r.capture_mode {
            id(&capture.pixel_format)?;
            ensure!(
                capture.width > 0 && capture.height > 0 && capture.fps > 0,
                "invalid reference geometry"
            );
            let (depth, color) = match capture.dynamic_range {
                DynamicRange::Sdr => (8, "BT.709"),
                DynamicRange::Hdr => (10, "PQ/BT.2020"),
            };
            ensure!(
                capture.bit_depth == depth && capture.color_space == color,
                "reference precision or color does not match dynamic range"
            );
            let mode = &self.declaration.requested;
            ensure!(
                (
                    capture.width,
                    capture.height,
                    capture.fps,
                    capture.dynamic_range
                ) == (mode.width, mode.height, mode.fps, mode.dynamic_range),
                "reference must preserve requested native geometry and dynamic range"
            );
        }
        Ok(())
    }
}
fn id(value: &str) -> Result<()> {
    ensure!(
        value
            .as_bytes()
            .first()
            .is_some_and(u8::is_ascii_alphanumeric)
            && value.len() <= 128
            && value
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b)),
        "invalid identifier"
    );
    Ok(())
}
fn reason(value: &str) -> Result<()> {
    ensure!(
        !value.is_empty()
            && value.chars().count() <= 1024
            && value.chars().any(|c| !c.is_whitespace()),
        "invalid reason"
    );
    Ok(())
}
fn range(value: f64, min: f64, max: f64) -> Result<()> {
    if !value.is_finite() || !(min..=max).contains(&value) {
        bail!("controller value outside range");
    }
    Ok(())
}

fn reject_null_optional(value: &Value, fields: &[&str]) -> Result<()> {
    for field in fields {
        ensure!(
            !value.get(field).is_some_and(Value::is_null),
            "{field} cannot be null"
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn fixture() -> Value {
        json!({"schema_version":"1.0.0","id":"scenario-test","host":"127.0.0.1",
            "pairing":{"pin_delivery":"already_paired"},
            "requested":{"width":1920,"height":1080,"fps":60,"bitrate_kbps":20000,"codec":"h264","dynamic_range":"sdr"},
            "duration_seconds":1,"controllers":[{"id":0,"actions":[
                {"event_id":"connect","at_seconds":0,"type":"connect"},
                {"event_id":"button","at_seconds":0.1,"type":"button","button":"cross","pressed":true},
                {"event_id":"disconnect","at_seconds":1,"type":"disconnect"}]}],
            "reference":{"required":false},
            "required_criteria":{"profile_id":"reference_host_compatibility","profile_version":"1.0.0",
                "ids":["REF-pairing","REF-stream","REF-controller","REF-teardown","REF-recording","REF-report"]},
            "evidence":{"host_metrics":false,"input_delivery":false,"reference_recording":false,
                "official_client_observations":false,"operator_observations":false},
            "output_dir":"artifacts/run","write_policy":{"max_bytes":1048576,"max_files":100,"on_limit":"incomplete"}})
    }
    #[test]
    fn valid_declaration_round_trips_without_filling_missing_evidence() {
        let value = fixture();
        let scenario =
            Scenario::from_reader(serde_json::to_vec(&value).unwrap().as_slice()).unwrap();
        assert_eq!(scenario.as_value(), &value);
        assert_eq!(scenario.required_criteria().ids.len(), 6);
        assert!(!scenario.evidence().host_metrics);
        for (profile, exclusions) in [
            (
                "ps5_release_first",
                vec!["SC-014.prior_release_median_regression"],
            ),
            ("ps5_release_subsequent", vec![]),
        ] {
            let mut value = fixture();
            value["required_criteria"] = json!({"profile_id":profile,"profile_version":"1.0.0",
                "ids":(1..=14).map(|n| format!("SC-{n:03}")).collect::<Vec<_>>(),"declared_exclusions":exclusions});
            assert!(Scenario::from_value(value).is_ok());
        }
    }
    #[test]
    fn rejects_schema_and_relational_errors() {
        for (pointer, replacement) in [
            ("/schema_version", json!("2.0.0")),
            ("/id", json!("-invalid")),
            ("/host", json!("https://host")),
            ("/duration_seconds", json!(0)),
            ("/requested/width", json!(0)),
            ("/requested/bitrate_kbps", json!(-1)),
            ("/requested/codec", json!("vp9")),
            ("/pairing/pin_delivery", json!("manual")),
            (
                "/pairing",
                json!({"pin_delivery":"command","provider_ref":"shell $(bad)"}),
            ),
            ("/pairing", json!({"pin_delivery":"wolf"})),
            (
                "/pairing",
                json!({"pin_delivery":"already_paired","provider_ref":null}),
            ),
            ("/controllers/0/id", json!(4)),
            ("/controllers/0/actions/1/event_id", json!("connect")),
            ("/controllers/0/actions/1/at_seconds", json!(2)),
            ("/controllers/0/actions/0/type", json!("disconnect")),
            (
                "/controllers/0/actions/1",
                json!({"event_id":"extra","at_seconds":0.5,"type":"connect","shell":"bad"}),
            ),
            (
                "/controllers/0/actions/1",
                json!({"event_id":"stick","at_seconds":0.5,"type":"stick","stick":"left","x":1.1,"y":0}),
            ),
            (
                "/controllers/0/actions/1",
                json!({"event_id":"touch","at_seconds":0.5,"type":"touchpad","contact_id":0,"phase":"move","x":0,"y":0}),
            ),
            (
                "/controllers/0/actions/1",
                json!({"event_id":"rumble","at_seconds":0.5,"type":"expect_rumble","minimum_amplitude":0.1,"timeout_seconds":1}),
            ),
            ("/write_policy/max_bytes", json!(0)),
            ("/write_policy/max_files", json!(0)),
            ("/write_policy/on_limit", json!("truncate")),
            ("/reference", json!({"required":true})),
            ("/reference", json!({"required":false,"device_ref":null})),
            (
                "/reference",
                json!({"required":false,"calibration_id":null}),
            ),
            ("/required_criteria/profile_version", json!("2.0.0")),
            ("/required_criteria/ids/0", json!("REF-report")),
            (
                "/required_criteria/declared_exclusions",
                json!(["SC-014.prior_release_median_regression"]),
            ),
        ] {
            let mut value = fixture();
            if let Some(target) = value.pointer_mut(pointer) {
                *target = replacement;
            } else {
                value["required_criteria"]["declared_exclusions"] = replacement;
            }
            assert!(Scenario::from_value(value).is_err(), "accepted {pointer}");
        }
        let mut value = fixture();
        value["controllers"][0]["actions"][0]["unexpected"] = json!(true);
        assert!(
            Scenario::from_value(value).is_err(),
            "unknown connect field"
        );
        let mut value = fixture();
        value["controllers"][0]["actions"][1]["unexpected"] = json!(true);
        assert!(Scenario::from_value(value).is_err(), "unknown button field");
        let mut value = fixture();
        let duplicate = value["controllers"][0].clone();
        value["controllers"].as_array_mut().unwrap().push(duplicate);
        assert!(Scenario::from_value(value).is_err());
    }
    #[test]
    fn rejects_paths_and_reference_normalization() {
        for path in [
            "",
            "/absolute",
            "../escape",
            "a/../escape",
            "a//b",
            "./a",
            "a/./b",
            "https:host",
            "a\\b",
            "$HOME",
            "a b",
        ] {
            let mut value = fixture();
            value["output_dir"] = json!(path);
            assert!(Scenario::from_value(value).is_err(), "accepted path {path}");
        }
        let mut value = fixture();
        value["reference"] = json!({"required":true,"device_ref":"capture","calibration_id":null,"calibration_reason":"calibration pending",
            "capture_mode":{"width":1920,"height":1080,"fps":60,"dynamic_range":"sdr","pixel_format":"yuv420p","bit_depth":8,"color_space":"BT.709"}});
        assert!(Scenario::from_value(value.clone()).is_ok());
        for (pointer, replacement) in [
            ("/reference/capture_mode/width", json!(960)),
            ("/reference/capture_mode/bit_depth", json!(10)),
            ("/reference/calibration_reason", Value::Null),
        ] {
            let mut bad = value.clone();
            *bad.pointer_mut(pointer).unwrap() = replacement;
            assert!(Scenario::from_value(bad).is_err());
        }
    }
    #[test]
    #[cfg(unix)]
    fn output_resolution_rejects_existing_and_dangling_symlinks() {
        let root = std::env::temp_dir().join(format!("scenario-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&root).unwrap();
        let scenario = Scenario::from_value(fixture()).unwrap();
        assert_eq!(
            scenario.resolve_output(&root).unwrap(),
            root.canonicalize().unwrap().join("artifacts/run")
        );
        std::os::unix::fs::symlink(root.join("missing"), root.join("artifacts")).unwrap();
        assert!(scenario.resolve_output(&root).is_err());
        std::fs::remove_dir_all(root).unwrap();
    }
}
