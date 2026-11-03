//! RTSP session setup: OPTIONS, DESCRIBE, SETUP ×3, ANNOUNCE, PLAY (protocol-notes §3–§4).
//!
//! Every request uses a new TCP connection, and the response is read until the host closes it.
//! When the launch response's session URL uses `rtspenc://`, each message travels in an
//! AES-128-GCM envelope keyed with the session's `rikey`.

use anyhow::{anyhow, bail, Context, Result};
use serde::Serialize;
use std::collections::HashMap;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

use tokio::net::TcpStream;

const MAX_RESPONSE: usize = 1024 * 1024;

use crate::codec::Codec;
use crate::crypto::{aes_gcm_decrypt, aes_gcm_encrypt};

/// `x-ss-general.encryption*` flags (§3).
pub const ENC_CONTROL_V2: u32 = 0x01;
pub const ENC_VIDEO: u32 = 0x02;
pub const ENC_AUDIO: u32 = 0x04;

/// moonlight-qt's LAN packet size (§4).
pub const DEFAULT_PACKET_SIZE: u32 = 1392;
/// Size of the per-datagram prefix when video is encrypted (§5).
pub const ENC_VIDEO_PREFIX: u32 = 32;
/// Bitrate at or above which moonlight-common-c asks for high-quality stereo (§3).
const HIGH_AUDIO_BITRATE_KBPS: u32 = 15_000;
const TIMEOUT: Duration = Duration::from_secs(10);

/// What the client asks for.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct StreamSettings {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate_kbps: u32,
    pub codec: Codec,
    /// Request 10-bit output; with an HDR host display this yields PQ/BT.2020 (§10).
    pub hdr: bool,
    pub packet_size: u32,
    /// Opt in to video and audio encryption even if the host does not require it.
    pub encrypt_av: bool,
}

impl StreamSettings {
    pub fn validate(&self) -> Result<()> {
        if self.width == 0
            || self.height == 0
            || self.fps == 0
            || self.fps.checked_mul(100).is_none()
            || self.bitrate_kbps == 0
            || !(64..=DEFAULT_PACKET_SIZE).contains(&self.packet_size)
        {
            bail!("invalid stream dimensions, rate, bitrate or packet size");
        }
        if self.hdr && self.codec == Codec::H264 {
            bail!("H.264 does not support this ten-bit HDR request");
        }
        Ok(())
    }
}

/// Capabilities read from the DESCRIBE response.
#[derive(Debug, Clone, Default, Serialize)]
pub struct Describe {
    pub hevc: bool,
    pub av1: bool,
    pub feature_flags: u32,
    pub encryption_supported: Option<u32>,
    pub encryption_requested: u32,
    pub ref_pic_invalidation: bool,
}

impl Describe {
    fn parse(sdp: &str) -> Result<Self> {
        for name in [
            "x-ss-general.featureFlags",
            "x-ss-general.encryptionSupported",
            "x-ss-general.encryptionRequested",
        ] {
            let values: Vec<_> = sdp
                .lines()
                .filter_map(|line| line.trim().strip_prefix(&format!("a={name}:")))
                .collect();
            if values.len() > 1
                || values
                    .first()
                    .is_some_and(|value| parse_u32(value.trim()).is_none())
            {
                bail!("invalid or duplicate SDP attribute {name}");
            }
        }
        let attr = |name: &str| {
            sdp.lines().find_map(|l| {
                l.trim()
                    .strip_prefix("a=")
                    .and_then(|a| a.strip_prefix(name))
                    .and_then(|v| v.strip_prefix(':'))
                    .map(|v| v.trim().to_string())
            })
        };
        let num = |name: &str| attr(name).and_then(|v| parse_u32(&v));
        Ok(Self {
            hevc: sdp.contains("sprop-parameter-sets=AAAAAU"),
            av1: sdp.contains("AV1/90000"),
            feature_flags: num("x-ss-general.featureFlags").unwrap_or(0),
            encryption_supported: num("x-ss-general.encryptionSupported"),
            encryption_requested: num("x-ss-general.encryptionRequested").unwrap_or(0),
            ref_pic_invalidation: sdp.contains("x-nv-video[0].refPicInvalidation"),
        })
    }
}

fn parse_u32(v: &str) -> Option<u32> {
    match v.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => v.parse().ok(),
    }
}

