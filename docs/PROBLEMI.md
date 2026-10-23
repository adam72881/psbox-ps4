# Problemi, cause e fix

Cronologia operativa del port PS4. Serve a chi testa o continua lo sviluppo: non è marketing.

Upstream di riferimento: [PSBox PS5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) v0.9.8 (`f8b2726`).

## WebRTC / xCloud

### DTLS non parte (0 byte RX)

- **Sintomo:** ICE connected, timeout stream, nessun media.
- **Causa 01.36:** rimozione di `transport-cc` dall’SDP “per allinearci al REMB-only”. Sul PS5 l’offerta ha ancora transport-cc; senza, su Orbis il server non risponde al ClientHello DTLS.
- **Fix 01.37:** ripristino transport-cc + extension id 3 come PS5 / 01.35.

### Stream che muore dopo ~1 minuto

- **Sintomo:** video/RTP ok all’inizio, poi si ferma; log con TWCC enormi.
- **Causa:** feedback TWCC malformato / count altissimi (centinaia di hit), pressione su RTCP/UDP.
- **Fix 01.37:** cap ≤40 hit, solo ext id 3, flush TWCC dal pump (non dal path RTP); REMB periodico; MTU 1200.

### ICE ok, DTLS ancora a 0 dopo 3 s

- **Sintomo:** come sopra ma SDP già corretto.
- **Ispirazione:** AJ-GFN — alcune porte rispondono ICE ma non accettano DTLS media.
- **Fix 01.38:** a 3 s con `xc_dtls_rx_pkts()==0`, re-iniezione candidati remoti + host sulle porte `m=` / Teredo (9002 incluso). Niente riparto della PeerConnection (quello rompeva decoder/pad in 01.27–01.32).

### Deadlock SCTP / input

- **Sintomo:** pad appeso, coda DTLS piena.
- **Fix:** niente `DataChannel::send` dai callback RTP; ack/pad in coda, flush in `pumpFast`.

## Video / present

### Tear o present che “salta”

- **Causa:** 2 soli buffer VideoOut; si riscriveva il buffer ancora in flip.
- **Fix 01.38:** 4 buffer, `pickFree` (non latchare su un indice occupato), `drainFlips` più lungo prima di Unregister/Close.

### Freeze con audio vivo

- **Classe di bug (documentata anche su GFN PS4):** un gate “già presentato” aggiornato anche sul frame fallito → present bloccato per sempre.
- **Nostra disciplina:** se `drawNv12` fallisce non si chiama `present`; il frame successivo riprova. Menu PS mette in pausa solo il present, non ack/RTCP.

### Qualità 1080 vs present 1:1

- Altri port (GFN) fissano framebuffer = res server (anche 540/720) per stare nel budget 16.6 ms.
- **Noi vogliamo 1080HQ** dal server xCloud: teniamo decode 1080 e blit SSE 3:2 → BGRA720. Il CSV `pres_us` dice se il present è fuori budget.

## Chiusura / stabilità processo

### CE-34878-0 chiudendo l’app

- **Causa:** Close/join/resetCallbacks mentre il sistema revoca Videodec2/Audio/VideoOut.
- **Fix:** teardown leggero → stop stream → `_exit(0)`; SIGSEGV/BUS/ILL/ABRT/TERM → `_exit(0)`; log con fsync a ogni passo.

### “Lost connectivity” / reconnect aggressivo

- Riparto ICE/DTLS a 3 s (01.27–01.32) chiudeva la PC mentre il server era già ok → NEW_SEQUENCE e pad scollegato.
- **Ora:** un solo negotiate; warning + reinject; abandon su fail senza `pc->close` dove possibile.

## Fix 01.39

- **TWCC freeze ~6–8 s:** i feedback scartavano i hit più vecchi (kMaxHits + clear-all) → GCC spegneva l’RTP a ~57 feedback. Ora oldest-first, backlog in coda, log `[TWCC]`.
- **Update locale:** `APP_VER` da param.sfo (fmt 0x0204), size stabile FTP, log `[UPDATE]`, AppInst→BGFT FORCE_UPDATE, delete pkg + relaunch/notify.
- **Chiusura menu PS:** AppMessaging da SystemService, IsAppSuspended(void), decode in pausa in background, `[EXIT] chiusura richiesta dal sistema`.

## Diagnostica (01.38+)

| File | Uso |
| --- | --- |
| `last_stage.txt` | Ultimo stadio fsyncato (es. `DTLS_REINJECT`, `PRESENT_FIRST`) |
| `trace_stream.csv` | `t_ms,frame,src_w/h,dst_w/h,scaled,pres_us,budget_us,over,drawn` |
| `trace_stream.txt` | Conteggi al secondo |

