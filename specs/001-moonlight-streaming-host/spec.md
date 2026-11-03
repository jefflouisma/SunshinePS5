# Feature Specification: Self-Contained Moonlight Streaming Host

**Feature Branch**: `001-moonlight-streaming-host`

**Created**: 2026-10-24

**Status**: Draft

**Input**: User description: "self contained (meaning their should be a mobile friendly web app allowing the pairing of devices) otherwise how does an apple TV moonlight client pair with the PS5 backend. there should be an ability to turn on and off performance metrics so we can validate where bottlenecks are in the stack, and the overall thing should be very easy to use."

**Product context**: SunshinePS5 is a PS5 homebrew application that lets users on the local network
control their PS5 and play its games from a standard Moonlight client.

## Clarifications

### Session 2026-10-24

- Q: Which console firmware versions must the first release support? → A: Firmware 13.60 only,
  with the etaHEN homebrew environment verified on the development console; other firmware is
  added later by spec amendment after hardware verification.
- Q: How should the HDR requirements be verified, given that the current capture device can neither
  show nor record HDR? → A: Add HDR test hardware before HDR acceptance: an HDR-capable capture
  device with HDR passthrough to an HDR TV, giving both a live side-by-side view and recorded,
  repeatable comparisons.
- Q: Which device is the HDR test hardware? → A: An Elgato 4K X (4K144 HDR10, model 10GBH9901)
  between the console and an HDR TV. The test client must capture its 10-bit HDR output, which
  macOS hides from standard capture, using the approach of the elgato-hdr project
  (https://github.com/talkaboutdesign/elgato-hdr).
- Q: What pass rule should the stream's picture fidelity meet when compared with the HDMI capture
  of the console's output? → A: A floor plus a regression gate: 99% of compared frames score SSIM
  ≥ 0.70, and the median may not fall more than 0.01 below the previous release on the same
  reference scenes. An absolute median target is added once planning has measured the capture
  chain's own error.
- Q: Is the protocol test client delivered as part of this feature, or as its own feature? → A:
  Part of this feature (User Story 5, FR-054 to FR-060).
- Q: How should the console be networked so official Moonlight clients can reach it for acceptance
  testing? → A: On the home network with a static address and no usable gateway, as on its current
  direct link, with safeguards against an IPv6 route to the internet.
- Q: Which devices on your private home network should be allowed to access SunshinePS5 and its web app? → A: Devices on any reachable private home-network segment, including router-connected private subnets; public source addresses remain rejected.
- Q: How should a verification run be classified when a required measurement is missing or unreliable? → A: Incomplete; missing or unreliable required evidence blocks acceptance until valid measurements are available.
- Q: What should happen when Moonlight requests a bitrate outside the host's supported range? → A: Use the nearest supported bitrate and show the requested and chosen values in the web app and test report.
- Q: Which taps should count toward the three-tap limit for phone-based pairing and management? → A: Count navigation, submission, and confirmation taps; exclude typing and keyboard dismissal.

## Release scope decision (owner, 2026-11-01)

The first release is scoped to **SDR 1920x1080 H.264 at up to 60 fps with one controller**.
HDR (User Story 2) and capture-protected content are **deferred and unsupported** in this scope.
The host advertises SDR only. Clients requesting HDR receive SDR. Capture protection is an explicit
operator opt-in (`CLEAR` diagnostic premise); protected content is not claimed to work. The
requirements below remain the long-term product requirements; the deferred markers identify what
this release does not advertise or accept.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Pair a Moonlight device and play (Priority: P1)

A console owner launches SunshinePS5 on their PS5. Their Apple TV, running the standard Moonlight
app, lists the PS5 as an available host. They select it and Moonlight displays a PIN. Moonlight's
pairing requires that PIN to be entered on the host, and the PS5 has no host-side screen for it, so
the console shows a notification telling the owner to open the SunshinePS5 web app on their phone.
They enter the PIN there and pairing completes. They select the PS5 in Moonlight, see the console's
screen with its sound, and use their controller to navigate the PS5 home screen, start a game, and
play.

**Why this priority**: This is the product. Without the web app, clients such as Apple TV cannot
complete pairing at all, so pairing through the web app is part of the minimum viable slice, not an
add-on.

**Independent Test**: On a supported console with no devices paired, pair an Apple TV running the
current Moonlight release using only the on-screen prompts and a phone, then play a game for
30 minutes with video, audio, and controller input working.

**Acceptance Scenarios**:

1. **Given** SunshinePS5 is running and a Moonlight client is on the same local network, **When**
   the user opens Moonlight, **Then** the PS5 appears in the host list without manual entry.
2. **Given** a client that has never paired with the host, **When** the user selects the PS5 in
   Moonlight, **Then** Moonlight shows a PIN and the console displays a notification saying a
   device wants to pair and where to enter the PIN.
3. **Given** a pending pairing request, **When** the user enters the correct PIN in the web app,
   **Then** Moonlight reports pairing success within 3 seconds and the device appears in the web
   app's paired-device list.
4. **Given** a paired client, **When** the user starts a session, **Then** the client shows the
   console's current screen with its audio, and the user's controller operates the console,
   including the PS button and touchpad button.
5. **Given** an active session, **When** the user quits or disconnects in Moonlight, **Then** the
   session ends, the game on the console keeps running, and no buttons or sticks remain held on the
   console.
6. **Given** a previously paired client, **When** SunshinePS5 or the console restarts, **Then** the
   client can stream again without pairing again.
7. **Given** an otherwise supported session request with a bitrate outside the supported range,
   **When** the session starts, **Then** the chosen bitrate is the nearest supported boundary,
   and the web app and test report show both requested and chosen values and the adjustment.

---

### User Story 2 - Play in HDR (Priority: P2; deferred from the 2026-11-01 SDR release)

The owner's PS5 outputs HDR to their TV. In Moonlight on their Apple TV 4K they turn on HDR, start a
session, and launch an HDR game. The stream arrives in HDR with the same highlights and color range
they see on the console's own display. When the console switches between HDR and SDR output, for
example when a game without HDR starts, the stream follows without reconnecting.

**Why this priority**: Many PS5 games are designed to be seen in HDR, and the owner requires it in
the first release. It builds on the streaming from User Story 1 and is otherwise independent.

**Independent Test**: With the console outputting HDR through the HDR test hardware (an Elgato 4K X
passing the signal through to an HDR TV while its 10-bit capture output feeds the test machine) and
an HDR-capable client with Moonlight's HDR option on, stream an HDR game for 30 minutes. Confirm the
client reports an HDR stream and the picture matches both the TV side by side and the recorded
capture of the console's output. Then turn the client's HDR option off and confirm the SDR picture
looks correct.

**Acceptance Scenarios**:

1. **Given** the console outputs HDR and the client has HDR enabled, **When** a session starts,
   **Then** the stream is HDR and the client reports it as HDR.
2. **Given** an HDR session, **When** the stream is compared side by side with the console's
   display, **Then** highlights, color, and shadow detail match, with no banding, clipping, or
   washed-out color.
3. **Given** an HDR session, **When** the console switches between HDR and SDR output, **Then** the
   stream switches to match within 2 seconds without ending the session.
4. **Given** the console outputs HDR and the client has not requested HDR, **When** a session
   starts, **Then** the client receives a correctly converted SDR picture, neither washed out nor
   dim.
5. **Given** the console outputs SDR and the client has HDR enabled, **When** a session starts,
   **Then** the stream is SDR and plays normally.

---

### User Story 3 - Manage the host from a phone (Priority: P3)

From the web app on their phone, the owner sees whether SunshinePS5 is running and whether anyone
is streaming, from which device, and at what resolution and frame rate. They rename paired devices,
revoke a device they no longer trust (for example, a friend's phone), change the name the PS5 shows
in Moonlight, and stop SunshinePS5 cleanly.

**Why this priority**: Owners need to see and control who can reach their console. Revocation is the
safety net for pairing. Streaming works without it, so it ranks below User Stories 1 and 2.

**Independent Test**: With two paired clients, revoke one from the web app; verify the revoked
client can no longer start a session and the other still can.

**Acceptance Scenarios**:

1. **Given** a session is active, **When** the owner opens the web app, **Then** it shows the
   streaming device's name, resolution, frame rate, requested and chosen bitrate, any bitrate
   adjustment, and session duration.
2. **Given** a paired device is streaming, **When** the owner revokes it, **Then** its session ends
   within 2 seconds and the device must pair again before it can stream.
3. **Given** the owner changes the host name, **When** a client next refreshes its host list,
   **Then** the new name appears.
4. **Given** SunshinePS5 is running, **When** the owner selects Stop in the web app and confirms,
   **Then** any active session ends, SunshinePS5 exits, and the console keeps operating normally.

---

### User Story 4 - Find bottlenecks with performance metrics (Priority: P4)

A developer or advanced user investigating stutter or latency turns on performance metrics in the
web app. While they stream, the web app shows live timings for each stage of the host pipeline
(capture, encode, packetize, send, and controller input handling) together with frame rate, dropped
frames, bitrate, and packet loss reported by the client. It highlights the stage taking the largest
share of the per-frame time budget. They export a capture to attach to a bug report, then turn
metrics off.

**Why this priority**: The constitution requires measurements for every hot-path change, and this is
how they are gathered. It also lets users diagnose their own setup. Streaming works without it, so
it ranks last.

**Independent Test**: Enable metrics, stream for 5 minutes, and confirm every stage reports values
and the export contains them; disable metrics and confirm collection stops and stream performance
is unchanged.

**Acceptance Scenarios**:

1. **Given** metrics are off and a session is active, **When** the owner turns metrics on, **Then**
   live values appear within 2 seconds and the stream is not interrupted.
2. **Given** metrics are on, **When** the owner views the metrics page, **Then** each pipeline
   stage shows its median, 95th, and 99th percentile time, and the stage with the largest share of
   the frame budget is highlighted.
3. **Given** metrics are on, **When** the 99th percentile total host time exceeds the frame
   interval of the current session, **Then** the web app flags the session as over budget.
4. **Given** metrics are on, **When** the owner exports a capture, **Then** they receive a file
   containing the timestamped samples and the context needed to interpret them.
5. **Given** metrics are on, **When** the owner turns them off, **Then** collection stops within
   2 seconds and the stream is not interrupted.

---

### User Story 5 - Verify a build automatically (Priority: P1)

A developer, a CI job, or an AI agent runs the project's protocol test client against a host. It
pairs, starts a session with the requested resolution, frame rate, bitrate, codec, and dynamic
range, sends controller input, and ends the session, while recording the console's HDMI output on
the test machine, in 10-bit HDR when the console outputs HDR. It produces a report with frame
timing, host processing latency, frame loss, audio health, HDR state, and picture fidelity against
the HDMI capture, and reports Pass, Fail, or Incomplete against the success criteria. Missing or
unreliable required evidence makes the run Incomplete and blocks acceptance.

**Why this priority**: SC-003 to SC-006, SC-008, SC-012, and SC-014 can only be measured with it,
and constitution Principle V requires end-to-end CI through it, so it is built alongside User
Story 1.

**Independent Test**: Run a scripted scenario against an existing reference host (for example,
Wolf or Sunshine) and confirm pairing, streaming, input, and the report all work. Then run it
against SunshinePS5.

**Acceptance Scenarios**:

1. **Given** a reachable host and a PIN delivery method, **When** the test client pairs, **Then**
   it completes pairing with all of an official client's checks, and a host that fails any check
   is reported as failed.
2. **Given** a paired host, **When** a scenario requests a session, **Then** the report contains
   frame rate, frame interval distribution, per-frame host processing latency, missing and
   incomplete frames, requested and chosen bitrate with any adjustment, measured bitrate, audio
   packet loss and whether audio was non-silent, and HDR state.
3. **Given** a session and the HDMI capture running together, **When** the run ends, **Then** the
   report contains per-frame fidelity scores and the stream's delay relative to the capture, with
   sample image pairs for review.
4. **Given** the console outputs HDR through the HDR test hardware, **When** the test client
   records the capture, **Then** the recording holds the full 10-bit HDR signal, not the 8-bit
   modes the operating system's standard capture interface offers, and HDR streams are compared
   with it in the same PQ/BT.2020 color space.
5. **Given** a scenario that sends controller input, **When** the run ends, **Then** the report
   shows each input was delivered over the session's control channel.
6. **Given** a required measurement is missing or unreliable, **When** the report is produced,
   **Then** the affected criterion and the run are marked Incomplete, the evidence problem is
   explained, and the run cannot be accepted.

---

### Edge Cases

- **Unsupported firmware**: SunshinePS5 refuses to start and the console shows the detected
  firmware version and where to find the supported list.
- **Discovery blocked by the network** (for example, Wi-Fi client isolation): the user adds the
  host in Moonlight by address; the address is shown in the startup notification and in the web
  app.
- **Wrong PIN**: pairing fails in Moonlight, the web app shows the failure, and the user can retry.
  After 5 consecutive failures the host refuses new pairing requests for 60 seconds.
- **Pairing request never completed**: it expires after 5 minutes, disappears from the web app, and
  Moonlight reports failure.
- **Second pairing request while one is pending**: refused until the first completes or expires.
- **Second client tries to stream during an active session**: refused; the active session is
  unaffected.
- **Client loses network mid-session**: controller input is released as soon as the host detects
  loss of contact, the session ends no later than 10 seconds after the last contact, and the game
  keeps running. The client can start a new session when it reconnects.
- **Capture-protected content on screen** (for example, some video apps or system screens): the
  client sees a placeholder image while that content is displayed, the session stays connected, and
  the stream resumes when the content changes. The host does not bypass the protection.
- **Console enters rest mode or powers off during a session**: the session ends. After the console
  restarts, the user relaunches SunshinePS5.
- **Client requests a resolution or frame rate the host cannot sustain**: the mode is not offered;
  the client falls back to a supported mode.
- **Client requests a bitrate outside the supported range**: the host chooses the nearest range
  boundary and continues session establishment. The web app and test report identify the
  adjustment and show both requested and chosen bitrate.
- **Client requests HDR while the console outputs SDR**: the stream is SDR until the console
  switches to HDR output.
- **The console's own Remote Play or Share Play is in use**: new Moonlight sessions are refused
  with a plain-language reason. If either starts during a Moonlight session, that session ends
  cleanly with a recorded reason.
- **Connection from an address outside the permitted private-network ranges**: refused, for both
  streaming and the web app, even if that address is on the console's own network segment.
- **Connection from another reachable private subnet**: permitted by the source-address rule;
  pairing and single-session restrictions still apply. If discovery does not cross the router,
  the user adds the host by address.
- **A client disconnects its controller**: the matching console controller is released; the console
  remains usable and no additional client controller is required.
- **More controllers on the client than the host supports**: controllers beyond the host's
  implementation limit are ignored.
- **Saved pairing data cannot be read**: SunshinePS5 starts with no paired devices and the web app
  explains that devices must be paired again.
- **Required acceptance evidence is missing or unreliable** (for example, an unavailable reference
  recording or an unreliable frame comparison): the test client reports Incomplete, identifies
  the evidence problem, and blocks acceptance until a run supplies valid evidence.

## Requirements *(mandatory)*

### Functional Requirements

**Startup and discovery**

- **FR-001**: SunshinePS5 MUST start with a single action in the user's homebrew environment and
  MUST NOT require any configuration before first use.
- **FR-002**: On startup, the console MUST display a notification giving the web app's address.
- **FR-003**: The supported-firmware list for this feature is firmware 13.60 only. On any other
  firmware version SunshinePS5 MUST refuse to start and display the detected version.
- **FR-004**: Moonlight clients on the same local network MUST discover the host automatically. The
  host MUST also be reachable by entering the console's address manually in Moonlight.
- **FR-005**: The host MUST appear in Moonlight under the default name "SunshinePS5", which the
  owner can change. Host names MUST contain 1–63 Unicode scalars and at most 63 UTF-8
  octets; over-long names MUST be rejected with a clear “too long” message.

**Pairing**

- **FR-006**: The host MUST pair with unmodified Moonlight clients using Moonlight's standard PIN
  flow.
- **FR-007**: When a pairing request arrives, the console MUST display a notification saying a
  device wants to pair and where to enter the PIN. When a device finishes pairing, the console MUST
  display a notification naming it.
- **FR-008**: The web app MUST show the pending pairing request and accept the PIN and an optional
  device name.
- **FR-009**: Only one pairing request MAY be pending at a time. Further requests MUST be refused
  until it completes or expires. A pending request MUST expire after 5 minutes.
- **FR-010**: After 5 consecutive failed PIN entries, the host MUST refuse new pairing requests for
  60 seconds.
- **FR-011**: Paired devices MUST remain paired across restarts of SunshinePS5 and the console.
- **FR-012**: The host MUST refuse streaming, input, and launch requests from clients that are not
  paired.

**Web app**

- **FR-013**: SunshinePS5 MUST serve the web app from the console itself. Using it MUST NOT require
  installing anything, an extra device beyond a browser, or an internet connection.
- **FR-014**: The web app MUST be fully usable on phone screens 360 pixels wide or wider without
  horizontal scrolling or zooming, and on tablet and desktop browsers, in current versions of
  Safari on iOS, Chrome on Android, and Chrome, Edge, Firefox, and Safari on desktop.
- **FR-015**: The web app MUST NOT require a login. Any browser on the local network can use it;
  FR-040 keeps it unreachable from outside the local network.

**Streaming**

- **FR-016**: The host MUST stream the console's video output and game audio to a paired client.
- **FR-017**: The host MUST support 1280×720 and 1920×1080 at 30 and 60 frames per second. Higher
  resolutions and 120 frames per second MUST be offered only when the host can keep its 99th
  percentile per-frame time below one frame interval at that mode.
- **FR-018**: The host MUST honor the bitrate the client requests when it is within the host's
  supported range. If the request is below the supported minimum, the host MUST choose the
  minimum; if it exceeds the supported maximum, the host MUST choose the maximum. This adjustment
  MUST NOT by itself prevent the session from starting. The web app and test report MUST show
  both the requested and chosen bitrate and identify any adjustment. The chosen bitrate is the
  streaming target; measured bitrate remains a separate observation.
- **FR-019**: The host MUST deliver stereo audio.
- **FR-020**: The host MUST allow one active session at a time and refuse additional session
  requests without affecting the active session.
- **FR-021**: The host MUST report its per-frame processing latency to clients in the form
  supported clients display in their statistics overlay.
- **FR-022**: *(Deferred in the SDR release.)* Content the console protects from capture MUST NOT be streamed. The client MUST see a
  placeholder image while it is on screen, and the session MUST stay connected.
- **FR-023**: In Moonlight's app list, the host MUST present a single entry named "PS5" that streams
  whatever is on the console's screen. Quitting it in Moonlight MUST end the session without closing
  any game.

**HDR (User Story 2; deferred from the 2026-11-01 SDR release)**

- **FR-024**: The host MUST advertise HDR support to clients and MUST stream in HDR when the console
  outputs HDR and the client requests HDR.
- **FR-025**: HDR streams MUST carry the console's color and brightness information so the client
  displays the picture as the console intended.
- **FR-026**: HDR MUST be available at least at 1920×1080 and 60 frames per second. Other modes
  follow the per-frame time rule in FR-017, applied to HDR separately.
- **FR-027**: When the console switches between HDR and SDR output during a session, the stream MUST
  switch to match within 2 seconds without ending the session.
- **FR-028**: When the console outputs HDR and the client has not requested HDR, the host MUST
  convert the picture to SDR so that it is neither washed out nor dim.
- **FR-029**: When the console outputs SDR, the host MUST stream SDR regardless of the client's HDR
  setting.

**Controller input**

- **FR-030**: The host MUST map each client controller to a PS5 controller, covering face buttons,
  D-pad, shoulder buttons, analog triggers (proportionally), both sticks and their clicks, Options,
  Create, the PS button (or the client's equivalent guide-button action), and touchpad click.
- **FR-031**: The host MUST support one simultaneous controller, appearing to the console as a
  separate controller bound to the foreground user. The required deployment and acceptance use one
  controller; additional client controllers MAY be accepted up to the implementation limit but are
  not required.
- **FR-032**: The host MUST forward rumble from the console to client controllers that support it.
- **FR-033**: The host MUST forward motion and touchpad-surface input when the client provides it.
- **FR-034**: When a session ends, the client disconnects, or loss of contact is detected, the host
  MUST immediately release all controller input by submitting a neutral state. The virtual
  controller MUST remain registered and neutral so the running title does not raise a
  controller-disconnected notice; it is removed only when the host exits (or when no further
  session is expected).
- **FR-035**: The host MUST end a session no later than 10 seconds after last contact with the
  client.

**Console safety and errors**

- **FR-036**: The running game MUST continue unaffected when a session ends or SunshinePS5 stops.
- **FR-037**: Stopping SunshinePS5 MUST return the console to its pre-launch state without a reboot.
- **FR-038**: If the console's own Remote Play or Share Play is in use, the host MUST refuse new
  sessions with a plain-language reason. If either starts during a session, the host MUST end the
  session cleanly and record the reason.
- **FR-039**: Every error shown to the user, on the console or in the web app, MUST state in plain
  language what happened and what to do next. Every session end MUST be recorded with its reason.

**Network boundaries**

- **FR-040**: For streaming and web app access, the permitted local-network boundary includes
  any reachable private home-network segment, including private subnets connected through a
  router. Source-address checks MUST accept private IPv4 addresses (10.0.0.0/8, 172.16.0.0/12,
  and 192.168.0.0/16) and IPv6 unique-local addresses (fc00::/7), without requiring the source to
  share the console's subnet. IPv6 link-local addresses (fe80::/10) are permitted on the same
  network link. All other network source addresses, including public IPv4 and global IPv6
  addresses, MUST be refused. Pairing and single-session restrictions still apply.
- **FR-041**: SunshinePS5 MUST NOT send telemetry, contact Sony online services, or require an
  internet connection.

**Host management (User Story 3)**

- **FR-042**: The web app MUST show host status: running state, the active session if any (device
  name, resolution, frame rate, requested bitrate, chosen bitrate, any bitrate adjustment, and
  duration), SunshinePS5 version, firmware version, and the console's network address.
- **FR-043**: The web app MUST list paired devices with name, pairing date, and last session time,
  and MUST let the owner rename or revoke each one. Revocation MUST require confirmation and MUST
  end that device's active session within 2 seconds. Device names MUST contain 1–63 Unicode
  scalars and at most 63 UTF-8 octets; over-long names MUST be rejected with a clear
  “too long” message.
- **FR-044**: The web app MUST let the owner change the host name. The name MUST contain
  1–63 Unicode scalars and at most 63 UTF-8 octets; over-long names MUST be rejected with
  a clear “too long” message.
- **FR-045**: The web app MUST let the owner stop SunshinePS5, after confirmation.

**Performance metrics (User Story 4)**

- **FR-046**: Performance metrics MUST be off by default. The owner MUST be able to turn them on and
  off from the web app at any time; the change MUST take effect within 2 seconds without
  interrupting a session and MUST persist across restarts.
- **FR-047**: When metrics are on, the host MUST record, for every frame, the time spent in capture,
  encode, packetize, and send, and the total host time; for every controller input, the time from
  receipt to delivery to the console; and, continuously, frame rate, frames dropped by the host,
  bitrate, and packet loss and recovery requests reported by the client.
- **FR-048**: The metrics view MUST update at least once per second and show median, 95th, and
  99th percentile values for the last 10 seconds and for the whole session.
- **FR-049**: The metrics view MUST highlight the stage with the largest share of the per-frame time
  budget and flag the session when the 99th percentile total host time exceeds the frame interval.
- **FR-050**: The owner MUST be able to export a capture as a file containing the retained samples
  plus SunshinePS5 version, firmware version, resolution, frame rate, bitrate, and client device
  name.
- **FR-051**: The host MUST retain at most the most recent 10 minutes of metric samples, discarding
  older samples.
- **FR-052**: When metrics are off, the host MUST NOT collect per-frame or per-input timing data
  beyond what FR-021 requires.
- **FR-053**: The owner MUST be able to download a diagnostics file containing recent logs, session
  end reasons, the current metrics capture if any, and version information, for attaching to bug
  reports.

**Protocol test client (User Story 5)**

- **FR-054**: The project MUST provide a protocol test client that, without human help, pairs with
  a host, starts a session with a chosen resolution, frame rate, bitrate, codec, and dynamic range,
  sends controller input, and ends the session.
- **FR-055**: The test client MUST perform every pairing check an official Moonlight client
  performs, so that host pairing defects are detected rather than tolerated.
- **FR-056**: For every session the test client MUST write a machine-readable report containing
  frame rate, frame interval distribution, per-frame host processing latency, missing and
  incomplete frames (and which incomplete frames error correction could have recovered), requested
  and chosen bitrate with any adjustment, measured bitrate, audio packet loss, whether audio was
  non-silent, and every HDR state change the host signaled.
  Each success criterion required by the declared scenario MUST have a Pass, Fail, or Incomplete
  result. Missing or unreliable required evidence MUST produce Incomplete for that criterion and
  the overall run, with the reason recorded; any demonstrated criterion failures MUST remain
  recorded as Fail. Such a run MUST block acceptance until valid evidence is available. A run
  MAY be Pass only when all its required criteria have valid evidence and pass. A required
  criterion MUST NOT be omitted or treated as inapplicable because its evidence is unavailable.
  The machine-readable overall status MUST distinguish Incomplete from Pass and Fail, and
  automated callers MUST receive a non-success outcome for both Incomplete and Fail.
- **FR-057**: The test client MUST record the console's HDMI output at the same time as a stream,
  with nothing running on the console, and report per-frame fidelity (SSIM and PSNR) and the
  stream's delay relative to the capture (SC-014).
- **FR-058**: The test client MUST run from scenario files, be usable by AI agents through a tool
  interface, and run in CI against the host core with fake platform backends.
- **FR-059**: Before it is used to accept SunshinePS5, the test client MUST be validated against at
  least one existing reference host, with pairing, streaming, input, and reporting all working.
- **FR-060**: The test client MUST NOT send captures, frames, or reports to any external service;
  all analysis runs on the test machine.
- **FR-061**: The test client MUST record the HDR test hardware's 10-bit HDR capture format on the
  test machine, including on macOS, whose standard capture interface exposes only the device's
  8-bit modes, and MUST compare HDR streams with that recording in PQ/BT.2020 without first
  converting either to SDR.

### Key Entities *(include if feature involves data)*

- **Host**: The console running SunshinePS5. Has a display name, a persistent identity that clients
  recognize across restarts, a network address, running state, SunshinePS5 version, and firmware
  version.
- **Paired Device**: A Moonlight client allowed to stream. Has a persistent identity established at
  pairing, an owner-editable name, pairing date, and last session time. Revoking deletes it.
- **Pairing Request**: A client's attempt to pair. Has the time it began and a state: pending,
  completed, failed, or expired. At most one is pending.
- **Streaming Session**: A paired device's active stream. Has the device, start time, resolution,
  frame rate, dynamic range (SDR or HDR), requested and chosen bitrate, connected controllers, and,
  when ended, an end reason.
- **Metrics Capture**: Timestamped per-frame and per-input timing samples plus session context,
  limited to the most recent 10 minutes.
- **Settings**: Host name and metrics on/off. Persist across restarts.
- **Test Report**: The test client's record of one run: scenario, negotiated stream settings,
  requested and chosen bitrate with any adjustment, measurements (FR-056), fidelity results
  (FR-057), per-criterion Pass/Fail/Incomplete results,
  an overall Pass/Fail/Incomplete status, and reasons for missing or unreliable required evidence.
- **Reference Recording**: Frames captured from the console's HDMI output, each with its arrival
  time on the test machine's clock and its dynamic range (8-bit SDR or 10-bit HDR in PQ/BT.2020),
  used for fidelity and delay comparison.

## Success Criteria *(mandatory)*

### Measurable Outcomes

Acceptance requires valid evidence for every required criterion. An Incomplete result blocks
acceptance as defined in FR-056.

- **SC-001**: With SunshinePS5 already running, a first-time user pairs an Apple TV and is playing a
  game within 5 minutes using only the on-screen prompts. At least 9 of 10 test participants
  succeed on their first attempt without documentation.
- **SC-002**: A previously paired device shows the console's screen within 5 seconds of being
  selected in Moonlight.
- **SC-003**: At 1920×1080 and 60 frames per second on a wired local network, the host processing
  latency shown in Moonlight's statistics overlay stays below 16.7 ms for 99% of frames over a
  30-minute session.
- **SC-004**: Under the same conditions, the host drops fewer than 0.1% of frames over a 30-minute
  session.
- **SC-005**: For 99% of controller inputs, the time from arrival at the host to delivery to the
  console is under 2 ms, as reported by the performance metrics.
- **SC-006**: A 2-hour continuous session completes with no disconnect, no crash, no growth in host
  memory use, and no increase in 99th percentile host processing latency between the first and last
  10 minutes.
- **SC-007**: The current official Moonlight release on each supported client platform (Apple TV,
  iPhone/iPad, Android, and Windows, macOS, and Linux desktops) pairs and streams successfully.
- **SC-008**: Turning metrics on raises 99th percentile host processing latency by no more than 2%.
- **SC-009**: Across a test set of at least 5 games, gameplay on the console's own display shows no
  visible stutter or slowdown while streaming, compared with the same games without SunshinePS5
  running.
- **SC-010**: 100% of session attempts from revoked or unpaired devices are refused.
- **SC-011**: On a 360-pixel-wide phone screen, entering a PIN, revoking a device, and turning
  metrics on or off each take no more than 3 taps from the web app's home screen, with no
  horizontal scrolling or zooming. Count taps on app controls for navigation, submission, and
  confirmation. Exclude taps used for typing and dismissing the keyboard; a tap that submits an
  app action still counts even if it also dismisses the keyboard. Required confirmations remain
  part of the three-tap limit.
- **SC-012**: At 1920×1080 and 60 frames per second in HDR, the targets in SC-003 and SC-004 also
  hold.
- **SC-013**: On each supported client platform, using a device with an HDR display, the client
  reports an HDR stream whenever the console outputs HDR and the client requests it. Across the
  game test set, at 1920×1080 and 60 frames per second (a 10-bit capture mode of the HDR test
  hardware), both the side-by-side view and comparison with the recorded 10-bit capture of the
  console's output show no banding, clipping, or washed-out color. The full declared compared-frame
  population has median luma SSIM at least 0.97; at least 99% of declared frames jointly have luma
  SSIM at least 0.95 and Y/Cb/Cr PSNR at least 35/40/40 dB, with perfect planes satisfying the
  corresponding PSNR floor. The fixed known-source HDR ramp/patch scenes, compared with the
  matching native reference, retain at least 512 distinct integer column-median ramp levels, have
  maximum backward ramp step of 4 native codes, maximum neutral/solid-patch median error of 8
  native codes, and zero clipped interior columns after an 8-code reference-endpoint guard.
- **SC-014**: Compared frame by frame with a simultaneous capture of the console's HDMI output, at
  1920×1080 and 60 frames per second in SDR, 99% of stream frames have a structural similarity
  (SSIM) of at least 0.70 with the matching captured frame, and the median luma SSIM of the full
  declared compared-frame population is at least 0.95. From the second release on, the median
  SSIM is also no more than 0.01 below the previous release's on the same reference scenes (a
  fixed set of console screens and gameplay sequences reused for every release).

## Assumptions

- The console runs firmware 13.60 with a user-installed etaHEN homebrew environment, the setup
  verified on the development console. Installing that environment is out of scope. Other firmware
  versions are added by spec amendment once verified on hardware.
- The user launches SunshinePS5 after each console boot through their homebrew environment.
  Starting automatically at boot and waking the console from rest mode are out of scope.
- The console, Moonlight clients, and the phone or browser used for the web app are within the
  permitted private-network boundary in FR-040; they may be on different router-connected private
  subnets. Automatic discovery across subnets is not required; manual host-address entry remains
  available. Success criteria are measured on a wired network unless stated otherwise. Streaming
  from outside this private-network boundary is out of scope (constitution Principle IV).
- Test environment (owner decision, 2026-10-24): for acceptance testing the console joins the home
  network with a static address and no usable gateway, so Apple TV and phone clients on the home
  network reach it directly while it stays offline. Because a router advertising IPv6 could still
  give the console an internet route, the test environment blocks IPv6 routing for the console,
  and every acceptance run begins by confirming the console has no internet access.
- Only one session runs at a time because the console has a single display output.
- The console's on-screen notifications are how SunshinePS5 communicates with a user at the TV.
- "Supported clients" means the current official Moonlight release on each platform listed in
  SC-007 at the time of a SunshinePS5 release.
- Out of scope for this feature: listing or launching installed games from Moonlight, surround
  sound, keyboard and mouse input, microphone input, adaptive triggers and haptic feedback, multiple
  simultaneous viewers, and streaming capture-protected content.
- HDR streaming requires a client device and display that support HDR, and Moonlight's own HDR
  requirements on the client apply.
- HDR test hardware: an Elgato 4K X (4K144 HDR10, model 10GBH9901) sits between the console and an
  HDR TV. Its passthrough feeds the TV, so the console sees an HDR display and outputs HDR, and its
  USB capture output feeds the test machine. The console's single HDMI output then runs through
  the 4K X, so it also provides the SDR captures and the SDR-only Cam Link 4K leaves the chain. HDR
  acceptance waits until this setup is in place.
- Capturing the 4K X's HDR output on macOS needs work beyond standard capture. macOS exposes only
  the device's 8-bit modes; its 10-bit format is reachable only by talking to the device over USB
  directly, which requires administrator rights on the test machine. The MIT-licensed elgato-hdr
  project (https://github.com/talkaboutdesign/elgato-hdr) demonstrates this and is the reference
  planning evaluates for the test client's HDR capture path.
- The 4K X's 10-bit capture modes, per that project, are 3840×2160 at 30 frames per second,
  2560×1440 at 60, 2560×1080 at 60, 1920×1080 at 120, 60, and 30, and 1280×720 at 120. HDR
  comparisons at 4K therefore sample the console's output at 30 frames per second.
- The 4K X does not forward the console's HDR metadata (mastering display and light levels) over
  USB, so the capture cannot confirm FR-025. Planning must define how FR-025 is verified.
- Planning will evaluate reusing the console's built-in Remote Play components (for example, screen
  capture and hardware encoding) wherever that makes development easier. Any reuse calls those
  components on the console at runtime; their code is never copied into the repository and PSN is
  never involved (constitution, Platform, Legal, and Licensing Constraints). Whether those
  components can produce HDR output is unverified and must be established during planning
  research, because HDR is required regardless.
- The protocol test client starts from the owner's moonlight-mcp tool
  (`/Volumes/4TB_Drive/Documents/homelab/moonlight-mcp`). A spike on 2026-10-24 found it needs a
  near rewrite: its pairing, input, and stream checks depended on Wolf-specific APIs, and its
  input never reached a Sunshine-compatible host. The spike code in `tools/moonlight-mcp` and the
  cited protocol notes in `tools/moonlight-mcp/docs/protocol-notes.md` are reference material for
  planning, not accepted work. Being an independent implementation, the test client supplements
  but does not replace verification with official Moonlight clients (SC-007, constitution
  Principle I).
- Accepted risk (owner decision, 2026-10-24): because the web app has no login, any device on the
  local network can approve its own pairing, revoke devices, or stop SunshinePS5. Mitigations are
  the local-network boundary (FR-040), console notifications for every pairing request and
  completed pairing (FR-007), the failed-PIN limit (FR-010), and revocation (FR-043).
- SC-014 includes its 99%-at-0.70 floor, absolute median luma SSIM target of 0.95, and applicable
  prior-release regression gate. The absolute target is informed by the native SDR known-source
  measurements in research-r4-calibration.md; valid per-run calibration, complete populations and
  uncertainty handling remain required. SC-013's objective HDR rules are informed by the known
  native ten-bit source measurements in the same record; per-run calibration, independent source
  metadata, complete coverage and all HDR platform/display/visual evidence remain required.
- The latency targets in SC-003 through SC-005 will be checked for feasibility on hardware during
  planning. If a target proves infeasible, this spec is amended explicitly rather than the target
  being relaxed silently.
- Performance metrics are aimed at developers and advanced users. Ordinary users never need to
  turn them on.
