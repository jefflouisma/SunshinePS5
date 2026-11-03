//! Decoding and inspection of recorded elementary streams through ffmpeg/ffprobe.
//!
//! Decoding is done after a session ends, from the recorded file, so it never competes with
//! packet reception for CPU time during measurement.

use anyhow::{anyhow, bail, ensure, Context, Result};
use serde::Serialize;
use std::path::Path;
use std::process::Stdio;
use tokio::process::Command;

use crate::codec::Codec;

/// Acceptance uses exact native planar samples; preview normalization is a separate API.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct NativeFormat {
    pub width: u32,
    pub height: u32,
    pub pixel_format: String,
    pub bit_depth: u8,
    pub color_space: String,
}
impl NativeFormat {
    pub fn from_info(info: &ColorInfo, width: u32, height: u32, hdr: bool) -> Result<Self> {
        ensure!(
            width > 0 && height > 0 && width.is_multiple_of(2) && height.is_multiple_of(2),
            "invalid native geometry"
        );
        ensure!(
            info.width == Some(width) && info.height == Some(height),
            "native geometry mismatch; acceptance cannot scale"
        );
        let expected = if hdr { "yuv420p10le" } else { "yuv420p" };
        ensure!(
            info.pix_fmt.as_deref() == Some(expected),
            "native precision mismatch; acceptance cannot normalize"
        );
        let (matrix, transfer, primaries) = if hdr {
            ("bt2020nc", "smpte2084", "bt2020")
        } else {
            ("bt709", "bt709", "bt709")
        };
        ensure!(
            info.color_space.as_deref() == Some(matrix)
                && info.color_transfer.as_deref() == Some(transfer)
                && info.color_primaries.as_deref() == Some(primaries),
            "missing or incompatible native color interpretation"
        );
        ensure!(
            info.color_range.as_deref() == Some("tv"),
            "native range must be explicitly limited"
        );
        Ok(Self {
            width,
            height,
            pixel_format: expected.into(),
            bit_depth: if hdr { 10 } else { 8 },
            color_space: if hdr { "PQ/BT.2020" } else { "BT.709" }.into(),
        })
    }
    pub fn frame_bytes(&self) -> Result<usize> {
        (self.width as usize)
            .checked_mul(self.height as usize)
            .and_then(|v| v.checked_mul(3))
            .map(|v| if self.bit_depth == 8 { v / 2 } else { v })
            .ok_or_else(|| anyhow!("native frame size overflow"))
    }
}

/// Authenticated control observation (§9), kept separate from decoded pixel inspection.
/// It is not an independent observation of console-source HDR metadata.
#[derive(Debug, Clone, Serialize)]
pub struct HdrSignal {
    pub observed_us: u64,
    pub enabled: bool,
    pub metadata: crate::control::HdrMetadata,
}
impl HdrSignal {
    pub fn from_control(event: &crate::control::ControlEvent, observed_us: u64) -> Option<Self> {
        match event {
            crate::control::ControlEvent::Hdr { enabled, metadata } => Some(Self {
                observed_us,
                enabled: *enabled,
                metadata: metadata.clone(),
            }),
            _ => None,
        }
    }
}
#[derive(Debug, Clone, Serialize)]
pub struct FrameStamp {
    pub frame_id: u64,
    pub received_us: u64,
    pub clock_domain: String,
    pub hdr_signal: Option<HdrSignal>,
}
#[derive(Debug)]
pub struct NativeFrame {
    pub stamp: FrameStamp,
    pub format: NativeFormat,
    pub pixels: Vec<u8>,
}
#[derive(Debug, Clone, Copy)]
pub struct DecodeLimits {
    pub max_frames: u64,
    pub max_output_bytes: u64,
    pub max_frame_bytes: usize,
    pub timeout: std::time::Duration,
}

