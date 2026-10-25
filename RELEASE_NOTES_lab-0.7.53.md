# XCloud4 Lab 0.7.53-lab — Experimental Tester Release Notes

> **EXPERIMENTAL BUILD WARNING:** `XCloud4 Lab` is an experimental laboratory edition intended for advanced testers and protocol/decoder diagnostics. Some combinations of settings (such as 1080p streaming profiles or alternative decoder pipeline depths) **may be unstable, freeze, or abort the stream**. For everyday play, use the stable **Public Beta (`XCloud4-1.0.2-beta-publico.pkg`)**.

---

## Package Information

- **File:** `XCloud4-0.7.53-lab.pkg`
- **PS4 Menu Title:** `XCloud4 Lab`
- **Title ID / Content ID:** `XCLD00001` (`IV0000-XCLD00001_00-XCLOUD4APP000000`)
- **Product Version / PS4 `APP_VER`:** `0.7.53-lab` / `01.21`
- **Size:** `13,565,952` bytes
- **SHA-256:** `230b66f37a03eaa05f4fa0eda0c71cea2a73d8d87b8ea20b61b22b4f57462963`

### Side-by-Side Installation with the Public Beta

`XCloud4 Lab` uses its own Title ID (`XCLD00001`), menu title (`XCloud4 Lab`), and `LAB` badge icon (`assets/icon0-lab.png`). It can be installed on the PS4 **at the same time** as the Public Beta (`XCloud4`, `XCLD00003`) without overwriting it.

---

## What's New in 0.7.53-lab

- **Smoother Adaptive Audio:**
  - **Dynamic Headroom Buffer (50–100 ms):** The audio buffer now automatically expands following network delays/stalls, preserving delayed packets within target headroom rather than discarding them immediately. After stability is maintained, target headroom steps down gradually.
  - **Gentle Crossfaded Trimming:** When trimming is required to prevent buffer bloat, cuts are bounded (<=10 ms per 200 ms) and smoothly crossfaded to avoid clicks and pops.
  - **Low-Latency 512-Sample Audio Blocks:** AudioOut requests 512-sample blocks natively (with automatic fallback to 1024), halving buffer granularity and reducing audio latency.
  - **Measured Console Validation:** In a 16-minute live test on PS4, audio underflow episodes plummeted from 35–109/min (in 0.7.48) to ~1.05/min (17 episodes total), and PLC concealment dropped to 0.07%.
- **Enhanced `/data` Diagnostic Metrics:**
  - `target_ms`: Logs dynamic target adjustments, current target, median, and maximum.
  - `trim_events` and `trimmed_ms`: Explicitly reports gentle trim actions and sample counts.
  - `audio_out_open`: Records native 512-sample negotiation and exact return codes.
- **In-Game Touchpad Menu (since 0.7.52):** Hold **two fingers on the DualShock 4 touchpad for 1 second** during streaming to open the in-game menu.
- **Removed L1+R1 Chords:** Fixes GitHub issue #2 (EA Sports FC Power Shot conflict). Legacy `L1+R1` shortcuts are removed.
- **Lab Menu Options:**
  - **Return to catalog:** Cleanly disconnects the current session and returns to the catalog.
  - **Mute / Unmute audio:** Toggles audio stream state.
  - **Xbox button:** Sends the **Xbox Guide / Nexus** button (~200 ms pulse).
  - **Mark test:** Toggles the diagnostic epoch marker (`stable` / `transition`) in session traces.
  - **Exit app:** Initiates clean application exit.
  - **Cancel:** Closes the menu and resumes play.
- **15-Second Inactivity Timeout:** Automatically closes if no buttons are pressed for 15 seconds.
- **Neutral Gamepad Input:** Sends neutral controller state to Xbox while the menu is open, preventing game interactions.

---

## Experimental Features Included in `0.7.53-lab`

### 1. 720p and 1080p Request Profiles

From the catalog, testers can switch between six cloud session request profiles before launching a game:
- `720p60 · android` (default baseline)
- `720p-HQ · web windows · alias 720HQ`
- `1080p-G · web windows · alias 1080`
- `1080p-G8 · same as G · 8 Mbps request`
- `1080p-H · native windows · alias 1080`
- `1080p-HQ · web windows · alias 1080HQ`

### 2. Technical Settings Menu (`R3` in Catalog)

Press **`R3`** inside the catalog to open the next-session experimental configuration panel (`Opciones de la próxima sesión`), adjust fields with **D-Pad Up/Down/Left/Right**, and press **`X`** to apply:
- **Request Profile (`Perfil de solicitud`):** Select among the 720p and 1080p request profiles above.
- **Fallback Color Matrix (`Color si el SPS no especifica la matriz`):** `Auto (BT.709 for >=720p)` or `BT.601`.
- **CPU Scheduling & Affinity (`CPU de la próxima sesión`):** `CPU-0`, `CPU-1`, `CPU-2`, or `CPU-3`.
- **Decoder Pipeline Depth (`Profundidad del decoder`):** `Auto (720p: 1 · 1080p: 2)`, `1`, or `2`.
- **Controller Sending (`Envío del mando`):** `Immediate` (`Inmediato`) or `60 Hz (previous)`.
- **Vibration (`Vibración`):** `Yes` (`Sí`) or `No`.
- **Audio Mode (`Audio`):** `New` (`Nuevo`, adaptive) or `Previous` (`Anterior`).

### 3. Automatic `/data` Session Diagnostics

Every streaming session writes bounded numeric diagnostic files to `/data` on the PS4 (`/data/xcloud4-stream-*`, `/data/xcloud4-trace-*`). These contain no tokens or credentials.

---

## Requirements and Installation

- **Console:** PlayStation 4 with **GoldHEN** (tested on PS4 Fat, firmware **12.00**, **GoldHEN v2.4b18.7**).
- **Subscription:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not).
- **Network:** At least **15 Mbps** stable internet connection, preferably via wired Ethernet.
- **Installation:** Copy `XCloud4-0.7.53-lab.pkg` to a USB drive or via GoldHEN FTP to `/data/pkg` and install it with the **GoldHEN Package Installer**.

---

## Mission, Non-Affiliation, and License

- **Mission:** Helping more people play next-gen games without buying a new console.
- **Voluntary support:** [https://ko-fi.com/asapedgr](https://ko-fi.com/asapedgr) (voluntary donations support development and do not grant access to games or subscriptions).
- **Non-affiliation:** Independent open-source project; **not affiliated with, endorsed by, or sponsored by Microsoft, Xbox, or Sony Interactive Entertainment**.
- **License:** **GPL-3.0-only**. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [BUILDING.md](BUILDING.md).
