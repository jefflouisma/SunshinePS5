# GameStream / Moonlight protocol notes (client-side test driver reference)

Reverse-engineering notes for extending the Rust GameStream test client. Every protocol constant below cites
`file:lines @ sha`. The sources are GPL-3.0; these notes record **facts** (constants, layouts, sequences) and
quote only short fragments where needed to verify them. No code is copied or translated.

## Pinned sources

| Tag used below | Repository | Commit SHA |
|---|---|---|
| `mcc@41cedb8` | github.com/moonlight-stream/moonlight-common-c (master) | `41cedb89d6b8592f99c09975c764ca3875034726` |
| `sun@d0b4a88` | github.com/LizardByte/Sunshine (master) | `d0b4a88ef727f8dabf80cc970a14739b55c885de` |
| `mqt@8f2e52e` | github.com/moonlight-stream/moonlight-qt (master), used for the `/launch` parameters | `8f2e52e54046fc942e2b80ffb88a71ff0f2300bd` |
| `enet@aca8784` | github.com/cgutman/enet (branch `moonlight`): the ENet fork vendored as the `enet` submodule of mcc, at both mcc@41cedb8 and Sunshine's pinned mcc `04053843` | `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1` |
| `enet-base@2a85cd6` | github.com/lsalzman/enet: merge base used to diff the fork | `2a85cd64459f6ba038d233a634d9440490dbba12` |
| `nanors@a9b1636` | github.com/sleepybishop/nanors (mcc@41cedb8 submodule) | `a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29` |
| `nanors@b1e3c22` | nanors as pinned by Sunshine's mcc submodule `04053843c90c4268583f2496b997d54d59f03957` | `b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a` |

Sunshine compiles parts of moonlight-common-c directly: `Input.h`, `Rtsp.h`, `RtspParser.c`, `Video.h`, nanors
`rs.c`, and the ENet headers (`cmake/compile_definitions/common.cmake:134-140, 215-217 @ sun@d0b4a88`). It also
includes `Limelight-internal.h` (`src/stream.cpp:22`, `src/rtsp.cpp:8 @ sun@d0b4a88`). So the struct layouts in
mcc `Video.h`/`Input.h` are shared by both ends. Sunshine pins mcc at `04053843`, which is older than mcc@41cedb8.

Citation style: sections 1-11 write `path:lines @ mcc@41cedb8` (or `sun@`, `mqt@`, `enet@`). Sections 12-13 write
`mcc/…`, `sun/…`, `mqt/…` path prefixes with the 7-character SHA. The pins are the same in both.

Default port map (Sunshine base port `47989`, `src/config.cpp:905 @ sun@d0b4a88`; `net::map_port(off) = base + off`,
`src/network.cpp:243-253`):

| Purpose | Offset (citation) | Default port |
|---|---|---|
| HTTP (pairing, serverinfo) | `PORT_HTTP = 0` (`src/nvhttp.h:48`) | TCP 47989 |
| HTTPS (launch, resume, applist) | `PORT_HTTPS = -5` (`src/nvhttp.h:53`) | TCP 47984 |
| Video RTP | `VIDEO_STREAM_PORT = 9` (`src/stream.h:19`) | UDP 47998 |
| Control (ENet) | `CONTROL_PORT = 10` (`src/stream.h:20`) | UDP 47999 |
| Audio RTP | `AUDIO_STREAM_PORT = 11` (`src/stream.h:21`) | UDP 48000 |
| RTSP | `RTSP_SETUP_PORT = 21` (`src/rtsp.h:18`) | TCP 48010 |

mcc's own well-known fallbacks are the same numbers (`src/Limelight.h:912-929 @ mcc@41cedb8`).

Sunshine identifies itself with `appversion` `"7.1.431.-1"` and `GfeVersion` `"3.23.0.74"` (`src/nvhttp.h:38,43 @ sun@d0b4a88`).
mcc treats a negative 4th version component as "is Sunshine": `IS_SUNSHINE() (AppVersionQuad[3] < 0)`
(`src/Limelight-internal.h:84 @ mcc@41cedb8`). Every Sunshine-extension code path in mcc is gated on this check.

---

## 1. serverinfo XML (codec / HDR / resolution fields)

**`ServerCodecModeSupport` bit values** (`src/Limelight.h:505-522 @ mcc@41cedb8`):

| Constant | Value | Notes |
|---|---|---|
| `SCM_H264` | `0x00000001` | |
| `SCM_HEVC` | `0x00000100` | HEVC Main |
| `SCM_HEVC_MAIN10` | `0x00000200` | HEVC Main10 (HDR-capable) |
| `SCM_AV1_MAIN8` | `0x00010000` | Sunshine extension |
| `SCM_AV1_MAIN10` | `0x00020000` | Sunshine extension |
| `SCM_H264_HIGH8_444` | `0x00040000` | Sunshine extension (4:4:4) |
| `SCM_HEVC_REXT8_444` | `0x00080000` | Sunshine extension (4:4:4) |
| `SCM_HEVC_REXT10_444` | `0x00100000` | Sunshine extension (4:4:4, 10-bit) |
| `SCM_AV1_HIGH8_444` | `0x00200000` | Sunshine extension (4:4:4) |
| `SCM_AV1_HIGH10_444` | `0x00400000` | Sunshine extension (4:4:4, 10-bit) |

Masks: `SCM_MASK_10BIT = HEVC_MAIN10|HEVC_REXT10_444|AV1_MAIN10|AV1_HIGH10_444`,
`SCM_MASK_YUV444 = H264_HIGH8_444|HEVC_REXT8_444|HEVC_REXT10_444|AV1_HIGH8_444|AV1_HIGH10_444` (`src/Limelight.h:517-522`).

**How Sunshine fills it**: `get_codec_mode_flags()` (`src/nvhttp.cpp:1203-1234 @ sun@d0b4a88`). It always sets
`SCM_H264`. It adds `H264_HIGH8_444` if the encoder probe found 4:4:4 support. `active_hevc_mode >= 2` gives
`SCM_HEVC` (plus `REXT8_444` if 4:4:4 is supported). `active_hevc_mode` 3 or 5, with 10-bit probed, gives
`SCM_HEVC_MAIN10`. Modes 4 or 5, with 10-bit 4:4:4 probed, give `SCM_HEVC_REXT10_444`. AV1 follows the same
pattern with `active_av1_mode`. The value is written as `root.ServerCodecModeSupport` (`src/nvhttp.cpp:1293-1294`).

**Video-format constants on the client side**, used in `supportedVideoFormats` and as the negotiated format
(`src/Limelight.h:223-241 @ mcc@41cedb8`). These are a *different* bit space from SCM:
`H264 0x0001`, `H264_HIGH8_444 0x0004`, `H265 0x0100`, `H265_MAIN10 0x0200`, `H265_REXT8_444 0x0400`,
`H265_REXT10_444 0x0800`, `AV1_MAIN8 0x1000`, `AV1_MAIN10 0x2000`, `AV1_HIGH8_444 0x4000`, `AV1_HIGH10_444 0x8000`.
Masks: `MASK_H264 0x000F`, `MASK_H265 0x0F00`, `MASK_AV1 0xF000`, `MASK_10BIT 0xAA00`, `MASK_YUV444 0xCC04`.

**Other serverinfo fields Sunshine emits** (`src/nvhttp.cpp:1243-1310 @ sun@d0b4a88`): `hostname`, `appversion`,
`GfeVersion`, `uniqueid`, `HttpsPort`, `ExternalPort`, `mac`, `LocalIP`, `ServerCodecModeSupport`,
`ExternalIP` (only if configured), `PairStatus` (1 only over HTTPS when `uniqueid` is present), `currentgame`, and
`state` (`SUNSHINE_SERVER_BUSY`/`SUNSHINE_SERVER_FREE`).
- **Max resolution**: the only resolution-related field is `MaxLumaPixelsHEVC`. It is `"1869449984"` when
  `active_hevc_mode > 1`, else `"0"` (`src/nvhttp.cpp:1268`). Sunshine emits no max-width/height field.
  moonlight-qt parses `MaxLumaPixelsHEVC` and `ServerCodecModeSupport`, and defaults SCM to `SCM_H264` if the
  field is absent (`app/backend/nvcomputer.cpp:149-163 @ mqt@8f2e52e`).
- **HDR**: serverinfo has no HDR field. HDR capability is expressed only through SCM 10-bit bits. `/applist`
  carries a per-app `IsHdrSupported` = `active_hevc_mode >= 3 ? 1 : 0` (`src/nvhttp.cpp:1353 @ sun@d0b4a88`).
- UNCONFIRMED: the exact meaning of each numeric `hevc_mode`/`av1_mode` value (0-5). Only the thresholds used in
  `get_codec_mode_flags()` and the probe-fallback logic (`src/video.cpp:3311-3340`) were read.

---

## 2. `/launch` and `/resume` query parameters

**What moonlight-qt sends** (`NvHTTP::startApp`, `app/backend/nvhttp.cpp:199-246 @ mqt@8f2e52e`):

| Param | Value sent |
|---|---|
| `appid` | app ID |
| `mode` | `{width}x{height}x{fps}` (fps forced to 0 only for GFE when fps > 60) |
| `additionalStates` | `1` |
| `sops` | `0`/`1` |
| `rikey` | 16-byte AES key as **hex** (`QByteArray::toHex()`, lowercase) |
| `rikeyid` | first 4 bytes of `remoteInputAesIv` read **big-endian** into a **signed** int, then decimal (`nvhttp.cpp:210-213,228`). Only those 4 IV bytes are random; the rest are zero (`app/streaming/session.cpp:704-705`) |
| `hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0` | sent only if `supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT` (`nvhttp.cpp:229-231`) |
| `localAudioPlayMode` | `0`/`1` |
| `surroundAudioInfo` | `SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(cfg)` = `channelMask << 16 \| channelCount` (`src/Limelight.h:205-219 @ mcc@41cedb8`). Stereo = `0x30002` = `196610` |
| `remoteControllersBitmap`, `gcmap` | gamepad mask |
| `gcpersist` | `0`/`1` |
| *suffix* | `LiGetLaunchUrlQueryParameters()` returns `"&corever=1"` (`src/Connection.c:550-553 @ mcc@41cedb8`; API doc `src/Limelight.h:37-42`) |

**How Sunshine interprets them** (`make_launch_session`, `src/nvhttp.cpp:487-545 @ sun@d0b4a88`):
- `rikey`: hex-decoded into `gcm_key` (`:492-493`). This 16-byte key is used for control-stream AES-GCM, RTSP
  AES-GCM, video AES-GCM, and audio AES-CBC (see sections 5, 8, 9).
- `rikeyid`: parsed as an integer and stored **big-endian** in the first 4 bytes of a 16-byte `iv` (`:540-543`).
  The audio IV base is `avRiKeyId = BE32(iv[0..4])`, which is numerically the `rikeyid` value
  (`src/stream.cpp:2385`).
- `mode`: split on `x` into width/height/fps (`:496-511`). `uniqueid` and `appid` are also read (`:512-513`).
- `sops`, `surroundAudioInfo` (default `196610`), `surroundParams` (default empty), `continuousAudio`, `gcmap`: `:514-518`.
- `hdrMode` → `enable_hdr` (`:519`). **None of the `clientHdrCap*` parameters are read anywhere in Sunshine.**
  `grep` of the downloaded sources finds no match, and a GitHub code search for `clientHdrCapVersion` in
  LizardByte/Sunshine returned 0 results. `enable_hdr` is used only for (a) the display-device HDR toggle when
  `dd.hdr_option == automatic` (`src/display_device.cpp:525-536`) and (b) the app environment variable
  `SUNSHINE_CLIENT_HDR` (`src/process.cpp:198`). It does **not** select the codec or bit depth; ANNOUNCE does that
  (section 4).
- `corever >= 1` enables **encrypted RTSP** and switches the session URL scheme to `rtspenc://` (`:521-530`).
- A random 8-byte `av_ping_payload` is generated and sent **hex-encoded (16 ASCII chars)**. A random 32-bit
  `control_connect_data` is also generated (`:534-538`).
- `/launch` requires `rikey`, `rikeyid`, `localAudioPlayMode` and `appid`, else it returns 400 (`src/nvhttp.cpp:1389-1401`).
  `localAudioPlayMode` becomes `host_audio` (`:1414`).
- `/resume` requires `rikey` and `rikeyid` (`:1511-1521`). `localAudioPlayMode` is honoured only when no session is
  active (`:1523-1529`).
- Response: `root.sessionUrl0 = "{rtsp://|rtspenc://}{host-local-addr}:{base+21}"` and `root.gamesession=1` on
  launch (`:1461-1471`), `root.resume=1` on resume (`:1562-1572`). If encryption is MANDATORY for the client's
  address and `corever` was not sent, the response is 403 (`:1439-1448`, `:1551-1560`).
- mcc reads the RTSP port from the last `:` of `sessionUrl0`, else defaults to 48010 (`src/Connection.c:182-200, 272-279`).
  It turns encrypted RTSP on iff the URL contains `rtspenc://` (`src/RtspConnection.c:955`).

---

## 3. RTSP sequence (OPTIONS / DESCRIBE / SETUP×3 / ANNOUNCE / PLAY)

**Transport.** Sunshine RTSP is **TCP** on base+21. Each request uses a **new TCP connection**. The client writes the
request, then reads until the server closes the socket (`transactRtspMessageTcp`, `src/RtspConnection.c:386-518 @ mcc@41cedb8`).
RTSP over ENet exists only for GFE `5.x`–`7.x` with build < 404 (`:950`). It is not used with Sunshine.
Sunshine binds each accepted RTSP connection to the **single currently pending launch session**, or closes it if
there is none (`src/rtsp.cpp:495-518 @ sun@d0b4a88`).

**Common headers** (`initializeRtspRequest`, `src/RtspConnection.c:72-91`): `CSeq` (starts at 1, incremented per
request, `:951`) and `X-GS-ClientVersion: 14` (gen 7, `:990-1008`). `Host: <urlAddr>` is sent only when not using
ENet. **`Host` matters to Sunshine for stereo:** HIGH_QUALITY stereo audio is enabled unless the `Host` value contains
`0.0.0.0` (`src/rtsp.cpp:1163-1173 @ sun@d0b4a88`). mcc uses `0.0.0.0` (low quality) unless bitrate ≥ 15000 kbps,
the opus decoder is not slow, and the stream is not remote surround (`src/RtspConnection.c:968-988`;
`HIGH_AUDIO_BITRATE_THRESHOLD 15000`, `src/Limelight-internal.h:94`).

**Sequence and what to parse** (`performRtspHandshake`, `src/RtspConnection.c:944-1442 @ mcc@41cedb8`):

