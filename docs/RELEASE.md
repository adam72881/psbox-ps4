# Release 01.56 (v1.0.56)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.56.pkg` | Build CI. SHA-256 dall'artifact `PSBox-PS4`. |

## Contenuto 01.56

Correzioni sul fail 01.55 (trace ~50 fps per ~5 s poi stop inbound; Consent expired; niente `stream:`/`recv:`; EXIT ERROR da menu PS):

1. **Deadlock Handshake:** `queueText` da `message->onOpen` / `onHandshakeAck`, ma `flushOutbound` solo in `pumpFast` dopo `start()`. Durante `waitFor(handshakeDone)` ora si fa flush (+ RR). Senza HandshakeAck non partono auth/config/alias → il server taglia l’RTP ~5 s.
2. **Capabilities XCloud4:** `clientdevicecapabilities` piene (width/height/bitrate/fps), non `{}`; log «RTC canales Xbox preparados».
3. **recv: 1 Hz** su thread dedicato (`psbox_rd_start_logger`), indipendente dal worker bloccato in `start()`.
4. **EXIT menu PS post-stream:** se lo stream è già morto, niente `sceVideoOutClose` (solo `releaseForExit`); dopo `homeSuspend` niente ShellUiFg/PSButton; `sigwait` fa solo write+unlink marker+`_exit(0)` (niente stats).
