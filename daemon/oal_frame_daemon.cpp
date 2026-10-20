// OAL Frame daemon: turns the coarse frames published by the OAL capture layer
// (common/oal_shm.h) into Adalight LED data, using the same sample geometry and
// colour pipeline as OpenVR Ambient Light (ambient_light_sampler.cpp).
//
// Usage:
//   oal-frame-daemon                 run (reads ~/.config/oal-frame/oal.ini)
//   oal-frame-daemon --dry-run       run without sending anything (serial/UDP), print LED colours
//   oal-frame-daemon --status        print what the capture layer is publishing, then exit
//   oal-frame-daemon --snapshot F    write a PPM picture of the sampled eye images with the
//                                    LED areas outlined, then exit
//   oal-frame-daemon --config F      use another config file
//   oal-frame-daemon --selftest      run built-in checks, then exit

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "../common/oal_shm.h"
#include "../../mathutil.h" // OpenVR Ambient Light's CIELAB helpers (repo root)
#include "../third_party/openvr/openvr.h" // only for types/interfaces; the library is loaded at runtime

#include <dlfcn.h>

namespace
{
std::atomic<bool> g_quit{false};
std::atomic<bool> g_reload{false};

uint64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void SleepNs(uint64_t ns)
{
    timespec ts = {time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
    nanosleep(&ts, nullptr);
}

// ---------------------------------------------------------------------------
// Settings: same names and defaults as OpenVR Ambient Light's [Main] and
// [AdaLight] sections, plus a few Linux/Frame specific keys.

struct Settings
{
    bool EnableLights = true;
    int SampleRate = 60;
    int NumLights = 18;

    bool SwapLeftRight = false;
    bool BottomToTopLeft = false;
    bool BottomToTopRight = true;

    float HeightFraction = 0.5f;
    float WidthFraction = 0.35f;
    float VerticalOffset = -0.02f;
    float HorizontalOffset = 0.22f;
    float VerticalAreaSize = 1.0f;
    float Curvature = 0.12f;
    float CurvatureShape = 0.12f;

    float Brightness = 1.0f;
    // Brightness floor: content that is dark but not black keeps at least this much light
    // (fraction of full, on the brightest channel, same hue) instead of fading to off.
    // 0 = off. Scales with the panel brightness like Brightness does.
    float MinBrightness = 0.0f;
    float BlackLevel = 0.004f;           // sampled light below this counts as black (no floor)
    float Contrast = 1.0f;
    float Saturation = 1.0f;

    float MinRed = 0.0f, MinGreen = 0.0f, MinBlue = 0.0f;
    float MaxRed = 1.0f, MaxGreen = 1.0f, MaxBlue = 1.0f;
    float GammaRed = 2.2f, GammaGreen = 2.2f, GammaBlue = 2.2f;

    // Output: "serial" (Adalight over USB serial) or "udp" (Wi-Fi to the LED controller)
    std::string Output = "serial";
    // Adalight over serial
    std::string SerialPort = "/dev/ttyACM0";
    int BaudRate = 115200;
    // UDP
    std::string UdpHost = "oal-led.local"; // hostname or IP of the LED controller
    int UdpPort = 21324;
    std::string UdpProtocol = "adalight";  // adalight | drgb (WLED realtime) | oal16 (16-bit, dithering firmware)
    // oal16 only: the firmware dithers 16-bit colours and these corrections run here
    bool Dither = true;                  // temporal dithering on the controller
    float DitherCutoff = 0.5f;           // levels below this fraction of one 8-bit step are shown as off
    float LedWhiteRed = 1.0f;            // white balance toward D65 (from measured emitter brightness R:G:B ~ 0.26:1:0.22)
    float LedWhiteGreen = 0.59f;
    float LedWhiteBlue = 0.34f;
    float LedGamut = 1.0f;               // 0 = off, 1 = full sRGB -> WS2812B primaries conversion

    // Frame specific
    std::string ViewMode = "auto";       // auto | split | layers | swapchains
    int LeftView = -1;                   // view index for swapchains mode (-1 = automatic)
    int RightView = -1;
    std::string InputEncoding = "auto";  // auto | linear | srgb
    float Smoothing = 0.0f;              // 0 = off, up to 0.95
    int IdleTimeoutMs = 500;             // LEDs off when no new frame for this long
    int EyeRotation = 0;                 // 0 or 180: turn each eye's image before sampling (Steam Frame: 180)

    // Where each LED samples:
    //   edge - a thin strip just inside the visible (lens) edge, at the LED's height (learned shape)
    //   box  - OAL's original boxes (HorizontalOffset / WidthFraction / Curvature)
    std::string SampleMode = "edge";
    float EdgeDepth = 0.03f;             // strip thickness, fraction of the eye's width (one grid cell is ~0.03)
    float EdgeInset = 0.02f;             // skip this much just inside the edge (soft, half-lit border)

    // Follow the headset's panel brightness (SteamVR setting steamvr/analogGain):
    // LED brightness is multiplied by analogGain / PanelBrightnessReference.
    bool FollowPanelBrightness = true;
    float PanelBrightnessReference = 1.0f;
    std::string OpenVRLibrary = "/opt/steamvr/bin/linuxarm64/libopenvr_api.so";
};

std::string Lower(std::string s)
{
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ParseBool(const std::string& v, bool def)
{
    std::string l = Lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") return true;
    if (l == "0" || l == "false" || l == "no" || l == "off") return false;
    return def;
}

std::string DefaultConfigPath()
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg) : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config";
    return base + "/oal-frame/oal.ini";
}

bool LoadSettings(const std::string& path, Settings& s, std::string* err)
{
    std::ifstream f(path);
    if (!f)
    {
        if (err) *err = "cannot open " + path + " (using defaults)";
        return false;
    }
    Settings d = Settings();
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    auto F = [&](const char* k, float def) { auto it = kv.find(Lower(k)); return it == kv.end() ? def : float(atof(it->second.c_str())); };
    auto I = [&](const char* k, int def) { auto it = kv.find(Lower(k)); return it == kv.end() ? def : atoi(it->second.c_str()); };
    auto B = [&](const char* k, bool def) { auto it = kv.find(Lower(k)); return it == kv.end() ? def : ParseBool(it->second, def); };
    auto S = [&](const char* k, const std::string& def) { auto it = kv.find(Lower(k)); return it == kv.end() ? def : it->second; };

    d.EnableLights = B("EnableLights", d.EnableLights);
    d.SampleRate = std::clamp(I("SampleRate", I("Samplerate", d.SampleRate)), 1, 240);
    d.NumLights = std::clamp(I("NumLights", d.NumLights), 2, 1024);
    d.NumLights -= d.NumLights % 2; // half per eye, like OAL
    d.SwapLeftRight = B("SwapLeftRight", d.SwapLeftRight);
    d.BottomToTopLeft = B("BottomToTopLeft", d.BottomToTopLeft);
    d.BottomToTopRight = B("BottomToTopRight", d.BottomToTopRight);
    d.HeightFraction = F("HeightFraction", d.HeightFraction);
    d.WidthFraction = F("WidthFraction", d.WidthFraction);
    d.VerticalOffset = F("VerticalOffset", d.VerticalOffset);
    d.HorizontalOffset = F("HorizontalOffset", d.HorizontalOffset);
    d.VerticalAreaSize = F("VerticalAreaSize", d.VerticalAreaSize);
    d.Curvature = F("Curvature", d.Curvature);
    d.CurvatureShape = F("CurvatureShape", d.CurvatureShape);
    d.Brightness = F("Brightness", d.Brightness);
    d.MinBrightness = std::clamp(F("MinBrightness", d.MinBrightness), 0.0f, 0.5f);
    d.BlackLevel = std::clamp(F("BlackLevel", d.BlackLevel), 0.0f, 0.1f);
    d.Contrast = F("Contrast", d.Contrast);
    d.Saturation = F("Saturation", d.Saturation);
    d.MinRed = F("MinRed", d.MinRed);
    d.MinGreen = F("MinGreen", d.MinGreen);
    d.MinBlue = F("MinBlue", d.MinBlue);
    d.MaxRed = F("MaxRed", d.MaxRed);
    d.MaxGreen = F("MaxGreen", d.MaxGreen);
    d.MaxBlue = F("MaxBlue", d.MaxBlue);
    d.GammaRed = std::max(0.05f, F("GammaRed", d.GammaRed));
    d.GammaGreen = std::max(0.05f, F("GammaGreen", d.GammaGreen));
    d.GammaBlue = std::max(0.05f, F("GammaBlue", d.GammaBlue));
    d.SerialPort = S("SerialPort", S("ComPort", d.SerialPort));
    d.BaudRate = I("BaudRate", d.BaudRate);
    d.Output = Lower(S("Output", d.Output));
    if (d.Output != "udp") d.Output = "serial";
    d.UdpHost = S("UdpHost", d.UdpHost);
    d.UdpPort = std::clamp(I("UdpPort", d.UdpPort), 1, 65535);
    d.UdpProtocol = Lower(S("UdpProtocol", d.UdpProtocol));
    if (d.UdpProtocol != "drgb" && d.UdpProtocol != "oal16") d.UdpProtocol = "adalight";
    d.Dither = B("Dither", d.Dither);
    d.DitherCutoff = std::clamp(F("DitherCutoff", d.DitherCutoff), 0.0f, 1.0f);
    d.LedWhiteRed = std::clamp(F("LedWhiteRed", d.LedWhiteRed), 0.0f, 1.0f);
    d.LedWhiteGreen = std::clamp(F("LedWhiteGreen", d.LedWhiteGreen), 0.0f, 1.0f);
    d.LedWhiteBlue = std::clamp(F("LedWhiteBlue", d.LedWhiteBlue), 0.0f, 1.0f);
    d.LedGamut = std::clamp(F("LedGamut", d.LedGamut), 0.0f, 1.0f);
    d.ViewMode = Lower(S("ViewMode", d.ViewMode));
    d.LeftView = I("LeftView", d.LeftView);
    d.RightView = I("RightView", d.RightView);
    d.InputEncoding = Lower(S("InputEncoding", d.InputEncoding));
    d.Smoothing = std::clamp(F("Smoothing", d.Smoothing), 0.0f, 0.95f);
    d.IdleTimeoutMs = std::clamp(I("IdleTimeoutMs", d.IdleTimeoutMs), 50, 60000);
    d.EyeRotation = I("EyeRotation", d.EyeRotation) == 180 ? 180 : 0;
    d.SampleMode = Lower(S("SampleMode", d.SampleMode));
    if (d.SampleMode != "box") d.SampleMode = "edge";
    d.EdgeDepth = std::clamp(F("EdgeDepth", d.EdgeDepth), 0.005f, 0.5f);
    d.EdgeInset = std::clamp(F("EdgeInset", d.EdgeInset), 0.0f, 0.3f);
    d.FollowPanelBrightness = B("FollowPanelBrightness", d.FollowPanelBrightness);
    d.PanelBrightnessReference = std::clamp(F("PanelBrightnessReference", d.PanelBrightnessReference), 0.05f, 4.0f);
    d.OpenVRLibrary = S("OpenVRLibrary", d.OpenVRLibrary);
    s = d;
    return true;
}

time_t FileMTime(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

// ---------------------------------------------------------------------------
// Geometry: a direct port of AmbientLightSampler::UpdateSampleArea. Areas are
// in per-eye normalised coordinates. The first NumLights/2 entries computed in
// the first loop are the left-eye LEDs, the rest the right-eye LEDs; `index`
// is the LED's position on the strip.

struct Area
{
    float xMin, yMin, xMax, yMax;
    bool rightEye;
};

std::vector<Area> ComputeAreas(const Settings& m)
{
    const int n = m.NumLights;
    std::vector<Area> areas(n);
    float vertFrac = m.HeightFraction / (n / 2);
    float vertRadius = m.HeightFraction * m.VerticalAreaSize / (n / 2) / 2.0f;
    float curvatureFactor = m.Curvature * 1.0f / n;
    float curvatureHalfway = m.CurvatureShape * (n / 4.0f) + (n / 4.0f) - 0.5f;

    for (int i = 0; i < n / 2; i++)
    {
        int index = m.SwapLeftRight ? (m.BottomToTopLeft ? n - 1 - i : i + n / 2)
                                    : (m.BottomToTopLeft ? n / 2 - 1 - i : i);
        float xOrigin = m.HorizontalOffset + curvatureFactor * powf(fabsf(i - curvatureHalfway), 2.0f);
        float yOrigin = vertFrac * (i + 0.5f) - m.VerticalOffset + (1.0f - m.HeightFraction) / 2.0f;
        areas[index] = {xOrigin - m.WidthFraction / 2.0f, yOrigin - vertRadius, xOrigin + m.WidthFraction / 2.0f,
                        yOrigin + vertRadius, false};
    }
    for (int i = 0; i < n / 2; i++)
    {
        int index = m.SwapLeftRight ? (m.BottomToTopRight ? n / 2 - 1 - i : i)
                                    : (m.BottomToTopRight ? n - 1 - i : i + n / 2);
        float xOrigin = 1.0f - m.HorizontalOffset - curvatureFactor * powf(fabsf(i - curvatureHalfway), 2.0f);
        float yOrigin = vertFrac * (i + 0.5f) - m.VerticalOffset + (1.0f - m.HeightFraction) / 2.0f;
        areas[index] = {xOrigin - m.WidthFraction / 2.0f, yOrigin - vertRadius, xOrigin + m.WidthFraction / 2.0f,
                        yOrigin + vertRadius, true};
    }
    return areas;
}

// ---------------------------------------------------------------------------
// Colour: a direct port of AmbientLightSampler::CalculateOutputColor, except
// that it returns 0..1 floats (so smoothing can happen before quantising) and
// clamps negative pow() inputs to 0 (OAL relied on the Windows min/max macros
// turning NaN into 0, which has the same effect).

struct RGBf
{
    double r = 0, g = 0, b = 0;
};

double Channel(double v, float minV, float brightness, float gamma, float maxV)
{
    double base = (v - minV) * brightness / (1.0 - minV);
    if (!(base > 0.0)) return 0.0;
    double o = std::pow(base, 1.0 / gamma) * maxV;
    return std::clamp(o, 0.0, 1.0);
}

RGBf CalculateOutputColor(const RGBf& in, const Settings& m)
{
    double L, a, b, red, green, blue;
    LinearRGBtoLAB_D65(in.r, in.g, in.b, L, a, b);
    L = std::min(std::max((L - 50.0) * m.Contrast + 50.0, 0.0), 100.0);
    a *= m.Saturation;
    b *= m.Saturation;
    LABtoLinearRGB_D65(L, a, b, red, green, blue);
    RGBf o;
    o.r = Channel(red, m.MinRed, m.Brightness, m.GammaRed, m.MaxRed);
    o.g = Channel(green, m.MinGreen, m.Brightness, m.GammaGreen, m.MaxGreen);
    o.b = Channel(blue, m.MinBlue, m.Brightness, m.GammaBlue, m.MaxBlue);
    return o;
}

inline uint8_t To8(uint16_t v) { return uint8_t((uint32_t(v) * 255 + 32767) / 65535); }
inline uint16_t To16(double v) { return uint16_t(lround(std::clamp(v, 0.0, 1.0) * 65535.0)); }

// LED colour correction (oal16 output only; other protocols leave it to the firmware).
// Linear sRGB -> LED primaries, then white balance. The primaries are spectrophotometer
// readings of SK6812/WS2812-type emitters (R 0.6932,0.3066  G 0.1723,0.7308
// B 0.1295,0.0650; measured by JChalka), both sets normalised to D65 so white stays
// (1,1,1) before the white balance. A few sRGB colours near pure blue fall just outside
// what the LEDs can make (they would need negative green); those get just enough neutral
// light added to fit, which keeps their hue, instead of clipping each channel separately.
// Colours above full are scaled down as a whole for the same reason.
RGBf LedCorrect(const RGBf& c, const Settings& s)
{
    static const double M[3][3] = {{0.62932, 0.32109, 0.04958}, {0.05281, 0.97662, -0.02942}, {0.01469, 0.03657, 0.94874}};
    const double k = s.LedGamut, in[3] = {c.r, c.g, c.b};
    double o[3];
    for (int r = 0; r < 3; r++)
    {
        double m = 0;
        for (int j = 0; j < 3; j++) m += ((r == j ? 1.0 - k : 0.0) + k * M[r][j]) * in[j];
        o[r] = m;
    }
    // Out of gamut (a negative channel): add the smallest equal amount to all three channels
    // that fills the gap. That is a dash of neutral light, which desaturates slightly but keeps
    // the hue and the colour's strength (pulling pure blue toward a same-brightness grey would
    // dim it by about a third to cover a 3% green shortfall).
    const double lowest = std::min({o[0], o[1], o[2]});
    if (lowest < 0)
        for (double& v : o) v -= lowest;
    o[0] = std::max(0.0, o[0]) * s.LedWhiteRed;
    o[1] = std::max(0.0, o[1]) * s.LedWhiteGreen;
    o[2] = std::max(0.0, o[2]) * s.LedWhiteBlue;
    const double peak = std::max({o[0], o[1], o[2]});
    if (peak > 1.0)
        for (double& v : o) v /= peak;
    return {o[0], o[1], o[2]};
}

double SrgbToLinear(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// ---------------------------------------------------------------------------
// Shared memory reader

struct ViewSnapshot
{
    int index = -1;
    OalViewInfo info{};
    std::vector<float> data; // gridWidth * gridHeight * 4
    uint64_t ageMs = 0;
};

struct ShmReader
{
    const OalShmHeader* hdr = nullptr;

    bool Open()
    {
        if (hdr) return true;
        int fd = shm_open(OAL_SHM_NAME, O_RDONLY, 0);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) != 0 || uint64_t(st.st_size) < kOalShmBytes)
        {
            close(fd);
            return false;
        }
        void* p = mmap(nullptr, kOalShmBytes, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return false;
        auto* h = static_cast<const OalShmHeader*>(p);
        if (memcmp(h->magic, OAL_SHM_MAGIC, 8) != 0 || h->version != OAL_SHM_VERSION)
        {
            munmap(p, kOalShmBytes);
            return false;
        }
        hdr = h;
        return true;
    }

    // Copy one view consistently (seqlock read). Returns false if never written.
    bool Read(int vi, ViewSnapshot& out, uint64_t now, bool withData)
    {
        const OalView& v = hdr->views[vi];
        // The writer holds a view "odd" for a few tens of microseconds while it
        // copies a frame in. Wait it out (up to ~5 ms) instead of giving up,
        // otherwise an unlucky read looks like "no frame".
        for (int attempt = 0; attempt < 100; attempt++)
        {
            if (attempt > 0) SleepNs(50000); // 50 us
            uint32_t s1 = v.seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            OalViewInfo copy;
            memcpy(&copy, &v.info, sizeof(copy));
            if (copy.frameCounter == 0) return false;
            if (copy.gridWidth == 0 || copy.gridHeight == 0 || copy.gridWidth > OAL_MAX_GRID_DIM ||
                copy.gridHeight > OAL_MAX_GRID_DIM)
                return false;
            if (withData)
            {
                out.data.resize(size_t(copy.gridWidth) * copy.gridHeight * 4);
                memcpy(out.data.data(), OalViewData(hdr, uint32_t(vi)), out.data.size() * sizeof(float));
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t s2 = v.seq.load(std::memory_order_relaxed);
            if (s1 != s2) continue;
            out.index = vi;
            out.info = copy;
            out.ageMs = now > copy.timestampNs ? (now - copy.timestampNs) / 1000000ull : 0;
            return true;
        }
        return false;
    }
};

// Which part of which view each eye comes from.
struct EyeSource
{
    int view = -1;
    float x0 = 0, x1 = 1; // horizontal sub-range of the view (split mode)
};

struct Selection
{
    EyeSource eye[2]; // [0] left, [1] right
    std::string mode;
    bool ok = false;
};

Selection SelectViews(const std::vector<ViewSnapshot>& fresh, const Settings& s)
{
    Selection sel;
    if (fresh.empty()) return sel;
    auto byIndex = [&](int vi) -> const ViewSnapshot* {
        for (auto& v : fresh)
            if (v.index == vi) return &v;
        return nullptr;
    };

    std::string mode = s.ViewMode;
    if (mode == "swapchains" && s.LeftView >= 0 && s.RightView >= 0)
    {
        if (byIndex(s.LeftView) && byIndex(s.RightView))
        {
            sel.eye[0].view = s.LeftView;
            sel.eye[1].view = s.RightView;
            sel.mode = "swapchains (manual)";
            sel.ok = true;
        }
        return sel;
    }

    // Primary swapchain: the biggest one that is currently presenting.
    const ViewSnapshot* primary = nullptr;
    for (auto& v : fresh)
    {
        uint64_t area = uint64_t(v.info.srcWidth) * v.info.srcHeight;
        if (!primary || area > uint64_t(primary->info.srcWidth) * primary->info.srcHeight ||
            (area == uint64_t(primary->info.srcWidth) * primary->info.srcHeight && v.index < primary->index))
            primary = &v;
    }
    const ViewSnapshot* layer1 = nullptr;
    const ViewSnapshot* twin = nullptr;
    for (auto& v : fresh)
    {
        if (&v == primary) continue;
        if (v.info.swapchainSlot == primary->info.swapchainSlot && v.info.layer == 1 && primary->info.layer == 0) layer1 = &v;
        if (v.info.swapchainSlot != primary->info.swapchainSlot && v.info.layer == 0 &&
            v.info.srcWidth == primary->info.srcWidth && v.info.srcHeight == primary->info.srcHeight && !twin)
            twin = &v;
    }

    if ((mode == "auto" || mode == "layers") && layer1)
    {
        sel.eye[0].view = primary->index;
        sel.eye[1].view = layer1->index;
        sel.mode = "layers";
    }
    else if ((mode == "auto" || mode == "swapchains") && twin)
    {
        bool primaryFirst = primary->info.swapchainSlot < twin->info.swapchainSlot;
        sel.eye[0].view = primaryFirst ? primary->index : twin->index;
        sel.eye[1].view = primaryFirst ? twin->index : primary->index;
        sel.mode = "swapchains";
    }
    else if (mode == "auto" || mode == "split")
    {
        sel.eye[0] = {primary->index, 0.0f, 0.5f};
        sel.eye[1] = {primary->index, 0.5f, 1.0f};
        sel.mode = "split";
    }
    else return sel;
    sel.ok = true;
    return sel;
}

// Average the grid cells under an eye-normalised rectangle.
RGBf SampleArea(const ViewSnapshot& v, const EyeSource& eye, const Area& a, bool linearize)
{
    const uint32_t gw = v.info.gridWidth, gh = v.info.gridHeight;
    auto clamp01 = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    float vx0 = eye.x0 + clamp01(a.xMin) * (eye.x1 - eye.x0);
    float vx1 = eye.x0 + clamp01(a.xMax) * (eye.x1 - eye.x0);
    float vy0 = clamp01(a.yMin), vy1 = clamp01(a.yMax);
    int x0 = int(std::floor(vx0 * gw)), x1 = int(std::ceil(vx1 * gw));
    int y0 = int(std::floor(vy0 * gh)), y1 = int(std::ceil(vy1 * gh));
    x0 = std::clamp(x0, 0, int(gw) - 1);
    y0 = std::clamp(y0, 0, int(gh) - 1);
    x1 = std::clamp(std::max(x1, x0 + 1), 1, int(gw));
    y1 = std::clamp(std::max(y1, y0 + 1), 1, int(gh));
    double r = 0, g = 0, b = 0;
    int count = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            const float* p = &v.data[(size_t(y) * gw + x) * 4];
            r += p[0];
            g += p[1];
            b += p[2];
            count++;
        }
    RGBf o;
    if (count)
    {
        o.r = r / count;
        o.g = g / count;
        o.b = b / count;
    }
    if (linearize)
    {
        o.r = SrgbToLinear(o.r);
        o.g = SrgbToLinear(o.g);
        o.b = SrgbToLinear(o.b);
    }
    return o;
}

bool ShouldLinearize(const ViewSnapshot& v, const Settings& s)
{
    if (s.InputEncoding == "linear") return false;
    if (s.InputEncoding == "srgb") return true;
    return !(v.info.flags & OAL_VIEW_LINEAR);
}

// ---------------------------------------------------------------------------
// Lens mask: which grid cells show what you see through the lens. Games are drawn
// only inside the lens shape, so cells outside it stay black; a cell that has been
// lit in a few separate captures is inside the lens.
//
// The mask is learned once and then LOCKED: after enough good captures (about 20 s
// of normal content) it is saved and never changes again until `oalctl reset-edge`.
// Otherwise anything drawn past the lens shape later (the room view as the display
// wakes up, fades, loading screens, passthrough edges sweeping as you turn your
// head) would slowly widen it.
//
// While learning, captures that light more than kMaxLitFraction of the image are
// skipped: a game frame cut to the lens shape lights at most ~72%, while the room
// view lights ~80% and fades/loading screens all of it. Only captures with at least
// kMinLitFraction lit count toward the learning time, so a dark scene doesn't lock
// a half-learned edge.

struct LensMask
{
    static constexpr float kThreshold = 0.004f;   // ~1/255: brighter than this counts as lit
    static constexpr uint8_t kNeeded = 4;         // captures a cell must be lit in
    static constexpr double kMaxLitFraction = 0.75;
    static constexpr double kMinLitFraction = 0.30;
    static constexpr uint32_t kLearnCaptures = 600; // good captures before locking (~20 s at 30 Hz)

    uint32_t w = 0, h = 0;
    std::string tag;                  // "-r180" when eyes are rotated, so each orientation keeps its own mask
    std::vector<uint8_t> seen;        // per cell: captures it was lit in, up to kNeeded
    uint32_t learned = 0;             // good captures learned from
    bool locked = false;
    bool justLocked = false;          // set when the mask locks; the daemon logs it and clears it
    std::map<int, uint64_t> lastCapture; // view index -> frameCounter already learned from
    uint64_t skippedFullPanel = 0;
    bool dirty = false;
    uint64_t lastSaveNs = 0;

    static std::string Dir()
    {
        const char* xdg = getenv("XDG_STATE_HOME");
        std::string base = (xdg && *xdg) ? std::string(xdg) : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.local/state";
        return base + "/oal-frame";
    }
    std::string Path() const { return Dir() + "/lensmask-" + std::to_string(w) + "x" + std::to_string(h) + tag + "-v4.bin"; }

    // In-memory mask of the given size (no file); used by Use() and the selftest.
    void Reset(uint32_t gw, uint32_t gh)
    {
        w = gw;
        h = gh;
        seen.assign(size_t(w) * h, 0);
        learned = 0;
        locked = false;
        lastCapture.clear();
    }

    void Use(uint32_t gw, uint32_t gh, const std::string& t)
    {
        if (gw == w && gh == h && t == tag) return;
        Save();
        Reset(gw, gh);
        tag = t;
        dirty = false;
        std::ifstream f(Path(), std::ios::binary);
        uint32_t hdr[3] = {};
        std::vector<uint8_t> tmp(seen.size());
        if (f && f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) && hdr[0] == 0x3453414d /* "MAS4" */ &&
            f.read(reinterpret_cast<char*>(tmp.data()), std::streamsize(tmp.size())))
        {
            locked = hdr[1] != 0;
            learned = hdr[2];
            seen = tmp;
        }
    }

    static bool Lit(const float* p) { return std::max(p[0], std::max(p[1], p[2])) > kThreshold; }

    void Learn(const ViewSnapshot& v, const std::string& t)
    {
        Use(v.info.gridWidth, v.info.gridHeight, t);
        if (locked) return;
        // Each capture counts once, however often the daemon reads it.
        auto it = lastCapture.find(v.index);
        if (it != lastCapture.end() && it->second == v.info.frameCounter) return;
        lastCapture[v.index] = v.info.frameCounter;
        size_t n = 0;
        for (size_t i = 0; i < seen.size(); i++) n += Lit(&v.data[i * 4]);
        if (double(n) > kMaxLitFraction * double(seen.size()))
        {
            skippedFullPanel++;
            return;
        }
        for (size_t i = 0; i < seen.size(); i++)
            if (seen[i] < kNeeded && Lit(&v.data[i * 4]))
            {
                seen[i]++;
                dirty = true;
            }
        if (double(n) >= kMinLitFraction * double(seen.size()) && ++learned >= kLearnCaptures)
        {
            locked = true;
            justLocked = true;
            dirty = true;
        }
    }

    double LearnProgress() const { return locked ? 1.0 : std::min(1.0, double(learned) / kLearnCaptures); }

    bool Visible(int x, int y) const { return seen[size_t(y) * w + x] >= kNeeded; }

    double Coverage() const
    {
        size_t n = 0;
        for (uint8_t c : seen) n += c >= kNeeded;
        return seen.empty() ? 0.0 : double(n) / double(seen.size());
    }

    void Save()
    {
        if (!dirty || seen.empty()) return;
        std::string dir = Dir();
        std::string parent = dir.substr(0, dir.rfind('/'));
        mkdir(parent.c_str(), 0755);
        mkdir(dir.c_str(), 0755);
        std::string tmp = Path() + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return;
        uint32_t hdr[3] = {0x3453414d, locked ? 1u : 0u, learned};
        bool ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 && fwrite(seen.data(), 1, seen.size(), f) == seen.size();
        ok = (fclose(f) == 0) && ok;
        if (ok && rename(tmp.c_str(), Path().c_str()) == 0) dirty = false;
    }

    void MaybeSave(uint64_t now)
    {
        if (dirty && (justLocked || now - lastSaveNs > 10000000000ull))
        {
            Save();
            lastSaveNs = now;
        }
    }
};

// Edge sampling: for every grid row the LED covers, find the outermost visible
// cell (left edge for the left eye, right edge for the right eye), skip EdgeInset,
// and average EdgeDepth worth of cells inward. Falls back to the box if the
// mask hasn't seen anything on those rows yet. Optionally returns the cells used.
RGBf SampleEdge(const ViewSnapshot& v, const EyeSource& eye, const Area& a, const LensMask& mask, const Settings& s,
                bool linearize, std::vector<std::pair<int, int>>* cells)
{
    const int gw = int(v.info.gridWidth), gh = int(v.info.gridHeight);
    int ex0 = std::clamp(int(std::floor(eye.x0 * gw)), 0, gw - 1);
    int ex1 = std::clamp(int(std::ceil(eye.x1 * gw)), ex0 + 1, gw);
    // In split mode the boundary column is shared by both eyes; keep each eye on its own side.
    if (eye.x0 > 0.0f && eye.x0 < 1.0f) ex0 = std::min(ex0 + 1, ex1 - 1);
    if (eye.x1 > 0.0f && eye.x1 < 1.0f) ex1 = std::max(ex1 - 1, ex0 + 1);
    const int eyeW = ex1 - ex0;
    const int depth = std::max(1, int(lround(s.EdgeDepth * eyeW)));
    const int inset = std::max(0, int(lround(s.EdgeInset * eyeW)));
    int y0 = std::clamp(int(std::floor(std::clamp(a.yMin, 0.f, 1.f) * gh)), 0, gh - 1);
    int y1 = std::clamp(int(std::ceil(std::clamp(a.yMax, 0.f, 1.f) * gh)), y0 + 1, gh);

    double r = 0, g = 0, b = 0;
    int count = 0;
    if (mask.w == uint32_t(gw) && mask.h == uint32_t(gh))
    {
        for (int y = y0; y < y1; y++)
        {
            int edge = -1;
            if (!a.rightEye)
            {
                for (int x = ex0; x < ex1 && edge < 0; x++)
                    if (mask.Visible(x, y)) edge = x;
            }
            else
            {
                for (int x = ex1 - 1; x >= ex0 && edge < 0; x--)
                    if (mask.Visible(x, y)) edge = x;
            }
            if (edge < 0) continue;
            for (int k = 0; k < depth; k++)
            {
                int x = a.rightEye ? edge - inset - k : edge + inset + k;
                if (x < ex0 || x >= ex1 || !mask.Visible(x, y)) continue;
                const float* p = &v.data[(size_t(y) * gw + x) * 4];
                r += p[0];
                g += p[1];
                b += p[2];
                count++;
                if (cells) cells->push_back({x, y});
            }
        }
    }
    if (!count) return SampleArea(v, eye, a, linearize); // mask not learned yet for these rows
    RGBf o{r / count, g / count, b / count};
    if (linearize)
    {
        o.r = SrgbToLinear(o.r);
        o.g = SrgbToLinear(o.g);
        o.b = SrgbToLinear(o.b);
    }
    return o;
}

// ---------------------------------------------------------------------------
// Adalight over a serial port (same packet as adalight_led_interface.cpp)

speed_t BaudConstant(int baud)
{
    switch (baud)
    {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
    default: return 0;
    }
}

struct Adalight
{
    int fd = -1;
    std::string port;
    int numLeds = 0;
    std::vector<uint8_t> buf;
    uint64_t nextRetryNs = 0;
    std::string lastError;