1. **OPTIONS** `<rtspTargetUrl>` → must be 200 (`:1047-1065`). Sunshine replies with CSeq only (`src/rtsp.cpp:846-856`).
2. **DESCRIBE** `<rtspTargetUrl>` with `Accept: application/sdp` and `If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT`
   (`src/RtspConnection.c:551-572`). The body is read and used (`:1067-1165`):
   - **Codec availability**: AV1 if the body contains `AV1/90000` (`:1090`). HEVC if it contains
     `sprop-parameter-sets=AAAAAU` (the base64 HEVC VPS prefix, `:1104-1110`). Otherwise H.264. The profile within
     each codec comes from SCM ∩ client formats, with 10-bit/4:4:4 preferred (`:1090-1136`). Sunshine emits the
     bare line `sprop-parameter-sets=AAAAAU` when `active_hevc_mode != 1` and `a=rtpmap:98 AV1/90000` when
     `active_av1_mode != 1` (`src/rtsp.cpp:904-910`).
   - `x-nv-video[0].refPicInvalidation` present → RFI supported (`:1139`; Sunshine `src/rtsp.cpp:900-902`).
   - `x-ss-general.featureFlags` → `SunshineFeatureFlags` (`:1145-1147`). Sunshine sends
     `platf::get_capabilities()` (`src/rtsp.cpp:878`): `pen_touch = 0x01`, `controller_touch = 0x02`
     (`src/platform/common.h:401-415 @ sun@d0b4a88`), which equal mcc `LI_FF_PEN_TOUCH_EVENTS 0x01`,
     `LI_FF_CONTROLLER_TOUCH_EVENTS 0x02` (`src/Limelight.h:1016-1019 @ mcc@41cedb8`).
   - `x-ss-general.encryptionSupported` / `x-ss-general.encryptionRequested` (`:1149-1156`). Sunshine always
     *supports* `SS_ENC_CONTROL_V2|SS_ENC_AUDIO`, adds `SS_ENC_VIDEO` unless encryption mode for the client
     address is NEVER, always *requests* `SS_ENC_CONTROL_V2`, and requests VIDEO|AUDIO only in MANDATORY mode
     (`src/rtsp.cpp:880-898`). Default modes are LAN = NEVER, WAN = OPPORTUNISTIC (`src/config.cpp:844-845`).
     Flag values: `SS_ENC_CONTROL_V2 0x01`, `SS_ENC_VIDEO 0x02`, `SS_ENC_AUDIO 0x04`
     (`src/Limelight-internal.h:47-50 @ mcc@41cedb8`).
   - Opus `a=fmtp:97 surround-params=<C><S><CS><mapping...>` lines, needed only for > 2 channels
     (`parseOpusConfigurations`, `:747-848`). Sunshine format: `src/rtsp.cpp:912-943`.
3. **SETUP `streamid=audio/0/0`** with `Transport: unicast;X-GS-ClientPort=50000-50001` (the value is ignored by the
   host) and `If-Modified-Since` (`:575-615`).
   - Audio port from `Transport: ... server_port=<n>`: mcc does `strstr("server_port=")` then `strtol`
     (`:717-745`). Falls back to 48000 (`:1190-1200`).
   - `X-SS-Ping-Payload`: accepted only if exactly 16 chars, copied into `AudioPingPayload.payload` (`:1202-1207`).
   - `Session`: required. The first `;`-token is kept (`:1214-1242`). **The audio ping thread starts now**, before
     the rest of RTSP (`notifyAudioPortNegotiationComplete`, `:1209-1212`; `src/AudioStream.c:90-110`).
   - Sunshine's reply (`cmd_setup`, `src/rtsp.cpp:956-1013`): `Session: DEADBEEFCAFE;timeout = 90` (constant),
     `Transport: server_port=<port>` with no other transport tokens, and `X-SS-Ping-Payload: <16 hex chars>` for
     audio and video. The stream type is the text between `=` and the next `/` of the target (`:969-985`).
4. **SETUP `streamid=video/0/0`** (with `Session`) → video port from `server_port`, fallback 47998
   (`src/RtspConnection.c:1247-1287`). `X-SS-Ping-Payload` is parsed again into `VideoPingPayload`. Sunshine sends the
   **same** `av_ping_payload` for audio and video (`src/rtsp.cpp:1006-1007`; `src/stream.cpp:2347,2384`).
5. **SETUP `streamid=control/13/0`** for 7.1.431+ (`controlStreamId`, `src/RtspConnection.c:953`; request at
   `:1289-1331`) → **control port from `server_port`**, fallback 47999 (`:1318-1328`).
   `X-SS-Connect-Data: <decimal u32>` → `ControlConnectData = strtoul(..., 0)` (`:1309-1316`). Sunshine sends
   `control_connect_data` as decimal (`src/rtsp.cpp:1000-1004`).
6. **ANNOUNCE** target `streamid=control/13/0` (7.1.431+, `src/RtspConnection.c:647-648`) with `Session`,
   `Content-type: application/sdp`, `Content-length`, and the SDP body (`:639-676`; attributes in section 4).
   Sunshine allocates and **starts the stream session on ANNOUNCE** (`src/rtsp.cpp:1251-1262`). That starts the
   video and audio threads, which wait up to `ping_timeout` for a UDP ping (section 8).
7. **PLAY** target `/` (7.1.431+) with `Session` (`src/RtspConnection.c:1353-1372`). Sunshine ignores the target and
   replies 200 (`src/rtsp.cpp:1273-1283`).

**Session ID**: Sunshine always returns `DEADBEEFCAFE;timeout = 90`, and the client echoes `DEADBEEFCAFE`. The real
per-session correlation uses the ping payload (UDP) and connect-data (ENet), not the RTSP Session header.

**Encrypted RTSP** (`rtspenc://`, enabled by `corever=1`):
- Framing: 24-byte header `{u32 BE typeAndLength = 0x80000000 | plaintextLen, u32 BE sequenceNumber, u8 tag[16]}`
  followed by AES-128-GCM ciphertext of the full serialized RTSP message, including the body
  (`src/RtspConnection.c:93-154 @ mcc@41cedb8`; `src/rtsp.h:25-65 @ sun@d0b4a88`).
- Key = `rikey`. IV = 12 bytes: `[0..3] = seq` little-endian, `[10] = 'C'` (client) or `'H'` (host), `[11] = 'R'`.
  The client pre-increments its counter (first message seq = 1) (`src/RtspConnection.c:123-135`). The host keeps its
  own pre-incremented counter (`src/rtsp.cpp:783-801`) and decrypts the client's with `'C','R'` (`src/rtsp.cpp:206-231`).
- A plaintext message on an encrypted session is rejected by both sides (`src/RtspConnection.c:182-185`; `src/rtsp.cpp:165-170`).

---

## 4. ANNOUNCE SDP attributes

**Format**: header lines `v=0`, `o=android 0 <rtspClientVersion> IN IPv4|IPv6 <addr>`, `s=NVIDIA Streaming Client`.
Then one `a=<name>:<value> \r\n` line per attribute (note the space before CRLF). The tail is `t=0 0` and
`m=video <videoPort>  ` (`src/SdpGenerator.c:27-80, 546-564 @ mcc@41cedb8`). Sunshine splits lines on `\r`/`\n`,
keeps only `s=` and `a=` lines, splits `name:value` at the first `:`, and strips **one** trailing space from the
value (`src/rtsp.cpp:1032-1073 @ sun@d0b4a88`).

**Sunshine defaults for omitted attributes** (`src/rtsp.cpp:1075-1090`): `encoderCscMode 0`, `bitStreamFormat 0`,
`dynamicRangeMode 0`, `aqos.packetDuration 5`, `useReliableUdp 1`, `fec.minRequiredFecPackets 0`,
`x-nv-general.featureFlags 135`, `x-ml-general.featureFlags 0`, `vqos qosTrafficType 5`, `aqos qosTrafficType 4`,
`x-ml-video.configuredBitrateKbps 0`, `x-ss-general.encryptionEnabled 0`, `chromaSamplingType 0`, `intraRefresh 0`,
`clientRefreshRateX100 0`. **Any other attribute Sunshine reads is mandatory**: a missing one throws
`out_of_range` → `400 BAD REQUEST` (`:1096-1161`).

| Attribute (exact name) | Client value (mcc) | Sunshine meaning |
|---|---|---|
| `x-ml-general.featureFlags` | `ML_FF_FEC_STATUS(0x01) \| ML_FF_SESSION_ID_V1(0x02)` = `3`, Sunshine only (`SdpGenerator.c:270-274`; flags `Limelight-internal.h:86-88`) | `mlFeatureFlags` (`rtsp.cpp:1107`). `SESSION_ID_V1` makes Sunshine match ENet peers by connect-data and UDP pings by payload only (`stream.cpp:693-707, 2090-2124`) |
| `x-ss-general.encryptionEnabled` | `EncryptionFeaturesEnabled`: `CONTROL_V2` whenever supported, plus VIDEO/AUDIO if the client opts in or the host requests (`SdpGenerator.c:276-304`) | `encryptionFlagsEnabled` (`rtsp.cpp:1110`). **If absent it is 0 → legacy control-stream IV** (section 9) |
| `x-ss-video[0].chromaSamplingType` | `1` if the negotiated format is 4:4:4, else `0` (`SdpGenerator.c:306-312`) | 0 = 4:2:0, 1 = 4:4:4 (`video.h:44`) |
| `x-ss-video[0].intraRefresh` | not sent by mcc@41cedb8 | Sunshine-only, default 0 (`rtsp.cpp:1089,1155`) |
| `x-nv-video[0].clientViewportWd` / `clientViewportHt` | width / height (`SdpGenerator.c:315-318`) | encode resolution (`rtsp.cpp:1134-1135`) |
| `x-nv-video[0].maxFPS` | fps (`:320-321`) | framerate (`rtsp.cpp:1136`) |
| `x-nv-video[0].packetSize` | `StreamConfig.packetSize`, **minus 32 (`sizeof(ENC_VIDEO_HEADER)`) when video encryption is on** (`:323-330`). moonlight-qt default is 1392 on LAN, 1024 on VPN/remote (`session.cpp:1668-1699 @ mqt`) | `config.packetsize` = NV payload bytes per packet, including the 16-byte NV header. Host config `packetsize` may lower it (`rtsp.cpp:1105, 1117-1132`) |
| `x-nv-video[0].rateControlMode` | `4` | not read by Sunshine |
| `x-nv-video[0].timeoutLengthMs` | `7000` | not read |
| `x-nv-video[0].framesWithInvalidRefThreshold` | `0` | not read |
| `x-nv-video[0].initialBitrateKbps`, `initialPeakBitrateKbps`, `x-nv-vqos[0].bw.minimumBitrateKbps`, `x-nv-vqos[0].bw.maximumBitrateKbps` | `0.8 × bitrate` (−500 if remote), capped at 100000 (`:337-367`) | Sunshine reads **only** `maximumBitrateKbps` (`rtsp.cpp:1148`) |
| `x-ml-video.configuredBitrateKbps` | full user bitrate, Sunshine only (`:369-373`) | if non-zero, Sunshine computes the encoder bitrate itself from host FEC % and audio overhead (`rtsp.cpp:1202-1226`) |
| `x-nv-vqos[0].fec.enable` | `1` (`:386-387`) | not read |
| `x-nv-vqos[0].videoQualityScoreUpdateTime` | `5000` | not read |
| `x-nv-vqos[0].qosTrafficType` / `x-nv-aqos.qosTrafficType` | `5`/`4` local, `0`/`0` remote (`:400-407`) | non-zero enables DSCP tagging (`rtsp.cpp:1108-1109`; `stream.cpp:2160,2187`) |
| `x-nv-general.featureFlags` | `NVFF_BASE 0x07 \| NVFF_RI_ENCRYPTION 0x80` = `135`, plus `NVFF_AUDIO_ENCRYPTION 0x20` (→ `167`) only if audio encryption is enabled (`SdpGenerator.c:177-201`) | **bit `0x20` forces `SS_ENC_AUDIO` on** (legacy path, `rtsp.cpp:1112-1115`) |
| `x-nv-general.useReliableUdp` | `13` for 7.1.431+ (`:203-205`) | `controlProtocolType`. **13 = encrypted control stream**; any other value means plaintext control (`stream.cpp:589-591, 746-749`) |
| `x-nv-vqos[0].fec.minRequiredFecPackets` | `2` (`:207-211`) | minimum parity shards per FEC block; raises FEC % for small frames (`rtsp.cpp:1106`; `stream.cpp:867-873`) |
| `x-nv-vqos[0].bllFec.enable` | `0` (`:218`) | not read (GFE-only) |
| `x-nv-vqos[0].drc.enable` | `0` (or `1` + `drc.tableType 2` for tiny resolutions on GFE 3.26) (`:234-247`) | not read |
| `x-nv-general.enableRecoveryMode` | `0` (`:251`) | not read |
| `x-nv-video[0].videoEncoderSlicesPerFrame` | `CAPABILITY_SLICES_PER_FRAME` from decoder caps, default 1 (`:422-432`) | `slicesPerFrame` (`rtsp.cpp:1149`) |
| `x-nv-vqos[0].bitStreamFormat` | `0` H.264, `1` HEVC, `2` AV1 (`:434-452`) | `videoFormat` 0/1/2 (`video.h:42`). Requesting HEVC/AV1 while the mode is disabled → 400 (`rtsp.cpp:1228-1240`) |
| `x-nv-clientSupportHevc` | `1` for HEVC, `0` for H.264; **omitted for AV1** (`:437-451`) | **not read by Sunshine** |
| `x-nv-video[0].dynamicRangeMode` | `1` if the negotiated format is 10-bit, else `0` (`:454-461`) | `dynamicRange`: 0 = 8-bit, 1 = 10-bit (`video.h:43`). Combined with host display HDR it selects PQ (section 10) |
| `x-nv-video[0].maxNumReferenceFrames` | `0` (unlimited) if the decoder supports RFI, else `1` (`:463-475`) | `numRefFrames` (`rtsp.cpp:1150`) |
| `x-nv-video[0].clientRefreshRateX100` | e.g. 5994 (`:477-478`) | discarded unless within 1% of maxFPS (`rtsp.cpp:1137-1147`) |
| `x-nv-audio.surround.numChannels` / `channelMask` / `enable` | channel count / mask / `1` if > 2 channels (`:481-490`) | `audio.channels`, `audio.mask` (`rtsp.cpp:1097-1098`) |
| `x-nv-audio.surround.AudioQuality` | `1` only for HQ surround (`:493-508`) | HIGH_QUALITY flag (`rtsp.cpp:1101-1102`). For stereo, overridden by the `Host` header rule (section 3) |
| `x-nv-aqos.packetDuration` | `5` (default) or `10` (slow decoder / low bitrate) (`:509-523`) | Opus frame duration in ms (`rtsp.cpp:1099`) |
| `x-nv-video[0].encoderCscMode` | `(colorSpace << 1) \| colorRange`, where colorSpace 0 = 601, 1 = 709, 2 = 2020 and range 0 = limited, 1 = full (`:533-536`; `Limelight.h:22-30`) | SDR colorspace and range; HDR ignores the colorspace bits (`video_colorspace.cpp:49-105`) |

**FEC percentage is not client-selectable with Sunshine.** It is the host config `fec_percentage` (default 20,
`src/config.cpp:842, 1828`). `x-nv-vqos[0].fec.repairPercent` is sent only to old GFE (`SdpGenerator.c:225-231`) and
Sunshine never reads it.

If mandatory encryption applies and VIDEO|AUDIO are not both enabled, ANNOUNCE fails with 403 (`src/rtsp.cpp:1242-1249`).

---

## 5. Video packet layout on the wire

**Per-datagram layout without video encryption** (`video_packet_raw_t`, `src/stream.cpp:140-154 @ sun@d0b4a88`;
`src/Video.h:27-48 @ mcc@41cedb8`):

| Offset | Size | Field | Endianness / notes |
|---|---|---|---|
| 0 | 1 | RTP `header` | Sunshine sets `0x80 \| FLAG_EXTENSION(0x10)` = `0x90` (`stream.cpp:1754`; `Video.h:37`) |
| 1 | 1 | RTP `packetType` | 0 (never set for video) |
| 2 | 2 | RTP `sequenceNumber` | **BE**. Contiguous across data and parity shards of a frame: `lowseq + x` (`stream.cpp:1755`) |
| 4 | 4 | RTP `timestamp` | **BE**, 90 kHz, from the capture timestamp (`stream.cpp:1742-1743,1756`) |
| 8 | 4 | RTP `ssrc` | 0 |
| 12 | 4 | reserved ("extension") | mcc skips 4 bytes when `header & 0x10` (`RtpVideoQueue.c:553-556`). `MAX_RTP_HEADER_SIZE = 16` (`Video.h:39-40`) |
| 16 | 4 | `streamPacketIndex` | **LE**. Sunshine writes `(lowseq + x) << 8`, so the index sits in the **upper 24 bits** (`stream.cpp:1699`). mcc does `>>= 8; &= 0xFFFFFF` (`VideoDepacketizer.c:758-760`) |
| 20 | 4 | `frameIndex` | **LE** (`RtpVideoQueue.c:566`; `stream.cpp:1698,1759`) |
| 24 | 1 | `flags` | `FLAG_CONTAINS_PIC_DATA 0x1`, `FLAG_EOF 0x2`, `FLAG_SOF 0x4` (`Video.h:21-23`) |
| 25 | 1 | `extraFlags` | `NV_VIDEO_PACKET_EXTRA_FLAG_LTR_FRAME 0x1` (`Video.h:25`) |
| 26 | 1 | `multiFecFlags` | Sunshine writes `0x10` (`stream.cpp:1702`) |
| 27 | 1 | `multiFecBlocks` | bits 4-5 = current FEC block index, bits 6-7 = last block index (`(blockIndex << 4) \| ((blocks-1) << 6)`, `stream.cpp:1703,1758`; parsed `RtpVideoQueue.c:585,715`) |
| 28 | 4 | `fecInfo` | **LE**. Bits 22-31 = data shard count in this FEC block, bits 12-21 = this shard's index in the block, bits 4-11 = FEC percentage (`stream.cpp:1749-1752`; parsed `RtpVideoQueue.c:584,709-710`) |
| 32 | … | payload | `packetSize − 16` bytes, except the last packet of a block, which may be shorter |

