# Recovery inspector observation core

Build only with `bash tools/ps5-probes/build-recovery-inspector.sh OUTPUT_DIRECTORY`.
Run fixture verification with `python3 tools/ps5-probes/test_recovery_inspector.py OUTPUT_DIRECTORY/recovery-inspector-host`.

The build emits both the relocatable observation object and
`recovery-inspector-target.elf`. The target uses the complete pinned SDK v0.45 CRT
and statically links the pinned target OpenSSL SHA-256 implementation; it does not
use a bare `_start` or `-nostartfiles`. The launcher checks the exact firmware value
`0x13600007` before calling the observer. Normal and `NDEBUG` host tests prove that
unsupported firmware and a missing observer make no observer call and that observer
results propagate unchanged. The target build rejects changed compiler, `crt1.o`,
OpenSSL header or `libcrypto.a` inputs before linking.

The CRT's source-backed initialization includes process-local credential and
syscall-origin bootstrap before `main`, in addition to libc/runtime-linker setup.
Thus the observation core is read-only with respect to the inspected storage, but
the whole payload startup is not mutation-free. Building the ELF does not establish
deployability. Host regressions, target ELF inspection and independent code review
pass for the offline increment. Inspector-specific deployment, current-boot loader
continuity, independent exit observation and actual inspector behavior remain
unverified; no inspector was sent to the console.

`recovery_inspector_observe()` targets only `/data/ps5-dev/t050-20261031-codex6`.
The host fixture executable alone accepts another root. Every path component is
opened with no-follow semantics. Each opened entry is compared against its parent
directory lookup before and after reading. The entire ancestor chain (up to 32
descriptors including `/`) stays open until traversal ends, then every binding is
revalidated before cleanup and output. Each owned descriptor is closed once;
any close error refuses the observation without retrying the uncertain close.
Traversal rejects cross-device entries,
duplicate inodes, regular files with multiple links, symlinks, special types and
names outside ASCII alphanumerics plus dot/dash/underscore. Bounds are 128 entries,
three levels below root, 1 MiB per file, 4 MiB total hashed bytes and 255-byte paths.
Directories naturally have multiple links; the single-link check is for files.
Failure emits no partial inventory. Output includes only metadata and SHA256, never
file contents. Checks do not use `assert` and remain active with `NDEBUG`.

`complete` means this traversal completed, **not** that the expected codex6 fixture
layout was verified, writers were excluded, or the snapshot was atomic. The caller
must validate expected layout separately and require independent no-writer evidence.
Metadata comparisons detect tested mutations but cannot prove absence of hostile
concurrent replacements, restored timestamps, or post-observation changes.

`boot_time` uses read-only `CTL_KERN/KERN_BOOTTIME` and validates response length and
timeval ranges. It is null on failure. The SDK defines this as kernel boot time;
13.60 runtime semantics and persistence across rest still need console validation.
It is not a cryptographic boot nonce, is not an existing pre-rest baseline, and
must not alone authorize recovery. The observer's `firmware_validated` field remains
false; the launcher's exact firmware gate does not change that observation contract. Exact
process-start identities, no-active-game and no-writer observations remain null.
In particular, SDK `ki_start` availability does not prove the firmware process
record ABI. No claims about independent process-exit observation are implemented.

The retained SDK source archive matches all 30 files in the audited CRT source
tree, and the installed `crt1.o` matches the publisher-digest-matched v0.45 release
archive. The [exact-loader audit](../../.omo/evidence/t050-relapse-loader-compat-20261101/REPORT.md)
establishes distribution provenance for the Relapse 13.60 loader and a byte-identical
SDK v0.45 full-CRT verifier that historically executed through it on the same console.
Five CRT functions match the inspector after masking only pinned object relocation
fields, supporting the historically exercised full-CRT ABI class. The inspector's
23 GLOB_DAT imports and one R_X86_64_64 relocation, current-boot loader continuity,
inspector execution/output and independent exit remain unverified. Exact loader build
provenance remains unresolved; public v0.27 source is not byte-equivalence evidence
for this distinct binary. Target build hashes and inspection remain in the
[launcher report](../../.omo/evidence/t050-recovery-launcher-20261101/REPORT.md).

All seven recovery callbacks (services, boot, process, isolation, HDMI, inventory
and exit observer) remain unbound, as do the reviewed public-certificate fingerprint
path and genuine pre-rest boot evidence. [Discovery at `2026-11-01T01:15:03Z`](../../.omo/evidence/t050-rest-device-20261031/discovery-20261101T011503Z.log)
returned `HTTP/1.1 620 Server Standby` for host `00E421FDEC6B`, firmware `13600007`;
the [rest record](../../.omo/evidence/t050-rest-device-20261031/rest-attempt.md)
also records absent exploit/development-service ports. No inspector, verifier or
cleanup payload was dispatched. Preserve the codex6 population without reinitializing
it before inspection. T050 remains Incomplete.
