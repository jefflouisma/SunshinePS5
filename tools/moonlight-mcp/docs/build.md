# Building the protocol test client

Run these commands from `tools/moonlight-mcp`. This builds the independent Rust
client, not the PS5 host. A successful build alone does not establish reference-host,
console, or capture acceptance.

## Pinned inputs

`rust-toolchain.toml` selects Rust **1.93.0**, edition 2021, with the minimal
profile plus rustfmt and Clippy. `Cargo.toml` declares the matching Rust 1.93
compiler floor; compatibility with the former 1.85 minimum has not been verified.
Use rustup-managed `cargo` and `rustc` on `PATH`, and run inside this crate so
rustup discovers its toolchain file. Running from the repository root with only
`--manifest-path` does not select this directory's Rust toolchain.

```sh
rustup show active-toolchain
rustc --version
cargo --version
```

The active toolchain must be `1.93.0` for the machine's target; both version
commands must report `1.93.0`. Install that exact toolchain if missing:

```sh
rustup toolchain install 1.93.0 --profile minimal --component rustfmt --component clippy --no-self-update
```

Keep `Cargo.lock` and use `--locked`; do not run `cargo update` as a setup step.
The T004 reconciliation changes only the manifest's compiler floor: Cargo already
accepts the existing lockfile, and no package version, checksum, dependency edge,
or feature selection was changed. The package keeps `unsafe_code = "forbid"`;
the lint applies to this package, not dependency internals. Privileged USB access
belongs in the separate native helper, never Rust FFI in this client.