    void Close()
    {
        if (fd >= 0) close(fd);
        fd = -1;
    }

    bool Open(const std::string& p, int baud, int leds)
    {
        Close();
        port = p;
        numLeds = leds;
        buf.assign(6 + size_t(leds) * 3, 0);
        buf[0] = 'A';
        buf[1] = 'd';
        buf[2] = 'a';
        buf[3] = uint8_t((leds - 1) >> 8);
        buf[4] = uint8_t((leds - 1) & 0xff);
        buf[5] = uint8_t(buf[3] ^ buf[4] ^ 0x55);

        speed_t sp = BaudConstant(baud);
        if (!sp)
        {
            lastError = "unsupported baud rate " + std::to_string(baud);
            return false;
        }
        fd = open(p.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            lastError = "cannot open " + p + ": " + strerror(errno);
            if (errno == EACCES)
                lastError += " (is your user in the group that owns the port? see README)";
            return false;
        }
        termios t = {};
        if (tcgetattr(fd, &t) != 0)
        {
            lastError = "tcgetattr failed: " + std::string(strerror(errno));
            Close();
            return false;
        }
        cfmakeraw(&t);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
        t.c_cflag = (t.c_cflag & ~CSIZE) | CS8;
        cfsetispeed(&t, sp);
        cfsetospeed(&t, sp);
        if (tcsetattr(fd, TCSANOW, &t) != 0)
        {
            lastError = "tcsetattr failed: " + std::string(strerror(errno));
            Close();
            return false;
        }
        lastError.clear();
        return true;
    }

