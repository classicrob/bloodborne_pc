#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ $(uname -s) == Darwin ]] || { echo 'MetalFX signature qualification requires macOS.' >&2; exit 1; }
[[ $# == 0 ]] || { echo 'This test takes no arguments.' >&2; exit 1; }
mkdir -p out/macos-metalfx/signature
for cpu in arm64 x86_64; do
    binary="out/macos-metalfx/signature/support-$cpu"
    clang++ -arch "$cpu" -std=c++20 -O2 -fobjc-arc -Wall -Wextra -Werror \
        tests/macos_metalfx/support.mm -framework Foundation -framework Metal \
        -framework MetalFX -o "$binary"
    python3 packaging/macos/sign.py "$binary"
    /usr/bin/arch "-$cpu" "$binary" > "$binary.log" 2>&1
    cat "$binary.log"
    if rg -q 'com.apple.appleneuralengine|Code=26' "$binary.log"; then
        echo "Signed $cpu scaler creation reported an ANE error." >&2; exit 1
    fi
done
unsigned=out/macos-metalfx/signature/support-x86_64-unsigned
cp out/macos-metalfx/signature/support-x86_64 "$unsigned"
# Deliberate negative configuration, never used by the provider or launcher.
codesign --remove-signature "$unsigned"
/usr/bin/arch -x86_64 "$unsigned" > "$unsigned.log" 2>&1
cat "$unsigned.log"
if ! rg -q 'com.apple.appleneuralengine.*Code=26' "$unsigned.log"; then
    echo 'Unsigned diagnostic did not reproduce the signature-dependent ANE error; review the finding.' >&2
    exit 1
fi
echo 'PASS: signed ARM and Rosetta scaler creation clean; unsigned Rosetta Code26 reproduced.'
prefix=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
gate=out/macos-metalfx/signature/module-gate
clang++ -arch x86_64 -std=c++20 -O2 -fobjc-arc -Wall -Wextra -Werror \
    -I"$prefix/include" tests/macos_metalfx/signature_gate.mm gpu/shim/macos_metalfx.mm \
    -framework Foundation -framework Metal -framework MetalFX -framework Security -o "$gate"
python3 packaging/macos/sign.py "$gate"
cp "$gate" "$gate-unsigned"
codesign --remove-signature "$gate-unsigned"
ulimit -c 0
for configuration in signed unsigned; do
    binary=$gate
    [[ $configuration == signed ]] || binary="$gate-unsigned"
    if /usr/bin/arch -x86_64 "$binary" > "$gate-$configuration.log" 2>&1; then
        echo "Invalid $configuration module configuration unexpectedly succeeded." >&2; exit 1
    fi
done
rg -q 'device and two distinct exportable timeline semaphores are required' "$gate-signed.log"
rg -q 'host signature invalid or missing' "$gate-unsigned.log"
echo 'PASS: actual module rejects unsigned host before device validation/ANE creation.'
