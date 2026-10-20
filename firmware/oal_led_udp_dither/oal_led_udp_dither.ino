// OpenVR Ambient Light - Wi-Fi LED receiver with temporal dithering
// for Arduino Nano ESP32 (ESP32-S3)
//
// Same job as oal_led_udp, plus 16-bit colours with temporal dithering:
// the strip is refreshed REFRESH_HZ times a second and each LED channel cycles
// between two levels so that, on average, it shows the in-between brightness.
// Dim colours fade smoothly and keep their hue.
//
// How it is organised (following JChalka's TemporalBFI approach):
//   * Core 1 does nothing but refresh the LEDs, woken by a hardware timer at
//     exactly REFRESH_HZ, so the cadence is steady and never follows the rate
//     packets arrive at.
//   * Core 0 runs Wi-Fi, receives packets and turns each 16-bit colour into a
//     dither "state" per channel: a high and a low 8-bit level, a cycle length,
//     and how many refreshes of each cycle show the high level (spread evenly).
//     New states are handed to core 1 through a triple buffer.
//   * Each channel switches to a new state only at the end of its own cycle, so
//     every cycle completes and averages exactly; new packets never cut a cycle
//     short (that was the source of flicker that depended on the packet rate).
//   * States come from JChalka's measured TemporalBFI table when its header is in
//     this folder (see TEMPORAL TABLE below), otherwise from a built-in even spread.
//   * Nothing changes the values after dithering: FastLED's brightness, colour
//     correction, dithering, power limiter and refresh cap are all off. The current
//     limit is applied here, to the 16-bit colours, before dithering.
//
// Accepted packets on UDP port 21324:
//   * oal16:     'O' '1' '6' version flags cutoff countHi countLo, then R G B per LED as
//                big-endian 16-bit values. flags bit 0 = dither. Levels below `cutoff`
//                (in 16-bit units; one 8-bit step is ~257) are shown as off.
//   * Adalight:  'A' 'd' 'a' countHi countLo checksum, then R G B per LED
//   * WLED DRGB: 0x02 timeoutSeconds, then R G B per LED
//   8-bit packets get the WHITE_* balance below (FastLED's TypicalLEDStrip) and are
//   dithered too when DITHER_8BIT is 1.
//
// Arduino IDE settings:
//   Board:        Arduino Nano ESP32
//   Pin Numbering: "By GPIO number (legacy)"   (required by FastLED; D6 = GPIO9)
//   Library:      FastLED 3.9.x (3.9.20 known good)
//
// With the board plugged into a PC, the Serial Monitor (115200) prints the IP address
// and, every 5 seconds, the achieved refresh rate and any late refreshes.
//
// Put your Wi-Fi name/password in wifi_secrets.h (copy wifi_secrets.example.h, or
// copy wifi_secrets.h from the oal_led_udp folder).
//
// To go back to the previous firmware, upload oal_led_udp again and set
// UdpProtocol back to adalight in oal.ini.

// LED data rate: 1.0 = the standard 800 kHz, 1.25 = 1 MHz. Faster sending leaves headroom
// for REFRESH_HZ (24 LEDs take ~0.72 ms at 800 kHz, ~0.58 ms at 1 MHz). WS2812-type LEDs
// normally accept 1 MHz; if colours glitch or flash randomly, set this back to 1.0.
// Check the effect with the Serial Monitor's "longest show" figure.
#define FASTLED_OVERCLOCK 1.75

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <FastLED.h>

#include "wifi_secrets.h" // defines WIFI_SSID and WIFI_PASSWORD

