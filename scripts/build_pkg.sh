#!/usr/bin/env bash
# Configura, compila e impacchetta PSBox per PS4 (GoldHEN / OpenOrbis).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v ld.lld >/dev/null 2>&1 && ! command -v ld.lld-18 >/dev/null 2>&1; then
    echo "manca ld.lld: sudo apt-get install -y clang lld-18 llvm-18 ninja-build cmake" >&2
    exit 1
fi
# PkgTool.Core è un runtime .NET che apre libssl.so.1.1. Ubuntu 24 ha solo OpenSSL 3.
if [[ ! -e /usr/lib/x86_64-linux-gnu/libssl.so.1.1 && ! -e /lib/x86_64-linux-gnu/libssl.so.1.1 ]]; then
    echo "manca libssl.so.1.1 (serve a PkgTool). Esempio:" >&2
    echo "  curl -fsSL -o /tmp/libssl1.1.deb http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb && sudo dpkg -i /tmp/libssl1.1.deb" >&2
    exit 1
fi
# shellcheck source=fetch_deps.sh
source "$ROOT/scripts/fetch_deps.sh"

BUILD="${BUILD_DIR:-$ROOT/build-ps4}"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/openorbis.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target pkg
"$ROOT/scripts/build_payload.sh"
echo "Pacchetto in $ROOT/dist"
ls -lh "$ROOT/dist/"*.pkg "$ROOT/dist/psbox-install.elf"