/// Everything the streams need from RTSP.
#[derive(Debug, Clone, Serialize)]
pub struct RtspSession {
    /// The caller's exact request, distinct from the attributes announced on the wire.
    pub requested: StreamSettings,
    /// Accepted ANNOUNCE attributes; acceptance does not attest the encoder's effective settings.
    pub announced: StreamSettings,
    /// This protocol supplies no selected encoder target in its RTSP responses (notes §4).
    /// Keep it absent until independently supplied host evidence establishes it.
    pub host_chosen_bitrate_kbps: Option<u32>,
    pub session_id: String,
    pub describe: Describe,
    pub audio_port: u16,
    pub video_port: u16,
    pub control_port: u16,
    /// 16 ASCII characters prefixed to every UDP ping.
    pub ping_payload: String,
    pub video_ping_payload: String,
    /// Sent in the ENet CONNECT so the host can match the control connection to this session.
    pub connect_data: u32,
    pub encryption_enabled: u32,
    pub packet_size: u32,
    pub encrypted_rtsp: bool,
}

/// Established protocol session. The ENet owner also guarantees disconnect on drop.
/// A streaming runner polls `control` independently of media decode/recording work.
pub struct ActiveSession {
    pub negotiation: RtspSession,
    pub control: crate::control::ControlConnection,
}

impl ActiveSession {
    /// RTSP has no persistent socket and the pinned hosts implement no TEARDOWN method.
    /// Their session-ending signal is ENet disconnect (protocol-notes §11).
    pub async fn teardown(&mut self) -> Result<()> {
        self.control.disconnect().await
    }
}

pub async fn establish(
    address: &str,
    session_url: &str,
    rikey: [u8; 16],
    settings: &StreamSettings,
) -> Result<ActiveSession> {
    let negotiation = handshake(address, session_url, rikey, settings).await?;
    let remote = tokio::net::lookup_host((address, negotiation.control_port))
        .await?
        .next()
        .ok_or_else(|| anyhow!("host address resolved to no endpoints"))?;
    let control = crate::control::ControlConnection::connect(
        remote,
        negotiation.connect_data,
        rikey,
        negotiation.encryption_enabled & ENC_CONTROL_V2 != 0,
    )
    .await?;
    Ok(ActiveSession {
        negotiation,
        control,
    })
}

struct Response {
    status: u16,
    headers: HashMap<String, String>,
    body: String,
}

impl Response {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .get(&name.to_ascii_lowercase())
            .map(String::as_str)
    }

    fn parse(raw: &[u8]) -> Result<Self> {
        if raw.len() > MAX_RESPONSE {
            bail!("RTSP response exceeds limit");
        }
        let text = std::str::from_utf8(raw).context("RTSP response is not UTF-8")?;
        let (head, body) = text
            .split_once("\r\n\r\n")
            .ok_or_else(|| anyhow!("unterminated RTSP headers"))?;
        let mut lines = head.split("\r\n");
        let mut status_line = lines.next().unwrap_or_default().splitn(3, ' ');
        if status_line.next() != Some("RTSP/1.0") {
            bail!("invalid RTSP version");
        }
        let code = status_line.next().unwrap_or_default();
        if code.len() != 3 || !code.bytes().all(|b| b.is_ascii_digit()) {
            bail!("invalid RTSP status");
        }
        let status = code.parse()?;
        let mut headers = HashMap::new();
        for line in lines {
            let (key, value) = line
                .split_once(':')
                .ok_or_else(|| anyhow!("malformed RTSP header"))?;
            if key.is_empty()
                || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
                || headers
                    .insert(key.to_ascii_lowercase(), value.trim().to_string())
                    .is_some()
            {
                bail!("invalid or duplicate RTSP header");
            }
        }
        let length = headers
            .get("content-length")
            .map(|v| v.parse::<usize>())
            .transpose()?;
        // mcc 41cedb89 RtspParser.c treats bytes after headers as payload when length is absent.
        if length.is_some_and(|declared| declared != body.len()) {
            bail!("RTSP Content-Length mismatch");
        }
        Ok(Self {
            status,
            headers,
            body: body.to_string(),
        })
    }

    fn validate(&self, cseq: u32) -> Result<()> {
        if self.status != 200 {
            bail!("RTSP status {}", self.status);
        }
        if self.header("CSeq").and_then(parse_u32) != Some(cseq) {
            bail!("RTSP CSeq mismatch");
        }
        Ok(())
    }
}

struct Client {
    address: String,
    port: u16,
    host_header: String,
    cseq: u32,
    /// Present for `rtspenc://` sessions: the key and the client's message counter.
    encryption: Option<([u8; 16], u32)>,
}

