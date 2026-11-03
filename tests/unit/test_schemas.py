"""Synthetic contract fixtures only: these tests establish no hardware evidence.

Run: .venv/bin/python -m pytest -q -p no:cacheprovider tests/unit/test_schemas.py
Schema validation checks structure. Consumers must additionally check clock/ID
references, timestamp ordering, population arithmetic, action times against run
duration, and profile immutability across a run (T016/T019/T035).
"""

import copy
import json
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator
from referencing import Registry, Resource

SCHEMA_DIR = Path(__file__).resolve().parents[1] / "schemas"
NAMES = ("scenario", "evidence", "host-observation")


@pytest.fixture(scope="module")
def validators():
    schemas = {
        name: json.loads((SCHEMA_DIR / f"{name}.schema.json").read_text())
        for name in NAMES
    }
    # No network resolution: an unregistered reference fails the suite.
    registry = Registry().with_resources(
        (schema["$id"], Resource.from_contents(schema)) for schema in schemas.values()
    )
    return {
        name: Draft202012Validator(schema, registry=registry)
        for name, schema in schemas.items()
    }


def clock():
    return {
        "id": "synthetic-host-clock",
        "type": "monotonic",
        "epoch": "synthetic-boot-1",
        "native_resolution_ns": 100,
        "uncertainty_ns": None,
        "uncertainty_reason": "Synthetic fixture; accuracy is not calibrated",
        "conversion": "floor_native_delta_to_integer_microseconds",
        "quantization_bound_ns": 1000,
        "validity": "valid",
        "reason": None,
    }


def population():
    return {
        "expected": 1,
        "observed": 1,
        "represented": 1,
        "excluded": 0,
        "missing": 0,
        "overflow": 0,
        "failed_delivery": 0,
    }


def coverage():
    return {
        "state": "complete",
        "enabled_intervals": [{"start": 1000, "end": 3000}],
        "gaps": [],
        "reason": None,
    }


def evidence():
    return {
        "schema_version": "1.0.0",
        "id": "fixture-evidence-1",
        "session_id": "fixture-session-1",
        "provenance": "synthetic_fixture",
        "kind": "full_pipeline",
        "source": "host_pipeline",
        "frame_id": 1,
        "clock_domain": "synthetic-host-clock",
        "source_clock": clock(),
        "interval": {"start": 1000, "end": 2000},
        "units": "microseconds",
        "resolution": 1,
        "uncertainty": None,
        "uncertainty_reason": "Synthetic fixture; not calibrated",
        "population": population(),
        "coverage": coverage(),
        "value": 1000,
        "validity": "valid",
        "reason": None,
        "artifact_refs": ["fixtures/frame-1.json"],
    }


def scenario():
    return {
        "schema_version": "1.0.0",
        "id": "synthetic-controller-run",
        "host": "fixture.invalid",
        "pairing": {"pin_delivery": "command", "provider_ref": "fixture-pin-helper"},
        "requested": {
            "width": 1920,
            "height": 1080,
            "fps": 60,
            "bitrate_kbps": 20000,
            "codec": "h264",
            "dynamic_range": "sdr",
        },
        "duration_seconds": 30,
        "controllers": [{"id": 0, "actions": []}],
        "reference": {"required": False},
        "required_criteria": {
            "profile_id": "reference_host_compatibility",
            "profile_version": "1.0.0",
            "ids": [
                "REF-pairing",
                "REF-stream",
                "REF-controller",
                "REF-teardown",
                "REF-recording",
                "REF-report",
            ],
        },
        "evidence": {
            "host_metrics": True,
            "input_delivery": True,
            "reference_recording": False,
            "official_client_observations": False,
            "operator_observations": False,
        },
        "output_dir": "artifacts/fixture-run",
        "write_policy": {
            "max_bytes": 1048576,
            "max_files": 100,
            "on_limit": "incomplete",
        },
    }


