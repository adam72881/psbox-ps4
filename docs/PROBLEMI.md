# Problemi aperti (01.38)

Versione di riferimento: **01.38** (`PSBox-PS4-01.38.pkg`). Il diario tecnico lungo sta in [`CRONISTORIA.md`](CRONISTORIA.md).

## Aperti

### RTP che si ferma (~15 s)

Lo stream parte e i pacchetti RTP arrivano, poi dopo una quindicina di secondi il video si interrompe. Dopo un test: `/data/psbox/xcloud.log`, `/data/psbox/last_stage.txt`, `trace_stream.csv` (`pres_us`, `scaled`, `over`).

### Qualità e reconnect

La qualità 1080HQ e il rientro in sessione non sono stabili. Un reconnect può perdere video o pad.

### Chiusura bloccata

Uscire dallo stream o chiudere l’app a volte non torna al menu: il processo resta appeso.

### DTLS se l’SDP è sbagliato

Con ICE già connesso, un’offerta senza `transport-cc` (TWCC, extension id 3, come su PS5) lascia il ClientHello senza risposta: 0 byte DTLS in ricezione e nessuno stream. L’offerta della 01.38 include TWCC. Un SDP diverso riapre il problema.

## Già sistemato

- **TWCC nell’SDP.** `transport-cc` con extension id 3, come PSBox PS5. I feedback TWCC sono limitati a 40 hit.
- **MTU 1200.** Il PeerConnection usa 1200, non il default 1280 di libdatachannel.
- **Abandon.** La sessione si lascia con `abandon()`, senza `pc->close()`.

Diagnostica: `/data/psbox/last_stage.txt`, `trace_stream.csv`, `trace_stream.txt`. La qualità chiesta al server resta 1080HQ.
