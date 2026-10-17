# PSBox Cloud Gaming per PS4

<p align="center">
  <strong>Xbox Cloud Gaming (xCloud) nativo su PlayStation 4</strong><br/>
  Port di <a href="https://github.com/RafaelNGP/xbox-cloud-gaming-ps5">PSBox per PS5</a> (RafaelNGP) su GoldHEN
</p>

<p align="center">
  <a href="https://github.com/RafaelNGP/xbox-cloud-gaming-ps5">Upstream PS5</a> ·
  <a href="NOTICE.md">Attribuzione</a> ·
  <a href="docs/PROBLEMI.md">Problemi &amp; fix</a> ·
  <a href="LICENSE">GPL-3.0-or-later</a>
</p>

---

Client homebrew di **Xbox Cloud Gaming**: login Microsoft, catalogo Game Pass, sessione xCloud e WebRTC sono il codice di [PSBox PS5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) (v0.9.8 / [`f8b2726`](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/commit/f8b272621e1df0ce4a293acb8162299328724dc7)), non una riscrittura. Su PS4 abbiamo portato rete Orbis, Videodec2, VideoOut, DualShock 4 e il pacchetto GoldHEN.

| | |
| --- | --- |
| **Upstream** | [RafaelNGP/xbox-cloud-gaming-ps5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) |
| **Console** | PS4 + GoldHEN (riferimento **11.52**) |
| **Title ID** | `XCLD00001` |
| **Build** | `1.0.38-ps4` → `dist/PSBox-PS4-01.38.pkg` |
| **Dati** | `/data/psbox` (account, settings, log) |
| **Licenza** | GPL-3.0-or-later (come l’upstream) |

> Progetto non ufficiale. Nessuna affiliazione con Microsoft o Sony.

## Cosa fa

- Login con codice dispositivo / QR (niente password sulla console)
- Libreria Game Pass, ricerca, preferiti
- Stream xCloud a **1080HQ** (qualità server), present su framebuffer BGRA 1280×720
- Decode hardware **Videodec2**, audio Opus via audio_orbis (Moonlight-ps4)
- Pad DualShock 4 mappato come Xbox
- Menu in-stream (touchpad), invio log, auto-update da `/data/psbox/update`

## Installazione

1. Con GoldHEN attivo, installa `PSBox-PS4-01.38.pkg` (Package Installer / GoldHEN).
2. Se hai già una build ≥ 01.22: copia il pkg in `/data/psbox/update` e avvia l’app (BGFT).
3. Servono internet e un account Microsoft con Game Pass che includa xCloud.

**Da PC (FTP GoldHEN `:2121`):**

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1 -Pkg .\dist\PSBox-PS4-01.38.pkg
```

I dati restano in `/data/psbox`. Non condividere `account.json`.

## Compilare

Serve clang 18, lld, CMake, Ninja, Python 3, OpenSSL 1.1 per PkgTool.

```bash
./scripts/build_pkg.sh
```

Output: `dist/PSBox-PS4-01.38.pkg` e `dist/psbox-install.bin`.  
Dipendenze: OpenOrbis 0.5.4, mbedTLS 3.6.2, libdatachannel 0.23.1, opus 1.5.2.

## Diagnostica stream

Dopo una sessione, su console:

| File | Contenuto |
| --- | --- |
| `/data/psbox/xcloud.log` | Log completo (fsync) |
| `/data/psbox/last_stage.txt` | Ultimo stadio (ICE / DTLS / PRESENT…) |
| `/data/psbox/trace_stream.csv` | Per frame: `pres_us`, `scaled`, `over` |
| `/data/psbox/trace_stream.txt` | Riepilogo al secondo |

## Problemi affrontati (sintesi)

Dettaglio e lezioni da altri port PS4 → [`docs/PROBLEMI.md`](docs/PROBLEMI.md).

| Sintomo | Causa | Fix (build) |
| --- | --- |
| DTLS 0 byte RX, niente video | SDP senza `transport-cc` (01.36) | Ripristinato transport-cc / ext id 3 (**01.37**) |
| Stream muore dopo ~1 min | TWCC malformato / troppo grande | TWCC cap ≤40, solo ext id 3, flush dal pump (**01.37**) |
| ICE ok ma DTLS non parte | Candidati/porte incompleti | Re-iniezione candidati a 3 s se 0 RX (**01.38**) |
| Tear / present fallito | Solo 2 buffer VideoOut | **4 buffer** + `pickFree` + drain prima di Close (**01.38**) |
| Chiusure / freeze opachi | Poca traccia su disco | `last_stage` + CSV `pres_us` (**01.38**) |
| Crash chiusura sistema | `pc->close` / join sotto present | Abandon + `_exit(0)`, niente close pesante |
| CE-34878-0 su teardown | Videodec2/Audio revocati a metà | Stop stream senza Close/join; handler segnale → `_exit` |

**Scelta di qualità:** restiamo su **1080HQ** dal server. Non adottiamo il present 1:1 a 720/540 di altri client (es. GeForce NOW PS4) che scambiano nitidezza per fps.

## Struttura

```
src/
  app/          UI loop, stream player, update, crash log
  stream/       WebRTC / GSSV session, RTCP, H.264 receiver
  display/      VideoOut PS4 (BGRA720, blit SSE)
  media/        Videodec2, audio
  platform/ps4/ Orbis shims, entropia, rete
  xcloud/       Catalogo, auth helpers
tools/          ps4_deploy.ps1, register-ps4-worker, …
scripts/        build_pkg.sh, …
docs/           PROBLEMI.md e note
```

## Crediti

- **[PSBox PS5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5)** — RafaelNGP — base del client (GPL-3.0-or-later)
- [Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4) — audio_orbis / riferimenti VideoOut
- OpenOrbis, libdatachannel, mbedTLS, Opus — vedi [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
- Idee di diagnostica / disciplina VideoOut anche da [AJ-GeforceNow-PS4](https://github.com/AJfiles/AJ-GeforceNow-PS4) (MIT; stack e protocollo diversi)

## Licenza

GPL-3.0-or-later — [`LICENSE`](LICENSE).  
Attribuzione upstream obbligatoria — [`NOTICE.md`](NOTICE.md).
