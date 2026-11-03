//! Recording integrity is independent of calibration and source HDR metadata authority.

use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};

use crate::evidence::{validate_id, validate_reason, ArtifactRef, Validity};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct CaptureMode {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum PixelFormat {
    #[serde(rename = "yuv420p")]
    Yuv420p,
    #[serde(rename = "p010le")]
    P010le,
    #[serde(rename = "yuv420p10le")]
    Yuv420p10le,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum ColorSpace {
    #[serde(rename = "BT.709")]
    Bt709,
    #[serde(rename = "PQ/BT.2020")]
    PqBt2020,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Recording {
    pub source: String,
    pub capture_mode: CaptureMode,
    pub calibration_id: Option<String>,
    pub calibration_reason: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frames: Option<ArtifactRef>,
    pub pixel_format: PixelFormat,
    pub bit_depth: u8,
    pub color_space: ColorSpace,
    pub clock_domain: String,
    pub frame_count: u64,
    pub timestamp_count: u64,
    pub timestamps: Option<Vec<u64>>,
    pub validity: Validity,
    pub metadata_evidence: Option<String>,
}

impl Recording {
    /// Intrinsic integrity only. A structurally valid synthetic fixture is not calibration.
    pub fn validate(&self) -> Result<()> {
        validate_reason(&self.source)?;
        validate_id(&self.clock_domain)?;
        let mode = &self.capture_mode;
        ensure!(
            mode.width > 0
                && mode.height > 0
                && mode.width.is_multiple_of(2)
                && mode.height.is_multiple_of(2)
                && mode.fps > 0,
            "invalid native capture geometry"
        );
        ensure!(
            self.frame_count > 0 && self.frame_count == self.timestamp_count,
            "frame/timestamp count mismatch"
        );
        let timestamps = self
            .timestamps
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("timestamps unavailable"))?;
        ensure!(
            u64::try_from(timestamps.len())? == self.timestamp_count,
            "timestamp index count mismatch"
        );
        ensure!(
            timestamps.windows(2).all(|w| w[1] > w[0]),
            "timestamp reversal or discontinuity"
        );
        match (self.pixel_format, self.bit_depth, self.color_space) {
            (PixelFormat::Yuv420p, 8, ColorSpace::Bt709)
            | (PixelFormat::P010le | PixelFormat::Yuv420p10le, 10, ColorSpace::PqBt2020) => (),
            _ => anyhow::bail!("pixel precision and color space disagree"),
        }
        if let Some(frames) = &self.frames {
            frames.validate()?;
        }
        if let Some(id) = &self.calibration_id {
            validate_id(id)?;
        } else {
            validate_reason(self.calibration_reason.as_deref().unwrap_or(""))?;
        }
        if let Some(id) = &self.metadata_evidence {
            validate_id(id)?;
        }
        ensure!(
            self.validity == Validity::Valid,
            "recording marked unavailable or invalid"
        );
        Ok(())
    }

    /// Require the declared geometry, precision and shared machine monotonic domain.
    /// The caller supplies the established domain identity, including its boot epoch.
    pub fn validate_for(
        &self,
        required: &CaptureMode,
        hdr: bool,
        stream_clock_domain: &str,
        require_calibration: bool,
    ) -> Result<()> {
        self.validate()?;
        ensure!(
            &self.capture_mode == required,
            "native geometry or cadence mismatch"
        );
        ensure!(
            self.clock_domain == stream_clock_domain,
            "unestablished cross-process clock mapping"
        );
        ensure!(
            (self.color_space == ColorSpace::PqBt2020) == hdr,
            "SDR cannot substitute for HDR"
        );
        ensure!(
            !require_calibration || self.calibration_id.is_some(),
            "capture chain is not calibrated"
        );
        ensure!(self.frames.is_some(), "local frame sequence unavailable");
        Ok(())
    }

    pub fn checked_frame_bytes(&self) -> Result<u64> {
        let pixels = u64::from(self.capture_mode.width)
            .checked_mul(u64::from(self.capture_mode.height))
            .ok_or_else(|| anyhow::anyhow!("frame geometry overflow"))?;
        let bytes = if self.bit_depth == 8 {
            pixels.checked_mul(3).map(|v| v / 2)
        } else {
            pixels.checked_mul(3)
        };
        bytes.ok_or_else(|| anyhow::anyhow!("frame byte count overflow"))
    }

    /// Verify the local packed frame sequence length without reading pictures into memory.
    pub fn validate_files(&self, root: &std::path::Path) -> Result<()> {
        self.validate()?;
        let frames = self
            .frames
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("local frame sequence unavailable"))?;
        let expected = self
            .checked_frame_bytes()?
            .checked_mul(self.frame_count)
            .ok_or_else(|| anyhow::anyhow!("recording byte count overflow"))?;
        ensure!(
            std::fs::metadata(frames.resolve(root)?)?.len() == expected,
            "frame file length mismatch"
        );
        Ok(())
    }
}