Total datagram = `packetSize + 16`. Sunshine's FEC `blocksize = packetsize + MAX_RTP_HEADER_SIZE` (`stream.cpp:1614`).

**SOF/EOF semantics with multi-block FEC.** Sunshine sets `FLAG_CONTAINS_PIC_DATA` on every data shard, `FLAG_SOF` on
the **first packet of every FEC block**, and `FLAG_EOF` on the **last data packet of every FEC block**
(`stream.cpp:1692-1712`). A frame is split into up to **4** FEC blocks (2-bit field, `MAX_FEC_BLOCKS = 4`,
`stream.cpp:1620-1642`). The number of blocks is `ceil(payload / (D_max × blocksize))` with
`D_max = 255·100 / (100 + fec%)` (`:1623-1634`). So mcc treats a packet as the frame's first only if
`(flags == SOF or SOF|EOF) and block == 0` (`VideoDepacketizer.c:740-741`), and as the frame's last only if
`EOF and block == lastBlock` (`:772`).

**Parity vs data shards.** A shard is **parity iff `fecIndex >= dataShards`**, where
`fecIndex = (fecInfo >> 12) & 0x3FF` and `dataShards = fecInfo >> 22`. Equivalently, its RTP seq is at or beyond
`lowestSeq + dataShards`, with `lowestSeq = seq − fecIndex` (`RtpVideoQueue.c:701,709-713,739`).
**Do not use `flags` to recognise parity.** After RS encoding, Sunshine overwrites only `rtp.header`, `sequenceNumber`,
`timestamp`, `fecInfo`, `multiFecBlocks` and `frameIndex` on every shard (`stream.cpp:1746-1759`). In parity shards
the `flags`, `streamPacketIndex`, `extraFlags` and `multiFecFlags` bytes are RS parity output, i.e. arbitrary values.
INFERRED from the encode order in `stream.cpp:1692-1760`; mcc never inspects parity-shard flags.

**Video encryption** (only if `SS_ENC_VIDEO` is in `x-ss-general.encryptionEnabled`; `src/stream.cpp:2348-2355`):
- Each datagram = 32-byte prefix + AES-128-GCM ciphertext of the **whole `blocksize` shard** (RTP header through payload).
  Prefix (`ENC_VIDEO_HEADER`, `src/Video.h:12-19 @ mcc`; `video_packet_enc_prefix_t`, `src/stream.cpp:159-166, 328-330`):
  `u8 iv[12]`, `u32 frameNumber` (plaintext, LE, used for cheap early discard), `u8 tag[16]`.
- IV = 12 bytes: `[0..7]` = 64-bit per-session counter in LE (starts at 0, +1 per encrypted shard), `[8..10] = 0`,
  `[11] = 'V'` (`stream.cpp:1551, 1761-1779, 2354`). The client takes the IV from the prefix and does not derive it
  (`src/VideoStream.c:185-222 @ mcc`). Key = `rikey`.
- Client receive size = `packetSize + 16 + 32` (`VideoStream.c:95-98`). mcc lowers `packetSize` by 32 in ANNOUNCE so
  that the total datagram size stays the same (`SdpGenerator.c:323-328`).

**Video UDP ping (client → host video port).** `SS_PING` = `char payload[16]` (the `X-SS-Ping-Payload` string) +
`u32 sequenceNumber` **BE**, counting from 1. Sent every **500 ms** for the whole session. If no payload is known,
the legacy 4-byte `"PING"` (`0x50 0x49 0x4E 0x47`) is sent instead (`src/Video.h:50-54`; `src/VideoStream.c:55-82`).
Sunshine treats a 4-byte datagram as a legacy ping, matched by source IP, and a datagram ≥ 20 bytes as a v2 ping,
matched by its first 16 bytes (`src/stream.cpp:1500-1516`). The first matching ping fixes the destination IP:port
for video (`recv_ping`, `:2086-2137`). The ping must arrive within `ping_timeout` (default 10 s,
`src/config.cpp:838`) of ANNOUNCE, otherwise the video thread exits and stops the session (`:2144-2156`).
After that first ping, liveness is tracked through the ENet control stream, not UDP pings (section 9).

---

## 6. Per-frame header (Sunshine short frame header)

At the start of the **first data shard of block 0** of each frame, prepended to the encoded bitstream
(`video_short_frame_header_t`, `src/stream.cpp:101-135, 1588-1609 @ sun@d0b4a88`):

| Offset (from payload start, i.e. byte 32 of the datagram) | Size | Field |
|---|---|---|
| 0 | 1 | `headerType` = `0x01` (short 8-byte header) |
| 1 | 2 | `frame_processing_latency`, **LE u16, units of 1/10 ms**. `(µs + 50) / 100` clamped to u16; **0 for repeated/duplicate frames** or when no capture timestamp exists |
| 3 | 1 | `frameType`: Sunshine emits **2 = IDR**, **5 = P-frame after reference-frame invalidation**, **1 = normal P-frame** (`:1590-1592`). The struct comment also lists **4 = P-frame with intra-refresh blocks**, but the code never emits 4. mcc additionally accepts `104` ("Sunshine hardcoded header") (`VideoDepacketizer.c:859-887`) |
| 4 | 2 | `lastPayloadLen`, **LE u16** = `(frameBytes + 8) % (packetsize − 16)`, or `packetsize − 16` if that is 0 (`:1593-1596`). This is the payload length of the frame's final packet, counted after the 16-byte NV header and including the 8-byte frame header when the frame is a single packet |
| 6 | 2 | `unknown` (zero) |

Total header length = **8 bytes**. mcc picks the header size from byte 0 and the host version
(`VideoDepacketizer.c:915-966`). Sunshine reports appversion 7.1.431, so mcc takes the `[7.1.415, 7.1.446)` branch:
`0x01` → 8 bytes, `0x81` → 24 bytes (`:939-950`). In every 7.1.415+ branch, a `0x01` header is 8 bytes.

**How mcc surfaces host latency**: it reads bytes 1-2 LE when `IS_SUNSHINE()` (`VideoDepacketizer.c:898-904`) into
`DECODE_UNIT.frameHostProcessingLatency`, documented as "1/10 ms units, zero when not provided or the frame is
repeated" (`src/Limelight.h:149-155 @ mcc`; copied at `VideoDepacketizer.c:486-488`).

**IDR detection**: for H.264/HEVC, mcc parses NAL types to find IDR frames. For other codecs (AV1) it trusts
`frameType == 2` (`VideoDepacketizer.c:862-869`). `lastPayloadLen` is used to trim only for non-H.264/HEVC codecs.
H.264/HEVC tolerate trailing zero padding (`:906-913, 1028-1066`). After the 8-byte header, mcc expects an
Annex-B start code for H.264/HEVC and strips a leading AUD and SEI NALs (`:975-999`).

---

## 7. FEC (video)

- **Code**: Reed-Solomon erasure code from **nanors** over **GF(2⁸)**. Both ends compile nanors `rs.c`
  (Sunshine: `cmake/compile_definitions/common.cmake:138-140 @ sun@d0b4a88`). GF(2⁸) evidence: the API works on
  `uint8_t` symbols and coefficients (`rs.h:14-34 @ nanors@b1e3c22`), the tree ships `deps/obl/gf2_8_tables.h`,
  and newer nanors renames the shard limit `RS8_DATA_SHARDS_MAX 255` (`rs.h:8 @ nanors@a9b1636`).
  `DATA_SHARDS_MAX 255` (`rs.h:8 @ nanors@b1e3c22`). Sunshine also keeps data + parity ≤ 255 per block
  (comment "D = 255 - P", `stream.cpp:1623-1630`).
- **Shard size** = `blocksize = packetSize + 16`: the full RTP datagram, before any encryption prefix
  (`stream.cpp:1614`).
- **Data shards**: the byte stream `[8-byte frame header ‖ encoded frame]` is cut into `packetSize − 16`-byte slices (`payload_blocksize = blocksize - sizeof(video_packet_raw_t)`, `src/stream.cpp:1614-1616 @ sun@d0b4a88`).
  Each slice is placed after a 32-byte header region (`concat_and_insert`, `stream.cpp:946-982, 1613-1618`). The
  stream is then divided into blocksize-aligned FEC blocks (`:1650-1669`). The last data shard of a block is
  zero-padded to blocksize (`fec::encode`, `:858-906`).
- **Parity count** per block = `ceil(D × pct / 100)`. If that is below `minRequiredFecPackets`, parity is raised to
  that minimum and `pct` is rewritten as `100 × P / D` (`:865-873`). The client recomputes
  `P = (D × pct + 99) / 100` from `fecInfo` (`RtpVideoQueue.c:711`). If the frame would need more than 4 blocks,
  FEC is turned off for that frame (`pct = 0`) (`stream.cpp:1636-1642`).
- **Reconstruction (mcc)**: shards are indexed by `seq − lowestSeq` (data `0..D-1`, parity `D..D+P-1`). Missing
  shards are marked, and `reed_solomon_decode(rs, shards, marks, D+P, packetSize+16)` runs once ≥ D shards of the
  block are present (`RtpVideoQueue.c:195-464`, especially `:269, 281, 291-332`). Received shards shorter than
  blocksize are zero-padded first (`:313-316`). Afterwards mcc **rewrites** `sequenceNumber`, RTP `header`,
  `timestamp`, `ssrc`, `frameIndex` and `multiFecBlocks` in recovered data shards (`:350-369`), because Sunshine
  changed those fields after encoding (`stream.cpp:1746-1759`). INFERRED: this works because RS erasure decoding is
  per byte column, so the post-encode header edits corrupt only those header columns in recovered shards.
- **Ignoring parity on a lossless link: yes, it works.** mcc itself finishes a block without any RS work once
  `receivedDataPackets == bufferDataPackets` (`RtpVideoQueue.c:248-252`). Data shards carry the full bitstream in
  order. Parity-ignoring clients must still (a) drop parity by the `fecIndex >= dataShards` test, not by flags, and
  (b) concatenate all FEC blocks 0..last of a frame before treating the frame as complete (section 5).
- **Loss feedback (Sunshine extension)**: mcc sends `SS_FRAME_FEC_STATUS` (type `0x5502`, **big-endian** fields;
  `src/Video.h:56-70 @ mcc`) as unsequenced packets (`ControlStream.c:1411-1434`). Sunshine at d0b4a88 registers
  **no handler** for client→host `0x5502` (handler list `src/stream.cpp:1178-1315`), so it is ignored. Note that
  `0x5502` is also the host→client "Set RGB LED" type; the two directions are distinct.

---

## 8. Audio stream

- **Port**: from the SETUP `streamid=audio` `server_port`, default 48000 = base+11 (`src/RtspConnection.c:1190-1200 @ mcc`;
  `src/stream.cpp:1976 @ sun`).
- **Ping/keepalive**: same `SS_PING` format as video (16-byte payload + BE32 seq from 1), every **500 ms**, or the
  legacy `"PING"` (`src/AudioStream.c:38-64 @ mcc`). It starts right after SETUP audio (`:90-110`). Sunshine's audio
  thread waits up to `ping_timeout` after ANNOUNCE for the first matching ping (`src/stream.cpp:2171-2183`).
- **RTP layout**: 12-byte RTP header with no extension: `header 0x80`, `packetType 97`, `sequenceNumber` BE
  (from 0, +1 per packet), `timestamp` BE, `ssrc 0` (`src/stream.cpp:1879-1881, 1910-1914, 2386-2387`). **The
  timestamp advances by `packetDuration` per packet, so its units are milliseconds, not 48 kHz samples**
  (`stream.cpp:1914`). The payload follows directly. Max client packet size is 1400 (`AudioStream.c:26`;
  `stream.cpp:345`).
