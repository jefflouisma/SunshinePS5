#!/usr/bin/env bash
set -euo pipefail
probe_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_output=${1:?usage: build-recovery-inspector.sh OUTPUT_DIRECTORY}
probe_sdk=${PS5_PAYLOAD_SDK:-/Users/jefflouisma/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk}
probe_target_ssl=${RECOVERY_INSPECTOR_OPENSSL:-/Volumes/4TB_Drive/tmp/t043-t044/openssl-source/openssl-3.5.9}
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
mkdir -p "$probe_output"
probe_hash() {
    shasum -a 256 "$1" | awk '{print $1}'
}
[[ $(probe_hash "$probe_sdk/bin/prospero-clang") == 3dcf3139662db7b0f796c2073bc8f673a5557e48bc24d2e413896d33847a69db ]]
[[ $(probe_hash "$probe_sdk/target/lib/crt1.o") == 5a5ba34cc55f8325517ac897db868570779dc80972508d424f1a251ef29b5b05 ]]
[[ $(probe_hash "$probe_target_ssl/include/openssl/sha.h") == 7e006accd6565cc97dee672c097acc5e38482be7cd4fa1e0c316bf466425b895 ]]
[[ $(probe_hash "$probe_target_ssl/libcrypto.a") == 30b8b7af17a0a9bec99e67474dae6f4f3df46de6ab1b1e7ed3fe10d88ad32629 ]]
clang -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -DRECOVERY_INSPECTOR_HOST "$probe_root/recovery_inspector.c" -o "$probe_output/recovery-inspector-host"
clang -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror -Wno-deprecated-declarations \
    "$probe_root/test_recovery_inspector_faults.c" -o "$probe_output/recovery-inspector-faults"
for probe_mode in normal ndebug; do
    probe_defines=()
    if [[ $probe_mode == ndebug ]]; then
        probe_defines=(-DNDEBUG)
    fi
    clang -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
        "${probe_defines[@]}" -DRECOVERY_INSPECTOR_LAUNCHER_TEST \
        "$probe_root/recovery_inspector_launcher.c" \
        "$probe_root/test_recovery_inspector_launcher.c" \
        -o "$probe_output/recovery-inspector-launcher-$probe_mode"
    "$probe_output/recovery-inspector-launcher-$probe_mode"
done
"$probe_sdk/bin/prospero-clang" -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror \
    -Wno-deprecated-declarations -I"$probe_target_ssl/include" \
    -c "$probe_root/recovery_inspector.c" -o "$probe_output/recovery-inspector-target.o"
# Use the complete pinned SDK CRT. It establishes the loader/libc contract and
# performs process-local credential/syscall bootstrap before launcher main.
# This build alone is not approval to deploy the artifact.
"$probe_sdk/bin/prospero-clang" -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror \
    -Wno-deprecated-declarations -I"$probe_target_ssl/include" \
    "$probe_root/recovery_inspector.c" "$probe_root/recovery_inspector_launcher.c" \
    "$probe_target_ssl/libcrypto.a" -pthread -o "$probe_output/recovery-inspector-target.elf"
shasum -a 256 "$probe_root/recovery_inspector.c" \
    "$probe_root/recovery_inspector_launcher.c" \
    "$probe_root/test_recovery_inspector_launcher.c" \
    "$probe_output/recovery-inspector-host" \
    "$probe_output/recovery-inspector-target.o" \
    "$probe_output/recovery-inspector-target.elf"
