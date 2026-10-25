# Building XCloud4 from Source

This document describes how to build both **XCloud4 Public Beta** (`X4_PUBLIC=1`) and **XCloud4 Lab** (`X4_PUBLIC=0`) from this source tree on an `x86_64` Linux host.

---

## 1. Host Prerequisites

Install the standard build tools on an `x86_64` Ubuntu/Lubuntu host (tested on Ubuntu 26.04 LTS with Clang/LLD 21):

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential clang lld make cmake ninja-build \
    git python3 curl ca-certificates pkg-config unzip
```

### OpenOrbis PS4 Toolchain v0.5.4

1. Download the official OpenOrbis v0.5.4 release archive (`toolchain-llvm-18.tar.gz`, SHA-256 `3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`) from [OpenOrbis-PS4-Toolchain v0.5.4](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/tag/v0.5.4).
2. Extract it to `~/.local/share/xcloud4/OpenOrbis/PS4Toolchain`.
3. Create `~/.config/xcloud4/env.sh`:

```bash
mkdir -p ~/.config/xcloud4
cat > ~/.config/xcloud4/env.sh <<'EOF'
export OO_PS4_TOOLCHAIN="$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain"
export PATH="$HOME/.local/share/xcloud4/bin:$OO_PS4_TOOLCHAIN/bin/linux:$PATH"
EOF
source ~/.config/xcloud4/env.sh
```

---

## 2. Third-Party Dependencies and Packaging Runtime

### Isolated OpenSSL 1.1 for `PkgTool.Core`

OpenOrbis's `PkgTool.Core` (.NET Core 3.0) requires `libssl.so.1.1`. Extract it into an isolated local directory (`~/.local/share/xcloud4/hostlibs`) without installing it system-wide:

```bash
bash scripts/preparar-empaquetador.sh
```

### Pinned WebRTC, Mbed TLS, and Opus Static Libraries

Build the pinned static dependencies (`libdatachannel 0.24.6`, `libjuice 1.7.4`, `usrsctp`, `libsrtp`, `Mbed TLS 3.6.7`, and `Opus 1.5.2`) into `~/.local/share/xcloud4/webrtc`:

```bash
source ~/.config/xcloud4/env.sh
bash scripts/webrtc/build_native.sh
```

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [docs/licenses/](docs/licenses/) for exact upstream commit hashes, licenses, and OpenOrbis port patches (`scripts/webrtc/prepare_port.py`, `scripts/webrtc/prepare_receive_diagnostics.py`, `scripts/webrtc/prepare_receiver_nack.py`).

### OpenOrbis Auxiliary SELF Modules

The PS4 loader requires the open-source OpenOrbis auxiliary modules `libSceFios2.prx` and `libc.prx` in SELF format (built from OpenOrbis v0.5.4 `src/modules`). Place them in `~/.local/share/xcloud4/runtime/sdk-v0.5.4` and export:

```bash
export X4_RUNTIME_MODULES="$HOME/.local/share/xcloud4/runtime/sdk-v0.5.4"
```

---

## 3. Building and Packaging

Both editions are compiled and packaged from the root of this repository after loading `~/.config/xcloud4/env.sh` and setting `X4_RUNTIME_MODULES`.

### A. Public Beta (`X4_PUBLIC=1` -> `dist/XCloud4-1.1.0-beta-publico.pkg`)

```bash
source ~/.config/xcloud4/env.sh
export X4_RUNTIME_MODULES="$HOME/.local/share/xcloud4/runtime/sdk-v0.5.4"

rm -rf build dist
make -j2 X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=1 \
    X4_INGRESS_MODE=mpsc X4_VIDEO_CAPACITY_1080=1 X4_VIDEO_PROBE=0 \
    X4_STREAM_PROFILE=1 X4_PUBLIC=1 all

X4_PACKAGE_MODE=public X4_STREAM_PROFILE=1 X4_PUBLIC=1 \
    bash scripts/empaquetar.sh

python3 scripts/package_identity.py verify --mode public
```

Output package:
- `dist/XCloud4-1.1.0-beta-publico.pkg` (`TITLE`: `XCloud4`, `TITLE_ID`: `XCLD00003`, `APP_VER`: `01.04`, icon: `assets/icon0.png`)

### B. Experimental Lab (`X4_PUBLIC=0` -> `dist/XCloud4-0.7.55-lab.pkg`)

```bash
source ~/.config/xcloud4/env.sh
export X4_RUNTIME_MODULES="$HOME/.local/share/xcloud4/runtime/sdk-v0.5.4"

rm -rf build dist
make -j2 X4_RECEIVER_NACK=1 X4_NACK_GRACE_MS=45 X4_RUMBLE=1 \
    X4_INGRESS_MODE=mpsc X4_VIDEO_CAPACITY_1080=1 X4_VIDEO_PROBE=0 \
    X4_STREAM_PROFILE=1 X4_PUBLIC=0 all

X4_PACKAGE_MODE=normal X4_PACKAGE_SUFFIX="" X4_STREAM_PROFILE=1 X4_PUBLIC=0 \
    bash scripts/empaquetar.sh

python3 scripts/package_identity.py verify --mode normal --suffix=""
```

Output package:
- `dist/XCloud4-0.7.55-lab.pkg` (`TITLE`: `XCloud4 Lab`, `TITLE_ID`: `XCLD00001`, `APP_VER`: `01.23`, icon: `assets/icon0-lab.png`)

---

## 4. Build Audits and Host Tests

During `make all`, the Makefile automatically runs two static ELF/stack gates before creating `build/eboot.bin`:
- `scripts/audit_stream_posix_elf.py`: verifies that no forbidden stubbed POSIX imports are reachable from the active streaming entry points.
- `scripts/prepare_stream_stack_policy.py` and `scripts/audit_probe_stack.py`: verifies `-fstack-usage` (`*.su`) across all compiled translation units and ensures zero dynamic stack allocations (`-Werror=vla -Werror=alloca`).

Portable host tests under `tests/` can be executed via their corresponding runner scripts in `scripts/test_*.sh`.
