<!--
Sync Impact Report
- Version change: 1.0.0 → 1.1.0 (MINOR: materially changed guidance in Principle V and the
  CI gates)
- Modified principles:
  - V. Portable Core, Thin Platform Layer: end-to-end CI now uses the project's protocol test
    client (tools/moonlight-mcp) instead of requiring an unmodified Moonlight client; added the rule
    that official clients are authoritative when they disagree with the test client.
- Modified sections:
  - Development Workflow and Quality Gates: CI gates now name formatters and linters per language
    (C/C++ and Rust) and include the test client's build.
- Added sections: none
- Removed sections: none
- Templates reviewed: plan-template.md, spec-template.md, tasks-template.md,
  checklist-template.md (no edit required; plan-template reads gates from this file at runtime)
- Deferred TODOs: none
- Note: 1.0.0 was ratified earlier on 2026-10-24 and never committed; its report is superseded.
-->

# SunshinePS5 Constitution

SunshinePS5 is a PS5 homebrew application that acts as a Moonlight-compatible game streaming
host, letting users on the local network control the console and play its games from a standard
Moonlight client.

## Core Principles

### I. Moonlight Client Compatibility (NON-NEGOTIABLE)

- SunshinePS5 MUST work with unmodified, officially released Moonlight clients. A feature that
  requires a patched, forked, or plugin-extended client is not acceptable.
- Expected protocol behavior is defined by current upstream Moonlight clients and the Sunshine
  host. If a supported client fails or degrades against SunshinePS5, that is a SunshinePS5 defect,
  regardless of what any written protocol description says.
- Capabilities the console cannot provide MUST be declined through the protocol's normal
  negotiation (for example, by not advertising a codec or mode), never by accepting a request and
  then failing the session.
- The project MUST maintain a supported-client matrix (client, platform, version). Every release
  MUST be verified against every entry in it.

Rationale: the value of the project is that users need nothing beyond the Moonlight client they
already have.

### II. Latency Is the Product

- The hot paths are: frame capture → encode → packetize → send, and input receive → inject.
- During a steady-state session, hot-path code MUST NOT allocate heap memory, use unbounded
  queues, perform blocking I/O other than designated send and wait points, or log unsampled
  per-frame messages.
- The p99 per-frame host pipeline time (capture start to last packet handed to the network stack)
  MUST stay below the frame interval of the negotiated frame rate (16.67 ms at 60 fps, 8.33 ms at
  120 fps). Exceeding it means frames queue and latency grows without bound.
- Input handling MUST run on a path that never waits on video capture, encode, or send.
- Video MUST be encoded by the console's hardware encoder. CPU software encoding is not an
  acceptable shipping path, because the CPU belongs to the running game.
- The host MUST expose per-stage timing metrics (capture, encode, packetize, send, input
  receive-to-inject) that can be read without attaching a debugger.
- Changes to a hot path MUST include before/after measurements from those metrics on hardware. A
  p99 regression greater than 5% blocks merge unless the PR justifies it and review accepts it.

### III. The Console Comes First

- Host threads MUST run at priorities and on cores that do not starve the foreground game. Each
  feature plan MUST declare its CPU and memory budget, verified on-device.
- Streaming buffers MUST be allocated at session start from fixed-size pools. Host memory use MUST
  NOT grow over the course of a session.
- Any failure inside a session MUST end that session cleanly and leave the console usable. The
  host MUST NOT panic the kernel or hang the system.
- When a session ends, the client disconnects, or the control channel times out, all injected
  input state MUST be released immediately (no held buttons or deflected sticks) and all capture
  and encode resources freed.
- Every session termination MUST be logged with a reason code.
- Code that needs elevated privileges or kernel memory access MUST be confined to a single,
  minimal module. Each kernel patch or hook MUST document the firmware versions it supports, its
  purpose, the source of every offset it uses, and how it is reverted.
- Firmware-specific offsets and symbols MUST live in per-firmware data tables, never inline. On an
  unsupported firmware version the app MUST refuse to start and report the detected version; it
  MUST NOT guess or fall back to the nearest known version.
