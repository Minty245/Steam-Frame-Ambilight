# OpenVR Ambient Light for Steam Frame (Linux / aarch64)

This folder makes OpenVR Ambient Light (OAL) run natively on the Steam Frame, without Proton.
It has two small parts:

```
vrcompositor ──► OAL capture layer ──► /dev/shm/oal-capture ──► oal-frame-daemon ──► UDP / serial ──► ESP32 ──► LEDs
(SteamVR)        (Vulkan layer, ~30 Hz,      (tiny grid of          (OAL's own geometry and
                  2:1 GPU blits, no shaders)  colours per eye)       colour code, same settings)
```

* **Capture layer** (`layer/`). A Vulkan layer that SteamVR's compositor loads, the same way FrameColor
  loads. About 30 times a second it copies the image being shown on the panels, shrinks it to at most
  128×128 on the GPU, and publishes it. It never waits on the GPU. If anything fails, it switches itself
  off for that image and lets frames through untouched.
* **LED daemon** (`daemon/`). Reads that grid and produces LED colours with OAL's sample-area geometry
  (`UpdateSampleArea`) and colour pipeline (`CalculateOutputColor`, `mathutil.h` from the repo root).
  It sends them to the LED controller over Wi-Fi (UDP) or Adalight serial. It uses the same setting names
  as the Windows app.
* **LED firmware** (`firmware/oal_led_udp/`). An ESP32 sketch that receives the colours over Wi-Fi.

FrameColor is **not** needed and is not modified. If it is installed, both layers run together.

## Quick start

From your PC (PowerShell has `scp`), copy the release archive to the Frame:

```sh
scp linux/dist/oal-frame-aarch64.tar.gz steamos@FRAME_IP:
```

On the Frame (Konsole or SSH):

```sh
tar -xzf oal-frame-aarch64.tar.gz
cd oal-frame
sh install.sh                    # installs DISABLED; SteamVR is not touched
~/.local/bin/oalctl doctor       # preflight checks
~/.local/bin/oalctl enable       # turns the layer on (on probation) and restarts SteamVR
```

Put the headset on. **If everything looks normal**, run:

```sh
~/.local/bin/oalctl confirm
```

**If you don't run `confirm` within 3 minutes, the layer turns itself off and SteamVR restarts.** A
broken display therefore fixes itself even if you can't reach a terminal.

## Testing without LEDs

```sh
~/.local/bin/oalctl status       # what the layer captures: image size, format, which part is each eye
~/.local/bin/oalctl snapshot     # writes ~/oal-snapshot.ppm (open it in Gwenview): shows exactly what each LED samples
```

The snapshot shows the left and right eye images the daemon uses. The boxes are the LED sample areas
and the strip at the bottom shows the LED colours in strip order. Use it to tune the geometry in
`~/.config/oal-frame/oal.ini`. The daemon reloads the file when you save it.

To watch live LED colours in the terminal:

```sh
systemctl --user stop oal-frame-daemon
~/.local/bin/oal-frame-daemon --dry-run      # Ctrl+C to stop
systemctl --user start oal-frame-daemon
```

## Backout plan (from mildest to most thorough)

| Situation | What happens / what to do |
| --- | --- |
| Display broken after `oalctl enable` | Do nothing. After 3 min the layer turns off and SteamVR restarts. |
| You rebooted during those 3 minutes | The guard turns the layer off before SteamVR starts. |
| SteamVR fails within 10 s of starting with the layer on | The guard turns the layer off (logged in `~/.config/oal-frame/events.log`). |
| You want it off | `~/.local/bin/oalctl disable` (restarts SteamVR; settings kept) |
| `oalctl` itself won't run | `touch ~/.config/oal-frame/layer-disabled && systemctl --user restart steamvr.service` (the layer loads but does nothing) |
| You want OAL out of SteamVR entirely | `rm ~/.config/systemd/user/steamvr.service.d/91-oal-capture.conf && systemctl --user daemon-reload && systemctl --user restart steamvr.service` |
| Remove everything | `sh uninstall.sh` (add `--purge` to also delete `oal.ini`) |

Everything is installed in your home directory: `~/.local/share/oal-frame`, `~/.local/bin`,
`~/.local/libexec/oal-guard`, `~/.config/oal-frame`, and two files in `~/.config/systemd/user`. Nothing
under `/opt/steamvr` or the OS is changed.

## Connecting the LEDs

### Over Wi-Fi (Steam Frame: recommended)

The Frame's kernel has no USB-serial driver (`cdc_acm`), so a USB-connected controller can't receive
data. Use an ESP32 board on Wi-Fi instead; the Frame's USB port can still power it.

1. Open `firmware/oal_led_udp/oal_led_udp.ino` in the Arduino IDE. Copy `wifi_secrets.example.h` to
   `wifi_secrets.h` in the same folder and enter your Wi-Fi name and password (2.4 GHz only).
   `wifi_secrets.h` is git-ignored.
