# Agent notes — PSBox PS4 deploy

## Deploy sulla PS4 (LAN)

- PS4 GoldHEN FTP: `192.168.1.19:2121` (anonimo), destinazione `/data/pkg`.
- Script: `tools/ps4_deploy.ps1` (solo `PSBox-PS4-*.pkg`; non toccare `DiPlay_*`).
- Log: `tools/ps4_log.ps1`.
- Un agente **Cloud** non raggiunge la LAN. Serve il worker locale **adam-windows-ps4**.

## Worker My Machines

- Nome: `adam-windows-ps4`
- Auto-start Windows: `tools/register-ps4-worker.cmd` (una volta; Task Scheduler AtLogOn).
- Il worker compare in `list-self-hosted-workers` solo mentre il processo `agent worker start` e' vivo.
- Questa chat deve girare su **My Machines → adam-windows-ps4** (non Cloud VM). Da una sessione Cloud non c'e' placement/`CreateAgent` verso il worker: non tentare workaround PowerShell lato utente.

## Quando Adam chiede "mandalo sulla PS4" / "solo psbox"

1. Se sei gia' sul worker Windows: `powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1` (pkg piu' recente in `dist\` se non specificato).
2. Se sei su Cloud: di' di aprire/continuare l'agente su My Machines `adam-windows-ps4`; non fargli aprire PowerShell a mano per il deploy.
3. Non installare DiPlay ne' altri pkg non richiesti.

## Compilazione nel Cloud Agent

- Pacchetto: `./scripts/build_pkg.sh` → `dist/PSBox-PS4-*.pkg`. L'ambiente fornisce clang 18, `ld.lld-18`, Ninja, CMake, `libssl.so.1.1` (PkgTool) e OpenOrbis in `~/ps4dev/OpenOrbis`.
- Test offline, senza console e senza account: i comandi `g++` in testa ai file in `tests/`.
- Harness Linux: `cmake -S host -B build-host -G Ninja`. Il clone pulito di libdatachannel v0.23.1 sta in `~/ps4dev/libdatachannel-host`; lo `start` dell'ambiente lo collega a `/tmp/libdatachannel-host`. `stream_session.cpp` chiama `silenceIce()`, presente solo nella patch PS4, quindi oggi quel target non compila contro il clone pulito.

## Diagnostica stream (01.38+)

Dopo un test: `/data/psbox/last_stage.txt`, `trace_stream.csv` (`pres_us`, `scaled`, `over`), `trace_stream.txt`. Qualita' resta 1080HQ; non abbassare a 720 present 1:1.
