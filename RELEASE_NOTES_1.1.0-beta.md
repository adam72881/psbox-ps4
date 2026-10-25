# XCloud4 1.1.0-beta — Public Beta Release Notes

**XCloud4** is an independent, native Xbox Cloud Gaming client for PlayStation 4 consoles running GoldHEN, built with the open-source OpenOrbis PS4 Toolchain.

> **Mission:** Helping more people play next-gen games without buying a new console.

---

## What's New in 1.1.0-beta

- **1080p60 Streaming by Default:**
  - Full HD (1920×1080 at 60 fps) streaming is now enabled by default across all cloud sessions.
  - Utilizes an optimized 2-frame decode pipeline (`decoder_depth = 2` via Auto), maintaining a stable presentation at a steady ~58 fps in our tests.
  - Preserves network bandwidth efficiency: targets ~7 Mbps average with an 8 Mbps cap (`1080p-G8` profile), using the same bandwidth budget as the previous 720p stream.
- **Lower Latency Presentation (VSync-Free Display):**
  - Frames are presented immediately upon decode without waiting for the vertical blank interval (`ORBIS_VIDEO_OUT_FLIP_HSYNC`), removing the 11–16 ms flip observation delay.
  - **Measured Local Latency:** In hardware benchmarks on PS4 Fat running *Fortnite*, local presentation latency dropped from **43 ms down to 27 ms**, delivering noticeably snappier and more immediate controller response.
  - **Visual Trade-off Note:** In very fast camera sweeps or horizontal pans, a faint tearing line may occasionally appear. In console testing, gameplay felt noticeably more responsive; the tester did not notice tearing.
- **Direct Sign-in QR Code:**
  - The Microsoft device sign-in screen now generates a scannable QR code alongside the on-screen code, pointing directly to `https://www.microsoft.com/link?otc=<user_code>`.
  - Scan with a smartphone camera to sign in directly without typing the code manually.
- **Update Notifications:**
  - XCloud4 checks the public GitHub Releases API once at startup in the background (10-second timeout, non-blocking). XCloud4 sends no account or personal data; GitHub only sees the connection, as with any website.
  - Displays a green notice on the main menu when a new release is available (`NUEVA VERSION DISPONIBLE: <version> · GITHUB.COM/ASP11EDGR/XCLOUD4` / `NEW VERSION AVAILABLE: <version> · GITHUB.COM/ASP11EDGR/XCLOUD4`), along with detailed status on the About page.
- **Smoother Audio (Adaptive Audio Engine, since 1.0.2):**
  - **Dynamic Headroom Buffer (50–100 ms):** Adapts to network jitter; expands temporarily during delayed packet bursts and scales down during stable network conditions.
  - **Gentle Crossfaded Trimming:** Keeps the buffer small with crossfaded trims designed to avoid clicks (rate-limited <=10 ms / 200 ms).
  - **Low-Latency 512-Sample Blocks:** Delivers audio in ~10.7 ms increments.
- **DualShock 4 Touchpad In-Game Menu (since 1.0.1):**
  - Hold **two fingers on the touchpad for 1 second** during gameplay to open the in-game menu.
  - Cleanly bypasses in-game chord conflicts (such as EA Sports FC Power Shot `L1+R1+Circle`).
  - Options: **Return to catalog**, **Mute / Unmute audio**, **Xbox button** (sends Guide/Nexus pulse), and **Cancel**.
  - Automatically closes after 15 seconds of inactivity. Sends neutral gamepad state while open.

---

## Package Information

- **File:** `XCloud4-1.1.0-beta-publico.pkg`
- **PS4 Menu Title:** `XCloud4`
- **Title ID / Content ID:** `XCLD00003` (`IV0000-XCLD00003_00-XCLOUD4PUBLIC000`)
- **Product Version / PS4 `APP_VER`:** `1.1.0-beta` / `01.04`
- **Size:** `12,713,984` bytes
- **SHA-256:** `3510ff5d61237c12538b5752e5f578c31cc5f05fabd3fc4fbf04a0eb1975a09b`

---

## Requirements

- **Console:** PlayStation 4 running **GoldHEN** (tested on PS4 Fat, firmware **12.00**, **GoldHEN v2.4b18.7**).
- **Subscription:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not).
- **Network:** A stable internet connection of at least **15 Mbps**, preferably with the PS4 connected by wired Ethernet.

---

## Installation

1. Copy `XCloud4-1.1.0-beta-publico.pkg` to your PS4 using a USB drive or via the GoldHEN FTP server to `/data/pkg`.
2. On the PS4, open **GoldHEN > Debug Settings > Package Installer** and install the package. (It cleanly updates over `01.03`, `01.02`, `01.01`, or `01.00`).
3. Launch **XCloud4** from the PS4 home screen and sign in with your Microsoft account at the address shown on screen (usually `microsoft.com/link`) using the on-screen code.

*(If you also install `XCloud4-0.7.55-lab.pkg`, both editions can coexist on the PS4 menu because they use different Title IDs and icons.)*

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

- **Tested only on PS4 Fat, FW 12.00:** Validated on standard PlayStation 4 hardware under firmware 12.00.
- **Catalog availability:** The playable cloud gaming catalog depends on your Xbox Game Pass plan and region.
- **Network sensitivity:** Brief video or audio pauses may occur if the network drops or delays packets. A wired Ethernet connection is strongly recommended.

---

## Mission, Transparency, and Support

XCloud4 is created with the mission of **helping more people play next-gen games without buying a new console**.

- **Voluntary donations:** If you would like to support development, you can contribute voluntarily at [https://ko-fi.com/asapedgr](https://ko-fi.com/asapedgr). Donations are completely optional and **do not grant access to any games or subscriptions**.
- **Sign-in client identifier:** XCloud4 authenticates through Microsoft's official OAuth device-code flow at the address shown on screen (usually `microsoft.com/link`). When the project tested its own application registration, the streaming authorization step (Passport) rejected it with `invalid_scope`; for that reason, XCloud4 uses the public client identifier that is also used by other open-source clients. Passwords are entered only on Microsoft's website, and tokens live only in PS4 RAM during the session.
- **Non-affiliation notice:** XCloud4 is an independent homebrew project and is **not affiliated with, endorsed by, or sponsored by Microsoft, Xbox, or Sony Interactive Entertainment**.
- **Source code and license:** Licensed under **GPL-3.0-only**. Complete source code, build instructions ([BUILDING.md](BUILDING.md)), license terms ([LICENSE](LICENSE)), and third-party notices ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) are included in the repository.