The historical T004 lockfile's SHA-256 was
`65c49e07d0e9c6edde18ba0fefdf5eb7a91b593580cca2cd567a833b9631ef46`.
The later [dependency advisory remediation](#dependency-advisories) updates that
lockfile and records the current digest and remaining open risks.

## Native prerequisites

Provide a native C compiler/linker and CMake (4.3.4 in the verified build).
The locked `opus` 0.4.0 → `opusic-sys` 0.7.6 dependency builds its bundled
libopus 1.6.1 statically; no system Opus or `opus.pc` discovery is used.
The separate libusb helper still requires its native development package and
pkg-config metadata. An unversioned install is not a reproducible substitute.

| Prerequisite | Required version | Verify installed version | Purpose |
|---|---|---|---|
| FFmpeg | 8.0.1 | `ffmpeg -version` | External decode/record/compare tooling; executable on `PATH` |
| ffprobe | 8.0.1 | `ffprobe -version` | External media inspection; executable on `PATH` |
| libopus | 1.6.1 | Locked `opusic-sys` crate's `opus/package_version` | Bundled native library built through CMake and statically linked by `opusic-sys` 0.7.6 |
| libusb | 1.0.29 | `pkg-config --modversion libusb-1.0` | Separate native HDR capture helper prerequisite; not a Rust client link dependency |

On macOS, Xcode Command Line Tools supply the native compiler/linker. On Linux,
provide a C build toolchain, CMake and the matching libusb development package.
If libusb is installed outside pkg-config's search path, set `PKG_CONFIG_PATH`
to the directory containing `libusb-1.0.pc` when building that helper.
The client CI checks the locked binding, bundled feature and native source
version before compilation; Cargo verifies the crate against `Cargo.lock`.
FFmpeg/ffprobe and libusb are workflow prerequisites, not proof of an available
capture device, valid HDMI signal, or implemented ten-bit capture path.

## Build and test

```sh
cargo build --locked
cargo test --locked
```

For a warning-as-error check, use the same commands with `RUSTFLAGS=-Dwarnings`.
Do not weaken lints or exclude the binary to turn a source failure into a pass.
The shared quality entry point is tracked separately by T003.

## Local observation (2026-10-24)

Observed on this macOS Apple Silicon development machine, without opening a
console connection or capture device:

| Command | Observed result |
|---|---|
| `rustc --version` | `rustc 1.93.0 (254b59607 2026-01-19)` |
| `cargo --version` | `cargo 1.93.0 (083ac5135 2025-12-15)` |
| `ffmpeg -version` | `ffmpeg version 8.0.1`; `/opt/homebrew/bin/ffmpeg` |
| `ffprobe -version` | `ffprobe version 8.0.1`; `/opt/homebrew/bin/ffprobe` |
| `pkg-config --modversion opus` | `1.5.2` |
| `pkg-config --modversion libusb-1.0` | `1.0.29` |

The default installed toolchain was named `stable` but reported 1.93.0. To verify
the exact pin without changing the machine's default toolchain or configuration,
T004 installed a separate Rust 1.93.0 toolchain cache and used:

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
export RUSTUP_HOME="$TMPDIR/sunshine-t004/rustup"
export CARGO_TARGET_DIR="$TMPDIR/sunshine-t004/target"
export PATH="$RUSTUP_HOME/toolchains/1.93.0-aarch64-apple-darwin/bin:$PATH"
rustup show active-toolchain
cargo build --locked
cargo test --locked
cargo clippy --version
cargo clippy --locked --all-targets -- -D warnings
rustfmt --check --config skip_children=true src/main.rs
```

The explicit `PATH` selects every toolchain binary, including Cargo subcommands.
On this machine, `rustup run 1.93.0 cargo clippy --version` alone found a different
Clippy (0.1.98); prepending the pinned bin directory reports
`clippy 0.1.93 (254b59607d 2026-01-19)`. Do not mistake the former for a pinned
quality check. The corresponding pinned directory differs on other targets;
`rustup which --toolchain 1.93.0 cargo` identifies it.

Historical T004 results, before the T010 reconciliation below:

| Verification | Result |
|---|---|
| `cargo build --locked` | Exit 0; 35 existing dead-code warnings |
| `cargo test --locked` | Exit 0; 27 passed, 0 failed; 11 existing dead-code warnings in the test binary |
| `cargo clippy --locked --all-targets -- -D warnings` | Exit 101; existing dead code plus `manual_is_multiple_of` in `crypto.rs` (2) / `decode.rs` (1), and test `identity_op` in `imgcmp.rs` (1) |
| `rustfmt --check --config skip_children=true src/main.rs` | Exit 0 with Rust 1.93.0 rustfmt and the T003 configuration |
| SHA-256 comparison against the pre-change lockfile | Identical; no dependency update |

Command logs are local at `/Volumes/4TB_Drive/tmp/sunshine-t004/`:
`build-final.log`, `test-final.log`, and `clippy-pinned.log`. Native version
observations are inventory evidence, not hardware or interoperability results.

## Spike compile repair

The initial locked build and test both exited 101 with ten compiler errors.
The coordinator explicitly extended T004 ownership to minimal compile repairs.
Only `src/main.rs` changed:

- Supply the existing persisted `ClientIdentity::unique_id` to HTTP server-info
  and unpair calls, matching their current signatures.
- Replace nonexistent `pairing::pinned_cert` with the existing
  `Paths::load_host_cert` lookup for deciding whether server-info can use HTTPS.
- Wire HTTPS server-info, app-list and quit through the existing
  `PairedHost::connect`, `server_info`, `app_list`, and `cancel` methods. Remove
  the redundant broken `paired_client` wrapper and its unused imports.
- Bind the HTTPS result explicitly as `ServerInfo` before serialization,
  eliminating the never-type fallback failure without weakening lints.
- Format this file with the pinned rustfmt and T003 configuration.

All existing command names remain available. This is API wiring, not a new
pairing implementation, streaming runner, reference-host test, or proof that the
commands interoperate with hardware.

## T010 portable quality reconciliation

The coordinator extended T010 ownership to retire the recorded quality debt before
activating strict portable CI. Rust 1.93.0 rustfmt now formats all compiled modules.
The four Clippy fixes preserve behavior: replace two AES block-alignment remainders
and the decoded-frame remainder with `is_multiple_of`, and remove `0 +` from the
image-downscale test's expected sum.

`src/lib.rs` exposes the same fourteen modules previously declared by `main.rs`.
The binary imports them from `moonlight_mcp` and retains the existing commands.
The library makes existing protocol/media APIs available to integration tests;
it does not add a streaming runner or assert protocol interoperability. No existing
item was removed, no additional source module was activated, and no lint was suppressed.
In particular, the standalone `input.rs` and `rtsp.rs` files remain outside the crate's
module tree, as before this reconciliation.

With the pinned toolchain bin directory first on `PATH`, run from the repository root:

```sh
cargo_path=$(rustup which --toolchain 1.93.0 cargo)
export PATH="$(dirname -- "$cargo_path"):$PATH"
RUSTFLAGS=-Dwarnings cargo build --manifest-path tools/moonlight-mcp/Cargo.toml --locked --all-targets
RUSTFLAGS=-Dwarnings cargo test --manifest-path tools/moonlight-mcp/Cargo.toml --locked --all-targets --all-features
cargo clippy --manifest-path tools/moonlight-mcp/Cargo.toml --locked --all-targets --all-features -- -D warnings
cargo fmt --manifest-path tools/moonlight-mcp/Cargo.toml --all -- --check
```

Local T010 verification: all four commands exit zero, with 27 library tests passing.
The CLI target has no tests; CI requires a nonempty aggregate Rust test list and
explicitly requires both real C behavior suites (`test_runtime`, `test_fakes`).
The lockfile retains the SHA-256 recorded above. Full repository quality checks
also pass through `scripts/check-quality.sh`.

The original T010 CI built Opus 1.5.2 statically into runner scratch storage rather than
substituting an arbitrary system Opus. The source archive at
`https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz` is pinned to SHA-256
`65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`.
That digest identifies the archive downloaded and hashed during T010; no publisher
signature verification is claimed. Local CMake build/install and pkg-config report
1.5.2. FFmpeg/ffprobe and the separate USB helper are not exercised by these unit
checks. This verification provides no console, reference-host, capture-device,
latency, or fidelity evidence, and GitHub Actions itself was not run locally.
The 2026-10-31 Opus remediation below supersedes that native build step.

## Dependency advisories

T011 follow-up, observed locally on 2026-10-24 (audit timestamps use UTC).
The baseline audit recorded **24 dependency/advisory matches across nine crates**;
RustSec and GHSA aliases are counted separately by the audit. Fixed releases were
selected from the live [OSV API](https://google.github.io/osv.dev/api/) advisory
ranges, choosing the smallest compatible release that closes every reported
advisory for each crate. No application source, manifest requirement, toolchain
pin, audit script, workflow, exclusion, or allowlist changed.

| Crate | Before → after | Baseline advisory IDs resolved |
|---|---|---|
| `anyhow` | `1.0.100` → `1.0.103` | RUSTSEC-2026-0190 |
| `bytes` | `1.11.0` → `1.11.1` | GHSA-434x-w66g-qw3r; RUSTSEC-2026-0007 |
| `rand` | `0.8.5` → `0.8.6` | GHSA-cq8v-f236-94qc; RUSTSEC-2026-0097 |
| `rustls` | `0.23.36` → `0.23.46` | GHSA-2mjx-qc3c-rqvc; RUSTSEC-2026-0285; RUSTSEC-2026-0346; RUSTSEC-2026-0348; RUSTSEC-2026-0349 |
| `rustls-webpki` | `0.103.9` → `0.103.16` | GHSA-82j2-j2ch-gfr8; GHSA-965h-392x-2mh5; GHSA-pwjx-qhcg-rvj4; GHSA-xgp8-3hg3-c2mh; RUSTSEC-2026-0049; RUSTSEC-2026-0098; RUSTSEC-2026-0099; RUSTSEC-2026-0104; RUSTSEC-2026-0382 |
| `time` | `0.3.45` → `0.3.47` | GHSA-r6v5-fh4h-64xc; RUSTSEC-2026-0009 |
| `tokio` | `1.49.0` → `1.51.6` | RUSTSEC-2026-0386 |

The required transitive changes are `libc` 0.2.180 → 0.2.183, `mio` 1.1.1 →
1.2.0, `socket2` 0.6.1 → 0.6.3, `tokio-macros` 2.6.0 → 2.7.0, `num-conv`
0.1.0 → 0.2.0, `time-core` 0.1.7 → 0.1.8, and `time-macros` 0.2.25 → 0.2.27.
The new Tokio requires these Mio/socket/macro minimums; Mio requires libc
0.2.183. The fixed time release requires num-conv 0.2 and the exact time-core
and time-macros releases above. These are dependency-internal changes accepted
by the unchanged direct requirements, with no client API migration. The Windows
graph now uses the already-present windows-sys 0.61.2 instead of 0.60.2, removing
the obsolete windows-targets 0.53.5 and its eight architecture packages. Selecting
tokio-macros 2.7.0 avoids the unnecessary syn 3 dependency chosen by 2.7.2;
the existing syn 2 version remains unchanged. Other package versions are unchanged.

`Cargo.lock` SHA-256 at that earlier update (superseded by the migrations below):
`0324ef95f4a746cb65a9cfe44739f2e4dc4a8f7e64604ae970c81805f2abf3a3`.

### Resolved dependency: RUSTSEC-2023-0071 — rsa 0.9.10

The [RustSec advisory](https://rustsec.org/advisories/RUSTSEC-2023-0071.html)
still reports no patched `rsa` release. The 2026-10-31 migration removes that
crate and uses `aws-lc-rs` 1.19.0 / `aws-lc-sys` 0.47.0 for RSA key generation,
PKCS#8 loading, SHA-256/PKCS#1 v1.5 signing and host-proof verification. It adds a
native C/CMake build dependency. TLS and certificate generation retain their
existing rustls/ring and rcgen paths.

Existing PKCS#8 PEM keys, certificates, unique IDs and certificate fingerprints
remain unchanged when loaded. A synthetic identity generated with `rsa` 0.9.10
produces the identical deterministic signature under AWS-LC and OpenSSL.
Identity keys must be 2048–8192 bits; host proof verification also requires
2048–8192 bits. Signing errors now propagate as errors before sending the proof.
Malformed, partial, mismatched and undersized identities fail closed.

The [verification record](../../../.omo/evidence/rsa-backend-remediation-20261031/REPORT.md)
retains 217 passing Rust tests, Clippy/format/build checks, cross-backend evidence
and a complete live dependency audit with zero matches. This removes the affected
dependency; it does not prove absence of timing leaks in every backend or replace
hosted CI and live pairing acceptance. No task marker was changed.

### Resolved dependency: RUSTSEC-2026-0150 — audiopus_sys 0.2.2

The [RustSec advisory](https://rustsec.org/advisories/RUSTSEC-2026-0150.html)
marks `audiopus_sys` unmaintained, with **no patched release**, and describes its
bundled build's incompatibility with CMake 4.0. On 2026-10-31, `opus` was updated
from 0.3.1 to 0.4.0, replacing `audiopus_sys` 0.2.2 with `opusic-sys` 0.7.6.
The old binding and its now-unused `pkg-config` crate leave the lock entirely.
The Rust audio API is unchanged; `src/audio.rs` uses it for stereo decoding,
including encrypted, malformed, silent, reordered and generated-audio checks.

The new binding builds bundled libopus 1.6.1 using CMake and links it statically.
This deliberately supersedes the old system/CI Opus 1.5.2 selection. Portable CI
now fails if `audiopus_sys` returns, either binding version changes unexpectedly,
the bundled feature disappears, or the bundled native version differs. It no
longer downloads an unused system Opus archive. The dependency audit includes
the native source bytes within the `opusic-sys` crate checksum; it does not
independently authenticate the publisher or establish runtime security.

The [2026-10-31 verification record](../../../.omo/evidence/opus-advisory-remediation-20261031/REPORT.md)
records the exact lock and crate digests, local CMake 4.3.4 build, audio/full-suite
checks and fresh audit outcome. Linux/macOS hosted CI and live audio acceptance
still require their own runs. The later RSA migration above removes the remaining
affected client dependency.

### Reproduction and verification

Run version changes from `tools/moonlight-mcp` with the pinned toolchain bin
directory first on `PATH`; these are maintenance commands, not setup commands:

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
cargo_path=$(rustup which --toolchain 1.93.0 cargo)
export PATH="$(dirname -- "$cargo_path"):$PATH"
cargo update -p anyhow --precise 1.0.103
cargo update -p bytes --precise 1.11.1
cargo update -p rand@0.8.5 --precise 0.8.6
cargo update -p rustls --precise 0.23.46
cargo update -p rustls-webpki --precise 0.103.16
cargo update -p time --precise 0.3.47
cargo update -p tokio --precise 1.51.6
cargo update -p tokio-macros --precise 2.7.0
cargo update -p mio --precise 1.2.0
cargo update -p socket2@0.6.5 --precise 0.6.3
cargo update -p libc --precise 0.2.183
cargo update -p num-conv --precise 0.2.0
export CARGO_TARGET_DIR="$TMPDIR/sunshine-t011-remediation/target"
cargo build --locked
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo fmt --check
```

Run the final checks from the repository root:

```sh
export CARGO_TARGET_DIR="$TMPDIR/sunshine-t011-remediation/quality-target"
./scripts/check-quality.sh
export AUDIT_REPORT_PATH="$TMPDIR/sunshine-t011-remediation/dependency-audit.json"
./scripts/audit-dependencies.sh
```

| Verification on the final lockfile | Observed result |
|---|---|
| `cargo build --locked` | Exit 0, no warnings |
| `cargo test --locked` | Exit 0; 27 library tests passed, 0 failed; binary/doc targets contain no tests |
| `cargo clippy --locked --all-targets --all-features -- -D warnings` | Exit 0 |
| `cargo fmt --check` | Exit 0 |
| `./scripts/check-quality.sh` | Exit 0; all checks passed |
| `./scripts/audit-dependencies.sh` | **Exit 1, fail**; exactly two advisory matches; no incomplete checks |

The audit completed all host archive/integrity and vulnerability checks, all
client crate archive checks, and 244 live client OSV queries. Every audited input
hash stayed unchanged during the run. The complete remaining-match mapping is:

| Remaining match | Open-risk entry above |
|---|---|
| `client rsa@0.9.10` / RUSTSEC-2023-0071 | [Historical match, resolved by backend migration](#resolved-dependency-rustsec-2023-0071--rsa-0910) |
| `client audiopus_sys@0.2.2` / RUSTSEC-2026-0150 | [Historical match, resolved by the later Opus update](#resolved-dependency-rustsec-2026-0150--audiopus_sys-022) |

All seven fixable crates were updated, resolving 22 baseline matches. Those two
risks failed the dependency gate at that time: that remediation's completion
is not a passing dependency audit. Licensing remains excluded by owner direction,
never passed. Local checks do not establish Linux CI, reference-host pairing,
console, capture, timing-side-channel, or media-decoding acceptance.

Logs (`build.log`, `test.log`, `clippy.log`, `fmt.log`, `quality.log`, `audit.log`),
the JSON audit, baseline lock snapshot, and raw OSV records are retained locally
under `/Volumes/4TB_Drive/tmp/sunshine-t011-remediation/`.

## Scenario and pairing foundation (T019–T020)

`src/scenario.rs` parses the shared version 1.0.0 scenario declaration without
network or output side effects. It freezes the complete declaration, checks the
versioned acceptance profile, controller/event identities and action ordering,
reference format, explicit provider references, and positive disk/file limits.
`resolve_output` checks an existing selected root and rejects output symlinks;
T028's writers must enforce the declared quotas and recheck containment at creation.
Parsing a scenario does not run it; runner integration remains T036.

Automated Sunshine PIN delivery requires `pair --pin-delivery sunshine:URL
--sunshine-certificate FILE --sunshine-user USER --sunshine-password PASSWORD`.
`FILE` contains the single trusted PEM leaf certificate of the Sunshine HTTPS
web UI, obtained through a trusted local channel. This is a separate trust input
from the GameStream certificate authenticated by the PIN protocol. Missing or
invalid certificates are refused; HTTPS checks the exact leaf and its handshake
signature, and redirects are disabled. `pair --help` describes the option.
Manual PIN delivery is an explicit interactive strategy; unattended callers use
`pair_unattended`, which refuses it before contacting a host. Scenario providers
are resolved from separately supplied local configuration; no credentials or
commands belong in retained scenario files.

Pairing commits host and endpoint certificate pins only after the SHA-256 PIN
proof, host RSA signature, client signed response, and final pinned mutual-TLS
`paired=1` confirmation. Changed pins require explicit removal/re-pairing rather
than silent trust replacement. The retained `nvhttp` module re-exports the HTTP
implementation in `http.rs`; `identity.rs` and `tls.rs` retain their public roles
with state integrity, atomic file writes, private permissions, and exact pins.
A partial or damaged identity fails closed; choose a fresh `--config-dir` or
restore it rather than regenerating paired state. An interrupted writer's
`identity.lock` requires inspection before explicit removal.

Tests use generated identities and loopback synthetic protocol/TLS fixtures.
They do not establish live compatibility. T038 still must retain versioned
reference-host pairing, app listing, persistence/reconnect, PIN-provider delivery,
official-client cross-checks, and the complete stream/input/audio/HDR/fidelity/
teardown evidence. No real host or console was contacted for these tasks.

## Shared monotonic receipts (T027)

The exact `nix =0.31.3` dependency enables only `time` (default features disabled),
providing safe `clock_gettime`/`clock_getres` wrappers while this crate keeps
`unsafe_code = "forbid"`. See the [upstream API](https://docs.rs/nix/0.31.3/nix/time/).
Its libc minimum requires the locked libc update from 0.2.183 to 0.2.191.
Both macOS and Linux use **CLOCK_MONOTONIC**, also required of the isolated C helper;
CLOCK_UPTIME_RAW is a different domain. `clock.rs` owns this selection and hashes
the local boot UUID into the domain identity. Cross-process receipts share the OS
epoch, with integer microseconds rounded down and a <1000 ns quantization bound.
Native resolution comes from clock_getres; timestamp/capture accuracy remains
uncalibrated and must not be inferred from storage precision or these unit tests.

T027 dependency re-audit: `./scripts/audit-dependencies.sh` completed 245 live OSV
queries and returned exit 1 for exactly the existing `rsa` RUSTSEC-2023-0071 and
`audiopus_sys` RUSTSEC-2026-0150 matches; **no new matches** were introduced by
nix/libc. That historical audit remained failing for those two risks; the later
Opus remediation above removes the unmaintained binding.