- **Payload types**: audio data `97`, FEC `127` (`src/RtpAudioQueue.c:21 @ mcc`; `src/stream.cpp:1880, 2372`).
- **Opus parameters** (`src/audio.cpp:45-138, 298-309 @ sun@d0b4a88`): sample rate 48000. Multistream encoder,
  `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, **CBR** (`OPUS_SET_VBR(0)`), frame size = `packetDuration × 48` samples
  (5 ms → 240, 10 ms → 480; client `samplesPerFrame = 48 × AudioPacketDuration`, `AudioStream.c:440`).
  Layouts and bitrates (`audio.cpp:52-101`):

  | Config | Channels / streams / coupled | Bitrate |
  |---|---|---|
  | stereo | 2/1/1 | 96 kbps |
  | stereo HQ | 2/1/1 | 512 kbps |
  | 5.1 | 6/4/2 | 256 kbps |
  | 5.1 HQ | 6/6/0 | 1536 kbps |
  | 7.1 | 8/5/3 | 450 kbps |
  | 7.1 HQ | 8/8/0 | 2048 kbps |

  Stereo needs no surround-params. mcc hardcodes stereo as 1 stream, 1 coupled, mapping {0,1}
  (`RtspConnection.c:756-763`). `packetDuration` is negotiated via `x-nv-aqos.packetDuration` (section 4).
- **Audio encryption**: enabled if `SS_ENC_AUDIO` is in `encryptionEnabled` **or** legacy
  `x-nv-general.featureFlags & 0x20` (`src/rtsp.cpp:1110-1115`). Cipher **AES-128-CBC with PKCS#7 padding**,
  key = `rikey` (`cbc_t{gcm_key, true}`, `src/stream.cpp:2379-2382`; client `ALGORITHM_AES_CBC` → PKCS7 in
  `src/PlatformCrypto.c:99-100 @ mcc`). **IV = 16 bytes: `[0..3] = BE32(rikeyid + rtpSequenceNumber)` (mod 2³²),
  `[4..15] = 0`** (`src/stream.cpp:1898, 2385`; `src/AudioStream.c:178-201 @ mcc`, which explains: "The IV is the
  avkeyid (equivalent to the rikeyid) + the RTP sequence number, in big endian").
- **Audio FEC**: RS with **4 data + 2 parity** shards (`RTPA_DATA_SHARDS 4`, `RTPA_FEC_SHARDS 2`,
  `src/RtpAudioQueue.h:11-13 @ mcc`). Both sides **overwrite the RS parity matrix** with the fixed bytes
  `{0x77,0x40,0x38,0x0e,0xc7,0xa7,0x0d,0x6c}` (`src/RtpAudioQueue.c:48-60 @ mcc`; `src/stream.cpp:1871-1877 @ sun`).
  Blocks are aligned to `seq % 4 == 0`. After the 4th data packet, 2 FEC packets are sent with RTP seq =
  `lastDataSeq + 1 + x`, PT 127, timestamp 0. A 12-byte `AUDIO_FEC_HEADER` follows the RTP header:
  `u8 fecShardIndex (0..1)`, `u8 payloadType (=97)`, `u16 baseSequenceNumber BE`, `u32 baseTimestamp BE`, `u32 ssrc (0)`
  (`src/RtpAudioQueue.h:18-24 @ mcc`; `src/stream.cpp:1930-1958, 2371-2377`). The shard bytes are the (encrypted, if
  enabled) payloads. Sunshine RS-encodes with the length of the block's 4th packet (`stream.cpp:1939`). This relies on
  CBR Opus producing equal-size packets; that is INFERRED, not stated in the source.
- **Distinguishing FEC packets**: by RTP payload type (97 data vs 127 FEC) (`RtpAudioQueue.c:197-285`). A client that
  ignores PT 127 still gets every Opus packet on a lossless link.

---

## 9. Control stream messages (inside ENet; transport in section 11)

**Encrypted envelope** (used when `useReliableUdp = 13`; `NVCTL_ENCRYPTED_PACKET_HEADER`, `src/ControlStream.c:25-32 @ mcc`;
`control_encrypted_t`, `src/stream.cpp:292-309 @ sun`). One ENet packet contains:

```
u16 LE encryptedHeaderType = 0x0001
u16 LE length               = 4 (seq) + 16 (tag) + ciphertextLen
u32 LE seq
u8[16] AES-GCM tag
ciphertext = AES-128-GCM( u16 LE type ‖ u16 LE payloadLength ‖ payload )   // NVCTL_ENET_PACKET_HEADER_V2
```

(`src/ControlStream.c:548-591, 692-737 @ mcc`; tag-then-ciphertext layout `src/crypto.cpp:215-218 @ sun`;
`encode_control` `src/stream.cpp:583-631`.) There is no AAD. Key = `rikey`.
- **IV, V2** (`SS_ENC_CONTROL_V2` negotiated): 12 bytes, `[0..3] = seq` LE, `[4..9] = 0`, `[10] = 'C'` for
  client→host or `'H'` for host→client, `[11] = 'C'` (`ControlStream.c:554-566, 617-629`; `stream.cpp:596-608, 1269-1281`).
- **IV, legacy** (V2 not negotiated): 16 bytes, `iv[0] = (u8)seq`, rest 0 (`ControlStream.c:568-574`; `stream.cpp:609-614, 1282-1287`).
- **Sequence numbers**: each direction has its own counter starting at 0 and incrementing per message (client
  `currentEnetSequenceNumber`, `ControlStream.c:353, 721`; host `session->control.seq++`, `stream.cpp:593`).
  mcc does not check host seq ordering ("seq is accepted as an arbitrary value in Moonlight", `stream.cpp:296`).
  mcc checks `length` against the packet size and rejects runts below `4 + 16 + 4` (`ControlStream.c:603-615`).
- **Failure handling**: Sunshine **stops the session** on a GCM tag failure (`stream.cpp:1290-1296`) and drops
  unencrypted packets on an encrypted stream (`:746-749`). mcc drops unencrypted host packets (`ControlStream.c:1259-1263`).
- Sunshine reads the inner `type` from plaintext bytes 0-1 and skips 4 bytes (type + payloadLength) to reach the
  payload. For `0x0206` input it passes everything after those 4 bytes to the input parser (`stream.cpp:1299-1314`).
  mcc converts V2→V1 by dropping the length field (`ControlStream.c:657-660`).

**Message type IDs** (encrypted Gen7: `packetTypesGen7Enc`, `src/ControlStream.c:203-217 @ mcc`; Sunshine `packetTypes`,
`src/stream.cpp:41-75 @ sun`):

| Dir | Type | Name | Payload (after V2 header) | Citations |
|---|---|---|---|---|
| C→H | `0x0302` | START A slot = **Request IDR frame** | 2 bytes `{0,0}` | `ControlStream.c:204, 228, 1881-1913, 1535-1544`; Sunshine raises an IDR event (`stream.cpp:1206-1210`) |
| C→H | `0x0307` | START B | 1 byte `{0}` | `ControlStream.c:205, 226, 1915-1947`; Sunshine no-op (`stream.cpp:1186-1188`) |
| C→H | `0x0200` | **Periodic ping** | 8 bytes LE `{u32 4, u32 0}`, every **100 ms**, reliable, channel 0 | `ControlStream.c:297-298, 1403-1455`; Sunshine no-op handler (`stream.cpp:1178-1180`). Any received ENet event refreshes `pingTimeout` (`stream.cpp:776`) |
| C→H | `0x0301` | Invalidate reference frames (RFI) | `SS_RFI_REQUEST` 24 bytes LE `{u32 first, u32 0, u32 last, u32 0[3]}`. Sunshine reads `int64[0]`, `int64[1]` | `Video.h:79-86`; `ControlStream.c:1550-1572`; `stream.cpp:1212-1223` |
| C→H | `0x0350` | LTR frame ACK (Sunshine ext.) | `{u32 frameIndex LE, u32 0}` | `Video.h:72-77`; `ControlStream.c:1574-1592`. **No Sunshine handler** at d0b4a88 |
| C→H | `0x5502` | Frame FEC status (Sunshine ext.) | `SS_FRAME_FEC_STATUS`, BE | section 7. **No Sunshine handler** |
| C→H | `0x0206` | Input data | `NV_INPUT_HEADER` + input packet (section 12) | `ControlStream.c:1705-1714`; `stream.cpp:1309-1311` |
| C→H | `0x0201` | Loss stats (legacy, non-Sunshine path) | 32 bytes | `ControlStream.c:1457-1499`; Sunshine logs it (`stream.cpp:1190-1204`) |
| H→C | `0x0109` | **Termination (extended)** | `u32` **BE** HRESULT. Sunshine sends `0x80030023` (graceful) at shutdown | `stream.cpp:195-199, 1398-1428`; mcc `ControlStream.c:1311-1346` |
| H→C | `0x010b` | Rumble | `u32 filler (0xC0FFEE)`, `u16 id`, `u16 lowFreq`, `u16 highFreq`, all LE | `stream.cpp:204-212, 1023-1039`; mcc skips 4 bytes, then reads 3×u16 LE (`ControlStream.c:1058-1066`) |
| H→C | `0x010e` | **HDR mode** | see below | `stream.cpp:280-287, 1143-1170`; `ControlStream.c:1279-1305` |
| H→C | `0x5500` | Rumble triggers | `u16 id, u16 left, u16 right` LE | `stream.cpp:217-223, 1040-1055`; `ControlStream.c:1067-1073` |
| H→C | `0x5501` | Set motion event state | `u16 id, u16 reportRateHz, u8 motionType` (1 = accel, 2 = gyro) | `stream.cpp:228-234, 1056-1071`; `ControlStream.c:1074-1080`; `Limelight.h:821-822` |
| H→C | `0x5502` | Set RGB LED | `u16 id, u8 r, u8 g, u8 b` | `stream.cpp:239-246`; `ControlStream.c:1081-1088` |
| H→C | `0x5503` | DualSense adaptive triggers | `u16 id, u8 eventFlags (0x04 R, 0x08 L), u8 typeLeft, u8 typeRight, u8 left[10], u8 right[10]` | `stream.cpp:262-275, 331-333`; `ControlStream.c:1092-1101`; `Limelight.h:478-480` |
| H→C | `0x5504` | Set player indicator LEDs (Sunshine-only) | `u16 id, u8 solid, u8 flashing` | `stream.cpp:75, 251-257, 1089-1104`. **Not in mcc@41cedb8 `packetTypesGen7Enc`**, so mcc ignores it |

The short (non-extended) termination `0x0100`, with u16 LE reason `0x0100` = SERVER_TERMINATED_INTENDED, is the
non-encrypted Gen7 variant (`ControlStream.c:197, 1347-1372`). It is not used by Sunshine with `useReliableUdp=13`.

**HDR mode message `0x010e`** payload (27 bytes; `control_hdr_mode_t` = header + `u8 enabled` + `SS_HDR_METADATA`;
`src/stream.cpp:280-287`; `SS_HDR_METADATA` `src/Limelight.h:981-1002 @ mcc`). All `u16` are **LE**. mcc reads LE
(`ControlStream.c:1283-1301`). Sunshine copies the native struct without byte-swapping, which is LE on supported
hosts (`stream.cpp:1154-1155`).

| Offset | Field | Units |
|---|---|---|
| 0 | `u8 enabled` | non-zero means the host display is in HDR and the stream colorspace is BT.2020 PQ |
| 1 | `displayPrimaries[3]{u16 x, u16 y}` (RGB order) | normalized to 50,000 |
| 13 | `whitePoint{u16 x, u16 y}` | normalized to 50,000 |
| 17 | `u16 maxDisplayLuminance` | nits |
| 19 | `u16 minDisplayLuminance` | 1/10000 nit |
| 21 | `u16 maxContentLightLevel` (MaxCLL) | nits |
| 23 | `u16 maxFrameAverageLightLevel` (MaxFALL) | nits |
| 25 | `u16 maxFullFrameLuminance` | nits |

mcc reads the metadata only when `IS_SUNSHINE()`, then sets `hdrEnabled = enabled != 0` and calls `setHdrMode`
asynchronously (`ControlStream.c:994-1009, 1279-1305`). It exposes the state via `LiGetCurrentHostDisplayHdrMode()` and
`LiGetHdrMetadata()` (`:2087-2098`).

**Termination reason normalization (mcc)** (`ControlStream.c:1317-1372`): `0x800e9403` →
`ML_ERROR_FRAME_CONVERSION (-104)`; `0x800e9302` → `ML_ERROR_PROTECTED_CONTENT (-103)`; `0x80030023` →
`ML_ERROR_GRACEFUL_TERMINATION (0)` if any frame was seen, else `ML_ERROR_UNEXPECTED_EARLY_TERMINATION (-102)`.
Other values pass through (`Limelight.h:413-440`). On termination mcc calls `enet_peer_disconnect_now` immediately (`:1374-1385`).

**Liveness**: Sunshine stops a session when `now > pingTimeout`. `pingTimeout` starts at session start and is refreshed
only by ENet events for that session (`stream.cpp:776, 1342-1346, 2302`). Default timeout 10 s (`config.cpp:838`).
**Without an ENet control connection, the stream is torn down about 10 s after ANNOUNCE.** HDR and rumble messages are
dropped until the ENet peer exists ("still waiting for PING from Moonlight", `stream.cpp:1016-1020, 1144-1148`).

**IDR / RFI**: `LiRequestIdrFrame()` flushes pending RFI and sends `0x0302` on channel `CTRL_CHANNEL_URGENT` (reliable)
(`ControlStream.c:415-422, 1502-1548, 1636-1652`). RFI is used only when the host advertises `refPicInvalidation` and
the decoder supports it (`ControlStream.c:386-412`).

**Connection-quality heuristic (client-only)**: 3 s windows. 30% frame loss marks the connection POOR immediately;
two consecutive windows at ≥15% also mark it POOR; ≤5% returns it to OKAY (`ControlStream.c:125-128, 471-515`).

---

## 10. HDR signalling end to end

To get **HEVC Main10 or AV1 Main10 with PQ**:

1. **serverinfo**: require `ServerCodecModeSupport & SCM_HEVC_MAIN10 (0x200)` or `& SCM_AV1_MAIN10 (0x20000)`
   (section 1). The applist `IsHdrSupported` reflects `hevc_mode >= 3` only (`src/nvhttp.cpp:1353`).
2. **/launch**: send `hdrMode=1`. moonlight-qt also sends the `clientHdrCap*` set, which Sunshine ignores (section 2).
   `hdrMode=1` asks Sunshine to switch the host display into HDR when the display-device option is "automatic"
   (`src/display_device.cpp:525-536`). Without host-display HDR there is no PQ (step 5).
3. **DESCRIBE**: confirm the codec is advertised (`sprop-parameter-sets=AAAAAU` for HEVC, `AV1/90000` for AV1)
   (`src/rtsp.cpp:904-910`).
4. **ANNOUNCE**: `x-nv-vqos[0].bitStreamFormat:1` (HEVC) or `:2` (AV1), and `x-nv-video[0].dynamicRangeMode:1`
   (`src/SdpGenerator.c:434-461 @ mcc`). Optionally `x-ss-video[0].chromaSamplingType:1` for 4:4:4 10-bit
   (REXT10 / AV1 High). `encoderCscMode` still sets range and SDR colorspace; mcc sends Rec.2020 = colorSpace 2
   when the client asks for it (`Limelight.h:22-26`). Sunshine ignores `x-nv-clientSupportHevc`.
5. **Sunshine's decision** (`src/video_colorspace.cpp:49-105 @ sun`):
   - If `dynamicRange > 0` **and** the captured display reports HDR (`disp.is_hdr()`, `src/video.cpp:2585`), the
     colorspace is `bt2020` = Rec.2020 primaries + SMPTE ST 2084 (PQ) transfer + BT.2020-NCL matrix
     (`video_colorspace.cpp:139-146`). Otherwise it is the SDR colorspace from `encoderCscMode >> 1`.
   - Bit depth = 10 whenever `dynamicRange == 1`, **even if the host display is SDR** (10-bit SDR stream)
     (`:84-97`). If the encoder lacks 10-bit for the requested codec/chroma, `resolve_dynamic_range` forces
     `dynamicRange = 0` (SDR, 8-bit) (`src/video.cpp:170-187`).
   - Profiles: HEVC `MAIN_10` when `dynamicRange` (RExt for 4:4:4); AV1 Main (High for 4:4:4) covers both depths
     (`src/video.cpp:2032-2051`).
6. **Detecting HDR state (including mid-stream changes)**: every time Sunshine (re)creates the encode device it
   raises an `hdr_info` event, at session start and on each display reinit. `enabled = true` and metadata are
   filled only when the colorspace is PQ and the display returns HDR metadata (`src/video.cpp:2649-2658, 2934-2943`).
   The control thread forwards each event as message `0x010e` (`src/stream.cpp:1377-1382`). A client must keep the
   ENet control stream up and treat each `0x010e` as the authoritative HDR on/off + mastering metadata. mcc stamps
   every decode unit with `hdrActive = LiGetCurrentHostDisplayHdrMode()` and `colorspace = hdrActive ? REC_2020 :
   configured` (`src/VideoDepacketizer.c:498-499`). mcc notes this is not parsed from the bitstream
   (`Limelight.h:182-193`); an HEVC/AV1 bitstream parser (VUI/sequence header transfer characteristics) is the
   stronger check. moonlight-qt re-applies `LiGetCurrentHostDisplayHdrMode()` after decoder resets
   (`app/streaming/session.cpp:2252-2254 @ mqt`).
   - UNCONFIRMED: exactly which host events trigger a display reinit when the user toggles HDR mid-stream. That is
     platform capture-backend code (e.g. Windows DXGI duplication), which was not read. The protocol fact is that a
     new `0x010e` follows every encoder re-initialization.

---

## 11. ENet transport for the control stream

- **ENet is mandatory on modern hosts.** mcc uses ENet for the control stream whenever `AppVersionQuad[0] >= 5`
  (`src/ControlStream.c:1765-1851 @ mcc`). TCP port 47995 is used only for gen 3/4 GFE (`:1852-1863`). Sunshine's
  control server is **only** an ENet host (`control_server_t::bind` → `net::host_create`, `src/stream.cpp:395-399`;
  `src/network.cpp:208-226 @ sun`). A raw UDP datagram that does not parse as an ENet protocol header is processed by
  `enet_host_service` as ENet. With no matching peer or a bad session ID it is discarded (`protocol.c:1017-1050 @ enet`).
  It never reaches `control_server_t::call`. INFERRED from the ENet receive path: the current client's raw
  `0x0001` datagrams are dropped.
- **Fork vs upstream wire format: identical.** The fork's diff of `include/enet/protocol.h` against
  `enet-base@2a85cd6` only adds `ENET_PROTOCOL_MAXIMUM_MTU = 1400` under `__WIIU__`/`__3DS__`. All protocol structs
  (`protocol.h:69-195 @ enet@aca8784`) are upstream ENet 1.3.x (`ENET_VERSION 1.3.17`, `enet.h:26-28`). The
  `protocol.c` changes are behavioural only: multi-homed source-address handling, RTO/RTT rounding and caps, QoS
  disable after 3 lost connects, and wake-up timing. Compare `lsalzman/enet 2a85cd6...cgutman/enet aca8784`
  (73 commits ahead). One default differs: **`ENET_HOST_DEFAULT_MTU = 900`** (`enet.h:215`) vs upstream `1392`
  (fork commit `7546b505` "Lower ENet's MTU to 900…"). MTU is negotiated in CONNECT/VERIFY_CONNECT, so a standard
  ENet 1.3 implementation interoperates.
  - **Protocol header**: `u16 peerID` (network byte order). Bit 15 = `SENT_TIME` flag, bit 14 = `COMPRESSED`,
    bits 12-13 = session ID, bits 0-11 = peer ID (`0xFFF` = unassigned). An optional `u16 sentTime` follows if bit
    15 is set (`protocol.h:47-58, 69-73`; parse `protocol.c:1026-1031`).
  - **Checksum: none.** `host->checksum = NULL` by default (`host.c:91`), and neither mcc nor Sunshine sets it
    (searched both trees). When unset, no 4-byte checksum field is present (`protocol.c:1032-1033`).
  - **Compression: none.** The compressor is NULL by default (`host.c:109-112`), and neither side calls
    `enet_host_compress`.
  - **Command header** (4 bytes): `u8 command` (low nibble = command; bit 7 `ACKNOWLEDGE`, bit 6 `UNSEQUENCED`),
    `u8 channelID`, `u16 reliableSequenceNumber` BE (`protocol.h:27-50, 75-80`). Commands: ACK 1, CONNECT 2,
    VERIFY_CONNECT 3, DISCONNECT 4, PING 5, SEND_RELIABLE 6, SEND_UNRELIABLE 7, SEND_FRAGMENT 8,
    SEND_UNSEQUENCED 9, BANDWIDTH_LIMIT 10, THROTTLE_CONFIGURE 11, SEND_UNRELIABLE_FRAGMENT 12.
  - **CONNECT** (48 bytes, `channelID = 0xFF`, flag ACK): `outgoingPeerID u16, incomingSessionID u8,
    outgoingSessionID u8, mtu, windowSize, channelCount, incomingBandwidth, outgoingBandwidth,
    packetThrottleInterval, packetThrottleAcceleration, packetThrottleDeceleration, connectID, data` (all u32 BE
    except `connectID`, which is an opaque random value copied verbatim) (`protocol.h:89-105`; built at
    `host.c:247-261`). **VERIFY_CONNECT** = the same without `data` (44 bytes) (`protocol.h:107-122`).
    The receiver converts `data` with `ENET_NET_TO_HOST_32` and exposes it as `event.data` (`protocol.c:350`).
- **Peer count / channels / connect data**:
  - Client: `enet_host_create(family, local wildcard-port addr, peerCount = 1, channelLimit = CTRL_CHANNEL_COUNT (0x30), 0, 0)`,
    then `enet_host_connect(client, remote:ControlPort, channelCount = 0x30, data = ControlConnectData)`
    (`ControlStream.c:1771-1806`). It waits up to 10 s for CONNECT (`:1809`) and sets peer timeout
    `enet_peer_timeout(peer, 2, 10000, 10000)` (`:1849`). QoS is enabled before connect (`:1797`).
  - Host: `enet_host_create(af, bindaddr:base+10, 128 peers, channelLimit 0 (= protocol max 255), 0, 0)`, with QoS on
    (`src/network.cpp:219-223`).
  - **Connect `data`** = the decimal `X-SS-Connect-Data` value from SETUP control. Sunshine matches it against
    `session->control.connect_data` **if** the client advertised `ML_FF_SESSION_ID_V1` in
    `x-ml-general.featureFlags`. Otherwise it matches the ENet peer IP against the RTSP client IP
    (`src/stream.cpp:672-735`). Unmatched peers are disconnected (`:768-773`).
  - **Channel IDs** (`src/Limelight-internal.h:56-65 @ mcc`): `GENERIC 0x00` (START A/B, periodic ping, FEC status),
    `URGENT 0x01` (IDR, RFI, LTR ACK), `KEYBOARD 0x02`, `MOUSE 0x03`, `PEN 0x04`, `TOUCH 0x05`,
    `GAMEPAD_BASE 0x10`–`0x1F` (by controller index), `SENSOR_BASE 0x20`–`0x2F`, `COUNT 0x30`. mcc forces channel 0
    for GFE or when the channel is ≥ the peer's channel count (`ControlStream.c:761-765`).
  - **Reliability flags**: START A/B, IDR, RFI, LTR ACK and periodic ping use `ENET_PACKET_FLAG_RELIABLE` (`(1<<0)`).
    FEC status uses `ENET_PACKET_FLAG_UNSEQUENCED` (`(1<<1)`) (`ControlStream.c:1420-1450, 1522-1590, 1881-1921`;
    flags `enet.h:99-103`). Input flags vary by input type (section 12). GFE gets everything reliable
    (`ControlStream.c:698-701`). **Sunshine sends every host→client message reliable on channel 0**
    (`src/stream.cpp:451-459`).
- **The `0x0001` encrypted wrapper is the ENet packet payload.** mcc builds the whole
  `NVCTL_ENCRYPTED_PACKET_HEADER + tag + ciphertext` as one `ENetPacket`'s data and `enet_peer_send`s it
  (`ControlStream.c:703-768`). On the wire it is the data of a SEND_RELIABLE (or SEND_UNSEQUENCED, etc.) command.
  Sunshine reads the first 2 bytes of `event.packet->data` as the type (`stream.cpp:655-670`).
- **Ports**: the control port is **negotiated** via the SETUP `streamid=control/13/0` `Transport: server_port=`
  (`RtspConnection.c:1318-1328`). Sunshine returns `base+10` (`rtsp.cpp:979-980`), which is 47999 only with the
  default base port. 47999 is mcc's *fallback* when parsing fails. The client's local port is ephemeral
  (`ControlStream.c:1776`).
- **Keepalive**: ENet's own PING command every `ENET_PEER_PING_INTERVAL = 500 ms` when idle (`enet.h:232`), plus the
  application-level `0x0200` ping every 100 ms. Either keeps Sunshine's `pingTimeout` fresh, since any ENet event
  counts (`stream.cpp:776`).
- **Disconnect**: mcc disconnects gracefully with a 2 s linger (`CONTROL_STREAM_LINGER_TIMEOUT_SEC`,
  `ControlStream.c:145, 1685-1690`). Sunshine stops the session on `ENET_EVENT_TYPE_DISCONNECT` (`stream.cpp:791-797`).

---

## 12. Input packets sent over the control stream

Pinned sources: moonlight-common-c `41cedb8` (= 41cedb89d6b8592f99c09975c764ca3875034726), Sunshine `d0b4a88` (= d0b4a88ef727f8dabf80cc970a14739b55c885de). Sunshine compiles against moonlight-common-c's own `Input.h`/`Limelight.h` (`sun/src/input.cpp:6-9 @ d0b4a88`), so both sides share one set of struct and magic definitions.

### 12.1 How input is framed on modern (Gen 7 encrypted) hosts

From the outside in:

1. **ENet packet.** It goes on a per-device channel (12.2) with per-type reliability flags. The client sends it to the ENet peer on the negotiated control port (see sections 9 and 11).
2. **Encrypted control wrapper.** Fields: `encryptedHeaderType` u16 LE = `0x0001`, `length` u16 LE, `seq` u32 LE, then a 16-byte GCM tag, then the ciphertext (`mcc/src/ControlStream.c:26-32, 703-737 @ 41cedb8`). The IV and key rules are in section 9.
3. **Plaintext = `NVCTL_ENET_PACKET_HEADER_V2` plus payload.** The header is `type` u16 LE followed by `payloadLength` u16 LE (`ControlStream.c:20-23, 724-727 @ 41cedb8`). For input, `type` is `packetTypes[IDX_INPUT_DATA]` = `0x0206` on Gen7-encrypted hosts (`ControlStream.c:203-217` (0x0206 at line 209), `1705-1714 @ 41cedb8`; Sunshine table `sun/src/stream.cpp:58-76` (line 64) `@ d0b4a88`).
4. **Payload = one `NV_INPUT_HEADER` packet.** On an encrypted control stream the client passes the raw input packet to the control layer without further input-specific encryption (`mcc/src/InputStream.c:237-254 @ 41cedb8`). The older AES-CBC/GCM input wrapper with a BE32 length prefix applies only to pre-7.1.431 hosts (`InputStream.c:255-305 @ 41cedb8`).

How Sunshine receives it:
- It decrypts the wrapper and reads `type` as the first u16 of the plaintext.
- It skips 4 bytes (type plus payloadLength). It never checks the value of `payloadLength`; the field only has to be present.
- If `type == 0x0206`, it erases those 4 bytes and passes the rest straight to `input::passthrough` (`sun/src/stream.cpp:1299-1311 @ d0b4a88`).
- When the session uses control protocol 13 (encrypted), unencrypted messages are dropped (`stream.cpp:744-749 @ d0b4a88`).

### 12.2 ENet channel and reliability per input type

The channel IDs are defined in `mcc/src/Limelight-internal.h:56-65 @ 41cedb8`: GENERIC 0x00, URGENT 0x01, KEYBOARD 0x02, MOUSE 0x03, PEN 0x04, TOUCH 0x05, GAMEPAD_BASE 0x10 (0x10-0x1F by controller index), SENSOR_BASE 0x20 (0x20-0x2F by controller index), and CTRL_CHANNEL_COUNT 0x30.

| Input | Channel | ENet flags | Source (`InputStream.c @ 41cedb8`) |
|---|---|---|---|
| Enable haptics | 0x00 | RELIABLE | 673-674 |
| Keyboard, UTF-8 text | 0x02 | RELIABLE | 973-974, 1057-1058 |
| Mouse rel/abs/button/scroll/hscroll | 0x03 | RELIABLE | 773-777, 848-851, 931-932, 1303-1304, 1336-1337, 1389-1390 |
| Touch | 0x05 | 0 (unreliable) for HOVER/MOVE, else RELIABLE | 1429-1433, macro at 48 |
| Pen | 0x04 | 0 for HOVER/MOVE with unchanged buttons and tool, else RELIABLE | 1478-1486 |
| Multi-controller, arrival, battery | 0x10 + controllerNumber | RELIABLE | 1150-1153, 1538-1539, 1707-1708 |
| Controller touch | 0x10 + controllerNumber | 0 for HOVER/MOVE, else RELIABLE | 1583-1587 |
| Controller motion | 0x20 + controllerNumber | 0, but RELIABLE for an all-zero gyro sample | 1656, 549-558 |

Channel and flag rules:
- If the requested channel is at or above the peer's channel count, the client falls back to channel 0. GFE always gets channel 0 and reliable delivery (`ControlStream.c:698-701, 761-765 @ 41cedb8`).
- Sunshine does not inspect the receive channel. It sends its own host-to-client messages on channel 0, reliable (`sun/src/stream.cpp:452 @ d0b4a88`).

### 12.3 `NV_INPUT_HEADER` and endianness rules

- All structs are `#pragma pack(push, 1)` (`mcc/src/Input.h:5, 199 @ 41cedb8`).
- `NV_INPUT_HEADER` is `{ uint32 size; uint32 magic; }`:
  - `size` is **big-endian** and counts the bytes after itself (magic plus body).
  - `magic` is **little-endian** (`Input.h:11-14 @ 41cedb8`).
  - The client always sets `size = BE32(sizeof(packet) - 4)` (for example `InputStream.c:1182 @ 41cedb8`).
