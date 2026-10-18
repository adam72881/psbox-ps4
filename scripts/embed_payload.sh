#!/usr/bin/env bash
# Compila lo shellcode del BinLoader e lo scrive come header C.
# Uso: embed_payload.sh <output.h>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?output header}"
LLD="$(command -v ld.lld || command -v ld.lld-18 || true)"
OBJCOPY="$(command -v llvm-objcopy || command -v llvm-objcopy-18 || true)"
READELF="$(command -v llvm-readelf || command -v llvm-readelf-18 || true)"
if [[ -z "$LLD" || -z "$OBJCOPY" || -z "$READELF" ]]; then
    echo "servono ld.lld, llvm-objcopy e llvm-readelf" >&2
    exit 1
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
clang --target=x86_64-pc-freebsd12-elf -ffreestanding -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-builtin -fPIC -fno-plt -mno-red-zone \
    -fvisibility=hidden -Os -Wall -Wextra -nostdlib \
    -c "$ROOT/src/payload/psbox_install.c" -o "$WORK/psbox_install.o"
"$LLD" -m elf_x86_64 -nostdlib -static -e _start \
    -T "$ROOT/src/payload/payload.ld" \
    -o "$WORK/psbox-install.elf" "$WORK/psbox_install.o"
"$OBJCOPY" -O binary "$WORK/psbox-install.elf" "$WORK/psbox-install.bin"
python3 - "$WORK/psbox-install.elf" "$WORK/psbox-install.bin" "$OUT" "$READELF" <<'PY'
import subprocess, sys
elf, raw, out, readelf = sys.argv[1:]
b = open(raw, "rb").read()
if b[:4] == b"\x7fELF":
    raise SystemExit("il .bin è ancora un ELF")
if not b or b[:4] == b"\x00\x00\x00\x00":
    raise SystemExit("il .bin non inizia con _start")
rel = subprocess.check_output([readelf, "-r", elf], text=True, stderr=subprocess.STDOUT)
body = [ln for ln in rel.splitlines() if ln.strip() and not ln.startswith("Relocation") and "Offset" not in ln and not ln.startswith("There are no")]
if body:
    raise SystemExit("rilocazioni nel payload:\n" + rel)
lines = ["#pragma once", "/* generato da scripts/embed_payload.sh — non modificare a mano */",
         "static const unsigned char kPsboxInstallBin[] = {"]
for i in range(0, len(b), 16):
    chunk = b[i:i+16]
    lines.append("    " + ", ".join("%d" % x for x in chunk) + ",")
lines.append("};")
lines.append("static const unsigned kPsboxInstallBinLen = %d;" % len(b))
open(out, "w").write("\n".join(lines) + "\n")
print("payload embedded: %d byte, primi %s" % (len(b), b[:8].hex()))
PY