- Stopping the app MUST remove every hook it installed and return the console to its pre-launch
  state without a reboot.

### IV. Secure by Default on the Local Network (NON-NEGOTIABLE)

- All network input is untrusted. The host runs with elevated privileges, so a memory-safety bug
  in any network-facing code is remote code execution on the console.
- No stream, input, or app-launch request is honored from a client that has not completed
  Moonlight PIN pairing. Pairing state MUST persist across restarts, and users MUST be able to list
  and revoke paired clients.
- The project MUST NOT provide any mechanism that exposes the console beyond the local network:
  no UPnP, NAT-PMP, relays, or cloud services. Connections from non-private source addresses MUST
  be rejected by default.
- The app MUST NOT send telemetry or communicate with Sony online services. Outbound traffic is
  limited to serving paired clients' sessions.
- Cryptography MUST come from an established, maintained library. No hand-rolled cryptographic
  primitives.
- Every parser of network data (HTTP/HTTPS, RTSP/SDP, control, input) MUST have a fuzz target that
  runs in CI.
- Host identity keys and certificates MUST be generated on the device at first run. No keys or
  secrets are committed to the repository.

### V. Portable Core, Thin Platform Layer

- The codebase is split into a portable core and a PS5 platform layer.
- The core contains protocol handling, pairing, RTSP/SDP, packetization, FEC, encryption, input
  translation, configuration, and session state. It MUST build and pass its tests on Linux and
  macOS, and MUST NOT include PS5 SDK headers or call PS5 system functions.
- The platform layer contains capture, hardware encode, audio capture, input injection, and system
  integration. Each sits behind a narrow interface with a fake implementation used by core tests.
- New core behavior MUST ship with unit tests. Every bug fix MUST ship with a regression test that
  fails without the fix.
- CI MUST run end-to-end sessions (pair, launch, stream, input, teardown) between the core, using
  fake platform backends, and the project's protocol test client (`tools/moonlight-mcp`). That
  client is an independent implementation: it supplements verification with official Moonlight
  clients (Principle I) and never replaces it.
- When the test client and an official Moonlight client disagree about host behavior, the
  official client is authoritative and the test client is corrected. The test client MUST NOT be
  relaxed to accept behavior official clients reject.
- Platform-layer changes MUST record on-device verification in the PR: firmware version, client
  and version, resolution, frame rate, codec, and observed results.

Rationale: most of the code can be tested on every commit without a console. Only the thin layer
that touches the console needs hardware verification.

### VI. Intentional Code, No Slop (NON-NEGOTIABLE)

Every line has a reason a reviewer can verify.

- No dead code, commented-out code, or abstractions without a current caller. Do not build for
  hypothetical future requirements.
- No placeholder implementations, stubbed return values, or TODOs merged without a linked issue.
- No invented facts. Every PS5 system interface, firmware offset, syscall, and protocol constant
  MUST cite its source: an upstream code reference, reverse-engineering notes in the repository,
  or recorded measurements. An unverified assumption MUST be labeled as one and MUST NOT reach a
  shipping code path.
- Comments explain why: intent, constraints, and invariants. They MUST NOT restate the code.
- Errors are handled or explicitly propagated. No ignored return codes, no silent catch-alls.
- Generated code, whether from AI tools or other tooling, is held to the same standard. The PR
  author owns every line and MUST be able to explain it. Generated files are committed only when
  produced by a checked-in, reproducible generator.
- Documentation describes only tested, shipping behavior. README and release notes MUST NOT claim
  features, performance, or compatibility that have not been verified.

## Platform, Legal, and Licensing Constraints

- **Target environment**: PS5 consoles running a user-installed homebrew environment on a
  firmware version listed in the project's supported-firmware matrix. SunshinePS5 runs on top of
  that environment and MUST NOT contain or distribute exploits.
- **Toolchain**: Built exclusively with open-source toolchains and SDKs. One documented command
  MUST produce the PS5 artifact from a clean checkout, using pinned toolchain and dependency
  versions.