    bool Write(const std::vector<uint16_t>& rgb16)
    {
        if (fd < 0) return false;
        size_t n = std::min(rgb16.size(), size_t(numLeds) * 3);
        for (size_t i = 0; i < n; i++) buf[6 + i] = To8(rgb16[i]);
        if (n < size_t(numLeds) * 3) memset(&buf[6 + n], 0, size_t(numLeds) * 3 - n);
        ssize_t w = write(fd, buf.data(), buf.size());
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            lastError = "write failed: " + std::string(strerror(errno));
            Close(); // unplugged? reopen later
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// LED colours over UDP (Wi-Fi). One datagram per frame, never blocks.
//   adalight: the same bytes as the serial protocol ('A','d','a', count hi/lo, checksum, RGB...)
//   drgb:     WLED realtime "DRGB" (byte 0 = 2, byte 1 = timeout in seconds, RGB...)
//   oal16:    'O','1','6', version 1, flags (bit 0 = dither), cutoff (16-bit units),
//             count hi, count lo, then R G B per LED as big-endian 16-bit values.
//             Understood only by firmware/oal_led_udp_dither.

struct UdpSink
{
    int fd = -1;
    sockaddr_in addr{};
    bool resolved = false;
    std::string host, protocol;
    int port = 0, numLeds = 0;
    uint64_t nextResolveNs = 0, reresolveAtNs = 0;
    std::string lastError;
    std::vector<uint8_t> buf;
    bool dither = true;
    uint8_t cutoff = 128;

    void Close()
    {
        if (fd >= 0) close(fd);
        fd = -1;
        resolved = false;
    }

    void Configure(const std::string& h, int p, const std::string& proto, int leds)
    {
        Close();
        host = h;
        port = p;
        protocol = proto;
        numLeds = leds;
        nextResolveNs = 0;
    }

    // Resolve the host (may block briefly for .local names). Called only when
    // needed: at start, after errors, and every few minutes in case DHCP moved it.
    bool Resolve(uint64_t now)
    {
        if (fd < 0)
        {
            fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0)
            {
                lastError = std::string("socket failed: ") + strerror(errno);
                nextResolveNs = now + 5000000000ull;
                return false;
            }
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (rc != 0 || !res)
        {
            lastError = "cannot resolve " + host + " (" + gai_strerror(rc) +
                        "); if .local names don't work on this system, set UdpHost to the board's IP";
            resolved = false;
            nextResolveNs = now + 5000000000ull;
            return false;
        }
        addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        addr.sin_port = htons(uint16_t(port));
        freeaddrinfo(res);
        resolved = true;
        lastError.clear();
        reresolveAtNs = now + 300000000000ull; // 5 minutes
        return true;
    }

    std::string Describe() const
    {
        char ip[INET_ADDRSTRLEN] = "?";
        if (resolved) inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return host + " (" + ip + "):" + std::to_string(port) + " " + protocol;
    }

    bool Write(const std::vector<uint16_t>& rgb16, uint64_t now)
    {
        if (!resolved && now >= nextResolveNs) Resolve(now);
        else if (resolved && now >= reresolveAtNs) Resolve(now);
        if (!resolved) return false;
        size_t n = std::min(rgb16.size(), size_t(numLeds) * 3);
        if (protocol == "oal16")
        {
            buf.assign(8 + size_t(numLeds) * 6, 0);
            buf[0] = 'O';
            buf[1] = '1';
            buf[2] = '6';
            buf[3] = 1; // version
            buf[4] = dither ? 1 : 0;
            buf[5] = cutoff;
            buf[6] = uint8_t(numLeds >> 8);
            buf[7] = uint8_t(numLeds & 0xff);
            for (size_t i = 0; i < n; i++)
            {
                buf[8 + i * 2] = uint8_t(rgb16[i] >> 8);
                buf[9 + i * 2] = uint8_t(rgb16[i] & 0xff);
            }
        }
        else if (protocol == "drgb")
        {
            buf.assign(2 + size_t(numLeds) * 3, 0);
            buf[0] = 2; // DRGB
            buf[1] = 2; // controller returns to its own mode after 2 s without packets
            for (size_t i = 0; i < n; i++) buf[2 + i] = To8(rgb16[i]);
        }
        else
        {
            buf.assign(6 + size_t(numLeds) * 3, 0);
            buf[0] = 'A';
            buf[1] = 'd';
            buf[2] = 'a';
            buf[3] = uint8_t((numLeds - 1) >> 8);
            buf[4] = uint8_t((numLeds - 1) & 0xff);
            buf[5] = uint8_t(buf[3] ^ buf[4] ^ 0x55);
            for (size_t i = 0; i < n; i++) buf[6 + i] = To8(rgb16[i]);
        }
        ssize_t w = sendto(fd, buf.data(), buf.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            lastError = "send to " + Describe() + " failed: " + strerror(errno);
            resolved = false; // network changed? look the name up again shortly
            nextResolveNs = now + 2000000000ull;
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------

std::vector<ViewSnapshot> ReadFresh(ShmReader& shm, uint64_t maxAgeMs, bool withData)
{
    std::vector<ViewSnapshot> out;
    uint64_t now = NowNs();
    for (int vi = 0; vi < int(OAL_MAX_VIEWS); vi++)
    {
        ViewSnapshot v;
        if (shm.Read(vi, v, now, withData) && v.ageMs <= maxAgeMs) out.push_back(std::move(v));
    }
    return out;
}

struct Frame
{
    Selection sel;
    std::vector<RGBf> leds; // 0..1 output values
    uint64_t newestAgeMs = UINT64_MAX;
    std::vector<std::vector<std::pair<int, int>>> cells; // edge mode: grid cells each LED used (snapshot only)
};

// Brightness floor: if the sampled colour is dark but not black, raise the LED colour so
// its brightest channel is at least MinBrightness, keeping its hue. Black stays off.
RGBf ApplyFloor(const RGBf& sampled, const RGBf& out, const Settings& s)
{
    if (s.MinBrightness <= 0) return out;
    const double inMax = std::max({double(sampled.r), double(sampled.g), double(sampled.b)});
    if (inMax <= s.BlackLevel) return out; // black: let it go off
    const double m = std::max({double(out.r), double(out.g), double(out.b)});
    if (m >= s.MinBrightness) return out;
    // Hue from the output if it has any, otherwise from what was sampled (e.g. crushed by Min*/contrast).
    const RGBf& base = m > 1e-6 ? out : sampled;
    const double k = s.MinBrightness / (m > 1e-6 ? m : inMax);
    return {std::min(1.0, base.r * k), std::min(1.0, base.g * k), std::min(1.0, base.b * k)};
}

// Turns one eye's part of a view by 180 degrees in place (columns lround(x0*w)..lround(x1*w)).
// The Steam Frame's compositor output is upside down and mirrored per eye, so the
// "outer" edge would otherwise be the nose side.
void RotateEye180(ViewSnapshot& v, const EyeSource& eye)
{
    const int gw = int(v.info.gridWidth), gh = int(v.info.gridHeight);
    const int a = std::clamp(int(lround(eye.x0 * gw)), 0, gw), b = std::clamp(int(lround(eye.x1 * gw)), a, gw);
    const int n = (b - a) * gh; // cells in the rectangle
    for (int i = 0; i < n / 2; i++)
    {
        int x1 = a + i % (b - a), y1 = i / (b - a);
        int x2 = a + b - 1 - x1, y2 = gh - 1 - y1;
        float* p = &v.data[(size_t(y1) * gw + x1) * 4];
        float* q = &v.data[(size_t(y2) * gw + x2) * 4];
        for (int c = 0; c < 4; c++) std::swap(p[c], q[c]);
    }
}

bool ComputeFrame(ShmReader& shm, const Settings& s, const std::vector<Area>& areas, LensMask& mask, Frame& f,
                  std::vector<ViewSnapshot>* keep = nullptr, bool wantCells = false)
{
    auto fresh = ReadFresh(shm, uint64_t(s.IdleTimeoutMs), true);
    f.leds.assign(areas.size(), RGBf());
    f.newestAgeMs = UINT64_MAX;
    for (auto& v : fresh) f.newestAgeMs = std::min(f.newestAgeMs, v.ageMs);
    f.sel = SelectViews(fresh, s);
    if (!f.sel.ok) return false;
    const ViewSnapshot* eyeView[2] = {};
    for (int e = 0; e < 2; e++)
        for (auto& v : fresh)
            if (v.index == f.sel.eye[e].view) eyeView[e] = &v;
    if (!eyeView[0] || !eyeView[1]) return false;
    if (s.EyeRotation == 180)
    {
        RotateEye180(const_cast<ViewSnapshot&>(*eyeView[0]), f.sel.eye[0]);
        // Two eyes in one view are separate rectangles; the same rectangle must not be turned twice.
        if (eyeView[1] != eyeView[0] || f.sel.eye[1].x0 != f.sel.eye[0].x0 || f.sel.eye[1].x1 != f.sel.eye[0].x1)
            RotateEye180(const_cast<ViewSnapshot&>(*eyeView[1]), f.sel.eye[1]);
    }
    const bool edge = s.SampleMode == "edge";
    const std::string maskTag = s.EyeRotation == 180 ? "-r180" : "";
    if (edge)
    {
        mask.Learn(*eyeView[0], maskTag);
        if (eyeView[1] != eyeView[0] && eyeView[1]->info.gridWidth == eyeView[0]->info.gridWidth &&
            eyeView[1]->info.gridHeight == eyeView[0]->info.gridHeight)
            mask.Learn(*eyeView[1], maskTag);
    }
    f.cells.assign(wantCells ? areas.size() : 0, {});
    for (size_t i = 0; i < areas.size(); i++)
    {
        int e = areas[i].rightEye ? 1 : 0;
        bool lin = ShouldLinearize(*eyeView[e], s);
        RGBf in = edge ? SampleEdge(*eyeView[e], f.sel.eye[e], areas[i], mask, s, lin, wantCells ? &f.cells[i] : nullptr)
                       : SampleArea(*eyeView[e], f.sel.eye[e], areas[i], lin);
        f.leds[i] = ApplyFloor(in, CalculateOutputColor(in, s), s);
    }
    if (keep) *keep = std::move(fresh);
    return true;
}

std::string Hex(const RGBf& c)
{
    char b[8];
    snprintf(b, sizeof(b), "%02x%02x%02x", int(lround(c.r * 255)), int(lround(c.g * 255)), int(lround(c.b * 255)));
    return b;
}

void PrintLeds(const Frame& f, const std::vector<Area>& areas)
{
    std::string l = "left :", r = "right:";
    for (size_t i = 0; i < areas.size(); i++) (areas[i].rightEye ? r : l) += " " + std::to_string(i) + "=" + Hex(f.leds[i]);
    printf("[%s, newest frame %llu ms old]\n  %s\n  %s\n", f.sel.mode.c_str(), (unsigned long long)f.newestAgeMs, l.c_str(),
           r.c_str());
    fflush(stdout);
}

int CmdStatus(const Settings& s)
{
    ShmReader shm;
    if (!shm.Open())
    {
        printf("No capture data (%s missing). Is the layer enabled and SteamVR running? See: oalctl status\n",
               "/dev/shm" OAL_SHM_NAME);
        return 1;
    }
    printf("writer pid %u (%s)\n", shm.hdr->pid, kill(pid_t(shm.hdr->pid), 0) == 0 ? "running" : "not running");
    uint64_t now = NowNs();
    int shown = 0;
    for (int vi = 0; vi < int(OAL_MAX_VIEWS); vi++)
    {
        ViewSnapshot v;
        if (!shm.Read(vi, v, now, false)) continue;
        shown++;
        printf("view %d: swapchain slot %u (serial %u) layer %u/%u, source %ux%u, grid %ux%u, VkFormat %u, colorspace %u, %s, %llu captures, last %llu ms ago\n",
               vi, v.info.swapchainSlot, v.info.swapchainSerial, v.info.layer, v.info.srcLayers, v.info.srcWidth,
               v.info.srcHeight, v.info.gridWidth, v.info.gridHeight, v.info.vkFormat, v.info.vkColorSpace,
               (v.info.flags & OAL_VIEW_LINEAR) ? "linear" : "gamma-encoded", (unsigned long long)v.info.frameCounter,
               (unsigned long long)v.ageMs);
    }
    if (!shown) printf("no views captured yet\n");
    auto fresh = ReadFresh(shm, uint64_t(s.IdleTimeoutMs), false);
    Selection sel = SelectViews(fresh, s);
    if (sel.ok)
        printf("eye selection (%s): left = view %d [%.2f..%.2f], right = view %d [%.2f..%.2f]\n", sel.mode.c_str(),
               sel.eye[0].view, sel.eye[0].x0, sel.eye[0].x1, sel.eye[1].view, sel.eye[1].x0, sel.eye[1].x1);
    else printf("eye selection: none (no fresh frames within %d ms)\n", s.IdleTimeoutMs);
    if (s.SampleMode == "edge" && !fresh.empty())
    {
        LensMask m;
        m.Use(fresh[0].info.gridWidth, fresh[0].info.gridHeight, s.EyeRotation == 180 ? "-r180" : "");
        printf("sampling: edge (depth %.2f, inset %.2f); lens edge %s, covers %.0f%% of the image (%s)\n", s.EdgeDepth,
               s.EdgeInset, m.locked ? "learned and locked" : ("still learning, " + std::to_string(int(m.LearnProgress() * 100)) + "% done").c_str(),
               m.Coverage() * 100.0, m.Path().c_str());
    }
    else printf("sampling: %s\n", s.SampleMode.c_str());
    return 0;
}

int CmdSnapshot(const Settings& s, const std::string& path)
{
    ShmReader shm;
    if (!shm.Open())
    {
        fprintf(stderr, "No capture data. Is the layer enabled and SteamVR running?\n");
        return 1;
    }
    auto areas = ComputeAreas(s);
    Frame f;
    std::vector<ViewSnapshot> views;
    Settings relaxed = s;
    relaxed.IdleTimeoutMs = 5000;
    LensMask mask; // loads what the running daemon has learned; the snapshot never saves it
    if (!ComputeFrame(shm, relaxed, areas, mask, f, &views, true))
    {
        fprintf(stderr, "No fresh frames to snapshot.\n");
        return 1;
    }
    // Keep the picture a similar size whatever the capture grid (OAL_CAPTURE_GRID).
    const int scale = views[0].info.gridWidth > 160 ? 3 : 6, gap = 8;
    // Each eye is drawn as its sub-range of the grid, nearest-neighbour upscaled.
    auto eyeView = [&](int e) -> const ViewSnapshot& {
        for (auto& v : views)
            if (v.index == f.sel.eye[e].view) return v;
        return views[0];
    };
    int ew[2], eh[2], ex0[2];
    for (int e = 0; e < 2; e++)
    {
        const auto& v = eyeView(e);
        ex0[e] = int(std::floor(f.sel.eye[e].x0 * v.info.gridWidth));
        ew[e] = std::max(1, int(std::ceil(f.sel.eye[e].x1 * v.info.gridWidth)) - ex0[e]) * scale;
        eh[e] = int(v.info.gridHeight) * scale;
    }
    const int W = ew[0] + gap + ew[1];
    const int swatch = 24;
    const int H = std::max(eh[0], eh[1]) + gap + swatch;
    std::vector<uint8_t> img(size_t(W) * H * 3, 40);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t* p = &img[(size_t(y) * W + x) * 3];
        p[0] = r;
        p[1] = g;
        p[2] = b;
    };
    auto toByte = [&](float c, bool linear) {
        // Show the picture gamma-encoded so it looks normal in an image viewer.
        double v = std::clamp(double(c), 0.0, 1.0);
        if (linear) v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        return uint8_t(lround(std::clamp(v, 0.0, 1.0) * 255));
    };
    int ox[2] = {0, ew[0] + gap};
    for (int e = 0; e < 2; e++)
    {
        const auto& v = eyeView(e);
        bool linear = (v.info.flags & OAL_VIEW_LINEAR) != 0;
        for (int y = 0; y < eh[e]; y++)
            for (int x = 0; x < ew[e]; x++)
            {
                int gx = ex0[e] + x / scale, gy = y / scale;
                if (gx >= int(v.info.gridWidth)) continue;
                const float* p = &v.data[(size_t(gy) * v.info.gridWidth + gx) * 4];
                put(ox[e] + x, y, toByte(p[0], linear), toByte(p[1], linear), toByte(p[2], linear));
            }
    }
    // Cells the learned lens edge counts as visible but that are dark in this frame: dim grey
    // dot. A band of these around the lens means the edge was learned too wide.
    for (int e = 0; e < 2; e++)
    {
        const auto& v = eyeView(e);
        if (mask.w != v.info.gridWidth || mask.h != v.info.gridHeight) continue;
        for (int gy = 0; gy < int(v.info.gridHeight); gy++)
            for (int gx = ex0[e]; gx < ex0[e] + ew[e] / scale && gx < int(v.info.gridWidth); gx++)
            {
                const float* p = &v.data[(size_t(gy) * v.info.gridWidth + gx) * 4];
                if (mask.Visible(gx, gy) && std::max(p[0], std::max(p[1], p[2])) <= LensMask::kThreshold)
                    put(ox[e] + (gx - ex0[e]) * scale + scale / 2, gy * scale + scale / 2, 90, 90, 90);
            }
    }
    // What each LED samples. Edge mode: each sampled cell gets a white frame (its own colour
    // stays visible inside). Box mode, or rows whose edge isn't learned yet: the box outline.
    auto outline = [&](int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
        for (int x = x0; x <= x1; x++)
        {
            put(x, y0, r, g, b);
            put(x, y1, r, g, b);
        }
        for (int y = y0; y <= y1; y++)
        {
            put(x0, y, r, g, b);
            put(x1, y, r, g, b);
        }
    };
    for (size_t i = 0; i < areas.size(); i++)
    {
        int e = areas[i].rightEye ? 1 : 0;
        auto c = f.leds[i];
        uint8_t r = uint8_t(lround(c.r * 255)), g = uint8_t(lround(c.g * 255)), b = uint8_t(lround(c.b * 255));
        if (i < f.cells.size() && !f.cells[i].empty())
        {
            for (auto [gx, gy] : f.cells[i])
            {
                int x0 = ox[e] + (gx - ex0[e]) * scale, y0 = gy * scale;
                if (scale >= 5) outline(x0, y0, x0 + scale - 1, y0 + scale - 1, 255, 255, 255);
                else outline(x0 - 1, y0 - 1, x0 + scale, y0 + scale, 255, 255, 255); // frame around small cells
            }
            continue;
        }
        int x0 = ox[e] + int(std::clamp(areas[i].xMin, 0.f, 1.f) * ew[e]);
        int x1 = ox[e] + int(std::clamp(areas[i].xMax, 0.f, 1.f) * ew[e]) - 1;
        int y0 = int(std::clamp(areas[i].yMin, 0.f, 1.f) * eh[e]);
        int y1 = int(std::clamp(areas[i].yMax, 0.f, 1.f) * eh[e]) - 1;
        outline(x0, y0, x1, y1, 255, 255, 255);
        outline(x0 + 1, y0 + 1, x1 - 1, y1 - 1, r, g, b);
    }
    // Strip order swatches along the bottom.
    int sw = std::max(2, W / std::max(1, int(areas.size())));
    for (size_t i = 0; i < areas.size(); i++)
    {
        auto c = f.leds[i];
        for (int y = H - swatch; y < H; y++)
            for (int x = int(i) * sw; x < int(i + 1) * sw - 1; x++)
                put(x, y, uint8_t(lround(c.r * 255)), uint8_t(lround(c.g * 255)), uint8_t(lround(c.b * 255)));
    }
    FILE* out = fopen(path.c_str(), "wb");
    if (!out)
    {
        fprintf(stderr, "Cannot write %s: %s\n", path.c_str(), strerror(errno));
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    fwrite(img.data(), 1, img.size(), out);
    fclose(out);
    printf("Wrote %s (%dx%d). Left eye on the left, right eye on the right; white-framed cells/boxes are what\n"
           "each LED samples, the strip at the bottom is the LED colours in strip order.\n",
           path.c_str(), W, H);
    if (s.SampleMode == "edge")
        printf("Edge mode: lens edge %s; it covers %.0f%% of the image (a fully learned Steam Frame lens is roughly 70-75%%).\n"
               "White-framed cells: what each LED samples. Boxes instead: the edge isn't learned for those rows.\n"
               "Grey dots: cells counted as inside the lens that are dark right now; a band of them all around the\n"
               "lens means it was learned too wide. Either way: oalctl reset-edge, then look around a bright game.\n",
               mask.locked ? "learned and locked (only oalctl reset-edge changes it)"
                           : ("still learning (" + std::to_string(int(mask.LearnProgress() * 100)) + "% done: keep looking around a bright scene)").c_str(),
               mask.Coverage() * 100.0);
    PrintLeds(f, areas);
    return 0;
}

int SelfTest()
{
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) fails++;
    };
    Settings s;
    auto areas = ComputeAreas(s);
    int left = 0, right = 0;
    for (auto& a : areas) (a.rightEye ? right : left)++;
    check(left == 9 && right == 9, "18 lights split 9/9 between eyes");
    bool inLeftHalf = true, inRightHalf = true;
    for (auto& a : areas)
    {
        float cx = (a.xMin + a.xMax) / 2;
        if (!a.rightEye && cx > 0.5f) inLeftHalf = false;
        if (a.rightEye && cx < 0.5f) inRightHalf = false;
    }
    check(inLeftHalf && inRightHalf, "left-eye areas sit on the outer (left) side, right-eye areas on the right");
    RGBf white = CalculateOutputColor({1, 1, 1}, s);
    check(white.r > 0.98 && white.g > 0.98 && white.b > 0.98, "white in -> white out");
    RGBf black = CalculateOutputColor({0, 0, 0}, s);
    check(black.r < 0.01 && black.g < 0.01 && black.b < 0.01, "black in -> black out");
    RGBf red = CalculateOutputColor({1, 0, 0}, s);
    check(red.r > 0.9 && red.g < 0.2 && red.b < 0.2, "red in -> red out");
    RGBf mid = CalculateOutputColor({0.2140, 0.2140, 0.2140}, s); // linear 0.214 ~ sRGB 0.5
    check(std::fabs(mid.r - std::pow(0.214, 1 / 2.2)) < 0.02, "grey follows OAL's 1/2.2 output gamma");
    check(std::fabs(SrgbToLinear(0.5) - 0.214) < 0.001, "sRGB decode");

    // Edge mode on a synthetic lens image: two eyes side by side, black outside a
    // circle, a 3-cell green rim just inside each circle's edge, a red (left) /
    // blue (right) interior and a white "menu" in the middle of each eye.
    {
        ViewSnapshot v;
        v.index = 0;
        v.info.gridWidth = 66;
        v.info.gridHeight = 32;
        v.info.flags = OAL_VIEW_LINEAR;
        v.data.assign(66 * 32 * 4, 0.0f);
        for (int e = 0; e < 2; e++)
            for (int y = 0; y < 32; y++)
            {
                double cx = e * 33 + 16.0, cy = 15.5, rad = 15.0;
                int first = -1, last = -1;
                for (int x = e * 33; x < e * 33 + 33; x++)
                    if (std::hypot(x + 0.5 - cx, y + 0.5 - cy) <= rad)
                    {
                        if (first < 0) first = x;
                        last = x;
                    }
                for (int x = first; first >= 0 && x <= last; x++)
                {
                    float* p = &v.data[(size_t(y) * 66 + x) * 4];
                    bool rim = (x - first < 3) || (last - x < 3);
                    bool menu = std::fabs(x + 0.5 - cx) < 6 && std::fabs(y + 0.5 - cy) < 6;
                    if (rim) p[1] = 1.0f;
                    else if (menu) p[0] = p[1] = p[2] = 1.0f;
                    else if (e == 0) p[0] = 1.0f;
                    else p[2] = 1.0f;
                }
            }
        LensMask m;
        m.Reset(66, 32); // in-memory only (no file)
        size_t litCells = 0;
        for (size_t i = 0; i < size_t(66 * 32); i++) litCells += LensMask::Lit(&v.data[i * 4]);
        for (int k = 0; k < 6; k++)
        {
            v.info.frameCounter = uint64_t(k + 1); // separate captures
            m.Learn(v, "");
        }
        // Room view (passthrough): lights past the lens shape, ~80% of the image, with edges that
        // sweep around as the head turns. It must not widen the learned edge.
        {
            double before = m.Coverage();
            uint64_t skipped = m.skippedFullPanel;
            ViewSnapshot room = v;
            for (int k = 0; k < 20; k++)
            {
                // light the outside except a ragged corner region that moves each capture
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 66; x++)
                    {
                        float* p = &room.data[(size_t(y) * 66 + x) * 4];
                        const float* q = &v.data[(size_t(y) * 66 + x) * 4];
                        bool corner = (x + y + k) % 66 < 8 || (65 - x + y + k) % 66 < 8;
                        if (LensMask::Lit(q)) { p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; }
                        else p[0] = p[1] = p[2] = corner ? 0.0f : 0.3f;
                    }
                room.info.frameCounter = uint64_t(100 + k);
                m.Learn(room, "");
            }
            ViewSnapshot full = v;
            std::fill(full.data.begin(), full.data.end(), 0.5f);
            for (int k = 0; k < 10; k++)
            {
                full.info.frameCounter = uint64_t(200 + k);
                m.Learn(full, "");
            }
            check(m.Coverage() == before && m.skippedFullPanel == skipped + 30,
                  "room view (passthrough) and full-panel frames are not learned as lens area");
            printf("     (test lens lights %.0f%% of the image; mask %.2f -> %.2f, skipped %llu)\n", 100.0 * double(litCells) / (66 * 32),
                   before, m.Coverage(), (unsigned long long)(m.skippedFullPanel - skipped));
        }
        // Learn-once: dark captures don't count toward learning; enough good ones lock the edge,
        // and nothing afterwards changes it (e.g. a dim room view on waking, lit < 75%).
        {
            LensMask L;
            L.Reset(66, 32);
            ViewSnapshot dark = v;
            std::fill(dark.data.begin(), dark.data.end(), 0.0f);
            for (int y = 0; y < 4; y++) dark.data[(size_t(16) * 66 + 33 + y) * 4] = 1.0f; // a few lit cells only
            for (uint32_t k = 0; k < LensMask::kLearnCaptures; k++)
            {
                dark.info.frameCounter = 10000 + k;
                L.Learn(dark, "");
            }
            bool darkDidntLock = !L.locked;
            for (uint32_t k = 0; k < LensMask::kLearnCaptures; k++)
            {
                v.info.frameCounter = 20000 + k;
                L.Learn(v, "");
            }
            double lockedCov = L.Coverage();
            ViewSnapshot wake = v; // dim room view: lights the lens plus a band outside it, ~70% total
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 66; x++)
                {
                    float* p = &wake.data[(size_t(y) * 66 + x) * 4];
                    if (!LensMask::Lit(p) && (x < 4 || x > 61)) p[0] = p[1] = p[2] = 0.2f;
                }
            for (int k = 0; k < 50; k++)
            {
                wake.info.frameCounter = 30000 + k;
                L.Learn(wake, "");
            }
            check(darkDidntLock && L.locked && L.Coverage() == lockedCov,
                  "lens edge locks after learning; dark frames don't count and later frames can't widen it");
        }
        Settings es; // defaults: edge mode, 18 lights
        auto ea = ComputeAreas(es);
        EyeSource eyes[2] = {{0, 0.0f, 0.5f}, {0, 0.5f, 1.0f}};
        int green = 0, total = 0;
        double boxGreen = 0;
        for (auto& a : ea)
        {
            RGBf c = SampleEdge(v, eyes[a.rightEye ? 1 : 0], a, m, es, false, nullptr);
            total++;
            if (c.g > 0.95 && c.r < 0.05 && c.b < 0.05) green++;
            boxGreen += SampleArea(v, eyes[a.rightEye ? 1 : 0], a, false).g;
        }
        check(green == total, "edge mode: every LED samples only the rim just inside the lens edge");
        check(boxGreen / total < 0.6, "box mode (for comparison) mixes in interior/menu colours");
        check(m.Coverage() > 0.6 && m.Coverage() < 0.8, "lens mask learned the circular visible area");
    }
    {
        // EyeRotation = 180 turns each eye of a split view in place: a cell at an eye's
        // top-left ends up at that eye's bottom-right, and the eyes stay on their own side.
        ViewSnapshot v;
        v.info.gridWidth = 6;
        v.info.gridHeight = 2;
        v.data.assign(6 * 2 * 4, 0.0f);
        v.data[(0 * 6 + 0) * 4 + 0] = 1.0f; // left eye top-left: red
        v.data[(0 * 6 + 3) * 4 + 2] = 1.0f; // right eye top-left: blue
        RotateEye180(v, EyeSource{0, 0.0f, 0.5f});
        RotateEye180(v, EyeSource{0, 0.5f, 1.0f});
        bool ok = v.data[(1 * 6 + 2) * 4 + 0] == 1.0f && v.data[(1 * 6 + 5) * 4 + 2] == 1.0f;
        float sum = 0;
        for (float x : v.data) sum += x;
        check(ok && sum == 2.0f, "EyeRotation 180 turns each eye in place");
    }
    {
        Settings cs;
        cs.LedGamut = 0;
        cs.LedWhiteRed = cs.LedWhiteGreen = cs.LedWhiteBlue = 1;
        RGBf c = LedCorrect({0.2f, 0.5f, 0.9f}, cs);
        check(std::fabs(c.r - 0.2) < 1e-6 && std::fabs(c.g - 0.5) < 1e-6 && std::fabs(c.b - 0.9) < 1e-6,
              "LED correction off (LedGamut 0, white 1) leaves colours unchanged");
        cs.LedGamut = 1;
        c = LedCorrect({0.5f, 0.5f, 0.5f}, cs);
        RGBf g = LedCorrect({0.0f, 1.0f, 0.0f}, cs);
        check(std::fabs(c.r - 0.5) < 1e-3 && std::fabs(c.g - 0.5) < 1e-3 && std::fabs(c.b - 0.5) < 1e-3 && g.r > 0.3 && g.g > 0.9,
              "gamut conversion keeps greys grey and adds red to sRGB green");
        RGBf b = LedCorrect({0.0f, 0.0f, 1.0f}, cs);
        // pure blue is just out of gamut: no negative light, blue kept strong, the gap filled with a dash of neutral
        check(b.r >= 0 && b.g >= 0 && b.b > 0.9 && b.g < 0.05 && std::fabs(b.r - (0.04958 + 0.02942)) < 1e-3,
              "out-of-gamut blue gets a dash of neutral, not per-channel clipping");
        RGBf y = LedCorrect({1.0f, 1.0f, 0.0f}, cs);
        check(std::max({y.r, y.g, y.b}) <= 1.0 + 1e-9 && std::fabs(y.r / y.g - (0.62932 + 0.32109) / (0.05281 + 0.97662)) < 1e-3,
              "colours above full are scaled down as a whole (hue kept)");
        cs.LedWhiteGreen = 0.69f;
        c = LedCorrect({1, 1, 1}, cs);
        check(std::fabs(c.g - 0.69) < 1e-3 && std::fabs(c.r - 1.0) < 1e-3, "white balance scales white per channel");
        check(To8(0) == 0 && To8(65535) == 255 && To8(To16(0.5)) == 128, "16-bit to 8-bit rounding");
    }
    {
        Settings fs;
        fs.MinBrightness = 0.02f;
        RGBf dim = ApplyFloor({0.006f, 0.0f, 0.012f}, {0.003f, 0.0f, 0.006f}, fs);
        RGBf black = ApplyFloor({0.001f, 0.0f, 0.002f}, {0.0005f, 0.0f, 0.001f}, fs);
        RGBf bright = ApplyFloor({0.5f, 0.2f, 0.1f}, {0.5f, 0.2f, 0.1f}, fs);
        check(std::fabs(dim.b - 0.02) < 1e-6 && std::fabs(dim.r - 0.01) < 1e-6 && black.b < 0.002 && bright.r == 0.5f,
              "brightness floor lifts dim colours (same hue), leaves black off and bright colours alone");
    }
    printf("%s\n", fails ? "SELFTEST FAILED" : "selftest passed");
    return fails ? 1 : 0;
}

void OnSignal(int sig)
{
    if (sig == SIGHUP) g_reload = true;
    else g_quit = true;
}

// ---------------------------------------------------------------------------
// Panel brightness from SteamVR. Connects as a background app (never starts
// SteamVR), reads steamvr/analogGain a few times a second, and lets go when
// SteamVR quits. libopenvr_api.so is loaded at runtime so the daemon still
// works (at fixed brightness) without it.

struct PanelBrightness
{
    using InitFn = uint32_t (*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    using GetInterfaceFn = void* (*)(const char*, vr::EVRInitError*);
    using ShutdownFn = void (*)();

    void* lib = nullptr;
    InitFn init = nullptr;
    GetInterfaceFn getInterface = nullptr;
    ShutdownFn shutdown = nullptr;
    vr::IVRSettings* settings = nullptr;
    vr::IVRSystem* system = nullptr;
    bool connected = false;
    uint64_t nextTryNs = 0, nextReadNs = 0;
    float gain = 1.0f;
    bool known = false;
    int readFailures = 0;
    std::string lastError;

    bool Load(const std::string& path)
    {
        lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib)
        {
            const char* e = dlerror();
            lastError = "cannot load " + path + (e ? std::string(": ") + e : std::string());
            return false;
        }
        init = reinterpret_cast<InitFn>(dlsym(lib, "VR_InitInternal2"));
        getInterface = reinterpret_cast<GetInterfaceFn>(dlsym(lib, "VR_GetGenericInterface"));
        shutdown = reinterpret_cast<ShutdownFn>(dlsym(lib, "VR_ShutdownInternal"));
        if (!init || !getInterface || !shutdown)
        {
            lastError = path + " is missing OpenVR entry points";
            dlclose(lib);
            lib = nullptr;
            return false;
        }
        return true;
    }