- `netfloat` is a 4-byte **little-endian IEEE-754 float** (`Input.h:7-9`; client conversion `InputStream.c:311-322 @ 41cedb8`; Sunshine decode `sun/src/input.cpp:150-152 @ d0b4a88`).
- Sunshine validation (`sun/src/input.cpp:1744-1794`, helper `1730-1735 @ d0b4a88`):
  - The packet must be at least 8 bytes.
  - It requires `4 <= BE32(size) <= len - 4`.
  - For every known fixed-size magic, `BE32(size)` must equal `sizeof(struct) - 4` exactly.
  - UTF-8 text body must be 32 bytes or less.
  - Unknown magics pass validation but have no dispatch case, so they are silently ignored (`input.cpp:2162-2207 @ d0b4a88`).
  - Malformed packets are dropped with a warning log (`input.cpp:2240-2253 @ d0b4a88`).
- Field endianness differs by packet type and is listed per packet below. In short:
  - Mouse and scroll fields are **big-endian**.
  - Keyboard keyCode, controller fields, Sunshine-extension integers, and netfloats are **little-endian**.

### 12.4 Magic values and GFE-generation differences

Defined in `mcc/src/Input.h @ 41cedb8` (line numbers are the `#define`s). Sunshine accepts only the Gen5+ / Sunshine values listed in its validation and dispatch (`sun/src/input.cpp:1760-1790, 2162-2205 @ d0b4a88`).

| Packet | Gen3/4 magic | Gen5+ magic (what Sunshine accepts) | Lines |
|---|---|---|---|
| Key down / up | 0x03 / 0x04 | 0x03 / 0x04 (same) | 22-23 |
| Mouse abs move | 0x05 | 0x05 (same) | 47 |
| Mouse rel move | 0x06 | **0x07** | 39-40 |
| Mouse button down / up | action 0x07 / 0x08 | **0x08 / 0x09** (action + 1) | 62-63; client +1 at `InputStream.c:934-938` |
| Scroll | 0x09 | **0x0A** | 111-112 |
| Legacy controller (Gen3 only) | 0x0A | n/a | 69 |
| Multi-controller | 0x0D | **0x0C** (Gen5 "decremented by one") | 87-88; `InputStream.c:1184-1190` |
| Enable haptics | 0x0D | 0x0D (Sunshine ignores it: no dispatch case) | 16 |
| UTF-8 text | 0x17 | 0x17 | 32 |
| SS hscroll | | 0x55000001 | 120 |
| SS touch | | 0x55000002 | 126 |
| SS pen | | 0x55000003 | 140 |
| SS controller arrival | | 0x55000004 | 157 |
| SS controller touch | | 0x55000005 | 166 |
| SS controller motion | | 0x55000006 | 179 |
| SS controller battery | | 0x55000007 | 190 |

The client picks the Gen5 magics when `AppVersionQuad[0] >= 5` (`InputStream.c:361-368, 780-785, 935-937, 1185-1190, 1307-1311 @ 41cedb8`). Sunshine reports appversion `7.1.431.-1` (`sun/src/nvhttp.h:38 @ d0b4a88`), so a client always uses the Gen5+ column with it.

### 12.5 Per-packet byte layouts

Offsets are from the start of `NV_INPUT_HEADER`. Every packet starts with `[0..4) size BE` and `[4..8) magic LE`.

**Multi-controller (gamepad).** `Input.h:87-109 @ 41cedb8`. Total 34 bytes, `size` = 30 (0x1E), magic 0x0C.

| Off | Type | Field | Value / meaning | Endian |
|---|---|---|---|---|
| 8 | i16 | headerB | `MC_HEADER_B` 0x001A | LE |
| 10 | i16 | controllerNumber | 0-15 on Sunshine (0-3 on GFE) | LE |
| 12 | i16 | activeGamepadMask | bit n = controller n present | LE |
| 14 | i16 | midB | `MC_MID_B` 0x0014 | LE |
| 16 | u16 | buttonFlags | low 16 bits of the button mask | LE |
| 18 | u8 | leftTrigger | 0-255 | |
| 19 | u8 | rightTrigger | 0-255 | |
| 20 | i16 | leftStickX | | LE |
| 22 | i16 | leftStickY | | LE |
| 24 | i16 | rightStickX | | LE |
| 26 | i16 | rightStickY | | LE |
| 28 | i16 | tailA | `MC_TAIL_A` 0x009C | LE |
| 30 | u16 | buttonFlags2 | high 16 bits of the mask; Sunshine extension, 0 for GFE | LE |
| 32 | i16 | tailB | `MC_TAIL_B` 0x0055 | LE |

