# Steam Frame Ambilight

LEDs inside the Steam Frame that extend the edges of the picture into
your peripheral vision, so the field of view feels wider.

<!-- TODO: hero photo or short GIF: headset on a table with the LEDs lit, plus one through-the-lens shot
if you can get it. GitHub plays .gif and .mp4 inline. -->

> **Status:** Just Works:tm: on my Steam Frame. No daily tinkering required once you get the config how you like it.

> **Tested on:** Steam Frame, Arduino Nano ESP32, 24 × WS2812B 0807 LEDs.

> **AI Disclaimer:** AI was used to develop this software, converted from the human-made OpenVR Ambient Light software to C++ and on the Vulkan runtime. I've done my best to review the code, but I don't have much experience with C++. Please open an issue immediately if you notice any vulnerabilities and I'll do my best to fix them ASAP.
Pull requests are also welcome.

Hardware setup and the diffuser are completely developed by human hands.

## Table of contents
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
- Lights up the edges of your peripheral vision with colours taken from the very edge of what's on screen.
- Runs entirely on the headset: no PC software, works standalone and with Steam Link streaming.
- Follows the headset's display brightness automatically.
- Smooth, flicker-free dim colours (16-bit colour with temporal dithering) and colour correction for the LEDs.
- Low software power consumption, no detectable increased power usage in Steam Frame performance monitor at 144Hz.

## Why I made this
Since modern VR has come about 10 years ago, I've always felt that FOV is king for immersion, no matter the resolution. But no matter what, VR seems to be stuck at the 90-110 degree mark for FOV which just isn't quite enough and I always feel like I'm looking through a tube.
We're even moving backwards in some ways, with the new Meta VR glasses having an absolutely abhorrent *70 degree fov*. What the actual heck?
I find that the Steam Frame is right at that point where I can forget about FOV, but I just needed a tiny bit more. It's worse because I have to use prescription lenses as well, and those reduce the FOV back to the point where at times I feel like I'm back inside the tube.
I decided to take inspiration from others that have done a similar mod like this and apply it to the Steam Frame and enhance it as best I can with the tools I have.
This won't replace a true extension of FOV, but I find it finally lets me completely forget about the tube and lets me feel finally immersed in VR. 

I encourage everyone to give this a try. It's a total of about $50 of materials for just a massive upgrade in immersion.

## How it works
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
- **Heat:** The LEDs are very close to your face and need to run bright enough to get through the diffuser stack. After a while you can feel the heat a tad, but in my experience it doesn't make you sweat or anything.
- **Power:** the LEDs run on 5 V straight from the Frame's USB port, through the board's VBUS pin. The
  firmware caps the strip's current (`MAX_MILLIAMPS`, 500 mA by default).
- **Brightness and flicker:** bright light close to the eyes; photosensitivity warning; how to turn it off
  quickly (`oalctl disable`, or just unplug the board).
- Not affiliated with Valve.

## Bill of materials
| Part | Qty/Size | Notes | Link |
| --- | --- | --- | --- |
| Arduino Nano ESP32 (ESP32-S3) | 1 | Any ESP32-S3 should work; this guide uses the Nano's pin names | https://www.amazon.com/dp/B0C947C9QS |
| WS2812B 0807 addressable LEDs | 24 | 12 per side. | https://www.amazon.com/dp/B0DWX8BPGY |
| Thin wire | 22awg | I recommend getting 3 strand wire, the kind that's attached in a flat line. | |
| USB-C cable / adapter | 1, 1.5ft | Powers the board from the Frame's USB port, right angle, 1.5 feet | https://www.amazon.com/dp/B0H5QJFQR1 |
| Light-reducing tape |  | Cuts brightness before diffusing | https://www.amazon.com/dp/B0CPVRS29P |
| Thin Velcro |  | Diffuser layer(s), also used to attach diffusers | https://www.amazon.com/dp/B0013AIAQ2 |
| 3D-printed parts | see below | | |
| Heat Shrink | | Any standard heat shrink works. To cover up the wires and make it look sleek. | |
| Thick Transparent Double-sided Tape | | Any brand works. As long as it's like Alien Tape. | https://www.amazon.com/dp/B09H2RXXD9/ |
| USB-C hub | | To allow you to charge the frame while still having the arduino plugged in| https://www.amazon.com/dp/B0DC51W9YC |