    void Disconnect()
    {
        if (connected && shutdown) shutdown();
        connected = false;
        settings = nullptr;
        system = nullptr;
    }

    void Connect(uint64_t now)
    {
        vr::EVRInitError err = vr::VRInitError_None;
        init(&err, vr::VRApplication_Background, nullptr);
        if (err != vr::VRInitError_None)
        {
            lastError = "SteamVR not reachable (init error " + std::to_string(int(err)) + ")";
            nextTryNs = now + 5000000000ull;
            return;
        }
        connected = true;
        settings = static_cast<vr::IVRSettings*>(getInterface(vr::IVRSettings_Version, &err));
        if (!settings || err != vr::VRInitError_None)
        {
            lastError = "SteamVR has no " + std::string(vr::IVRSettings_Version);
            Disconnect();
            nextTryNs = now + 30000000000ull;
            return;
        }
        err = vr::VRInitError_None;
        system = static_cast<vr::IVRSystem*>(getInterface(vr::IVRSystem_Version, &err));
        if (err != vr::VRInitError_None) system = nullptr; // optional: only used for the quit event
        lastError.clear();
    }

    // Updates `gain` at most 4 times a second. Returns the factor to apply to
    // LED brightness (1.0 when unknown or disabled).
    float Factor(const Settings& s, uint64_t now)
    {
        if (!s.FollowPanelBrightness)
        {
            if (connected) Disconnect();
            return 1.0f;
        }
        if (!lib)
        {
            if (now < nextTryNs) return 1.0f;
            if (!Load(s.OpenVRLibrary))
            {
                nextTryNs = now + 30000000000ull;
                return 1.0f;
            }
        }
        if (!connected)
        {
            if (now < nextTryNs) return known ? gain / s.PanelBrightnessReference : 1.0f;
            Connect(now);
            if (!connected) return known ? gain / s.PanelBrightnessReference : 1.0f;
        }
        if (system)
        {
            vr::VREvent_t ev;
            while (system->PollNextEvent(&ev, sizeof(ev)))
            {
                if (ev.eventType == vr::VREvent_Quit)
                {
                    system->AcknowledgeQuit_Exiting();
                    Disconnect();
                    nextTryNs = now + 5000000000ull;
                    return known ? gain / s.PanelBrightnessReference : 1.0f;
                }
            }
        }
        if (now >= nextReadNs)
        {
            nextReadNs = now + 250000000ull;
            vr::EVRSettingsError se = vr::VRSettingsError_None;
            float g = settings->GetFloat(vr::k_pch_SteamVR_Section, "analogGain", &se);
            if (se == vr::VRSettingsError_None && std::isfinite(g))
            {
                gain = std::clamp(g, 0.0f, 4.0f);
                known = true;
            }
            else if (se == vr::VRSettingsError_UnsetSettingHasNoDefault)
            {
                gain = 1.0f; // never changed from the default
                known = true;
            }
            // Repeated failures (e.g. IPC errors after SteamVR died without a quit event): reconnect later.
            readFailures = (se == vr::VRSettingsError_None || se == vr::VRSettingsError_UnsetSettingHasNoDefault) ? 0 : readFailures + 1;
            if (readFailures >= 4)
            {
                lastError = "lost SteamVR settings (error " + std::to_string(int(se)) + ")";
                readFailures = 0;
                Disconnect();
                nextTryNs = now + 5000000000ull;
            }
        }
        return known ? std::clamp(gain / s.PanelBrightnessReference, 0.0f, 4.0f) : 1.0f;
    }
};

int Run(const std::string& configPath, bool dryRun)
{
    Settings s;
    std::string err;
    if (!LoadSettings(configPath, s, &err)) fprintf(stderr, "%s\n", err.c_str());
    time_t cfgTime = FileMTime(configPath);
    auto areas = ComputeAreas(s);

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGHUP, OnSignal);

