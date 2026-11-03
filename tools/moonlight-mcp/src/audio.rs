//! Stereo Opus observation only; no fixture or packet ACK establishes audible console output.

use anyhow::{ensure, Context, Result};
use serde::Serialize;
use std::collections::{BTreeMap, VecDeque};

/// Loss is measured from the first accepted sequence through the last observed sequence,
/// after a bounded reorder deadline or finish. No loss before/after that span is inferred.
/// FEC datagrams are counted separately; this receiver does not claim audio FEC recovery.
#[derive(Debug, Default, Clone, Serialize)]
pub struct AudioPopulation {
    pub expected_packets: u64,
    pub received_packets: u64,
    pub missing_packets: u64,
    pub duplicates: u64,
    pub late_packets: u64,
    pub fec_packets: u64,
    pub malformed_packets: u64,
    pub decoded_packets: u64,
    pub decode_failures: u64,
    pub decoded_samples_per_channel: u64,
    pub nonzero_samples_per_channel: [u64; 2],
}
impl AudioPopulation {
    /// Exact digital non-silence over decoded samples, not a loudness or speaker test.
    /// Absent when there are no decoded samples; decode failures remain explicit.
    pub fn non_silent(&self) -> Option<bool> {
        (self.decoded_samples_per_channel > 0)
            .then(|| self.nonzero_samples_per_channel.iter().any(|&v| v > 0))
    }
    pub fn loss_fraction(&self) -> Option<f64> {
        (self.expected_packets > 0)
            .then(|| self.missing_packets as f64 / self.expected_packets as f64)
    }
}
#[derive(Debug, Clone, Serialize)]
pub struct AudioObservation {
    pub sequence: u16,
    /// Protocol timestamp in milliseconds (§8); never a 48 kHz sample counter.
    pub rtp_timestamp_ms: u32,
    pub received_us: u64,
    pub samples_per_channel: Option<usize>,
    pub peak_per_channel: [Option<f64>; 2],
    pub rms_per_channel: [Option<f64>; 2],
    pub decode_error: Option<String>,
}
struct Pending {
    sequence: u16,
    timestamp: u32,
    received: u64,
    payload: Vec<u8>,
}
pub struct AudioReceiver {
    decoder: opus::Decoder,
    duration_ms: u32,
    window: u64,
    key: Option<[u8; 16]>,
    key_id: u32,
    next: Option<u64>,
    highest: u64,
    pending: BTreeMap<u64, Pending>,
    recent: VecDeque<u64>,
    pcm: Vec<i16>,
    population: AudioPopulation,
    finished: bool,
}
impl AudioReceiver {
    pub fn new(
        duration_ms: u32,
        reorder_packets: u16,
        key: Option<[u8; 16]>,
        key_id: u32,
    ) -> Result<Self> {
        ensure!(
            [5, 10].contains(&duration_ms),
            "unsupported negotiated Opus packet duration"
        );
        ensure!(
            (1..=1024).contains(&reorder_packets),
            "audio reorder bound must be 1..1024"
        );
        Ok(Self {
            decoder: opus::Decoder::new(48000, opus::Channels::Stereo)?,
            duration_ms,
            window: u64::from(reorder_packets),
            key,
            key_id,
            next: None,
            highest: 0,
            pending: BTreeMap::new(),
            recent: VecDeque::new(),
            pcm: vec![0; 5760 * 2],
            population: AudioPopulation::default(),
            finished: false,
        })
    }
    pub fn population(&self) -> &AudioPopulation {
        &self.population
    }
    /// A caller supplies same-domain monotonic receipt timestamps; RTP time is independent.
    /// No PLC is counted as a successful decode or evidence of non-silence.
    pub fn push(&mut self, packet: &[u8], received_us: u64) -> Result<Vec<AudioObservation>> {
        ensure!(!self.finished, "audio observation already finalized");
        if !(13..=1400).contains(&packet.len())
            || packet[0] != 0x80
            || ![97, 127].contains(&(packet[1] & 0x7f))
        {
            self.population.malformed_packets += 1;
            anyhow::bail!("invalid audio RTP header, size or payload type");
        }
        if packet[1] & 0x7f == 127 {
            ensure!(
                packet.len() >= 25 && packet[12] < 2 && packet[13] == 97,
                "invalid audio FEC header"
            );
            self.population.fec_packets += 1;
            return Ok(Vec::new());
        }
        let sequence = u16::from_be_bytes([packet[2], packet[3]]);
        if self.next.is_none() {
            self.next = Some(u64::from(sequence));
            self.highest = u64::from(sequence);
        }
        let delta = i64::from(sequence.wrapping_sub(self.highest as u16) as i16);
        let Some(extended) = self.highest.checked_add_signed(delta) else {
            self.population.late_packets += 1;
            return Ok(Vec::new());
        };
        if self.pending.contains_key(&extended) || self.recent.contains(&extended) {
            self.population.duplicates += 1;
            return Ok(Vec::new());
        }
        if extended < self.next.unwrap() {
            self.population.late_packets += 1;
            return Ok(Vec::new());
        }
        self.highest = self.highest.max(extended);
        self.population.received_packets += 1;
        let mut observations = Vec::new();
        if extended >= self.next.unwrap() + self.window {
            self.drain_until(extended - self.window + 1, &mut observations)?;
        }
        self.pending.insert(
            extended,
            Pending {
                sequence,
                timestamp: u32::from_be_bytes(packet[4..8].try_into()?),
                received: received_us,
                payload: packet[12..].to_vec(),
            },
        );
        while self.pending.contains_key(&self.next.unwrap()) {
            self.drain_until(self.next.unwrap() + 1, &mut observations)?;
        }
        Ok(observations)
    }
    pub fn finish(&mut self) -> Result<Vec<AudioObservation>> {
        let mut observations = Vec::new();
        if !self.finished && self.next.is_some() {
            self.drain_until(self.highest + 1, &mut observations)?;
        }
        self.finished = true;
        Ok(observations)
    }
    fn drain_until(&mut self, end: u64, observations: &mut Vec<AudioObservation>) -> Result<()> {
        while let Some(next) = self.next.filter(|&v| v < end) {
            self.population.expected_packets += 1;
            self.next = Some(next.checked_add(1).context("audio sequence overflow")?);
            if let Some(p) = self.pending.remove(&next) {
                self.recent.push_back(next);
                if self.recent.len() > self.window as usize {
                    self.recent.pop_front();
                }
                observations.push(self.decode(p));
            } else {
                self.population.missing_packets += 1;
            }
        }
        Ok(())
    }
    fn decode(&mut self, p: Pending) -> AudioObservation {
        let mut observation = AudioObservation {
            sequence: p.sequence,
            rtp_timestamp_ms: p.timestamp,
            received_us: p.received,
            samples_per_channel: None,
            peak_per_channel: [None; 2],
            rms_per_channel: [None; 2],
            decode_error: None,
        };
        let decoded = (|| -> Result<usize> {
            let payload = if let Some(key) = &self.key {
                let mut iv = [0; 16];
                iv[..4].copy_from_slice(
                    &self
                        .key_id
                        .wrapping_add(u32::from(p.sequence))
                        .to_be_bytes(),
                );
                crate::crypto::aes_cbc_decrypt_pkcs7(key, &iv, &p.payload)?
            } else {
                p.payload
            };
            ensure!(!payload.is_empty(), "missing Opus payload");
            let n = self.decoder.decode(&payload, &mut self.pcm, false)?;
            ensure!(
                n == self.duration_ms as usize * 48,
                "Opus duration differs from negotiated population"
            );
            Ok(n)
        })();
        match decoded {
            Ok(n) => {
                self.population.decoded_packets += 1;
                self.population.decoded_samples_per_channel += n as u64;
                observation.samples_per_channel = Some(n);
                for channel in 0..2 {
                    let samples = self.pcm[..n * 2].iter().skip(channel).step_by(2);
                    let mut square = 0.0f64;
                    let mut peak = 0.0f64;
                    for &sample in samples {
                        let value = f64::from(sample) / 32768.0;
                        square += value * value;
                        peak = peak.max(value.abs());
                        if sample != 0 {
                            self.population.nonzero_samples_per_channel[channel] += 1;
                        }
                    }
                    observation.peak_per_channel[channel] = Some(peak);
                    observation.rms_per_channel[channel] = Some((square / n as f64).sqrt());
                }
            }
            Err(error) => {
                self.population.decode_failures += 1;
                observation.decode_error = Some(error.to_string());
            }
        }
        observation
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(sequence: u16, payload: &[u8]) -> Vec<u8> {
        let mut p = vec![0x80, 97];
        p.extend(sequence.to_be_bytes());
        p.extend((u32::from(sequence) * 5).to_be_bytes());
        p.extend([0; 4]);
        p.extend(payload);
        p
    }
    #[test]
    fn sequence_wrap_reordering_loss_and_missing_decode_are_separate() {
        let mut audio = AudioReceiver::new(5, 4, None, 0).unwrap();
        audio.push(&packet(65534, &[0xff]), 10).unwrap();
        audio.push(&packet(0, &[0xff]), 30).unwrap();
        audio.push(&packet(65535, &[0xff]), 20).unwrap();
        audio.push(&packet(2, &[0xff]), 50).unwrap();
        audio.push(&packet(2, &[0xff]), 51).unwrap();
        audio.finish().unwrap();
        let p = audio.population();
        assert_eq!(
            (
                p.expected_packets,
                p.received_packets,
                p.missing_packets,
                p.duplicates
            ),
            (5, 4, 1, 1)
        );
        assert_eq!(p.decoded_packets, 0);
        assert_eq!(p.decode_failures, 4);
        assert_eq!(p.non_silent(), None);
    }
    #[test]
    fn generated_stereo_opus_has_defined_sample_population() {
        let mut encoder =
            opus::Encoder::new(48000, opus::Channels::Stereo, opus::Application::LowDelay).unwrap();
        let pcm: Vec<i16> = (0..480)
            .flat_map(|i| {
                let a = ((i as f32 * 0.2).sin() * 15000.0) as i16;
                [a, -a]
            })
            .collect();
        let mut compressed = [0; 1200];
        let n = encoder.encode(&pcm, &mut compressed).unwrap();
        let mut audio = AudioReceiver::new(10, 8, None, 0).unwrap();
        let observations = audio.push(&packet(0, &compressed[..n]), 100).unwrap();
        assert_eq!(observations.len(), 1);
        assert_eq!(audio.population().decoded_samples_per_channel, 480);
        assert_eq!(audio.population().non_silent(), Some(true));
        assert!(observations[0]
            .peak_per_channel
            .iter()
            .all(|v| v.unwrap() > 0.1));
        assert!(audio.push(&[0; 12], 200).is_err());
    }
}

#[cfg(test)]
mod additional_tests {
    use super::*;
    #[test]
    fn silent_stereo_encrypted_payload_and_fec_are_distinct() {
        use aes::cipher::{block_padding::Pkcs7, BlockEncryptMut, KeyIvInit};
        let mut encoder =
            opus::Encoder::new(48000, opus::Channels::Stereo, opus::Application::LowDelay).unwrap();
        let mut encoded = [0; 1200];
        let n = encoder.encode(&[0; 480], &mut encoded).unwrap();
        let key = [7; 16];
        let key_id = u32::MAX;
        let sequence = 1u16;
        let mut iv = [0; 16];
        iv[..4].copy_from_slice(&key_id.wrapping_add(u32::from(sequence)).to_be_bytes());
        let mut buffer = vec![0; n + 16];
        buffer[..n].copy_from_slice(&encoded[..n]);
        let cipher = cbc::Encryptor::<aes::Aes128>::new((&key).into(), (&iv).into());
        let payload = cipher.encrypt_padded_mut::<Pkcs7>(&mut buffer, n).unwrap();
        let mut packet = vec![0x80, 97, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0];
        packet.extend_from_slice(payload);
        let mut audio = AudioReceiver::new(5, 4, Some(key), key_id).unwrap();
        let decoded = audio.push(&packet, 100).unwrap();
        assert_eq!(decoded[0].samples_per_channel, Some(240));
        assert_eq!(audio.population().non_silent(), Some(false));
        let mut fec = vec![0; 25];
        fec[0] = 0x80;
        fec[1] = 127;
        fec[13] = 97;
        audio.push(&fec, 101).unwrap();
        assert_eq!(audio.population().fec_packets, 1);
        assert_eq!(audio.population().received_packets, 1);
        packet[12] ^= 1;
        packet[3] = 2;
        let broken = audio.push(&packet, 102).unwrap();
        assert!(broken[0].decode_error.is_some());
        assert_eq!(broken[0].samples_per_channel, None);
        assert_eq!(audio.population().decode_failures, 1);
    }
    #[test]
    fn reorder_deadline_does_not_turn_late_or_duplicate_packets_into_decode() {
        let packet = |seq: u16| {
            let mut p = vec![0x80, 97];
            p.extend(seq.to_be_bytes());
            p.extend([0; 8]);
            p.push(0xff);
            p
        };
        let mut audio = AudioReceiver::new(5, 2, None, 0).unwrap();
        audio.push(&packet(0), 0).unwrap();
        audio.push(&packet(3), 3).unwrap();
        audio.push(&packet(1), 4).unwrap();
        assert_eq!(audio.population().late_packets, 1);
        audio.finish().unwrap();
        let p = audio.population();
        assert_eq!(
            (p.expected_packets, p.received_packets, p.missing_packets),
            (4, 2, 2)
        );
        assert_eq!(p.loss_fraction(), Some(0.5));
        assert!(audio.finish().unwrap().is_empty());
        assert!(audio.push(&packet(4), 5).is_err());
    }
}