**Tools:**
- Pinecil soldering iron
- Flush cutters
- 3D Printer, I used a P1S
- Scissors to cut the tape
- Wire stripper
- X-Acto Knife

## 3D-printed parts
| Part | File | Material | Notes |
| --- | --- | --- | --- | --- |
| Low profile arduino case | https://www.printables.com/model/1009486-enclosure-for-arduino-nano-esp32 | PLA, black to match the Frame | |
| Diffuser | https://www.printables.com/model/1884448-steam-frame-ambilight-diffuser | Transparent PETG | |

## Hardware build
### 1. Plan the LED layout
- 24 LEDs: 12 down each side of the lenses.

### 2. Wiring
| From (Nano ESP32) | To (LED strip) | Notes |
| --- | --- | --- |
| D6 (GPIO9) | DIN of first LED | Data |
| VBUS (5 V from USB) | 5V/VDD | Current capped by `MAX_MILLIAMPS` in the firmware |
| GND | GND | |

I recommend about __ cm of wire between the arduino and the first LED strip, and then __ of wire between the first led strip and the second. Put heat shrink where you think it fits, but don't cover too much of the actual LED strip. These wires are tiny, so there's no risk of electric shock or anything. And they won't move much once actually mounted.
But I do recommend putting some heat shrink where the wire from the arduino and the first led strip connect for some strain relief. That has broken off before for me due to the many adjustments I was doing. And those LEDs don't matter quite as much as the others anyways.

### 3. Mounting inside the headset
<!-- TODO: step-by-step with photos: removing the facial interface (?), attaching mounts, gluing strips. -->
Remove the adhesive strip for the LEDs and mount them roughly level with the lens. The lower half should tape right on, but you'll want to use the top LED adhesive part to mount to the top. It's a bit janky, but I haven't thought of a better solution currently.
The wire can be threaded under the face gasket as well, and be hidden almost completely. I don't feel the extra pressure from the wire myself either. It's completely hidden!
<img width="4096" height="2304" alt="IMG_20261024_135438375" src="https://github.com/user-attachments/assets/bf596287-4800-4304-88e4-6a4203e18f93" />

While you're in here, cut a small bit of the hook part of the velcro and attach it to the top of the speaker housing area. Give it a bit of a triangle shape so you have some adjustment to work with on the diffuser.
<img width="4096" height="2304" alt="IMG_20261024_135750661" src="https://github.com/user-attachments/assets/eb37defb-da77-4230-9665-bfb57d370b14" />


### 4. Diffuser
<!-- TODO: your tape stack / printed diffuser, and how each changes brightness/heat. -->
Preparing the diffuser is a bit tricky. You'll want to put the full width of the tape and cover as much of the bottom half as possible. I've found laying the diffuser on my leg and placing the tape on top helps a lot.<img width="2304" height="4096" alt="IMG_20261024_135147109" src="https://github.com/user-attachments/assets/c2de2b86-ab6a-4900-8477-4994c7bdba32" />

<img width="2304" height="4096" alt="IMG_20261024_135156830" src="https://github.com/user-attachments/assets/d53f4d93-6765-4c2f-8868-66ee6fb797f5" />

First tape you should put on is the dimming tape, then the velcro. Use a precision cutter like an X-Acto Knife to trim the edges.
Then, put a tiny strip of the felt part of the velcro on the external fin. This is how we'll actually mount it to the headset.
Place the diffusers onto that little speaker housing area, and make sure the LEDs are covered. You'll likely have to adjust them a bit more in the future as the light can shine out from underneaht them a tad.

