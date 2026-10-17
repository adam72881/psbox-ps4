#!/usr/bin/env bash
# Shellcode raw per il BinLoader di GoldHEN (TCP 9090).
# psbox-install.bin non è un ELF: il primo byte è _start.
# psbox-install.elf è lo stesso codice, solo per readelf. Non va inviato.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLD="$(command -v ld.lld || command -v ld.lld-18 || true)"
OBJCOPY="$(command -v llvm-objcopy || command -v llvm-objcopy-18 || true)"
if [[ -z "$LLD" || -z "$OBJCOPY" ]]; then
    echo "servono ld.lld e llvm-objcopy" >&2
    exit 1
fi
mkdir -p "$ROOT/build-ps4" "$ROOT/dist"
clang --target=x86_64-pc-freebsd12-elf -ffreestanding -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-builtin -fPIC -fno-plt -mno-red-zone \
    -fvisibility=hidden -Os -Wall -Wextra -nostdlib \
    -c "$ROOT/src/payload/psbox_install.c" -o "$ROOT/build-ps4/psbox_install.o"
"$LLD" -m elf_x86_64 -nostdlib -static -e _start \
    -T "$ROOT/src/payload/payload.ld" \
    -o "$ROOT/dist/psbox-install.elf" "$ROOT/build-ps4/psbox_install.o"
"$OBJCOPY" -O binary "$ROOT/dist/psbox-install.elf" "$ROOT/dist/psbox-install.bin"
python3 - "$ROOT/dist/psbox-install.elf" "$ROOT/dist/psbox-install.bin" <<'PY'
import subprocess, sys
elf, raw = sys.argv[1], sys.argv[2]
b = open(raw, "rb").read(16)
if b[:4] == b"\x7fELF":
    raise SystemExit("il .bin è ancora un ELF")
if b[:4] == b"\x00\x00\x00\x00":
    raise SystemExit("il .bin inizia con zeri, non con _start")
rel = subprocess.check_output(["llvm-readelf-18" if subprocess.call(["bash","-lc","command -v llvm-readelf-18"], stdout=subprocess.DEVNULL)==0 else "llvm-readelf", "-r", elf], text=True, stderr=subprocess.STDOUT)
# Un raw copiato a un indirizzo ignoto non applica rilocazioni.
body = [ln for ln in rel.splitlines() if ln.strip() and not ln.startswith("Relocation") and "Offset" not in ln and not ln.startswith("There are no")]
if body:
    raise SystemExit("rilocazioni nel payload:\n" + rel)
print("shellcode raw, %d byte, niente magic ELF, niente rilocazioni" % __import__("os").path.getsize(raw))
print("primi byte: " + b.hex())
PY
ls -lh "$ROOT/dist/psbox-install.elf" "$ROOT/dist/psbox-install.bin"
