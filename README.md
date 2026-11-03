# SunshinePS5

SunshinePS5 is a PS5 homebrew payload that turns a jailbroken PlayStation 5 into a
[Moonlight](https://moonlight-stream.org/)-compatible game-streaming host. It speaks the
GameStream/Sunshine protocol, so the **official, unmodified Moonlight client** can pair with the
console, stream its screen and audio, and drive it with a controller.

> **Status: v0.1.1 MVP.** One streaming configuration is supported and has been validated on real
> hardware. Read [Scope and limitations](#scope-and-limitations) before using it.

## What works in v0.1.1

| Area | Supported |
|---|---|
| Console | PS5 on firmware **13.60**, jailbroken, with a payload loader on port 9021 |
| Client | Official **Moonlight for macOS 6.1.0** (other official Moonlight clients are untested) |
| Video | **SDR 1920×1080 H.264 at up to 60 fps**, hardware-encoded on the console, 1–50 Mbit/s |
| Audio | 48 kHz stereo Opus |
| Input | **One** controller (gamepad buttons, sticks, triggers) |
| Pairing | Standard Moonlight PIN pairing, completed in the host's built-in web page |
| Network | Private IPv4 networks only (10/8, 172.16/12, 192.168/16); public peers are refused |

Measured on the development console with the official client over a wired link: 59.98 fps
received and decoded, 0.00% network frame drops, 0.1 ms host processing latency, about 1 ms
network latency ([v0.1.0 record](docs/evidence/v0.1.0-validation.md),
[v0.1.1 record](docs/evidence/v0.1.1-validation.md)). Earlier diagnostic
builds also ran 30-minute and two-hour sessions with at most 0.013% host-to-client frame loss
and flat memory use.

## Requirements

- A PS5 on firmware **13.60** that is already jailbroken with a homebrew environment that
  provides an ELF payload loader on TCP **9021** (for example etaHEN with its ELF loader and
  kstuff). Setting up the jailbreak is out of scope for this project.
- The PS5 and the Moonlight device on the **same private IPv4 network**, preferably wired.
- [Moonlight](https://moonlight-stream.org/) for macOS 6.1.0.
- `socat` on the computer that sends the payload (`brew install socat` / `apt install socat`).

## Install and run

1. Download `sunshineps5-0.1.1.tar.gz` from the release page and verify it:

   ```sh
   shasum -a 256 -c sunshineps5-0.1.1.tar.gz.sha256
   tar -xzf sunshineps5-0.1.1.tar.gz && cd sunshineps5-0.1.1
   shasum -a 256 -c sunshineps5.elf.sha256
   ```

2. With the PS5 jailbroken and its loader listening, send the payload (replace the address with
   your console's):

   ```sh
   ./send-to-ps5.sh 192.168.1.50
   ```

   The terminal shows the host's JSON log. Wait for an `"event":"ready"` line; it reports the
   address and the web page URL, e.g. `"web":"http://192.168.1.50:8081"`. Closing the terminal
   does not stop the host.

   The host picks the first private IPv4 address of an active console interface. Its identity
   and paired clients are stored in `/data/ps5-dev/sunshine-state` on the console and survive
   restarts of the host.

3. **First run only:** the console sees the host's virtual controller as a new pad and may show
   *"Who's using this controller?"*. Once streaming, press **Cross** in Moonlight to pick your
   user (or pick it with a physical controller).

## Pair and stream with Moonlight

1. In Moonlight, choose **Add Host** (the `+` button) and enter the PS5's IP address if it is not
   discovered automatically. Select the new **SunshinePS5** tile; Moonlight shows a 4-digit PIN.
2. On a phone or computer on the same network, open the host's web page
   (`http://<ps5-address>:8081`), enter the PIN under **Pair a device**, and submit. Moonlight
   completes pairing.
3. Moonlight lists one app, **PS5**. Select it to start streaming the console.
4. In Moonlight settings, use **1080p**, **60 FPS**, video codec **H.264** (or Automatic), and keep
   HDR **off**. Around 20 Mbit/s is a good bitrate on a wired network.
5. Quit the stream with Moonlight's normal disconnect (on macOS: **Ctrl+Alt+Shift+Q**). The host
   keeps running and is ready for the next session.

## Scope and limitations

These are deliberate MVP boundaries, not bugs:

- **SDR only.** HDR is not advertised. If the console outputs HDR, set it to SDR for the best
  picture.
- **One controller.** Connect a single controller to Moonlight; multiple controllers are not
  supported in this release. Rumble, motion and touchpad are not supported. Keyboard and mouse
  input from Moonlight are not forwarded; a gamepad is required to control the console.
- **One validated game title.** Streaming the system UI and one game (God of War Ragnarök) was
  tested; other titles are expected to work but are untested.
- **Protected content is not detected.** The console API used here exposes no capture-protection
  state, so the host assumes content may be captured. Do not use it to capture content you are not
  entitled to; the console may black out protected video regardless.
- **Firmware 13.60 only.** Other firmware is refused at startup.
- **Leave the host running.** The host keeps one virtual controller registered between
  sessions. Sending a newer build hands that controller to the new host, and the old host exits;
  nothing stays behind. Killing the host from a shell instead removes the controller: the console
  reports it disconnected. If the console's *"Who's using this controller?"* prompt belonged to a
  controller that was removed, in testing the prompt stayed on screen and the console refused
  every other controller until it was rebooted. Answer the prompt before stopping the host.
- **Private networks only, no login.** Anyone on the private network can open the web page and
  submit a PIN while Moonlight is showing one. Do not expose the console to the internet.

## Build from source

The console build needs the PS5 payload SDK (pinned commit `a7685ce8`), LLVM 18.1.8, and
target builds of OpenSSL 3.5.x, ENet and libopus. Paths are set with environment variables; see
the header of [`scripts/build-ps5-stream.sh`](scripts/build-ps5-stream.sh).

```sh
PS5_PAYLOAD_SDK=... PS5_OPENSSL_DIR=... PS5_OPUS_LIB=... PS5_OPUS_INCLUDE=... \
PS5_ENET_DIR=... PS5_ENET_INCLUDE=... ./scripts/build-ps5-stream.sh   # -> dist/sunshineps5-stream.elf
./scripts/package-release.sh                                              # -> dist/sunshineps5-<version>.tar.gz
```

The portable core (protocol, pairing, sessions, web app) builds and tests on macOS/Linux:

```sh
cmake -S . -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
./scripts/check-quality.sh    # formatting, clang-tidy, clippy, biome, ruff
```

## Repository layout

| Path | Contents |
|---|---|
| `core/` | Portable C11 host: HTTP/pairing, RTSP, video/audio/control protocols, FEC, sessions, settings, web API |
| `platform/` | PS5 adapters (capture, hardware encode, audio, virtual controller, system) and the console entry point `main_stream.c` |
| `webapp/` | The embedded pairing/management web page |
| `tests/` | Unit, fuzz and browser tests, fakes and scenario files |
| `tools/moonlight-mcp/` | Rust Moonlight protocol test client used for automated validation |
| `tools/ps5-probes/` | Non-shipping research probes used to establish console capabilities |
| `specs/` | Feature specification, plan, research and readiness records |
| `docs/` | Release notes and evidence summaries |

## License

Apache License 2.0; see [LICENSE](LICENSE). Moonlight, Sunshine, PlayStation and PS5 are
trademarks of their respective owners; this project is not affiliated with them.
