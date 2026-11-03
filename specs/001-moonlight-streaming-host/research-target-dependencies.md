# T043 target dependencies

Recorded 2026-10-25. **T043 complete: cross-build/link and bounded device smoke passed.**
This establishes target dependency feasibility, not production-host readiness or R5 budgets.

The nonshipping [probe](../../tools/ps5-probes/dependencies.c) and
[lock](../../cmake/dependencies.lock.json) are the authoritative sources/build pins.
Local evidence and commands are retained in `/Volumes/4TB_Drive/tmp/t043-t044/`.
No keys/certificates persist: the smoke identity is generated in memory and freed.
It is not a pairing identity or a production trust policy.

## Sources and selection

| Component | Exact source | Relevant source/permission boundary |
|---|---|---|
| Public payload SDK 0.45 | `a7685ce8e97ce1f8a4f38253a41d54241c9124f1` | [target POSIX declarations](https://github.com/ps5-payload-dev/sdk/tree/a7685ce8e97ce1f8a4f38253a41d54241c9124f1) for sockets, pthreads, clocks; installed target headers were read before calls |
| OpenSSL 3.5.9 | `45e844fa2a14ec92d146bd8f5778ac130b6625fb` | [TLS source](https://github.com/openssl/openssl/tree/45e844fa2a14ec92d146bd8f5778ac130b6625fb/ssl), [OS seeding](https://github.com/openssl/openssl/blob/45e844fa2a14ec92d146bd8f5778ac130b6625fb/providers/implementations/rands/seeding/rand_unix.c), `crypto/threads_pthread.c`, `include/openssl/*.h.in` |
| Selected host ENet | `cgutman/enet` `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1`, version 1.3.17 Moonlight fork | [API](https://github.com/cgutman/enet/blob/aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1/include/enet/enet.h), [wire structs](https://github.com/cgutman/enet/blob/aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1/include/enet/protocol.h), [socket implementation](https://github.com/cgutman/enet/blob/aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1/unix.c), `host.c`, `protocol.c` |
| Independent wire counterpart | `rusty_enet` 0.4.0 | Same registry package/checksum as `tools/moonlight-mcp/Cargo.lock`; standalone scratch program, no changes to the test client |

[Protocol notes section 11](../../tools/moonlight-mcp/docs/protocol-notes.md#11-enet-transport-for-the-control-stream)
pins this exact Moonlight fork and its 900-byte default MTU, standard ENet wire
layout, CONNECT data, channels and flags. It is preferred over an invented
transport and over original IPv4-only ENet for the host's IPv6 requirement.
Source support for IPv6 does **not** establish target IPv6 interoperability;
that remains unverified. Full official-client/session conformance remains a later
production-host obligation. The smoke's application bytes are deliberate fixtures.

## Reproducible local build

Environment: macOS arm64; SDK installed at
`/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk`;
`LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config` reports 18.1.8.
The manifest's installed-LLVM archive-provenance gap is preserved.
All scratch/build/archive extraction is under `/Volumes/4TB_Drive/tmp/t043-t044`.
Archives are untrusted data: verify SHA-256 first and copy only regular files and
directories with Python tarfile's data filter, never execute archive startup hooks.
No global software/configuration installation occurred.

The locked OpenSSL archive SHA-256 is
`603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`.
The selected ENet codeload archive SHA-256 is
`6bb1a151e6d21e1756baeff5a95eaf85a5b6731aeda6d0bc255651192ba4a32d`.
The latter identifies actual downloaded bytes; no independent publisher digest or
signature was supplied. Its full commit was resolved through GitHub's commits API.

Apply the exact embedded `patches[].unified_diff` from the dependency lock to each
extracted source before building. OpenSSL's patch supplies missing metadata-generator
arguments in `build.info` / `exporters/build.info`; no cryptography or threading code
changes. `TERM=xterm-256color` supplies real terminal capabilities for Apple's Perl
5.34 `re debugcolor`; this avoids its undefined color-capability warnings without
disabling warnings. The initial devrandom-only build failed on an unused upstream
`sysctl_random`; use OS seeding, retaining the sourced FreeBSD KERN_ARND path and
the upstream OS-device fallback. No entropy data or test seed is injected.

With `CC`, `AR`, `RANLIB` pointing to the pinned SDK `bin/prospero-*` wrappers:

```sh
export TMPDIR=/Volumes/4TB_Drive/tmp
export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config
export TERM=xterm-256color
perl Configure BSD-x86_64 no-shared no-dso no-module no-tests no-asm \
  no-secure-memory no-devcryptoeng --with-rand-seed=os --libdir=lib \
  --prefix=/Volumes/4TB_Drive/tmp/t043-t044/openssl-install -Werror
make -j8 build_libs
```

ENet's upstream CMake adds `-Wno-error`; the initial CMake build exposed six unused
parameters. The locked patch adds `(void)` uses for those parameters, preserving
behavior. Build the eight Unix source units directly with
`prospero-clang -O2 -g -Wall -Wextra -Werror -I<enet>/include` and
`-DHAS_SOCKLEN_T=1 -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1
-DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1`.
Units: `callbacks compress host list packet peer protocol unix`; archive with
`prospero-ar rcs libenet.a <objects>`.
The selected flags exercise the SDK-declared functions; do not infer their presence
from CMake cross-compilation's static try-link results.

Build both probe variants with `prospero-clang -std=c11 -O2 -g -Wall -Wextra
-Wpedantic -Werror tools/ps5-probes/dependencies.c`:

- TLS: `-I<openssl>/include <openssl>/libssl.a <openssl>/libcrypto.a -pthread -o dependencies.elf`.
- ENet: `-DDEPENDENCIES_ENET -I<enet>/include <enet>/libenet.a -o dependencies-enet.elf`.

The full local commands are in `build-probes.py`, `final-probes-build.log`,
`openssl-strict-build.log`, and `enet-ps5-strict.log` in the evidence directory.
The host smoke uses a separate OpenSSL build from the same archive with
`darwin64-arm64-cc`, LLVM 18.1.8 and the same static/no-module approach.
It is supplementary software evidence, never PS5 execution evidence.

## Local observations

| Verification | Result |
|---|---|
| Final OpenSSL clean cross-build | Exit 0; no compiler/Perl warnings after supplying metadata arguments |
| ENet direct strict build, host and target | Exit 0; no warnings, no upstream `-Wno-error` |
| Four final TLS/ENet probe builds | Exit 0 with `-Wall -Wextra -Wpedantic -Werror` |
| `dependencies-host` | Exit 0: RAND poll/status, distinct 32-byte outputs; RSA-2048/SHA-256 signature and PEM round-trip; untrusted self-signed certificate rejected; trusted mutual TLS 1.3 / AES-256-GCM over TCP, ping/pong, concurrent thread entropy and joined cleanup |
| `cargo +1.93.0 build --offline --locked --manifest-path /Volumes/4TB_Drive/tmp/t043-t044/rust-counterpart/Cargo.toml` | Exit 0; `rusty_enet` 0.4.0 |
| `python3 /Volumes/4TB_Drive/tmp/t043-t044/wire-run.py host` | Exit 0; actual UDP relay retained 19 datagrams / 9,150 bytes in the recorded run |

Wire observations: CONNECT data `0x12345678`, 48 channels; 32-byte reliable messages
on channels 0, 1, 18 and 47; 32-byte unsequenced message on channel 0;
4,096-byte reliable fragmented message on channel 18; exact reassembled payloads
echoed reliably on host channel 0; both sides observe graceful disconnect.
`host-wire-datagrams.json` retains directional raw UDP hex without pcap/root access;
`enet-host-wire-relay.log` / `rust-host-wire-relay.log` retain endpoint observations.
This validates transport framing/behavior, not controller delivery or media streaming.
RAND poll/status and nonidentical outputs prove functioning API paths, not a
statistical estimate or independent measurement of physical entropy strength.

## Target transaction and observed results

The coordinator explicitly granted the exclusive console window at 17:31:47 UTC
for the exact source/payload hashes below. It was released after postflight checks.
Console firmware is the owner's 13.60 baseline, Mac en9 192.168.2.1, console
192.168.2.2, dead gateway 192.168.2.254. Before probes: fresh Mac interface/reachability
and needed service checks, read-only console getifaddrs plus complete IPv4/IPv6
route inventory, checked SDK route-message decoding, and current process listing.
No public outbound packet is used to test isolation.

Execute only these bounded ELFs through pinned `prospero-deploy -h 192.168.2.2
-p 9021 <ELF>`; the TCP sender's success alone is not payload success.
Capture target stdout and the probe's explicit result, then repeat inventory and
process inspection. Stop and ask on console unresponsiveness; no retry/reboot.
No capture device, game, settings, firmware, shell service or hooks are changed.
The stock CRT's `crt/crt.c -> crt/patch.c` per-process credential/syscall-bound
writes are the owner-approved existing baseline; reversal is unverified and no
claim of zero privileged changes is made. Probe code adds none.

Fresh preflight and postflight inventories independently show console interfaces
192.168.2.2, its link-local IPv6 and loopback only; exactly one IPv4 default via
192.168.2.254 and no IPv6 default/global address. Mac en9 has 192.168.2.1/24;
ARP reports no dead-gateway entry. Mac IPv4 and IPv6 forwarding flags are both 1;
they were retained, and PF/NAT was not read with sudo. Isolation rests on the
owner-attested sole direct link plus the observed dead-gateway/interface/route
state, not an invented claim that forwarding is disabled. Console `ps` banner
reports 13.600.007 and the read-only app query reports no running BigApp (-1).
No current hypervisor attestation beyond the existing owner-approved environment
is claimed. Full route dumps, checks and raw stdout remain under `preflight/` and
`postflight/` in the evidence directory; raw shell banners are private (serial omitted here).

| Target operation | Actual observations |
|---|---|
| `timeout 25 <sdk>/bin/prospero-deploy -h 192.168.2.2 -p 9021 <root>/dependencies.elf` | Sender exit 0 and explicit probe result 0; OpenSSL 3.5.9, OS RAND poll/status and distinct outputs, RSA-2048/SHA-256, certificate PEM round-trip/signature verification, untrusted self-signed rejection, trusted mutual TLS 1.3 / TLS_AES_256_GCM_SHA384, TCP ping/pong, entropy calls in both threads, successful join and resource release |
| Same sender with `dependencies-enet.elf`, then `python3 <root>/wire-run.py ps5` | Sender and counterpart exit 0; target result 0; all six payloads exact and graceful disconnect observed; 18 real UDP datagrams / 9,148 bytes retained in `ps5-wire-datagrams.json` |
| Same sender with `budget-idle.elf` | Sleep-only diagnostic completed; findings and the CPU-clock semantic concern are in [R5](research-r5-budgets.md); this does not prove a host budget |
| Same sender with `network-inventory.elf` before/after | Both complete inventories decoded successfully; same isolation and no running BigApp |
| `python3 <root>/console-readonly.py ps --out <root>/postflight/ps.log` and service status | Only transient shell/ps helper PIDs differ; no probe remains; existing websrv/gdbsrv/klogsrv/shsrv remain listening |

Here `<root>` is `/Volumes/4TB_Drive/tmp/t043-t044`; exact argument lists are retained.
There was no console-file upload, global installation, hook, media acquisition,
controller input, foreground game, capture-device operation, settings change,
jailbreak recovery, firmware operation or retry after a failure.
User-visible normal-screen continuity was not independently captured in this run.
The sources' owned sockets/SSL contexts/certificates/keys/ENet hosts were freed,
and process absence plus service continuity were checked independently.

Approved and executed SHA-256 values:

| Artifact | SHA-256 |
|---|---|
| `dependencies.c` | `07935809da54075f97f1e2824b9ac69ec979bcf59273f33b167d8327ee8dc274` |
| `budget.c` | `6d53b0dd85911aa30373568570e7b79659bede7683c735a42624b2d385cdad77` |
| `dependencies.elf` | `471d0f2d7ed115d4240f5e196a9b4e51201f2fdc60cf70e7c3c90cb73811f0b9` |
| `dependencies-enet.elf` | `e411e7f989647fd5bf16fb8dd3c56e57e246d49a940b583d10388c24ca5af2b3` |
| `budget-idle.elf` | `1496cf0679ad68703fbe64315e06c47203b0a7a1f730fb5f1795a50ba3f7c591` |
| `network-inventory.elf` (existing R4 source) | `30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248` |

T043's target dependency smoke is complete. IPv6 target transport, complete TLS
security/conformance testing, production allocation cost and release reproducibility
remain later validation work; none is inferred from this bounded dependency test.


## Final verification and retained evidence integrity

`clang-format --dry-run --Werror tools/ps5-probes/dependencies.c tools/ps5-probes/budget.c`
passed. JSON parsing, every embedded dependency-patch digest, and scanning the final
OpenSSL host/target, ENet host/target and probe build logs for compiler/Perl warnings
and errors all passed. The host TLS smoke and raw-UDP exchange were rerun after
final local rebuilds and passed. The final PS5 binaries retain their approved,
executed hashes. Parsing the first recorded client datagram using the pinned
protocol header independently confirms CONNECT command 2, channel count 48 and
big-endian data 0x12345678; this is not inferred only from the application logs.

The SDK's installed `target/include` declarations come from the pinned source's
[include/freebsd](https://github.com/ps5-payload-dev/sdk/tree/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/include/freebsd)
(`time.h`, `pthread.h`, `sys/socket.h`, `sys/time.h`, `netinet/in.h`, `unistd.h`),
installed by `include/Makefile`. Symbols are imported by `sce_stubs/libkernel_web.c`
and the SDK libc setup; no proprietary header or guessed ABI is used.

The local `evidence-manifest.json` lists raw evidence sizes/digests. Primary device
observations have these SHA-256 values so the report can identify their exact bytes:

| Evidence | SHA-256 |
|---|---|
| `dependencies-device.log` | `1aff16672cad055d204a3df5ddce65f437b9c3aeea350ac14c5be8678e6e365a` |
| `enet-device.log` | `e67ad3d1c53e9dc1bb950334cd431aa1856e751dba2d482b3c40fe8c9869aeb0` |
| `rust-ps5-wire-relay.log` | `eb9adbd4d246feed947c427387ab9de1934e97e56e49cf7eceb2fb8ad0f3f7a2` |
| `ps5-wire-datagrams.json` | `f9ec583a2f024d9b18a8e980efa5b4edfce112792debfb6fd5f4f040385ee319` |
| `budget-device.log` | `d486091e15cb24cf64ee50b752c397e831c13c0fbc1277bf87c5bd9e4b5fb070` |

## T069 portable ENet timeout guard (2026-10-25; not device-validated)

Real independent-client silent-loss E2E under native AppleClang ASan/UBSan
exposed `protocol.c:1378:21`: **shift exponent 32 is too large for 32-bit
unsigned int**. The observed stack was `enet_protocol_check_timeouts` →
`enet_protocol_send_outgoing_commands` → `enet_host_service` →
`sp5_control_transport_step` → fake host `input_owner`. A deterministic unit
regression reproduces it at retry attempt 33 using real ENet service calls,
a silent loopback UDP sink and ENet's public test clock; no console is involved.

The coordinator approved a checksum-pinned fix during dispatch
`ctx_c73991224da0`, requiring an upstream search first. The latest inspected
protocol-file commits in [cgutman/enet 78cc9b4](https://github.com/cgutman/enet/blob/78cc9b4b03bdd4f95c277391cac6e0951407072b/protocol.c)
and [lsalzman/enet 8be2368](https://github.com/lsalzman/enet/blob/8be2368a8001f28db44e81d5939de5e613025023/protocol.c)
still contain the unguarded timeout shift. Inspection of 100/97 protocol-file
commit entries and both repositories' `shift timeout` issue searches found no
existing fix for this timeout predicate. Upstream's separate signed fragment
shift fix does not address this expression. These are the search's scope and
result, not an assertion that every upstream branch or issue was examined.

The source commit remains **aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1**.
The second lock-manifest patch short-circuits attempts greater than 32 before
shifting: mathematical `2^(attempts-1)` must then exceed every uint32 threshold.
Attempts 1–32 retain exactly their defined comparison. Zero represents no sent
attempt and falls through to the absolute timeout without a negative shift.
Minimum/maximum timeout checks, retry timing and wire messages are unchanged.
CMake verifies every embedded patch's SHA-256 and applies both to the pinned
archive. The unit covers attempts 0, 1, 32, 33 and 65535 and the absolute deadline;
the full independent-client loss scenario remains enabled with sanitizer
halt-on-error. Verification results and original traces are retained in
`/Volumes/4TB_Drive/tmp/task_9736bc4448cc/report.md`.

**Evidence limit:** the T043 firmware-13.60 ENet smoke used the earlier build
without this guard. This change has no on-device validation and establishes no
new console capability, timing result or production readiness. Portable
sanitizer coverage is reported separately from that historical device smoke.