/// Streams one frame at a time to a local sink with bounded memory, output and runtime.
/// Stamps come from reassembly in presentation order, never synthetic FPS or wall time.
/// A format transition requires a new native segment, retaining its original timestamps.
/// Both decoder and inspector are killed on error/cancellation, including sink failure.
pub async fn decode_native(
    input: &Path,
    codec: Codec,
    format: &NativeFormat,
    stamps: impl IntoIterator<Item = FrameStamp>,
    limits: DecodeLimits,
    mut sink: impl FnMut(NativeFrame) -> Result<()>,
) -> Result<u64> {
    use tokio::io::{AsyncReadExt, BufReader};
    ensure!(
        limits.max_frames > 0 && limits.max_output_bytes > 0 && !limits.timeout.is_zero(),
        "empty decode limits"
    );
    ensure!(
        (format.bit_depth == 8
            && format.pixel_format == "yuv420p"
            && format.color_space == "BT.709")
            || (format.bit_depth == 10
                && format.pixel_format == "yuv420p10le"
                && format.color_space == "PQ/BT.2020"),
        "unsupported native output format"
    );
    ensure!(
        format.width > 0
            && format.height > 0
            && format.width.is_multiple_of(2)
            && format.height.is_multiple_of(2),
        "invalid native geometry"
    );
    let frame_bytes = format.frame_bytes()?;
    ensure!(
        frame_bytes > 0 && frame_bytes <= limits.max_frame_bytes,
        "native frame exceeds memory bound"
    );
    let work = async {
        let mut decoder = Command::new("ffmpeg");
        decoder
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-threads",
                "1",
                "-f",
                codec.ffmpeg_format(),
                "-i",
            ])
            .arg(input)
            .args([
                "-map",
                "0:v:0",
                "-fps_mode",
                "passthrough",
                "-pix_fmt",
                &format.pixel_format,
                "-f",
                "rawvideo",
                "pipe:1",
            ])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true);
        let mut child = decoder.spawn().context("running native ffmpeg")?;
        let mut raw = child.stdout.take().context("native decoder stdout")?;
        let mut inspector = Command::new("ffprobe")
            .args([
                "-v",
                "error",
                "-threads",
                "1",
                "-f",
                codec.ffmpeg_format(),
                "-i",
            ])
            .arg(input)
            .args([
                "-select_streams",
                "v:0",
                "-show_entries",
                "frame=width,height,pix_fmt,color_range,color_space,color_transfer,color_primaries",
                "-of",
                "compact=p=0",
            ])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true)
            .spawn()
            .context("running native frame inspector")?;
        let mut metadata =
            BufReader::new(inspector.stdout.take().context("frame inspector stdout")?);
        let mut stamps = stamps.into_iter();
        let mut count = 0u64;
        let mut previous: Option<(u64, u64, String)> = None;
        let mut signal_time = None;
        loop {
            let mut pixels = vec![0; frame_bytes];
            if raw.read(&mut pixels[..1]).await? == 0 {
                break;
            }
            ensure!(
                count < limits.max_frames
                    && (count + 1)
                        .checked_mul(frame_bytes as u64)
                        .is_some_and(|v| v <= limits.max_output_bytes),
                "native output limit exceeded; recording incomplete"
            );
            raw.read_exact(&mut pixels[1..])
                .await
                .context("partial decoded frame")?;
            let line = bounded_line(&mut metadata, 8192)
                .await?
                .context("missing decoded frame inspection")?;
            let info = compact_frame_info(&line)?;
            ensure!(
                NativeFormat::from_info(
                    &info,
                    format.width,
                    format.height,
                    format.bit_depth == 10
                )? == *format,
                "native format changed; start a new segment"
            );
            let stamp = stamps
                .next()
                .context("missing per-frame receipt timestamp")?;
            ensure!(
                !stamp.clock_domain.is_empty(),
                "missing monotonic clock domain"
            );
            if let Some((id, time, domain)) = &previous {
                ensure!(
                    stamp.frame_id > *id
                        && stamp.received_us > *time
                        && stamp.clock_domain == *domain,
                    "frame ordering or monotonic domain discontinuity"
                );
            }
            if let Some(signal) = &stamp.hdr_signal {
                ensure!(
                    signal.observed_us <= stamp.received_us
                        && signal_time.is_none_or(|t| signal.observed_us >= t),
                    "HDR signal timestamp discontinuity"
                );
                signal_time = Some(signal.observed_us);
            }
            previous = Some((
                stamp.frame_id,
                stamp.received_us,
                stamp.clock_domain.clone(),
            ));
            sink(NativeFrame {
                stamp,
                format: format.clone(),
                pixels,
            })?;
            count += 1;
        }
        ensure!(
            stamps.next().is_none(),
            "timestamp/frame population mismatch"
        );
        // Inspector can emit empty side-data lines; no extra frame may remain.
        while let Some(line) = bounded_line(&mut metadata, 8192).await? {
            ensure!(
                !line.contains("width="),
                "inspection/frame population mismatch"
            );
        }
        ensure!(
            child.wait().await?.success() && inspector.wait().await?.success(),
            "native decode or inspection failed"
        );
        ensure!(count > 0, "no decoded frames");
        Ok(count)
    };
    tokio::time::timeout(limits.timeout, work)
        .await
        .context("native decode timed out; recording incomplete")?
}

