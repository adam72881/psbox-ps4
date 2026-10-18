#!/usr/bin/env bash
# Costruisce il .pkg GoldHEN (CATEGORY gd) da un eboot.bin già firmato fSELF.
# Uso: make_pkg.sh <eboot.bin> <cartella_output>
set -euo pipefail
EBOOT="$1"
OUTDIR="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"

TITLE="PSBox Cloud Gaming"
VERSION="01.38"
TITLE_ID="XCLD00001"
CONTENT_ID="IV0000-XCLD00001_00-PSBOXCLOUD000001"

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

STAGE="$OUTDIR/pkg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$STAGE/assets/fonts"

python3 - <<'PY' "$STAGE/sce_sys/icon0.png"
import struct, sys, zlib
path = sys.argv[1]
w = h = 512
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
raw = bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w):
        # campo verde scuro, cornice chiara, segno centrale
        edge = x < 18 or y < 18 or x >= w - 18 or y >= h - 18
        cx, cy = x - w // 2, y - h // 2
        mark = abs(cx) < 70 and abs(cy) < 46
        if edge:
            raw += bytes((186, 214, 176))
        elif mark and (abs(cx) > 48 or abs(cy) > 24):
            raw += bytes((120, 190, 96))
        else:
            raw += bytes((12, 22, 16))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
open(path, "wb").write(png)
PY

cp "$EBOOT" "$STAGE/eboot.bin"
cp "$ROOT/assets/cacert.pem" "$STAGE/assets/cacert.pem"
cp "$ROOT/assets/fonts/"*.ttf "$STAGE/assets/fonts/"
cp "$ROOT/assets/fonts/OFL.txt" "$STAGE/assets/fonts/" || true
for f in Inter-Regular.ttf Inter-SemiBold.ttf Inter-Bold.ttf; do
    if [[ ! -s "$STAGE/assets/fonts/$f" ]]; then
        echo "font mancante nello stage: $f" >&2
        exit 1
    fi
done
# Moduli di runtime distribuiti con gli esempi OpenOrbis (non sono codice nostro).
cp "$OO_PS4_TOOLCHAIN/samples/hello_world/sce_module/libc.prx" "$STAGE/sce_module/"
cp "$OO_PS4_TOOLCHAIN/samples/hello_world/sce_module/libSceFios2.prx" "$STAGE/sce_module/"
cp "$OO_PS4_TOOLCHAIN/samples/hello_world/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/"

SFO="$STAGE/sce_sys/param.sfo"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
# ATTRIBUTE 0: come Moonlight-ps4 e i template OpenOrbis. Valori non documentati
# (es. bit «suspend») su homebrew gd hanno causato comportamenti strani in chiusura;
# la CE-34878-0 si evita con _exit sui fault, non cambiando questo flag.
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value "gd"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "$VERSION"

cd "$STAGE"
FILES="eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx"
FILES="$FILES sce_module/libc.prx sce_module/libSceFios2.prx"
FILES="$FILES assets/cacert.pem assets/fonts/Inter-Regular.ttf assets/fonts/Inter-SemiBold.ttf assets/fonts/Inter-Bold.ttf assets/fonts/OFL.txt"
"$TOOLS/create-gp4" -out pkg.gp4 --content-id="$CONTENT_ID" --files "$FILES"
"$TOOLS/PkgTool.Core" pkg_build pkg.gp4 .

PKG_ID="$STAGE/$CONTENT_ID.pkg"
mkdir -p "$OUTDIR"
cp -f "$PKG_ID" "$OUTDIR/PSBox-PS4-${VERSION}.pkg"
ACTUAL=$(stat -c%s "$OUTDIR/PSBox-PS4-${VERSION}.pkg")
# Verifica che i font siano davvero dentro il pkg (non solo nello stage).
VERIFY="$OUTDIR/pkg-verify-$$"
mkdir -p "$VERIFY"
if "$TOOLS/PkgTool.Core" pkg_extract --passcode 00000000000000000000000000000000 \
    "$OUTDIR/PSBox-PS4-${VERSION}.pkg" "$VERIFY" >/dev/null; then
    for f in Inter-Regular.ttf Inter-SemiBold.ttf Inter-Bold.ttf; do
        got=$(stat -c%s "$VERIFY/uroot/assets/fonts/$f" 2>/dev/null || echo 0)
        want=$(stat -c%s "$ROOT/assets/fonts/$f")
        if [[ "$got" != "$want" ]]; then
            echo "font nel pkg diverso/assente: $f (pkg=$got src=$want)" >&2
            rm -rf "$VERIFY"
            exit 1
        fi
    done
    echo "font nel pkg: ok ($(stat -c%s "$ROOT/assets/fonts/Inter-Regular.ttf") byte Inter-Regular)"
fi
rm -rf "$VERIFY"
# Installare sopra un titolo già presente fallisce con CE-35659-8 se la
# dimensione cambia. Se l'installazione risponde CE-35659-8, disinstalla la precedente.
echo "OK: $OUTDIR/PSBox-PS4-${VERSION}.pkg ($ACTUAL byte)"
