//! Shared test-machine clock: POSIX `CLOCK_MONOTONIC` on both macOS and Linux.
//!
//! The isolated C helper MUST use `clock_gettime(CLOCK_MONOTONIC, ...)`, not
//! CLOCK_UPTIME_RAW/mach_absolute_time, CLOCK_REALTIME, or its process start time.
//! The OS chooses the boot-relative epoch; we retain the boot UUID as its identity.
//! Platform suspend/rate-adjustment semantics differ, so cross-machine subtraction is
//! forbidden. Resolution comes from clock_getres; accuracy is explicitly uncalibrated.

use anyhow::{ensure, Context, Result};
use nix::time::{clock_getres, clock_gettime, ClockId};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::sync::OnceLock;

use crate::evidence::{Clock, Validity};

pub const SHARED_CLOCK: ClockId = ClockId::CLOCK_MONOTONIC;
pub const DOMAIN: &str = "machine-monotonic";

pub fn monotonic_us() -> Result<u64> {
    let time = clock_gettime(SHARED_CLOCK)?;
    convert_to_us(time.tv_sec(), time.tv_nsec())
}

pub fn monotonic_seconds() -> Result<f64> {
    Ok(monotonic_us()? as f64 / 1_000_000.0)
}

pub fn convert_to_us(seconds: i64, nanoseconds: i64) -> Result<u64> {
    ensure!(
        seconds >= 0 && (0..1_000_000_000).contains(&nanoseconds),
        "invalid native timestamp"
    );
    u64::try_from(seconds)?
        .checked_mul(1_000_000)
        .and_then(|v| v.checked_add(nanoseconds as u64 / 1000))
        .context("timestamp conversion overflow")
}

/// Stable across processes in the same boot; failure is explicit, never a random private epoch.
pub fn local_clock() -> Result<Clock> {
    static EPOCH: OnceLock<String> = OnceLock::new();
    let epoch = if let Some(epoch) = EPOCH.get() {
        epoch.clone()
    } else {
        #[cfg(target_os = "linux")]
        let boot = std::fs::read_to_string("/proc/sys/kernel/random/boot_id")?;
        #[cfg(target_os = "macos")]
        let boot = {
            let output = std::process::Command::new("/usr/sbin/sysctl")
                .args(["-n", "kern.bootsessionuuid"])
                .output()?;
            ensure!(output.status.success(), "boot identity unavailable");
            String::from_utf8(output.stdout)?
        };
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        let boot = {
            anyhow::bail!("boot identity unsupported on this OS");
        };
        ensure!(
            !boot.trim().is_empty() && boot.len() <= 256,
            "invalid boot identity"
        );
        let epoch = hex::encode(Sha256::digest(boot.trim().as_bytes()));
        let _ = EPOCH.set(epoch.clone());
        epoch
    };
    let resolution = clock_getres(SHARED_CLOCK)?;
    let ns = resolution.tv_sec() as f64 * 1_000_000_000.0 + resolution.tv_nsec() as f64;
    let clock = Clock {
        id: format!("{DOMAIN}-{epoch}"),
        clock_type: "monotonic".into(),
        epoch: Some(epoch),
        native_resolution_ns: Some(ns),
        conversion: Some("CLOCK_MONOTONIC seconds*1000000 + floor(nanoseconds/1000)".into()),
        quantization_bound_ns: Some(1000.0),
        uncertainty_ns: None,
        uncertainty_reason: Some(
            "OS resolution is not verified capture/timestamp accuracy; calibration pending".into(),
        ),
        validity: Validity::Valid,
        reason: None,
    };
    clock.validate()?;
    Ok(clock)
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClockSeries {
    pub domain: String,
    pub epoch: String,
    pub timestamps_us: Vec<u64>,
}
#[derive(Debug, Serialize)]
pub struct AlignmentValidity {
    pub valid: bool,
    pub reason: Option<String>,
}

/// Structural clock compatibility only, independent of picture alignment/calibration.
pub fn alignment_validity(a: &ClockSeries, b: &ClockSeries) -> AlignmentValidity {
    let valid = a.domain == DOMAIN
        && b.domain == a.domain
        && !a.epoch.is_empty()
        && a.epoch == b.epoch
        && [&a.timestamps_us, &b.timestamps_us]
            .iter()
            .all(|v| !v.is_empty() && v.windows(2).all(|w| w[1] > w[0]));
    AlignmentValidity {
        valid,
        reason: (!valid)
            .then(|| "unrelated clock domains/epochs or missing/discontinuous timestamps".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn clock_child_fixture() {
        if std::env::var_os("MOONLIGHT_CLOCK_CHILD_FIXTURE").is_some() {
            println!(
                "CLOCK_FIXTURE={}",
                serde_json::json!({"domain": local_clock().unwrap().id, "us": monotonic_us().unwrap()})
            );
        }
    }

    #[test]
    fn independently_started_process_observes_the_same_boot_epoch() {
        let before = monotonic_us().unwrap();
        let output = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "clock::tests::clock_child_fixture",
                "--nocapture",
            ])
            .env("MOONLIGHT_CLOCK_CHILD_FIXTURE", "1")
            .output()
            .unwrap();
        let after = monotonic_us().unwrap();
        assert!(output.status.success());
        let text = String::from_utf8(output.stdout).unwrap();
        let json = text
            .lines()
            .find_map(|line| line.strip_prefix("CLOCK_FIXTURE="))
            .unwrap();
        let observation: serde_json::Value = serde_json::from_str(json).unwrap();
        assert_eq!(observation["domain"], local_clock().unwrap().id);
        let child = observation["us"].as_u64().unwrap();
        assert!(before <= child && child <= after);
    }

    #[test]
    fn native_conversion_checks_overflow_and_records_quantization() {
        assert_eq!(convert_to_us(1, 1999).unwrap(), 1_000_001);
        assert!(convert_to_us(-1, 0).is_err());
        assert!(convert_to_us(i64::MAX, 0).is_err());
        assert!(convert_to_us(0, 1_000_000_000).is_err());
        let clock = local_clock().unwrap();
        clock.validate().unwrap();
        assert_eq!(clock, local_clock().unwrap());
        assert!(monotonic_us().unwrap() <= monotonic_us().unwrap());
        assert!(clock.uncertainty_ns.is_none());
    }
}
