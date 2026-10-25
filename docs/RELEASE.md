# Release 01.53 (v1.0.53)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.53.pkg` | Build CI. SHA-256 dall'artifact `PSBox-PS4`. |

## Contenuto 01.53

Fix radice sul FAIL console 01.52 (server in pausa con 0 loss: HOL sul canale input + frame-ack Metadata + ABR su recv statico).

- **Input SCTP:** canale `input` unordered + `maxRetransmits=0` (XCloud4 `rtc_transport.c`).
- **Input reports:** solo `ClientMetadata` (type 8) all'apertura; poi gamepad type 2 a 60 Hz; skip se `bufferedAmount>0`; niente Metadata type1 di default (`sendFrameAcks=false`).
- **ABR:** taglio su loss / local_dropped / stale_ingress; niente taglio sul solo receive rate; warmup 5 s senza tagli.
- Resta dalla 01.52: dims 720HQ, REMB+TWCC, waitIdr, limiter KF, Baseline, depth 1 @720, LoadExec exit. UI invariata.
