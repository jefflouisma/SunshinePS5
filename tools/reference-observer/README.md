# Reference evdev observer

This nonshipping Python tool acquires read-only Wolf controller observations on
64-bit x86/ARM Linux. It inventories matching devices before READY, opens devices
read-only/nonblocking, records descriptor identity and initial ABS_X/Y state,
parses partial input events, records removals, and closes every acquired descriptor.
The supported axes must report the signed 16-bit range. It changes no controller,
kernel, container, or host configuration.

The parser accepts explicit 32/64-bit word sizes and little/big endian fixtures.
Live collection is limited to the 64-bit ABI above. ABI definitions are sourced
from Linux v6.12 [input.h](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/input.h)
and [asm-generic/ioctl.h](https://github.com/torvalds/linux/blob/v6.12/include/uapi/asm-generic/ioctl.h).

## Run protocol

Provide a binding JSON containing `session_id`, `scenario_id`, `host_version`,
`test_client_version`, `test_client_revision`, `official_client_version`, and
`runtime_identity`. Obtain the runtime identity independently from the exact pod
UID/container image digest; the tool retains this supplied identity but cannot
authenticate it. The scenario is the same JSON used by the runner. This slice
supports one controller, ID 0, with connect, left-stick, neutral, and disconnect
candidate matching. Other actions remain unmatched.

```sh
python3 tools/reference-observer/observer.py \
  --binding /path/to/binding.json --scenario /path/to/scenario.json \
  --output /path/to/new-run-directory --duration 30
```

Wait for the stdout JSON `READY` message with the expected session ID before
dispatching actions. READY includes the initial inventory; a nonempty inventory
invalidates diagnostic correlation as late attachment. Collection is finite
(at most 300 seconds, four devices, 4096 records, 1 MiB raw artifact). Keep stdout
drained. `DONE` means artifacts were published, not that acceptance passed.
Unexpected errors are retained where quota permits; quota exhaustion refuses
publication. SIGINT raises KeyboardInterrupt and runs the collector's `finally`
descriptor cleanup. SIGTERM uses Python's default termination behavior: the OS
closes descriptors when the process exits, without executing that cleanup block.
Neither interruption produces DONE.

The new output directory contains `raw.json`, `host.jsonl`, and `manifest.json`.
Each file is fsynced and renamed atomically; the lifecycle manifest is committed
last and binds both `raw.json` and `host.jsonl` bytes by SHA-256. Existing output
directories are refused.
Configure both runner bindings: `--host-observations /path/to/new-run-directory/host.jsonl`
and `--reference-lifecycle /path/to/new-run-directory/manifest.json`. After host
cancellation, the runner waits for the final manifest within the shared ten-second
observer publication window. The controller path must identify an artifact beneath
that manifest's directory; the runner parses its checksum-verified snapshot bytes
and retains the manifest/sources before evaluating controller evidence. A missing,
invalid, or unbound artifact stays Incomplete without falling back to a separate
file read. Source replacement after validation cannot change the imported records.
This tool still needs orchestration to bind the run, await READY before actions,
and finish collection/publication within that window. Failed cancellation makes
one immediate import attempt. Standalone JSONL without a lifecycle binding retains
legacy immediate import behavior and has no publication wait or checksum binding.

For the start boundary, the runner's optional `--observer-ready FILE` publishes an
immutable `observer-binding.json` before host launch and waits up to ten seconds
for a matching readiness record. Once that binding exists, start this collector
with these paired opt-in arguments in addition to the ordinary arguments:

```sh
  --runner-binding /path/to/runner-output/observer-binding.json \
  --ready-output /path/to/ready.json
```

The collector reads the binding once as a regular, unaliased, no-follow file
(at most 8 KiB), rejects unknown/duplicate fields, validates the schema, UUID-v4
nonce and digest format, and cross-checks session/scenario IDs and host/test-client
versions against its own supplied binding. It echoes the runner's scenario digest
as an opaque coordination token; it does not reproduce Rust's scenario serialization
or prove scenario-content equality. Keep the runner and collector scenario inputs
identical. The diagnostic digest in `raw.json` uses Python serialization and is a
separate value.

In this mode the initial device inventory must be empty. After inventory and
before acquisition, the collector publishes the exact runner binding inside
`{"schema_version":"1.0.0","state":"ready","binding":...}`. It writes and
fsyncs and closes a private file, then publishes it by an atomic no-replace rename
(`renameat2` with `RENAME_NOREPLACE` on Linux; `renameatx_np` with `RENAME_EXCL` on
macOS for portable verification). Unsupported systems/filesystems fail before
publication; there is no overwrite or hard-link fallback. The visible inode has
one link and no subsequent publisher metadata changes. Before visibility, failure
aborts collection, closes publication descriptors, and emits no stdout READY.
Existing destinations (including dangling symlinks), special
files, symlink parents, traversal and binding hard-link aliases are refused.
Visibility is irrevocable success because the runner may already retain the envelope.
The collector then attempts directory fsync and reports its result in stdout READY's
`ready_directory_synced` boolean. Directory-sync/close failure does not roll back the
envelope or abort acquisition; a broken stdout reader likewise cannot revoke file
readiness. If directory fsync fails, the directory entry's survival across a crash
is unconfirmed. This transient launch handshake does not require crash durability;
file-content fsync is completed before publication. Use a trusted local directory
and a fresh path per run.
`--fixture` cannot be combined with these arguments. Omitting both preserves the
ordinary stdout-only protocol and its conservative late-attachment diagnostics.

See
[the runner handshake](../moonlight-mcp/docs/usage.md) for the exact JSON envelope,
size/path restrictions and quota behavior. The collector does not attest the
supplied runtime binding. A coordinator must still arrange binding transfer,
collector startup within the runner's ten-second deadline, and final acquisition
publication after cancellation. The handshake provides a launch barrier; live
ownership and acquisition acceptance remain separate.

### Explicit finish channel

With the paired readiness options, `--finish-fd N` accepts an inherited pipe read
descriptor above 2. The descriptor must already be read-only and nonblocking;
regular files, sockets, write ends and stdio are refused. Ownership transfers to
`collect()` and closing is attempted exactly once on success or failure. The collector does
not reopen any control path or change shared descriptor flags. The coordinator
must close its unused pipe ends and keep the writer open until it sends finish.
Fixture replay cannot use this option.

After observing READY and cancelling the host, write exactly one UTF-8 JSON line
and close every copy of the pipe's write end:

```json
{"schema_version":"1.0.0","command":"finish","binding":{"...":"entire exact observer-binding.json object"}}
```

The binding includes the run/session/scenario IDs, scenario digest, versions and
nonce. Every field must match the retained runner binding; unknown/duplicate JSON
fields, a foreign or stale binding, multiple lines and trailing data are errors.
Bytes are decoded explicitly as strict UTF-8 before JSON parsing. UTF-16/UTF-32
encodings and all BOM-prefixed variants, including UTF-8 BOM, are rejected.
The whole message is bounded to 16 KiB. EOF commits the command, so a delayed
second command cannot be mistaken for a valid single finish. Fragmented delivery
must reach EOF within one second after its first observed byte. A writer left
open after sending a complete line produces a timeout, not completion. Empty EOF
is control loss. Buffered input or EOF detected by the two pre-READY checks
prevents READY publication. The pipe is a trusted coordinator channel, not an
authenticated proof of host cancellation; no timestamp can establish when an
untrusted sender wrote between the last preflight check and READY visibility.

Once a complete valid command arrives, the collector takes one final inventory,
records real removals/attachments, reads at most 64 events per present device,
checks for still-readable input and finishes every decoder. Partial records or
remaining queued input produce acquisition errors. The final snapshot is bounded;
it does not establish that no events arrive after the snapshot. An `end` record
with `termination_reason:"finish_command"` retains the exact finish binding and
the identities of devices still present. Those devices are never synthesized as
removed. Closing every acquired descriptor is attempted before manifest publication.
An uncertain close is never retried. A finish-pipe close failure appends
`control_close_error` while preserving the collected records and manifest when
quota permits; DONE then reports `finish_status:incomplete`. If collection failed
before returning records, its primary exception is preserved even when closing
the pipe also fails. Control, acquisition and close errors remain explicit where quota permits;
quota exhaustion refuses publication. The existing duration limit remains a
watchdog and records `termination_reason:"duration_limit"` without claiming that
a finish command arrived. In controlled mode DONE includes `finish_status`:
`finished` only for a valid finish and successful cleanup, otherwise `incomplete`.
DONE itself continues to mean artifact publication. Ordinary collection without
`--finish-fd` is unchanged.
Failures in the combined device/control wait use `poll_error`, because that wait
cannot identify which descriptor caused the failure.

The inherited-descriptor CLI test uses a 300-second duration, sends finish after
READY, and requires DONE plus a published manifest within two seconds in its
synthetic local run. This is not a hard real-time guarantee for arbitrary Linux
drivers, filesystems or blocked stdout. Keep stdout drained and budget publication
inside the runner's ten-second window. Runner integration, process supervision,
authenticated ownership and live acceptance remain unresolved.

## Evidence limits

### Local process supervisor

`supervisor.py` owns a collector process group and its inherited finish pipe on
one trusted machine. It validates the runner binding, READY and immutable
`observer-cancel.json`, then sends finish plus EOF only for the exact binding and
literal `request_succeeded: true`. False closes the pipe without finish. Missing
signals expire at the original absolute deadline; malformed signals are terminal.
The deadline covers binding wait, startup, acquisition and publication and is
never renewed by progress. Forced process reaping has a separate two-second
allowance. Stdout/stderr together are capped at 1 MiB and drained concurrently.
Owned descriptors are detached before one close attempt; an uncertain close is
never retried. Cleanup failures are retained in `cleanup_errors` and cannot skip
the subsequent live-child group-signal check, bounded child reap, stream/log
closure or final report attempt. Known-exited leaders are never group-signalled;
`descendant_cleanup: "unconfirmed"` records that direct-child reaping does not
establish descendant termination.
Failure to write the final report is returned to the caller when the filesystem
cannot retain it. Cleanup uncertainty keeps coordination incomplete.

```sh
python3 tools/reference-observer/supervisor.py \
  --runner-binding /trusted/runner/observer-binding.json \
  --binding /trusted/collector-binding.json \
  --scenario /trusted/scenario.json \
  --ready-output /trusted/ready.json \
  --output /trusted/new-supervisor-run --timeout 60
```

Parents must already exist and be canonical without symlinks. Output, READY and
cancellation destinations must initially be absent. Start this supervisor before
the runner publishes its binding, or while that runner is waiting for READY.
The runner must select `--observer-finish-signal` and the same READY path, with
`--host-observations .../new-supervisor-run/collector/host.jsonl` and
`--reference-lifecycle .../new-supervisor-run/collector/manifest.json`.
The supervisor retains input snapshots, process arguments/PID, stdout/stderr,
runner signals, collector artifacts and `supervisor.json`. It checks manifest
binding and the two collector source digests before reporting `coordination:
"published"`. This is publication status only; acceptance remains Incomplete
and the CLI exits 2 even on successful coordination. SIGINT executes owned-group
cleanup; SIGKILL or machine loss cannot guarantee cleanup or a final report.

The deployed Fenrir cancellation endpoint acknowledges submitted Session delete
requests. A true signal does not establish pod disappearance or resource cleanup.
False is never upgraded when pods subsequently disappear. Publication after a
failed runner cancellation may miss its immediate import and is diagnostic only.

The legacy collector binding requires the independently supplied runtime identity
before startup. The opt-in attachment mode below separates the persistent observer
identity from the future pod. A remote adapter must acquire the attachment through
verified transport, cover initial events and reconnection, and retain exact deployed
source versions. It must keep the owner
outside the ephemeral session pod, relay bounded hash-verified snapshots with
manifest last, map the runner's deadline conservatively across hosts, and close
control on transport loss. This implementation supplies no SSH, Kubernetes or
device-provenance adapter and cannot resolve those acceptance prerequisites.

### Postlaunch runtime attachment

On Linux, give the supervisor both `--runtime-attachment /trusted/runtime.json`
and `--attachment-ready /trusted/attached.json`. Both paths must be fresh. Configure
the runner with the same `--observer-attachment /trusted/attached.json`, in addition
to READY and finish-signal options. All paths and processes share one trusted local
filesystem and host monotonic clock. The supervisor's single absolute deadline
continues through attachment, acquisition and publication.

Before collector startup, the supervisor atomically publishes `observer-identity.json`:
schema `1.0.0`, exact `runner_binding`, UUID-v4 `observer_id`, Linux `host_boot_id`,
owner `pid` and `process_start_ticks`, `collector_sha256`, `supervisor_sha256`,
`clock_domain:host_monotonic`, `created_ns` and `authenticated:false`. It sets the
collector's `runtime_identity` to `observer:<UUID>`; this identifies the persistent
owner, not a future pod. The READY stdout record says runtime inventory is deferred.
No host-wide device-name scan runs in this mode.

The runner publishes `observer-launch.json` only after successful launch. Its strict
envelope contains schema `1.0.0`, `state:launched`, the full `binding`, and a fresh
UUID-v4 `launch_nonce`. A trusted local adapter waits for that marker, then atomically
publishes one complete runtime attachment. An attachment arriving before the marker,
or carrying a different nonce, is refused. The runtime object has exactly these fields:

| Field | Required content |
| --- | --- |
| `schema_version`, `state` | `1.0.0`, `attached` |
| `runner_binding`, `launch_nonce`, `observer_id` | Exact retained coordination identities |
| `observed_ns` | Host monotonic observation between identity creation and validation |
| `association`, `authenticated` | `exclusive_launch_candidate`, `false` |
| `source_revision` | `5c8251c8a7a547466226c5f08f72d94a5de05dd0` |
| `cluster_identity` | Bounded identifier supplied by the verified acquisition adapter |
| `session` | `namespace`, `name`, `uid`, `resource_version`, `pairing_name`, `user_name`, `app_name` |
| `deployment`, `replicaset` | `namespace`, `name`, `uid`, `resource_version`, `owner_references` |
| `pod` | The same fields plus `node_name`, `node_uid` |
| `containers` | 3–8 unique named objects with `name`, `container_id`, actual digest-bearing `image_id`, `restart_count`, `started_at`; includes `wolf-agent`, `wolf`, and the app |
| `wolf_session_id` | Bounded identifier |
| `devices` | 0–4 objects with `session_id`, exact `/dev/input/eventN` `path`, integer `major`, `minor`, `generation` |
| `sources` | 1–8 unique basename-only `{path,sha256}` references to redacted UTF-8 observations, each at most 64 KiB |

Each owner-reference list contains exactly one `{kind,name,uid,controller:true}`
object matching the preceding Session → Deployment → ReplicaSet → pod edge; all
namespaces agree. This deliberately narrow form rejects ambiguous/multiple candidates.
Container/device fields and source bytes remain supplied observations, not attestations.
The provider must remove AES keys, IVs, certificates and private pairing material before
publication; the collector does not perform semantic redaction of arbitrary source text.

The runtime JSON is bounded to 64 KiB, strict UTF-8, with no BOM, duplicate/unknown
fields, path aliases, special files, hard links or mutation during acquisition. Source
digests are checked before attachment. The first attachment's exact bytes and inode
identity remain fixed; a changed UID, restart, device generation or any replacement is
terminal. Exact character-device major/minor and descriptor identities are checked;
device disappearance is a terminal acquisition error, never a synthesized Unplug.
There is no device-name fallback or reconnect-and-merge behavior in this first slice.

The collector publishes the runner's attached envelope after validating the runtime
association and opening any already declared devices, including the exact launch nonce
and SHA-256 of the runtime bytes. An empty inventory is valid: Wolf may create pads only
after RTSP/controller arrival, so requiring a pad before allowing RTSP would deadlock.
For an empty inventory raw evidence records `device_acquisition:
unavailable_pending_later_device_feed`. The optional feed described below can acquire
later pads; connect/early-event coverage stays unavailable. The envelope
still says `association:exclusive_launch_candidate` and
`authenticated:false`; the runner retains the digest as an opaque coordination token.

`raw.json` retains observer identity, launch marker, exact runtime bytes, parsed object,
redacted source bytes/hashes, initial descriptor identity, and bounded device snapshots.
Every snapshot has `continuity:unverified`. The deployed `/input-devices` API retains
session ID and device generation, but provides no subscription epoch/history; successful
polls cannot exclude a missed reconnect. The existing manifest still binds only
`raw.json` and `host.jsonl`, and all acceptance criteria remain unavailable. The supervisor
cross-checks retained attachment identities, source hashes and the attached envelope.
No Kubernetes/SSH acquisition adapter or authenticated event ownership is supplied here.

### One-shot late device feed

Add `--device-feed /trusted/devices.json` to the supervisor (or collector in runtime
attachment mode). The path must be absent before startup and immediately before runtime
acknowledgement. This mode requires an empty initial runtime device inventory. A trusted
local provider waits for the attached envelope, then publishes a single immutable JSON
object, using atomic no-replace publication in a trusted canonical directory:

- `schema_version: "1.0.0"`, `state: "devices_attached"`;
- exact `runner_binding`, `launch_nonce`, `observer_id`, and `runtime_attachment_sha256`;
- `observed_ns` on the shared host monotonic clock, after acknowledgement;
- `continuity: "unverified"`;
- `devices`: one to four exact `{session_id,path,major,minor,generation}` objects,
  matching the runtime Wolf session and `/dev/input/eventN` paths;
- `sources`: one to eight redacted UTF-8 source `{path,sha256}` records, using the
  same basename, size and hash restrictions as runtime sources.

The 64 KiB strict JSON reader rejects duplicate/unknown fields, BOM/other encodings,
links, aliases, mutation, foreign bindings and reused path/generation values. After
first acquisition the exact feed bytes and inode remain fixed. The collector opens
only those character devices and verifies major/minor and descriptor incarnation.
Disappearance, replacement, malformed feed or control loss terminates correlation;
there is no rescan, reconnect, feed update or inferred release.

`raw.json` retains the exact feed bytes, sources, acknowledgement clock and a
`late_device_attachment` followed by `late_attach` before any later input records.
That diagnostic prevents candidate matching of missed connect/early events. Later
real evdev records are retained, while all controller/lifecycle acceptance remains
unavailable. The supervisor checks the retained feed, source hashes and diagnostic.
The existing absolute watchdog and finish pipe remain in control: a missing feed
does not prevent finishing with an unavailable device population. This interface
does not acquire agent snapshots or establish subscription continuity; a live provider
and authenticated transport remain separate work.

### Read-only Fenrir provider

`fenrir_provider.py` supplies the local runtime and later-device publications through
explicit `kubectl` reads. It requires `--enable-provider`, an absolute executable,
explicit kubeconfig/context/namespace, user/app/pairing selection and a reviewed
agent HTTPS port. Importing the module does nothing. Run it on the same Linux host
as the supervisor; it checks the observer boot ID and uses the shared monotonic clock.

Start the provider first with a fresh `--output /trusted/provider` directory. It
creates that directory and waits for `--runner-binding` and `--observer-identity`.
Start the supervisor with its existing runtime options plus
`--provider-ready /trusted/provider/provider-ready.json`,
`--runtime-attachment /trusted/provider/runtime.json` and
`--device-feed /trusted/provider/devices.json`. Give the provider the supervisor's
identity path, the runner's `--launch-marker` path and the shared
`--attachment-ready` path. The supervisor publishes its identity, then waits for
the provider baseline barrier before spawning the collector. The resulting order
is baseline → collector READY → runner launch marker → runtime attachment → attached
acknowledgement → optional devices. A missing/invalid provider barrier prevents
collector startup. Each process has one absolute watchdog; the runner's shorter
READY/attachment deadlines still apply. The provider is an independently owned
process; the supervisor never kills a process it did not spawn.

The provider checks the selected Kubernetes server uses HTTPS with certificate
verification and freezes its kubeconfig bytes/inode. Kubeconfig credentials,
credential plugins, CA files and executable installation are trusted operator
configuration. It records prelaunch Session UIDs, requires one new matching
user/game/pairing Session, validates owner UIDs through Deployment/ReplicaSet/pod,
and retains named container IDs, image digests, restarts and node UID. Only allowlisted
summaries are retained; raw API stdout/stderr may contain AES keys, IVs or credentials
and are discarded. The pinned Session schema is
[Fenrir 5c8251c](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/pkg/api/v1alpha1/session.go).
The source revision selects that schema; image digests do not independently prove
which source produced a running binary.

After attached acknowledgement, the provider reads the selected pod's
`/input-devices` through the authenticated Kubernetes pod proxy. `--agent-port`
must match a declared TCP port on the exact selected pod's `wolf-agent` container:
a canonical numeric port (1–65535), or a unique port name resolved to its numeric
`containerPort`. Missing/ambiguous ports or agent status fail closed. Retained source
summaries include the container name, resolved port and HTTPS scheme. The raw path is
`/api/v1/namespaces/<namespace>/pods/https:<pod>:<numeric-port>/proxy/input-devices`,
using Kubernetes' explicit scheme syntax. This matches the pinned
[HTTPS-only wolf-agent](https://github.com/jefflouisma/homelab/blob/5c8251c8a7a547466226c5f08f72d94a5de05dd0/fenrir/cmd/wolf-agent/main.go).
It rechecks the exact pod UID and runtime/container identities before and after
that request. Unsupported proxy routing, RBAC denial, an unavailable port, foreign
devices, ambiguity, replacement or timeout fails closed. The API server authenticates
the Kubernetes request; this does not attest the agent's device assertions or give
end-to-end authenticated input events. No direct agent TLS bypass or heuristic
client selection is used. The client authenticates the API server; the API server's
handling of the agent's self-signed backend certificate and the agent process identity
remain live verification limitations. No native deployment/proxy availability is
claimed by tests, and there is no direct-agent fallback.

Commands use literal argv, a five-second per-command bound inside the absolute
watchdog, at most 256 invocations, 64 KiB per stdout/stderr stream, and owned process
groups. Cleanup signals the group only while the direct child is still unreaped;
known-exited leaders never trigger a group signal. Descendants surviving a known
leader exit remain unconfirmed; this is not a full process-supervision guarantee.
Publications use the existing synced atomic no-replace primitive, with a
stricter 16 KiB per-file provider bound and at most 16 files. Raw sources, runtime,
feed and baseline become immutable when visible; later failure retains them as
partial Incomplete evidence. A bounded provider report binds redacted command
records and artifacts. Runner cancellation stops acquisition; publication never
upgrades cancellation or acceptance. Filesystem calls can still block in the kernel.

`test_supervisor.py` uses `supervisor_fixture.py` to run the real collector with
empty synthetic inventory and explicitly synthetic provenance. Tests cover true
finish, false control loss, strict signal parsing, path aliases, foreign READY,
output quota, timeout and descendants holding log pipes. Set
`SUPERVISOR_EVIDENCE` to a fresh existing evidence directory to retain both
successful and failed-cancellation populations. The fixture is never selectable
through the supervisor CLI.

Evdev carries neither authenticated session ownership nor the runner event ID.
Candidate matching is diagnostic: it assumes the current Wolf left-stick scale
and waits for SYN_REPORT; calibration and independent ownership are still needed.
SYN_DROPPED, ambiguous/reused devices, late attachment, foreign events and reordered
lifecycle invalidate correlation. Missing actions are never synthesized.

Every published HostObservation is `unavailable`, `authenticated:false`, with no
claimed timestamp or console-delivery boundary. All six lifecycle checks are
explicitly unavailable: device removal alone cannot establish session termination,
complete controller release or resource reclamation, and this collector observes
no pairing proof. Runtime version fields are supplied bindings, not attestations.
T038, HDR, fidelity/calibration and official-client acceptance remain open.

`--fixture records.json` replays a JSON array of raw records without Linux access;
all outputs are forcibly labeled `synthetic_fixture`. It cannot create live
acceptance evidence.

## Verification

### Cross-host relay (explicit opt-in)

`deployment_entrypoint.py` is a local server installation gate. Install it and
the four relay modules in an administrator-controlled directory, and configure
the dedicated account's SSH subsystem to invoke the literal Python executable
with `-I -S`, the absolute entrypoint path, `--serve --config /absolute/config.json`.
The entrypoint supports a direct `Subsystem sunshine-reference-observer-v1`
configuration only; any `SSH_ORIGINAL_COMMAND` (including an empty value) is
refused. ForceCommand installations need a separately reviewed arrangement.
The administrator must independently restrict the account/key against shell,
other subsystems and forwarding. This program does not install that boundary.

The strict administrator-owned JSON config has exactly `schema_version: 1`,
`account`, integer `uid`, `module_directory`, `hashes`, `python`, `remote_root`,
and `provider`. `hashes` contains the exact SHA-256 pins for `remote_relay.py`,
`supervisor.py`, `observer.py`, and `fenrir_provider.py`. `provider` has exactly
the eight fields documented below. The request's provider and root must equal
this server configuration before an owner can start. A fresh preflight also
rejects configuration changes between connection and owner construction.

Paths must be absolute, canonical and symlink-free. The config, modules,
module directory and executables must belong to root and deny group/other
writes; ancestor directories must deny group/other writes too. The account
must match the real/effective non-root UID and both passwd lookups. The session
root must be account-owned mode 0700 and kubeconfig account-owned mode 0600.
Files must be regular, singly linked and within quotas.
The module directory permits only the four pinned source files and the optional
entrypoint source; caches, bytecode, packages and other importable siblings are
refused. Each imported module must report the exact pinned `.py` path. Descriptor
ownership is detached before close; a close error stops preflight and is never retried.
Executable paths must
be literal regular executables; symlinked Python/kubectl installations are
refused. The configured Python must equal the running executable. Serving
requires Linux and isolated Python without site initialization; child processes
receive a minimal environment with user site packages and bytecode writes disabled.
Keep the administrator installation immutable for the connection lifetime.

For the g913 installation that supplies `/usr/local/bin/k3s`, the regular
`k3s-kubectl.sh` wrapper forwards arguments unchanged with `exec` to that literal
binary's `kubectl` subcommand. Install the wrapper outside the restricted Python
module directory as a root-owned, singly linked mode-0755 regular file, and set
`provider.kubectl` to its canonical installation path. A multicall symlink is
rejected by the deployment gate. The administrator must separately validate and
protect `/bin/sh`, `/usr/local/bin/k3s` and their parent paths; preflight checks the
configured wrapper file, not its interpreter or delegated executable. Local tests
substitute only the literal target in a private copy to verify argument preservation,
exit propagation and PID-preserving exec. They do not establish live k3s compatibility,
RBAC, or an installed subsystem, and no production target override is provided.

Omitting `--serve` runs only filesystem/account preflight and prints
`deployment_preflight: passed` with `acceptance: Incomplete`. It executes no
kubectl command and checks no live RBAC, backend proxy, SSH credential restriction,
or executable compatibility. The dedicated account and trusted executable/credential
installation remain administrative prerequisites. Local tests model administrator
ownership and parent directories while exercising real terminal filesystem checks:
`python3 -B -m unittest discover -s tools/reference-observer -p test_deployment_entrypoint.py -v`.

`remote_relay.py --enable-relay --config /absolute/relay-config.json` owns one
bounded SSH connection to an administrator-configured subsystem named
`sunshine-reference-observer-v1`. The remote subsystem must run this module with
`--serve` from an independently reviewed, pinned Linux installation of
`remote_relay.py`, `observer.py`, `supervisor.py`, and `fenrir_provider.py`.
The client uses OpenSSH `-s` with a fixed subsystem name; it does not send a shell
command. No subsystem installation, live g913 SSH invocation, credential change,
or cluster mutation is performed by the offline tests.

The JSON config contains `host`, `user`, integer `port`, absolute `known_hosts`
and `identity_file`, `runner_binding`, `collector`, `scenario`, `ready`,
`attached`, `output`, `remote_root`, `provider`, `expected_hashes`, and optional
`timeout` (at most 300 seconds). `provider` supplies the existing provider's
`kubectl`, `kubeconfig`, `context`, `namespace`, `agent_port`, `user`, `app`, and
`pairing` strings. `expected_hashes` maps the four module basenames above to
SHA-256 digests of the reviewed installation. These returned source hashes bind
transferred source versions; they are not runtime attestation against a malicious
SSH server. The CLI excludes the test-only SSH executable injection seam.

Use a dedicated one-line `known_hosts` file with the exact host alias and a pinned
public host key (no wildcard/CA/hashed aliases). Its absolute path must contain
only letters, digits, underscores, slashes, dots and hyphens. The identity file
must be private and owned by the current user. SSH uses no user/global SSH config,
password or keyboard-interactive prompts, agent, proxy/jump host, forwarding,
connection multiplexing, local commands, dynamic host-key updates or DNS trust.
The remote root must already exist as an owned mode-0700 directory. Each
connection creates a new UUID-named mode-0700 child exclusively; the local output
directory must also be fresh. The host key, subsystem installation, account
privileges, kubeconfig/RBAC, exact HTTPS agent port and code pins must be verified
on the live host before using this path for reference acquisition.

Start the relay before the runner writes its binding. Configure the runner's
READY and attachment paths to the relay's `ready` and `attached` destinations,
and enable the runner's immutable finish signal. The relay waits for the binding,
starts the remote provider before the supervisor (to establish the provider
directory), relays baseline READY, forwards the postlaunch nonce marker, then
relays the exact runtime acknowledgement. Linux-side monotonic checks and device
acquisition stay on Linux. Optional device feed, redacted sources, raw/host
observations, manifest and supervisor report are retained under `output/remote/`.
Every transfer carries a connection UUID, sequence and SHA-256; duplicate,
replaced, writable, aliased, oversized or foreign artifacts fail closed.
Coordination envelopes retain their 16-KiB limit; artifact publication supports
up to 1 MiB with the same no-replace rename and file/directory sync ordering.
Non-atomic supervisor outputs are read only after the supervisor process exits;
atomic observer identity, READY and runtime publications can flow earlier.
The supervisor report also hashes its remote-only `process.json`, `stdout.log`
and `stderr.log`. Those diagnostics stay in the remote session directory and
are not part of the relay's exported artifact inventory.

Each relay poll uses one direct-owner exit snapshot for both artifact eligibility
and terminal completion. If the supervisor exits during a scan that skipped its
non-atomic files, the next scan exports those completed files before completion;
the exit transition cannot bypass the final artifact inventory.

One local deadline bounds binding wait, pipe reads/writes and publication; a
remote lease bounds the remote owner even after transport loss. Process cleanup
polls before each signal and has bounded reaping allowances. An owner that exits
after SIGINT is not sent SIGKILL, and known-exited leaders are never
group-signalled. A successful final receipt confirms direct-owner process reaping
only; descendant cleanup after a known leader exit remains unconfirmed. Loss of
the connection records remote cleanup as
`unconfirmed`; it cannot prove remote process or Kubernetes resource cleanup.
Both values of the runner cancellation request are preserved. Cancellation
acknowledgement never establishes Pod deletion, neutral input, session release
or acceptance. All relay reports remain `acceptance: Incomplete`.

The fake SSH subsystem rejects insecure options and wrong destinations/commands.
Its true/false-cancellation populations run the real supervisor and collector
with synthetic inventory. It additionally tests deadlines, partial frames,
hash corruption, file/directory replacement, nonce mismatch, stale markers,
code-pin mismatch, no-follow path handling and reaping after signal errors.
A paused-writer test proves partial supervisor files are not read or frozen;
a valid collector publication larger than 16 KiB exercises artifact transfer.
The full verifier retains these populations and imports both completed ones
through the actual Rust consumer; this is offline evidence, not live SSH proof.

```sh
python3 tools/reference-observer/verify.py
```

This retains a fresh attempt beneath `.omo/evidence/t038-reference-observer-20261031/`
with unit, formatting/lint and real Rust consumer logs. The small contract-check
crate calls `measurements::import_host`, `controller_outcome` and the lifecycle
loader from the maintained client. It verifies that these synthetic publications
are accepted structurally, reject a foreign session, and remain Incomplete.
Unit scenarios include real pipe fragmentation/EOF, all four parser ABIs, dropped
events, missing/reordered/neutral/disconnect actions, device ambiguity/reuse/late
attachment, descriptor cleanup on removal/ioctl failure, hashes, publication
failure, quotas, and output-directory reuse refusal. No live Linux run is claimed.

Portable CI runs the same fixture-only driver on Ubuntu and macOS after fetching
the contract crate's locked dependencies. `--evidence-dir` places attempts under
`$RUNNER_TEMP/reference-observer-evidence`; the existing external `CARGO_TARGET_DIR`
and pinned Python/Rust setup are reused. The driver runs Cargo locked and offline,
uses `--no-cache` for both Ruff commands, and fails if any unit, lint, format, or
consumer check fails. It never calls live
collection. CI scheduling alone does not establish a hosted CI result.

The driver also runs the maintained Rust readiness barrier with a Python collector
publisher. Only Linux inventory/time are mocked; the binding read, filesystem
publication and Rust validation/retention are real. Its retained `python-collector/`
binding, READY envelope, report and observable prove launch follows readiness and
all criteria remain Incomplete. `readiness-artifacts.json` verifies those artifacts
exist, agree byte-for-byte where required, and contain the expected outcomes; an
empty Cargo test selection cannot pass verification. Additional unit cases cover
initial-device and fixture refusal, schema/version mismatches, replacement during
binding open/read, snapshot reuse, path aliases, no-replace races, fsync ordering,
publication failures and descriptor cleanup. These are offline synthetic checks.
Finish tests additionally retain empty/present-device final snapshots, partial and
unread-input errors, and a real inherited-pipe subprocess CLI transcript. Each
published population is checked by the actual Rust lifecycle/controller consumers;
all unsupported criteria remain Incomplete. Test acquisition is mocked and all
retained finish populations are explicitly `synthetic_fixture`.