// TEMPORAL TABLE (optional): put ONE of JChalka's TemporalBFI table headers in this sketch
// folder to use her measured dither states (~55,000-62,000 brightness steps per channel,
// accurate near black):
//   solver_precomputed_luts_shaper_stable_bfi19.h      cycles up to 20 refreshes (60 Hz at 1200)
//   solver_precomputed_luts_shaper_bfi9_interpolated.h cycles up to 10 refreshes (120 Hz at 1200)
// Dim levels are shown as one brighter refresh per cycle, so the light pulses at the cycle
// rate; if you notice flicker in your peripheral vision, try the bfi9 table or remove the
// table. Without a table, a built-in even spread is used (DITHER_CYCLE steps per 8-bit
// level, alternating neighbouring levels only). A table adds ~1.3 MB to the firmware.
#if __has_include("solver_precomputed_luts_shaper_stable_bfi19.h")
#include "solver_precomputed_luts_shaper_stable_bfi19.h"
#define HAVE_TEMPORAL_TABLE 1
#elif __has_include("solver_precomputed_luts_shaper_bfi9_interpolated.h")
#include "solver_precomputed_luts_shaper_bfi9_interpolated.h"
#define HAVE_TEMPORAL_TABLE 1
#else
#define HAVE_TEMPORAL_TABLE 0
#endif

// ---------------------------------------------------------------- settings
#define NUM_LEDS        24
#define DATA_PIN        9            // GPIO9 = D6 on the Nano ESP32
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define MAX_MILLIAMPS   500          // whole strip; colours are scaled down (before dithering) to stay under it
#define UDP_PORT        21324
#define HOSTNAME        "oal-led"    // reachable as oal-led.local where mDNS works
#define OFF_TIMEOUT_MS  2000         // LEDs off if no packet arrives for this long

// Dithering.
// REFRESH_HZ: strip refreshes per second. The temporal table's longest cycle is 20
//   refreshes, so 1200 keeps even that at 60 Hz. If the Serial Monitor reports late
//   refreshes, raise FASTLED_OVERCLOCK (above) or lower this.
// DITHER_CYCLE: cycle length of the built-in spread (used without the temporal table):
//   the slowest flicker is REFRESH_HZ / DITHER_CYCLE and the resolution 1/DITHER_CYCLE
//   of an 8-bit step.
#define REFRESH_HZ      1440
#define DITHER_CYCLE    8
// Table mode only: keep a channel's current table state until its level has moved by more
// than this fraction (1/TABLE_HOLD_DIV) or TABLE_HOLD_MIN 16-bit units, whichever is larger.
// Neighbouring table levels use very different pulse patterns, so re-picking the state on
// every tiny change (smoothing nudges colours every packet) shows as flicker. 64 = ~1.6%.
// Set TABLE_HOLD_DIV to 0 to disable.
#define TABLE_HOLD_DIV  64
#define TABLE_HOLD_MIN  8
#define DITHER_8BIT     1            // also dither Adalight/DRGB packets (after white balance)
#define CUTOFF_8BIT     128          // for 8-bit packets: below half a step is off
// White balance for 8-bit packets (oal16 packets arrive already balanced by the daemon).
// 255/176/240 is FastLED's TypicalLEDStrip, which the previous firmware used.
#define WHITE_R         255
#define WHITE_G         176
#define WHITE_B         240
// Current model for the limiter, in mA per channel at full (FastLED's WS2812 figures).
#define MA_RED          16
#define MA_GREEN        11
#define MA_BLUE         15
#define MA_IDLE         1            // per LED, even when dark

// Optional fixed IP (set USE_STATIC_IP to 1). A DHCP reservation on your router
// does the same job without changing the sketch.
#define USE_STATIC_IP   0
IPAddress staticIp(192, 168, 1, 50);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 1, 1);

// ---------------------------------------------------------------- dither states
// One state per channel: in each cycle of `len` refreshes, show `hi` on `count` of them
// (spread evenly) and `lo` on the rest.
struct State
{
  uint8_t hi, lo, len, count;
};
struct Frame
{
  State st[NUM_LEDS][3];
};

// Triple buffer. Core 0 fills a slot that is neither shown nor pending, then marks it
// pending; core 1 switches to the pending slot at its next refresh.
Frame slots[3];
volatile int frontSlot = 0;          // shown by core 1 (written only by core 1)
volatile int pendingSlot = 0;        // newest complete frame (written only by core 0)