    ShmReader shm;
    LensMask mask;
    PanelBrightness panel;
    float lastFactorShown = -1.0f;
    Adalight ada;
    UdpSink udp;
    udp.Configure(s.UdpHost, s.UdpPort, s.UdpProtocol, s.NumLights);
    auto outputName = [&]() {
        return s.Output == "udp" ? "udp " + s.UdpHost + ":" + std::to_string(s.UdpPort) + " (" + s.UdpProtocol + ")"
                                 : "serial " + s.SerialPort + " @ " + std::to_string(s.BaudRate);
    };
    std::vector<RGBf> smooth(areas.size());
    std::vector<uint16_t> rgb(areas.size() * 3); // 16-bit; 8-bit protocols round it
    bool lightsOn = false;
    uint64_t lastGoodNs = 0; // last time a frame was computed
    uint64_t nextPrint = 0, nextCfgCheck = 0, nextOffRefresh = 0, nextSkipLog = 0, skipShown = 0;
    std::string lastStatus;
    auto status = [&](const std::string& m) {
        if (m != lastStatus)
        {
            fprintf(stderr, "%s\n", m.c_str());
            lastStatus = m;
        }
    };
    std::string lastNetStatus; // output messages are de-duplicated separately from capture messages
    auto netStatus = [&](const std::string& m) {
        if (m != lastNetStatus)
        {
            fprintf(stderr, "%s\n", m.c_str());
            lastNetStatus = m;
        }
    };

