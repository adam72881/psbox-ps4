# PSBox 01.57 feature matrix (base XCloud4)

Transport/stream conflicts → **XCloud4 wins** (no PSBox `--wrap=setsockopt`, no juice IPV4_ONLY/RCVBUF/POLL_CAP/CLOCK4/DTLS fanout, no `stream_session` / `gssv` connect path / `rtcp_reporter`).

| Feature (01.56 user-facing) | 01.57 status | Notes |
|---|---|---|
| Branding TITLE / CONTENT_ID / APP_VER / icon0 / pkg name | **Ported** | `package_identity.py` mode `psbox`, `assets/icon0.png`, APP_VER `01.57` |
| Video profile 1080p-G8 / 42e02a / b=AS:8000 | **XCloud4** | `X4_PUBLIC=1` → `X4_REQUEST_1080P_G8` (`build_pkg.sh`) |
| WebRTC / ICE / DTLS / SCTP / keepalive / NACK / grace45 | **XCloud4** | Unchanged pins + `prepare_port` |
| `--wrap=setsockopt` / socket-clock wrappers | **Excluded** | Absent from Makefile `LDFLAGS` and CI `ld.lld` argv (REA: root cause of 01.56 ~6s stop) |
| PSBox streaming stack (`stream_session`, juice patches, RTCP reporter) | **Excluded** | XCloud4 `live_media` owns transport |
| PSCloud AppUi (home, sign-in, details, settings, search, filters, splash, error, toast) | **Ported** | `src/psbox/ui/app_ui.*` + `psbox_ui.cpp` facade |
| Library rows / owned / search / favorites / hidden / sort | **Ported (data from X4 catalog)** | Rows + Your games + Recent from `X4CatalogSnapshot` / `X4RecentSnapshot`; no dual PSBox login |
| Box art / hero / publisher / description | **Ported (via X4 public metadata)** | `psbox_ui_ingest_metadata` ← `x4_catalog_assets_copy_metadata` → ImageCache HTTPS |
| Friends-playing row | **UI present; data not filled** | Needs Xbox Live presence tokens; X4 keeps tokens private — reintroducing `auth_manager`/`social` dual login was excluded to avoid a second device-code and token surface |
| Store prices / sale badges | **UI present; data not filled** | Same token constraint (`prices.cpp` kept in tree, not linked) |
| Titlehub console badges (360/One/XS) | **UI present; data not filled** | Same token constraint (`titlehub.cpp` kept in tree, not linked) |
| Remote Play / “My consoles” list | **UI present; empty without GSSV** | Console list needs GSSV login token; not reintroduced (streaming connect remains X4) |
| In-stream StreamMenu (OPTIONS+touchpad) | **Ported** | Overlay on X4 live frames; Leave → `x4_auth_cancel` |
| Stream stats line / sharpness / deband / upscaler / triggers / deadzone / confirm | **Ported (settings persist)** | Applied to PSBox settings.json; GPU path still X4 (no PSBox FSR stack) |
| Settings persistence (`/data/psbox/settings.json`) | **Ported** | `app/settings.*` + per-game profiles |
| Screenshot PNG (L1+R1+□ / first stream frame) | **Ported** | `app/screenshot.*` + display shim `readBackRgb` |
| Crash guard / marker / watchdog / previous-session toast | **Ported** | `app/crash_guard.*` + existing `psbox_exit` SIGTERM→`_exit` |
| Report a problem / Send log (QR GitHub form) | **Ported** | `app/report.*` + qrcodegen; sanitized excerpt |
| GoldHEN local update (scan + RPI/BinLoader install) | **Ported (code)** | `local_update.*` + GoldHEN sdk; **helper payload stubbed** (`psbox_install_bin` empty until rebuilt with legacy embed scripts) |
| GitHub signed zip updater (`updater.cpp`, ProsperoStore paths) | **Not linked** | PS5-oriented zip/sig path; PS4 uses GoldHEN RPI. `update_check` still linked for “newer tag” toast pointing at `/data/psbox/update/` |
| Contatori recv/stream / NACK / grace | **XCloud4** | Unchanged; not PSBox CMake patches |
| Inter fonts + cacert | **Packaged** | `assets/fonts/Inter-*.ttf`, `assets/cacert.pem` via `empaquetar.sh` |
| Exit LoadExec path | **XCloud4** | `lifecycle.c` LoadExec kept; PSBox `_exit` only on SIGTERM |
| Dual GSSV auth / MSA device-code parallel to X4 | **Excluded** | Would duplicate login and re-expose tokens; catalog/session stay on X4 |

Console stream duration: **not tested** (worker `adam-windows-ps4` offline / Cloud agent has no LAN).