impl Client {
    async fn request(
        &mut self,
        method: &str,
        target: &str,
        headers: &[(&str, String)],
        body: &str,
    ) -> Result<Response> {
        let mut msg = format!(
            "{method} {target} RTSP/1.0\r\nCSeq: {}\r\nX-GS-ClientVersion: 14\r\nHost: {}\r\n",
            self.cseq, self.host_header
        );
        let cseq = self.cseq;
        self.cseq = self
            .cseq
            .checked_add(1)
            .ok_or_else(|| anyhow!("RTSP CSeq exhausted"))?;
        for (k, v) in headers {
            msg.push_str(&format!("{k}: {v}\r\n"));
        }
        msg.push_str("\r\n");
        msg.push_str(body);

        let wire = match &mut self.encryption {
            Some((key, seq)) => {
                *seq = seq
                    .checked_add(1)
                    .ok_or_else(|| anyhow!("RTSP encryption sequence exhausted"))?; // first is 1 (§3)
                encrypt_message(key, *seq, msg.as_bytes())?
            }
            None => msg.into_bytes(),
        };
        let mut stream = tokio::time::timeout(
            TIMEOUT,
            TcpStream::connect((self.address.as_str(), self.port)),
        )
        .await
        .map_err(|_| anyhow!("RTSP connect to {}:{} timed out", self.address, self.port))??;
        tokio::time::timeout(TIMEOUT, stream.write_all(&wire)).await??;
        let mut raw = Vec::new();
        tokio::time::timeout(
            TIMEOUT,
            (&mut stream)
                .take((MAX_RESPONSE + 25) as u64)
                .read_to_end(&mut raw),
        )
        .await
        .map_err(|_| anyhow!("RTSP {method} response timed out"))??;
        let plain = match &self.encryption {
            Some((key, _)) => decrypt_message(key, &raw)?,
            None => raw,
        };
        let response = Response::parse(&plain).with_context(|| format!("RTSP {method}"))?;
        response.validate(cseq)?;
        Ok(response)
    }
}

/// 24-byte header `{u32 BE 0x80000000 | len, u32 BE seq, tag[16]}` then the ciphertext.
fn encrypt_message(key: &[u8; 16], seq: u32, plain: &[u8]) -> Result<Vec<u8>> {
    let (ct, tag) = aes_gcm_encrypt(key, &rtsp_iv(seq, b'C'), plain)?;
    let mut out = Vec::with_capacity(24 + ct.len());
    out.extend_from_slice(&(0x8000_0000 | u32::try_from(plain.len())?).to_be_bytes());
    out.extend_from_slice(&seq.to_be_bytes());
    out.extend_from_slice(&tag);
    out.extend_from_slice(&ct);
    Ok(out)
}

fn decrypt_message(key: &[u8; 16], raw: &[u8]) -> Result<Vec<u8>> {
    if raw.len() < 24 {
        bail!("encrypted RTSP response is only {} bytes", raw.len());
    }
    let type_len = u32::from_be_bytes(raw[0..4].try_into().expect("4 bytes"));
    if type_len & 0x8000_0000 == 0 {
        bail!("host sent a plaintext RTSP message on an encrypted session");
    }
    let len = (type_len & 0x7fff_ffff) as usize;
    let seq = u32::from_be_bytes(raw[4..8].try_into().expect("4 bytes"));
    if seq == 0 || raw.len() != 24 + len {
        bail!("invalid encrypted RTSP length/sequence");
    }
    let ct = raw
        .get(24..24 + len)
        .ok_or_else(|| anyhow!("encrypted RTSP response truncated"))?;
    aes_gcm_decrypt(key, &rtsp_iv(seq, b'H'), ct, &raw[8..24])
}

/// IV: sequence number little-endian in bytes 0-3, origin ('C' or 'H') at 10, 'R' at 11.
fn rtsp_iv(seq: u32, origin: u8) -> [u8; 12] {
    let mut iv = [0u8; 12];
    iv[..4].copy_from_slice(&seq.to_le_bytes());
    iv[10] = origin;
    iv[11] = b'R';
    iv
}

fn server_port(resp: &Response) -> Result<u16> {
    let transport = resp
        .header("Transport")
        .ok_or_else(|| anyhow!("SETUP response has no Transport header"))?;
    transport
        .split("server_port=")
        .nth(1)
        .and_then(|rest| rest.split(|c: char| !c.is_ascii_digit()).next())
        .and_then(|p| p.parse().ok())
        .filter(|p| *p != 0)
        .ok_or_else(|| anyhow!("no server_port in Transport {transport:?}"))
}