2. Check `NUM_LEDS` and `DATA_PIN` at the top of the sketch. For the Arduino Nano ESP32 set
   **Tools → Pin Numbering → "By GPIO number (legacy)"** (D6 = GPIO9) and use **FastLED 3.9.x**. Newer
   FastLED versions don't drive LEDs on the Nano ESP32's 2.0.x core.
3. Upload. The board LED blinks while it joins Wi-Fi and goes dark once connected, so it doesn't glow
   inside the headset. Opening the Serial Monitor on your PC prints its IP address.
4. In `~/.config/oal-frame/oal.ini` on the Frame set:
   ```ini
   Output = udp
   UdpHost = oal-led.local      ; or the board's IP address
   NumLights = 24               ; same as NUM_LEDS
   ```
5. Run `~/.local/bin/oalctl doctor`. It checks that the board's name resolves and that it answers ping.
   If `.local` names don't resolve on the Frame, use the IP address. A DHCP reservation on your router
   keeps it from changing.

Latency is a few milliseconds on a normal home network. The firmware keeps the Wi-Fi radio awake
(`WiFi.setSleep(false)`) because power saving would add 100 ms+ of jitter, and it shows only the newest
packet. If packets stop for 2 seconds, the LEDs switch off. The firmware also accepts WLED's DRGB
realtime packets (`UdpProtocol = drgb`), and a board running WLED works with that setting too.

### Dithering and LED colour correction (optional firmware)

`firmware/oal_led_udp_dither/` is the same receiver with two additions. Set
`UdpProtocol = oal16` to use them:

* **Temporal dithering.** The daemon sends 16-bit colours. The board refreshes the strip 1200 times a
  second, and each LED channel cycles between two levels so the average lands between them. Dim colours
  fade smoothly and keep their hue. For example, a dark grey of 3.7 / 2.5 / 3.5 steps no longer rounds to
  a greenish 4 / 3 / 3.
  * One core does nothing but refresh the LEDs, woken by a hardware timer, so the refresh rate never
    follows the rate packets arrive at. The other core handles Wi-Fi.
  * Each channel switches to a new colour only at the end of its own dither cycle, so every cycle
    completes. An earlier version switched immediately, and the flicker then depended on `SampleRate`.
  * By default each 8-bit step is split into 16 sub-steps that alternate neighbouring levels. The slowest
    flicker is therefore 1200 / 16 = 75 Hz.
  * Optionally, put one of JChalka's measured TemporalBFI tables in the sketch folder (see the comment
    at the top of the sketch). It gives 55,000-62,000 measured steps per channel. Dim levels then show
    as one brighter refresh per cycle: up to 20 refreshes (60 Hz) with the bfi19 table, or 10 (120 Hz)
    with bfi9. To use its finest low levels, set `DitherCutoff` to 0.
  * The LEDs get their data at 1 MHz instead of 800 kHz (`FASTLED_OVERCLOCK 1.25`), which leaves headroom
    at 1200 Hz. If colours glitch, set it back to 1.0.
  * The current limit (`MAX_MILLIAMPS`) is applied to the 16-bit colours before dithering. FastLED's
    brightness, correction, dithering, power limiter and 400 Hz refresh cap are all off.
  * With the board on USB, the Serial Monitor reports the refresh rate, late refreshes, and how long each
    strip update takes, every 5 seconds.
  * In `oal.ini`, `Dither = false` turns dithering off and `DitherCutoff` sets how dim counts as "off".
* **LED colour correction** in the daemon. `LedGamut` converts screen colours to the LEDs' primaries,
  using spectrophotometer readings of WS2812/SK6812-type emitters. The few screen colours the LEDs
  can't reach get a dash of neutral light instead of a hue shift. `LedWhiteRed/Green/Blue` sets the
  white balance; the defaults aim at a neutral (D65) white based on the measured emitter brightness.

To tune the white balance, run `~/.local/bin/oalctl calibrate`. Open `contrib/calibrate.html` on a phone
or monitor next to the LEDs and step through the same colours in both. Edit the `LedWhite*` values in
`oal.ini` while it runs; each save applies immediately. Two whites can measure the same and still look
different, so judge by eye. The last step is a slow fade near black for checking the dithering.

Flash the new firmware before switching to `oal16`: the older firmware ignores those packets. The new
firmware still accepts Adalight and DRGB (and dithers those too). To go back, upload `oal_led_udp` again
and set `UdpProtocol = adalight`.

### Over USB serial (other Linux machines)

Set `Output = serial` and `SerialPort` (usually `/dev/ttyACM0` or `/dev/ttyUSB0`) and flash any Adalight
sketch. If doctor reports that the port belongs to a group you aren't in, the usual fix is
`sudo usermod -aG uucp $USER` and a reboot.

## Frame-specific settings (`[Frame]` in oal.ini)

