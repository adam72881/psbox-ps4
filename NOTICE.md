# Notice — upstream PSBox (PS5)

This project is a **port** of [PSBox Cloud Gaming for PlayStation 5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) to PlayStation 4 (GoldHEN).

| | |
| --- | --- |
| Upstream | <https://github.com/RafaelNGP/xbox-cloud-gaming-ps5> |
| Upstream author | RafaelNGP |
| Baseline | tag **v0.9.8**, commit `f8b272621e1df0ce4a293acb8162299328724dc7` (see `UPSTREAM_PSBOX.txt`) |
| License | **GPL-3.0-or-later** (same as upstream; see `LICENSE`) |

Login Microsoft, Xbox Live, GSSV/xCloud catalog, WebRTC session path, and much of the UI are from that codebase — not a from-scratch rewrite. PS4-specific work (OpenOrbis, Videodec2, VideoOut BGRA720, DualShock 4, pkg/auto-update, Orbis shims) lives under `src/platform/ps4/`, `src/display/display_ps4.cpp`, `src/media/hw_decoder_ps4.cpp`, and related files.

Third-party components and their licenses are listed in `THIRD_PARTY_NOTICES.md`.

This is an unofficial homebrew project, not affiliated with Microsoft or Sony.
