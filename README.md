<!--
  README outline for the Steam Frame ambient light mod.
  Sections marked TODO are for you to write. Bracketed notes like [facts] are things I already know
  from the build and filled in or pointed to; check them against your final hardware.
  Delete these comments as you go.
-->

# Steam Frame Ambilight <!-- TODO: pick a name -->

<!-- TODO: one-sentence pitch, e.g. "LEDs inside the Steam Frame that extend the edges of the picture into
your peripheral vision, so the field of view feels wider." -->

<!-- TODO: hero photo or short GIF: headset on a table with the LEDs lit, plus one through-the-lens shot
if you can get it. GitHub plays .gif and .mp4 inline. -->

> **Status:** <!-- TODO: e.g. "Working daily driver on my Steam Frame (SteamOS x.y, SteamVR x.y). Expect rough edges." -->
> **Tested on:** Steam Frame, Arduino Nano ESP32, 24 × WS2812B 0807 LEDs. <!-- TODO: versions -->

## Table of contents
<!-- GitHub also auto-generates one (the ☰ icon on the README), but a short manual list helps on mobile. -->
1. [What it does](#what-it-does)
2. [How it works](#how-it-works)
3. [Safety and disclaimers](#safety-and-disclaimers)
4. [Bill of materials](#bill-of-materials)
5. [3D-printed parts](#3d-printed-parts)
6. [Hardware build](#hardware-build)
7. [Firmware (ESP32)](#firmware-esp32)
8. [Software (on the headset)](#software-on-the-headset)
9. [Calibration and tuning](#calibration-and-tuning)
10. [Troubleshooting](#troubleshooting)
11. [Uninstalling / going back](#uninstalling--going-back)
12. [Building from source](#building-from-source)
13. [Repository layout](#repository-layout)
14. [Credits and licenses](#credits-and-licenses)

---

## What it does
<!-- TODO: 3–5 bullets in your own words. Ideas from the build: -->
- Lights up the edges of your peripheral vision with colours taken from the very edge of what's on screen.
- Runs entirely on the headset: no PC software, works standalone and with Steam Link streaming.
- Follows the headset's display brightness automatically.
- Smooth, flicker-free dim colours (16-bit colour with temporal dithering) and colour correction for the LEDs.
- <!-- TODO: anything else you care about (latency feel, games you've tried, comfort) -->

<!-- TODO (optional): "Why I built this" paragraph and a nod to VR_Ambilight as the inspiration. -->

## How it works
<!-- Short overview for the curious; link to linux/README.md for details. Suggested diagram: -->
```
SteamVR compositor ──► capture layer ──► LED daemon ──► Wi-Fi (UDP) ──► ESP32 ──► LEDs
(on the headset)       (Vulkan layer)     (colours,       (same network)  (dithering,
                                           brightness)                      current limit)
```
- **Capture layer:** a small Vulkan layer inside SteamVR's compositor copies a shrunk-down version of the
  final panel image ~30 times a second. It never waits on the GPU, and it turns itself off if anything
  fails.
- **LED daemon:** a background service that works out each LED's colour from the lens edge. It uses
  OpenVR Ambient Light's colour pipeline, follows the panel brightness, applies colour correction for the
  LEDs, and sends the result over Wi-Fi.
- **Firmware:** the ESP32 receives the colours. It refreshes the strip 1000 times a second, with one core
  dedicated to the LEDs, and uses temporal dithering for smooth dim colours.
- **Why Wi-Fi and not USB:** the Frame's kernel has no USB-serial driver (`cdc_acm`). The USB port only
  powers the board.

## Safety and disclaimers
<!-- TODO: write in your own voice. Points worth covering: -->
- Opening or modifying the headset may void your warranty; done at your own risk.
- **Heat:** LEDs and the board sit close to your face. Mention the current limit (`MAX_MILLIAMPS`), your
  diffuser and brightness choices, and how warm it gets in practice. <!-- TODO: your measurements -->
- **Power:** the LEDs run on 5 V straight from the Frame's USB port, through the board's VBUS pin. The
  firmware caps the strip's current (`MAX_MILLIAMPS`, 500 mA by default). <!-- TODO: your setting -->
- **Brightness and flicker:** bright light close to the eyes; photosensitivity warning; how to turn it off
  quickly (`oalctl disable`, or just unplug the board).
- Not affiliated with Valve.

## Bill of materials
<!-- Fill in links, prices, quantities. Keep one table; group by category. -->
| Part | Qty | Notes | Link |
| --- | --- | --- | --- |
| Arduino Nano ESP32 (ESP32-S3) | 1 | Any ESP32-S3 should work; this guide uses the Nano's pin names | <!-- TODO --> |
| WS2812B 0807 addressable LEDs | 24 | 12 per side. <!-- TODO: kit name --> | <!-- TODO: amazon.com/dp/B0DWX8BPGY --> |
| Thin wire | <!-- TODO --> | <!-- TODO: gauge, colours --> | |
| USB-C cable / adapter | 1 | Powers the board from the Frame's USB port <!-- TODO: what you used (right-angle? length?) --> | |
| Light-reducing tape | <!-- TODO --> | Cuts brightness before diffusing | |
| Fabric tape | <!-- TODO --> | Diffuser layer(s) | |
| 3D-printed parts | see below | | |
| <!-- TODO: glue/adhesive, heat-shrink, Kapton, screws, magnets... --> | | | |

**Tools:** <!-- TODO: soldering iron, flush cutters, 3D printer, multimeter (recommended)... -->

## 3D-printed parts
<!-- TODO: one row per part. Put STL/STEP/3MF files in a `hardware/` folder and link them. -->
| Part | File | Material | Settings | Notes |
| --- | --- | --- | --- | --- |
| <!-- TODO: diffuser --> | `hardware/....stl` | <!-- e.g. PETG; translucent? --> | <!-- layer height, infill, supports --> | <!-- heat notes --> |
| <!-- TODO: mounts/brackets --> | | | | |

<!-- TODO: note any print-orientation tips, and whether parts are remixable (link the CAD source if you share it). -->

## Hardware build
<!-- TODO: your words + photos at every step. Suggested structure: -->
### 1. Plan the LED layout
- 24 LEDs: 12 down each side of the lenses. <!-- TODO: exact placement, photo -->
- Which end of each strip is the data input, and which LED is LED 0 (this decides the `BottomToTop*` /
  `SwapLeftRight` settings later).

### 2. Wiring
<!-- TODO: wiring diagram image (Fritzing, KiCad, or a clear hand drawing). -->
| From (Nano ESP32) | To (LED strip) | Notes |
| --- | --- | --- |
| D6 (GPIO9) | DIN of first LED | Data |
| VBUS (5 V from USB) | 5V/VDD | Current capped by `MAX_MILLIAMPS` in the firmware |
| GND | GND | |
<!-- TODO: how the two sides are chained (left then right?), wire routing, strain relief. -->

### 3. Mounting inside the headset
<!-- TODO: step-by-step with photos: removing the facial interface (?), attaching mounts, gluing strips. -->

### 4. Diffuser
<!-- TODO: your tape stack / printed diffuser, and how each changes brightness/heat. -->

### 5. Mounting the board
<!-- TODO: where the Nano lives, cable routing to the Frame's USB port. -->

## Firmware (ESP32)
[facts — check and keep]
1. Install the **Arduino IDE** and the **Arduino ESP32 boards** package (tested with core 2.0.18).
2. Install **FastLED 3.9.20**. Newer FastLED versions leave the strip stuck white on this core.
3. **Tools → Board:** Arduino Nano ESP32. **Tools → Pin Numbering:** "By GPIO number (legacy)".
4. Open `firmware/oal_led_udp_dither/oal_led_udp_dither.ino`. Copy `wifi_secrets.example.h` to
   `wifi_secrets.h` and enter your 2.4 GHz Wi-Fi details.
5. Check the settings at the top of the sketch: `NUM_LEDS`, `DATA_PIN`, `MAX_MILLIAMPS`, `REFRESH_HZ`.
6. Upload. The board LED blinks while joining Wi-Fi and goes dark once connected. With USB connected to a
   PC, the Serial Monitor (115200) shows the IP address and the refresh rate.
- The simpler 8-bit sketch `firmware/oal_led_udp/` is a fallback (use `UdpProtocol = adalight`).
- <!-- TODO: tip: give the board a DHCP reservation on your router. -->

## Software (on the headset)
[facts — mostly from linux/README.md; trim or link as you like]
### Requirements
- Steam Frame with SteamOS and SteamVR. Desktop Mode / Konsole access, or SSH.
- The LED board on the same Wi-Fi network.

### Install
```sh
tar -xzf oal-frame-aarch64.tar.gz
cd oal-frame
sh install.sh                    # installs DISABLED; SteamVR is not touched
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
oalctl doctor                    # preflight checks
oalctl enable                    # turns the capture layer on (on probation) and restarts SteamVR
oalctl confirm                   # once the headset looks normal; otherwise it reverts itself after 3 min
```
<!-- TODO: link to the Releases page if you publish the tarball as a GitHub Release (recommended
instead of committing dist/ to the repo). -->

### Configure
Edit `~/.config/oal-frame/oal.ini` (changes apply when you save). The minimum:
```ini
Output = udp
UdpHost = oal-led.local      ; or the board's IP
UdpProtocol = oal16          ; needs the dithering firmware
NumLights = 24
EyeRotation = 180            ; the Frame's panel image is upside down per eye
```
<!-- TODO: a short table of the settings people will actually touch: Brightness, Saturation, Smoothing,
EdgeDepth/EdgeInset, SwapLeftRight/BottomToTop*, FollowPanelBrightness, LedWhite*, Dither. Link
contrib/oal.ini for the rest. -->

### Commands
| Command | What it does |
| --- | --- |
| `oalctl status` | Capture state, view layout, live LED colours |
| `oalctl snapshot` | Writes `~/oal-snapshot.ppm` showing what each LED samples |
| `oalctl calibrate` | Test colours on the LEDs, for white balance (with `contrib/calibrate.html`) |
| `oalctl reset-edge` | Forget the learned lens edge |
| `oalctl log` | Layer log and daemon journal |
| `oalctl doctor` | Preflight checks (network, files) |
| `oalctl disable` / `enable` | Turn the capture layer off / on |

## Calibration and tuning
<!-- TODO: your own walk-through. Suggested order: -->
1. **LED order:** put something bright at the top of the view and on each side. Fix `SwapLeftRight` /
   `BottomToTopLeft` / `BottomToTopRight` until the LEDs match.
2. **Lens edge:** look around a bright game or SteamVR Home for a few seconds, then run `oalctl snapshot`.
   You should see about 70–75% coverage and white frames hugging the lens edge. The room view
   (passthrough) is ignored on purpose.
3. **Edge position:** set `EdgeInset` / `EdgeDepth`. One cell is about 0.0075 of an eye's width. Insetting
   by one cell avoids a reddish colour fringe right at the lens edge.
4. **White balance:** run `oalctl calibrate` next to `contrib/calibrate.html` on a phone or monitor and
   adjust `LedWhite*`.
5. **Brightness / saturation / smoothing** to taste. <!-- TODO: your settings as a starting point -->

## Troubleshooting
<!-- Real problems from this build, worth keeping. Add your own. -->
| Symptom | Fix |
| --- | --- |
| Strip stays solid white | Use FastLED 3.9.20, and set Pin Numbering to "By GPIO number (legacy)" |
| Compile error about `CRGB` / `memset` / `pinMode` | Wrong FastLED version or pin-numbering setting (see Firmware) |
| LEDs never react; no `/dev/ttyACM*` | Expected: the Frame has no USB-serial driver. Use Wi-Fi (`Output = udp`) |
| `oal-led.local` doesn't resolve | Use the board's IP in `UdpHost`; set a DHCP reservation |
| LEDs show the wrong side / upside down | `EyeRotation = 180`, then `SwapLeftRight` / `BottomToTop*` |
| LEDs pick up stuff from further in | Check `oalctl snapshot`; lower `EdgeInset` / `EdgeDepth` |
| LEDs dark, only light when moving your head | Edge learned too wide: update and/or `oalctl reset-edge`, relearn in a game (not the room view) |
| Purple/red fringe on top and bottom LEDs | Lens colour-fringe correction at the very edge; `EdgeInset = 0.0075` |
| Brief blackouts | Loose wiring, or an old daemon version (update) |
| `oalctl: command not found` | Add `~/.local/bin` to PATH (see Install) |
| SteamVR asks for first-time setup after restarts | <!-- TODO: in your case this was FrameColor, not this mod --> |

## Uninstalling / going back
[facts]
- `oalctl disable` turns the capture layer off; your settings are kept.
- `sh uninstall.sh` removes everything (`--purge` also removes settings and the learned edge).
- If anything goes wrong after enabling, the layer turns itself off after 3 minutes without
  `oalctl confirm`. It also does that if SteamVR fails to start.
- Firmware: re-upload `oal_led_udp` for the plain 8-bit version.
- Nothing under `/opt/steamvr` or the OS is modified; everything lives in your home directory.

## Building from source
[facts] See `linux/README.md` → "Building from source": zig cross-compile (`sh linux/build.sh`) and the
Vulkan headers. It targets glibc 2.28+, with the C++ runtime built in.
<!-- TODO: mention the host test harness if you want contributors. -->

## Repository layout
<!-- Adjust to your final structure. -->
```
linux/
  layer/        Vulkan capture layer (runs inside SteamVR's compositor)
  daemon/       LED daemon (colours, brightness, colour correction, UDP output)
  common/       Shared-memory format between the two
  contrib/      install/uninstall, oalctl, systemd units, oal.ini, calibrate.html
  firmware/     ESP32 sketches (dithering and plain 8-bit)
  third_party/  OpenVR header (BSD-3)
  test/         Host test harness
hardware/       <!-- TODO: STL/3MF/STEP files, wiring diagram -->
docs/           <!-- TODO: photos used in this README -->
```

## Credits and licenses
[facts — keep all of these]
- **OpenVR Ambient Light** by Rectus (MIT): LED geometry, colour pipeline, Adalight protocol and settings.
- **FrameColor** by beko-kerbecotton (MIT): the idea of a Vulkan layer in SteamVR's compositor, enabled
  through a SteamVR service drop-in, and the startup guard. No FrameColor code is required.
- **JChalka** ([Blended-Frame-Insertion / TemporalBFI](https://github.com/JChalka/Blended-Frame-Insertion)):
  the dithering design and the LED measurements behind the colour correction.
- **Statonwest/VR_Ambilight**: inspiration for the hardware concept. <!-- TODO: confirm you want this -->
- **OpenVR SDK** header by Valve (BSD-3-Clause), in `third_party/openvr/`.
- **FastLED** (MIT), used by the firmware.
- <!-- TODO: your license for your own code and the hardware files (e.g. MIT for code, CC BY-SA 4.0 for
  3D models). The original OAL code is MIT, so keep its copyright notice in LICENSE. -->
