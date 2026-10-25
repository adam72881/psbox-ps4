# XCloud4 1.0.0-beta — Public Beta Release Notes

**XCloud4** is an independent, native Xbox Cloud Gaming client for PlayStation 4 consoles running GoldHEN, built with the open-source OpenOrbis PS4 Toolchain.

> **Mission:** Helping more people play next-gen games without buying a new console.

---

## Package Information

- **File:** `XCloud4-1.0.0-beta-publico.pkg`
- **PS4 Menu Title:** `XCloud4`
- **Title ID / Content ID:** `XCLD00003` (`IV0000-XCLD00003_00-XCLOUD4PUBLIC000`)
- **Product Version / PS4 `APP_VER`:** `1.0.0-beta` / `01.01`
- **Size:** `12,713,984` bytes
- **SHA-256:** `d51c22cf0d25f30f214a28b6215ee3aacce06850bd5e765466254470ae39596d`

---

## Requirements

- **Console:** PlayStation 4 with **GoldHEN**.
- **Subscription:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not).
- **Network:** A stable internet connection of at least **15 Mbps**, preferably with the PS4 connected by wired Ethernet.

---

## Installation

1. Copy `XCloud4-1.0.0-beta-publico.pkg` to your PS4 using a USB drive or by uploading it via the GoldHEN FTP server to `/data/pkg`.
2. On the PS4, open **GoldHEN > Debug Settings > Package Installer** and install the package.
3. Launch **XCloud4** from the PS4 home screen and sign in with your Microsoft account at the address shown on screen (usually `microsoft.com/link`) using the on-screen code.

*(If you also install `XCloud4-0.7.51-lab.pkg`, both editions can coexist on the PS4 menu because they use different Title IDs and icons.)*

---

## What's Included

- **720p60 Cloud Streaming:** Native H.264 hardware decoding (`sceVideodec2`) and Opus stereo audio output (`sceAudioOut`), preconfigured for the stable 720p60 profile.
- **DualShock 4 Controller with Vibration:** Full gamepad input with rumble support mapped to Xbox Cloud Gaming.
- **Automatic Spanish / English Interface:** Automatically selects Spanish or English UI text at startup based on your PS4 system language (`sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG)`).
- **Automatic Regional Catalog and Server Locale:** Dynamically selects the Microsoft Store and Xbox Cloud Gaming `market` and `locale` matching your PS4 language setting, with a six-card graphical catalog, cover art cache, Recent Games tab, and on-screen search keyboard.
- **Support Screen QR Code:** Displays an on-screen QR code on the "Support the project" screen for optional Ko-fi donations.

---

## Controls

- **Hold `L1 + R1`** (in game): Show on-screen shortcut help.
- **`L1 + R1 + CIRCLE`** (in game): Disconnect and return to the catalog.
- **`L1 + R1 + SQUARE`** (in game): Mute or unmute audio.
- **`L1 + R1 + TOUCHPAD`** (in game): Open the Xbox button (Xbox Guide).
- **To exit XCloud4:** Press the **PS button > Close Application**.

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
- **Source code and license:** Licensed under **GPL-3.0-only**. Complete source code, build instructions ([BUILDING.md](BUILDING.md)), license terms ([LICENSE](LICENSE)), and third-party notices ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) are included in the repository and release archive (`XCloud4-1.0.0-beta-source.zip`).