    printf("oal-frame-daemon: %d lights, %d Hz, %s, config %s\n", s.NumLights, s.SampleRate,
           dryRun ? "dry run (no output)" : outputName().c_str(),
           configPath.c_str());
    fflush(stdout);

    while (!g_quit)
    {
        uint64_t frameStart = NowNs();

        if (frameStart >= nextCfgCheck || g_reload)
        {
            nextCfgCheck = frameStart + 1000000000ull;
            time_t t = FileMTime(configPath);
            if (t != cfgTime || g_reload)
            {
                g_reload = false;
                cfgTime = t;
                Settings ns;
                if (LoadSettings(configPath, ns, &err))
                {
                    bool reopen = ns.SerialPort != s.SerialPort || ns.BaudRate != s.BaudRate || ns.NumLights != s.NumLights ||
                                  ns.Output != s.Output || ns.UdpHost != s.UdpHost || ns.UdpPort != s.UdpPort ||
                                  ns.UdpProtocol != s.UdpProtocol;
                    s = ns;
                    areas = ComputeAreas(s);
                    smooth.assign(areas.size(), RGBf());
                    rgb.assign(areas.size() * 3, 0);
                    if (reopen)
                    {
                        ada.Close();
                        udp.Configure(s.UdpHost, s.UdpPort, s.UdpProtocol, s.NumLights);
                    }
                    printf("config reloaded\n");
                    fflush(stdout);
                }
            }
        }

        if (!dryRun && s.Output == "serial" && ada.fd < 0 && frameStart >= ada.nextRetryNs)
        {
            if (ada.Open(s.SerialPort, s.BaudRate, s.NumLights)) status("serial: opened " + s.SerialPort);
            else
            {
                status("serial: " + ada.lastError + " (retrying every 2 s)");
                ada.nextRetryNs = frameStart + 2000000000ull;
            }
        }

        auto send = [&](const std::vector<uint16_t>& data) {
            if (dryRun) return;
            if (s.Output == "udp")
            {
                udp.dither = s.Dither;
                udp.cutoff = uint8_t(lround(s.DitherCutoff * 255.0)); // one 8-bit step is ~257 16-bit units

                if (udp.Write(data, NowNs())) netStatus("udp: sending to " + udp.Describe());
                else if (!udp.lastError.empty()) netStatus("udp: " + udp.lastError);
            }
            else ada.Write(data);
        };

        bool haveFrame = false;
        Frame f;
        if (shm.Open())
        {
            // Scale LED brightness with the headset's panel brightness.
            float factor = panel.Factor(s, frameStart);
            if (s.FollowPanelBrightness && std::fabs(factor - lastFactorShown) > 0.005f)
            {
                char msg[160];
                if (panel.known)
                    snprintf(msg, sizeof(msg), "panel brightness %.2f -> LED brightness x%.2f", panel.gain, factor);
                else
                    snprintf(msg, sizeof(msg), "panel brightness unknown (%s); LEDs at configured brightness", panel.lastError.c_str());
                fprintf(stderr, "%s\n", msg);
                lastFactorShown = factor;
            }
            Settings eff = s;
            eff.Brightness = s.Brightness * factor;
            eff.MinBrightness = s.MinBrightness * factor;
            haveFrame = s.EnableLights && ComputeFrame(shm, eff, areas, mask, f);
        }
        else
            status("waiting for the capture layer (no /dev/shm" OAL_SHM_NAME ")");
        if (mask.justLocked)
        {
            fprintf(stderr, "lens edge: learned (covers %.0f%% of the image) and locked; oalctl reset-edge relearns it\n",
                    mask.Coverage() * 100.0);
        }
        mask.MaybeSave(frameStart);
        mask.justLocked = false;
        if (mask.skippedFullPanel != skipShown && frameStart >= nextSkipLog)
        {
            fprintf(stderr, "lens edge: ignored %llu full-panel captures so far (passthrough, fades or loading screens)\n",
                    (unsigned long long)mask.skippedFullPanel);
            skipShown = mask.skippedFullPanel;
            nextSkipLog = frameStart + 60000000000ull;
        }

        if (haveFrame)
        {
            // when the newest frame was captured (so the hold below ends IdleTimeoutMs after capture stops)
            lastGoodNs = frameStart - std::min<uint64_t>(f.newestAgeMs, uint64_t(s.IdleTimeoutMs)) * 1000000ull;
            status("capturing (" + f.sel.mode + ")");
            const bool corrected = s.Output == "udp" && s.UdpProtocol == "oal16";
            for (size_t i = 0; i < areas.size(); i++)
            {
                double k = lightsOn ? s.Smoothing : 0.0;
                smooth[i].r = smooth[i].r * k + f.leds[i].r * (1 - k);
                smooth[i].g = smooth[i].g * k + f.leds[i].g * (1 - k);
                smooth[i].b = smooth[i].b * k + f.leds[i].b * (1 - k);
                RGBf c = corrected ? LedCorrect(smooth[i], s) : smooth[i];
                rgb[i * 3 + 0] = To16(c.r);
                rgb[i * 3 + 1] = To16(c.g);
                rgb[i * 3 + 2] = To16(c.b);
            }
            lightsOn = true;
            send(rgb);
            if (dryRun && frameStart >= nextPrint)
            {
                nextPrint = frameStart + 1000000000ull;
                Frame shown = f;
                shown.leds = smooth;
                PrintLeds(shown, areas);
            }
        }
        else if (lightsOn && s.EnableLights && frameStart - lastGoodNs < uint64_t(s.IdleTimeoutMs) * 1000000ull)
        {
            // A missed read or a short capture gap: keep showing the last colours
            // rather than blinking the strip off. Only IdleTimeoutMs without any
            // frame turns the LEDs off.
            send(rgb);
        }
        else
        {
            if (shm.hdr) status(s.EnableLights ? "idle: no fresh frames (headset idle or nothing presenting)" : "lights disabled in config");
            // Turn LEDs off once, then refresh "off" once a second.
            if (lightsOn || frameStart >= nextOffRefresh)
            {
                std::fill(rgb.begin(), rgb.end(), 0);
                send(rgb);
                lightsOn = false;
                nextOffRefresh = frameStart + 1000000000ull;
            }
        }

        uint64_t period = 1000000000ull / uint64_t(s.SampleRate);
        uint64_t spent = NowNs() - frameStart;
        if (spent < period) SleepNs(period - spent);
    }

