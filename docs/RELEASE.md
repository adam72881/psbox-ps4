# Release 01.54 (v1.0.54)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.54.pkg` | Build CI. SHA-256 dall'artifact `PSBox-PS4`. |

## Contenuto 01.54

Allineamento XCloud4 (008f4ab) / Nexo (ce73289) dopo FAIL 01.52/01.53: stallo a ~109 TWCC / ~5,5 s indipendente dalla banda.

1. **TWCC fuori:** niente extmap/rtcp-fb transport-cc né feedback TWCC; solo RR + REMB (+ PLI).
2. **Keepalive HTTP:** subito dopo `/play`, poi ogni 30 s; gestisce 410.
3. **RR audio:** reporter attivo e pompato; traccia audio RecvOnly.
4. **Audio:** niente `addSSRC(1)` / packetizer.
5. **H.264:** solo PT 102 Constrained Baseline (niente 104/106).
6. **/sdp body:** `reliableinput` / `unreliableinput` `{minVersion:9,maxVersion:9}`.
7. **systemUi:** nessun TransactionComplete automatico (tastiera come Nexo).
8. **control:** auth → un `gamepadChanged true` → config → un solo alias risoluzione.
9. **Identità /play:** timezone locale; osName come XCloud4; `b=AS` dal tetto ABR.

UI invariata.
