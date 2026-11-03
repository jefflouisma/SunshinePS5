//! Video codec selection shared by negotiation, recording, and decoding.

use serde::{Deserialize, Serialize};
use std::fmt;
use std::str::FromStr;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Codec {
    H264,
    Hevc,
    Av1,
}

impl Codec {
    /// ffmpeg demuxer for the raw elementary stream the host sends. AV1 arrives as OBUs in the
    /// low-overhead bitstream format, which ffmpeg reads with the `obu` demuxer.
    pub fn ffmpeg_format(self) -> &'static str {
        match self {
            Codec::H264 => "h264",
            Codec::Hevc => "hevc",
            Codec::Av1 => "obu",
        }
    }

    pub fn file_extension(self) -> &'static str {
        match self {
            Codec::H264 => "h264",
            Codec::Hevc => "hevc",
            Codec::Av1 => "obu",
        }
    }
}

impl fmt::Display for Codec {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Codec::H264 => "h264",
            Codec::Hevc => "hevc",
            Codec::Av1 => "av1",
        })
    }
}

impl FromStr for Codec {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.to_ascii_lowercase().as_str() {
            "h264" | "avc" => Ok(Codec::H264),
            "hevc" | "h265" => Ok(Codec::Hevc),
            "av1" => Ok(Codec::Av1),
            other => Err(format!(
                "unknown codec {other:?} (expected h264, hevc, or av1)"
            )),
        }
    }
}
