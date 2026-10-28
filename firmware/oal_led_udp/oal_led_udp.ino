// OpenVR Ambient Light - Wi-Fi LED receiver for Arduino Nano ESP32 (ESP32-S3)
//
// Receives LED colours from oal-frame-daemon over UDP (Output = udp in oal.ini)
// and drives a WS2812B strip with FastLED.
//
// Accepts two packet formats on UDP port 21324:
//   * Adalight:  'A' 'd' 'a' countHi countLo checksum, then R G B per LED
//   * WLED DRGB: 0x02 timeoutSeconds, then R G B per LED
//
// Arduino IDE settings:
//   Board:        Arduino Nano ESP32
//   Pin Numbering: "By GPIO number (legacy)"   (required by FastLED; D6 = GPIO9)
//   Library:      FastLED 3.9.x (3.9.20 known good)
//
// Put your Wi-Fi name/password in wifi_secrets.h (copy wifi_secrets.example.h).

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <FastLED.h>

#include "wifi_secrets.h" // defines WIFI_SSID and WIFI_PASSWORD

// ---------------------------------------------------------------- settings
#define NUM_LEDS        24
#define DATA_PIN        9            // GPIO9 = D6 on the Nano ESP32
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define BRIGHTNESS      255           // hard ceiling 0..255; fine-tune live with Brightness in oal.ini
#define MAX_MILLIAMPS   500          // FastLED scales brightness down to stay under this
#define UDP_PORT        21324
#define HOSTNAME        "oal-led"    // reachable as oal-led.local where mDNS works
#define OFF_TIMEOUT_MS  2000         // LEDs off if no packet arrives for this long

// Optional fixed IP (set USE_STATIC_IP to 1). A DHCP reservation on your router
// does the same job without changing the sketch.
#define USE_STATIC_IP   0
IPAddress staticIp(192, 168, 1, 50);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 1, 1);

// ---------------------------------------------------------------- state
CRGB leds[NUM_LEDS];
WiFiUDP udp;
uint8_t packet[1500];
unsigned long lastPacketMs = 0;
unsigned long timeoutMs = OFF_TIMEOUT_MS;
bool ledsLit = false;
bool mdnsStarted = false;

void allOff()
{
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
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

// Adalight packet: returns number of LEDs written, or -1 if not a valid packet.
int parseAdalight(const uint8_t* p, int len)
{
  if (len < 6 || p[0] != 'A' || p[1] != 'd' || p[2] != 'a') return -1;
  uint8_t hi = p[3], lo = p[4], chk = p[5];
  if (chk != (hi ^ lo ^ 0x55)) return -1;
  int count = ((hi << 8) | lo) + 1;
  count = min(count, NUM_LEDS);
  count = min(count, (len - 6) / 3);
  for (int i = 0; i < count; i++)
    leds[i] = CRGB(p[6 + i * 3], p[7 + i * 3], p[8 + i * 3]);
  return count;
}

// WLED DRGB packet: returns number of LEDs written, or -1 if not a valid packet.
int parseDrgb(const uint8_t* p, int len)
{
  if (len < 2 || p[0] != 2) return -1;
  // byte 1: seconds before the controller gives up (255 = never)
  timeoutMs = (p[1] == 255) ? 0 : max((unsigned long)p[1] * 1000UL, 500UL);
  int count = min(NUM_LEDS, (len - 2) / 3);
  for (int i = 0; i < count; i++)
    leds[i] = CRGB(p[2 + i * 3], p[3 + i * 3], p[4 + i * 3]);
  return count;
}

void setup()
{
  Serial.begin(115200); // only useful when plugged into a PC: prints the IP address

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setDither(DISABLE_DITHER); // dithering flickers at low brightness with ~60 Hz updates
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  allOff();

  connectWiFi();
  udp.begin(UDP_PORT);
}

void loop()
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
    Serial.print("OAL LED receiver ready: ");
    Serial.print(WiFi.localIP());
    Serial.print(" / " HOSTNAME ".local, UDP port ");
    Serial.println(UDP_PORT);
  }

  // Drain all queued packets and show only the newest one (lowest latency).
  bool gotFrame = false;
  int size;
  while ((size = udp.parsePacket()) > 0)
  {
    int len = udp.read(packet, sizeof(packet));
    if (len <= 0) continue;
    int n = parseAdalight(packet, len);
    if (n < 0) n = parseDrgb(packet, len);
    if (n >= 0)
    {
      if (packet[0] == 'A') timeoutMs = OFF_TIMEOUT_MS;
      for (int i = n; i < NUM_LEDS; i++) leds[i] = CRGB::Black;
      gotFrame = true;
    }
  }
  if (gotFrame)
  {
    FastLED.show();
    lastPacketMs = millis();
    ledsLit = true;
  }
  else if (ledsLit && timeoutMs > 0 && millis() - lastPacketMs > timeoutMs)
  {
    allOff();
  }

  delay(1); // let the Wi-Fi stack run
}
