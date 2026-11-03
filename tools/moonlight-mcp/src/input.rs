//! Input packets carried by the control stream (protocol-notes §12).
//!
//! Every packet starts with `NV_INPUT_HEADER`: a big-endian u32 size (bytes after the size
//! field) and a little-endian u32 magic. Field endianness then varies by packet type.

use anyhow::{bail, ensure, Result};
use serde::{Deserialize, Serialize};

pub const CHANNEL_GAMEPAD_BASE: u8 = 0x10;
pub const CHANNEL_SENSOR_BASE: u8 = 0x20;

const MAGIC_MULTI_CONTROLLER: u32 = 0x0C;
const MAGIC_CONTROLLER_ARRIVAL: u32 = 0x5500_0004;
const MAGIC_CONTROLLER_TOUCH: u32 = 0x5500_0005;
const MAGIC_CONTROLLER_MOTION: u32 = 0x5500_0006;

/// Button flags (§12.5). Bits above 0xFFFF travel in the multi-controller `buttonFlags2`.
pub mod button {
    pub const UP: u32 = 0x0001;
    pub const DOWN: u32 = 0x0002;
    pub const LEFT: u32 = 0x0004;
    pub const RIGHT: u32 = 0x0008;
    pub const START: u32 = 0x0010;
    pub const BACK: u32 = 0x0020;
    pub const LS_CLICK: u32 = 0x0040;
    pub const RS_CLICK: u32 = 0x0080;
    pub const LB: u32 = 0x0100;
    pub const RB: u32 = 0x0200;
    pub const HOME: u32 = 0x0400;
    pub const A: u32 = 0x1000;
    pub const B: u32 = 0x2000;
    pub const X: u32 = 0x4000;
    pub const Y: u32 = 0x8000;
    pub const PADDLE1: u32 = 0x01_0000;
    pub const PADDLE2: u32 = 0x02_0000;
    pub const PADDLE3: u32 = 0x04_0000;
    pub const PADDLE4: u32 = 0x08_0000;
    pub const TOUCHPAD: u32 = 0x10_0000;
    pub const MISC: u32 = 0x20_0000;
}

/// Accepts Xbox names and their PlayStation equivalents.
pub fn button_by_name(name: &str) -> Result<u32> {
    use button::*;
    Ok(match name.to_ascii_lowercase().as_str() {
        "a" | "cross" => A,
        "b" | "circle" => B,
        "x" | "square" => X,
        "y" | "triangle" => Y,
        "up" | "dpad_up" => UP,
        "down" | "dpad_down" => DOWN,
        "left" | "dpad_left" => LEFT,
        "right" | "dpad_right" => RIGHT,
        "start" | "options" => START,
        "back" | "select" | "create" | "share" => BACK,
        "ls" | "l3" | "left_stick" => LS_CLICK,
        "rs" | "r3" | "right_stick" => RS_CLICK,
        "lb" | "l1" => LB,
        "rb" | "r1" => RB,
        "home" | "guide" | "ps" => HOME,
        "touchpad" => TOUCHPAD,
        "misc" | "mute" => MISC,
        "paddle1" => PADDLE1,
        "paddle2" => PADDLE2,
        "paddle3" => PADDLE3,
        "paddle4" => PADDLE4,
        other => bail!("unknown button {other:?}"),
    })
}

/// Controller types and capability bits for the arrival packet (§12.5).
pub const CONTROLLER_TYPE_PS: u8 = 0x02;
pub mod capability {
    pub const ANALOG_TRIGGERS: u16 = 0x01;
    pub const RUMBLE: u16 = 0x02;
    pub const TOUCHPAD: u16 = 0x08;
    pub const ACCEL: u16 = 0x10;
    pub const GYRO: u16 = 0x20;
}

/// The full state of one controller; every multi-controller packet carries all of it.
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct GamepadState {
    pub buttons: u32,
    pub left_trigger: u8,
    pub right_trigger: u8,
    pub left_stick_x: i16,
    pub left_stick_y: i16,
    pub right_stick_x: i16,
    pub right_stick_y: i16,
}

fn header(size_after_field: usize, magic: u32) -> Vec<u8> {
    let mut p = Vec::with_capacity(4 + size_after_field);
    p.extend_from_slice(&(size_after_field as u32).to_be_bytes());
    p.extend_from_slice(&magic.to_le_bytes());
    p
}

