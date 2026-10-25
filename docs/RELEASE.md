# Release 01.51 (v1.0.51)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.51.pkg` | Build CI. SHA-256 `f920d717b3589dcabb840a431bd45b5a620aaa1cfc9b9bc42205cbbad5133d19`, 9961472 byte, magic `7f434e54`. |

## Contenuto 01.51

- **Patch libdatachannel applicabili** e pool di 4 thread (niente `hardware_concurrency` su Orbis).
- **Riconnessione:** budget DTLS 20 s, seconda re-iniezione candidati a 8 s se ancora 0 byte RX, keepalive GSSV durante la negoziazione. Uno stop di chiusura non viene cancellato da una sessione nuova. I sleep della negoziazione sono a fette da 200 ms.
- **Chiusura dal menu PS:** SIGTERM/INT/QUIT/HUP bloccati prima di `platform::init`, attesi da `sigwait`, poi `_exit`. Il monitor non li sblocca più.
- **Stream:** dopo coda piena, menu PS o errore di decode si scarta fino a un IDR. Keyframe al massimo uno ogni 500 ms e 10 in 5 s, con PLI. Bitrate di partenza 8 Mb/s a 720p e 12 Mb/s a 1080p; il tetto utente, se più basso, vince. REMB solo sotto il tetto. Framebuffer Videodec2 non si rioffre se il decoder lo tiene. Un solo flip in volo; il blit non aspetta il flip. Audio con prefill 40 ms e trim a fade. Scaler 3:2 con due tap di luma sui pixel dispari.
- **Diagnostica:** `/data/psbox/stats/stream-AAAAMMGG-HHMMSS.json`, ultimi 10, aggiornato ogni 5 s da un thread a bassa priorità. Nessun token.
- La qualità chiesta resta **1080HQ**. Niente profilo Constrained Baseline e niente copia frame via GPU.

Non è un PASS sulla console: la checklist di prova è nel messaggio della modifica.