CRGB leds[NUM_LEDS];
TaskHandle_t renderTaskHandle = nullptr;
hw_timer_t* refreshTimer = nullptr;

// Statistics (written by core 1, read and reset by core 0 for the Serial report).
volatile uint32_t statRefreshes = 0, statLate = 0, statMaxShowUs = 0, statOverruns = 0, statShowUsSum = 0;

// ---------------------------------------------------------------- core 0 state
WiFiUDP udp;
uint8_t packet[1500];
uint16_t target[NUM_LEDS][3];        // wanted light level per channel, 0..65535 (linear)
bool ditherOn = true;
uint16_t cutoff = CUTOFF_8BIT;
unsigned long lastPacketMs = 0;
unsigned long timeoutMs = OFF_TIMEOUT_MS;
bool ledsLit = false;
bool mdnsStarted = false;
uint32_t statPackets = 0, statLimited = 0;

// ---------------------------------------------------------------- core 1: render
void IRAM_ATTR onRefreshTimer()
{
  BaseType_t woke = pdFALSE;
  vTaskNotifyGiveFromISR(renderTaskHandle, &woke);
  if (woke) portYIELD_FROM_ISR();
}

void startRefreshTimer()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  refreshTimer = timerBegin(1000000);                       // 1 MHz tick
  timerAttachInterrupt(refreshTimer, &onRefreshTimer);
  timerAlarm(refreshTimer, 1000000 / REFRESH_HZ, true, 0);
#else
  refreshTimer = timerBegin(0, 80, true);                   // 80 MHz / 80 = 1 MHz tick
  timerAttachInterrupt(refreshTimer, &onRefreshTimer, true);
  timerAlarmWrite(refreshTimer, 1000000 / REFRESH_HZ, true);
  timerAlarmEnable(refreshTimer);
#endif
}

void renderTask(void*)
{
  // Per channel: the state being shown and how far through its cycle it is.
  static State active[NUM_LEDS][3];
  static uint8_t pos[NUM_LEDS][3];
  for (int i = 0; i < NUM_LEDS; i++)
    for (int c = 0; c < 3; c++)
    {
      active[i][c] = State{0, 0, 1, 0};
      pos[i][c] = 0;
    }
  startRefreshTimer(); // started from this task, so the timer interrupt runs on this core
  for (;;)
  {
    // Returns how many timer ticks arrived since last time: more than one means we were late.
    // A late refresh is counted, never replayed as a burst.
    uint32_t ticks = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (ticks > 1) statLate += ticks - 1;

    int p = __atomic_load_n(&pendingSlot, __ATOMIC_ACQUIRE);
    if (p != frontSlot) frontSlot = p;
    const Frame& f = slots[frontSlot];

    for (int i = 0; i < NUM_LEDS; i++)
    {
      uint8_t out[3];
      for (int c = 0; c < 3; c++)
      {
        // Adopt the newest state only at the start of this channel's cycle, so every
        // cycle completes and the average is exact whatever the packet rate.
        if (pos[i][c] == 0) active[i][c] = f.st[i][c];
        const State& a = active[i][c];
        const uint32_t len = a.len, k = a.count;
        // Each channel walks its cycle from a different offset, so neighbouring LEDs don't
        // all step together; the cycle still visits every position exactly once.
        const uint32_t s = (pos[i][c] + (uint32_t)(i * 7 + c * 5)) % len;
        const bool high = ((s + 1) * k) / len - (s * k) / len; // k of len, evenly spread
        out[c] = high ? a.hi : a.lo;
        pos[i][c] = (uint8_t)((pos[i][c] + 1) % len);
      }
      leds[i] = CRGB(out[0], out[1], out[2]);
    }
    uint32_t t0 = micros();
    FastLED.show();
    uint32_t dt = micros() - t0;
    if (dt > statMaxShowUs) statMaxShowUs = dt;
    if (dt > 1000000UL / REFRESH_HZ) statOverruns++; // longer than one refresh period
    statShowUsSum += dt;
    statRefreshes++;
  }
}