def observation():
    return {
        "schema_version": "1.0.0",
        "id": "fixture-observation-1",
        "session_id": "fixture-session-1",
        "provenance": "synthetic_fixture",
        "source": "host_pipeline",
        "kind": "frame",
        "frame_id": 1,
        "boundary": "network_submitted",
        "clock_domain": "synthetic-host-clock",
        "source_clock": clock(),
        "observed_at": 2000,
        "population": population(),
        "coverage": coverage(),
        "validity": "valid",
        "reason": None,
        "artifact_refs": [],
        "measurements": [evidence()],
    }


BUILDERS = {"scenario": scenario, "evidence": evidence, "host-observation": observation}


def replace(document, path, value):
    for key in path[:-1]:
        document = document[key]
    document[path[-1]] = value


@pytest.mark.parametrize("name", NAMES)
def test_schema_is_valid_and_baseline_fixture_passes(validators, name):
    validator = validators[name]
    Draft202012Validator.check_schema(validator.schema)
    validator.validate(BUILDERS[name]())


@pytest.mark.parametrize("name", NAMES)
@pytest.mark.parametrize("version", [None, 1, "", "2.0.0"])
def test_version_is_explicit_and_supported(validators, name, version):
    document = BUILDERS[name]()
    document["schema_version"] = version
    assert not validators[name].is_valid(document)


@pytest.mark.parametrize("name", NAMES)
def test_required_fields_cannot_disappear(validators, name):
    for key in validators[name].schema["required"]:
        document = BUILDERS[name]()
        del document[key]
        assert not validators[name].is_valid(document), key


@pytest.mark.parametrize("name", NAMES)
def test_unknown_fields_rejected(validators, name):
    document = BUILDERS[name]()
    document["keyboard"] = {"key": "A"}
    assert not validators[name].is_valid(document)


@pytest.mark.parametrize(
    ("path", "value"),
    [
        (("requested", "bitrate_kbps"), 0),
        (("requested", "bitrate_kbps"), -1),
        (("requested", "bitrate_kbps"), 1.5),
        (("requested", "bitrate_kbps"), "20000"),
        (("requested", "bitrate_kbps"), 2**32),
        (("requested", "fps"), 0),
        (("requested", "dynamic_range"), "auto"),
        (("duration_seconds",), 0),
        (("pairing", "pin_delivery"), "manual"),
        (("pairing", "pin"), "1234"),
        (("required_criteria", "ids"), []),
        (("required_criteria", "ids"), ["REF-stream"]),
        (("required_criteria", "profile_version"), "unversioned"),
        (("output_dir",), "https://example.invalid/upload"),
        (("output_dir",), "../escape"),
        (("output_dir",), "artifacts/../escape"),
        (("write_policy", "max_bytes"), 0),
        (("reference",), {"required": True}),
    ],
)
def test_invalid_scenarios(validators, path, value):
    document = scenario()
    replace(document, path, value)
    assert not validators["scenario"].is_valid(document)


@pytest.mark.parametrize("action_type", ["keyboard", "mouse", "text", "pen"])
def test_noncontroller_actions_rejected(validators, action_type):
    document = scenario()
    document["controllers"][0]["actions"] = [
        {"event_id": "e1", "at_seconds": 0, "type": action_type, "value": 1}
    ]
    assert not validators["scenario"].is_valid(document)


@pytest.mark.parametrize(
    "action",
    [
        {"type": "connect"},
        {"type": "disconnect"},
        {"type": "button", "button": "cross", "pressed": True},
        {"type": "stick", "stick": "left", "x": -1, "y": 1},
        {"type": "trigger", "trigger": "right", "value": 0.5},
        {"type": "motion", "sensor": "gyro", "x": 0, "y": 1, "z": -1},
        {"type": "touchpad", "contact_id": 0, "phase": "down", "x": 0.5, "y": 1},
        {"type": "expect_rumble", "minimum_amplitude": 0, "timeout_seconds": 1},
    ],
)
def test_controller_actions(validators, action):
    document = scenario()
    document["controllers"][0]["actions"] = [
        {"event_id": "e1", "at_seconds": 0, **action}
    ]
    validators["scenario"].validate(document)


