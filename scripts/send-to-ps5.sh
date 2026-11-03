#!/usr/bin/env bash
# Send the SunshinePS5 host ELF to a jailbroken PS5's payload loader and stream
# its log (JSON lines on stdout) back to this terminal.
#
# usage: scripts/send-to-ps5.sh PS5_ADDRESS [ELF] [LOADER_PORT]
#   ELF          default: sunshineps5.elf next to this script, else dist/sunshineps5-stream.elf
#   LOADER_PORT  default: 9021 (elfldr / etaHEN payload loader)
#
# The connection stays open while the host runs; closing it (Ctrl+C) does NOT
# stop the host on the console.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
host=${1:-${PS5_HOST:-}}
elf=${2:-}
port=${3:-${PS5_PORT:-9021}}

if [[ -z $host ]]; then
    printf 'usage: %s PS5_ADDRESS [ELF] [LOADER_PORT]\n' "$(basename -- "$0")" >&2
    exit 64
fi
if [[ -z $elf ]]; then
    for candidate in "$here/sunshineps5.elf" "$here/../dist/sunshineps5-stream.elf"; do
        if [[ -s $candidate ]]; then
            elf=$candidate
            break
        fi
    done
fi
[[ -n $elf && -s $elf ]] || { printf 'ELF not found: %s\n' "${elf:-<none>}" >&2; exit 66; }
[[ $(od -An -tx1 -N4 "$elf" | tr -d ' \n') == 7f454c46 ]] ||
    { printf 'not an ELF file: %s\n' "$elf" >&2; exit 65; }

printf 'Sending %s to %s:%s\n' "$elf" "$host" "$port" >&2
command -v socat >/dev/null 2>&1 ||
    { printf 'socat is required (macOS: brew install socat; Linux: apt install socat)\n' >&2; exit 69; }
# -t keeps reading the payload's stdout after the ELF upload half-closes.
exec socat -t 9999999 - "TCP:$host:$port" < "$elf"
