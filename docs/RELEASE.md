# Release 01.41 (v1.0.41)

| File | Note |
| --- | --- |
| `PSBox-PS4-01.41.pkg` | Build corrente |
| `PSBox-PS4-01.42.pkg` | Solo test update 01.41 → 01.42 da `/data/psbox/update/` |

## Contenuto 01.41

- **Chiusura falsa al boot:** `IsAppSuspended` con errore SCE (es. `0x80A10009`) non chiude più; solo `rc > 0` conta come sospeso. Niente Close da AppMessaging / eventi inventati / GetStatus ko.
- **Update con dialogo:** all’avvio e da «Controlla aggiornamenti» se c’è un pkg più nuovo → «È disponibile l’aggiornamento X. Vuoi installarlo?» **Sì / No** (✗ conferma, ○ indietro). No lascia il pkg e riproporrà al prossimo avvio. Log `[UPDATE] scelta Sì/No`.
- **Install sicuro (solo dopo Sì):** path BGFT `/user/data/...`, copia senza rename/unlink a metà, attesa `GetProgress` fino a fine/errore, poi `LoadExec("exit")`. Mai UnInstall di `XCLD00001`.
- Mantiene i fix 01.39: TWCC, pausa decode in menu PS, screenshot (incluso dialog update).
