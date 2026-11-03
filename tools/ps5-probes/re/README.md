# T041 Remote Play interoperability investigation

The owner directed the video investigation toward the console's Remote Play
implementation on 2026-10-25. These tools are nonshipping research. They do not
establish an encoder ABI, hardware video output, or a working streaming host.

`remoteplay_export.c` exports executable text and dynamic-linker symbol, string
and relocation tables from the resident `SceRemotePlay`, `SceAvCapture` and
`SceSysAvControl.elf` processes. It selects their main image and the explicitly
listed Remote Play/Avcap2/Venc/Ipmi modules. Other modules are inventoried by
basename only. No process heap, user credentials or content recording is requested.
The SDK's module metadata and reported text extent still require local validation;
neither a readable region nor its filename proves correct code or a callable ABI.

The sole firmware table is for 13.60. Structure layouts, its dynlib-head field,
and the existing page-table read helper are sourced to SDK v0.45 commit
`a7685ce8e97ce1f8a4f38253a41d54241c9124f1`,
[`crt/kernel.h`](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/crt/kernel.h),
[`crt/kernel.c`](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/crt/kernel.c)
and [`crt/kernel_procio.c`](https://github.com/ps5-payload-dev/sdk/blob/a7685ce8e97ce1f8a4f38253a41d54241c9124f1/crt/kernel_procio.c).
There are no newly guessed firmware offsets, media calls, foreign-process
injection, page-table writes, credential fallbacks, service restarts or online
Remote Play sessions. The **stock SDK startup does change process credentials
and syscall-origin bounds before main**; reversal/shared effects are unverified.
Execution approval must explicitly cover that existing runtime baseline.

## Bounds and ownership

- At most 512 module nodes per selected process, with cycle rejection.
- At most 16 MiB of reported executable text and 4 MiB per linker table;
  at most 128 MiB written in total. One fixed 16 KiB I/O buffer is reused.
- A 120-second cooperative window and SIGINT/SIGTERM cancellation are checked
  between operations. Individual kernel reads and filesystem writes have no
  verified hard deadline. Do not describe a sender timeout as payload termination.
- Output is created exclusively, mode 0600, under the new private directory
  `/data/ps5-dev/t041-remoteplay-<exporter-pid>/`, mode 0700. Existing paths are
  never replaced. Incomplete region files are removed and failures reported;
  completed files remain for retrieval and verified, explicit cleanup.
- The process is not stopped for a snapshot. Tables/code are live observations,
  potentially relocated or inconsistent. Do not treat them as original file
  images, fabricate ELF headers, or infer hardware capability from an export.

## Build and local tests

```sh
R3_BUILD_DIR=/Volumes/4TB_Drive/tmp/t041-audio-review/build \
  bash tools/ps5-probes/build-r3.sh
```

This builds the separately named `remoteplay-export.elf` and runs the two host
probe suites. It does not deploy anything. Portable CMake also registers
`test_media_probe` and `test_remoteplay_export` under `nonshipping-probe`.
Their simulated services/memory never count as console evidence.

## Proposed execution — approval still required

The earlier read-only console window was released. Obtain a fresh exclusive
window through the coordinator. First revalidate IPv4 and IPv6 isolation with
the previously reviewed network inventory payload, whose exact ELF SHA-256 is
`30154bcf13d0ee3d870426f7d3cb328f3aadcaa3c8fa72344d6666d5fd556248`.
Its existing source/ABI record is in [R3 findings](../../../specs/001-moonlight-streaming-host/research-r3-findings.md).
Mac forwarding flags alone do not establish console isolation. Do not change
routes, PF, NAT, Internet Sharing, services or firmware to make this check pass.

Only after owner approval, window grant, and passing fresh route/interface
evidence, execute the exact approved exporter bytes once:

| Input | SHA-256 |
|---|---|
| `remoteplay_export.c` | `6b18a84d902674a86e734d7978838dbe3b3124865725bd897c2e4628b0b1a071` |
| `remoteplay_export.h` | `1f85db0dc8295b2795684d8ed6b2871648cacb82387b8f3adb2a79871e98b7f4` |
| `remoteplay-export.elf` | `11b443de49fff827435d45e38ee8e291249ed99d822aebc96b757ee2c6feabcd` |

Five approved 2026-10-25 execution generations exist, each recorded in the
R3 findings with its exact hashes, run evidence and postflight checks:
the first (`ec1a8af8…e44`) exited 1 on a `sysctl` record-size assumption;
the second (`7dbd2253…71a5`) exported 24 linker tables and revealed the
live 13.60 `dynlib_obj` layout divergence (SDK `textsize` slot 0x40
holds a kernel pointer; `mapbase`/`mapsize`/`entry`/`vaddrbase`/`dynsec`
match the SDK offsets); the third (`8f833a35…f68`) added raw 384-byte
module-object dumps plus full-image attempts (SceAvCapture complete,
others stopped at the 13.60 text/data mapping gap); the fourth
(`cfb195bb…74a`) kept nonzero text prefixes as marked partials; this
fifth is the **gap-skipping segment walk**: per selected module it walks
`[mapbase, mapbase+mapsize)` in 4 KiB steps, keeps every readable run in
one `<pid>-<module>-segs.bin` (concatenated readable bytes only), prints
one `R3_RE_SEGMENT` manifest line per run plus an `R3_RE_SEGS` summary
(runs/written/skipped), and never interpolates unmapped pages. Bounds:
16 MiB per image, 4 MiB per table, 512 B per object, 128 MiB total,
120 s cooperative window; private new run directory per run with
exclusive 0600 creation, incomplete outputs deleted (nonzero partial
text kept only in v4 semantics), no media calls, no debugger, no
service changes, page-table READS only, stock SDK before-main credential
baseline. Every run's FTP retrieval (fresh connection per file, size
verification, SHA-256) and verified `DELE`/`RMD` cleanup of only that
run's files are recorded with the runs.

