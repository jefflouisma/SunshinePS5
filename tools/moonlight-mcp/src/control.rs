//! Gen7 encrypted control over the locked `rusty_enet` transport.
//!
//! Wire constants, IVs and layouts: protocol-notes §9 (mcc 41cedb8 ControlStream.c),
//! transport/channels/linger: §11. Call `poll` regularly on a path independent of media work;
//! it services ENet and sends the required reliable application ping every 100 ms.

use anyhow::{anyhow, bail, ensure, Context, Result};
use rusty_enet::{Event, EventNoRef, Host, HostSettings, Packet, PeerID};
use serde::Serialize;
use std::net::{SocketAddr, UdpSocket};
use std::time::{Duration, Instant};

use crate::crypto::{aes_gcm16_decrypt, aes_gcm16_encrypt, aes_gcm_decrypt, aes_gcm_encrypt};

// mcc Limelight-internal.h:56–65, ControlStream.c:203–217,297–298 (notes §§9,11).
pub const GENERIC_CHANNEL: u8 = 0;
pub const URGENT_CHANNEL: u8 = 1;
pub const GAMEPAD_CHANNEL_BASE: u8 = 0x10;
pub const SENSOR_CHANNEL_BASE: u8 = 0x20;
pub const CHANNEL_COUNT: usize = 0x30;
pub const PING: u16 = 0x0200;
pub const INPUT: u16 = 0x0206;
pub const IDR: u16 = 0x0302;
pub const START_B: u16 = 0x0307;
pub const PING_PAYLOAD: [u8; 8] = [4, 0, 0, 0, 0, 0, 0, 0];
const PING_INTERVAL: Duration = Duration::from_millis(100);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const LINGER: Duration = Duration::from_secs(2);

/// The ciphertext, channel and ENet flags are one send operation; the envelope is not UDP framing.
#[derive(Debug, PartialEq, Eq)]
pub struct OutgoingControl {
    pub bytes: Vec<u8>,
    pub channel: u8,
    pub reliable: bool,
}

#[derive(Debug, PartialEq, Eq)]
pub struct ControlMessage {
    pub sequence: u32,
    pub kind: u16,
    pub payload: Vec<u8>,
}

/// Per-direction sequence counters start at zero; V2 distinguishes host/client IV domains.
/// Host sequence ordering is deliberately unrestricted, matching mcc (notes §9).
pub struct ControlCipher {
    key: [u8; 16],
    v2: bool,
    next_sequence: Option<u32>,
}

impl ControlCipher {
    pub fn new(key: [u8; 16], v2: bool) -> Self {
        Self {
            key,
            v2,
            next_sequence: Some(0),
        }
    }

    pub fn encode(
        &mut self,
        kind: u16,
        payload: &[u8],
        channel: u8,
        reliable: bool,
    ) -> Result<OutgoingControl> {
        ensure!(
            usize::from(channel) < CHANNEL_COUNT,
            "invalid control channel"
        );
        // Four inner bytes plus sequence/tag must fit the envelope's u16 length.
        ensure!(
            payload.len() <= usize::from(u16::MAX) - 24,
            "control payload too large"
        );
        let sequence = self
            .next_sequence
            .ok_or_else(|| anyhow!("control sequence exhausted"))?;
        let mut plain = Vec::with_capacity(4 + payload.len());
        plain.extend_from_slice(&kind.to_le_bytes());
        plain.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        plain.extend_from_slice(payload);
        let (ct, tag) = if self.v2 {
            aes_gcm_encrypt(&self.key, &v2_iv(sequence, b'C'), &plain)?
        } else {
            aes_gcm16_encrypt(&self.key, &legacy_iv(sequence), &plain)?
        };
        let mut bytes = Vec::with_capacity(24 + ct.len());
        bytes.extend_from_slice(&1u16.to_le_bytes());
        bytes.extend_from_slice(&((20 + ct.len()) as u16).to_le_bytes());
        bytes.extend_from_slice(&sequence.to_le_bytes());
        bytes.extend_from_slice(&tag);
        bytes.extend_from_slice(&ct);
        self.next_sequence = sequence.checked_add(1);
        Ok(OutgoingControl {
            bytes,
            channel,
            reliable,
        })
    }