def test_four_controllers_and_valid_out_of_mode_bitrate(validators):
    document = scenario()
    document["controllers"] = [{"id": i, "actions": []} for i in range(4)]
    # Schema defines representation, not unverified mode-specific bitrate limits.
    document["requested"]["bitrate_kbps"] = 1
    validators["scenario"].validate(document)
    document["controllers"].append({"id": 4, "actions": []})
    assert not validators["scenario"].is_valid(document)


@pytest.mark.parametrize(
    "profile_id",
    ["reference_host_compatibility", "ps5_release_first", "ps5_release_subsequent"],
)
def test_frozen_profile_sets_match_acceptance_profile(validators, profile_id):
    profile_file = SCHEMA_DIR.parent / "scenarios/profiles/acceptance.json"
    profiles = json.loads(profile_file.read_text())
    declared = profiles["profiles"][profile_id]
    document = scenario()
    document["required_criteria"] = {
        "profile_id": profile_id,
        "profile_version": profiles["profile_version"],
        "ids": declared["required_criteria"],
        "declared_exclusions": declared["declared_exclusions"],
    }
    validators["scenario"].validate(document)
    document["required_criteria"]["ids"] = declared["required_criteria"][:-1]
    assert not validators["scenario"].is_valid(document)
    document["required_criteria"]["ids"] = declared["required_criteria"]
    document["required_criteria"]["declared_exclusions"] = ["SC-013"]
    assert not validators["scenario"].is_valid(document)


def test_required_uncalibrated_hdr_reference_remains_declarable(validators):
    document = scenario()
    document["reference"] = {
        "required": True,
        "device_ref": "synthetic-capture-device",
        "capture_mode": {
            "width": 1920,
            "height": 1080,
            "fps": 60,
            "dynamic_range": "hdr",
            "pixel_format": "p010le",
            "bit_depth": 10,
            "color_space": "PQ/BT.2020",
        },
        "calibration_id": None,
        "calibration_reason": "Synthetic fixture; real calibration unavailable",
    }
    validators["scenario"].validate(document)
    document["reference"]["capture_mode"]["bit_depth"] = 8
    assert not validators["scenario"].is_valid(document)
    document["reference"]["capture_mode"]["bit_depth"] = 10
    document["reference"]["calibration_reason"] = None
    assert not validators["scenario"].is_valid(document)


@pytest.mark.parametrize("absence", ["omitted", "null"])
@pytest.mark.parametrize("state", ["unavailable", "invalid"])
def test_missing_evidence_is_absent_and_explained(validators, absence, state):
    document = evidence()
    document.update(validity=state, reason="queue_gap", value=None)
    document["coverage"].update(state="partial", reason="queue_gap")
    document["population"].update(expected=2, missing=1)
    if absence == "omitted":
        del document["value"]
    validators["evidence"].validate(document)
    document["value"] = 0
    assert not validators["evidence"].is_valid(document)
    document["value"] = None
    document["reason"] = None
    assert not validators["evidence"].is_valid(document)


def test_measured_zero_is_distinct_from_absence(validators):
    document = evidence()
    document["value"] = 0
    validators["evidence"].validate(document)


def test_bounds_are_not_exact_values(validators):
    document = evidence()
    document.update(
        validity="bounded", reason="histogram_rank_overflow", lower_bound=65536
    )
    del document["value"]
    validators["evidence"].validate(document)
    document["value"] = 65535
    assert not validators["evidence"].is_valid(document)
    del document["value"]
    del document["lower_bound"]
    assert not validators["evidence"].is_valid(document)


