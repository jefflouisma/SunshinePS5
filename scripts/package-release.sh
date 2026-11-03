#!/usr/bin/env bash
# Package a built SunshinePS5 host into dist/sunshineps5-<version>.tar.gz.
#
# Run scripts/build-ps5-stream.sh first. This only packages; it never builds,
# deploys or contacts a console. The tarball contains the ELF, its SHA-256,
# the send helper, README, LICENSE and release notes.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=${SP5_VERSION:-$(tr -d '[:space:]' < "$repo/VERSION")}
elf=${PS5_OUTPUT:-$repo/dist/sunshineps5-stream.elf}
name=sunshineps5-$version
stage=$repo/build/release/$name
archive=$repo/dist/$name.tar.gz

fail() {
    printf 'PACKAGE_FAILED %s\n' "$*" >&2
    exit 1
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

[[ -s $elf ]] || fail "missing $elf (run scripts/build-ps5-stream.sh)"
[[ $(od -An -tx1 -N4 "$elf" | tr -d ' \n') == 7f454c46 ]] || fail "not an ELF: $elf"
grep -aq "sunshineps5-$version" "$elf" ||
    fail "$elf was not built as version $version (rebuild with scripts/build-ps5-stream.sh)"
notes=$repo/docs/releases/v$version.md
[[ -s $notes ]] || fail "missing release notes: $notes"

rm -rf "$stage"
mkdir -p "$stage"
cp "$elf" "$stage/sunshineps5.elf"
cp "$repo/scripts/send-to-ps5.sh" "$stage/send-to-ps5.sh"
cp "$repo/README.md" "$repo/LICENSE" "$stage/"
cp "$notes" "$stage/RELEASE-NOTES.md"
chmod 0755 "$stage/sunshineps5.elf" "$stage/send-to-ps5.sh"
(cd "$stage" && sha256_file sunshineps5.elf > sunshineps5.elf.sha256)

(
    cd "$repo/build/release"
    find "$name" -type f -print | LC_ALL=C sort | tar -c -z -f "$archive" -T -
)
(cd "$repo/dist" && sha256_file "$name.tar.gz" > "$name.tar.gz.sha256")

printf 'PACKAGE=%s\n' "$archive"
cat "$stage/sunshineps5.elf.sha256" "$repo/dist/$name.tar.gz.sha256"
