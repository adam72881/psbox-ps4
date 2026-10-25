# Release 01.52 (v1.0.52)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.52.pkg` | Build CI. SHA-256 dall'artifact `PSBox-PS4`. |

## Contenuto 01.52

Fix radice sul FAIL 01.51 (720HQ → 1920x1080; stima fissa senza REMB; Connessione persa).

- **BUG1:** `dimensionschanged` usa `streamDimsForAlias`: `720`/`720HQ` → 1280×720, `1080`/`1080HQ` → 1920×1080, `1440` → 2560×1440.
- **BUG2:** REMB sempre (anche con TWCC) + ABR su recv/stall (−40%, floor 4/6 Mb/s).
- **P1a:** attesa IDR dopo coda piena, errorFrame, menu PS; scarto P-frame; reset a 5 s.
- **P1b:** limiter keyframe 500 ms / max 10 in 5 s; control + PLI.
- **P1c:** tetto 8 Mb/s @720, 12 @1080 (partenza 10); `b=AS` = tetto; Baseline `42e020`/`42e02a`.
- **P1d:** Videodec2 depth 1 a 720p; FB trattenuti non ri-offerti.
- Chiusura: `sceSystemServiceLoadExec("exit")` + sigwait. UI invariata.
