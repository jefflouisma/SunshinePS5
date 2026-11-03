//! Video reassembly and client observations, using protocol-notes.md §5–§7.
//!
//! A newer frame closes the preceding frame, matching the pinned official client's
//! RtpVideoQueue.c:646–695 policy; packets within a frame may arrive in any order.
//! Parity is identified by fecInfo, never by its arbitrary post-encode flags.
//! This module supports the negotiated unencrypted Sunshine short-header format.

use anyhow::Result;
use serde::Serialize;
use std::collections::BTreeMap;
use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio::net::UdpSocket;
use tokio::sync::{mpsc, watch};

use crate::clock::monotonic_seconds;
use crate::fec;
use crate::stats::Summary;

pub const HEADER_LEN: usize = 32;
pub const FRAME_HEADER_LEN: usize = 8;
const FLAG_PIC_DATA: u8 = 0x1;
pub const FRAME_TYPE_IDR: u8 = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PacketHeader {
    pub sequence_number: u16,
    pub stream_packet_index: u32,
    pub frame_index: u32,
    pub flags: u8,
    pub block: u8,
    pub last_block: u8,
    pub shard_index: u16,
    pub data_shards: u16,
    pub fec_percentage: u8,
}

impl PacketHeader {
    pub fn parse(packet: &[u8]) -> Option<Self> {
        // Version 2, the required four-byte extension, no CSRC/padding variation.
        if packet.len() <= HEADER_LEN || packet.len() > 65536 || packet[0] != 0x90 {
            return None;
        }
        let u32_at = |o: usize| u32::from_le_bytes(packet[o..o + 4].try_into().expect("4 bytes"));
        let info = u32_at(28);
        let multi = packet[27];
        let header = Self {
            sequence_number: u16::from_be_bytes([packet[2], packet[3]]),
            stream_packet_index: u32_at(16) >> 8,
            frame_index: u32_at(20),
            flags: packet[24],
            block: (multi >> 4) & 3,
            last_block: (multi >> 6) & 3,
            shard_index: ((info >> 12) & 0x3ff) as u16,
            data_shards: (info >> 22) as u16,
            fec_percentage: ((info >> 4) & 0xff) as u8,
        };
        let total = usize::from(header.data_shards) + header.parity_shards();
        if header.data_shards == 0
            || header.block > header.last_block
            || (header.fec_percentage != 0 && total > 255)
            || usize::from(header.shard_index) >= total
            || (!header.is_parity() && header.flags & FLAG_PIC_DATA == 0)
        {
            return None;
        }
        Some(header)
    }

    pub fn is_parity(&self) -> bool {
        self.shard_index >= self.data_shards
    }

