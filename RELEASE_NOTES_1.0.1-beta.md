# XCloud4 1.0.1-beta — Public Beta Release Notes

**XCloud4** is an independent, native Xbox Cloud Gaming client for PlayStation 4 consoles running GoldHEN, built with the open-source OpenOrbis PS4 Toolchain.

> **Mission:** Helping more people play next-gen games without buying a new console.

---

## What's New in 1.0.1-beta

- **New In-Game Touch Menu:** Replacing the old `L1 + R1` button chords, players can now hold **two fingers on the DualShock 4 touchpad for 1 second** during gameplay to open a dedicated, centered in-game menu.
- **Fixes GitHub Issue #2 (EA Sports FC Power Shot conflict):** In EA Sports FC, executing a Power Shot requires pressing `L1 + R1 + CIRCLE`. In version 1.0.0-beta, this shortcut intercepted the buttons and abruptly terminated the session to return to the catalog. All `L1 + R1` in-game shortcuts have been completely removed, allowing every game button combination to pass to Xbox uninterrupted.
- **In-Game Menu Actions:**
  - **Return to catalog:** Disconnects the cloud session and returns to the game catalog.
  - **Mute / Unmute audio:** Toggles stream audio on or off.
  - **Xbox button:** Sends the **Xbox Guide / Nexus** button to the cloud console for ~200 ms and resumes play.
  - **Cancel:** Closes the menu and returns to gameplay.
- **In-Game Menu Quality of Life:**
  - Automatically closes after **15 seconds** of inactivity.
  - While open, neutral gamepad state is sent to the Xbox session so navigation buttons (`D-Pad Up/Down`, `CROSS`, `CIRCLE`) do not trigger game actions. Video and audio streaming continue smoothly in the background.
  - Requires lifting fingers before the menu can be reopened, avoiding accidental re-triggers.
  - Bilingual interface (Spanish / English) dynamically matching the console language.
  - Initial 6-second on-screen banner when video streaming begins to inform players of the touchpad gesture.

---

## Package Information

- **File:** `XCloud4-1.0.1-beta-publico.pkg`
- **PS4 Menu Title:** `XCloud4`
- **Title ID / Content ID:** `XCLD00003` (`IV0000-XCLD00003_00-XCLOUD4PUBLIC000`)
- **Product Version / PS4 `APP_VER`:** `1.0.1-beta` / `01.02`
- **Size:** `12,713,984` bytes
- **SHA-256:** `5ed0e51a3e01166ee56b98ff7e74a1be4db544fa1c87d5c34bdf8474a9cc6a05`

---

## Requirements

- **Console:** PlayStation 4 running **GoldHEN** (tested on PS4 Fat, firmware **12.00**, **GoldHEN v2.4b18.7**).
- **Subscription:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not).
- **Network:** A stable internet connection of at least **15 Mbps**, preferably with the PS4 connected by wired Ethernet.

---

## Installation

1. Copy `XCloud4-1.0.1-beta-publico.pkg` to your PS4 using a USB drive or via the GoldHEN FTP server to `/data/pkg`.
2. On the PS4, open **GoldHEN > Debug Settings > Package Installer** and install the package. (It will cleanly update over `01.01` or `01.00`).
3. Launch **XCloud4** from the PS4 home screen and sign in with your Microsoft account at the address shown on screen (usually `microsoft.com/link`) using the on-screen code.

*(If you also install `XCloud4-0.7.52-lab.pkg`, both editions can coexist on the PS4 menu because they use different Title IDs and icons.)*

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