How the client fills it and how Sunshine reads it:
- Client field population: `InputStream.c:1182-1205 @ 41cedb8`.
- Sunshine rebuilds the 32-bit mask as `buttonFlags | (buttonFlags2 << 16)` (`sun/src/input.cpp:1646-1649 @ d0b4a88`).
- Sunshine reads these fields with no byte swap, so they are native LE on LE hosts (`input.cpp:1646-1656 @ d0b4a88`).
- Sunshine does **not** validate headerB, midB, tailA or tailB; it checks only the size (`input.cpp:1777-1778 @ d0b4a88`).
- Controller number must be below 16 (`sun/src/input.cpp:49, 313, 1618-1622 @ d0b4a88`).

Gamepad allocation, removal and Back/Home handling:
- If the controller's bit is set in the mask and that pad is not allocated, Sunshine allocates it.
- If the bit is clear and the pad exists, Sunshine frees it and drops the event (`input.cpp:1626-1637 @ d0b4a88`).
- So a client **must** set its own bit in `activeGamepadMask`. If it does not, the event is ignored ("not allocated", `input.cpp:1639-1644`).
- Holding Back emulates Home after `back_button_timeout` (`input.cpp:1659-1714 @ d0b4a88`).

Client-side quirks:
- Negative `buttonFlags` from a sign-extended short are masked to 0xFFFF (`InputStream.c:1094-1096`).
- For GFE only, MISC is mapped onto SPECIAL (`1098-1111`).
- `LiSendControllerEvent` means controller 0 with mask 0x1 (`1249-1254`).

**Button flag bits.** `mcc/src/Limelight.h:720-742 @ 41cedb8`; Sunshine mirrors them in `sun/src/platform/common.h:84-104 @ d0b4a88`.

| Button | Flag | Bit |
|---|---|---|
| D-pad Up / Down / Left / Right | UP/DOWN/LEFT/RIGHT_FLAG | 0x0001 / 0x0002 / 0x0004 / 0x0008 |
| Start | PLAY_FLAG (Sunshine `START`) | 0x0010 |
| Back / Select | BACK_FLAG | 0x0020 |
| Left stick click | LS_CLK_FLAG | 0x0040 |
| Right stick click | RS_CLK_FLAG | 0x0080 |
| LB (left shoulder) | LB_FLAG | 0x0100 |
| RB (right shoulder) | RB_FLAG | 0x0200 |
| Home / Guide | SPECIAL_FLAG (Sunshine `HOME`) | 0x0400 |
| A / B / X / Y | A/B/X/Y_FLAG | 0x1000 / 0x2000 / 0x4000 / 0x8000 |
| Paddle 1-4 (Sunshine only) | PADDLE1..4_FLAG | 0x010000 / 0x020000 / 0x040000 / 0x080000 |
| Touchpad click (Sony) | TOUCHPAD_FLAG | 0x100000 |
| Misc (Share/Mic/Capture/Mute) | MISC_FLAG | 0x200000 |

Bits 0x0800 and 0x400000+ are unassigned in both headers. Triggers are not buttons: they travel as the two u8 axes.

Stick axis polarity (which direction is positive Y) is not documented in these files. **UNCONFIRMED.**

**Controller arrival (Sunshine extension).** `Input.h:157-164 @ 41cedb8`. 16 bytes, size 12, magic 0x55000004.
- Off 8: u8 `controllerNumber`.
- Off 9: u8 `type` (LI_CTYPE).
- Off 10: u16 LE `capabilities` (LI_CCAP).
- Off 12: u32 LE `supportedButtonFlags`.

Client behaviour:
- It forces `LI_CCAP_TOUCHPAD` on when `LI_CCAP_DUAL_TOUCHPAD` is set.
- It sends the arrival packet only to Sunshine.
- It always follows with an empty multi-controller event (`controllerNumber`, mask, all zeros) as a fallback (`InputStream.c:1513-1558 @ 41cedb8`).

Sunshine reads `capabilities` and `supportedButtonFlags` LE and allocates the pad (`sun/src/input.cpp:1367-1378 @ d0b4a88`).

Constants:
- LI_CTYPE: UNKNOWN 0x00, XBOX 0x01, PS 0x02, NINTENDO 0x03, STEAM 0x04 (`Limelight.h:777-781`).
- LI_CCAP: ANALOG_TRIGGERS 0x01, RUMBLE 0x02, TRIGGER_RUMBLE 0x04, TOUCHPAD 0x08, ACCEL 0x10, GYRO 0x20, BATTERY_STATE 0x40, RGB_LED 0x80, DUAL_TOUCHPAD 0x100 (`Limelight.h:782-790 @ 41cedb8`).

**Controller touch (Sunshine extension).** `Input.h:166-177`. 28 bytes, size 24, magic 0x55000005.
- Off 8: u8 `controllerNumber`.
- Off 9: u8 `eventType`.
- Off 10: u8 reserved (0).
- Off 11: u8 `touchpadIndex` (0 or 1).
- Off 12: u32 LE `pointerId`.
- Off 16: netfloat `x`.
- Off 20: netfloat `y`.
- Off 24: netfloat `pressure`.

Client fill: `InputStream.c:1561-1608`. It is gated on host feature flag `LI_FF_CONTROLLER_TOUCH_EVENTS` (0x02) (`InputStream.c:1570`). That flag comes from DESCRIBE `x-ss-general.featureFlags` (`mcc/src/RtspConnection.c:1145`; Sunshine emits `platf::get_capabilities()` at `sun/src/rtsp.cpp:878`, with caps `pen_touch` 0x01 and `controller_touch` 0x02 at `sun/src/platform/common.h:401-415 @ d0b4a88`).

Sunshine clamps x, y and pressure to [0,1]. At this SHA its handler does **not** forward `touchpadIndex` (`sun/src/input.cpp:1517-1543 @ d0b4a88`).

Touch event types (shared with screen touch): HOVER 0x00, DOWN 0x01, UP 0x02, MOVE 0x03, CANCEL 0x04, BUTTON_ONLY 0x05, HOVER_LEAVE 0x06, CANCEL_ALL 0x07 (`Limelight.h:651-658 @ 41cedb8`).

**Controller motion (Sunshine extension).** `Input.h:179-188`. 24 bytes, size 20, magic 0x55000006.
- Off 8: u8 `controllerNumber`.
- Off 9: u8 `motionType`.
- Off 10: 2 reserved bytes (0).
- Off 12, 16, 20: netfloat `x`, `y`, `z`.

Types and units:
- ACCEL 0x01 in m/s² (gravity included).
- GYRO 0x02 in deg/s.
- Axes follow SDL's convention (`Limelight.h:812-822 @ 41cedb8`).

Client behaviour:
- It is gated on `LI_FF_CONTROLLER_TOUCH_EVENTS`.
- It coalesces to the latest sample per controller and type (`InputStream.c:1614-1683, 535-569 @ 41cedb8`).
- Clients should report motion only after the host requests it with control message 0x5501 (`Limelight.h:474, 813-814`; see section 9).

Sunshine decodes the floats LE (`sun/src/input.cpp:1550-1575 @ d0b4a88`).

**Controller battery (Sunshine extension).** `Input.h:190-197`. 12 bytes, size 8, magic 0x55000007.
- Off 8: u8 `controllerNumber`.
- Off 9: u8 `batteryState`.
- Off 10: u8 `batteryPercentage` (0xFF = unknown).
- Off 11: reserved.

States: UNKNOWN 0, NOT_PRESENT 1, DISCHARGING 2, CHARGING 3, NOT_CHARGING 4, FULL 5 (`Limelight.h:828-834`). Client fill: `InputStream.c:1685-1725`. Sunshine handler: `sun/src/input.cpp:1582-1605 @ d0b4a88`.

**Keyboard.** `Input.h:22-30`. 14 bytes, size 10, magic 0x03 (down) or 0x04 (up), equal to KEY_ACTION_DOWN/UP (`Limelight.h:701-702`).
- Off 8: u8 `flags`. Sunshine extension; `SS_KBE_FLAG_NON_NORMALIZED` 0x01 means "keycode is not a US-layout VK, interpret as-is" (`Limelight.h:713`). Always 0 for GFE.
- Off 9: i16 LE `keyCode` (Win32 VK code, US layout; `Limelight.h:694-700`).
- Off 11: u8 `modifiers`.
- Off 12: i16 `zero2` = 0.

Modifier bits: SHIFT 0x01, CTRL 0x02, ALT 0x04, META 0x08, EXTENDED 0x10. EXTENDED marks the 0xE0-prefixed variant and is a Sunshine extension (`Limelight.h:703-707`; client fill `InputStream.c:1023-1028`; GFE-only modifier fixups `981-1021`).

Sunshine uses only `keyCode & 0x00FF` and keys press state on `(keyCode, flags)` (`sun/src/input.cpp:1193-1224 @ d0b4a88`).

**UTF-8 text.** `Input.h:32-37`. Magic 0x17. The body is 1-32 bytes of UTF-8 with no terminator; `size = 4 + len`.
- Sunshine packs as many whole code points as fit into 32 bytes.
- GFE gets one code point per packet (`InputStream.c:571-645 @ 41cedb8`).
- Sunshine reads `BE32(size) - 4` bytes (`sun/src/input.cpp:1312-1319`; max-length check `1775-1776 @ d0b4a88`).

**Mouse relative move.** `Input.h:39-45`. 12 bytes, size 8, magic 0x07.
- Off 8: i16 **BE** `deltaX`.
- Off 10: i16 **BE** `deltaY`.

The client batches deltas within 1 ms and splits values outside the i16 range into several packets (`InputStream.c:44, 398-466, 746-823`). Sunshine reads both fields BE (`sun/src/input.cpp:771-786 @ d0b4a88`).

**Mouse absolute position.** `Input.h:47-60`. 18 bytes, size 14, magic 0x05.
- Off 8: i16 BE `x`.
- Off 10: i16 BE `y`.
- Off 12: i16 `unused` = 0.
- Off 14: i16 BE `width`.
- Off 16: i16 BE `height`.

The client sends `referenceWidth-1` and `referenceHeight-1` to work around a GFE rounding issue (`InputStream.c:853-873`). Sunshine reads all fields BE (`sun/src/input.cpp:881-902 @ d0b4a88`).

**Mouse button.** `Input.h:62-67`. 9 bytes, size 5, magic 0x08 (down) or 0x09 (up).
- Off 8: u8 `button`: LEFT 1, MIDDLE 2, RIGHT 3, X1 4, X2 5 (`Limelight.h:685-691`).

Client: `InputStream.c:918-957`. Sunshine: `sun/src/input.cpp:920-934 @ d0b4a88`.

**Vertical scroll.** `Input.h:111-118`. 14 bytes, size 10, magic 0x0A.
- Off 8: i16 BE `scrollAmt1`.
- Off 10: i16 BE `scrollAmt2`, a copy of scrollAmt1.
- Off 12: i16 `zero3`.

One click equals WHEEL_DELTA 120 (`InputStream.c:38, 1313-1315, 1346-1348, 1362-1364`). Sunshine uses `scrollAmt1` BE (`sun/src/input.cpp:1266-1275 @ d0b4a88`).

**Horizontal scroll (Sunshine extension).** `Input.h:120-124`. 10 bytes, size 6, magic 0x55000001.
- Off 8: i16 BE `scrollAmount`.

Client: `InputStream.c:1367-1404`. Sunshine: `sun/src/input.cpp:1289-1297 @ d0b4a88`.

**Touch (Sunshine extension).** `Input.h:126-138`. 36 bytes, size 32, magic 0x55000002.
- Off 8: u8 `eventType`.
- Off 9: u8 reserved.
- Off 10: u16 LE `rotation` (degrees; 0xFFFF = unknown, `Limelight.h:659`).
- Off 12: u32 LE `pointerId`.
- Off 16, 20, 24, 28, 32: netfloat `x`, `y`, `pressureOrDistance`, `contactAreaMajor`, `contactAreaMinor`.
- Coordinates are normalized 0..1 (`Limelight.h:615-645`).

Gated on `LI_FF_PEN_TOUCH_EVENTS` 0x01 (`InputStream.c:1410-1455`). Sunshine reads integers LE and floats LE, clamped (`sun/src/input.cpp:1465-1489`, normalization helper from ~1420 `@ d0b4a88`).

**Pen (Sunshine extension).** `Input.h:140-155`. 36 bytes, size 32, magic 0x55000003.
- Off 8: u8 `eventType`.
- Off 9: u8 `toolType`: UNKNOWN 0, PEN 1, ERASER 2.
- Off 10: u8 `penButtons`: PRIMARY 0x01, SECONDARY 0x02, TERTIARY 0x04.
- Off 11: reserved.
- Off 12, 16, 20: netfloat `x`, `y`, `pressureOrDistance`.
- Off 24: u16 LE `rotation`.
- Off 26: u8 `tilt` (0xFF = unknown).
- Off 27: reserved.
- Off 28, 32: netfloat `contactAreaMajor`, `contactAreaMinor`.

Constants: `Limelight.h:672-678`. Client: `InputStream.c:1457-1511`. Sunshine: `sun/src/input.cpp:1490+ @ d0b4a88`.

**Enable haptics.** `Input.h:16-20`. 10 bytes, size 6, magic 0x0D.
- Off 8: u16 LE `enable` = 1.

The client sends it once at input start for app version 7.1.0 or later (`InputStream.c:658-687, 716-717`). Sunshine has no dispatch case for 0x0D, so the packet is ignored (`sun/src/input.cpp:2162-2207 @ d0b4a88`).

### 12.6 Batching and coalescing
- **Client.**
  - Multi-controller packets are coalesced per controller until the button mask changes (`InputStream.c:1117-1162, 379-396 @ 41cedb8`).
  - Mouse moves are batched within 1 ms (`44, 397-466`).
  - Pen events are batched within 1 ms (`45, 489-534`).
  - Motion keeps only the latest sample (`535-569`).
- **Sunshine.** It queues validated packets and merges consecutive same-magic packets of these batchable types: rel/abs mouse, scroll, hscroll, multi-controller, touch, pen, controller touch, controller motion. It processes up to 32 packets per task turn (`sun/src/input.cpp:2050-2080, 2211-2233 @ d0b4a88`).
- A plain test client does not need batching. Each packet stands alone.

### 12.7 Discrepancies in the existing client (`moonlight-mcp/src/control_stream.rs`)

Upstream's correct plaintext for one gamepad event is 38 bytes:
`[06 02][22 00]` (type 0x0206 LE, then payloadLength 34 LE), followed by the 34-byte multi-controller packet starting `[00 00 00 1E][0C 00 00 00][1A 00]…`.

Problems found:

1. **No `payloadLength` u16 in the V2 header.**
   - `send_input` writes `[0x0206 LE][input_type u8][data]` (control_stream.rs `send_input`, about lines 160-170), and `send_encrypted` adds nothing.
   - Upstream inserts `payloadLength` after `type` (`ControlStream.c:20-23, 724-727 @ 41cedb8`).
   - Sunshine **always skips 4 bytes** after decrypting (`sun/src/stream.cpp:1299-1300, 1309-1311 @ d0b4a88`). So the client's `input_type` byte and the low byte of its "0x001e" field are consumed as if they were `payloadLength`.
2. **No `NV_INPUT_HEADER`.**
   - Upstream needs `size` (BE u32) and `magic` (LE u32).
   - The existing code writes a 1-byte "input_type" and a 2-byte LE "packet length" instead.
   - After Sunshine strips 4 bytes, the bytes it parses as `size` are `00 01 00 00`. BE32 of that is 0x00010000, which is larger than the packet, so the packet fails `validate_input_packet` and is **dropped** with "Dropping malformed input packet" (`sun/src/input.cpp:1755-1758, 2240-2253 @ d0b4a88`). No gamepad input reaches Sunshine.
3. **Wrong magic.**
   - The code uses 0x0D, which is the Gen4 `MULTI_CONTROLLER_MAGIC` and also `ENABLE_HAPTICS_MAGIC`.
   - Sunshine accepts only `MULTI_CONTROLLER_MAGIC_GEN5` = **0x0C** (`Input.h:87-88`; `sun/src/input.cpp:1777, 2186 @ d0b4a88`).