// ---------------------------------------------------------------- core 0: states
// 16-bit light level of one channel (c: 0 = R, 1 = G, 2 = B) -> dither state.
State makeState(uint16_t t, int c)
{
  if (t < cutoff) return State{0, 0, 1, 0};
  if (!ditherOn)
  {
    uint8_t v = (uint8_t)(((uint32_t)t * 255 + 32767) / 65535);
    return State{v, v, 1, 0};
  }
#if HAVE_TEMPORAL_TABLE
  {
    namespace L = TemporalBFIPrecomputedSolverLUTs;
    static const uint8_t storage[3] = {1, 0, 2}; // table channel order is G, R, B, W
    const uint8_t ch = storage[c];
    const uint16_t idx = L::solverNonBlackStateIndexFromQ16(t, ch);
    if (idx == L::SOLVER_BLACK_STATE_INDEX) return State{0, 0, 1, 0};
    const uint8_t upper = pgm_read_byte(&L::solverValueLUT[ch][idx]);
    const uint8_t lower = pgm_read_byte(&L::solverValueFloorLUT[ch][idx]);
    const uint8_t bfi = pgm_read_byte(&L::solverBFILUT[ch][idx]);
    // TemporalBFI "Distributed": upper once per cycle of bfi+1 refreshes, lower the rest.
    return State{upper, lower, (uint8_t)(bfi + 1), 1};
  }
#else
  {
    uint32_t level = ((uint32_t)t * 255) >> 8;                // 0..65280 = 0.0..255.0 in 1/256 steps
    uint32_t k = ((level & 0xff) * DITHER_CYCLE + 128) >> 8;
    uint8_t base = (uint8_t)(level >> 8);
    if (k >= DITHER_CYCLE) { base++; k = 0; }
    return State{(uint8_t)(base + (k ? 1 : 0)), base, DITHER_CYCLE, (uint8_t)k};
  }
#endif
}

// Applies the current limit to `target` and publishes the dither states for core 1.
void publish()
{
  // Current limit, on the 16-bit colours so the dithering stays exact.
  uint32_t sum = 0; // in mA * 65535
  for (int i = 0; i < NUM_LEDS; i++)
    sum += (uint32_t)target[i][0] * MA_RED + (uint32_t)target[i][1] * MA_GREEN + (uint32_t)target[i][2] * MA_BLUE;
  const uint32_t idle = (uint32_t)NUM_LEDS * MA_IDLE;
  const uint32_t budget = MAX_MILLIAMPS > idle ? MAX_MILLIAMPS - idle : 0;
  uint32_t scale = 65536; // 16.16
  if (sum / 65535 > budget)
  {
    scale = (uint32_t)(((uint64_t)budget * 65535 * 65536) / sum);
    statLimited++;
  }

  int front = __atomic_load_n(&frontSlot, __ATOMIC_ACQUIRE);
  int pending = pendingSlot;
  int w = 0;
  while (w == front || w == pending) w++;
  Frame& f = slots[w];
  for (int i = 0; i < NUM_LEDS; i++)
    for (int c = 0; c < 3; c++)
    {
      uint32_t t = ((uint32_t)target[i][c] * scale) >> 16;
#if HAVE_TEMPORAL_TABLE && TABLE_HOLD_DIV > 0
      // Hold the current state while the level stays close to the one it was picked for.
      static uint16_t heldQ[NUM_LEDS][3];
      static State held[NUM_LEDS][3];
      static bool haveHeld[NUM_LEDS][3];
      static bool heldDither[NUM_LEDS][3];
      const uint32_t q = heldQ[i][c];
      const uint32_t band = max((uint32_t)TABLE_HOLD_MIN, q / TABLE_HOLD_DIV);
      const bool close = haveHeld[i][c] && heldDither[i][c] == ditherOn && (t > q ? t - q : q - t) <= band &&
                         (t == 0) == (q == 0); // always react to going fully off / on
      if (!close)
      {
        held[i][c] = makeState((uint16_t)t, c);
        heldQ[i][c] = (uint16_t)t;
        haveHeld[i][c] = true;
        heldDither[i][c] = ditherOn;
      }
      f.st[i][c] = held[i][c];
#else
      f.st[i][c] = makeState((uint16_t)t, c);
#endif
    }
  __atomic_store_n(&pendingSlot, w, __ATOMIC_RELEASE);
}