Direct FTP `RETR` of the two caller eboots failed at both 3 s and 120 s
(etaHEN starts decrypting into `/user/temp` but never streams; temp files
cleaned on disconnect), so memory images are the acquisition path. The
retrieved v5 images carry the full SceRemotePlay and SceAvCapture caller
code; see
[R3 findings](../../../specs/001-moonlight-streaming-host/research-r3-findings.md)
for the recovered Avcap2/Venc calling-contract facts and remaining gaps.

The preflight command, repeated once after the export for the after-state, is:

```sh
timeout 20 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t040-t042/build/network-inventory.elf
```

The single export command is:

```sh
timeout 150 /Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk/bin/prospero-deploy \
  -h 192.168.2.2 -p 9021 \
  /Volumes/4TB_Drive/tmp/t041-hermes/build/remoteplay-export.elf
```

The reviewed hashes belong in a private run manifest and the dated R3 findings.
If the sender times out, establish process state through existing read-only
services; do not kill a Sony service, retry the payload or reboot. Stop on
unresponsiveness. Verify that the exporter exited and the original services and
foreground state remain intact. Retrieve only the files it reports, enforce
local size bounds, compute checksums, and retain them privately under
`/Volumes/4TB_Drive/tmp/t041-audio-review/`. Remove only this run's console files
after successful retrieval/verification, and release the console window.

## Local analysis after acquisition

Hash each region with its process/module/base/size provenance before importing
code into the installed Ghidra 11.4.2 or Rizin. Import raw code at the observed
base as x86-64; retain that it is a live code-region reconstruction. Decode
`Elf64_Sym` and `Elf64_Rela` tables using their sourced 24-byte entry layouts,
validate every string offset and symbol index, and correlate relocations/imports
against the public SDK's name-derived NIDs and the seven source-cited audio NIDs.
Unknown NIDs remain unknown, without invented function names or signatures.

Trace Remote Play's actual video session setup through its Avcap2/Venc/Ipmi
callers and corresponding implementations. Recover argument registers, request
sizes, bounds checks, service/context requirements, surface ownership/fences,
encoded-buffer release, profiles/bit depth, protection/source epochs and teardown.
Trace audio metadata direction and accepted argument widths while those code
regions are available. Keep decompilation, binaries and raw tables outside Git;
record only sourced interface facts and evidence references in the findings.
No media invocation is admitted until its complete contract and safe cleanup are
established. Sustained modes, hardware encoding, Main10/HDR metadata/conversion,
protection and resource budgets still require their separate device observations.

## Selector-list investigation (2026-10-25)

The separately built `resource-library-export.elf` selects only
`libSceResourceArbitrator.sprx` and `libSceVideoRecording.sprx` in resident
SceAvCapture, SceRemotePlay and SceShellCore. It uses the same bounded segment
walk and table export; it does not load a library or export those processes'
main images. Its approved PID328 run recovered two complete 64 KiB
ResourceArbitrator images plus linker tables. Neither selected library was
resident in ShellCore, and VideoRecording was not recovered by this run.

The saved daemon GOT maps selector-query NID `fBX+DBOwNis` to ResourceArbitrator
offset `0x11f0`. It copies 128 bytes from `/SceRASharedMemory` (mapped size
`0x360`) under semaphore `SceResourceArbitrator`. The library also exposes
registration (`kJvjeet+f9I`, IPC 0), unregistration (`LqmYZGB-UEQ`, IPC 1),
and state requests (`CYjfezkWcaE`, IPC 2) to `SceArbitratorIpc`. Sony's
Remote Play caller registers ID `0xb4`, requests `RUNNING` / `STOP`, and
copies that ID into original video request `+0x28`. A constructor's zeroed
request is not its final initialized request. These recovered contracts still
need independent payload-context acceptance and cleanup observations.

Images, tables, exact hashes, new disassembly and run evidence stay under
`/Volumes/4TB_Drive/tmp/t041-hermes/t041-codex/`. The predecessor's
`analysis/` and `analysis-final/` artifacts remain unchanged. The R3 findings
contain the dated correction to their earlier selector-offset interpretation.