@pytest.mark.parametrize(
    ("path", "value"),
    [
        (("source",), "client_receipt"),
        (("clock_domain",), None),
        (("source_clock", "type"), "wall_clock"),
        (("source_clock", "validity"), "invalid"),
        (("source_clock", "native_resolution_ns"), 0),
        (("source_clock", "epoch"), None),
        (("source_clock", "uncertainty_reason"), None),
        (("uncertainty_reason",), None),
        (("population", "observed"), -1),
        (("population", "observed"), 0),
        (("population", "observed"), None),
        (("population", "expected"), None),
        (("population", "represented"), None),
        (("population", "missing"), 1),
        (("coverage", "state"), "partial"),
        (("coverage", "gaps"), [{"start": 1, "end": 2, "reason": "disabled"}]),
        (("interval", "start"), None),
        (("value",), None),
        (("lower_bound",), 1),
        (("frame_id",), -1),
        (("artifact_refs",), ["https://example.invalid/evidence"]),
    ],
)
def test_unreliable_evidence_cannot_claim_exact_validity(validators, path, value):
    document = evidence()
    replace(document, path, value)
    assert not validators["evidence"].is_valid(document)


def test_protocol_zero_and_saturation_semantics(validators):
    document = evidence()
    document.update(
        kind="protocol_latency",
        source="protocol_header",
        units="0.1_milliseconds",
        raw_protocol_latency=10,
        value=10,
        resolution=1,
    )
    validators["evidence"].validate(document)
    document.update(raw_protocol_latency=0, value=0)
    assert not validators["evidence"].is_valid(document)
    document.update(
        validity="unavailable", reason="repeated_or_missing_capture_time", value=None
    )
    validators["evidence"].validate(document)
    document.update(
        raw_protocol_latency=65535, validity="bounded", reason="wire_saturation"
    )
    document["lower_bound"] = 65535
    validators["evidence"].validate(document)


def test_input_delivery_requires_host_authority_and_boundary(validators):
    document = evidence()
    del document["frame_id"]
    document.update(
        kind="input_delivery",
        event_id="controller-event-1",
        controller_id=0,
        delivery_boundary="synthetic platform fixture success",
        authenticated=True,
    )
    validators["evidence"].validate(document)
    for key, value in [
        ("source", "client_receipt"),
        ("delivery_boundary", ""),
        ("authenticated", False),
        ("controller_id", 4),
    ]:
        invalid = copy.deepcopy(document)
        invalid[key] = value
        assert not validators["evidence"].is_valid(invalid)


@pytest.mark.parametrize("kind", ["frame", "input"])
def test_host_observation_correlations(validators, kind):
    document = observation()
    if kind == "input":
        del document["frame_id"]
        document.update(
            kind="input",
            event_id="e1",
            controller_id=0,
            boundary="console_delivered",
            authenticated=True,
            delivery_boundary="synthetic fixture delivery operation",
            measurements=[],
        )
    validators["host-observation"].validate(document)
    correlation_key = "frame_id" if kind == "frame" else "event_id"
    del document[correlation_key]
    assert not validators["host-observation"].is_valid(document)


@pytest.mark.parametrize(
    ("path", "value"),
    [
        (("source",), "client_receipt"),
        (("observed_at",), None),
        (("frame_id",), -1),
        (("boundary",), "transport_ack"),
        (("measurements", 0, "schema_version"), "2.0.0"),
    ],
)
def test_invalid_host_observations(validators, path, value):
    document = observation()
    replace(document, path, value)
    assert not validators["host-observation"].is_valid(document)


def test_unknown_host_timestamp_is_not_zero(validators):
    document = observation()
    document.update(
        observed_at=None, validity="unavailable", reason="clock_unavailable"
    )
    validators["host-observation"].validate(document)
    document["observed_at"] = 0
    assert not validators["host-observation"].is_valid(document)


def test_observed_delivery_failure_preserves_its_timestamp(validators):
    document = observation()
    del document["frame_id"]
    document.update(
        kind="input",
        event_id="e1",
        controller_id=0,
        boundary="delivery_failed",
        authenticated=True,
        reason="synthetic_platform_failure",
        measurements=[],
    )
    document["population"]["failed_delivery"] = 1
    validators["host-observation"].validate(document)
    document["reason"] = None
    assert not validators["host-observation"].is_valid(document)


def test_host_drops_require_their_own_reason(validators):
    document = observation()
    document.update(boundary="host_dropped", drop_reason="synthetic_encoder_failure")
    validators["host-observation"].validate(document)
    del document["drop_reason"]
    assert not validators["host-observation"].is_valid(document)


