# PSBox (base XCloud4)

Client homebrew **Xbox Cloud Gaming** per PS4 (GoldHEN). Dalla **01.57** il trasporto streaming è il sorgente **XCloud4** [v1.1.0-beta](https://github.com/asp11edgr/XCloud4) (invariato dove c’è conflitto sul path RTC); sopra restano branding, exit handler e packaging PSBox.

> Upstream streaming: [asp11edgr/XCloud4](https://github.com/asp11edgr/XCloud4) — GPL-3.0-only. Vedi [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) e i crediti sotto.

| | |
|---|---|
| **Build** | `01.57` → `dist/PSBox-PS4-01.57.pkg` |
| **Title ID** | `XCLD00001` |
| **Base stream** | XCloud4 public profile (`X4_PUBLIC=1`, `X4_STREAM_PROFILE=1`, NACK+grace45) |

## Installazione

1. Installa `PSBox-PS4-01.57.pkg` (Package Installer / GoldHEN).
2. Avvia **PSBox Cloud Gaming**, accedi con il codice Microsoft mostrato a schermo.

Deploy LAN (worker `adam-windows-ps4`):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1 -Pkg .\dist\PSBox-PS4-01.57.pkg
```

## Build

Vedi [BUILDING.md](BUILDING.md) (toolchain OpenOrbis + `scripts/webrtc/build_native.sh`). Per il pkg PSBox:

```bash
export OO_PS4_TOOLCHAIN=…
export X4_RUNTIME_MODULES="$OO_PS4_TOOLCHAIN/samples/hello_world/sce_module"
./scripts/build_pkg.sh
```

## Cosa è XCloud4 / cosa è PSBox

| Area | Provenienza |
|---|---|
| WebRTC / ICE / DTLS / SCTP, data channel, `/play`, keepalive, video/audio decode, catalogo UI nativa | **XCloud4** (`src/streaming`, `src/auth`, `src/media`, `src/video`, `src/audio`, `src/ui`, `scripts/webrtc/*`) |
| Title / CONTENT_ID / APP_VER `01.57` / nome pkg / icon0 | **PSBox** (`scripts/package_identity.py` mode `psbox`, `assets/icon0.png`) |
| SIGTERM → `_exit(0)` (exit post-menu) | **PSBox** (`src/core/psbox_exit.c`) |
| Contatori recv/stream / NACK / grace | **XCloud4** (invariati; non i patch CMake PSBox 01.56) |
| UI C++ PSCloud (libreria/grafica precedente) | In `legacy/psbox/` — **non** ancora innestata su questa base |
| Update GoldHEN Remote Package Install | In `legacy/psbox/src/app/local_update.*` — **non** ancora portato |

Provenienza import: [UPSTREAM_XCLOUD4.txt](UPSTREAM_XCLOUD4.txt).

## Crediti

- **XCloud4** by [asp11edgr](https://github.com/asp11edgr/XCloud4) — streaming stack and native PS4 client (GPL-3.0-only).
- Third-party pins: libdatachannel 0.24.6, libjuice `b89c792`, Mbed TLS 3.6.7, Opus 1.5.2 — see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- PSBox packaging / exit overlay — this repository.

## Note on testing

Non dichiarare lo stream “funzionante” finché non è verificato su console (worker `adam-windows-ps4`).