4. **Missing fields.**
   - The code omits `headerB` (0x001A), `midB` (0x0014), `tailA` (0x009C), `buttonFlags2`, and `tailB` (0x0055).
   - Its order is `[len][active][ctrl#][buttons]…`, but upstream order is `headerB, controllerNumber, activeGamepadMask, midB, buttonFlags, …` (`Input.h:93-109`).
   - So **controllerNumber and activeGamepadMask are also swapped**.
   - The struct must total 34 bytes.
5. **`button_flags: u16`.** This cannot carry PADDLE, TOUCHPAD or MISC (0x010000-0x200000). Those bits go in `buttonFlags2` (`InputStream.c:1204 @ 41cedb8`).
6. **No ENet framing or channel.** Datagrams are raw UDP. Upstream sends on ENet channel 0x10+n, reliable (`InputStream.c:1150-1153`). ENet transport itself is covered in section 11.
7. **No controller arrival or mask handshake.** Sunshine allocates a pad only when the controller's bit is set in `activeGamepadMask`. Arrival (0x55000004) is preferred (`sun/src/input.cpp:1626-1644, 1367-1378 @ d0b4a88`).
8. **Related messages in the same file (not input, but sharing the V2-header issue).**
   - `send_ping` / `test_connection` send plaintext `[00 00 00 00]`. Sunshine treats that as control type 0x0000, an unknown type, and only debug-logs it (`stream.cpp:751-757`). It is not the real periodic ping (0x0200; see section 9).
   - `close()` sends 2-byte plaintext `[00 01]`. The wrapper length is then 4+16+2 = 22, below Sunshine's runt threshold of 24, so it is dropped as "Control: Runt packet" (`stream.cpp:1256-1262 @ d0b4a88`).
   - The IDR request `[02 03][u32 0][u32 0]` works **only by accident**: Sunshine reads type 0x0302 and skips the next 2 bytes as `payloadLength` (`stream.cpp:1299-1300, 1206-1210`).
9. **HTTP input endpoints.** `pairing.rs` `send_keyboard_input`, `send_mouse_input` and `send_gamepad_input` POST to `https://host:47984/input`. Sunshine's HTTPS server has no `/input` route; it serves only serverinfo, pair, applist, appasset, launch, resume and cancel (`sun/src/nvhttp.cpp:1728-1741 @ d0b4a88`), so these calls get a 404. They look like Wolf-specific endpoints.

---

## 13. Pairing as a client must perform it

Pinned sources: moonlight-qt `8f2e52e` (= 8f2e52e54046fc942e2b80ffb88a71ff0f2300bd) as the client reference; Sunshine `d0b4a88` as the host reference.

### 13.1 Ports and identity
- Ports:
  - The plain-HTTP pairing port defaults to 47989. HTTPS defaults to 47984 (`mqt/app/backend/nvaddress.h:5-6 @ 8f2e52e`).
  - On Sunshine, HTTP = base port + 0 and HTTPS = base − 5 (`sun/src/nvhttp.h:48-53`). The base port is 47989 (`sun/src/config.cpp:905`), and `map_port` adds the offset to the base (`sun/src/network.cpp:243-253 @ d0b4a88`).
- Query parameters on every request:
  - Moonlight always prepends `uniqueid=<id>&uuid=<random RFC4122 hex>` (`mqt/app/backend/nvhttp.cpp:495-498 @ 8f2e52e`).
  - `<id>` is the real per-install ID unless the host is NVIDIA software, in which case it is the fixed placeholder `0123456789ABCDEF` (`nvhttp.cpp:43-45, 496`). "NVIDIA software" means the serverinfo `state` contains `MJOLNIR` (`mqt/app/backend/nvcomputer.cpp:202`).
  - The per-install ID is a random 64-bit value rendered as lowercase hex. It is generated once and persisted (`mqt/app/backend/identitymanager.cpp:137-151 @ 8f2e52e`).
- How Sunshine uses `uniqueid`:
  - It requires `uniqueid` on `/pair` (`sun/src/nvhttp.cpp:1095-1101`).
  - It uses it **only as the key for the pending pairing session**. A second concurrent session with the same uniqueid gets HTTP-level `status_code` 409; more than 32 pending sessions gets 503 (`nvhttp.cpp:612-635, 1027-1039`; limit `nvhttp.h:58`).
  - Paired clients are **identified by their client certificate**. Pairing stores the PEM certificate taken from the `clientcert` parameter (`nvhttp.cpp:1008, 906-909`). Later HTTPS requests are authorized by TLS client-certificate verification against the stored certificate chain plus an "enabled" check (`nvhttp.cpp:97-98, 367-372, 1680-1711`).
  - At launch the uniqueid is only recorded (`nvhttp.cpp:512 @ d0b4a88`).

### 13.2 Request sequence (moonlight-qt `pair()`, `mqt/app/backend/nvpairingmanager.cpp:215-381 @ 8f2e52e`)

All `/pair` requests are HTTP GET with the query `uniqueid=…&uuid=…&devicename=roth&updateState=1&…`. Moonlight hardcodes `devicename=roth`.

| # | Transport | Extra query | Client sends | Host returns |
|---|---|---|---|---|
| 1 | HTTP | `phrase=getservercert&salt=<hex 16B>&clientcert=<hex of client PEM>` | random 16-byte salt | `<paired>1</paired><plaincert>hex(server PEM)</plaincert>` |
| 2 | HTTP | `clientchallenge=<hex>` (**no phrase**) | AES-ECB(key, random 16-byte challenge) | `<challengeresponse>hex(AES-ECB(key, H ‖ serverChallenge16))` |
| 3 | HTTP | `serverchallengeresp=<hex>` (**no phrase**) | AES-ECB(key, hash(serverChallenge ‖ clientCertSig ‖ clientSecret16) zero-padded to 32 bytes) | `<pairingsecret>hex(serverSecret16 ‖ RSA-SHA256 sig(serverSecret))` |
| 4 | HTTP | `clientpairingsecret=<hex>` (**no phrase**) | clientSecret16 ‖ RSA-SHA256 sig(clientSecret) | `<paired>1</paired>` on success |
| 5 | **HTTPS** (47984, client cert) | `phrase=pairchallenge` | nothing | `<paired>1</paired>` |

Client sources: step 1 `242-252`, step 2 `274-287`, step 3 `296-318`, step 4 `350-365`, step 5 `367-377`.

Notes:
- Step 1 uses timeout 0, which means no transfer timeout (`nvhttp.cpp:528-531 @ 8f2e52e`). The host holds that response until the PIN is entered (13.6).
- After step 1, Moonlight pins the returned server certificate for TLS (`nvpairingmanager.cpp:261-272`).
- An empty `plaincert` means "already pairing". Moonlight then unpairs and returns ALREADY_IN_PROGRESS (`254-259`).
- Every request checks the XML `status_code` and requires `<paired>1</paired>`.

### 13.3 How Sunshine dispatches (`sun/src/nvhttp.cpp:1081-1146 @ d0b4a88`)
- `phrase=getservercert` starts a session (`1105-1111`).
- `phrase=pairchallenge` immediately returns `paired=1` and status 200 **with no session check at all** (`1112-1116`). It is routed on both the HTTP and HTTPS servers (`1728-1732, 1747-1751`).
- Any other request needs an existing session for that `uniqueid`; otherwise the host returns 400 "Invalid uniqueid" (`1118-1126`). The step is then chosen **purely by which parameter is present**, tested in this order: `clientchallenge`, `serverchallengeresp`, `clientpairingsecret`. If none is present, it fails with "Invalid pairing request" (`1128-1141`).
- Each step enforces order through `last_phase`. A step out of order fails and erases the session (`753-757, 784-787, 834-837, 870-873`; `fail_pair` `737-743`; erase `1143-1145`).
- **What this means for the existing client.** It sends `phrase=clientchallenge`, `phrase=serverchallengeresp` and `phrase=clientpairingsecret` as well as the value parameter. Sunshine checks `phrase` only for the two literals `getservercert` and `pairchallenge`, so those extra phrases fall through to parameter-presence dispatch and are handled correctly. **They are harmless on Sunshine** (and they are not what moonlight-qt sends).

### 13.4 Key derivation and crypto
- **Key:**
  - `key = H(salt ‖ PIN_utf8)` truncated to 16 bytes.
  - H is SHA-256 when the server's appversion major is 7 or higher; SHA-1 (20-byte hash) below that (`nvpairingmanager.cpp:209-213, 218-240 @ 8f2e52e`).
  - Sunshine always uses SHA-256 (`sun/src/crypto.cpp:325-345 @ d0b4a88`) and reports major version 7 (`sun/src/nvhttp.h:38`).
  - The salt is sent as 32 hex characters. Sunshine fails with "Salt too short" below 32 and parses the first 32 characters as 16 bytes (`nvhttp.cpp:759-769`).
- **Cipher:** AES-128-ECB with **no padding** on both sides (`nvpairingmanager.cpp:54-106`; Sunshine `ecb_t(key, false)` at `nvhttp.cpp:794, 846` → `crypto.cpp:226-230, 254-258, 308-310`).
- **"Cert signature"** means the raw signature bytes of an X.509 certificate (`X509_get0_signature`) (`nvpairingmanager.cpp:108-129`; `sun/src/crypto.cpp:404-410`).
- **RSA signatures** are SHA-256 digest signatures (`nvpairingmanager.cpp:153-207`; `sun/src/crypto.cpp:508-509, 544-545`).

### 13.5 Response layouts and the checks the client must do
- **challengeresponse.** Decrypted, it is `H(clientChallenge ‖ serverCertSig ‖ serverSecret)` (hashLength bytes: 32 for SHA-256), followed by a 16-byte `serverChallenge`.
  - Host side: `sun/src/nvhttp.cpp:796-823`.
  - The client requires at least hashLength + 16 bytes and splits at hashLength (`nvpairingmanager.cpp:289-300`).
- **serverchallengeresp (client to host).** The client computes `H(serverChallenge ‖ clientCertSig ‖ clientSecret)`, zero-pads it to 32 bytes, and encrypts it (`nvpairingmanager.cpp:296-306`).
  - Sunshine stores the decrypted value (`nvhttp.cpp:845-850`).
  - At step 4 Sunshine compares it with `SHA-256(serverChallenge ‖ clientCertSig(from clientcert) ‖ clientSecret)`, requiring equal length, and verifies the client's RSA signature over `clientSecret`. Only when both pass does it add the client certificate. A mismatch gives `paired=0` with status 200 (`nvhttp.cpp:883-913`).
- **pairingsecret** = `serverSecret` (16 bytes) followed by the RSA-SHA256 signature over `serverSecret` (`sun/src/nvhttp.cpp:852-857`). The client requires more than 16 bytes (`nvpairingmanager.cpp:320-328`).
- **Client check A (MITM).** Verify the signature over `serverSecret` with the public key from the `plaincert` received in step 1. On failure: "MITM detected", unpair (`nvpairingmanager.cpp:330-337`).
- **Client check B (wrong PIN).** Recompute `H(clientChallenge ‖ serverCertSig(plaincert) ‖ serverSecret)` and compare it with the hash from challengeresponse. On mismatch: "Incorrect PIN", unpair, PIN_WRONG (`nvpairingmanager.cpp:339-348`). Check B runs **before** step 4 is sent.
- **Unpair on failure.** Moonlight sends `GET /unpair` over HTTP after any failure from step 1 onward (`nvpairingmanager.cpp:257, 266, 285, 292, 316, 323, 335, 346, 363, 375`). **Sunshine registers no `/unpair` route** (`sun/src/nvhttp.cpp:1728-1755`), so that request gets the 404 `not_found` response (`nvhttp.cpp:966-983`). Cleanup on Sunshine is implicit: failed or out-of-order steps erase the session (`1143-1145`), and pending sessions expire after 5 minutes with 408 (`nvhttp.h:73`; `nvhttp.cpp:595-610`).
- **Final step.** HTTPS `phrase=pairchallenge` on the HTTPS port, presenting the client certificate (`nvpairingmanager.cpp:367-377`). On Sunshine the client is already authorized after step 4, and this step is an unconditional `paired=1` (`nvhttp.cpp:1112-1116`). Its practical value is proving that the TLS client certificate is now accepted; that is an inference from the TLS verify path at `1680-1711`.

### 13.6 How Sunshine accepts the PIN on the host side
- **Default mode (not `PIN_STDIN`):**
  - `getservercert` creates a pending session with a random 32-hex-character `pairing_id`. It stores `salt`, `clientcert` and `devicename` plus the requester's address, and **holds the HTTP response open** (`sun/src/nvhttp.cpp:996-1052`; ID generation `612-635`; `PAIRING_ID_SIZE` 32 at `nvhttp.h:63`).
  - The client's step-1 request blocks until an operator submits the PIN, for up to 5 minutes.
  - In `PIN_STDIN` mode the PIN is read from the console instead (`1054-1072`).
- **Web UI server:**
  - HTTPS on base port + 1, which is **47990** by default (`sun/src/confighttp.h:29`; `confighttp.cpp:2458-2461`; base `config.cpp:905`).
  - Routes `/api/csrf-token` (GET) and `/api/pin` (GET, DELETE, POST) (`confighttp.cpp:2500-2504 @ d0b4a88`).
- **Authentication:**
  - HTTP Basic auth. The username must match case-insensitively. The check is `hex(SHA-256(password + salt))` against the stored hash (`confighttp.cpp:648-689`).
  - The request origin must be within `origin_web_ui_allowed`, default `lan` (`confighttp.cpp:651-655`; `config.cpp:853`).
  - If no username has been set yet, the server redirects to `/welcome` (`658-661`).
- **CSRF:** Requests with **neither** an `Origin` nor a `Referer` header are exempt, which covers non-browser clients (`confighttp.cpp:877-882`). Otherwise an `X-CSRF-Token` header from `GET /api/csrf-token` is required (`886-895, 1062-1075`).
- **`GET /api/pin`** returns `{"pairings":[{"id","name","address"}…]}`. `name` is the client's untrusted `devicename` (`confighttp.cpp:1805-1822`; `nvhttp.cpp:670-695`).
- **`POST /api/pin`:**
  - Requires `Content-Type: application/json`.
  - Body: `{"pairing_id":"<32 hex>","pin":"<exactly 4 digits>","name":"<1..128 bytes>"}` (`confighttp.cpp:1886-1928`; validators `nvhttp.cpp:649-663`; `MAX_PAIRING_CLIENT_NAME_SIZE` 128 at `nvhttp.h:68`).
  - On submit, Sunshine derives the key from the operator's PIN, releases the held `getservercert` response, and stores `name` as the paired client's name. It then **blocks** until the handshake finishes, up to the lesser of the session expiry and `config::stream.ping_timeout`, and returns `{"status": true|false}` (`nvhttp.cpp:1148-1201`).
- **`DELETE /api/pin`** with `{"pairing_id":…}` cancels a pending request. The waiting client then gets `paired=0` with status 400 (`confighttp.cpp:1835-1867`; `nvhttp.cpp:697-720`).
- **Timing consequence.** A test harness that drives both sides must issue step 1 asynchronously, poll `GET /api/pin` for the new `id`, and then POST the PIN. The POST does not return until the client has finished steps 2-4, so the POST must also be asynchronous (or run concurrently with steps 2-4).

### 13.7 Discrepancies in the existing client (`moonlight-mcp/src/pairing.rs:125-445`)
1. **Host PIN entry targets endpoints Sunshine does not have.**
   - The client calls `GET http://host:47989/pairing/pending` (expects a JSON `secrets` array) and `POST http://host:47989/pin/` with `{pin, secret}`.
   - Sunshine's HTTP server on 47989 serves only `/serverinfo` and `/pair`; everything else gets the XML 404 (`sun/src/nvhttp.cpp:1747-1751, 966-983`). The JSON parse in `poll_for_secret` will fail.
   - Sunshine needs `GET` and `POST https://host:47990/api/pin`, with Basic auth, body fields `pairing_id`/`pin`/`name`, and the `pairing_id` taken from `pairings[].id` (13.6). These look like Wolf endpoints.
