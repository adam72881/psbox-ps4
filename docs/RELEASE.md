# Release 01.38 (v1.0.38)

## Artifact

| File | Size | SHA-256 |
| --- | --- | --- |
| `PSBox-PS4-01.38.pkg` | 9 830 400 byte | `f795986f7e39ba0e9b25aebbd8cb2a7fc4028f6edbe1c84ff2e7b86cbae02e65` |

## Pubblicare su GitHub Releases

Il binario non entra nei commit testuali. Da PC (con `gh` autenticato o dalla UI):

```bash
gh release create v1.0.38 \
  ./dist/PSBox-PS4-01.38.pkg \
  --repo adam72881/psbox-ps4 \
  --title "PSBox PS4 01.38" \
  --notes-file docs/RELEASE.md
```

Oppure: GitHub → **Releases** → **Draft a new release** → tag `v1.0.38` → allega il pkg → Publish.

Da Windows (dopo `gh auth login`):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish-release.ps1
```

## Contenuto 01.38

- Diagnostica: `/data/psbox/last_stage.txt`, `trace_stream.csv` (`pres_us`)
- VideoOut: 4 buffer, `pickFree`, drain prima di Close
- DTLS: re-iniezione candidati a 3 s se RX = 0
- Qualità server **1080HQ**, present BGRA 1280×720

Vedi anche [PROBLEMI.md](PROBLEMI.md).
