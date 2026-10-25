# Release 01.55 (v1.0.55)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.55.pkg` | Build CI. SHA-256 dall'artifact `PSBox-PS4`. |

## Contenuto 01.55

Root-cause vs stallo ~5–6 s (STALLO_5S_ANALISI) allineando XCloud4 v1.1.0-beta:

1. **libdatachannel 0.24.6** + **libjuice b89c792** (pin XCloud4); patch PS4 ribasate. Contiene tryPush, MbedTLS timeout, fairness juice.
2. **Niente lock/send** nei callback ldc: coda video lock-free (drop se piena); `queueText`/`flushOutbound` per i JSON.
3. **Identità XCloud4:** `/play` web/windows + displayInfo; altri endpoint native console; `osName` windows; `clientSessionId` vuoto; touch 0; video 1920×1080 / 60 / 12000 kbps / alias `1080`.
4. **RTCP:** RR solo su SR (SSRC media); niente REMB periodico; niente `goog-remb` duplicato né `a=ssrc` su recvonly.
5. **Data channel:** JSON come testo; `clientAppInstallId` XCloud4; niente TransactionComplete automatico.
6. **Keepalive:** primo dopo Provisioned, poi ogni 30 s (non subito dopo `/play`).
7. **PC:** mtu 1280, STUN Google, maxMessageSize 65536, certificato ECDSA.
8. **Diagnostica:** contatori recv permanenti 1 Hz in `xcloud.log`.