def test_disabled_coverage_keeps_the_gap_and_absent_measurement(validators):
    document = evidence()
    document.update(value=None, validity="unavailable", reason="metrics_disabled")
    document["population"] = dict.fromkeys(document["population"], None)
    document["coverage"] = {
        "state": "disabled",
        "enabled_intervals": [],
        "gaps": [{"start": 1000, "end": 3000, "reason": "metrics_disabled"}],
        "reason": "metrics_disabled",
    }
    validators["evidence"].validate(document)
    document["value"] = 0
    assert not validators["evidence"].is_valid(document)


@pytest.mark.parametrize("name", ["evidence", "host-observation"])
def test_coverage_is_bounded_and_never_silently_truncated(validators, name):
    document = BUILDERS[name]()
    document["coverage"]["enabled_intervals"] *= 129
    assert not validators[name].is_valid(document)


def test_client_frame_rate_has_an_explicit_rate_unit(validators):
    value = evidence()
    value["kind"] = "client_receipt"
    value["source"] = "client_receipt"
    value["units"] = "frames_per_second"
    value["value"] = 60.0
    validators["evidence"].validate(value)


def test_metrics_extension_keeps_old_documents_and_checks_typed_fields(validators):
    validator = validators["host-observation"]
    document = observation()
    validator.validate(document)
    document["schema_version"] = "1.1.0"
    document["metrics"] = {
        "allocated_bytes": 83886080,
        "raw": {"observed_us": 2000, "durations_us": [0, None, 65536], "flags": 5},
    }
    validator.validate(document)
    for path, value in (
        (("metrics", "allocated_bytes"), 83886081),
        (("metrics", "raw", "durations_us"), [-1]),
        (("metrics", "raw", "durations_us"), [0] * 6),
        (("metrics", "raw", "flags"), "31"),
        (("metrics", "context"), {"client_name": "missing other required fields"}),
        (("metrics", "summaries"), []),
        (("metrics", "unexpected"), 1),
    ):
        invalid = copy.deepcopy(document)
        replace(invalid, path, value)
        assert list(validator.iter_errors(invalid)), (path, value)


def test_metrics_context_and_twelve_exact_summaries(validators):
    document = observation()
    document["schema_version"] = "1.1.0"
    quantile = {
        "value_us": 0,
        "lower_bound_us": None,
        "population": 1,
        "validity": "valid",
        "reason": None,
    }
    summaries = [
        {
            "channel": channel,
            "window": window,
            "population": {
                "seen": 1,
                "represented": 1,
                "overflow": 0,
                "missing": 0,
                "failed_deliveries": 0,
            },
            "median": copy.deepcopy(quantile),
            "p95": copy.deepcopy(quantile),
            "p99": copy.deepcopy(quantile),
        }
        for window in ("whole_session", "last_10_seconds")
        for channel in (
            "capture",
            "encode",
            "packetize",
            "send",
            "full_pipeline",
            "input_delivery",
        )
    ]
    context = {
        key: None
        for key in (
            "host_version",
            "firmware_version",
            "client_name",
            "codec",
            "delivery_boundary",
            "width",
            "height",
            "fps_numerator",
            "fps_denominator",
            "requested_bitrate_kbps",
            "chosen_bitrate_kbps",
        )
    }
    context["dynamic_range"] = "unknown"
    document["metrics"] = {"context": context, "summaries": summaries}
    validator = validators["host-observation"]
    validator.validate(document)
    for path, value in (
        (("metrics", "context", "requested_bitrate_kbps"), -1),
        (("metrics", "context", "dynamic_range"), "guessed HDR"),
        (("metrics", "summaries", 0, "p99", "value_us"), None),
        (("metrics", "summaries", 0, "p99", "lower_bound_us"), 65536),
        (("metrics", "summaries", 0, "window"), "approximate ten buckets"),
    ):
        invalid = copy.deepcopy(document)
        replace(invalid, path, value)
        assert list(validator.iter_errors(invalid)), (path, value)
