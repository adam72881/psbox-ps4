# XCloud4 Lab 0.7.55-lab — Experimental Tester Release Notes

> **EXPERIMENTAL BUILD WARNING:** `XCloud4 Lab` is an experimental laboratory edition intended for advanced testers and protocol/decoder diagnostics. Some combinations of settings (such as 1080p streaming profiles or alternative decoder pipeline depths) **may be unstable, freeze, or abort the stream**. For everyday play, use the stable **Public Beta (`XCloud4-1.1.0-beta-publico.pkg`)**.

---

## Package Information

- **File:** `XCloud4-0.7.55-lab.pkg`
- **PS4 Menu Title:** `XCloud4 Lab`
- **Title ID / Content ID:** `XCLD00001` (`IV0000-XCLD00001_00-XCLOUD4APP000000`)
- **Product Version / PS4 `APP_VER`:** `0.7.55-lab` / `01.23`
- **Size:** `13,565,952` bytes
- **SHA-256:** `7f7eaa4a662df96b6a1561ae389600b7d9578c740dee1541b6a9fb9aa6802a99`

### Side-by-Side Installation with the Public Beta

`XCloud4 Lab` uses its own Title ID (`XCLD00001`), menu title (`XCloud4 Lab`), and `LAB` badge icon (`assets/icon0-lab.png`). It can be installed on the PS4 **at the same time** as the Public Beta (`XCloud4`, `XCLD00003`) without overwriting it.

---

## What's New in 0.7.55-lab

- **Direct Sign-in QR Code:**
  - Displays a scannable QR code on the Microsoft device-code authentication screen alongside the code, pointing directly to `https://www.microsoft.com/link?otc=<user_code>`.
  - Scan with a smartphone camera to sign in without manually typing the code.
- **Update Notifications (Lab Releases):**
  - Queries GitHub Releases API once at startup in the background (10-second timeout, non-blocking). XCloud4 sends no account or personal data; GitHub only sees the connection, as with any website.
  - Checks for newer experimental builds starting with `lab-` and displays a notification banner on the main menu and About page.
- **Optional Vertical Synchronization (VSync Toggle):**
  - **New Row in R3 Settings Menu:** Adds Row 8 ("Sincronía vertical") with options **Sí** (VSync ON, default) and **No (menos retraso, puede verse un corte)** (VSync OFF / HSYNC).
  - **Lower Presentation Latency:** When set to `No`, frame presentation bypasses waiting for the vertical blank interval (`ORBIS_VIDEO_OUT_FLIP_HSYNC`), eliminating 11–16 ms of local flip observation delay.
  - **Measured Console Results (PS4 Fat hardware):**
    - **Measurement A / A' (720p):** Local presentation latency dropped from **33 ms** (VSync ON) to **17 ms** (VSync OFF).
    - **Measurement B / B' (1080p):** Local presentation latency dropped from **43 ms** (VSync ON) to **27 ms** (VSync OFF).
    - In console testing with *Fortnite* at 1080p-G8 (decoder depth 2, VSync OFF), the stream sustained 57.9 fps with 7.2 Mbps bandwidth and no noticeable tearing during gameplay, resulting in significantly more responsive controls.
- **Session Synchronization Diagnostics:**
  - Every session exported to `/data/xcloud4-stream-*.json` includes an explicit `"synchronization"` block:
    ```json
    "synchronization": {"flip_mode": "hsync", "vsync_enabled": false}
    ```
- **Smoother Adaptive Audio (since 0.7.53):** Dynamic headroom buffer (50–100 ms), gentle crossfaded trims (<=10 ms / 200 ms), and low-latency 512-sample AudioOut blocks.
- **In-Game Touchpad Menu (since 0.7.52):** Hold **two fingers on the DualShock 4 touchpad for 1 second** to open the in-game menu; no `L1+R1` conflicts.

---

## Experimental Features Included in `0.7.55-lab`

### 1. 720p and 1080p Request Profiles

From the catalog, testers can switch between six cloud session request profiles before launching a game:
- `720p60 · android` (default baseline)
- `720p-HQ · web windows · alias 720HQ`
- `1080p-G · web windows · alias 1080`
- `1080p-G8 · same as G · 8 Mbps request`
- `1080p-H · native windows · alias 1080`
- `1080p-HQ · web windows · alias 1080HQ`

### 2. Technical Settings Menu (`R3` in Catalog)

Press **`R3`** inside the catalog to open the 8-row experimental configuration panel (`Opciones de la próxima sesión`):
- **Request Profile (`Perfil de solicitud`):** Select among 720p and 1080p profiles.
- **Fallback Color Matrix (`Color si el SPS no especifica la matriz`):** `Auto (BT.709)` or `BT.601`.
- **CPU Scheduling & Affinity (`CPU de la próxima sesión`):** `CPU-0`, `CPU-1`, `CPU-2`, or `CPU-3`.
- **Decoder Pipeline Depth (`Profundidad del decoder`):** `Auto (720p: 1 · 1080p: 2)`, `1`, or `2`.
- **Controller Sending (`Envío del mando`):** `Immediate` (`Inmediato`) or `60 Hz (previous)`.
- **Vibration (`Vibración`):** `Yes` (`Sí`) or `No`.
- **Audio Mode (`Audio`):** `New` (`Nuevo`, adaptive) or `Previous` (`Anterior`).
- **Vertical Synchronization (`Sincronía vertical`):** `Yes` (`Sí`) or `No (menos retraso, puede verse un corte)`.

### 3. Automatic `/data` Session Diagnostics

Every streaming session writes diagnostic files to `/data` on the PS4 (`/data/xcloud4-stream-*`, `/data/xcloud4-trace-*`).

---

## Requirements and Installation

- **Console:** PlayStation 4 with **GoldHEN** (tested on PS4 Fat, firmware **12.00**, **GoldHEN v2.4b18.7**).
- **Subscription:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not).
- **Network:** At least **15 Mbps** stable internet connection, preferably via wired Ethernet.
- **Installation:** Copy `XCloud4-0.7.55-lab.pkg` to a USB drive or via GoldHEN FTP to `/data/pkg` and install it with the **GoldHEN Package Installer**.

---

## Mission, Non-Affiliation, and License

- **Mission:** Helping more people play next-gen games without buying a new console.
- **Voluntary support:** [https://ko-fi.com/asapedgr](https://ko-fi.com/asapedgr) (voluntary donations support development and do not grant access to games or subscriptions).
- **Non-affiliation:** Independent open-source project; **not affiliated with, endorsed by, or sponsored by Microsoft, Xbox, or Sony Interactive Entertainment**.
- **License:** **GPL-3.0-only**. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [BUILDING.md](BUILDING.md).