/// Multi-controller packet: 34 bytes, magic 0x0C. `active_mask` must include this controller's
/// bit or the host frees the pad and drops the event.
pub fn multi_controller(controller: u8, active_mask: u16, s: &GamepadState) -> Vec<u8> {
    let mut p = header(30, MAGIC_MULTI_CONTROLLER);
    p.extend_from_slice(&0x001Au16.to_le_bytes()); // headerB
    p.extend_from_slice(&i16::from(controller).to_le_bytes());
    p.extend_from_slice(&active_mask.to_le_bytes());
    p.extend_from_slice(&0x0014u16.to_le_bytes()); // midB
    p.extend_from_slice(&(s.buttons as u16).to_le_bytes());
    p.push(s.left_trigger);
    p.push(s.right_trigger);
    for axis in [
        s.left_stick_x,
        s.left_stick_y,
        s.right_stick_x,
        s.right_stick_y,
    ] {
        p.extend_from_slice(&axis.to_le_bytes());
    }
    p.extend_from_slice(&0x009Cu16.to_le_bytes()); // tailA
    p.extend_from_slice(&((s.buttons >> 16) as u16).to_le_bytes()); // buttonFlags2
    p.extend_from_slice(&0x0055u16.to_le_bytes()); // tailB
    p
}

/// Controller arrival (Sunshine extension): 16 bytes.
pub fn controller_arrival(
    controller: u8,
    controller_type: u8,
    capabilities: u16,
    supported_buttons: u32,
) -> Vec<u8> {
    let mut p = header(12, MAGIC_CONTROLLER_ARRIVAL);
    p.push(controller);
    p.push(controller_type);
    p.extend_from_slice(&capabilities.to_le_bytes());
    p.extend_from_slice(&supported_buttons.to_le_bytes());
    p
}

/// Controller motion (Sunshine extension): 24 bytes; type 1 = accelerometer (m/s²), 2 = gyro
/// (deg/s).
pub fn controller_motion(controller: u8, motion_type: u8, xyz: [f32; 3]) -> Vec<u8> {
    let mut p = header(20, MAGIC_CONTROLLER_MOTION);
    p.push(controller);
    p.push(motion_type);
    p.extend_from_slice(&[0, 0]);
    for v in xyz {
        p.extend_from_slice(&v.to_le_bytes());
    }
    p
}

/// Controller touchpad (Sunshine extension): 28 bytes. Coordinates and pressure in 0..1;
/// event types: 1 down, 2 up, 3 move.
pub fn controller_touch(
    controller: u8,
    event_type: u8,
    pointer_id: u32,
    x: f32,
    y: f32,
    pressure: f32,
) -> Vec<u8> {
    let mut p = header(24, MAGIC_CONTROLLER_TOUCH);
    p.push(controller);
    p.push(event_type);
    p.push(0);
    p.push(0); // touchpadIndex
    p.extend_from_slice(&pointer_id.to_le_bytes());
    for v in [x, y, pressure] {
        p.extend_from_slice(&v.to_le_bytes());
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn multi_controller_matches_documented_bytes() {
        let s = GamepadState {
            buttons: button::A | button::TOUCHPAD,
            left_trigger: 0x11,
            right_trigger: 0x22,
            left_stick_x: -1,
            ..Default::default()
        };
        let p = multi_controller(0, 0x1, &s);
        assert_eq!(p.len(), 34);
        // Upstream example in protocol-notes §12.7: [00 00 00 1E][0C 00 00 00][1A 00]...
        assert_eq!(&p[..10], &[0, 0, 0, 0x1E, 0x0C, 0, 0, 0, 0x1A, 0]);
        assert_eq!(&p[10..12], &[0, 0], "controller number");
        assert_eq!(&p[12..14], &[1, 0], "active mask");
        assert_eq!(&p[16..18], &(button::A as u16).to_le_bytes());
        assert_eq!((p[18], p[19]), (0x11, 0x22));
        assert_eq!(&p[20..22], &[0xFF, 0xFF]);
        assert_eq!(
            &p[30..32],
            &0x0010u16.to_le_bytes(),
            "touchpad in buttonFlags2"
        );
        assert_eq!(&p[32..34], &[0x55, 0]);
    }

    #[test]
    fn sizes_match_header_rule() {
        // Sunshine requires BE32(size) == packet length - 4 for fixed-size packets.
        for p in [
            controller_arrival(0, CONTROLLER_TYPE_PS, 0x3b, 0x3f_ffff),
            controller_motion(0, 2, [0.0, 1.0, 2.0]),
            controller_touch(0, 1, 0, 0.5, 0.5, 1.0),
        ] {
            assert_eq!(
                u32::from_be_bytes(p[..4].try_into().unwrap()) as usize,
                p.len() - 4
            );
        }
        assert_eq!(controller_arrival(0, 2, 0, 0).len(), 16);
        assert_eq!(controller_motion(0, 1, [0.0; 3]).len(), 24);
        assert_eq!(controller_touch(0, 1, 0, 0.0, 0.0, 0.0).len(), 28);
    }

    #[test]
    fn playstation_button_names() {
        assert_eq!(button_by_name("Cross").unwrap(), button::A);
        assert_eq!(button_by_name("ps").unwrap(), button::HOME);
        assert_eq!(button_by_name("create").unwrap(), button::BACK);
        assert!(button_by_name("turbo").is_err());
    }
}

/// Source-pinned channel/reliability semantics from protocol-notes §12.2.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InputPacket {
    pub bytes: Vec<u8>,
    pub channel: u8,
    pub reliable: bool,
}
pub fn gamepad_packet(
    controller: u8,
    active_mask: u16,
    state: &GamepadState,
) -> Result<InputPacket> {
    ensure!(
        controller < 4 && active_mask & !0xf == 0,
        "scenario supports four controller identities"
    );
    Ok(InputPacket {
        bytes: multi_controller(controller, active_mask, state),
        channel: CHANNEL_GAMEPAD_BASE + controller,
        reliable: true,
    })
}

