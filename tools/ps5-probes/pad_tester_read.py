"""Read the pad tester stream (TCP 9041); print decoded ScePadData changes.

usage: python3 pad_tester_read.py [seconds] [output-file]
PAD_TESTER_HOST overrides the console address.
"""

import os
import socket
import struct
import sys
import time

HOST = os.environ.get("PAD_TESTER_HOST", "192.168.1.20")


def decode(raw):
    d = bytes.fromhex(raw)
    (buttons,) = struct.unpack_from("<I", d, 0)
    lx, ly, rx, ry, l2, r2 = d[4:10]
    quat = struct.unpack_from("<4f", d, 0x0C)
    accel = struct.unpack_from("<3f", d, 0x1C)
    gyro = struct.unpack_from("<3f", d, 0x28)
    count = d[0x34]
    touches = []
    for i in range(min(count, 2)):
        x, y = struct.unpack_from("<HH", d, 0x3C + 8 * i)
        touches.append((d[0x40 + 8 * i], x, y))
    return {
        "buttons": f"0x{buttons:08x}",
        "sticks": (lx, ly, rx, ry),
        "triggers": (l2, r2),
        "touch": (count, touches),
        "accel_g": tuple(round(v, 3) for v in accel),
        "gyro_rad_s": tuple(round(v, 3) for v in gyro),
        "quat": tuple(round(v, 3) for v in quat),
        "connected": d[0x4C],
    }


def run(out, seconds):
    sock = socket.create_connection((HOST, 9041), timeout=5)
    sock.settimeout(1)
    buffer = b""
    last = None
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = sock.recv(65536)
        except TimeoutError:
            continue
        if not chunk:
            break
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            parts = line.decode().split()
            if len(parts) != 4 or parts[0] != "PAD":
                continue
            state = decode(parts[3])
            state["rc"] = parts[2]
            if state != last:
                print(f"{time.time():.3f} {state}", file=out, flush=True)
                last = state
    sock.close()


seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
if len(sys.argv) > 2:
    with open(sys.argv[2], "w") as target:
        run(target, seconds)
else:
    run(sys.stdout, seconds)
