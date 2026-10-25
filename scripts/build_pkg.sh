#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# CI / host: build PSBox-PS4-01.57.pkg on the XCloud4 streaming base.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v ld.lld >/dev/null 2>&1 && ! command -v ld.lld-18 >/dev/null 2>&1; then
    echo "manca ld.lld: sudo apt-get install -y clang lld-18 llvm-18 ninja-build cmake" >&2
    exit 1
fi
if [[ ! -e /usr/lib/x86_64-linux-gnu/libssl.so.1.1 && ! -e /lib/x86_64-linux-gnu/libssl.so.1.1 ]]; then
    echo "manca libssl.so.1.1 (PkgTool)" >&2
    exit 1
fi

# OpenOrbis — reuse PSBox cache layout when present.
OO_ROOT="${OO_PS4_TOOLCHAIN:-}"
if [[ -z "$OO_ROOT" ]]; then
    if [[ -d "$HOME/ps4dev/OpenOrbis/PS4Toolchain" ]]; then
        OO_ROOT="$HOME/ps4dev/OpenOrbis/PS4Toolchain"
    elif [[ -d "$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain" ]]; then
        OO_ROOT="$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain"
    fi
fi
if [[ -z "$OO_ROOT" || ! -d "$OO_ROOT" ]]; then
    echo "Install OpenOrbis v0.5.4 and set OO_PS4_TOOLCHAIN" >&2
    # Bootstrap like legacy PSBox CI: download into ~/ps4dev/OpenOrbis
    mkdir -p "$HOME/ps4dev"
    if [[ ! -d "$HOME/ps4dev/OpenOrbis/PS4Toolchain" ]]; then
        echo "Fetching OpenOrbis v0.5.4…"
        curl -fsSL -o /tmp/oo-toolchain.tar.gz \
            "https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz"
        mkdir -p "$HOME/ps4dev/OpenOrbis"
        tar -xzf /tmp/oo-toolchain.tar.gz -C "$HOME/ps4dev/OpenOrbis"
        # Archive layout varies; locate PS4Toolchain
        if [[ ! -d "$HOME/ps4dev/OpenOrbis/PS4Toolchain" ]]; then
            found=$(find "$HOME/ps4dev/OpenOrbis" -maxdepth 3 -type d -name PS4Toolchain | head -1 || true)
            if [[ -n "$found" ]]; then
                ln -sfn "$found" "$HOME/ps4dev/OpenOrbis/PS4Toolchain"
            fi
        fi
    fi
    OO_ROOT="$HOME/ps4dev/OpenOrbis/PS4Toolchain"
fi
export OO_PS4_TOOLCHAIN="$OO_ROOT"
export PATH="$OO_PS4_TOOLCHAIN/bin/linux:${PATH:-}"

# Runtime SELF modules (OpenOrbis sample copies — same as legacy PSBox make_pkg).
export X4_RUNTIME_MODULES="${X4_RUNTIME_MODULES:-$OO_PS4_TOOLCHAIN/samples/hello_world/sce_module}"
if [[ ! -s "$X4_RUNTIME_MODULES/libc.prx" ]]; then
    echo "Missing $X4_RUNTIME_MODULES/libc.prx" >&2
    exit 1
fi

# WebRTC static deps (XCloud4 prepare_port + pins — not PSBox CMake patches).
export X4_WEBRTC_ROOT="${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}"
if [[ ! -f "$X4_WEBRTC_ROOT/build-datachannel/libdatachannel-static.a" ]]; then
    echo "Building pinned WebRTC stack (libdatachannel 0.24.6 + juice b89c792)…"
    bash "$ROOT/scripts/webrtc/build_native.sh"
fi

# PSBox package on public stream profile.
export X4_PACKAGE_MODE=psbox
export X4_PUBLIC=1
export X4_STREAM_PROFILE=1
rm -rf build/package-psbox
make -j"$(nproc 2>/dev/null || echo 2)" \
    X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=1 \
    X4_INGRESS_MODE=mpsc X4_VIDEO_CAPACITY_1080=1 X4_VIDEO_PROBE=0 \
    X4_STREAM_PROFILE=1 X4_PUBLIC=1 all
X4_PACKAGE_MODE=psbox X4_STREAM_PROFILE=1 X4_PUBLIC=1 bash scripts/empaquetar.sh
python3 scripts/package_identity.py verify --mode psbox \
    --self build/eboot.bin >/dev/null
ls -lh dist/PSBox-PS4-01.57.pkg
sha256sum dist/PSBox-PS4-01.57.pkg