- **Prohibited content**: The repository MUST NOT contain Sony proprietary code, leaked SDK
  material, firmware images, keys, or copyrighted game content.
- **Prohibited functionality**: The project MUST NOT implement or assist piracy, game decryption,
  circumvention of game DRM, or interaction with PSN or the user's account.
- **Licensing**: The project is licensed Apache-2.0. Every incorporated source file and linked
  dependency MUST be license-compatible with Apache-2.0 and recorded in THIRD_PARTY_NOTICES with
  its license and source version. GPL-licensed implementations, including Sunshine and
  moonlight-common-c, MAY be read as protocol references but MUST NOT be copied, translated, or
  linked into shipped artifacts. Changing this requires a constitution amendment and relicensing
  the project.

## Development Workflow and Quality Gates

- **Spec-driven work**: Features and non-trivial changes follow spec → plan → tasks → implement.
  A plan MUST pass the Constitution Check before research and again after design. Trivial changes
  (typos, documentation fixes, dependency bumps with no behavior change) may skip the spec flow but
  never skip review or CI.
- **Branching**: `main` is always releasable. Work happens on short-lived branches merged by pull
  request; direct pushes to `main` are blocked. History stays linear (squash or rebase merges).
- **Commits**: Conventional Commits format. Every commit on `main` builds and passes tests.
- **Pull requests**: One logical change per PR. A PR SHOULD stay under roughly 400 changed lines,
  excluding tests and generated files. The description states what changed and why, links its
  issue or spec, and includes test evidence. Hot-path PRs include measurements (Principle II);
  platform-layer PRs include on-device verification (Principle V).
- **Review**: Every PR requires approval from a human maintainer who has read the full diff.
  Agent-authored changes are never merged without that review.
- **Required CI gates**: formatters and linters clean for every language in the repository
  (clang-format and clang-tidy for C/C++; rustfmt and Clippy for Rust); warning-free builds of the
  core on Linux and macOS, of the PS5 artifact, and of the test client, with warnings treated as
  errors; unit and end-to-end tests passing; host test runs under AddressSanitizer and
  UndefinedBehaviorSanitizer; fuzz targets run time-boxed per PR and longer nightly; license and
  dependency audit passing.
- **Releases**: Semantic Versioning. CHANGELOG.md follows Keep a Changelog and is updated in the PR
  that makes the change. Each release ships a tagged artifact with checksums, release notes, the
  supported-firmware and supported-client matrices, and known issues. Before tagging, an on-device
  soak test of at least two hours of continuous streaming MUST complete with no crash, no memory
  growth, and no p99 frame-time drift.
- **Issue tracking**: All work is tracked in the project issue tracker. Bug reports MUST include
  firmware version, client and version, network setup, reproduction steps, and logs. An issue is
  done when its change is merged with tests, documentation is updated, and its CHANGELOG entry
  exists.

## Governance

- This constitution overrides every other project guideline, convention, and agent instruction
  file (including CLAUDE.md). When they conflict, the constitution wins and the conflicting
  document is corrected.
- **Amendments** are made by a pull request that edits this file, states the rationale, bumps the
  version, and updates any affected templates or documents in the same PR. Amendments require
  maintainer approval.
- **Versioning**: MAJOR for removing or redefining a principle in a backward-incompatible way;
  MINOR for adding a principle or section or materially expanding guidance; PATCH for
  clarifications and wording fixes that do not change meaning.
- **Exceptions**: Principles marked NON-NEGOTIABLE admit no exceptions. Any other rule may be
  excepted only through an entry in the plan's Complexity Tracking table, with a justification and
  a linked follow-up issue. An exception does not set precedent.
- **Compliance review**: Reviewers check every PR against this constitution. At each minor release
  the maintainers review the constitution itself; a rule that is not being followed is either
  enforced or removed.

**Version**: 1.1.0 | **Ratified**: 2026-10-24 | **Last Amended**: 2026-10-24