void allOff()
{
  for (int i = 0; i < NUM_LEDS; i++)
    for (int c = 0; c < 3; c++) target[i][c] = 0;
  publish();
  ledsLit = false;
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
#if USE_STATIC_IP
  WiFi.config(staticIp, gateway, subnet, dns);
#endif
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false); // modem sleep adds 100 ms+ of random latency; keep the radio awake
}

// 8-bit value -> 16-bit target with this channel's white balance.
uint16_t from8(uint8_t v, uint8_t white) { return (uint16_t)(((uint32_t)v * 257 * white + 127) / 255); }

void set8(int i, const uint8_t* rgb)
{
  target[i][0] = from8(rgb[0], WHITE_R);
  target[i][1] = from8(rgb[1], WHITE_G);
  target[i][2] = from8(rgb[2], WHITE_B);
}

// oal16 packet: returns number of LEDs written, or -1 if not a valid packet.
int parseOal16(const uint8_t* p, int len)
{
  if (len < 8 || p[0] != 'O' || p[1] != '1' || p[2] != '6' || p[3] != 1) return -1;
  ditherOn = (p[4] & 1) != 0;
  cutoff = p[5];
  int count = (p[6] << 8) | p[7];
  count = min(count, NUM_LEDS);
  count = min(count, (len - 8) / 6);
  for (int i = 0; i < count; i++)
    for (int c = 0; c < 3; c++)
    {
      const uint8_t* q = p + 8 + i * 6 + c * 2;
      target[i][c] = (uint16_t)((q[0] << 8) | q[1]);
    }
  return count;
}

// Adalight packet: returns number of LEDs written, or -1 if not a valid packet.
int parseAdalight(const uint8_t* p, int len)
{
  if (len < 6 || p[0] != 'A' || p[1] != 'd' || p[2] != 'a') return -1;
  uint8_t hi = p[3], lo = p[4], chk = p[5];
  if (chk != (hi ^ lo ^ 0x55)) return -1;
  int count = ((hi << 8) | lo) + 1;
  count = min(count, NUM_LEDS);
  count = min(count, (len - 6) / 3);
  for (int i = 0; i < count; i++) set8(i, p + 6 + i * 3);
  return count;
}

// WLED DRGB packet: returns number of LEDs written, or -1 if not a valid packet.
int parseDrgb(const uint8_t* p, int len)
{
  if (len < 2 || p[0] != 2) return -1;
  // byte 1: seconds before the controller gives up (255 = never)
  timeoutMs = (p[1] == 255) ? 0 : max((unsigned long)p[1] * 1000UL, 500UL);
  int count = min(NUM_LEDS, (len - 2) / 3);
  for (int i = 0; i < count; i++) set8(i, p + 2 + i * 3);
  return count;
}

