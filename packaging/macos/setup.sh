#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ $OSTYPE == darwin* ]] || { echo 'macOS setup requires macOS.' >&2; exit 1; }
[[ $(/usr/sbin/sysctl -n hw.optional.arm64) == 1 ]] || {
    echo 'This experimental macOS port requires Apple Silicon and Rosetta 2.' >&2; exit 1;
}
/usr/bin/xcrun --find clang >/dev/null
/usr/bin/arch -x86_64 /usr/bin/true || {
    echo 'Rosetta 2 is required; install it before running setup.' >&2; exit 1;
}
host_brew=${BB_HOST_BREW_PREFIX:-/opt/homebrew}
[[ -x $host_brew/bin/brew ]] || {
    echo "Install native ARM Homebrew first; expected $host_brew/bin/brew." >&2; exit 1;
}
export PATH="$host_brew/bin:$PATH"
export BB_DEPS_PREFIX=${BB_DEPS_PREFIX:-$HOME/bbport-deps/x86_64}
python_prefix=${BB_PYTHON_PREFIX:-$HOME/bbport-deps/python-arm64}
missing=()
for formula in cmake ninja pkg-config glslang boost robin-map python@3.14 ffmpeg; do
    if ! "$host_brew/bin/brew" list --versions "$formula" >/dev/null 2>&1; then
        missing+=("$formula")
    fi
done
if (( ${#missing[@]} )); then
    printf 'Installing missing Homebrew formulas:'
    printf ' %s' "${missing[@]}"
    printf '\n'
    HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 \
        HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 \
        "$host_brew/bin/brew" install "${missing[@]}"
else
    echo 'All required Homebrew formulas are installed; keeping existing versions.'
fi
export PATH="$host_brew/opt/python@3.14/libexec/bin:$PATH"
bash packaging/macos/build-deps.sh
python3 packaging/macos/fetch-python.py --prefix "$python_prefix"
export PYTHON="$python_prefix/bin/python3"
export PATH="$python_prefix/bin:$PATH"
bash build.sh --test
out/bb-probe --verify-macos-host
echo "macOS contributor setup complete. Launch with: $PYTHON launcher/bbport_launcher_mac.py"