    pub fn parity_shards(&self) -> usize {
        (usize::from(self.data_shards) * usize::from(self.fec_percentage)).div_ceil(100)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameHeader {
    pub host_latency_tenths_ms: u16,
    pub frame_type: u8,
    pub last_payload_len: u16,
}

impl FrameHeader {
    pub fn parse(payload: &[u8]) -> Option<Self> {
        if payload.len() < FRAME_HEADER_LEN || payload[0] != 1 {
            return None;
        }
        Some(Self {
            host_latency_tenths_ms: u16::from_le_bytes([payload[1], payload[2]]),
            frame_type: payload[3],
            last_payload_len: u16::from_le_bytes([payload[4], payload[5]]),
        })
    }

    pub fn protocol_latency(&self) -> ProtocolLatency {
        ProtocolLatency::from_wire(self.host_latency_tenths_ms)
    }
}

/// Protocol pre-packetization latency, never capture-to-final-send pipeline time.
/// Zero cannot distinguish a repeated frame from an unavailable capture timestamp.
/// 65535 is at the saturation limit, excluded from exact-value summaries.
/// The source rounds microseconds before clamping: that tick covers >=6553.45 ms.
#[derive(Debug, Clone, Copy, PartialEq, Serialize)]
pub struct ProtocolLatency {
    pub raw_tenths_ms: u16,
    pub value_ms: Option<f64>,
    pub lower_bound_ms: Option<f64>,
    pub saturated: bool,
}

impl ProtocolLatency {
    pub fn from_wire(raw: u16) -> Self {
        Self {
            raw_tenths_ms: raw,
            value_ms: (raw > 0 && raw < u16::MAX).then(|| f64::from(raw) / 10.0),
            lower_bound_ms: (raw == u16::MAX).then_some(6553.45),
            saturated: raw == u16::MAX,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct FrameRecord {
    pub frame_index: u32,
    pub frame_type: Option<u8>,
    /// Exact unsaturated protocol latency, in milliseconds; raw zero is absent.
    pub host_latency_ms: Option<f64>,
    pub protocol_latency: Option<ProtocolLatency>,
    pub first_packet_at: f64,
    /// Arrival when enough unique shards made recovery possible, excluding trailing parity.
    pub last_packet_at: f64,
    pub bytes: usize,
    pub complete: bool,
    /// Incomplete despite sufficient unique shards (e.g. invalid reconstructed frame header).
    pub recoverable_with_fec: bool,
    pub recovered_data_shards: usize,
    /// Before reconstruction. None means at least one entire block was never observed.
    pub missing_data_shards: Option<usize>,
    pub failure_reason: Option<String>,
    /// A partial final frame cannot prove network loss after observation stopped.
    pub session_boundary: bool,
}

struct Block {
    data_shards: usize,
    parity_shards: usize,
    sequence_base: u16,
    fec_percentage: u8,
    stream_index_base: Option<u32>,
    shards: Vec<Option<Vec<u8>>>,
}

impl Block {
    fn new(h: &PacketHeader) -> Self {
        let data_shards = usize::from(h.data_shards);
        let parity_shards = h.parity_shards();
        Self {
            data_shards,
            parity_shards,
            sequence_base: h.sequence_number.wrapping_sub(h.shard_index),
            fec_percentage: h.fec_percentage,
            stream_index_base: None,
            shards: vec![None; data_shards + parity_shards],
        }
    }

    fn missing(&self) -> usize {
        self.shards[..self.data_shards]
            .iter()
            .filter(|s| s.is_none())
            .count()
    }

    fn recoverable(&self) -> bool {
        self.shards.iter().flatten().count() >= self.data_shards
    }
}

struct Assembly {
    frame_index: u32,
    blocks: BTreeMap<u8, Block>,
    last_block: Option<u8>,
    first_packet_at: f64,
    last_packet_at: f64,
    ready_at: Option<f64>,
    invalid: Option<String>,
}

impl Assembly {
    fn new(frame_index: u32, now: f64) -> Self {
        Self {
            frame_index,
            blocks: BTreeMap::new(),
            last_block: None,
            first_packet_at: now,
            last_packet_at: now,
            ready_at: None,
            invalid: None,
        }
    }

    fn recoverable(&self) -> bool {
        self.last_block.is_some_and(|last| {
            (0..=last).all(|b| self.blocks.get(&b).is_some_and(Block::recoverable))
        })
    }

    // Returns false for duplicate or inconsistent packets; neither adds an equation.
    fn add(&mut self, h: &PacketHeader, payload: &[u8], now: f64) -> bool {
        if self.last_block.is_some_and(|last| last != h.last_block) {
            self.invalid = Some("conflicting last FEC block".into());
            return false;
        }
        self.last_block = Some(h.last_block);
        let block = self.blocks.entry(h.block).or_insert_with(|| Block::new(h));
        if block.data_shards != usize::from(h.data_shards)
            || block.parity_shards != h.parity_shards()
            || block.fec_percentage != h.fec_percentage
            || block.sequence_base != h.sequence_number.wrapping_sub(h.shard_index)
        {
            self.invalid = Some("conflicting FEC geometry or RTP sequence".into());
            return false;
        }
        if !h.is_parity() {
            let stream_base =
                h.stream_packet_index.wrapping_sub(u32::from(h.shard_index)) & 0x00ff_ffff;
            if block
                .stream_index_base
                .is_some_and(|base| base != stream_base)
            {
                self.invalid = Some("conflicting stream packet index".into());
                return false;
            }
            block.stream_index_base = Some(stream_base);
        }
        let entry = &mut block.shards[usize::from(h.shard_index)];
        if let Some(previous) = entry {
            if previous != payload {
                self.invalid = Some("conflicting duplicate shard".into());
            }
            return false;
        }
        *entry = Some(payload.to_vec());
        self.last_packet_at = now;
        if self.ready_at.is_none() && self.recoverable() {
            self.ready_at = Some(now);
        }
        true
    }

    fn finish(mut self) -> (FrameRecord, Option<Vec<u8>>) {
        let enough = self.recoverable();
        let missing = self.last_block.and_then(|last| {
            (0..=last).try_fold(0, |sum, b| {
                self.blocks.get(&b).map(|block| sum + block.missing())
            })
        });
        let mut recovered = 0;
        // Each byte column is independent: reconstruct only payload, because RTP/frame
        // header fields are overwritten after the host's RS encode (protocol notes §7).
        for block in self.blocks.values_mut() {
            if block.missing() > 0 && block.recoverable() {
                let size = block
                    .shards
                    .iter()
                    .flatten()
                    .map(Vec::len)
                    .max()
                    .unwrap_or(0);
                match fec::reconstruct(
                    block.data_shards,
                    block.parity_shards,
                    &mut block.shards,
                    size,
                ) {
                    Ok(count) => recovered += count,
                    Err(error) => self.invalid = Some(error.to_string()),
                }
            }
        }
        let header = self
            .blocks
            .get(&0)
            .and_then(|b| b.shards[0].as_deref())
            .and_then(FrameHeader::parse);
        let mut bitstream = None;
        if enough && self.invalid.is_none() {
            if let Some(header) = header {
                let mut bytes = Vec::new();
                let last = self.last_block.expect("recoverable frame");
                let mut valid = true;
                for (&number, block) in &self.blocks {
                    let size = block
                        .shards
                        .iter()
                        .flatten()
                        .map(Vec::len)
                        .max()
                        .unwrap_or(0);
                    for (i, shard) in block.shards.iter().take(block.data_shards).enumerate() {
                        let shard = shard.as_ref().expect("recovered data");
                        let final_shard = number == last && i + 1 == block.data_shards;
                        let len = if final_shard {
                            usize::from(header.last_payload_len)
                        } else {
                            size
                        };
                        if len == 0 || len > shard.len() || (!final_shard && shard.len() != size) {
                            valid = false;
                            break;
                        }
                        bytes.extend_from_slice(&shard[..len]);
                    }
                }
                if valid && bytes.len() > FRAME_HEADER_LEN {
                    bitstream = Some(bytes[FRAME_HEADER_LEN..].to_vec());
                } else {
                    self.invalid =
                        Some("invalid last payload length or truncated data shard".into());
                }
            } else {
                self.invalid = Some("missing or unsupported short frame header".into());
            }
        }
        if !enough && self.invalid.is_none() {
            self.invalid = Some("insufficient unique shards or unobserved FEC block".into());
        }
        let latency = header.map(|h| h.protocol_latency());
        let record = FrameRecord {
            frame_index: self.frame_index,
            frame_type: header.map(|h| h.frame_type),
            host_latency_ms: latency.and_then(|l| l.value_ms),
            protocol_latency: latency,
            first_packet_at: self.first_packet_at,
            last_packet_at: self.ready_at.unwrap_or(self.last_packet_at),
            bytes: bitstream.as_ref().map_or(0, Vec::len),
            complete: bitstream.is_some(),
            recoverable_with_fec: enough && bitstream.is_none() && missing.is_some_and(|m| m > 0),
            recovered_data_shards: recovered,
            missing_data_shards: missing,
            failure_reason: self.invalid,
            session_boundary: false,
        };
        (record, bitstream)
    }
}

/// Received frames are serial-number ordered across u32 wrap. A wholly unobserved
/// frame is a client gap with unknown cause, never evidence of a host drop.
#[derive(Default)]
pub struct FrameReassembler {
    current: Option<Assembly>,
    pub packets: u64,
    pub parity_packets: u64,
    pub frames_missing: u64,
    pub rejected_packets: u64,
    pub duplicate_or_conflicting_packets: u64,
}

impl FrameReassembler {
    pub fn push(&mut self, packet: &[u8], now: f64) -> Option<(FrameRecord, Option<Vec<u8>>)> {
        let Some(h) = PacketHeader::parse(packet).filter(|_| now.is_finite()) else {
            self.rejected_packets += 1;
            return None;
        };
        self.packets += 1;
        self.parity_packets += u64::from(h.is_parity());
        let mut finished = None;
        if let Some(current) = &self.current {
            let distance = h.frame_index.wrapping_sub(current.frame_index);
            if distance >= 1 << 31 {
                self.rejected_packets += 1;
                return None;
            }
            if distance > 0 {
                self.frames_missing += u64::from(distance - 1);
                finished = self.current.take().map(Assembly::finish);
            }
        }
        let assembly = self
            .current
            .get_or_insert_with(|| Assembly::new(h.frame_index, now));
        if !assembly.add(&h, &packet[HEADER_LEN..], now) {
            self.duplicate_or_conflicting_packets += 1;
        }
        finished
    }

    pub fn finish(&mut self) -> Option<(FrameRecord, Option<Vec<u8>>)> {
        self.current.take().map(|assembly| {
            let (mut record, bytes) = assembly.finish();
            record.session_boundary = !record.complete;
            (record, bytes)
        })
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct VideoReport {
    pub packets: u64,
    pub parity_packets: u64,
    pub frames_complete: usize,
    pub frames_incomplete: usize,
    /// Incomplete, enough unique parity, but output still could not be validated.
    pub frames_recoverable_with_fec: usize,
    pub frames_recovered_with_fec: usize,
    pub recovered_data_shards: usize,
    pub missing_data_shards: usize,
    pub frames_with_unobserved_blocks: usize,
    /// Gaps only between observed frame indices; no inferred start/end population.
    pub frames_missing: u64,
    /// Requires separate host evidence with captured/encoded/submitted populations.
    pub host_drops: Option<u64>,
    pub host_drop_reason: String,
    pub frames_at_session_boundary: usize,
    pub host_latency_observed: usize,
    pub host_latency_unavailable: usize,
    pub host_latency_saturated: usize,
    pub protocol_latency_units: String,
    pub protocol_latency_resolution_ms: f64,
    pub idr_frames: usize,
    pub fps: Option<f64>,
    pub bitrate_kbps: Option<f64>,
    pub frame_interval_ms: Option<Summary>,
    pub host_latency_ms: Option<Summary>,
    pub frame_bytes: Option<Summary>,
    pub first_frame_at: Option<f64>,
    pub elementary_stream: Option<PathBuf>,
}

#[derive(Debug, Clone, Copy)]
pub enum VideoEvent {
    FrameLost { frame_index: u32 },
}

pub struct VideoReceiver {
    pub socket: UdpSocket,
    pub host: IpAddr,
    pub port: u16,
    pub ping_payload: Vec<u8>,
    pub elementary_stream: Option<PathBuf>,
}

pub async fn run(
    rx: VideoReceiver,
    events: mpsc::UnboundedSender<VideoEvent>,
    mut stop: watch::Receiver<bool>,
) -> Result<(VideoReport, Vec<FrameRecord>)> {
    let remote = SocketAddr::new(rx.host, rx.port);
    let mut es = match &rx.elementary_stream {
        Some(path) => Some(tokio::io::BufWriter::new(
            tokio::fs::File::create(path).await?,
        )),
        None => None,
    };
    let mut ping_seq: u32 = 0;
    let mut ping_timer = tokio::time::interval(Duration::from_millis(500));
    let mut buf = vec![0u8; 65536];
    let mut assembler = FrameReassembler::default();
    let mut frames = Vec::new();
    while !*stop.borrow() {
        tokio::select! {
            changed = stop.changed() => {
                if changed.is_err() || *stop.borrow() { break; }
            },
            _ = ping_timer.tick() => {
                let ping = if rx.ping_payload.is_empty() {
                    b"PING".to_vec()
                } else {
                    ping_seq = ping_seq.wrapping_add(1);
                    let mut ping = rx.ping_payload.clone();
                    ping.extend_from_slice(&ping_seq.to_be_bytes());
                    ping
                };
                rx.socket.send_to(&ping, remote).await?;
            },
            received = rx.socket.recv_from(&mut buf) => {
                let (len, from) = received?;
                if from != remote { continue; }
                if let Some(done) = assembler.push(&buf[..len], monotonic_seconds()?) {
                    finish_frame(done, &mut frames, &mut es, &events).await?;
                }
            }
        }
    }
    if let Some(done) = assembler.finish() {
        finish_frame(done, &mut frames, &mut es, &events).await?;
    }
    if let Some(w) = es.as_mut() {
        w.flush().await?;
    }
    let report = summarize(
        &frames,
        assembler.packets,
        assembler.parity_packets,
        assembler.frames_missing,
        rx.elementary_stream,
    );
    Ok((report, frames))
}

async fn finish_frame(
    (record, bitstream): (FrameRecord, Option<Vec<u8>>),
    frames: &mut Vec<FrameRecord>,
    es: &mut Option<tokio::io::BufWriter<tokio::fs::File>>,
    events: &mpsc::UnboundedSender<VideoEvent>,
) -> Result<()> {
    if let Some(bytes) = bitstream {
        if let Some(w) = es.as_mut() {
            w.write_all(&bytes).await?;
        }
    } else if !record.session_boundary
        && events
            .send(VideoEvent::FrameLost {
                frame_index: record.frame_index,
            })
            .is_err()
    {
        anyhow::bail!("video loss event receiver closed");
    }
    frames.push(record);
    Ok(())
}

pub fn summarize(
    frames: &[FrameRecord],
    packets: u64,
    parity_packets: u64,
    frames_missing: u64,
    elementary_stream: Option<PathBuf>,
) -> VideoReport {
    let complete: Vec<_> = frames.iter().filter(|f| f.complete).collect();
    let span = match (complete.first(), complete.last()) {
        (Some(a), Some(b)) if complete.len() > 1 => Some(b.last_packet_at - a.last_packet_at),
        _ => None,
    };
    let intervals: Vec<_> = complete
        .windows(2)
        .map(|w| (w[1].last_packet_at - w[0].last_packet_at) * 1000.0)
        .collect();
    let latencies: Vec<_> = frames.iter().filter_map(|f| f.host_latency_ms).collect();
    let sizes: Vec<_> = complete.iter().map(|f| f.bytes as f64).collect();
    let total_bytes: usize = complete.iter().skip(1).map(|f| f.bytes).sum();
    VideoReport {
        packets,
        parity_packets,
        frames_complete: complete.len(),
        frames_incomplete: frames
            .iter()
            .filter(|f| !f.complete && !f.session_boundary)
            .count(),
        frames_recoverable_with_fec: frames.iter().filter(|f| f.recoverable_with_fec).count(),
        frames_recovered_with_fec: frames
            .iter()
            .filter(|f| f.complete && f.recovered_data_shards > 0)
            .count(),
        recovered_data_shards: frames.iter().map(|f| f.recovered_data_shards).sum(),
        missing_data_shards: frames.iter().filter_map(|f| f.missing_data_shards).sum(),
        frames_with_unobserved_blocks: frames
            .iter()
            .filter(|f| f.missing_data_shards.is_none())
            .count(),
        frames_missing,
        host_drops: None,
        host_drop_reason: "client frame gaps cannot establish host drops; host evidence absent"
            .into(),
        frames_at_session_boundary: frames.iter().filter(|f| f.session_boundary).count(),
        host_latency_observed: latencies.len(),
        host_latency_unavailable: frames
            .iter()
            .filter(|f| f.protocol_latency.is_none_or(|l| l.raw_tenths_ms == 0))
            .count(),
        host_latency_saturated: frames
            .iter()
            .filter(|f| f.protocol_latency.is_some_and(|l| l.saturated))
            .count(),
        protocol_latency_units: "milliseconds (wire: 0.1 ms); pre-packetization, excludes FEC/send"
            .into(),
        protocol_latency_resolution_ms: 0.1,
        idr_frames: complete
            .iter()
            .filter(|f| f.frame_type == Some(FRAME_TYPE_IDR))
            .count(),
        fps: span
            .filter(|s| *s > 0.0)
            .map(|s| (complete.len() - 1) as f64 / s),
        bitrate_kbps: span
            .filter(|s| *s > 0.0)
            .map(|s| total_bytes as f64 * 8.0 / s / 1000.0),
        frame_interval_ms: Summary::of(&intervals),
        host_latency_ms: Summary::of(&latencies),
        frame_bytes: Summary::of(&sizes),
        first_frame_at: complete.first().map(|f| f.last_packet_at),
        elementary_stream,
    }
}
#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a data or parity packet with the documented layout.
    fn packet(frame: u32, flags: u8, shard: u16, data_shards: u16, payload: &[u8]) -> Vec<u8> {
        let mut p = vec![0u8; HEADER_LEN];
        p[0] = 0x90;
        p[2..4].copy_from_slice(&shard.to_be_bytes());
        p[16..20].copy_from_slice(&(u32::from(shard) << 8).to_le_bytes());
        p[20..24].copy_from_slice(&frame.to_le_bytes());
        p[24] = flags;
        let fec_info = (u32::from(data_shards) << 22) | (u32::from(shard) << 12) | (20 << 4);
        p[28..32].copy_from_slice(&fec_info.to_le_bytes());
        p.extend_from_slice(payload);
        p
    }

    fn frame_header(latency_tenths: u16, frame_type: u8, last_len: u16) -> Vec<u8> {
        let mut h = vec![0x01];
        h.extend_from_slice(&latency_tenths.to_le_bytes());
        h.push(frame_type);
        h.extend_from_slice(&last_len.to_le_bytes());
        h.extend_from_slice(&[0, 0]);
        h
    }

    #[test]
    fn parses_header_fields() {
        let p = packet(42, FLAG_PIC_DATA, 3, 10, &[0]);
        let h = PacketHeader::parse(&p).unwrap();
        assert_eq!(
            (
                h.frame_index,
                h.shard_index,
                h.data_shards,
                h.fec_percentage
            ),
            (42, 3, 10, 20)
        );
        assert!(!h.is_parity());
        assert!(PacketHeader::parse(&p[..31]).is_none());
    }

    #[test]
    fn assembles_frame_and_trims_padding() {
        // Two 8-byte shards; the bitstream is 10 bytes, so the last shard carries 2 real bytes.
        let mut first = frame_header(52, FRAME_TYPE_IDR, 2);
        first.truncate(8);
        let second = [0xAA, 0xBB, 0, 0, 0, 0, 0, 0];
        let mut a = Assembly::new(7, 1.0);
        for (i, payload) in [first.as_slice(), second.as_slice()].iter().enumerate() {
            let p = packet(7, FLAG_PIC_DATA, i as u16, 2, payload);
            a.add(
                &PacketHeader::parse(&p).unwrap(),
                &p[HEADER_LEN..],
                1.0 + i as f64,
            );
        }
        let (record, bitstream) = a.finish();
        assert!(record.complete);
        assert_eq!(record.host_latency_ms, Some(5.2));
        assert_eq!(record.frame_type, Some(FRAME_TYPE_IDR));
        assert_eq!(bitstream.unwrap(), vec![0xAA, 0xBB]);
    }

    // Payload columns produced by nanors@a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29
    // reed_solomon_new(3, 2), reed_solomon_encode(..., 16). See fec.rs for provenance.
    #[test]
    fn pinned_parity_reconstructs_two_missing_data_shards() {
        let payloads = [
            (2, "abcd0000000000000000000000000000"),
            (3, "5e4c2d79766669444b5adbbc7dfa87e9"),
            (4, "d54fb5b13755d366e0770523070e091c"),
        ];
        let mut a = Assembly::new(9, 1.0);
        for (shard, encoded) in payloads {
            let payload = hex::decode(encoded).unwrap();
            let mut p = packet(9, FLAG_PIC_DATA, shard, 3, &payload);
            p[28..32].copy_from_slice(
                &((3u32 << 22) | (u32::from(shard) << 12) | (66 << 4)).to_le_bytes(),
            );
            a.add(&PacketHeader::parse(&p).unwrap(), &p[HEADER_LEN..], 2.0);
        }
        let (record, bitstream) = a.finish();
        assert!(
            record.complete,
            "received D unique shards must recover the frame"
        );
        assert_eq!(record.host_latency_ms, Some(5.2));
        assert_eq!(
            bitstream.unwrap(),
            hex::decode("00000165112233445566778899aabbccddeeff0010203040abcd").unwrap()
        );
    }

    #[test]
    fn duplicate_parity_does_not_inflate_recoverability() {
        let mut a = Assembly::new(9, 0.0);
        for shard in [0, 3, 3] {
            let p = packet(9, FLAG_PIC_DATA, shard, 3, &[0; 8]);
            a.add(&PacketHeader::parse(&p).unwrap(), &p[HEADER_LEN..], 0.0);
        }
        let (record, _) = a.finish();
        assert!(
            !record.recoverable_with_fec,
            "duplicate parity is one equation"
        );
    }

    #[test]
    fn rejects_missing_rtp_extension_and_impossible_fec_geometry() {
        let mut p = packet(1, FLAG_PIC_DATA, 0, 1, &[0; 8]);
        p[0] = 0x80;
        assert!(PacketHeader::parse(&p).is_none());
        p[0] = 0x90;
        p[28..32].copy_from_slice(&((256u32 << 22) | (20 << 4)).to_le_bytes());
        assert!(PacketHeader::parse(&p).is_none());
    }

    fn single_packet(frame: u32, latency: u16) -> Vec<u8> {
        let mut payload = frame_header(latency, FRAME_TYPE_IDR, 12);
        payload.extend_from_slice(&[0, 0, 1, 0x65]);
        packet(frame, FLAG_PIC_DATA | 6, 0, 1, &payload)
    }

    #[test]
    fn protocol_latency_zero_quantization_and_saturation_have_explicit_populations() {
        let mut receiver = FrameReassembler::default();
        let mut frames = Vec::new();
        for (frame, raw) in [0, 1, 65534, 65535].into_iter().enumerate() {
            if let Some((record, _)) =
                receiver.push(&single_packet(frame as u32, raw), frame as f64)
            {
                frames.push(record);
            }
        }
        frames.push(receiver.finish().unwrap().0);
        assert_eq!(frames[0].host_latency_ms, None);
        assert_eq!(frames[1].host_latency_ms, Some(0.1));
        assert_eq!(frames[2].host_latency_ms, Some(6553.4));
        assert_eq!(frames[3].host_latency_ms, None);
        assert_eq!(
            frames[3].protocol_latency.unwrap().lower_bound_ms,
            Some(6553.45)
        );
        let report = summarize(&frames, 4, 0, 0, None);
        assert_eq!(
            (
                report.host_latency_observed,
                report.host_latency_unavailable,
                report.host_latency_saturated
            ),
            (2, 1, 1)
        );
        assert_eq!(report.host_latency_ms.unwrap().count, 2);
        assert_eq!(report.protocol_latency_resolution_ms, 0.1);
        assert!(report.host_drops.is_none());
        let json = serde_json::to_value(report).unwrap();
        assert!(json["host_drops"].is_null());
    }

    #[test]
    fn four_blocks_reassemble_out_of_order_and_ignore_trailing_parity_timing() {
        let mut a = Assembly::new(12, 1.0);
        let payloads = [frame_header(52, 2, 8), vec![1; 8], vec![2; 8], vec![3; 8]];
        for (time, block) in [3u8, 1, 0, 2].into_iter().enumerate() {
            let mut p = packet(12, FLAG_PIC_DATA | 6, 0, 1, &payloads[usize::from(block)]);
            p[27] = (block << 4) | (3 << 6);
            let h = PacketHeader::parse(&p).unwrap();
            a.add(&h, &p[HEADER_LEN..], 1.0 + time as f64);
            a.add(&h, &p[HEADER_LEN..], 99.0); // exact duplicate cannot change timing
        }
        let mut p = packet(12, 0xff, 1, 1, &[0xff; 8]);
        p[27] = 3 << 6; // block 0 parity, arbitrary flags
        a.add(&PacketHeader::parse(&p).unwrap(), &p[HEADER_LEN..], 100.0);
        let (record, bytes) = a.finish();
        assert!(record.complete);
        assert_eq!(record.last_packet_at, 4.0);
        assert_eq!(
            bytes.unwrap(),
            [vec![1; 8], vec![2; 8], vec![3; 8]].concat()
        );
    }

    #[test]
    fn lost_block_is_unknown_and_duplicate_data_does_not_replace_it() {
        let mut a = Assembly::new(1, 0.0);
        let mut p = packet(1, FLAG_PIC_DATA, 0, 1, &frame_header(10, 1, 8));
        p[27] = 1 << 6;
        for now in [1.0, 2.0] {
            a.add(&PacketHeader::parse(&p).unwrap(), &p[HEADER_LEN..], now);
        }
        let (record, bytes) = a.finish();
        assert!(bytes.is_none());
        assert!(!record.complete);
        assert!(!record.recoverable_with_fec);
        assert_eq!(record.missing_data_shards, None);
        assert_eq!(record.host_latency_ms, Some(1.0));
    }

    #[test]
    fn enough_parity_but_invalid_reconstructed_header_is_attributed_separately() {
        let mut a = Assembly::new(1, 0.0);
        for (i, encoded) in [
            (1, "5566778899aabbccddeeff0010203040"),
            (2, "abcd0000000000000000000000000000"),
            (3, "5e4c2d79766669444b5adbbc7dfa87e9"),
        ] {
            let mut bytes = hex::decode(encoded).unwrap();
            if i == 3 {
                bytes[0] ^= 0xff;
            }
            let mut p = packet(1, FLAG_PIC_DATA, i, 3, &bytes);
            p[28..32]
                .copy_from_slice(&((3u32 << 22) | (u32::from(i) << 12) | (66 << 4)).to_le_bytes());
            a.add(&PacketHeader::parse(&p).unwrap(), &p[HEADER_LEN..], 1.0);
        }
        let (record, bytes) = a.finish();
        assert!(bytes.is_none());
        assert!(record.recoverable_with_fec);
        assert_eq!(record.recovered_data_shards, 1);
        assert_eq!(record.missing_data_shards, Some(1));
        assert_eq!(
            record.failure_reason.as_deref(),
            Some("missing or unsupported short frame header")
        );
        let report = summarize(&[record], 3, 1, 0, None);
        assert_eq!(report.frames_recovered_with_fec, 0);
        assert_eq!(report.frames_recoverable_with_fec, 1);
        assert_eq!(report.frames_incomplete, 1);
    }

    #[test]
    fn client_gaps_wrap_late_packets_and_stop_boundary_do_not_become_host_drops() {
        let mut receiver = FrameReassembler::default();
        assert!(receiver.push(&single_packet(u32::MAX, 10), 1.0).is_none());
        let first = receiver.push(&single_packet(0, 10), 2.0).unwrap().0;
        assert_eq!(first.frame_index, u32::MAX);
        assert!(receiver.push(&single_packet(u32::MAX, 10), 3.0).is_none());
        let second = receiver.push(&single_packet(3, 10), 4.0).unwrap().0;
        assert_eq!(second.frame_index, 0);
        assert_eq!(receiver.frames_missing, 2);
        assert_eq!(receiver.rejected_packets, 1);
        receiver.finish().unwrap();
        // A final partially observed frame is recorded, but cannot prove post-stop loss.
        let partial = packet(4, FLAG_PIC_DATA, 0, 2, &frame_header(10, 1, 8));
        receiver.push(&partial, 5.0);
        let terminal = receiver.finish().unwrap().0;
        assert!(!terminal.complete && terminal.session_boundary);
        let report = summarize(
            &[first, second, terminal],
            4,
            0,
            receiver.frames_missing,
            None,
        );
        assert_eq!(
            (
                report.frames_missing,
                report.frames_incomplete,
                report.frames_at_session_boundary
            ),
            (2, 0, 1)
        );
        assert_eq!(report.host_drops, None);
    }

    #[test]
    fn no_fec_large_block_and_invalid_last_length_are_handled_without_underflow() {
        let mut a = Assembly::new(1, 0.0);
        for i in 0..256 {
            let payload = if i == 0 {
                frame_header(0, 1, 8)
            } else {
                vec![1; 8]
            };
            let mut p = packet(1, FLAG_PIC_DATA, i, 256, &payload);
            p[28..32].copy_from_slice(&((256u32 << 22) | (u32::from(i) << 12)).to_le_bytes());
            a.add(
                &PacketHeader::parse(&p).unwrap(),
                &p[HEADER_LEN..],
                f64::from(i),
            );
        }
        assert_eq!(a.finish().1.unwrap().len(), 255 * 8);
        let mut p = single_packet(2, 10);
        p[36..38].copy_from_slice(&u16::MAX.to_le_bytes());
        let mut a = Assembly::new(2, 0.0);
        a.add(&PacketHeader::parse(&p).unwrap(), &p[HEADER_LEN..], 1.0);
        let (record, bytes) = a.finish();
        assert!(!record.complete && bytes.is_none());
        assert!(record
            .failure_reason
            .unwrap()
            .contains("invalid last payload length"));
    }

    #[test]
    fn inconsistent_fec_geometry_sequence_and_duplicate_payload_fail_closed() {
        for fault in 0..5 {
            let mut a = Assembly::new(1, 0.0);
            let p = single_packet(1, 10);
            let h = PacketHeader::parse(&p).unwrap();
            a.add(&h, &p[HEADER_LEN..], 0.0);
            let mut conflicting = p.clone();
            match fault {
                0 => conflicting[27] = 1 << 6,
                1 => conflicting[2] = 0x80,
                2 => conflicting[17] = 0x80,
                3 => conflicting[28] ^= 0x10,
                _ => conflicting[HEADER_LEN] ^= 0xff,
            }
            a.add(
                &PacketHeader::parse(&conflicting).unwrap(),
                &conflicting[HEADER_LEN..],
                1.0,
            );
            let (record, bytes) = a.finish();
            assert!(!record.complete && bytes.is_none());
            assert!(record.failure_reason.unwrap().starts_with("conflicting"));
        }
    }

    #[tokio::test]
    async fn udp_receiver_writes_last_complete_frame_and_emits_only_observed_losses() {
        let task_dir = std::env::temp_dir()
            .join("t023")
            .join(uuid::Uuid::new_v4().to_string());
        tokio::fs::create_dir_all(&task_dir).await.unwrap();
        let path = task_dir.join("stream.h264");
        let host = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let socket = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let local = socket.local_addr().unwrap();
        let (events, mut event_rx) = mpsc::unbounded_channel();
        let (stop, stop_rx) = watch::channel(false);
        let receiver = VideoReceiver {
            socket,
            host: host.local_addr().unwrap().ip(),
            port: host.local_addr().unwrap().port(),
            ping_payload: b"0123456789abcdef".to_vec(),
            elementary_stream: Some(path.clone()),
        };
        let worker = tokio::spawn(run(receiver, events, stop_rx));
        let mut ping = [0; 32];
        let (len, _) = tokio::time::timeout(Duration::from_secs(2), host.recv_from(&mut ping))
            .await
            .unwrap()
            .unwrap();
        assert_eq!(&ping[..len], b"0123456789abcdef\0\0\0\x01");
        host.send_to(&single_packet(1, 52), local).await.unwrap();
        let partial = packet(2, FLAG_PIC_DATA, 0, 2, &frame_header(0, 1, 8));
        host.send_to(&partial, local).await.unwrap();
        host.send_to(&single_packet(3, 52), local).await.unwrap();
        let event = tokio::time::timeout(Duration::from_secs(2), event_rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert!(matches!(event, VideoEvent::FrameLost { frame_index: 2 }));
        stop.send(true).unwrap();
        let (report, frames) = worker.await.unwrap().unwrap();
        assert_eq!((report.frames_complete, report.frames_incomplete), (2, 1));
        assert_eq!(frames.len(), 3);
        assert_eq!(
            tokio::fs::read(&path).await.unwrap(),
            [0, 0, 1, 0x65, 0, 0, 1, 0x65]
        );
        tokio::fs::remove_dir_all(task_dir).await.unwrap();
    }

    #[test]
    fn summary_counts_rates() {
        let frames: Vec<FrameRecord> = (0..61)
            .map(|i| FrameRecord {
                frame_index: i,
                frame_type: Some(1),
                host_latency_ms: Some(4.0),
                protocol_latency: Some(ProtocolLatency::from_wire(40)),
                first_packet_at: f64::from(i) / 60.0,
                last_packet_at: f64::from(i) / 60.0,
                bytes: 1000,
                complete: true,
                recoverable_with_fec: false,
                recovered_data_shards: 0,
                missing_data_shards: Some(0),
                failure_reason: None,
                session_boundary: false,
            })
            .collect();
        let r = summarize(&frames, 100, 10, 0, None);
        assert!((r.fps.unwrap() - 60.0).abs() < 1e-6);
        assert!((r.bitrate_kbps.unwrap() - 480.0).abs() < 1e-6);
        assert_eq!(r.host_latency_ms.unwrap().p99, 4.0);
    }
}
