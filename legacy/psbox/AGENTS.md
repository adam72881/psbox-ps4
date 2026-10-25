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

## Diagnostica stream (01.38+)

Dopo un test: `/data/psbox/last_stage.txt`, `trace_stream.csv` (`pres_us`, `scaled`, `over`), `trace_stream.txt`. Qualita' resta 1080HQ; non abbassare a 720 present 1:1.