2. **The step-1 timeout is 60 s**, but Sunshine can hold the response up to 5 minutes (`nvhttp.h:73`). Moonlight uses no timeout (`nvpairingmanager.cpp:246`).
3. **No MITM or wrong-PIN checks.**
   - Phase 2 discards the 32-byte hash in challengeresponse and keeps only the last 16 bytes.
   - Phase 3 never parses `pairingsecret`, never verifies the server's signature over serverSecret, and never recomputes `H(clientChallenge ‖ serverCertSig ‖ serverSecret)`.
   - Result: a wrong PIN is detected only when step 4 returns `paired=0`, and a MITM is not detected at all (`nvpairingmanager.cpp:330-348`).
4. **The final HTTPS `phrase=pairchallenge` step is missing.** That is harmless on Sunshine, where the step is a no-op (`nvhttp.cpp:1112-1116`), but it differs from moonlight-qt. Sending it is a cheap check that the client certificate is now accepted over TLS.
5. **No unpair on failure.** Sunshine has no `/unpair` route anyway; the session is erased on failure (13.5).
6. **Constant `uniqueid` `moonlight-mcp-e2e`.** This works because Sunshine identifies clients by certificate. Two parallel pairing attempts from the same harness would collide (409). moonlight-qt uses a persisted random 64-bit hex ID (`identitymanager.cpp:137-151`).
7. **Extra `phrase=` values on steps 2-4.** These are harmless (13.3).
8. **Things that match upstream:**
   - SHA-256 key derivation over salt ‖ PIN truncated to 16 bytes (`pairing.rs` `derive_aes_key`, about line 177).
   - AES-128-ECB.
   - Client hash input `serverChallenge ‖ clientCertSig ‖ clientSecret` (32 bytes, so no padding is needed).
   - `clientpairingsecret` = secret ‖ RSA-SHA256 signature.
   - Salt as 32 lowercase hex characters (Sunshine's hex parse accepts it; `nvhttp.cpp:764-766`).

### 13.8 UNCONFIRMED
- Stick axis sign convention (12.5).
- Whether Sunshine uses `touchpadIndex` anywhere other than the handler read here. At this SHA the controller-touch handler drops it (`sun/src/input.cpp:1533-1540`).
- The exact `platf::get_capabilities()` value per OS. Only the bit meanings are confirmed (`sun/src/platform/common.h:401-415`).
- Whether `pairchallenge` over HTTPS can succeed before step 4 completes. That depends on TLS verification of a not-yet-stored certificate (`nvhttp.cpp:1680-1711`); inferred but not tested.

---

## Discrepancies between the existing client and upstream (sections 1-11)

Existing client files: `/Volumes/4TB_Drive/Documents/homelab/moonlight-mcp/src/pairing.rs`,
`/Volumes/4TB_Drive/Documents/homelab/moonlight-mcp/src/control_stream.rs`,
`/Volumes/4TB_Drive/Documents/homelab/moonlight-mcp/src/tests.rs`.

**Control stream (most severe)**
1. **No ENet framing.** `control_stream.rs` sends bare UDP datagrams (`socket.send(&packet)`, around lines 130-140).
   Sunshine only speaks ENet on the control port (section 11), so the datagrams are discarded. Consequences: no
   control peer, so (a) input never arrives, (b) HDR/rumble/termination never come back, and (c) Sunshine
   **tears the session down when `pingTimeout` (10 s) expires** (section 9 "Liveness").
2. **Control IV mismatch.** The client always uses the V2 12-byte IV (`build_nonce`, around lines 88-105). But its
   ANNOUNCE omits `x-ss-general.encryptionEnabled`, which Sunshine then defaults to 0. Sunshine therefore uses the
   **legacy 16-byte IV** (`iv[0] = seq & 0xFF`) (`rtsp.cpp:1087,1110`; `stream.cpp:1269-1287`), and GCM
   verification fails, which makes Sunshine call `session::stop` (`stream.cpp:1290-1296`). Fix: send
   `x-ss-general.encryptionEnabled:1` (at least CONTROL_V2), or implement the legacy IV.
3. **Missing V2 inner header.** The plaintext must be `u16 type ‖ u16 payloadLength ‖ payload`. The client encrypts
   `[type][input_type][...]` (`send_input`, around lines 160-170) or raw bytes (`send_ping` encrypts `[0,0,0,0]`).
   Sunshine always skips 4 bytes after the type (`stream.cpp:1299-1300`).
4. **Wrong message semantics.** "Ping" should be type `0x0200` with an 8-byte payload every 100 ms. The current
   4-zero-byte plaintext decodes as type `0x0000`, which Sunshine only debug-logs as unknown (`stream.cpp:751-757`).
   The IDR request should be `0x0302` with a 2-byte `{0,0}` payload. The client's `[02 03][u32 0][u32 0]` works only
   by accident: Sunshine consumes the next 2 bytes as `payloadLength` and ignores the payload (`stream.cpp:1206-1210, 1299-1300`).
   `close()` encrypts a 2-byte plaintext, so the wrapper length is 22, below Sunshine's runt threshold of 24. Sunshine
   drops it as "Control: Runt packet" (`stream.cpp:1259-1262`). Even if it got through, `0x0100` is a host→client
   type; upstream clients end with an ENet disconnect. START A (`0x0302 {0,0}`) and START B (`0x0307 {0}`) are never
   sent. The enum value `Termination = 0x0100` is the non-encrypted Gen7 short form; Sunshine sends `0x0109`.
   Input-packet discrepancies are detailed in 12.7, and pairing discrepancies in 13.7.
5. **Control port hardcoded to 47999** (`tests.rs:316-318, 677`). It must come from the SETUP control `server_port`
   (`RtspConnection.c:1318-1328`). `rtsp_handshake` never parses the SETUP control response (`pairing.rs` around
   lines 1303-1316) and ignores `X-SS-Connect-Data`, which ENet CONNECT `data` must carry.
6. `test_nonce_building` (`control_stream.rs` tests) places `rikeyid` in the nonce. No upstream IV uses `rikeyid` for
   control. Only audio uses it (section 8).

**RTSP / ANNOUNCE (`pairing.rs` `build_announce_payload`, around lines 1178-1218, and `rtsp_handshake`, around 1220-1364)**
7. `x-nv-general.featureFlags:167` (= `0x87 | 0x20`) **forces audio encryption** (`rtsp.cpp:1112-1115`). Upstream sends
   135 unless audio encryption is wanted. Any future audio decoding must implement AES-CBC (section 8), or send 135.
8. Missing Sunshine attributes: `x-ml-general.featureFlags` (so Sunshine falls back to IP-based peer matching and
   accepts legacy pings), `x-ss-general.encryptionEnabled` (see item 2), `x-ss-video[0].chromaSamplingType`,
   `x-ml-video.configuredBitrateKbps` (without it the encoder bitrate = `maximumBitrateKbps` = 15500 with no FEC/audio
   adjustment), and `x-nv-vqos[0].bllFec.enable` (GFE only, harmless).
9. `x-nv-clientSupportHevc:0` and `bitStreamFormat:0`, `dynamicRangeMode:0` are hardcoded, so H.264 8-bit SDR only.
   HEVC/AV1/10-bit need the section 4/10 values. The DESCRIBE body is never parsed (codec availability, featureFlags,
   encryption flags, surround-params).
10. The SETUP control target is `streamid=control/0/0` and upstream uses `streamid=control/13/0`. This is harmless for
    Sunshine, which parses only the type token (`rtsp.cpp:969-985`). The PLAY target is `streamid=control/13/0` vs
    upstream `/`, also harmless (`rtsp.cpp:1273-1283`). The fallback Session `DEADBEEFCAFE` when the header is missing
    is harmless with Sunshine, but upstream treats a missing Session as fatal (`RtspConnection.c:1214-1220`).
11. `Host: <real IP>` enables HIGH_QUALITY (512 kbps) stereo on Sunshine (`rtsp.cpp:1166-1173`). Upstream sends `0.0.0.0`
    below 15 Mbps.
12. `/launch` (`pairing.rs` around lines 596-603) omits `corever=1` (plain `rtsp://`, which is fine) and `hdrMode`
    (needed for host HDR auto-switch). `rikeyid` is sent as an unsigned decimal and moonlight-qt sends signed; Sunshine
    `(int)`-casts, so this is UNCONFIRMED but likely equivalent.

**Video capture (`capture_moonlight_frame`, around lines 1396-1519)**
13. **Parity shards are not filtered.** The client accepts any packet with `flags & 0x1`. Parity shards carry RS-garbage
    `flags` bytes but the real `frameIndex` (section 5), so garbage can be appended, or can fake SOF/EOF. Filter on
    `fecIndex >= dataShards` from `fecInfo` (bytes 28-31 LE).
14. **Multi-FEC-block frames are not handled.** SOF/EOF are set per FEC block (`stream.cpp:1706-1711`). Large IDR frames
    (more than about 212 data shards at 20% FEC) span blocks, so the client ends the frame at block 0's EOF and
    restarts on block 1's SOF. Use `multiFecBlocks` bits 4-5 / 6-7.
15. No ordering by RTP sequence number, no `streamPacketIndex` continuity check (`VideoDepacketizer.c:786-799`), and no
    check of `rtp.header & 0x10` before assuming the 16-byte RTP header. On a loss the frame is silently corrupt.
16. Video/audio pings are sent 3× at 50 ms intervals only. That is enough for Sunshine to learn the peer (one ping
    suffices), but upstream pings every 500 ms for the whole session.
17. Applying `lastPayloadLen` trimming to H.264 is consistent with Sunshine's computation (section 6), but upstream only
    trims for non-H.264/HEVC codecs. It is correct only for the final block's final packet (see item 14).

---

## UNCONFIRMED items (sections 1-11; see 13.8 for sections 12-13)

- Exact semantics of each `hevc_mode` / `av1_mode` numeric value (0-5) beyond the thresholds cited in section 1.
- Which platform events cause Sunshine to re-initialize capture when HDR is toggled mid-stream (section 10). Only the
  "every encoder re-init sends `0x010e`" behaviour is confirmed.
- Audio FEC shard length: Sunshine uses the 4th packet's length and assumes equal-size (CBR) packets. That assumption
  is inferred, not stated in the source (section 8).
- RS per-byte-column independence, which explains why Sunshine's post-encode header edits do not corrupt recovered
  payloads, is inferred from the code flow (sections 5 and 7).
- Whether a raw non-ENet datagram can ever reach Sunshine's handler. Based on reading the ENet receive path it cannot,
  but this was not tested on the wire (section 11).
- Whether sending `rikeyid` as an unsigned decimal above 2³¹−1 parses identically to moonlight-qt's signed form in
  Sunshine's `util::from_view` (section 2, discrepancy 12).

### 13.8 T020 implementation and fixture boundary

The client now implements §13.2's five steps, using the pinned source checks in
§13.5. The `http` module owns bounded XML HTTP(S), app listing, and paired calls;
`nvhttp` is a compatibility re-export. Certificate and endpoint pins persist only
after the final mutual-TLS confirmation and an explicit `paired=1`. TLS verifies
the exact certificate and the handshake signatures; there is no trust-any mode.
Client state is isolated by the selected config directory, and partial, corrupt,
or mismatched key/certificate/ID state fails rather than silently regenerating.
The client retains the explicit major-version >=7 SHA-256 scope; pre-7 SHA-1
pairing is refused with a diagnostic, as before.

The 1 MiB HTTP/XML bound, 64 KiB state-file bound, 128 MiB scenario-input bound,
330-second pairing/PIN-delivery deadline and output-path symlink rejection are
local engineering limits, not upstream wire constants or measured host limits.
Concurrent protocol/PIN delivery uses cancellation-safe futures; a provider
failure cancels the outstanding protocol request and command helpers are killed
on cancellation. Sunshine PIN automation requires a separately trusted web-UI
leaf certificate; redirects and credential-bearing URLs are refused. A scenario
cannot fall back to manual delivery, and ambiguous Wolf pending requests fail.

T013's malformed certificate, signature, challenge, wrong-PIN and final-rejection
fixtures are exercised alongside a generated loopback TLS server requiring the
client certificate. The final fixture covers a matching host certificate,
a substituted certificate, and a rejected `paired` value, including persistence
outcomes. These are synthetic fixtures, not an official-client execution or
reference-host observation. T038 remains responsible for real-host and
versioned official-client cross-validation, including provider endpoints,
application lists and reconnect after process restart.

### T038 live Fenrir app-list cross-check

On 2026-10-25 the deployed Fenrir proxy `5c8251c` returned `IsHdrSupported` text
`false` for both Cove (ID 4) and EmuDeck (ID 2). The client previously rejected
that whole list as `invalid protocol boolean`. Official Moonlight Qt 6.1.0 at
[f786e94c7b2f943e24e65d7d74deb539b827fc84, nvhttp.cpp:318–319](https://github.com/moonlight-stream/moonlight-qt/blob/f786e94c7b2f943e24e65d7d74deb539b827fc84/app/backend/nvhttp.cpp#L318)
sets app `hdrSupported` to `readElementText() == "1"`: only exact `1` is true;
all other text, empty content and absence are false rather than a parse error.
The client now follows that field-specific behavior, including whitespace;
`PairStatus` still uses its strict numeric parser. The sanitized observed XML
regression failed before the fix and passes after it. This does not establish
host HDR capability, HDR metadata correctness or stream fidelity.

### T038 live RTSP, audio and cancellation corrections

The deployed Wolf/Fenrir DESCRIBE response carried a 195-byte SDP body without a
Content-Length header. [moonlight-common-c 41cedb89d6b8592f99c09975c764ca3875034726,
RtspParser.c:95–103](https://github.com/moonlight-stream/moonlight-common-c/blob/41cedb89d6b8592f99c09975c764ca3875034726/src/RtspParser.c#L95)
uses the remaining bounded message bytes as payload. The client now accepts that
connection-close-delimited case; explicit length mismatch, duplicate headers and
response bounds still fail. The absent-length regression failed before correction.

Audio encryption has a legacy opt-in independent of Sunshine extension support:
[SdpGenerator.c:187–201](https://github.com/moonlight-stream/moonlight-common-c/blob/41cedb89d6b8592f99c09975c764ca3875034726/src/SdpGenerator.c#L187)
sets NV feature flag 0x20 when the client opts in, even if no x-ss encryption flags
are advertised. [Moonlight Qt session.cpp:648–658](https://github.com/moonlight-stream/moonlight-qt/blob/f786e94c7b2f943e24e65d7d74deb539b827fc84/app/streaming/session.cpp#L648)
opts into at least audio encryption on non-Steam-Link platforms. The runner now
opts in, announces NV flag 167 on legacy hosts, and enables AES-CBC audio decoding
from that same opt-in. It does not advertise unsupported x-ss extensions. The
regression failed with flag 135 before correction; live decode failures fell from
1912 of 1984 packets to zero of 1981 packets in the next ten-second observation.
Digital silence in that later population is retained as silence, not non-silence.

Fenrir's successful cancel response has only root status 200. Official
[Moonlight Qt nvhttp.cpp:239–258](https://github.com/moonlight-stream/moonlight-qt/blob/f786e94c7b2f943e24e65d7d74deb539b827fc84/app/backend/nvhttp.cpp#L239)
checks response status then current game rather than requiring a cancel element.
The client now follows that behavior and still rejects a subsequent busy game.
A generated loopback TLS fixture establishes both cases. Fenrir can still report
its terminating pod busy during that immediate check; such cancellation remains
unconfirmed in the report until independent cleanup evidence exists.

A separate receipt-report defect used `machine-monotonic` instead of the actual
boot-scoped local clock ID, so valid frame/audio populations failed validation.
The positive receipt regression now keeps the source clock ID and measurement
domain identical, and frame-rate values explicitly use `frames_per_second` in
the Rust model, evaluator and evidence schema. These changes establish software
correctness; the missing host/source timing and calibration remain unavailable.