async fn bounded_line(
    reader: &mut (impl tokio::io::AsyncBufRead + Unpin),
    max: usize,
) -> Result<Option<String>> {
    use tokio::io::AsyncBufReadExt;
    let mut line = Vec::new();
    loop {
        let buf = reader.fill_buf().await?;
        if buf.is_empty() {
            return if line.is_empty() {
                Ok(None)
            } else {
                Ok(Some(String::from_utf8(line)?))
            };
        }
        let end = buf
            .iter()
            .position(|&v| v == b'\n')
            .map(|i| i + 1)
            .unwrap_or(buf.len());
        ensure!(line.len() + end <= max, "frame metadata exceeds bound");
        line.extend_from_slice(&buf[..end]);
        let done = buf[end - 1] == b'\n';
        reader.consume(end);
        if done {
            let text = String::from_utf8(line)?;
            if text.contains("width=") {
                return Ok(Some(text));
            }
            line = Vec::new();
        }
    }
}
fn compact_frame_info(line: &str) -> Result<ColorInfo> {
    let mut fields = serde_json::Map::new();
    for field in line.trim().split('|') {
        if let Some((key, value)) = field.split_once('=') {
            let value = if key == "width" || key == "height" {
                serde_json::json!(value.parse::<u32>()?)
            } else {
                serde_json::json!(value)
            };
            fields.insert(key.into(), value);
        }
    }
    parse_probe_json(&serde_json::to_vec(
        &serde_json::json!({"streams":[fields]}),
    )?)
}

/// Colour description the decoder reports for the stream. HDR10 output from a host shows a
/// 10-bit pixel format, `smpte2084` (PQ) transfer, and `bt2020` primaries.
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct ColorInfo {
    pub codec_name: Option<String>,
    pub profile: Option<String>,
    pub pix_fmt: Option<String>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub color_range: Option<String>,
    pub color_space: Option<String>,
    pub color_transfer: Option<String>,
    pub color_primaries: Option<String>,
}

impl ColorInfo {
    pub fn is_hdr10(&self) -> bool {
        self.color_transfer.as_deref() == Some("smpte2084")
            && self.color_primaries.as_deref() == Some("bt2020")
            && self.pix_fmt.as_deref().is_some_and(|f| f.contains("10"))
    }
}

pub async fn probe(path: &Path, codec: Codec) -> Result<ColorInfo> {
    use tokio::io::AsyncReadExt;
    let mut child = Command::new("ffprobe")
        .args(["-v", "error", "-f", codec.ffmpeg_format(), "-i"])
        .arg(path)
        .args(["-select_streams", "v:0", "-show_entries",
            "stream=codec_name,profile,pix_fmt,width,height,color_range,color_space,color_transfer,color_primaries", "-of", "json"])
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null())
        .kill_on_drop(true).spawn().context("running ffprobe")?;
    let work = async {
        let mut bytes = Vec::new();
        child
            .stdout
            .take()
            .context("ffprobe stdout")?
            .take(65537)
            .read_to_end(&mut bytes)
            .await?;
        ensure!(bytes.len() <= 65536, "ffprobe output exceeds bound");
        ensure!(child.wait().await?.success(), "ffprobe failed");
        parse_probe_json(&bytes)
    };
    tokio::time::timeout(std::time::Duration::from_secs(30), work)
        .await
        .context("ffprobe timed out")?
}

fn parse_probe_json(json: &[u8]) -> Result<ColorInfo> {
    let value: serde_json::Value = serde_json::from_slice(json)?;
    let stream = value["streams"]
        .get(0)
        .ok_or_else(|| anyhow!("ffprobe found no video stream"))?;
    let text = |key: &str| {
        stream[key]
            .as_str()
            .filter(|s| !s.is_empty() && *s != "unknown")
            .map(str::to_owned)
    };
    let num = |key: &str| stream[key].as_u64().and_then(|v| u32::try_from(v).ok());
    Ok(ColorInfo {
        codec_name: text("codec_name"),
        profile: text("profile"),
        pix_fmt: text("pix_fmt"),
        width: num("width"),
        height: num("height"),
        color_range: text("color_range"),
        color_space: text("color_space"),
        color_transfer: text("color_transfer"),
        color_primaries: text("color_primaries"),
    })
}