/// Validated SETUP response (notes §3). Audio establishes the Session token.
pub struct SetupResponse {
    pub session_id: String,
    pub port: u16,
}

impl SetupResponse {
    pub fn parse(raw: &[u8], cseq: u32) -> Result<Self> {
        let response = Response::parse(raw)?;
        response.validate(cseq)?;
        Self::from_response(&response)
    }

    fn from_response(response: &Response) -> Result<Self> {
        let session_id = response
            .header("Session")
            .and_then(|s| s.split(';').next())
            .filter(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_graphic()))
            .ok_or_else(|| anyhow!("SETUP returned no valid Session"))?
            .to_string();
        Ok(Self {
            session_id,
            port: server_port(response)?,
        })
    }
}

/// Splits a launch `sessionUrl0` into (encrypted, port). The host part is ignored: the client
/// keeps using the address it already reached the host on, as moonlight-common-c does (§2).
pub fn parse_session_url(url: &str) -> Result<(bool, u16)> {
    let (encrypted, rest) = if let Some(r) = url.strip_prefix("rtspenc://") {
        (true, r)
    } else if let Some(r) = url.strip_prefix("rtsp://") {
        (false, r)
    } else {
        bail!("unexpected session URL {url:?}");
    };
    let authority = rest.split('/').next().unwrap_or(rest);
    let (host, port) = authority
        .rsplit_once(':')
        .ok_or_else(|| anyhow!("session URL has no port"))?;
    if host.is_empty() || host.contains(['\r', '\n', '@']) {
        bail!("invalid session URL authority");
    }
    if !port.bytes().all(|b| b.is_ascii_digit()) {
        bail!("invalid session URL port");
    }
    let port = port.parse::<u16>().context("invalid session URL port")?;
    if port == 0 {
        bail!("zero session URL port");
    }
    Ok((encrypted, port))
}

pub async fn handshake(
    address: &str,
    session_url: &str,
    rikey: [u8; 16],
    settings: &StreamSettings,
) -> Result<RtspSession> {
    settings.validate()?;
    if address.is_empty() || address.contains(['\r', '\n', ' ']) {
        bail!("invalid host address");
    }
    let (encrypted, port) = parse_session_url(session_url)?;
    let scheme = if encrypted { "rtspenc" } else { "rtsp" };
    let authority = if address.contains(':') {
        format!("[{address}]")
    } else {
        address.to_string()
    };
    let target = format!("{scheme}://{authority}:{port}");
    let mut client = Client {
        address: address.to_string(),
        port,
        host_header: if settings.bitrate_kbps >= HIGH_AUDIO_BITRATE_KBPS {
            address.to_string()
        } else {
            "0.0.0.0".to_string()
        },
        cseq: 1,
        encryption: encrypted.then_some((rikey, 0)),
    };
    let if_modified = (
        "If-Modified-Since",
        "Thu, 01 Jan 1970 00:00:00 GMT".to_string(),
    );

    client.request("OPTIONS", &target, &[], "").await?;
    let describe = client
        .request(
            "DESCRIBE",
            &target,
            &[
                ("Accept", "application/sdp".to_string()),
                if_modified.clone(),
            ],
            "",
        )
        .await?;
    let describe = Describe::parse(&describe.body)?;
    match settings.codec {
        Codec::Hevc if !describe.hevc => bail!("host does not offer HEVC for this session"),
        Codec::Av1 if !describe.av1 => bail!("host does not offer AV1 for this session"),
        _ => {}
    }

    let transport = (
        "Transport",
        "unicast;X-GS-ClientPort=50000-50001".to_string(),
    );
    let audio = client
        .request(
            "SETUP",
            "streamid=audio/0/0",
            &[transport.clone(), if_modified.clone()],
            "",
        )
        .await?;
    let setup = SetupResponse::from_response(&audio)?;
    let session_id = setup.session_id;
    let audio_port = setup.port;
    let ping_payload = audio
        .header("X-SS-Ping-Payload")
        .filter(|p| p.len() == 16 && p.is_ascii())
        .map(str::to_owned)
        .ok_or_else(|| anyhow!("SETUP audio returned no 16-character X-SS-Ping-Payload"))?;
    let session = ("Session", session_id.clone());

    let video = client
        .request(
            "SETUP",
            "streamid=video/0/0",
            &[session.clone(), transport.clone(), if_modified.clone()],
            "",
        )
        .await?;
    let video_port = server_port(&video)?;
    let video_ping_payload = video
        .header("X-SS-Ping-Payload")
        .filter(|p| p.len() == 16 && p.is_ascii())
        .ok_or_else(|| anyhow!("SETUP video returned no 16-character X-SS-Ping-Payload"))?
        .to_string();

    let control = client
        .request(
            "SETUP",
            "streamid=control/13/0",
            &[session.clone(), transport, if_modified],
            "",
        )
        .await?;
    let control_port = server_port(&control)?;
    let connect_data = control
        .header("X-SS-Connect-Data")
        .and_then(parse_u32)
        .ok_or_else(|| anyhow!("SETUP control returned no X-SS-Connect-Data"))?;

    let encryption_enabled = encryption_flags(&describe, settings.encrypt_av);
    if describe.encryption_requested & !encryption_enabled != 0 {
        bail!("host requires unsupported encryption");
    }
    let packet_size = if encryption_enabled & ENC_VIDEO != 0 {
        settings
            .packet_size
            .checked_sub(ENC_VIDEO_PREFIX)
            .filter(|p| *p >= 32)
            .ok_or_else(|| anyhow!("packet size too small for encrypted video"))?
    } else {
        settings.packet_size
    };
    let sdp = build_sdp(
        address,
        video_port,
        settings,
        encryption_enabled,
        packet_size,
    );
    client
        .request(
            "ANNOUNCE",
            "streamid=control/13/0",
            &[
                session.clone(),
                ("Content-type", "application/sdp".to_string()),
                ("Content-length", sdp.len().to_string()),
            ],
            &sdp,
        )
        .await?;
    client.request("PLAY", "/", &[session], "").await?;

    let mut announced = settings.clone();
    announced.packet_size = packet_size;
    Ok(RtspSession {
        requested: settings.clone(),
        announced,
        host_chosen_bitrate_kbps: None,
        session_id,
        video_ping_payload,
        describe,
        audio_port,
        video_port,
        control_port,
        ping_payload,
        connect_data,
        encryption_enabled,
        packet_size,
        encrypted_rtsp: encrypted,
    })
}

