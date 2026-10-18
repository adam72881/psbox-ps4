# PSBox Cloud Gaming per PS4

Port di [PSBox Cloud Gaming per PS5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) di **RafaelNGP** (v0.9.8, commit [`f8b2726`](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/commit/f8b272621e1df0ce4a293acb8162299328724dc7)) su PlayStation 4 con GoldHEN. Attribuzione e licenza dell’upstream: [`NOTICE.md`](NOTICE.md).

Client homebrew di Xbox Cloud Gaming (xCloud). Login Microsoft, catalogo Game Pass, sessione e WebRTC sono il codice di PSBox PS5. Qui c’è il lavoro per la PS4: OpenOrbis, Videodec2, VideoOut, DualShock 4 e il pacchetto GoldHEN.

Progetto non ufficiale, senza affiliazione con Microsoft o Sony.

## Requisiti

- PlayStation 4 con **GoldHEN** (riferimento firmware 11.52)
- Account Microsoft con **Game Pass Ultimate** (serve xCloud)
- Connessione a internet

## Installazione

Unica versione: **01.38**. Il file è `PSBox-PS4-01.38.pkg` (versione pacchetto `1.0.38-ps4`, Title ID `XCLD00001`). I dati restano in `/data/psbox`.

**Release in arrivo / build locale.** Il pkg 01.38 non è ancora allegato a una GitHub Release. Quando lo sarà, si scarica da [Releases](https://github.com/adam72881/psbox-ps4/releases). Fino ad allora si compila in locale, oppure si copia un `PSBox-PS4-01.38.pkg` già costruito.

1. Metti `PSBox-PS4-01.38.pkg` in `/data/pkg` (FTP GoldHEN, porta 2121, login anonimo) oppure su una chiavetta che GoldHEN vede.
2. Con GoldHEN avviato, se PSBox è già installato, disinstallalo prima. I file in `/data/psbox` restano. Installare sopra un pacchetto di dimensione diversa dà CE-35659-8.
3. Apri Package Installer (Debug Settings → Game → Package Installer, oppure l’installer di GoldHEN) e installa il pkg. L’icona è **PSBox Cloud Gaming**.

Da Windows, dalla root del repo:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1 -Pkg .\dist\PSBox-PS4-01.38.pkg
```

Lo script sostituisce solo i `PSBox-PS4-*.pkg` in `/data/pkg`.

## Build

Servono clang 18, lld, CMake, Ninja e Python 3. Dalla root del repo:

```bash
./scripts/build_pkg.sh
```

Il pacchetto è `dist/PSBox-PS4-01.38.pkg`. Dipendenze, payload e diario tecnico: [`docs/CRONISTORIA.md`](docs/CRONISTORIA.md).

## Stato

Login, lista Game Pass e avvio sessione funzionano su GoldHEN 11.52: decode Videodec2, qualità server 1080HQ, present BGRA720.
Lo stream resta fragile: l’RTP può fermarsi dopo circa 15 secondi.
Qualità e riconnessione non tengono la sessione in modo affidabile.
La chiusura dell’app a volte si blocca.
Se l’offerta SDP non include TWCC, il DTLS non parte.
In 01.38 sono già a posto l’SDP con TWCC, l’MTU 1200 e l’abandon della sessione.
Dettaglio: [`docs/PROBLEMI.md`](docs/PROBLEMI.md).

## Licenza

**GPL-3.0-or-later**, come l’upstream. Testo in [`LICENSE`](LICENSE). Componenti terzi: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
