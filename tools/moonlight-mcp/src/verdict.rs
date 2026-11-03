//! Acceptance evaluation is independent of transport success and never fills absent numbers.

use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::report::Status;

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Operator {
    Lt,
    Lte,
    Gt,
    Gte,
    Eq,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Threshold {
    pub operator: Operator,
    pub value: Option<f64>,
    pub units: Option<String>,
}

/// Validity, population and units are checked before applying measurement uncertainty.
pub fn evaluate_measurement(
    threshold: &Threshold,
    measurement: &crate::evidence::Measurement,
) -> Result<Status> {
    use crate::evidence::{Units, Validity};
    measurement.validate()?;
    let unit = match measurement.units {
        Units::Milliseconds => "ms",
        Units::Microseconds => "us",
        Units::Seconds => "seconds",
        Units::Ratio => "ratio",
        Units::BitsPerSecond => "bits_per_second",
        Units::Frames => "frames",
        Units::FramesPerSecond => "frames_per_second",
        Units::Packets => "packets",
        Units::Events => "events",
        Units::Decibels => "dB",
        Units::ProtocolTicks => "0.1_milliseconds",
        Units::Boolean => "boolean",
    };
    if threshold
        .units
        .as_deref()
        .is_some_and(|expected| expected != unit)
    {
        return Ok(Status::Incomplete);
    }
    Ok(match measurement.validity {
        Validity::Bounded => {
            match compare_bounds(threshold, measurement.lower_bound, measurement.upper_bound) {
                Status::Fail => Status::Fail,
                _ => Status::Incomplete,
            }
        }
        Validity::Valid => match measurement.value.zip(measurement.uncertainty) {
            Some((value, error)) => {
                compare_bounds(threshold, Some(value - error), Some(value + error))
            }
            None => Status::Incomplete,
        },
        _ => Status::Incomplete,
    })
}

/// Bounds include known measurement error, in threshold units. Unknown error is not exactness.
/// A one-sided bound may establish failure; it cannot establish a passing exact population.
pub fn compare_bounds(threshold: &Threshold, lower: Option<f64>, upper: Option<f64>) -> Status {
    let Some(target) = threshold.value.filter(|v| v.is_finite()) else {
        return Status::Incomplete;
    };
    if lower.is_some_and(|v| !v.is_finite()) || upper.is_some_and(|v| !v.is_finite()) {
        return Status::Incomplete;
    }
    if lower.zip(upper).is_some_and(|(a, b)| a > b) {
        return Status::Incomplete;
    }
    let (pass, fail) = match threshold.operator {
        Operator::Lt => (upper.map(|v| v < target), lower.map(|v| v >= target)),
        Operator::Lte => (upper.map(|v| v <= target), lower.map(|v| v > target)),
        Operator::Gt => (lower.map(|v| v > target), upper.map(|v| v <= target)),
        Operator::Gte => (lower.map(|v| v >= target), upper.map(|v| v < target)),
        Operator::Eq => (
            lower.zip(upper).map(|(a, b)| a == target && b == target),
            Some(lower.is_some_and(|v| v > target) || upper.is_some_and(|v| v < target)),
        ),
    };
    if fail == Some(true) {
        Status::Fail
    } else if pass == Some(true) && lower.is_some() && upper.is_some() {
        Status::Pass
    } else {
        Status::Incomplete
    }
}

pub fn overall(states: impl IntoIterator<Item = Status>) -> Status {
    let mut result = Status::Pass;
    let mut count = 0;
    for state in states {
        count += 1;
        match state {
            Status::Incomplete => return Status::Incomplete,
            Status::Fail => result = Status::Fail,
            Status::Pass => (),
        }
    }
    if count == 0 {
        Status::Incomplete
    } else {
        result
    }
}

/// Bounded aggregation boundary for already established functional results and numeric checks.
/// The caller owns evidence authority; this does not certify arbitrary supplied statuses.
pub fn evaluate(required: &[&str], results: &Value) -> Result<Value> {
    ensure!(
        !required.is_empty() && required.len() <= 128,
        "required criterion capacity"
    );
    let results = results
        .as_array()
        .context("criterion results must be an array")?;
    ensure!(results.len() <= 128, "criterion capacity");
    for (i, result) in results.iter().enumerate() {
        let id = result["id"].as_str().context("missing criterion ID")?;
        ensure!(required.contains(&id), "undeclared criterion");
        ensure!(
            !results[..i].iter().any(|r| r["id"] == id),
            "duplicate result"
        );
    }
    let mut criteria = Vec::with_capacity(required.len());
    let mut states = Vec::with_capacity(required.len());
    for (i, id) in required.iter().enumerate() {
        crate::evidence::validate_id(id)?;
        ensure!(!required[..i].contains(id), "duplicate required criterion");
        let mut result = results
            .iter()
            .find(|r| r["id"] == *id)
            .cloned()
            .unwrap_or_else(
                || json!({"id":id,"status":"Incomplete","reason":"required evidence unavailable"}),
            );
        let status = if result["status"] == "Fail" {
            Status::Fail
        } else if result.get("threshold").is_some_and(|v| !v.is_null()) {
            let threshold: Threshold = serde_json::from_value(result["threshold"].clone())?;
            // Absent values never become zero; explicit bounds can still prove a failure.
            let status = compare_bounds(
                &threshold,
                result["lower_bound"].as_f64(),
                result["upper_bound"].as_f64(),
            );
            if result["validity"]
                .as_str()
                .is_some_and(|v| v != "valid" && v != "bounded")
                || (status == Status::Pass && result["value"].as_f64().is_none())
            {
                Status::Incomplete
            } else {
                status
            }
        } else if result.get("value").is_some() {
            Status::Incomplete
        } else {
            serde_json::from_value(result["status"].clone())?
        };
        result["status"] = serde_json::to_value(status)?;
        states.push(status);
        criteria.push(result);
    }
    Ok(json!({"overall_status":overall(states),"criteria":criteria}))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn strict_boundaries_and_one_sided_failures() {
        let t = Threshold {
            operator: Operator::Lt,
            value: Some(2.0),
            units: Some("ms".into()),
        };
        for (a, b, status) in [
            (Some(1.9), Some(1.99), Status::Pass),
            (Some(2.0), Some(2.0), Status::Fail),
            (Some(1.99), Some(2.01), Status::Incomplete),
            (None, None, Status::Incomplete),
            (Some(3.0), None, Status::Fail),
            (None, Some(1.0), Status::Incomplete),
        ] {
            assert_eq!(compare_bounds(&t, a, b), status);
        }
        let unresolved = Threshold { value: None, ..t };
        assert_eq!(
            compare_bounds(&unresolved, Some(0.0), Some(0.0)),
            Status::Incomplete
        );
        assert_eq!(overall([]), Status::Incomplete);
    }
    #[test]
    fn duplicate_or_undeclared_results_are_rejected() {
        assert!(evaluate(&["a"], &json!([{"id":"b","status":"Pass"}])).is_err());
        assert!(evaluate(
            &["a"],
            &json!([{"id":"a","status":"Pass"},{"id":"a","status":"Pass"}])
        )
        .is_err());
    }
    #[test]
    fn absent_exact_values_stay_incomplete_and_established_failures_survive() {
        let results = json!([
            {"id":"a","value":null,"lower_bound":1,"upper_bound":1,"threshold":{"operator":"lt","value":2,"units":"ms"}},
            {"id":"b","status":"Fail","reason":"independent observed failure","threshold":{"operator":"lt","value":null,"units":"ms"}}
        ]);
        let verdict = evaluate(&["a", "b"], &results).unwrap();
        assert_eq!(verdict["overall_status"], "Incomplete");
        assert_eq!(verdict["criteria"][0]["status"], "Incomplete");
        assert_eq!(verdict["criteria"][1]["status"], "Fail");
    }
}
