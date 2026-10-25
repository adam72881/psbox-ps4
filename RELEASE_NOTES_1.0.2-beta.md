# XCloud4 1.0.2-beta — Public Beta Release Notes

**XCloud4** is an independent, native Xbox Cloud Gaming client for PlayStation 4 consoles running GoldHEN, built with the open-source OpenOrbis PS4 Toolchain.

> **Mission:** Helping more people play next-gen games without buying a new console.

---

## What's New in 1.0.2-beta

- **Smoother Audio (Adaptive Audio Engine):**
  - **Dynamic Headroom Buffer (50–100 ms):** The audio buffer now intelligently adapts to real-world network jitter. Following temporary packet stalls, the buffer expands to preserve incoming delayed bursts within safe headroom instead of abruptly truncating them. When network conditions remain stable, target headroom steps down automatically to preserve low latency.
  - **Gentle Crossfaded Trimming:** To prevent buffer bloat without audible artifacts, buffer trims are strictly rate-limited (<=10 ms every 200 ms) and smoothly crossfaded to avoid clicks, pops, and sudden volume jumps.
  - **Lower Audio Latency:** Audio output now negotiates native 512-sample blocks on PlayStation 4 (with automatic fallback to 1024), halving block delivery time from ~21.3 ms to ~10.7 ms.
  - **Measured Improvement:** In a 16-minute live cloud session on PS4 hardware, stutter/underflow episodes dropped dramatically from dozens per minute down to approximately one per minute (~1.05/min), with packet loss concealment reduced to just 0.07%.
- **DualShock 4 Touchpad In-Game Menu (since 1.0.1):**
  - Hold **two fingers on the touchpad for 1 second** during gameplay to open the in-game menu.
  - Fixes GitHub issue #2 (EA Sports FC Power Shot conflict): All `L1 + R1` chords have been removed so all game button combinations pass cleanly to Xbox.
  - Options: **Return to catalog**, **Mute / Unmute audio**, **Xbox button** (sends Guide/Nexus pulse), and **Cancel**.
  - Automatically closes after 15 seconds of inactivity. Sends neutral gamepad state while open.

---

## Package Information

- **File:** `XCloud4-1.0.2-beta-publico.pkg`
- **PS4 Menu Title:** `XCloud4`
- **Title ID / Content ID:** `XCLD00003` (`IV0000-XCLD00003_00-XCLOUD4PUBLIC000`)
- **Product Version / PS4 `APP_VER`:** `1.0.2-beta` / `01.03`
- **Size:** `12,713,984` bytes
- **SHA-256:** `09943670337212c039620e836c4d6b4148098888db7dc1b832a8f8b1ae00f7d1`

---

## Requirements

- **Console:** PlayStation 4 running **GoldHEN** (tested on PS4 Fat, firmware **12.00**, **GoldHEN v2.4b18.7**).
- **Subscription:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not).
- **Network:** A stable internet connection of at least **15 Mbps**, preferably with the PS4 connected by wired Ethernet.

---

## Installation

1. Copy `XCloud4-1.0.2-beta-publico.pkg` to your PS4 using a USB drive or via the GoldHEN FTP server to `/data/pkg`.
2. On the PS4, open **GoldHEN > Debug Settings > Package Installer** and install the package. (It cleanly updates over `01.02`, `01.01`, or `01.00`).
3. Launch **XCloud4** from the PS4 home screen and sign in with your Microsoft account at the address shown on screen (usually `microsoft.com/link`) using the on-screen code.

*(If you also install `XCloud4-0.7.53-lab.pkg`, both editions can coexist on the PS4 menu because they use different Title IDs and icons.)*

---

## Controls

### In-Game Menu (while streaming)

| Control | Action |
|---|---|
| **Hold two fingers on Touchpad for 1 s** | Open the in-game menu |
| **D-Pad Up / Down** | Navigate menu options |
| **CROSS (X)** | Confirm highlighted option |
| **CIRCLE** | Close menu and return to game |
| **Auto-close** | Closes automatically after 15 seconds of inactivity |

### Xbox Controller Mapping

| PS4 Button | Xbox Equivalent |
|---|---|
| **Touchpad click** | **Xbox View** (Change view / Back) |
| **OPTIONS** | **Xbox Menu** (Start) |
| **D-Pad, Sticks, Triggers, Face buttons** | Mapped 1:1 to Xbox controller |

### Exiting the Application

- Press the **PS button** and select **Close Application** (`PS button > Close Application`).

---

## Tested Hardware

- **Tested on:** Original PS4 (Fat), firmware **12.00**, **GoldHEN v2.4b18.7**.
- **Not yet tested on:** PS4 Slim, PS4 Pro, or other system firmware versions.

---

## Known Limitations

- **720p only in this version:** The Public Beta is locked to 720p60 for stability (1080p experimental profiles are available separately in `XCloud4 Lab`).
- **Catalog availability:** The playable cloud gaming catalog depends on your Xbox Game Pass plan and region.
- **Network sensitivity:** Brief video or audio pauses may occur if the network drops or delays packets. A wired Ethernet connection is strongly recommended.

---

## Mission, Transparency, and Support

XCloud4 is created with the mission of **helping more people play next-gen games without buying a new console**.

- **Voluntary donations:** If you would like to support development, you can contribute voluntarily at [https://ko-fi.com/asapedgr](https://ko-fi.com/asapedgr). Donations are completely optional and **do not grant access to any games or subscriptions**.
- **Sign-in client identifier:** XCloud4 authenticates through Microsoft's official OAuth device-code flow at the address shown on screen (usually `microsoft.com/link`). When the project tested its own application registration, the streaming authorization step (Passport) rejected it with `invalid_scope`; for that reason, XCloud4 uses the public client identifier that is also used by other open-source clients. Passwords are entered only on Microsoft's website, and tokens live only in PS4 RAM during the session.
- **Non-affiliation notice:** XCloud4 is an independent homebrew project and is **not affiliated with, endorsed by, or sponsored by Microsoft, Xbox, or Sony Interactive Entertainment**.
- **Source code and license:** Licensed under **GPL-3.0-only**. Complete source code, build instructions ([BUILDING.md](BUILDING.md)), license terms ([LICENSE](LICENSE)), and third-party notices ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) are included in the repository.