/// Legacy preview conversion only; its output is inadmissible for native acceptance.
/// Decodes `input` to raw yuv420p frames of `width`×`height` written to `output`, returning the
/// number of frames. Output is limited-range BT.709 so it is directly comparable with the HDMI
/// reference recording, which is converted the same way.
pub async fn decode_to_yuv420(
    input: &Path,
    codec: Codec,
    width: usize,
    height: usize,
    output: &Path,
) -> Result<usize> {
    let status = Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            codec.ffmpeg_format(),
            "-i",
        ])
        .arg(input)
        .args(["-fps_mode", "passthrough", "-vf"])
        .arg(scale_filter(width, height))
        .args(["-f", "rawvideo"])
        .arg(output)
        .stdin(Stdio::null())
        .status()
        .await
        .context("running ffmpeg")?;
    if !status.success() {
        bail!("ffmpeg could not decode {}", input.display());
    }
    let bytes = tokio::fs::metadata(output).await?.len() as usize;
    let frame = yuv420_frame_size(width, height);
    if !bytes.is_multiple_of(frame) {
        bail!("decoded output is not a whole number of frames ({bytes} bytes)");
    }
    Ok(bytes / frame)
}

/// The scaling/conversion every comparison input goes through.
pub fn scale_filter(width: usize, height: usize) -> String {
    format!("scale={width}:{height}:out_color_matrix=bt709:out_range=tv,format=yuv420p")
}

pub fn yuv420_frame_size(width: usize, height: usize) -> usize {
    width * height * 3 / 2
}

/// Writes one yuv420p frame as a PNG, for human review of comparison results.
pub async fn write_png(frame: &[u8], width: usize, height: usize, output: &Path) -> Result<()> {
    use tokio::io::AsyncWriteExt;
    let mut child = Command::new("ffmpeg")
        .args(["-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo"])
        .args([
            "-pix_fmt",
            "yuv420p",
            "-s",
            &format!("{width}x{height}"),
            "-i",
            "pipe:0",
        ])
        .args(["-frames:v", "1"])
        .arg(output)
        .stdin(Stdio::piped())
        .spawn()
        .context("running ffmpeg")?;
    let mut stdin = child.stdin.take().expect("stdin was piped");
    stdin.write_all(frame).await?;
    drop(stdin);
    if !child.wait().await?.success() {
        bail!("ffmpeg could not write {}", output.display());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_hdr10_probe_output() {
        let json =
            br#"{"streams":[{"codec_name":"hevc","profile":"Main 10","pix_fmt":"yuv420p10le",
            "width":1920,"height":1080,"color_range":"tv","color_space":"bt2020nc",
            "color_transfer":"smpte2084","color_primaries":"bt2020"}]}"#;
        let info = parse_probe_json(json).unwrap();
        assert!(info.is_hdr10());
        assert_eq!(info.width, Some(1920));
    }

    #[test]
    fn sdr_and_unknown_fields() {
        let json = br#"{"streams":[{"codec_name":"h264","pix_fmt":"yuv420p","color_transfer":"unknown"}]}"#;
        let info = parse_probe_json(json).unwrap();
        assert!(!info.is_hdr10());
        assert_eq!(info.color_transfer, None);
    }

    #[test]
    fn frame_size() {
        assert_eq!(yuv420_frame_size(960, 540), 777_600);
    }
}

#[cfg(test)]
mod native_tests {
    use super::*;

    #[test]
    fn acceptance_rejects_normalized_and_unlabeled_hdr() {
        let hdr = parse_probe_json(br#"{"streams":[{"pix_fmt":"yuv420p10le","width":1920,"height":1080,"color_range":"tv","color_space":"bt2020nc","color_transfer":"smpte2084","color_primaries":"bt2020"}]}"#).unwrap();
        assert!(NativeFormat::from_info(&hdr, 1920, 1080, true).is_ok());
        assert!(NativeFormat::from_info(&hdr, 960, 540, true).is_err());
        let mut normalized = hdr.clone();
        normalized.pix_fmt = Some("yuv420p".into());
        assert!(NativeFormat::from_info(&normalized, 1920, 1080, true).is_err());
        normalized = hdr;
        normalized.color_transfer = None;
        assert!(NativeFormat::from_info(&normalized, 1920, 1080, true).is_err());
    }
}

#[cfg(test)]
mod native_process_tests {
    use super::*;
    use std::time::Duration;