    std::fill(rgb.begin(), rgb.end(), 0);
    if (!dryRun && s.Output == "udp") udp.Write(rgb, NowNs());
    if (!dryRun && s.Output == "serial" && ada.fd >= 0)
    {
        ada.Write(rgb);
        tcdrain(ada.fd);
    }
    mask.Save();
    panel.Disconnect();
    printf("oal-frame-daemon: stopped\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Calibration: shows fixed test colours on every LED (with the colour correction
// from oal.ini, but none of the capture settings) so you can compare them with the
// same colours on a phone or monitor (contrib/calibrate.html). oal.ini is
// reloaded as you save it, so LedWhite*/LedGamut/Dither changes show immediately.

struct CalStep
{
    const char* name;
    const char* hex;   // sRGB colour, as a screen would show it
    bool fade;         // slow 0..3% fade of white (dithering test)
};

const CalStep kCalSteps[] = {
    {"White", "#ffffff", false},       {"Grey 50%", "#808080", false},   {"Dark grey", "#404040", false},
    {"Very dark grey", "#202020", false}, {"Red", "#ff0000", false},     {"Green", "#00ff00", false},
    {"Blue", "#0000ff", false},        {"Yellow", "#ffff00", false},     {"Cyan", "#00ffff", false},
    {"Magenta", "#ff00ff", false},     {"Orange", "#ff8000", false},
    {"Slow fade, 0-3% white (dithering test: watch for steps or flicker)", "#000000", true},
};

int CmdCalibrate(const std::string& configPath)
{
    Settings s;
    std::string err;
    if (!LoadSettings(configPath, s, &err)) fprintf(stderr, "%s\n", err.c_str());
    time_t cfgTime = FileMTime(configPath);
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    UdpSink udp;
    Adalight ada;
    udp.Configure(s.UdpHost, s.UdpPort, s.UdpProtocol, s.NumLights);
    if (s.Output == "serial" && !ada.Open(s.SerialPort, s.BaudRate, s.NumLights))
    {
        fprintf(stderr, "serial: %s\n", ada.lastError.c_str());
        return 1;
    }
    const bool corrected = s.Output == "udp" && s.UdpProtocol == "oal16";
    printf("Calibration: open contrib/calibrate.html on a phone or monitor and step through it together.\n"
           "Enter = next colour, b + Enter = back, q + Enter = quit. Save oal.ini to apply changes live.\n");
    if (!corrected)
        printf("Note: colour correction and dithering need Output = udp and UdpProtocol = oal16 (currently %s %s).\n",
               s.Output.c_str(), s.Output == "udp" ? s.UdpProtocol.c_str() : "");
    const int nSteps = int(sizeof(kCalSteps) / sizeof(kCalSteps[0]));
    int step = 0, shown = -1;
    char lineCmd = 0; // 'b' when the current input line asks to go back
    uint64_t t0 = NowNs();
    std::vector<uint16_t> rgb(size_t(s.NumLights) * 3);
    while (!g_quit)
    {
        uint64_t now = NowNs();
        if (step != shown)
        {
            printf("[%d/%d] %s  %s\n", step + 1, nSteps, kCalSteps[step].name, kCalSteps[step].hex);
            fflush(stdout);
            shown = step;
            t0 = now;
        }
        time_t t = FileMTime(configPath);
        if (t != cfgTime)
        {
            cfgTime = t;
            Settings ns;
            if (LoadSettings(configPath, ns, &err))
            {
                s.LedWhiteRed = ns.LedWhiteRed;
                s.LedWhiteGreen = ns.LedWhiteGreen;
                s.LedWhiteBlue = ns.LedWhiteBlue;
                s.LedGamut = ns.LedGamut;
                s.Dither = ns.Dither;
                s.DitherCutoff = ns.DitherCutoff;
                printf("  oal.ini reloaded: white %.2f %.2f %.2f, gamut %.2f, dither %s (cutoff %.2f)\n", s.LedWhiteRed,
                       s.LedWhiteGreen, s.LedWhiteBlue, s.LedGamut, s.Dither ? "on" : "off", s.DitherCutoff);
                fflush(stdout);
            }
        }
        const CalStep& cs = kCalSteps[step];
        RGBf c;
        if (cs.fade)
        {
            double ph = std::fmod(double(now - t0) / 8e9, 1.0); // 8 s up and down
            double v = 0.03 * (ph < 0.5 ? ph * 2 : (1 - ph) * 2);
            c = {v, v, v};
        }
        else
        {
            unsigned r = 0, g = 0, b = 0;
            sscanf(cs.hex, "#%02x%02x%02x", &r, &g, &b);
            c = {SrgbToLinear(r / 255.0), SrgbToLinear(g / 255.0), SrgbToLinear(b / 255.0)};
        }
        if (corrected) c = LedCorrect(c, s);
        for (int i = 0; i < s.NumLights; i++)
        {
            rgb[size_t(i) * 3 + 0] = To16(c.r);
            rgb[size_t(i) * 3 + 1] = To16(c.g);
            rgb[size_t(i) * 3 + 2] = To16(c.b);
        }
        if (s.Output == "udp")
        {
            udp.dither = s.Dither;
            udp.cutoff = uint8_t(lround(s.DitherCutoff * 255.0));
            if (!udp.Write(rgb, now) && !udp.lastError.empty())
            {
                fprintf(stderr, "udp: %s\n", udp.lastError.c_str());
                udp.lastError.clear();
            }
        }
        else ada.Write(rgb);

        // Keys: read the terminal directly (stdio buffering would hold back lines typed together).
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 16) > 0)
        {
            char buf[64];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break; // stdin closed
            bool quit = false;
            for (ssize_t i = 0; i < n && !quit; i++)
            {
                if (buf[i] == 'q') quit = true;
                else if (buf[i] == 'b') lineCmd = 'b';
                else if (buf[i] == '\n')
                {
                    step = lineCmd == 'b' ? (step + nSteps - 1) % nSteps : (step + 1) % nSteps;
                    lineCmd = 0;
                }
            }
            if (quit) break;
        }
    }
    std::fill(rgb.begin(), rgb.end(), 0);
    if (s.Output == "udp") udp.Write(rgb, NowNs());
    else ada.Write(rgb);
    printf("calibration finished. Copy the values you settled on into oal.ini (they are already there if you edited it).\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string config = DefaultConfigPath();
    std::string snapshot;
    bool dryRun = false, status = false, selftest = false, calibrate = false;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config = argv[++i];
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--status") status = true;
        else if (a == "--snapshot" && i + 1 < argc) snapshot = argv[++i];
        else if (a == "--selftest") selftest = true;
        else if (a == "--calibrate") calibrate = true;
        else
        {
            fprintf(stderr,
                    "usage: %s [--config FILE] [--dry-run | --status | --snapshot OUT.ppm | --calibrate | --selftest]\n", argv[0]);
            return 2;
        }
    }
    if (selftest) return SelfTest();
    if (calibrate) return CmdCalibrate(config);
    Settings s;
    std::string err;
    if (!LoadSettings(config, s, &err) && (status || !snapshot.empty())) fprintf(stderr, "%s\n", err.c_str());
    if (status) return CmdStatus(s);
    if (!snapshot.empty()) return CmdSnapshot(s, snapshot);
    return Run(config, dryRun);
}