/// Control encryption whenever offered; video/audio when the host requests it or the caller
/// opts in, limited to what the host supports (§3, §4).
fn encryption_flags(describe: &Describe, encrypt_av: bool) -> u32 {
    let Some(supported) = describe.encryption_supported else {
        return 0;
    };
    let mut wanted = ENC_CONTROL_V2 | describe.encryption_requested;
    if encrypt_av {
        wanted |= ENC_VIDEO | ENC_AUDIO;
    }
    wanted & supported & (ENC_CONTROL_V2 | ENC_VIDEO | ENC_AUDIO)
}

/// The ANNOUNCE body, with moonlight-common-c's attribute set and values (§4).
pub fn build_sdp(
    address: &str,
    video_port: u16,
    s: &StreamSettings,
    encryption_enabled: u32,
    packet_size: u32,
) -> String {
    // moonlight-common-c sends 80% of the configured bitrate in the NVIDIA fields and the full
    // value in configuredBitrateKbps, from which Sunshine derives its encoder bitrate.
    let nv_bitrate = (u64::from(s.bitrate_kbps) * 8 / 10).min(100_000);
    let (format, hevc_flag) = match s.codec {
        Codec::H264 => (0, Some(0)),
        Codec::Hevc => (1, Some(1)),
        Codec::Av1 => (2, None),
    };
    // encoderCscMode = (colour space << 1) | range; BT.709 (1) or BT.2020 (2), limited range.
    let csc = if s.hdr { 2 << 1 } else { 1 << 1 };
    // mcc 41cedb89 SdpGenerator.c:187-201 negotiates legacy audio independently of x-ss.
    let nv_feature_flags = if s.encrypt_av || encryption_enabled & ENC_AUDIO != 0 {
        0x07 | 0x80 | 0x20
    } else {
        0x07 | 0x80
    };

    let mut attrs: Vec<(&str, String)> = vec![
        ("x-ml-general.featureFlags", "3".into()),
        (
            "x-ss-general.encryptionEnabled",
            encryption_enabled.to_string(),
        ),
        ("x-ss-video[0].chromaSamplingType", "0".into()),
        ("x-nv-video[0].clientViewportWd", s.width.to_string()),
        ("x-nv-video[0].clientViewportHt", s.height.to_string()),
        ("x-nv-video[0].maxFPS", s.fps.to_string()),
        ("x-nv-video[0].packetSize", packet_size.to_string()),
        ("x-nv-video[0].rateControlMode", "4".into()),
        ("x-nv-video[0].timeoutLengthMs", "7000".into()),
        ("x-nv-video[0].framesWithInvalidRefThreshold", "0".into()),
        ("x-nv-video[0].initialBitrateKbps", nv_bitrate.to_string()),
        (
            "x-nv-video[0].initialPeakBitrateKbps",
            nv_bitrate.to_string(),
        ),
        ("x-nv-vqos[0].bw.minimumBitrateKbps", nv_bitrate.to_string()),
        ("x-nv-vqos[0].bw.maximumBitrateKbps", nv_bitrate.to_string()),
        (
            "x-ml-video.configuredBitrateKbps",
            s.bitrate_kbps.to_string(),
        ),
        ("x-nv-vqos[0].fec.enable", "1".into()),
        ("x-nv-vqos[0].videoQualityScoreUpdateTime", "5000".into()),
        ("x-nv-vqos[0].qosTrafficType", "5".into()),
        ("x-nv-aqos.qosTrafficType", "4".into()),
        ("x-nv-general.featureFlags", nv_feature_flags.to_string()),
        ("x-nv-general.useReliableUdp", "13".into()),
        ("x-nv-vqos[0].fec.minRequiredFecPackets", "2".into()),
        ("x-nv-vqos[0].bllFec.enable", "0".into()),
        ("x-nv-vqos[0].drc.enable", "0".into()),
        ("x-nv-general.enableRecoveryMode", "0".into()),
        ("x-nv-video[0].videoEncoderSlicesPerFrame", "1".into()),
        ("x-nv-vqos[0].bitStreamFormat", format.to_string()),
    ];
    if let Some(flag) = hevc_flag {
        attrs.push(("x-nv-clientSupportHevc", flag.to_string()));
    }
    attrs.extend([
        (
            "x-nv-video[0].dynamicRangeMode",
            u8::from(s.hdr).to_string(),
        ),
        // 1 reference frame: this client does not implement reference-frame invalidation.
        ("x-nv-video[0].maxNumReferenceFrames", "1".into()),
        (
            "x-nv-video[0].clientRefreshRateX100",
            (u64::from(s.fps) * 100).to_string(),
        ),
        ("x-nv-audio.surround.numChannels", "2".into()),
        ("x-nv-audio.surround.channelMask", "3".into()),
        ("x-nv-audio.surround.enable", "0".into()),
        ("x-nv-audio.surround.AudioQuality", "0".into()),
        ("x-nv-aqos.packetDuration", "5".into()),
        ("x-nv-video[0].encoderCscMode", csc.to_string()),
    ]);

    let family = if address.contains(':') {
        "IPv6"
    } else {
        "IPv4"
    };
    let mut sdp =
        format!("v=0\r\no=android 0 14 IN {family} {address}\r\ns=NVIDIA Streaming Client\r\n");
    for (name, value) in attrs {
        sdp.push_str(&format!("a={name}:{value} \r\n"));
    }
    sdp.push_str(&format!("t=0 0\r\nm=video {video_port}  \r\n"));
    sdp
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings(codec: Codec, hdr: bool) -> StreamSettings {
        StreamSettings {
            width: 1920,
            height: 1080,
            fps: 60,
            bitrate_kbps: 20_000,
            codec,
            hdr,
            packet_size: DEFAULT_PACKET_SIZE,
            encrypt_av: false,
        }
    }

    #[test]
    fn describe_detects_codecs_and_flags() {
        let sdp = "v=0\r\na=sprop-parameter-sets=AAAAAU\r\na=rtpmap:98 AV1/90000\r\n\
                   a=x-ss-general.featureFlags:3\r\na=x-ss-general.encryptionSupported:7\r\n\
                   a=x-ss-general.encryptionRequested:1\r\n";
        let d = Describe::parse(sdp).unwrap();
        assert!(d.hevc && d.av1);
        assert_eq!(
            (
                d.feature_flags,
                d.encryption_supported,
                d.encryption_requested
            ),
            (3, Some(7), 1)
        );
        assert_eq!(encryption_flags(&d, false), ENC_CONTROL_V2);
        assert_eq!(encryption_flags(&d, true), 7);
        assert_eq!(
            encryption_flags(&Describe::default(), true),
            0,
            "no x-ss support: legacy"
        );
    }

    #[test]
    fn legacy_audio_opt_in_sets_nvflag_without_sunshine_extension() {
        let mut request = settings(Codec::H264, false);
        request.encrypt_av = true;
        let flags = encryption_flags(&Describe::default(), true);
        assert_eq!(flags, 0);
        let sdp = build_sdp("127.0.0.1", 47998, &request, flags, 1392);
        assert!(sdp.contains("a=x-nv-general.featureFlags:167 \r\n"));
        assert!(sdp.contains("a=x-ss-general.encryptionEnabled:0 \r\n"));
    }
    #[test]
    fn sdp_selects_codec_and_dynamic_range() {
        let hdr = build_sdp("10.0.0.2", 47998, &settings(Codec::Hevc, true), 1, 1392);
        assert!(hdr.contains("a=x-nv-vqos[0].bitStreamFormat:1 \r\n"));
        assert!(hdr.contains("a=x-nv-video[0].dynamicRangeMode:1 \r\n"));
        assert!(hdr.contains("a=x-nv-clientSupportHevc:1 \r\n"));
        assert!(hdr.contains("a=x-nv-general.useReliableUdp:13 \r\n"));
        assert!(hdr.contains("a=x-nv-vqos[0].bw.maximumBitrateKbps:16000 \r\n"));
        assert!(hdr.ends_with("t=0 0\r\nm=video 47998  \r\n"));
        let av1 = build_sdp("10.0.0.2", 47998, &settings(Codec::Av1, false), 1, 1392);
        assert!(av1.contains("bitStreamFormat:2 "));
        assert!(!av1.contains("clientSupportHevc"), "omitted for AV1");
    }

    #[test]
    fn session_url_parsing() {
        assert_eq!(
            parse_session_url("rtsp://192.168.1.2:48010").unwrap(),
            (false, 48010)
        );
        assert_eq!(
            parse_session_url("rtspenc://[::1]:48011").unwrap(),
            (true, 48011)
        );
        assert!(parse_session_url("http://x").is_err());
    }

    #[test]
    fn encrypted_envelope_round_trip() {
        let key = [5u8; 16];
        let wire = encrypt_message(&key, 1, b"OPTIONS x RTSP/1.0\r\n\r\n").unwrap();
        assert_eq!(&wire[0..4], &(0x8000_0000u32 | 22).to_be_bytes());
        // A host reply uses origin 'H'; emulate one to check the decrypt path.
        let (ct, tag) =
            aes_gcm_encrypt(&key, &rtsp_iv(1, b'H'), b"RTSP/1.0 200 OK\r\n\r\n").unwrap();
        let mut reply = (0x8000_0000u32 | ct.len() as u32).to_be_bytes().to_vec();
        reply.extend_from_slice(&1u32.to_be_bytes());
        reply.extend_from_slice(&tag);
        reply.extend_from_slice(&ct);
        let plain = decrypt_message(&key, &reply).unwrap();
        assert_eq!(Response::parse(&plain).unwrap().status, 200);
    }

    #[test]
    fn transport_port() {
        let r = Response::parse(b"RTSP/1.0 200 OK\r\nTransport: server_port=48000\r\nSession: DEADBEEFCAFE;timeout = 90\r\n\r\n")
            .unwrap();
        assert_eq!(server_port(&r).unwrap(), 48000);
        assert_eq!(r.header("session"), Some("DEADBEEFCAFE;timeout = 90"));
    }
    #[test]
    fn wolf_describe_can_use_connection_close_without_content_length() {
        // Live Wolf DESCRIBE had a body but no Content-Length; mcc treats remaining bytes as payload.
        let raw =
            b"RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\nv=0\r\na=x-nv-video[0].maxResH264:1920x1080\r\n";
        let response = Response::parse(raw).unwrap();
        response.validate(2).unwrap();
        assert!(response.body.starts_with("v=0"));
    }
    #[test]
    fn rejects_malformed_responses_and_setup_fields() {
        for raw in [
            b"RTSP/1.0 200 OK\r\nCSeq: 3\r\nCSeq: 3\r\n\r\n".as_slice(),
            b"HTTP/1.1 200 OK\r\nCSeq: 3\r\n\r\n",
            b"RTSP/1.0 200 OK\r\nCSeq: 3\r\nContent-Length: 9\r\n\r\nshort",
            b"RTSP/1.0 200 OK\r\nCSeq: 3\r\nBadHeader\r\n\r\n",
            b"RTSP/1.0 200 OK\r\nCSeq: 3",
        ] {
            assert!(Response::parse(raw).is_err());
        }
        for headers in [
            "CSeq: 4\r\nSession: ABC\r\nTransport: server_port=12\r\n",
            "CSeq: 3\r\nTransport: server_port=12\r\n",
            "CSeq: 3\r\nSession: ;timeout=90\r\nTransport: server_port=12\r\n",
            "CSeq: 3\r\nSession: ABC\r\nTransport: server_port=0\r\n",
            "CSeq: 3\r\nSession: ABC\r\nTransport: server_port=65536\r\n",
        ] {
            assert!(SetupResponse::parse(
                format!("RTSP/1.0 200 OK\r\n{headers}\r\n").as_bytes(),
                3
            )
            .is_err());
        }
        assert!(Response::parse(&vec![0; MAX_RESPONSE + 1]).is_err());
        assert!(Describe::parse("a=x-ss-general.encryptionSupported:bad").is_err());
        assert!(Describe::parse(
            "a=x-ss-general.encryptionSupported:1\r\na=x-ss-general.encryptionSupported:7"
        )
        .is_err());
    }

    #[test]
    fn invalid_settings_and_encrypted_envelope_boundaries() {
        let mut s = settings(Codec::H264, false);
        s.bitrate_kbps = 0;
        assert!(s.validate().is_err());
        s.bitrate_kbps = u32::MAX;
        assert!(s.validate().is_ok());
        assert!(build_sdp("::1", 1, &s, 1, 1392).contains("IN IPv6 ::1"));
        s.packet_size = 31;
        assert!(s.validate().is_err());
        assert!(settings(Codec::H264, true).validate().is_err());
        for url in [
            "rtsp://host:bad",
            "rtsp://host:0",
            "rtsp://host:65536",
            "rtsp://host:+2",
            "rtsp://host",
            "rtsp://user@host:1",
        ] {
            assert!(parse_session_url(url).is_err());
        }
        let key = [5; 16];
        let plain = b"RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n";
        let (ct, tag) = aes_gcm_encrypt(&key, &rtsp_iv(1, b'H'), plain).unwrap();
        let mut reply = (0x8000_0000u32 | ct.len() as u32).to_be_bytes().to_vec();
        reply.extend_from_slice(&1u32.to_be_bytes());
        reply.extend_from_slice(&tag);
        reply.extend_from_slice(&ct);
        assert!(decrypt_message(&key, &reply).is_ok());
        for offset in [0, 4, 8, 24] {
            let mut invalid = reply.clone();
            invalid[offset] ^= 1;
            assert!(decrypt_message(&key, &invalid).is_err());
        }
        let mut trailing = reply.clone();
        trailing.push(0);
        assert!(decrypt_message(&key, &trailing).is_err());
        assert!(decrypt_message(&key, &reply[..reply.len() - 1]).is_err());
        assert!(decrypt_message(&key, plain).is_err());
    }

    #[tokio::test]
    async fn encrypted_request_and_response_use_separate_nonce_directions() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let key = [7; 16];
        let peer = tokio::spawn(async move {
            let (mut socket, _) = listener.accept().await.unwrap();
            let mut header = [0; 24];
            socket.read_exact(&mut header).await.unwrap();
            assert_eq!(u32::from_be_bytes(header[4..8].try_into().unwrap()), 1);
            let length = u32::from_be_bytes(header[..4].try_into().unwrap()) & 0x7fff_ffff;
            let mut ct = vec![0; length as usize];
            socket.read_exact(&mut ct).await.unwrap();
            let request = aes_gcm_decrypt(&key, &rtsp_iv(1, b'C'), &ct, &header[8..24]).unwrap();
            assert!(String::from_utf8(request).unwrap().contains("CSeq: 1\r\n"));
            let plain = b"RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n";
            let (ct, tag) = aes_gcm_encrypt(&key, &rtsp_iv(1, b'H'), plain).unwrap();
            let mut reply = (0x8000_0000u32 | ct.len() as u32).to_be_bytes().to_vec();
            reply.extend_from_slice(&1u32.to_be_bytes());
            reply.extend_from_slice(&tag);
            reply.extend_from_slice(&ct);
            socket.write_all(&reply).await.unwrap();
        });
        let mut client = Client {
            address: "127.0.0.1".into(),
            port,
            host_header: "127.0.0.1".into(),
            cseq: 1,
            encryption: Some((key, 0)),
        };
        client
            .request("OPTIONS", "rtspenc://fixture", &[], "")
            .await
            .unwrap();
        peer.await.unwrap();
    }
}
