#!/usr/bin/env bash
# Scarica OpenOrbis v0.5.4, mbedTLS 3.6.2 e libdatachannel 0.23.1 (con sottomoduli).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OO_PARENT="${OO_PARENT:-$HOME/ps4dev/OpenOrbis}"
OO_URL="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz"

if [[ ! -x "$OO_PARENT/OpenOrbis/PS4Toolchain/bin/linux/create-fself" && ! -x "$OO_PARENT/PS4Toolchain/bin/linux/create-fself" ]]; then
    echo "scarico OpenOrbis v0.5.4"
    mkdir -p "$OO_PARENT"
    tmp="$(mktemp)"
    curl -L --retry 3 -o "$tmp" "$OO_URL"
    tar -xzf "$tmp" -C "$OO_PARENT"
    rm -f "$tmp"
fi

if [[ -x "$OO_PARENT/OpenOrbis/PS4Toolchain/bin/linux/create-fself" ]]; then
    export OO_PS4_TOOLCHAIN="$OO_PARENT/OpenOrbis/PS4Toolchain"
else
    export OO_PS4_TOOLCHAIN="$OO_PARENT/PS4Toolchain"
fi
echo "OO_PS4_TOOLCHAIN=$OO_PS4_TOOLCHAIN"

if [[ ! -f "$ROOT/deps/mbedtls/CMakeLists.txt" ]]; then
    git clone --depth 1 --branch v3.6.2 https://github.com/Mbed-TLS/mbedtls.git "$ROOT/deps/mbedtls"
fi
# Il clone superficiale non porta il sottomodulo framework, richiesto da CMake.
if [[ ! -f "$ROOT/deps/mbedtls/framework/CMakeLists.txt" ]]; then
    git -C "$ROOT/deps/mbedtls" submodule update --init --depth 1 framework
fi
if [[ ! -f "$ROOT/deps/libdatachannel/CMakeLists.txt" ]]; then
    git clone --depth 1 --branch v0.23.1 --recurse-submodules \
        https://github.com/paullouisageneau/libdatachannel.git "$ROOT/deps/libdatachannel"
fi
if [[ ! -f "$ROOT/third_party/opus/CMakeLists.txt" ]]; then
    git clone --depth 1 --branch v1.5.2 https://github.com/xiph/opus.git "$ROOT/third_party/opus"
fi