void networkStep()
{
  // Wi-Fi state: blink the board LED while (re)connecting, LED off once connected
  // (so it doesn't glow inside the headset).
  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(LED_BUILTIN, (millis() / 250) % 2);
    if (ledsLit) allOff();
    mdnsStarted = false;
    delay(10);
    return;
  }
  if (!mdnsStarted)
  {
    digitalWrite(LED_BUILTIN, LOW);
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) MDNS.addService("oal-led", "udp", UDP_PORT);
    mdnsStarted = true;
    udp.stop();
    udp.begin(UDP_PORT);
    Serial.print(HAVE_TEMPORAL_TABLE ? "OAL LED receiver (dithering, TemporalBFI table) ready: "
                                     : "OAL LED receiver (dithering, built-in spread) ready: ");
    Serial.print(WiFi.localIP());
    Serial.print(" / " HOSTNAME ".local, UDP port ");
    Serial.println(UDP_PORT);
  }

  // Drain all queued packets and keep only the newest one (lowest latency).
  bool gotFrame = false;
  int size;
  while ((size = udp.parsePacket()) > 0)
  {
    int len = udp.read(packet, sizeof(packet));
    if (len <= 0) continue;
    int n = parseOal16(packet, len);
    if (n >= 0) timeoutMs = OFF_TIMEOUT_MS;
    else
    {
      n = parseAdalight(packet, len);
      if (n >= 0) timeoutMs = OFF_TIMEOUT_MS;
      else n = parseDrgb(packet, len);
      if (n >= 0)
      {
        ditherOn = DITHER_8BIT;
        cutoff = CUTOFF_8BIT;
      }
    }
    if (n >= 0)
    {
      for (int i = n; i < NUM_LEDS; i++)
        for (int c = 0; c < 3; c++) target[i][c] = 0;
      gotFrame = true;
      statPackets++;
    }
  }
  if (gotFrame)
  {
    publish();
    lastPacketMs = millis();
    ledsLit = true;
  }
  else if (ledsLit && timeoutMs > 0 && millis() - lastPacketMs > timeoutMs)
  {
    allOff();
  }
  delay(1); // packets arrive at most every few ms; this keeps core 0 free for the Wi-Fi stack
}

void networkTask(void*)
{
  connectWiFi();
  udp.begin(UDP_PORT);
  unsigned long nextReport = millis() + 5000;
  for (;;)
  {
    networkStep();
    if ((long)(millis() - nextReport) >= 0)
    {
      nextReport += 5000;
      uint32_t r = statRefreshes, late = statLate, showUs = statMaxShowUs, over = statOverruns, sum = statShowUsSum;
      statRefreshes = 0;
      statLate = 0;
      statMaxShowUs = 0;
      statOverruns = 0;
      statShowUsSum = 0;
      Serial.printf("refresh %lu Hz (target %d), late %lu, strip update avg %lu us / longest %lu us (budget %d us, over budget %lu times), "
                    "packets %lu/s, current-limited %lu%%, %s\n",
                    (unsigned long)(r / 5), REFRESH_HZ, (unsigned long)late, (unsigned long)(r ? sum / r : 0), (unsigned long)showUs,
                    1000000 / REFRESH_HZ, (unsigned long)over, (unsigned long)(statPackets / 5),
                    (unsigned long)(statPackets ? statLimited * 100 / statPackets : 0),
                    HAVE_TEMPORAL_TABLE ? "TemporalBFI table" : "built-in spread");
      statPackets = 0;
      statLimited = 0;
    }
  }
}

// ---------------------------------------------------------------- setup
void setup()
{
  Serial.begin(115200); // only useful when plugged into a PC

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  // Pass values through untouched: dithering and the current limit happen above.
  FastLED.setBrightness(255);
  FastLED.setCorrection(UncorrectedColor);
  FastLED.setDither(DISABLE_DITHER);
  FastLED.setMaxRefreshRate(0, false); // FastLED otherwise caps WS2812 strips at 400 refreshes/s
  for (auto& fr : slots)
    for (int i = 0; i < NUM_LEDS; i++)
      for (int c = 0; c < 3; c++) fr.st[i][c] = State{0, 0, 1, 0};

  // Core 1: LED refresh only, high priority. Core 0: Wi-Fi and packets.
  xTaskCreatePinnedToCore(renderTask, "oal-render", 4096, nullptr, configMAX_PRIORITIES - 2, &renderTaskHandle, 1);
  xTaskCreatePinnedToCore(networkTask, "oal-net", 8192, nullptr, 2, nullptr, 0);
}

void loop()
{
  vTaskDelete(nullptr); // all work happens in the two tasks above
}