Se qualcosa muore: manda `last_stage.txt` + coda del CSV + coda di `xcloud.log`.

## Cosa non è (ancora) risolto

- Chiusure sporadiche mid-stream senza pattern fisso (stessa classe “aperta” anche su altri client PS4).
- Present a 1080 framebuffer pieno: su Orbis è rischioso (altri l’hanno escluso); noi scanout a 720.
- Videodec2 su firmware ≠ 11.52: su 9.00 altri reportano `0x805A1000`; qui su 11.52 carica.

## Riferimenti esterni utili

- [PSBox PS5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) — protocollo e UI
- [AJ-GeforceNow-PS4](https://github.com/AJfiles/AJ-GeforceNow-PS4) — diagnostica present, buffer VideoOut, ICE/DTLS (stack libpeer / GFN, non copiare SDP NVIDIA)


## Audit codice main 01.49 — 2026-10-23

Questa sezione descrive lo stato verificato del codice, non certifica che i problemi siano risolti su ogni PS4.

### Stato attuale verificato nel sorgente

- `src/app/stream_player.cpp`: il thread di rete copia le access unit H.264 in una coda protetta da mutex; la coda video ha limite 30. Quando è piena elimina i frame più vecchi fino a lasciarne 4 e marca la necessità di chiedere un keyframe. La decodifica PS4 passa da Videodec2; il present resta su BGRA 1280×720.
- La stessa classe misura tempi di decode e present, frame scartati, fallimenti di decode, reset della coda e latenza arrivo-display. Questi valori vanno letti insieme a `trace_stream.csv`, `trace_stream.txt`, `last_stage.txt` e `xcloud.log`: il solo FPS non distingue rete, decoder e present.
- `src/display/display_ps4.cpp`: il percorso attuale usa quattro buffer VideoOut e sceglie un buffer libero; prima di chiudere/riaprire la porta prova a drenare i flip pendenti.
- `src/app/lifecycle.cpp` e `src/app/crash_guard.cpp`: la chiusura di sistema evita intenzionalmente il teardown pesante e registra l'ultimo stadio prima di uscire. Non rimuovere questa disciplina senza una prova specifica su PS4.
- `src/app/updater.cpp`: l'aggiornamento remoto verifica la firma del pacchetto, controlla la versione contenuta e usa journal/rollback per il cambio dei file. `src/app/local_update.cpp` gestisce invece il percorso d'installazione locale PKG; sono due percorsi diversi e vanno testati separatamente.

### Confronto con XCloud4 / Nexo X4: cosa non copiare alla cieca

Le ottimizzazioni sperimentali di Nexo X4 documentano un miglioramento misurato del solo stadio di copia immagine; non dimostrano automaticamente un aumento equivalente degli FPS su PSBox. Il percorso video e le API native devono essere confrontati con la nostra implementazione prima di trasferire codice. La coda RTP, il riordino, il decoder e il present sono componenti collegati: cambiare solo la dimensione della coda può ridurre o aumentare la latenza a seconda del carico.

### Protocollo minimo prima di dichiarare un fix

1. Salvare una baseline sulla stessa console, rete e scena: `xcloud.log`, `last_stage.txt`, coda di `trace_stream.csv` e `trace_stream.txt`.
2. Annotare firmware PS4, build PSBox, risoluzione/qualità scelta, tipo di collegamento (Ethernet o Wi‑Fi) e sintomo osservato.
3. Separare almeno tre casi: scatti durante gioco, perdita/freeze della sessione, chiusura dal menu PS4.
4. Per ogni modifica: compilazione OpenOrbis, controlli statici/test disponibili, PKG distinto e prova sulla console. Se non è stata eseguita la prova hardware, indicare il fix come non verificato.
5. Non cambiare UI, Title ID o percorso d'installazione come parte di un intervento sullo streaming.

### Priorità d'indagine

- **Streaming:** correlare età della coda e tempo di decode/present prima di cambiare la politica di drop; verificare se i frame scartati sono seguiti da un keyframe valido.
- **Chiusura:** usare l'ultimo stadio registrato per distinguere il percorso di uscita richiesto dal sistema da un crash durante la sessione.
- **Update:** provare in modo indipendente aggiornamento remoto firmato, rollback dopo interruzione e installazione PKG locale; non eliminare il journal se il ripristino non è completo.

Questa audit non modifica il comportamento dell'applicazione: serve a fissare una baseline tecnica e a evitare di confondere misure dichiarate da altri progetti con risultati già dimostrati su PSBox.
