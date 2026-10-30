# SSPI source — Sifu (CUSA25761)

This directory contains the text payload for an `embedded-catalog-v1` SSPI source.

- Title ID: `CUSA25761`
- Region: `EUR`
- Release: base game v1.29
- Listing page: https://dlpsgame.com/sifu-ps4-pkg/
- Hoster landing page: https://vik1ngfile.site/f/zgkRF3KmXV

## Package as `.gssource`

Create a ZIP archive whose root contains `source.json`, `catalog.json`, and `packages/sifu.json`, then use the `.gssource` extension. Preserve the relative paths exactly. The hashes and byte sizes in `source.json` are for the UTF-8 files with LF line endings.

**Important:** `HosterLanding` records the VikingFile landing URL; it does not guarantee that a particular SSPI build can resolve its JavaScript/Cloudflare-protected download page. The actual download resolution must be tested in SSPI.