### 5. Mounting the board
Ideally print a housing for the Arduino, and make sure the wire is held down in some way as well. You can just slap some Alien tape on the back and put it on the side of the headset onto the face gasket section. This will let you still take out the core module without fussing with the whole setup if needed.
<img width="2304" height="4096" alt="IMG_20261024_135306968" src="https://github.com/user-attachments/assets/a6d86df0-20e1-47c6-b354-002426da1fac" />


### 6. Making it tidy
If you put on the heat shrink where it should be and your wires are sized right, you should just be able to tuck the wires under the face cushion and make the entire thing nearly invisible.
<img width="2304" height="4096" alt="IMG_20261024_135323118" src="https://github.com/user-attachments/assets/9386a0f1-75c8-461e-8678-41764c8447ed" />
<img width="4096" height="2304" alt="IMG_20261024_135351136" src="https://github.com/user-attachments/assets/92633e44-a144-41f2-90b8-2fac8ad89e7a" />
<img width="4096" height="2304" alt="IMG_20261024_135403806" src="https://github.com/user-attachments/assets/c1362633-9370-4220-8ae7-8c75380cea90" />


From here, plug in the USB cable with the right angle side into the arduino, then make a little loop so it doesn't get in the way as much, then insert it into the built in cable loop and into the frame's usb port. You can use a velcro cable tie to make it even tidier if you like.

## Firmware (ESP32)
1. Install the **Arduino IDE** and the **Arduino ESP32 boards** package (tested with core 2.0.18).
2. Install **FastLED 3.9.20**. Newer FastLED versions leave the strip stuck white on this core.
3. **Tools → Board:** Arduino Nano ESP32. **Tools → Pin Numbering:** "By GPIO number (legacy)".
4. Open `firmware/oal_led_udp_dither/oal_led_udp_dither.ino`. Copy `wifi_secrets.example.h` to
   `wifi_secrets.h` and enter your 2.4 GHz Wi-Fi details.
5. Check the settings at the top of the sketch: `NUM_LEDS`, `DATA_PIN`, `MAX_MILLIAMPS`, `REFRESH_HZ`.
6. Upload. The board LED blinks while joining Wi-Fi and goes dark once connected. With USB connected to a
   PC, the Serial Monitor (115200) shows the IP address and the refresh rate.
- The simpler 8-bit sketch `firmware/oal_led_udp/` is a fallback (use `UdpProtocol = adalight`).

## Software (on the headset)
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
The .ini that's included should be good if you used the same LEDs and LED count as I have. Details below if you have a different setup.
1. **LED order:** put something bright at the top of the view and on each side. Fix `SwapLeftRight` /
   `BottomToTopLeft` / `BottomToTopRight` until the LEDs match.
2. **Lens edge:** look around a bright game or SteamVR Home for a few seconds, then run `oalctl snapshot`.
   You should see about 70–75% coverage and white frames hugging the lens edge. The room view
   (passthrough) is ignored on purpose.
3. **Edge position:** set `EdgeInset` / `EdgeDepth`. One cell is about 0.0075 of an eye's width. Insetting
   by one cell avoids a reddish colour fringe right at the lens edge.
4. **White balance:** run `oalctl calibrate` next to `contrib/calibrate.html` on a phone or monitor and
   adjust `LedWhite*`.
5. **Brightness / saturation / smoothing** to taste.

## Troubleshooting
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

## Uninstalling / going back
- `oalctl disable` turns the capture layer off; your settings are kept.
- `sh uninstall.sh` removes everything (`--purge` also removes settings and the learned edge).
- If anything goes wrong after enabling, the layer turns itself off after 3 minutes without
  `oalctl confirm`. It also does that if SteamVR fails to start.
- Firmware: re-upload `oal_led_udp` for the plain 8-bit version.
- Nothing under `/opt/steamvr` or the OS is modified; everything lives in your home directory.

## Building from source
See `linux/README.md` → "Building from source": zig cross-compile (`sh linux/build.sh`) and the
Vulkan headers. It targets glibc 2.28+, with the C++ runtime built in.

## Repository layout
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
