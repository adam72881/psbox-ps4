# Prova catalogo SSPI — Sifu

> Stato: bozza di catalogo/metadati, **non è ancora un file `.gssource` importabile**.
> Non contiene PKG, mirror o link di download.

## Scopo

Verificare come rappresentare Sifu in SSPI usando il motore statico documentato
`embedded-catalog-v1`. La prova resta isolata da PSBox: non modifica codice,
build, interfaccia o dipendenze dell'app.

## Dati rilevati dalla pagina DLPSGame fornita

| Edizione | Title ID | Regione | Versione indicata |
|---|---|---|---|
| Sifu | CUSA25761 | EUR | 1.29 |
| Sifu | CUSA25761 | EUR | 1.26 |
| Sifu | CUSA30202 | USA | 1.27 |
| Sifu OST | CUSA32161 | da verificare | — |
| Sifu Art Book | CUSA32209 | da verificare | — |

La pagina mostra per CUSA25761 EUR il gioco base e riferimenti ad aggiornamenti.
I contenuti OST e Art Book hanno Title ID distinti: non vengono classificati come
DLC di Sifu. L'elenco preciso di update, DLC e requisiti firmware va verificato
prima di pubblicarlo nel catalogo.

## Struttura SSPI prevista

Il formato statico letto nel sorgente SSPI richiede un archivio ZIP `.gssource`
con almeno:

- `source.json`: identità della source, versione, motore e dichiarazioni/hash dei file;
- `catalog.json`: indice del catalogo;
- file indice sotto `index/`;
- shard dei pacchetti sotto `packages/`.

Il motore riconosciuto dal sorgente consultato è `embedded-catalog-v1`;
il file d'ingresso è `catalog.json`. Il catalogo usa `formatVersion: 1` e
la normalizzazione `nfkd-lower-alnum-spaces-v1`.

## Perché non è ancora importabile

SSPI non può risolvere un titolo con soli nome e Title ID: lo shard dei pacchetti
deve contenere almeno una scelta di pacchetto valida per il titolo. Non inserisco
i mirror di DLPSGame né URL di PKG non autorizzati. Per completare una source
importabile serve un URL diretto a un PKG che l'utente abbia il diritto di
distribuire/ospitare, oppure un archivio di PKG propri servito da un proprio server.

## Prossimo test

1. Verificare la versione/build SSPI installata sul PS4.
2. Confermare i metadati di Sifu e le regioni.
3. Aggiungere solo URL diretti a pacchetti legittimamente ospitati.
4. Generare lo ZIP `.gssource`, calcolare SHA-256 di ogni file dichiarato e
   validare l'archivio con il validatore del formato prima dell'importazione.

## Riferimenti tecnici

- SSPI: https://github.com/Xyhlo/SSPI
- Guida: https://xyhlo.github.io/SSPI/
- Engine statico: https://github.com/Xyhlo/SSPI/blob/main/app/PackageSourceEngineStatic.cs
- Discussione sul formato statico: https://github.com/Xyhlo/SSPI/discussions/3
- Pagina Sifu usata per i metadati: https://dlpsgame.com/sifu-ps4-pkg/
