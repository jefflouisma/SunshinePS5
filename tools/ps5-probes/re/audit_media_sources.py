#!/usr/bin/env python3
"""Check public media-interface facts offline; never load or call console code."""

import argparse
import base64
import ctypes
import hashlib
import json
import re
from pathlib import Path

SALT = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
AUDIO_NAMES = (
    "sceAvcap2Initialize",
    "sceAvcap2Terminate",
    "sceAvcap2OpenAudio",
    "sceAvcap2Close",
    "sceAvcap2Start",
    "sceAvcap2Stop",
    "sceAvcap2ReadAudio",
)
MODULES = {
    "libSceAudioSystem.sprx": "0x80000083",
    "libSceVenc.sprx": "0x80000084",
    "libSceAvcap2.sprx": "0x80000085",
    "libSceShareInternal.native.sprx": "0x8000008e",
    "libSceRemotePlayClientIpc.sprx": "0x800000b9",
}


class AudioRequest(ctypes.LittleEndianStructure):
    """Public caller's field widths, independently expressed for offset checks."""

    _fields_ = [
        ("size", ctypes.c_uint64),
        ("unknown08", ctypes.c_uint64),
        ("unknown10", ctypes.c_uint32),
        ("source", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
        ("unknown1c", ctypes.c_uint32),
        ("unknown20", ctypes.c_uint64),
        ("bitrate", ctypes.c_uint32),
        ("reserved2c", ctypes.c_uint32),
        ("unknown30", ctypes.c_uint64),
    ]


def nid(name):
    digest = hashlib.sha1(name.encode("ascii") + SALT).digest()[:8][::-1]
    return base64.b64encode(digest).decode("ascii").replace("/", "-")[:11]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", type=Path, required=True)
    parser.add_argument("--audio-source", type=Path, action="append", required=True)
    args = parser.parse_args()
    errors = []
    sources = []
    loader = args.sdk_source / "crt/rtld_sprx.c"
    try:
        loader_text = loader.read_text()
        for module, module_id in MODULES.items():
            if not re.search(
                r'\{"' + re.escape(module) + r'",\s*' + module_id + r"\}",
                loader_text,
            ):
                errors.append(f"module-table mismatch: {module}")
        for path in args.audio_source:
            data = path.read_bytes()
            source = data.decode("utf-8")
            missing = [
                name
                for name in AUDIO_NAMES
                if (
                    name not in source
                    and "NID_" + name.removeprefix("sce") not in source
                )
                or nid(name) not in source
            ]
            if missing:
                errors.append(f"missing public symbol/NID pairs in {path}: {missing}")
            sources.append(
                {"path": str(path), "sha256": hashlib.sha256(data).hexdigest()}
            )
    except (OSError, UnicodeError) as error:
        errors.append(str(error))
    offsets = {
        name: getattr(AudioRequest, name).offset for name, _ in AudioRequest._fields_
    }
    if ctypes.sizeof(AudioRequest) != 0x38 or offsets["source"] != 0x14:
        errors.append("local ctypes cannot represent the public 64-bit request layout")
    print(
        json.dumps(
            {
                "scope": "offline source consistency; no firmware ABI or hardware pass",
                "audio_nids": {name: nid(name) for name in AUDIO_NAMES},
                "module_ids": MODULES,
                "request_size": ctypes.sizeof(AudioRequest),
                "request_offsets": offsets,
                "sources": sources,
                "errors": errors,
            },
            indent=2,
        )
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