#[derive(Debug, Clone, Serialize)]
pub struct InputOutcome {
    pub event_id: String,
    pub controller: u8,
    pub observed_us: u64,
    /// Enqueued to the control transport; no ACK or console delivery is inferred.
    pub packets_queued: u32,
    pub transport_errors: Vec<String>,
    pub unavailable: Option<String>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum FeedbackResult {
    Pending,
    Observed,
    TimedOut,
}
struct RumbleWait {
    start: u64,
    deadline: u64,
    minimum: f64,
    observed: bool,
}
#[derive(Default)]
struct Pad {
    connected: bool,
    state: GamepadState,
    contacts: std::collections::BTreeSet<u8>,
    motion_enabled: [bool; 2],
    rumble: Option<RumbleWait>,
}
/// No media work or clock waits occur here. The scenario runner owns scheduling and streams
/// returned outcomes to disk; only four controller states and one feedback wait each are kept.
pub struct ControllerSession {
    pads: [Pad; 4],
    host_controller_touch: bool,
    last_time: Option<u64>,
}
impl ControllerSession {
    pub fn new(host_feature_flags: u32) -> Self {
        Self {
            pads: std::array::from_fn(|_| Pad::default()),
            host_controller_touch: host_feature_flags & 2 != 0,
            last_time: None,
        }
    }
    pub fn active_mask(&self) -> u16 {
        self.pads
            .iter()
            .enumerate()
            .fold(0, |mask, (i, p)| mask | (u16::from(p.connected) << i))
    }
    pub fn apply(
        &mut self,
        controller: u8,
        action: &crate::scenario::Action,
        now_us: u64,
        mut send: impl FnMut(&InputPacket) -> Result<()>,
    ) -> Result<InputOutcome> {
        use crate::scenario::{ActionKind as A, Sensor, Side, TouchPhase};
        ensure!(
            controller < 4 && !action.event_id.is_empty() && action.event_id.len() <= 128,
            "invalid input identity"
        );
        ensure!(
            self.last_time.is_none_or(|t| now_us >= t),
            "input monotonic time reversed"
        );
        self.last_time = Some(now_us);
        let mut outcome = InputOutcome {
            event_id: action.event_id.clone(),
            controller,
            observed_us: now_us,
            packets_queued: 0,
            transport_errors: Vec::new(),
            unavailable: None,
        };
        let mut packets = Vec::new();
        let old_mask = self.active_mask();
        let pad = &mut self.pads[controller as usize];
        if !matches!(action.kind, A::Connect {}) {
            ensure!(pad.connected, "action on disconnected controller");
        }
        match &action.kind {
            A::Connect {} => {
                ensure!(!pad.connected, "controller already connected");
                *pad = Pad {
                    connected: true,
                    ..Default::default()
                };
                packets.push(InputPacket {
                    bytes: controller_arrival(
                        controller,
                        CONTROLLER_TYPE_PS,
                        capability::ANALOG_TRIGGERS
                            | capability::RUMBLE
                            | capability::TOUCHPAD
                            | capability::ACCEL
                            | capability::GYRO,
                        0x10_f7ff,
                    ),
                    channel: CHANNEL_GAMEPAD_BASE + controller,
                    reliable: true,
                });
                packets.push(gamepad_packet(
                    controller,
                    old_mask | (1 << controller),
                    &pad.state,
                )?);
            }
            A::Disconnect {} => {
                if !pad.contacts.is_empty() {
                    packets.push(InputPacket {
                        bytes: controller_touch(controller, 7, 0, 0.0, 0.0, 0.0),
                        channel: CHANNEL_GAMEPAD_BASE + controller,
                        reliable: true,
                    });
                }
                // Neutral state precedes removal; stale state never survives reconnect.
                packets.push(gamepad_packet(
                    controller,
                    old_mask,
                    &GamepadState::default(),
                )?);
                packets.push(gamepad_packet(
                    controller,
                    old_mask & !(1 << controller),
                    &GamepadState::default(),
                )?);
                *pad = Pad::default();
            }
            A::Button { button, pressed } => {
                let bit = scenario_button(*button);
                if *pressed {
                    pad.state.buttons |= bit;
                } else {
                    pad.state.buttons &= !bit;
                }
                packets.push(gamepad_packet(controller, old_mask, &pad.state)?);
            }
            A::Stick { stick, x, y } => {
                let axes = (stick_axis(*x)?, stick_axis(*y)?);
                match stick {
                    Side::Left => {
                        pad.state.left_stick_x = axes.0;
                        pad.state.left_stick_y = axes.1;
                    }
                    Side::Right => {
                        pad.state.right_stick_x = axes.0;
                        pad.state.right_stick_y = axes.1;
                    }
                }
                packets.push(gamepad_packet(controller, old_mask, &pad.state)?);
            }
            A::Trigger { trigger, value } => {
                ensure!(
                    value.is_finite() && (0.0..=1.0).contains(value),
                    "invalid trigger"
                );
                let value = (value * 255.0).round() as u8;
                match trigger {
                    Side::Left => pad.state.left_trigger = value,
                    Side::Right => pad.state.right_trigger = value,
                }
                packets.push(gamepad_packet(controller, old_mask, &pad.state)?);
            }
            A::Motion { sensor, x, y, z } => {
                let index = match sensor {
                    Sensor::Accel => 0,
                    Sensor::Gyro => 1,
                };
                ensure!(
                    [x, y, z]
                        .iter()
                        .all(|v| v.is_finite() && v.abs() <= f64::from(f32::MAX)),
                    "invalid motion sample"
                );
                if !self.host_controller_touch || !pad.motion_enabled[index] {
                    outcome.unavailable = Some("host has not enabled controller motion".into());
                } else {
                    packets.push(InputPacket {
                        bytes: controller_motion(
                            controller,
                            index as u8 + 1,
                            [*x as f32, *y as f32, *z as f32],
                        ),
                        channel: CHANNEL_SENSOR_BASE + controller,
                        reliable: index == 1 && [*x, *y, *z] == [0.0; 3],
                    });
                }
            }
            A::Touchpad {
                contact_id,
                phase,
                x,
                y,
            } => {
                ensure!(
                    [x, y]
                        .iter()
                        .all(|v| v.is_finite() && (0.0..=1.0).contains(*v)),
                    "invalid touch coordinates"
                );
                if !self.host_controller_touch {
                    outcome.unavailable = Some("host lacks controller touch feature".into());
                } else {
                    match phase {
                        TouchPhase::Down => ensure!(
                            pad.contacts.insert(*contact_id),
                            "touch contact already down"
                        ),
                        TouchPhase::Move => {
                            ensure!(pad.contacts.contains(contact_id), "touch contact missing")
                        }
                        TouchPhase::Up | TouchPhase::Cancel => {
                            ensure!(pad.contacts.remove(contact_id), "touch contact missing")
                        }
                    }
                    let kind = match phase {
                        TouchPhase::Down => 1,
                        TouchPhase::Up => 2,
                        TouchPhase::Move => 3,
                        TouchPhase::Cancel => 4,
                    };
                    packets.push(InputPacket {
                        bytes: controller_touch(
                            controller,
                            kind,
                            u32::from(*contact_id),
                            *x as f32,
                            *y as f32,
                            0.0,
                        ),
                        channel: CHANNEL_GAMEPAD_BASE + controller,
                        reliable: kind != 3,
                    });
                }
            }
            A::ExpectRumble {
                minimum_amplitude,
                timeout_seconds,
            } => {
                ensure!(
                    minimum_amplitude.is_finite()
                        && (0.0..=1.0).contains(minimum_amplitude)
                        && timeout_seconds.is_finite()
                        && *timeout_seconds > 0.0,
                    "invalid rumble expectation"
                );
                ensure!(
                    pad.rumble.is_none(),
                    "previous rumble expectation must be consumed"
                );
                let duration = std::time::Duration::try_from_secs_f64(*timeout_seconds)?;
                let deadline = now_us
                    .checked_add(u64::try_from(duration.as_micros())?)
                    .ok_or_else(|| anyhow::anyhow!("feedback deadline overflow"))?;
                pad.rumble = Some(RumbleWait {
                    start: now_us,
                    deadline,
                    minimum: *minimum_amplitude,
                    observed: false,
                });
            }
        }
        for packet in packets {
            match send(&packet) {
                Ok(()) => outcome.packets_queued += 1,
                Err(error) => outcome.transport_errors.push(error.to_string()),
            }
        }
        Ok(outcome)
    }
    /// Only authenticated control events belong here. A received rumble proves host feedback,
    /// not physical vibration; console input-delivery records are imported separately by T027.
    pub fn observe_feedback(
        &mut self,
        event: &crate::control::ControlEvent,
        now_us: u64,
    ) -> Result<()> {
        ensure!(
            self.last_time.is_none_or(|t| now_us >= t),
            "feedback monotonic time reversed"
        );
        self.last_time = Some(now_us);
        use crate::control::ControlEvent;
        match event {
            ControlEvent::Rumble {
                controller,
                low_frequency,
                high_frequency,
            } => {
                let pad = self
                    .pads
                    .get_mut(usize::from(*controller))
                    .ok_or_else(|| anyhow::anyhow!("feedback controller out of range"))?;
                if pad.connected {
                    if let Some(wait) = &mut pad.rumble {
                        if now_us >= wait.start
                            && now_us <= wait.deadline
                            && f64::from((*low_frequency).max(*high_frequency)) / 65535.0
                                >= wait.minimum
                        {
                            wait.observed = true;
                        }
                    }
                }
            }
            ControlEvent::Extension {
                kind: 0x5501,
                payload,
            } => {
                ensure!(payload.len() == 5, "invalid motion feedback size");
                let id = u16::from_le_bytes([payload[0], payload[1]]);
                let pad = self
                    .pads
                    .get_mut(usize::from(id))
                    .ok_or_else(|| anyhow::anyhow!("motion controller out of range"))?;
                ensure!([1, 2].contains(&payload[4]), "invalid motion feedback type");
                if pad.connected {
                    pad.motion_enabled[usize::from(payload[4] - 1)] =
                        u16::from_le_bytes([payload[2], payload[3]]) != 0;
                }
            }
            _ => {}
        }
        Ok(())
    }
    pub fn rumble_result(&mut self, controller: u8, now_us: u64) -> Result<Option<FeedbackResult>> {
        ensure!(
            self.last_time.is_none_or(|t| now_us >= t),
            "feedback monotonic time reversed"
        );
        self.last_time = Some(now_us);
        let pad = self
            .pads
            .get_mut(usize::from(controller))
            .ok_or_else(|| anyhow::anyhow!("feedback controller out of range"))?;
        let result = pad.rumble.as_ref().map(|wait| {
            if wait.observed {
                FeedbackResult::Observed
            } else if now_us > wait.deadline {
                FeedbackResult::TimedOut
            } else {
                FeedbackResult::Pending
            }
        });
        if result.is_some_and(|v| v != FeedbackResult::Pending) {
            pad.rumble = None;
        }
        Ok(result)
    }
    /// Best effort for all four identities even when one transport send fails. Caller still
    /// records disconnect/loss and imports actual host release evidence independently.
    pub fn release_all(
        &mut self,
        now_us: u64,
        mut send: impl FnMut(&InputPacket) -> Result<()>,
    ) -> Result<Vec<InputOutcome>> {
        let mut outcomes = Vec::new();
        for controller in 0..4 {
            if self.pads[controller as usize].connected {
                let action = crate::scenario::Action {
                    event_id: format!("cleanup-{controller}"),
                    at_seconds: 0.0,
                    expected_delivery: None,
                    expected_response: None,
                    kind: crate::scenario::ActionKind::Disconnect {},
                };
                outcomes.push(self.apply(controller, &action, now_us, &mut send)?);
            }
        }
        Ok(outcomes)
    }
}
fn stick_axis(value: f64) -> Result<i16> {
    ensure!(
        value.is_finite() && (-1.0..=1.0).contains(&value),
        "invalid stick axis"
    );
    Ok((value * if value < 0.0 { 32768.0 } else { 32767.0 }).round() as i16)
}
fn scenario_button(value: crate::scenario::Button) -> u32 {
    use crate::scenario::Button as B;
    match value {
        B::Cross => button::A,
        B::Circle => button::B,
        B::Square => button::X,
        B::Triangle => button::Y,
        B::DpadUp => button::UP,
        B::DpadDown => button::DOWN,
        B::DpadLeft => button::LEFT,
        B::DpadRight => button::RIGHT,
        B::L1 => button::LB,
        B::R1 => button::RB,
        B::L3 => button::LS_CLICK,
        B::R3 => button::RS_CLICK,
        B::Options => button::START,
        B::Share => button::BACK,
        B::Home => button::HOME,
        B::Touchpad => button::TOUCHPAD,
    }
}

#[cfg(test)]
mod scenario_tests {
    use super::*;
    use crate::scenario::{Action, ActionKind, Button, Sensor, Side, TouchPhase};
    fn action(kind: ActionKind) -> Action {
        Action {
            event_id: "fixture".into(),
            at_seconds: 0.0,
            expected_delivery: Some(true),
            expected_response: None,
            kind,
        }
    }
    #[test]
    fn four_identities_axes_buttons_and_cleanup_keep_transport_separate() {
        let mut session = ControllerSession::new(2);
        let mut sent = Vec::new();
        for id in 0..4 {
            let result = session
                .apply(id, &action(ActionKind::Connect {}), 0, |p| {
                    sent.push(p.clone());
                    Ok(())
                })
                .unwrap();
            assert_eq!(result.packets_queued, 2);
        }
        assert_eq!(session.active_mask(), 15);
        assert!(session
            .apply(4, &action(ActionKind::Connect {}), 0, |_| Ok(()))
            .is_err());
        assert!(session
            .apply(0, &action(ActionKind::Connect {}), 0, |_| Ok(()))
            .is_err());
        let mut send = |p: &InputPacket| {
            sent.push(p.clone());
            Ok(())
        };
        for button in [
            Button::Cross,
            Button::Circle,
            Button::Square,
            Button::Triangle,
            Button::DpadUp,
            Button::DpadDown,
            Button::DpadLeft,
            Button::DpadRight,
            Button::L1,
            Button::R1,
            Button::L3,
            Button::R3,
            Button::Options,
            Button::Share,
            Button::Home,
            Button::Touchpad,
        ] {
            session
                .apply(
                    2,
                    &action(ActionKind::Button {
                        button,
                        pressed: true,
                    }),
                    1,
                    &mut send,
                )
                .unwrap();
            assert_ne!(session.pads[2].state.buttons & scenario_button(button), 0);
            session
                .apply(
                    2,
                    &action(ActionKind::Button {
                        button,
                        pressed: false,
                    }),
                    1,
                    &mut send,
                )
                .unwrap();
        }
        for side in [Side::Left, Side::Right] {
            session
                .apply(
                    2,
                    &action(ActionKind::Stick {
                        stick: side,
                        x: -1.0,
                        y: 1.0,
                    }),
                    2,
                    &mut send,
                )
                .unwrap();
            session
                .apply(
                    2,
                    &action(ActionKind::Trigger {
                        trigger: side,
                        value: 0.5,
                    }),
                    2,
                    &mut send,
                )
                .unwrap();
        }
        assert_eq!(session.pads[2].state.left_stick_x, i16::MIN);
        assert_eq!(session.pads[2].state.right_stick_y, i16::MAX);
        assert_eq!(session.pads[2].state.left_trigger, 128);
        let failure = session
            .apply(
                2,
                &action(ActionKind::Button {
                    button: Button::Home,
                    pressed: true,
                }),
                3,
                |_| bail!("fixture transport down"),
            )
            .unwrap();
        assert_eq!(failure.packets_queued, 0);
        assert_eq!(failure.transport_errors.len(), 1);
        assert!(failure.unavailable.is_none());
        let cleanup = session
            .release_all(4, |_| bail!("fixture transport down"))
            .unwrap();
        assert_eq!(cleanup.len(), 4);
        assert!(cleanup.iter().all(|o| o.transport_errors.len() == 2));
        assert_eq!(session.active_mask(), 0);
        session
            .apply(2, &action(ActionKind::Connect {}), 5, |_| Ok(()))
            .unwrap();
        assert_eq!(session.pads[2].state, GamepadState::default());
        assert!(session
            .apply(
                2,
                &action(ActionKind::Stick {
                    stick: Side::Left,
                    x: f64::NAN,
                    y: 0.0
                }),
                5,
                |_| Ok(())
            )
            .is_err());
    }
    #[test]
    fn motion_touch_reliability_and_feedback_are_bounded_and_controller_specific() {
        let mut session = ControllerSession::new(2);
        for id in 0..2 {
            session
                .apply(id, &action(ActionKind::Connect {}), 0, |_| Ok(()))
                .unwrap();
        }
        let motion = action(ActionKind::Motion {
            sensor: Sensor::Gyro,
            x: 0.0,
            y: 0.0,
            z: 0.0,
        });
        assert!(session
            .apply(0, &motion, 1, |_| Ok(()))
            .unwrap()
            .unavailable
            .is_some());
        session
            .observe_feedback(
                &crate::control::ControlEvent::Extension {
                    kind: 0x5501,
                    payload: vec![0, 0, 60, 0, 2],
                },
                2,
            )
            .unwrap();
        let mut sent = Vec::new();
        session
            .apply(0, &motion, 3, |p| {
                sent.push(p.clone());
                Ok(())
            })
            .unwrap();
        assert_eq!((sent[0].channel, sent[0].reliable), (0x20, true));
        for (phase, reliable) in [
            (TouchPhase::Down, true),
            (TouchPhase::Move, false),
            (TouchPhase::Up, true),
        ] {
            session
                .apply(
                    0,
                    &action(ActionKind::Touchpad {
                        contact_id: 4,
                        phase,
                        x: 0.25,
                        y: 0.75,
                    }),
                    4,
                    |p| {
                        assert_eq!(p.reliable, reliable);
                        assert_eq!(p.bytes[12], 4);
                        Ok(())
                    },
                )
                .unwrap();
        }
        session
            .apply(
                0,
                &action(ActionKind::ExpectRumble {
                    minimum_amplitude: 0.5,
                    timeout_seconds: 1.0,
                }),
                5,
                |_| Ok(()),
            )
            .unwrap();
        session
            .observe_feedback(
                &crate::control::ControlEvent::Rumble {
                    controller: 1,
                    low_frequency: 65535,
                    high_frequency: 0,
                },
                6,
            )
            .unwrap();
        assert_eq!(
            session.rumble_result(0, 6).unwrap(),
            Some(FeedbackResult::Pending)
        );
        session
            .observe_feedback(
                &crate::control::ControlEvent::Rumble {
                    controller: 0,
                    low_frequency: 65535,
                    high_frequency: 0,
                },
                7,
            )
            .unwrap();
        assert_eq!(
            session.rumble_result(0, 7).unwrap(),
            Some(FeedbackResult::Observed)
        );
        session
            .apply(
                0,
                &action(ActionKind::ExpectRumble {
                    minimum_amplitude: 0.5,
                    timeout_seconds: 1.0,
                }),
                8,
                |_| Ok(()),
            )
            .unwrap();
        assert_eq!(
            session.rumble_result(0, 1_000_009).unwrap(),
            Some(FeedbackResult::TimedOut)
        );
        assert!(session
            .observe_feedback(
                &crate::control::ControlEvent::Extension {
                    kind: 0x5501,
                    payload: vec![]
                },
                1_000_010
            )
            .is_err());
    }
}
