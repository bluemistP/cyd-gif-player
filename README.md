# CYD GIF Player

Animated GIF player for the **Cheap Yellow Display (ESP32-2432S028, CYD2USB variant)**, with a browser-based upload UI — paste a GIF URL or pick a local file, and it streams to the SD card and plays on the 2.8" screen.

### 🚀 [**Flash your CYD right now — no installs, just your browser →**](https://bluemistp.github.io/cyd-gif-player/web-installer/)

*(Chrome or Edge on desktop required — see [Quick Start](#1-build-and-flash) below for details and the PlatformIO alternative.)*

https://github.com/user-attachments/assets/1133a82c-9222-4717-8181-4d4a0dc6d4e3

## Human is speaking here

This project is an iterative build coded with AI models and agents. I, the human author of this software, orchestrated this project as a means to both learn about the target CYD hardware, as well as learn about the nature of agentic coding itself. At my best effort, I ensured that no proprietary licenses have been violated (see reference Libraries below) by the actions of the coding agents, as well as trying to have a human review for any artifact outputs that it makes.

That said, I'm only human and this is more of an enthusiasm project, this is not my area of expertise. Please do inform me of any concerning items if they do pop up in any form. For disclosure, the project was not coded by any singular AI model alone, it is a mix of both online and local models, and a lot of human testing manhours and feedback were done to keep the project in workable, decent form. Hoping for your understanding on this matter.

## Overview

```
[Browser: paste URL or pick file → decode/resize in-browser if needed → POST to CYD]
        │  (plain HTTP, local LAN)
        ▼
[CYD ESP32: SD card → AnimatedGIF → TFT_eSPI → 2.8" screen]
```

The browser does all the heavy lifting (decoding, resizing, re-encoding); the ESP32 only ever receives finished GIF bytes and writes them to SD.

## Hardware

This project targets the **dual-USB "CYD2USB"** revision specifically, matching the board pinout and bus layout documented in the hardware reference: [ESP32 Touch Specification](https://robotcoders.net/2025/11/esp32touchspecification/).

- **ESP32-2432S028**, dual USB-C + USB-B (via CH340)
- 2.8" **ST7789** TFT (240×320) — *not* ILI9341, despite the board's product-family name
- Resistive touch (XPT2046) on its own dedicated SPI bus
- microSD slot on its own dedicated SPI bus (SD, touch, and the display are three physically independent SPI buses on this board — they don't share wiring)
- Class 10 microSD card (8GB+ recommended) — **optional**, see below

⚠️ **Other CYD variants exist** with different display controllers and pinouts (e.g. the original single-USB ILI9341 board). This firmware's `User_Setup.h` and pin defines are specific to the dual-USB ST7789 variant described in that specification — using it on a different board revision will likely just show a blank/dark screen.

Full pin reference: [`firmware/src/User_Setup.h`](firmware/src/User_Setup.h) (display) and the `#define` block near the top of [`firmware/src/main.cpp`](firmware/src/main.cpp) (touch, SD, backlight) — those are the actual source of truth this firmware runs against, not a copy pasted elsewhere.

## Quick Start

### 1. Build and flash

**Install PlatformIO first** — it's the build tool/toolchain manager this firmware is built with (handles downloading the ESP32 compiler, libraries, and flashing, so you don't install any of that by hand). Easiest path: install the free [PlatformIO IDE extension for VS Code](https://platformio.org/install/ide?install=vscode) — it bundles everything, including the `platformio` CLI command used below (VS Code's own integrated terminal picks it up automatically). Prefer a plain CLI with no editor involved? `pip install platformio` works too, if you already have Python.

```bash
cd firmware
platformio run
platformio run --target upload
platformio run --target uploadfs   # web UI + GIF encoder libraries
```

No Wi-Fi credentials or API keys need editing before flashing — see below.

**Alternative: flash from a browser, no PlatformIO install needed →** **[bluemistp.github.io/cyd-gif-player/web-installer](https://bluemistp.github.io/cyd-gif-player/web-installer/)**. It's a self-contained flashing tool built on [esptool-js](https://github.com/espressif/esptool-js) (Espressif's own browser-based esptool, vendored locally — no CDN dependency). Open it in Chrome or Edge, click "Connect to CYD", then "Flash Firmware". It flashes the same four images (`bootloader.bin`, `partitions.bin`, `firmware.bin`, `littlefs.bin`) that `platformio run --target upload`/`uploadfs` produce. (Building locally instead? The source is `web-installer/index.html` — see [Maintaining the web installer](#maintaining-the-web-installer) below, since those binaries have to be rebuilt and copied in after every firmware change; they're not generated automatically.)

### 2. Connect it to Wi-Fi

On first boot (or whenever it has no saved credentials), the CYD broadcasts its own access point, **`CYD-GIF-Setup`**. Connect to it from your phone or laptop and visit `192.168.4.1` to enter your home network's SSID/password. It saves them to flash and reboots onto your LAN.

⚠️ **No authentication.** The web UI and its HTTP API (`/upload`, `/delete`, `/clear`, `/browse`, ...) are wide open to anyone who can reach the device's IP — there's no login, and it's plain HTTP, not HTTPS. Fine for a typical trusted home LAN; think twice before putting it on a network you don't control, or forward its port to the internet.

### 3. Find the CYD's IP

The IP is shown on the CYD's screen once connected, and also logged over serial (115200 baud) as `IP: x.x.x.x`. Or skip this step entirely — see below.

### 4. Open the web UI

```
http://cydgifplayer.local/
```

Works on most desktop/mobile OSes out of the box (mDNS/Bonjour) — no need to look up the IP at all. If your network doesn't resolve `.local` names, fall back to the raw IP from step 3: `http://<CYD_IP>/`.

Paste a direct `.gif` URL (Giphy, Tenor, Wikimedia, etc. — the source needs permissive CORS for a URL paste to work) or pick a local file. If it's larger than the 240×320 screen, your browser downscales and re-encodes it before uploading, so oversized GIFs still work without manual resizing.

### 5. Play it

Tap a GIF in the on-screen list to play it — it loops in place until you tap again to advance to the next one. Tapping through every GIF once cycles back to the browser list (which also re-scans the SD card, so anything uploaded mid-session shows up).

## No SD card? No problem

If no SD card is present, the firmware doesn't block — it falls back to a small demo animation baked directly into the flash (not SPIFFS/LittleFS, an actual C byte array in the binary), so the device is always in a working, demoable state. Uploads still work without a card: they go into a small fixed 500KB budget in the same LittleFS partition that holds the web UI (that partition is only ~1.1MB total, so this is deliberately a fallback, not a real GIF library) — the web UI rejects an oversized file up front, and the built-in demo GIF can itself be "deleted" from the on-device list (it's not a real file, so this just hides it) to make room. Insert a FAT32-formatted SD card for the normal 8MB-per-file experience; the device detects it being inserted or removed at runtime, no reboot needed.

## Updating firmware later

There's no OTA/self-update mechanism — updating means re-flashing, the same way you flashed it the first time (either PlatformIO or the browser installer, see [Quick Start](#1-build-and-flash) above). Re-flashing is safe and doesn't lose anything that matters: Wi-Fi credentials, touch calibration, and other settings live in a separate flash region untouched by a normal firmware/filesystem update, and your GIFs stay on the SD card (or in the LittleFS fallback) regardless. The one exception is a full chip erase (`esptool.py erase_flash`) — that wipes everything, including saved settings — which is not part of the normal update flow and shouldn't be needed.

## Maintaining the web installer

`web-installer/` bundles pre-built binaries — it has no build step of its own and doesn't know when the firmware source has changed. **After any change to `firmware/src/` or `firmware/data/`, rebuild and re-copy the four files it flashes:**

```bash
cd firmware
platformio run                     # produces firmware.bin, bootloader.bin, partitions.bin
platformio run --target buildfs    # produces littlefs.bin, without uploading it anywhere

cp .pio/build/esp32dev/bootloader.bin  ../web-installer/
cp .pio/build/esp32dev/partitions.bin  ../web-installer/
cp .pio/build/esp32dev/firmware.bin    ../web-installer/
cp .pio/build/esp32dev/littlefs.bin    ../web-installer/
```

There's no file-presence check in the installer UI itself — a missing or stale binary just flashes whatever's there (or fails with a clear fetch error if a file is genuinely missing), and the device won't reflect the latest code if you forget this step. `esptool-bundle.js` (the vendored esptool-js library) only needs updating if you deliberately rebuild it from a newer esptool-js source — it's independent of the firmware itself. See the comments in `web-installer/index.html`'s flashing code for how each write is verified against the device's own checksum before being considered successful.

## Project Structure

```
cyd-gif-player/
├── firmware/
│   ├── platformio.ini          # PlatformIO config
│   ├── partitions.csv          # Flash layout (app + LittleFS partition named "spiffs")
│   ├── strip_build_paths.py    # Pre-build script — strips the local build machine's
│   │                           #   path (e.g. username) out of the compiled firmware.bin
│   ├── src/
│   │   ├── main.cpp            # Boot, Wi-Fi provisioning, SD, TFT, GIF playback, web server
│   │   ├── User_Setup.h        # TFT_eSPI config for the CYD2USB/ST7789 board
│   │   └── default_gif.h       # Auto-generated fallback GIF, embedded in flash
│   ├── data/                   # Uploaded to LittleFS via `uploadfs`
│   │   ├── index.html          # Web UI (paste URL / upload file / manage files)
│   │   ├── gif.js, gif.worker.js  # Client-side GIF encoder (vendored, no CDN)
│   │   └── gifuct.js           # Client-side GIF decoder (vendored, no CDN)
│   └── lib/                    # Optional local library overrides
├── web-installer/              # Browser-based flasher (see "Maintaining the web installer")
│   ├── index.html              # UI + flashing logic, built on esptool-js
│   ├── esptool-bundle.js       # Vendored esptool-js (ESM, no CDN)
│   ├── esptool-js-LICENSE      # esptool-js's own license (Apache-2.0)
│   └── *.bin                   # Pre-built binaries — rebuild after firmware changes
├── LICENSE
└── README.md
```

## Architecture

### Firmware Components

| Component | Library | Purpose |
|---|---|---|
| Display | `TFT_eSPI` (Bodmer) | ST7789 driver, batched-row pixel rendering |
| GIF Decoder | `AnimatedGIF` (bitbank2) | Scanline callback-based GIF playback, from SD or flash |
| Web Server | `ESPAsyncWebServer` + `AsyncTCP` | Non-blocking HTTP server |
| SD Card | Built-in `SD` | GIF storage, own dedicated SPI bus |
| Touch | `XPT2046_Touchscreen` | Own dedicated SPI bus, independent of display and SD |
| Filesystem | `LittleFS` | Web UI assets + client-side GIF encoder/decoder libs |
| Wi-Fi | Built-in `WiFi.h` + `Preferences` | Captive-portal provisioning, credentials saved in NVS |

### Key Design Decisions

1. **Browser does the heavy lifting** — decoding, resizing, and re-encoding GIFs all happen client-side; the ESP32 only ever streams finished bytes to SD.
2. **Three independent SPI buses** — display, touch, and SD are wired separately on this board and are kept on separate `SPIClass` instances in firmware; sharing one bus across them (as some example code assumes) doesn't match this board's actual wiring.
3. **SD is optional** — a built-in demo GIF is compiled into flash so the device always has something to show.
4. **`/browse` and `/file` serve from the in-memory GIF list**, not a live SD scan, to avoid racing with active playback on the same SD SPI bus. Thumbnail loading in the web UI is deliberately sequential for the same reason — the SD card is a single shared bus with no concurrency guard, and parallel requests to it can stall each other out.
5. **Stream uploads directly to SD** — no RAM buffering; 8MB cap per file.

## Known Risks

- **SD SPI bus contention** — no mutex around concurrent SD access. Confirmed in practice: parallel HTTP requests reading SD (e.g. multiple thumbnail loads) can stall each other. Mitigated by serializing client-side thumbnail loads and serving `/browse` from cache, but *not* fully solved — an upload arriving during active playback, for instance, is still a theoretical race.
- **RAM pressure** — AnimatedGIF + web server + Wi-Fi all compete for ~320KB usable RAM. Scanline mode avoids full-frame buffers.
- **GIF decode limit** — the `AnimatedGIF` library refuses to open any GIF wider than 480px, regardless of file size. The web UI's client-side resizer targets 240×320 specifically to stay well under this.
- **CYD hardware variants** — pin mappings and display controllers differ across boards sold under similar names. This firmware is tuned for one specific variant (see Hardware above).

## Troubleshooting

**CYD screen is dark / shows nothing:**
- Confirm this is genuinely the CYD2USB (dual-USB, ST7789) variant — a single-USB ILI9341 board needs a different `User_Setup.h` entirely (driver, pins, and RGB order all differ).
- Check the backlight: `TFT_BL`/`TFT_BACKLIGHT_ON` both need to be defined for `TFT_eSPI` to auto-drive the backlight pin.

**Wi-Fi provisioning doesn't seem to start:**
- Make sure nothing else (like a missing SD card) is blocking boot before it reaches the provisioning step — check serial output at 115200 baud.

**Web UI shows "No SD card inserted" on upload:**
- Insert a FAT32-formatted card and reboot; the boot log will say `SD OK (XX MB)` once it's detected.

**Upload succeeds but the GIF doesn't show in the list:**
- If a GIF is actively playing, the device won't rescan or exit playback until you tap through to cycle back to the browser list — a fresh upload only appears once you're back there.

**GIF playback looks slow or bugs out on odd GIFs:**
- Very long/high-frame-count GIFs may still land close to the 8MB cap even after client-side resizing — the resizer only reduces resolution, not frame count.

**Touch taps land on the wrong spot**
- To recalibrate: press (don't hold through reset) the physical **BOOT** button within ~2.5 seconds after boot — the screen prompts for this window, then walks through a 2-point crosshair calibration. GPIO0 (BOOT) is the ESP32's UART-download strapping pin, so holding it *through* a reset/power-on instead drops the chip into the ROM bootloader (blank screen, no serial output) rather than running the app — the press has to happen after boot, once app code is already executing.

**Forgot the Wi-Fi password you provisioned it with, or need to move it to a new network:**
- There's no menu option for this (deliberately — a credential reset forces re-provisioning with no undo, so it isn't a single accidental tap away). Instead: within that same post-boot BOOT-button window, **hold it down continuously for 3 seconds** instead of a quick press — the screen confirms the credentials were cleared and reboots straight into `CYD-GIF-Setup` provisioning. Only Wi-Fi credentials are affected; touch calibration, brightness, and other settings are untouched.

## Libraries

| Library | Source | Purpose |
|---|---|---|
| TFT_eSPI | [bodmer/TFT_eSPI](https://github.com/bodmer/TFT_eSPI) | Display driver |
| AnimatedGIF | [bitbank2/AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) | GIF decoding |
| ESPAsyncWebServer | [me-no-dev/ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | Web server |
| AsyncTCP | [me-no-dev/AsyncTCP](https://github.com/me-no-dev/AsyncTCP) | Async networking |
| XPT2046_Touchscreen | [PaulStoffregen/XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) | Touch input |
| gif.js | [jnordberg/gif.js](https://github.com/jnordberg/gif.js) | Client-side GIF encoding (vendored in `data/`) |
| gifuct-js | [matt-way/gifuct-js](https://github.com/matt-way/gifuct-js) | Client-side GIF decoding (vendored, bundled via esm.sh) |
| esptool-js | [espressif/esptool-js](https://github.com/espressif/esptool-js) | Browser-based flashing over Web Serial (vendored in `web-installer/`) |

## License

MIT