    pub fn decode_host(&self, bytes: &[u8]) -> Result<ControlMessage> {
        ensure!(bytes.len() >= 28, "control envelope truncated");
        ensure!(
            bytes[..2] == 1u16.to_le_bytes(),
            "unencrypted control packet"
        );
        let length = usize::from(u16::from_le_bytes(bytes[2..4].try_into()?));
        ensure!(
            length + 4 == bytes.len(),
            "control envelope length mismatch"
        );
        let sequence = u32::from_le_bytes(bytes[4..8].try_into()?);
        let plain = if self.v2 {
            aes_gcm_decrypt(
                &self.key,
                &v2_iv(sequence, b'H'),
                &bytes[24..],
                &bytes[8..24],
            )?
        } else {
            aes_gcm16_decrypt(&self.key, &legacy_iv(sequence), &bytes[24..], &bytes[8..24])?
        };
        ensure!(plain.len() >= 4, "control inner header truncated");
        let length = usize::from(u16::from_le_bytes(plain[2..4].try_into()?));
        ensure!(length + 4 == plain.len(), "control inner length mismatch");
        Ok(ControlMessage {
            sequence,
            kind: u16::from_le_bytes(plain[..2].try_into()?),
            payload: plain[4..].to_vec(),
        })
    }
}

fn v2_iv(sequence: u32, origin: u8) -> [u8; 12] {
    let mut iv = [0; 12];
    iv[..4].copy_from_slice(&sequence.to_le_bytes());
    iv[10] = origin;
    iv[11] = b'C';
    iv
}

fn legacy_iv(sequence: u32) -> [u8; 16] {
    let mut iv = [0; 16];
    iv[0] = sequence as u8;
    iv
}

/// Raw metadata retains the protocol's units rather than presenting an invented calibration.
/// Coordinates use 1/50000; minimum display luminance uses 1/10000 nit; other luminances use nits.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct HdrMetadata {
    pub display_primaries: [[u16; 2]; 3],
    pub white_point: [u16; 2],
    pub max_display_luminance: u16,
    pub min_display_luminance: u16,
    pub max_content_light_level: u16,
    pub max_frame_average_light_level: u16,
    pub max_full_frame_luminance: u16,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub enum ControlEvent {
    Hdr {
        enabled: bool,
        metadata: HdrMetadata,
    },
    Rumble {
        controller: u16,
        low_frequency: u16,
        high_frequency: u16,
    },
    RumbleTriggers {
        controller: u16,
        left: u16,
        right: u16,
    },
    Termination {
        hresult: u32,
    },
    Disconnected {
        data: u32,
    },
    /// Authenticated extensions are preserved without mistaking them for delivery evidence.
    Extension {
        kind: u16,
        payload: Vec<u8>,
    },
}