* `SampleMode`: `edge` (default) makes each LED sample a thin strip just inside the visible lens edge
  at its own height, and extends that colour outward. The edge's shape comes from the lens distortion, so
  the daemon learns it from what's shown, saves it in `~/.local/state/oal-frame/`, and **locks** it.
  `EdgeDepth` and `EdgeInset` set the strip's thickness and distance from the edge. `box` restores OAL's
  original sample boxes.
  * **Learning:** after install, or after `oalctl reset-edge`, look around a bright game or SteamVR Home
    for about 20 seconds. Only captures that look like normal lens-shaped content count: at least 30%
    and at most 75% of the image lit. Dark scenes, the room view (passthrough), fades and loading screens
    are skipped. A cell must be lit in 4 separate captures to count as inside the lens.
  * **Locked:** after about 20 seconds of good captures, the edge locks, the journal logs
    `lens edge: learned ... and locked`, and nothing changes it until `oalctl reset-edge`. Taking the
    headset off and the display waking into the room view can't widen it.
  * Before the edge is learned for a row, that LED falls back to its box.
  * A fully learned Frame lens covers about 70-75% of the image.
  * In `oalctl snapshot`, white frames mark the cells each LED samples. Grey dots mark cells counted as
    inside the lens that are dark in that frame; a band of them around the lens means it was learned too
    wide (`oalctl reset-edge`).

* `MinBrightness` (in `[Main]`, default 0 = off): a brightness floor. Edges that are dark but not black
  keep a faint glow in their own colour instead of fading out. `BlackLevel` sets what counts as black,
  which stays off.

* `ViewMode`: how the eyes are found in the presented image (`auto`, `split`, `layers`, `swapchains`).
  `oalctl status` shows what `auto` picked.
* `InputEncoding`: `auto` is normally right. Use `srgb` or `linear` if the LED colours look too dark or
  washed out compared with the headset.
* `Smoothing`: 0 (off) to 0.95. Reduces flicker at the cost of lag.
* `OAL_CAPTURE_HZ` in `~/.config/oal-frame/vulkan.env`: capture rate, default 30. Restart SteamVR after
  changing it.
* `OAL_CAPTURE_GRID` in the same file: capture detail. The panel image is halved until it fits in this
  many cells. The installer sets 270, which gives the Frame a 270×135 grid; one cell is about 0.75% of
  an eye's width, so edge sampling can stay close to the very edge. 128 gives the original 67×33 grid.
  The maximum is 320. Restart SteamVR after changing it. The learned lens edge is kept per grid size.

The layer writes a short log to `$XDG_RUNTIME_DIR/oal-frame/layer.log` (`oalctl log`). That is where
the compositor's image size, format and layer count appear.

## Building from source

The prebuilt archive is in `dist/`. To rebuild it, use any OS with [zig](https://ziglang.org) (or
`pip install ziglang`) and the Vulkan headers:

```sh
VULKAN_INCLUDE=/path/to/Vulkan-Headers/include sh linux/build.sh
```

The output targets glibc 2.28 or newer with the C++ runtime built in, so it doesn't depend on the
Frame's library versions.

## What has and hasn't been tested

Tested on an x86 Linux machine with Mesa's software Vulkan driver (lavapipe) and the Khronos validation
layer, with synchronization validation on, running *below* the capture layer:

* no validation errors over hundreds of presented frames
* correct colours end to end (red, blue and mid-grey patterns through layer → daemon → Adalight bytes)
* pass-through outside `vrcompositor`, the kill switch, `DISABLE_OAL_CAPTURE`, and LEDs turning off
  when frames stop
* the arm64 daemon (under qemu) reading data written by the layer
* install, enable, probation, guard, disable and uninstall flows with stand-in system commands

**Not yet tested on a real Steam Frame.** Unknowns that only the headset can answer:

* how the compositor presents the two eyes (one side-by-side image, array layers, or two images)
* whether its presentation surface allows the copy (if not, the log says `capture OFF` and nothing else changes)
* the exact colour encoding of the panel image

The image the layer sees is the final one sent to the panels, after lens distortion. The default OAL
geometry may need adjusting, which is what `oalctl snapshot` is for.

## Credits

* Capture mechanism (a Vulkan layer in `vrcompositor`, enabled through a SteamVR user-service drop-in)
  and the startup-guard idea come from [FrameColor](https://github.com/beko-kerbecotton/framecolor)
  (MIT). No FrameColor code is required to build or run this.
* `third_party/openvr/openvr.h` is from Valve's [OpenVR SDK](https://github.com/ValveSoftware/openvr)
  (BSD-3-Clause, see `third_party/openvr/LICENSE`). It is used only to read the panel brightness.
* The dithering design (bounded cycles, a dedicated render core on a hardware timer, nothing altering
  values after dithering) and the emitter measurements behind the colour correction come from JChalka
  and her TemporalBFI / [Blended-Frame-Insertion](https://github.com/JChalka/Blended-Frame-Insertion) work.
  No TemporalBFI code is used.
* LED geometry, colour pipeline, Adalight packet and settings come from OpenVR Ambient Light (MIT,
  this repository).