    #[tokio::test]
    async fn ffmpeg_native_frames_keep_precision_timestamps_and_limits() {
        let dir = std::env::temp_dir()
            .join("moonlight-t024")
            .join(format!("native-decode-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        for hdr in [false, true] {
            let path = dir.join(if hdr { "hdr.hevc" } else { "sdr.h264" });
            let (encoder, pix, matrix, transfer, primaries) = if hdr {
                ("libx265", "yuv420p10le", "bt2020nc", "smpte2084", "bt2020")
            } else {
                ("libx264", "yuv420p", "bt709", "bt709", "bt709")
            };
            let mut generate = Command::new("ffmpeg");
            generate.args([
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=size=64x64:rate=60",
                "-frames:v",
                "2",
                "-c:v",
                encoder,
                "-pix_fmt",
                pix,
                "-color_range",
                "tv",
                "-colorspace",
                matrix,
                "-color_trc",
                transfer,
                "-color_primaries",
                primaries,
            ]);
            if hdr {
                generate.args(["-x265-params", "log-level=error:pools=1:frame-threads=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"]);
            } else {
                generate.args([
                    "-x264-params",
                    "colorprim=bt709:transfer=bt709:colormatrix=bt709",
                ]);
            }
            assert!(generate
                .arg(&path)
                .stderr(Stdio::null())
                .status()
                .await
                .unwrap()
                .success());
            let codec = if hdr { Codec::Hevc } else { Codec::H264 };
            let format =
                NativeFormat::from_info(&probe(&path, codec).await.unwrap(), 64, 64, hdr).unwrap();
            let stamps = || {
                (0..2).map(|i| FrameStamp {
                    frame_id: i,
                    received_us: 100_000 + i * 16_667,
                    clock_domain: "fixture-monotonic".into(),
                    hdr_signal: Some(
                        HdrSignal::from_control(
                            &crate::control::ControlEvent::Hdr {
                                enabled: i == 0,
                                metadata: crate::control::HdrMetadata {
                                    display_primaries: [[0; 2]; 3],
                                    white_point: [0; 2],
                                    max_display_luminance: 1000,
                                    min_display_luminance: 1,
                                    max_content_light_level: 800,
                                    max_frame_average_light_level: 200,
                                    max_full_frame_luminance: 400,
                                },
                            },
                            99_000 + i * 16_667,
                        )
                        .unwrap(),
                    ),
                })
            };
            let limits = DecodeLimits {
                max_frames: 2,
                max_output_bytes: 24576,
                max_frame_bytes: 12288,
                timeout: Duration::from_secs(10),
            };
            let mut seen = Vec::new();
            assert_eq!(
                decode_native(&path, codec, &format, stamps(), limits, |frame| {
                    seen.push(frame);
                    Ok(())
                })
                .await
                .unwrap(),
                2
            );
            assert_eq!(seen[1].stamp.received_us, 116_667);
            assert!(seen[0].stamp.hdr_signal.as_ref().unwrap().enabled);
            assert!(!seen[1].stamp.hdr_signal.as_ref().unwrap().enabled);
            assert_eq!(
                seen[1].stamp.hdr_signal.as_ref().unwrap().observed_us,
                115_667
            );
            assert_eq!(seen[0].pixels.len(), if hdr { 12288 } else { 6144 });
            if hdr {
                assert!(seen[0]
                    .pixels
                    .chunks_exact(2)
                    .any(|v| u16::from_le_bytes([v[0], v[1]]) > 255));
            }
            assert!(decode_native(
                &path,
                codec,
                &format,
                stamps(),
                DecodeLimits {
                    max_frames: 1,
                    ..limits
                },
                |_| Ok(())
            )
            .await
            .is_err());
            assert!(
                decode_native(&path, codec, &format, stamps().take(1), limits, |_| Ok(()))
                    .await
                    .is_err()
            );
            assert!(
                decode_native(&path, codec, &format, stamps(), limits, |_| bail!(
                    "disk bound"
                ))
                .await
                .is_err()
            );
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