impl ControlMessage {
    pub fn event(self) -> Result<ControlEvent> {
        let p = &self.payload;
        let word = |offset| u16::from_le_bytes([p[offset], p[offset + 1]]);
        Ok(match self.kind {
            0x010e => {
                ensure!(p.len() == 27, "HDR payload must be 27 bytes");
                ControlEvent::Hdr {
                    enabled: p[0] != 0,
                    metadata: HdrMetadata {
                        display_primaries: [
                            [word(1), word(3)],
                            [word(5), word(7)],
                            [word(9), word(11)],
                        ],
                        white_point: [word(13), word(15)],
                        max_display_luminance: word(17),
                        min_display_luminance: word(19),
                        max_content_light_level: word(21),
                        max_frame_average_light_level: word(23),
                        max_full_frame_luminance: word(25),
                    },
                }
            }
            0x010b => {
                ensure!(p.len() == 10, "rumble payload must be 10 bytes");
                ControlEvent::Rumble {
                    controller: word(4),
                    low_frequency: word(6),
                    high_frequency: word(8),
                }
            }
            0x5500 => {
                ensure!(p.len() == 6, "trigger rumble payload must be 6 bytes");
                ControlEvent::RumbleTriggers {
                    controller: word(0),
                    left: word(2),
                    right: word(4),
                }
            }
            0x0109 => {
                ensure!(p.len() == 4, "termination payload must be four bytes");
                ControlEvent::Termination {
                    hresult: u32::from_be_bytes(p.as_slice().try_into()?),
                }
            }
            kind => ControlEvent::Extension {
                kind,
                payload: self.payload,
            },
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ControlObservation {
    /// Monotonic microseconds since this connection began, never a host timestamp.
    pub received_elapsed_us: u64,
    pub event: ControlEvent,
}

pub struct ControlConnection {
    host: Host<UdpSocket>,
    peer: PeerID,
    cipher: ControlCipher,
    connected: bool,
    started: Instant,
    last_ping: Instant,
}

impl ControlConnection {
    /// Bind an ephemeral same-family UDP socket and connect with SETUP's exact port/connect-data.
    /// CONNECT requests 48 channels; fall back to channel zero if the peer offers fewer (§11).
    pub async fn connect(
        remote: SocketAddr,
        connect_data: u32,
        key: [u8; 16],
        v2: bool,
    ) -> Result<Self> {
        ensure!(remote.port() != 0, "invalid control port");
        let local = if remote.is_ipv4() {
            "0.0.0.0:0"
        } else {
            "[::]:0"
        };
        let socket = UdpSocket::bind(local)?;
        let mut host = Host::new(
            socket,
            HostSettings {
                peer_limit: 1,
                channel_limit: CHANNEL_COUNT,
                ..HostSettings::default()
            },
        )
        .map_err(|e| anyhow!("create ENet host: {e:?}"))?;
        let peer = host.connect(remote, CHANNEL_COUNT, connect_data)?.id();
        host.peer_mut(peer).set_timeout(2, 10_000, 10_000);
        host.peer_mut(peer).set_ping_interval(500);
        let started = Instant::now();
        loop {
            if started.elapsed() >= CONNECT_TIMEOUT {
                bail!("control connect timed out");
            }
            match host.service()?.map(Event::no_ref) {
                Some(EventNoRef::Connect { .. }) => break,
                Some(EventNoRef::Disconnect { .. }) => bail!("control disconnected during connect"),
                Some(EventNoRef::Receive { .. }) => bail!("control data before connect"),
                None => tokio::time::sleep(Duration::from_millis(1)).await,
            }
        }
        let mut connection = Self {
            host,
            peer,
            cipher: ControlCipher::new(key, v2),
            connected: true,
            started,
            last_ping: Instant::now(),
        };
        connection.send(IDR, &[0, 0], GENERIC_CHANNEL, true)?;
        connection.send(START_B, &[0], GENERIC_CHANNEL, true)?;
        connection.send(PING, &PING_PAYLOAD, GENERIC_CHANNEL, true)?;
        Ok(connection)
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn send(&mut self, kind: u16, payload: &[u8], channel: u8, reliable: bool) -> Result<()> {
        self.send_with(kind, payload, channel, reliable, false)
    }

    /// `sequenced` selects ENet unreliable-sequenced delivery for unreliable packets, which
    /// moonlight-common-c uses for input (flags 0, InputStream.c); other unreliable traffic
    /// (for example 0x5502 FEC status) stays unsequenced.
    fn send_with(
        &mut self,
        kind: u16,
        payload: &[u8],
        channel: u8,
        reliable: bool,
        sequenced: bool,
    ) -> Result<()> {
        ensure!(self.connected, "control connection is closed");
        ensure!(
            usize::from(channel) < CHANNEL_COUNT,
            "invalid control channel"
        );
        let channel = if usize::from(channel) < self.host.peer(self.peer).channel_count() {
            channel
        } else {
            GENERIC_CHANNEL
        };
        let outgoing = self.cipher.encode(kind, payload, channel, reliable)?;
        let packet = if outgoing.reliable {
            Packet::reliable(outgoing.bytes)
        } else if sequenced {
            Packet::unreliable(outgoing.bytes)
        } else {
            Packet::unreliable_unsequenced(outgoing.bytes)
        };
        self.host
            .peer_mut(self.peer)
            .send(outgoing.channel, &packet)
            .context("send control packet")?;
        self.host.flush();
        Ok(())
    }

    pub fn request_idr(&mut self) -> Result<()> {
        self.send(IDR, &[0, 0], URGENT_CHANNEL, true)
    }

    pub fn send_input(&mut self, payload: &[u8], channel: u8, reliable: bool) -> Result<()> {
        self.send_with(INPUT, payload, channel, reliable, true)
    }

    /// Processes at most one event so a busy peer cannot monopolize the caller's input loop.
    pub fn poll(&mut self) -> Result<Option<ControlObservation>> {
        if !self.connected {
            return Ok(None);
        }
        if self.last_ping.elapsed() >= PING_INTERVAL {
            self.send(PING, &PING_PAYLOAD, GENERIC_CHANNEL, true)?;
            self.last_ping = Instant::now();
        }
        let event = match self.host.service()?.map(Event::no_ref) {
            Some(EventNoRef::Receive { packet, .. }) => {
                self.cipher.decode_host(packet.data())?.event()?
            }
            Some(EventNoRef::Disconnect { data, .. }) => {
                self.connected = false;
                ControlEvent::Disconnected { data }
            }
            Some(EventNoRef::Connect { .. }) | None => return Ok(None),
        };
        if matches!(event, ControlEvent::Termination { .. }) {
            self.host.peer_mut(self.peer).disconnect_now(0);
            self.connected = false;
        }
        Ok(Some(ControlObservation {
            received_elapsed_us: u64::try_from(self.started.elapsed().as_micros())?,
            event,
        }))
    }

    /// Official teardown is an ENet disconnect, with a bounded two-second linger (§11).
    /// No RTSP TEARDOWN is defined by the pinned Sunshine handshake.
    pub async fn disconnect(&mut self) -> Result<()> {
        if !self.connected {
            return Ok(());
        }
        self.host.peer_mut(self.peer).disconnect_later(0);
        let until = Instant::now() + LINGER;
        let result = loop {
            match self.host.service().map(|e| e.map(Event::no_ref)) {
                Ok(Some(EventNoRef::Disconnect { .. })) => break Ok(()),
                Err(e) => break Err(anyhow!(e)),
                _ if Instant::now() >= until => break Ok(()),
                _ => tokio::time::sleep(Duration::from_millis(1)).await,
            }
        };
        // Also runs after linger expires; no peer/session survives local teardown.
        self.host.peer_mut(self.peer).disconnect_now(0);
        self.connected = false;
        result
    }
}

impl Drop for ControlConnection {
    fn drop(&mut self) {
        if self.connected {
            self.host.peer_mut(self.peer).disconnect_now(0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusty_enet::PacketKind;

    const KEY: [u8; 16] = [0x17; 16];

    fn host_packet(sequence: u32, kind: u16, payload: &[u8], v2: bool) -> Vec<u8> {
        let mut plain = kind.to_le_bytes().to_vec();
        plain.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        plain.extend_from_slice(payload);
        let (ct, tag) = if v2 {
            aes_gcm_encrypt(&KEY, &v2_iv(sequence, b'H'), &plain).unwrap()
        } else {
            aes_gcm16_encrypt(&KEY, &legacy_iv(sequence), &plain).unwrap()
        };
        let mut bytes = 1u16.to_le_bytes().to_vec();
        bytes.extend_from_slice(&((20 + ct.len()) as u16).to_le_bytes());
        bytes.extend_from_slice(&sequence.to_le_bytes());
        bytes.extend_from_slice(&tag);
        bytes.extend_from_slice(&ct);
        bytes
    }

    #[test]
    fn authentication_lengths_directions_and_sequence_exhaustion() {
        let mut cipher = ControlCipher::new(KEY, true);
        let host = host_packet(77, 0x0109, &0x80030023u32.to_be_bytes(), true);
        assert_eq!(cipher.decode_host(&host).unwrap().sequence, 77);
        // Arbitrary host sequences, including lower values and repeats, match the official client.
        assert!(cipher
            .decode_host(&host_packet(0, 0x0109, &[0; 4], true))
            .is_ok());
        assert!(cipher.decode_host(&host).is_ok());
        let outgoing = cipher.encode(PING, &PING_PAYLOAD, 0, true).unwrap();
        assert!(
            cipher.decode_host(&outgoing.bytes).is_err(),
            "direction binds the IV"
        );
        for offset in [2, 4, 8, 24] {
            let mut tampered = host.clone();
            tampered[offset] ^= 1;
            assert!(cipher.decode_host(&tampered).is_err());
        }
        assert!(cipher.decode_host(&host[..host.len() - 1]).is_err());
        let mut extra = host.clone();
        extra.push(0);
        assert!(cipher.decode_host(&extra).is_err());
        cipher.next_sequence = Some(u32::MAX);
        assert!(cipher.encode(PING, &PING_PAYLOAD, 0, true).is_ok());
        assert!(cipher.encode(PING, &PING_PAYLOAD, 0, true).is_err());
        assert!(cipher.encode(PING, &[], CHANNEL_COUNT as u8, true).is_err());
        assert!(ControlCipher::new(KEY, true)
            .encode(PING, &vec![0; 65535], 0, true)
            .is_err());
    }

    #[test]
    fn legacy_iv_and_inner_length_rejection() {
        let cipher = ControlCipher::new(KEY, false);
        let bytes = host_packet(257, 0x0109, &[0; 4], false);
        assert_eq!(cipher.decode_host(&bytes).unwrap().sequence, 257);
        assert!(ControlCipher::new(KEY, true).decode_host(&bytes).is_err());
        let mut sender = ControlCipher::new(KEY, false);
        let packet = sender.encode(PING, &PING_PAYLOAD, 0, true).unwrap();
        let plain = aes_gcm16_decrypt(
            &KEY,
            &legacy_iv(0),
            &packet.bytes[24..],
            &packet.bytes[8..24],
        )
        .unwrap();
        assert_eq!(&plain[..4], &[0, 2, 8, 0]);
        // Authenticated ciphertext with a dishonest inner length must still be rejected.
        let (ct, tag) = aes_gcm_encrypt(&KEY, &v2_iv(0, b'H'), &[9, 1, 4, 0, 0]).unwrap();
        let mut wrong = host_packet(0, 0x0109, &[0], true);
        wrong[8..24].copy_from_slice(&tag);
        wrong[24..].copy_from_slice(&ct);
        assert!(ControlCipher::new(KEY, true).decode_host(&wrong).is_err());
    }

    #[test]
    fn metadata_rumble_and_termination_layouts() {
        let cipher = ControlCipher::new(KEY, true);
        let mut hdr = vec![1];
        for value in 1u16..=13 {
            hdr.extend_from_slice(&(value * 100).to_le_bytes());
        }
        let event = cipher
            .decode_host(&host_packet(0, 0x010e, &hdr, true))
            .unwrap()
            .event()
            .unwrap();
        let ControlEvent::Hdr { enabled, metadata } = event else {
            panic!("HDR event");
        };
        assert!(enabled);
        assert_eq!(
            metadata.display_primaries,
            [[100, 200], [300, 400], [500, 600]]
        );
        assert_eq!(metadata.white_point, [700, 800]);
        assert_eq!(metadata.max_display_luminance, 900);
        assert_eq!(metadata.min_display_luminance, 1000);
        assert_eq!(metadata.max_content_light_level, 1100);
        assert_eq!(metadata.max_frame_average_light_level, 1200);
        assert_eq!(metadata.max_full_frame_luminance, 1300);
        let rumble = hex::decode("eeffc000030034127856").unwrap();
        assert_eq!(
            cipher
                .decode_host(&host_packet(1, 0x010b, &rumble, true))
                .unwrap()
                .event()
                .unwrap(),
            ControlEvent::Rumble {
                controller: 3,
                low_frequency: 0x1234,
                high_frequency: 0x5678
            }
        );
        assert_eq!(
            cipher
                .decode_host(&host_packet(2, 0x0109, &[0x80, 3, 0, 0x23], true))
                .unwrap()
                .event()
                .unwrap(),
            ControlEvent::Termination {
                hresult: 0x80030023
            }
        );
        for (kind, length) in [(0x010e, 27), (0x010b, 10), (0x5500, 6), (0x0109, 4)] {
            assert!(cipher
                .decode_host(&host_packet(0, kind, &vec![0; length - 1], true))
                .unwrap()
                .event()
                .is_err());
        }
    }

    async fn loopback(channel_limit: usize, terminate: bool) {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        let remote = socket.local_addr().unwrap();
        let mut host = Host::new(
            socket,
            HostSettings {
                peer_limit: 1,
                channel_limit,
                ..HostSettings::default()
            },
        )
        .unwrap();
        let server = tokio::spawn(async move {
            let mut received = Vec::new();
            let mut disconnected = false;
            let mut sent = false;
            let until = Instant::now() + Duration::from_secs(5);
            while Instant::now() < until {
                match host.service().unwrap() {
                    Some(Event::Connect { data, .. }) => assert_eq!(data, 0x12345678),
                    Some(Event::Receive {
                        peer,
                        channel_id,
                        packet,
                    }) => {
                        assert_eq!(packet.kind(), PacketKind::Reliable);
                        let bytes = packet.data();
                        let sequence = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
                        let plain = aes_gcm_decrypt(
                            &KEY,
                            &v2_iv(sequence, b'C'),
                            &bytes[24..],
                            &bytes[8..24],
                        )
                        .unwrap();
                        let kind = u16::from_le_bytes(plain[..2].try_into().unwrap());
                        received.push((sequence, kind, channel_id, plain[4..].to_vec()));
                        if received.len() >= 6 && !sent {
                            // Both transitions are retained, including the arbitrary repeated host counter.
                            for enabled in [1, 0] {
                                let mut hdr = vec![0; 27];
                                hdr[0] = enabled;
                                peer.send(0, &Packet::reliable(host_packet(9, 0x010e, &hdr, true)))
                                    .unwrap();
                            }
                            peer.send(0, &Packet::reliable(host_packet(0, 0x010b, &[0; 10], true)))
                                .unwrap();
                            peer.send(0, &Packet::reliable(host_packet(1, 0x5500, &[0; 6], true)))
                                .unwrap();
                            if terminate {
                                peer.send(
                                    0,
                                    &Packet::reliable(host_packet(
                                        2,
                                        0x0109,
                                        &0x80030023u32.to_be_bytes(),
                                        true,
                                    )),
                                )
                                .unwrap();
                            }
                            sent = true;
                        }
                    }
                    Some(Event::Disconnect { .. }) => {
                        disconnected = true;
                        break;
                    }
                    None => tokio::time::sleep(Duration::from_millis(1)).await,
                }
            }
            assert!(disconnected, "host must observe ENet teardown");
            received
        });
        let mut client = ControlConnection::connect(remote, 0x12345678, KEY, true)
            .await
            .unwrap();
        client.request_idr().unwrap();
        client
            .send_input(&[1, 2, 3], GAMEPAD_CHANNEL_BASE + 2, true)
            .unwrap();
        client.last_ping = Instant::now() - PING_INTERVAL;
        let until = Instant::now() + Duration::from_secs(5);
        let mut observations = Vec::new();
        while observations.len() < if terminate { 5 } else { 4 } {
            assert!(Instant::now() < until, "missing host events");
            if let Some(event) = client.poll().unwrap() {
                observations.push(event);
            }
            tokio::time::sleep(Duration::from_millis(1)).await;
        }
        assert!(matches!(
            observations[0].event,
            ControlEvent::Hdr { enabled: true, .. }
        ));
        assert!(matches!(
            observations[1].event,
            ControlEvent::Hdr { enabled: false, .. }
        ));
        assert!(matches!(observations[2].event, ControlEvent::Rumble { .. }));
        assert!(matches!(
            observations[3].event,
            ControlEvent::RumbleTriggers { .. }
        ));
        assert!(observations
            .windows(2)
            .all(|v| v[0].received_elapsed_us <= v[1].received_elapsed_us));
        if terminate {
            assert!(!client.is_connected());
        }
        client.disconnect().await.unwrap();
        client.disconnect().await.unwrap();
        assert!(client.send_input(&[], 0, true).is_err());
        let received = server.await.unwrap();
        assert_eq!(
            received[..3]
                .iter()
                .map(|p| (p.0, p.1, p.2))
                .collect::<Vec<_>>(),
            [(0, IDR, 0), (1, START_B, 0), (2, PING, 0)]
        );
        let urgent = received.iter().find(|p| p.1 == IDR && p.0 != 0).unwrap();
        assert_eq!(
            urgent.2,
            if channel_limit == 1 {
                0
            } else {
                URGENT_CHANNEL
            }
        );
        let input = received.iter().find(|p| p.1 == INPUT).unwrap();
        assert_eq!(
            input.2,
            if channel_limit == 1 {
                0
            } else {
                GAMEPAD_CHANNEL_BASE + 2
            }
        );
        assert_eq!(input.3, [1, 2, 3]);
        let pings: Vec<_> = received.iter().filter(|p| p.1 == PING).collect();
        assert!(pings.len() >= 2);
        assert!(pings.iter().all(|p| p.2 == 0 && p.3 == PING_PAYLOAD));
    }

    #[tokio::test]
    async fn enet_loopback_keepalive_events_and_graceful_teardown() {
        loopback(CHANNEL_COUNT, false).await;
    }

    #[tokio::test]
    async fn enet_loopback_channel_fallback_and_host_termination() {
        loopback(1, true).await;
    }
}
