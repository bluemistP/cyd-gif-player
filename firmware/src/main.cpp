// CYD GIF Player — Main Firmware
// ESP32-2432S028 (CYD2USB, dual USB-C + USB-B, ST7789)

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <FS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <XPT2046_Touchscreen.h>
#include "default_gif.h"

// ──────────────────────────────────────────────
// Pins
// ──────────────────────────────────────────────
// Touch (XPT2046) and SD card are on two independent SPI buses on the
// ESP32-2432S028 — they do NOT share wiring with each other or the TFT.
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CS   33
#define TOUCH_IRQ  36

#define SD_SCLK    18
#define SD_MISO    19
#define SD_MOSI    23
#define SD_CS      5

#define BACKLIGHT_LEDC_CHANNEL 0
#define BACKLIGHT_LEDC_FREQ    5000
#define BACKLIGHT_LEDC_RES     8   // 8-bit duty, 0..255

// Largest dimension either orientation can report (240 portrait / 320
// landscape) — used to size the scanline buffer once, at compile time.
#define MAX_DIM 320

// Physical panel size — the menu/settings UI and all touch handling stay
// pinned to this fixed orientation regardless of the rotation setting
// (only GIF playback content itself rotates). See toScreen() below.
#define NATIVE_W 240
#define NATIVE_H 320

// ──────────────────────────────────────────────
// Globals
// ──────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
AnimatedGIF gif;
// Separate decoder instance for web-thumbnail generation — the web server
// callbacks run on their own FreeRTOS task, so decoding a thumbnail
// concurrently with an actively-playing GIF on the shared `gif` object
// would corrupt both decodes' internal state.
AnimatedGIF thumbGif;
AsyncWebServer httpServer(80);
SPIClass touchSPI(HSPI);
SPIClass sdSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

bool sdAvailable = false;

// ──────────────────────────────────────────────
// Settings (persisted to flash)
// ──────────────────────────────────────────────
#define SETTINGS_NVS_NS "cyd_settings"

uint8_t  settingRotation    = 0;   // 0..3, TFT_eSPI rotation index
uint8_t  settingBrightness  = 100; // 0 (off), 25, 50, 75, 100
uint16_t settingSlideshowSec = 0;  // 0 = off (single GIF loops until tapped)
bool     settingWifiEnabled = true;
// The built-in flash GIF isn't a real file, so "deleting" it (no-SD mode
// only) just hides it from the list from then on — this flag is that.
bool     settingDefaultGifRemoved = false;

// Raw XPT2046 ADC reading corresponding to each screen edge — factory
// defaults; overwritten by the on-device calibration flow (see
// runTouchCalibration()). Axes can be inverted (e.g. X's "0" reading
// being numerically larger than its "239" reading) — that's normal for
// resistive touch and map() handles it fine either direction.
// Measured directly from a real 4-corner tap test on this unit (this
// panel's raw X/Y axes are rotated 90° from the screen — see toScreen()
// — so calRawXAtScreen0/Max are actually the controller's Y-axis
// readings, and calRawYAtScreen0/Max are its X-axis readings).
int32_t calRawXAtScreen0   = 3573;
int32_t calRawXAtScreenMax = 468;
int32_t calRawYAtScreen0   = 400;
int32_t calRawYAtScreenMax = 3667;

// Bump this whenever a stored calibration needs to be force-discarded
// (e.g. a bug let a bad one through) — mismatched version means "ignore
// whatever is saved, use factory defaults" regardless of any other check.
#define CAL_FORMAT_VERSION 7

// A 12-bit ADC reading is 0..4095; extrapolating two close-together taps
// out to the screen edges can overshoot well past that, producing a value
// every real touch clamps against — so bounds-check, not just range-check.
bool calibrationLooksValid(int32_t x0, int32_t xMax, int32_t y0, int32_t yMax) {
  const int32_t MIN_RANGE = 500;
  const int32_t ADC_LO = -200, ADC_HI = 4300; // generous margin around 0..4095
  if (abs(xMax - x0) < MIN_RANGE || abs(yMax - y0) < MIN_RANGE) return false;
  if (x0 < ADC_LO || x0 > ADC_HI || xMax < ADC_LO || xMax > ADC_HI) return false;
  if (y0 < ADC_LO || y0 > ADC_HI || yMax < ADC_LO || yMax > ADC_HI) return false;
  return true;
}

void saveSettings(); // forward decl — loadSettings() self-heals bad calibration by re-saving

void loadSettings() {
  Preferences p;
  p.begin(SETTINGS_NVS_NS, true);
  settingRotation = p.getUChar("rotation", 0) % 4;
  settingBrightness = p.getUChar("brightness", 100);
  // A fully-off backlight (0) is indistinguishable from a dead/bricked
  // screen — no longer a selectable level (see the cycling levels[] in
  // handleSettingsTap()/runPlayer()), but clamp here too in case 0 is
  // still sitting in flash from before that change.
  if (settingBrightness < 10) settingBrightness = 10;
  settingSlideshowSec = p.getUShort("slideshow", 0);
  settingWifiEnabled = p.getBool("wifiEnabled", true);
  settingDefaultGifRemoved = p.getBool("defGifRm", false);
  uint8_t storedCalVersion = p.getUChar("calVer", 0);
  calRawXAtScreen0   = p.getInt("calX0", 3573);
  calRawXAtScreenMax = p.getInt("calXmax", 468);
  calRawYAtScreen0   = p.getInt("calY0", 400);
  calRawYAtScreenMax = p.getInt("calYmax", 3667);
  p.end();

  if (storedCalVersion != CAL_FORMAT_VERSION) {
    Serial.println("[CAL] Stored calibration is from an old/unknown format - resetting to defaults");
    calRawXAtScreen0 = 3573; calRawXAtScreenMax = 468;
    calRawYAtScreen0 = 400;  calRawYAtScreenMax = 3667;
    // Also reset brightness — a stray tap during this whole debugging
    // session may have cycled it down to 0 (backlight off), which would
    // otherwise look identical to a bricked screen after this update.
    settingBrightness = 100;
    saveSettings();
    return;
  }

  // Guard against a stored calibration that's collapsed or overshoots the
  // ADC's real range — that silently breaks touch everywhere, with no way
  // to navigate back to re-calibrate. Reset to factory defaults instead.
  if (!calibrationLooksValid(calRawXAtScreen0, calRawXAtScreenMax, calRawYAtScreen0, calRawYAtScreenMax)) {
    Serial.println("[CAL] Stored calibration invalid - resetting to defaults");
    calRawXAtScreen0 = 3573; calRawXAtScreenMax = 468;
    calRawYAtScreen0 = 400;  calRawYAtScreenMax = 3667;
    saveSettings();
  }

  Serial.printf("[CAL] active: X0=%d Xmax=%d Y0=%d Ymax=%d\n",
                calRawXAtScreen0, calRawXAtScreenMax, calRawYAtScreen0, calRawYAtScreenMax);
}

void saveSettings() {
  Preferences p;
  p.begin(SETTINGS_NVS_NS, false);
  p.putUChar("rotation", settingRotation);
  p.putUChar("brightness", settingBrightness);
  p.putUShort("slideshow", settingSlideshowSec);
  p.putBool("wifiEnabled", settingWifiEnabled);
  p.putBool("defGifRm", settingDefaultGifRemoved);
  p.putUChar("calVer", CAL_FORMAT_VERSION);
  p.putInt("calX0", calRawXAtScreen0);
  p.putInt("calXmax", calRawXAtScreenMax);
  p.putInt("calY0", calRawYAtScreen0);
  p.putInt("calYmax", calRawYAtScreenMax);
  p.end();
}

void applyBrightness() {
  uint8_t duty = map(settingBrightness, 0, 100, 0, 255);
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, duty);
}

void applyRotation() {
  tft.setRotation(settingRotation);
  tft.fillScreen(TFT_BLACK); // old content doesn't match the new geometry
}

// ──────────────────────────────────────────────
// Wi-Fi provisioning
// ──────────────────────────────────────────────
#define WIFI_NVS_NS        "cyd_wifi"
#define WIFI_NVS_SSID      "ssid"
#define WIFI_NVS_PASS      "pass"
#define PROVISION_AP_SSID  "CYD-GIF-Setup"
#define PROVISION_AP_TIMEOUT_MS  300000
#define MDNS_HOSTNAME      "cydgifplayer" // reachable as http://cydgifplayer.local/

char wifiSSID[64] = "";
char wifiPass[64] = "";
unsigned long provisionStart = 0;
bool hasCredentials = false;

void loadWifiCreds() {
  Preferences p;
  p.begin(WIFI_NVS_NS, true);
  wifiSSID[0] = '\0'; wifiPass[0] = '\0';
  if (p.isKey(WIFI_NVS_SSID)) {
    p.getString(WIFI_NVS_SSID, wifiSSID, sizeof(wifiSSID));
    p.getString(WIFI_NVS_PASS, wifiPass, sizeof(wifiPass));
  }
  hasCredentials = p.isKey(WIFI_NVS_SSID);
  p.end();
}

void saveWifiCreds(const char *ssid, const char *pass) {
  Preferences p;
  p.begin(WIFI_NVS_NS, false);
  p.putString(WIFI_NVS_SSID, ssid);
  p.putString(WIFI_NVS_PASS, pass);
  p.end();
}

// Wipes only the Wi-Fi namespace — touch calibration, brightness, etc.
// (a separate NVS namespace, see SETTINGS_NVS_NS) are untouched. Caller is
// responsible for rebooting afterward; hasCredentials still reads stale
// until loadWifiCreds() or a restart re-reads it.
void resetWifiCredentials() {
  Preferences p;
  p.begin(WIFI_NVS_NS, false);
  p.clear();
  p.end();
  Serial.println("[BOOT] Wi-Fi credentials cleared");
}

bool connectWiFi(const char *ssid, const char *pass, uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(500);
    Serial.print('.');
  }
  return WiFi.status() == WL_CONNECTED;
}

// ──────────────────────────────────────────────
// GIF list (SD-backed, or a single built-in entry with no SD)
// ──────────────────────────────────────────────
#define MAX_GIFS  64
#define MAX_FN    64
#define MAX_UPLOAD_BYTES (8UL * 1024 * 1024) // SD card has plenty of room
// No trailing slash — ESP32's SD/FatFs exists()/open() are unreliable
// with one (directory creation and listing silently fail).
#define SCAN_DIR  "/gifs"

// No-SD fallback storage: a small corner of the onboard flash's LittleFS
// partition (~1.1MB total, shared with the web UI's own assets), NOT the
// SD card — a completely separate backend from SCAN_DIR above, never
// mixed with it. 500KB is a hardcoded budget across the whole folder
// (not per file), deliberately well under the partition's real free
// space so the web UI assets always have room.
#define UPLOADS_DIR "/uploads"
#define LFS_UPLOAD_CAP_BYTES (500UL * 1024)

struct GifEntry { char name[MAX_FN]; size_t size; };

GifEntry gifList[MAX_GIFS];
int     gifCount   = 0;
int     playIdx    = 0;
bool    playing    = false;
bool    shouldQuit = false;
char    currentFN[MAX_FN] = "";

// ──────────────────────────────────────────────
// Screens
// ──────────────────────────────────────────────
enum AppScreen { SCREEN_MENU, SCREEN_SETTINGS, SCREEN_TOUCH_HELP };
AppScreen currentScreen = SCREEN_MENU;
int menuPage = 0;

void drawMenu();
void drawSettings();
void drawTouchHelp();

// ──────────────────────────────────────────────
// Scan SD
// ──────────────────────────────────────────────
// Re-runs the SD card init handshake and updates sdAvailable to match
// reality. Used both to detect a card that's been pulled at runtime
// (sdAvailable is otherwise only ever set once, at boot) and to notice
// one being reinserted later.
bool refreshSdAvailable() {
  // SDFS::begin() (arduino-esp32's SD.cpp) short-circuits to `return true`
  // whenever it's already mounted (_pdrv != 0xFF) — it does NOT re-probe
  // the hardware. Without end() first, this always reports the card as
  // still present even after it's been physically pulled. end() is a
  // safe no-op if nothing is mounted (e.g. the very first boot-time call).
  SD.end();
  bool ok = SD.begin(SD_CS, sdSPI, 40000000, "/sd", 1);
  if (ok != sdAvailable) {
    Serial.printf("[SD] %s\n", ok ? "Card detected" : "Card lost");
  }
  sdAvailable = ok;
  return ok;
}

void scanGifsSD() {
  if (!SD.exists(SCAN_DIR)) {
    SD.mkdir(SCAN_DIR);
    Serial.println("[SCAN] Created " SCAN_DIR);
    return;
  }

  File dir = SD.open(SCAN_DIR);
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String name = String(f.name());
      if (name.endsWith(".gif") || name.endsWith(".GIF")) {
        strncpy(gifList[gifCount].name, name.c_str(), MAX_FN - 1);
        gifList[gifCount].name[MAX_FN - 1] = '\0';
        gifList[gifCount].size = f.size();
        gifCount++;
      }
    }
    f.close();
    if (gifCount >= MAX_GIFS) break;
  }
  dir.close();
  Serial.printf("[SCAN] %d GIFs found (SD)\n", gifCount);
}

// No-SD fallback listing: the built-in flash GIF (unless the user has
// "deleted" it, see settingDefaultGifRemoved) followed by whatever's
// been uploaded into LittleFS's UPLOADS_DIR — entirely separate from
// scanGifsSD() above, never merged with it.
void scanGifsLFS() {
  if (!settingDefaultGifRemoved) {
    strncpy(gifList[gifCount].name, "default.gif", MAX_FN - 1);
    gifList[gifCount].name[MAX_FN - 1] = '\0';
    gifList[gifCount].size = default_gif_len;
    gifCount++;
  }

  if (!LittleFS.exists(UPLOADS_DIR)) {
    LittleFS.mkdir(UPLOADS_DIR);
    Serial.println("[SCAN] Created " UPLOADS_DIR);
    return;
  }

  File dir = LittleFS.open(UPLOADS_DIR);
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String name = String(f.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1); // LittleFS may report the full path
      if (name.endsWith(".gif") || name.endsWith(".GIF")) {
        strncpy(gifList[gifCount].name, name.c_str(), MAX_FN - 1);
        gifList[gifCount].name[MAX_FN - 1] = '\0';
        gifList[gifCount].size = f.size();
        gifCount++;
      }
    }
    f.close();
    if (gifCount >= MAX_GIFS) break;
  }
  dir.close();
  Serial.printf("[SCAN] %d GIFs found (LittleFS fallback)\n", gifCount);
}

void scanGifs() {
  gifCount = 0;
  menuPage = 0;

  if (sdAvailable) scanGifsSD();
  else scanGifsLFS();
}

// Total bytes currently used by uploaded (non-default) GIFs in the
// LittleFS fallback folder — used to enforce LFS_UPLOAD_CAP_BYTES.
uint32_t lfsUploadsUsedBytes() {
  if (!LittleFS.exists(UPLOADS_DIR)) return 0;
  uint32_t total = 0;
  File dir = LittleFS.open(UPLOADS_DIR);
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) total += f.size();
    f.close();
  }
  dir.close();
  return total;
}

// Cheap checksum over the current file list, used to skip redrawing the
// menu (and resetting menuPage) when a periodic rescan finds nothing
// actually changed — see the periodic rescan in loop().
uint32_t gifListSignature() {
  uint32_t sig = (uint32_t)gifCount * 2654435761u; // Knuth's multiplicative hash seed
  for (int i = 0; i < gifCount; i++) {
    for (const char *p = gifList[i].name; *p; p++) sig = sig * 33 + (uint8_t)*p;
    sig = sig * 33 + (uint32_t)gifList[i].size;
  }
  return sig;
}

// ──────────────────────────────────────────────
// Touch
// ──────────────────────────────────────────────
static const uint32_t TOUCH_DEBOUNCE_MS = 250;
static bool  touchPressed = false;
static uint32_t lastTap = 0;

bool readTouch(int *x, int *y) {
  bool down = ts.touched();
  if (down) {
    TS_Point p = ts.getPoint();
    *x = p.x; *y = p.y;
    return true;
  }
  return false;
}

// The XPT2046 touch overlay is a physical object glued to the screen —
// its raw ADC readings never move, regardless of tft.setRotation(). Touch
// (and the menu/settings UI) deliberately stay pinned to this one fixed
// physical orientation always; only GIF *content* in the player rotates
// with the rotation setting. This keeps the four touch corners meaning
// the same thing no matter what rotation is currently selected.
void toScreen(int tx, int ty, int *sx, int *sy) {
  // This panel's raw touch axes are rotated 90° from the screen: the
  // controller's Y reading tracks screen X (left/right), and its X
  // reading tracks screen Y (top/bottom). Confirmed with a real 4-corner
  // tap test — a swap like this can't be fixed by per-axis offset/scale
  // tuning alone, it has to be swapped explicitly here.
  *sx = constrain(map(ty, calRawXAtScreen0, calRawXAtScreenMax, 0, NATIVE_W - 1), 0, NATIVE_W - 1);
  *sy = constrain(map(tx, calRawYAtScreen0, calRawYAtScreenMax, 0, NATIVE_H - 1), 0, NATIVE_H - 1);
}

static int firstTouchX = 0, firstTouchY = 0;

bool pollTap(int *sx, int *sy) {
  if (millis() - lastTap < TOUCH_DEBOUNCE_MS) return false;
  int tx, ty;
  if (readTouch(&tx, &ty)) {
    // Capture only the FIRST sample of this press. Resistive touch panels
    // get noisy right as the finger lifts off (pressure drops unevenly
    // across the contact area) — sampling at release, as this used to do,
    // was picking up that release-noise instead of a stable reading.
    if (!touchPressed) {
      firstTouchX = tx;
      firstTouchY = ty;
    }
    touchPressed = true;
    return false;
  }
  if (touchPressed) {
    touchPressed = false;
    toScreen(firstTouchX, firstTouchY, sx, sy);
    Serial.printf("[TOUCH RAW] tx=%d ty=%d -> sx=%d sy=%d\n", firstTouchX, firstTouchY, *sx, *sy);
    lastTap = millis();
    return true;
  }
  return false;
}

// ──────────────────────────────────────────────
// Drawing helpers
// ──────────────────────────────────────────────
inline void fillR(int x, int y, int w, int h, uint16_t c) {
  tft.fillRect(x, y, w, h, c);
}

inline void drawStr(const char *s, int x, int y, int sz, uint16_t fg, uint16_t bg) {
  tft.setTextColor(fg, bg);
  tft.setTextSize(sz);
  tft.setCursor(x, y);
  tft.print(s);
}

void trunc(char *dst, const char *src, size_t n) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
  size_t l = strlen(dst);
  while (l > 0 && (dst[l-1] == ' ' || dst[l-1] == '\t')) dst[--l] = '\0';
}

// ──────────────────────────────────────────────
// GIF playback / decoding
// ──────────────────────────────────────────────
// AnimatedGIF API:
//   gif.open(filename, openCb, closeCb, readCb, seekCb, drawCb)  — from SD
//   gif.open(pData, dataSize, drawCb)                            — from flash (built-in default)
//   gif.begin()          — call ONCE at startup; resets internal state
//   gif.playFrame(false, &delayMs, NULL)  — 1 for more frames, 0 when done
//   gif.close()
//   gif.getFrameWidth(), gif.getFrameHeight()
//   gif.getCanvasWidth(), gif.getCanvasHeight()

static void *gifOpen(const char *name, int32_t *fsize) {
  File *f = new File(SD.open(name, FILE_READ));
  if (!f || !*f) {
    delete f;
    return nullptr;
  }
  *fsize = (int32_t)f->size();
  return f;
}

static void gifClose(void *handle) {
  File *f = (File *)handle;
  if (f) {
    f->close();
    delete f;
  }
}

static int32_t gifRead(GIFFILE *pFile, uint8_t *buf, int32_t len) {
  File *f = (File *)pFile->fHandle;
  if (!f) return 0;
  size_t n = f->read(buf, (size_t)len);
  pFile->iPos = (int32_t)f->position();
  return (int32_t)n;
}

static int32_t gifSeek(GIFFILE *pFile, int32_t pos) {
  File *f = (File *)pFile->fHandle;
  if (!f) return -1;
  if (!f->seek((uint32_t)pos)) return -1;
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// Same four callbacks as above, but reading from LittleFS instead of the
// SD card — used only for the no-SD fallback's uploaded GIFs.
static void *lfsGifOpen(const char *name, int32_t *fsize) {
  File *f = new File(LittleFS.open(name, FILE_READ));
  if (!f || !*f) {
    delete f;
    return nullptr;
  }
  *fsize = (int32_t)f->size();
  return f;
}

static void lfsGifClose(void *handle) {
  File *f = (File *)handle;
  if (f) {
    f->close();
    delete f;
  }
}

static int32_t lfsGifRead(GIFFILE *pFile, uint8_t *buf, int32_t len) {
  File *f = (File *)pFile->fHandle;
  if (!f) return 0;
  size_t n = f->read(buf, (size_t)len);
  pFile->iPos = (int32_t)f->position();
  return (int32_t)n;
}

static int32_t lfsGifSeek(GIFFILE *pFile, int32_t pos) {
  File *f = (File *)pFile->fHandle;
  if (!f) return -1;
  if (!f->seek((uint32_t)pos)) return -1;
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// Opens filename by name on whichever backend is currently active —
// SD, the built-in flash GIF, or a LittleFS-uploaded fallback GIF —
// against any AnimatedGIF instance (the main playback one or the
// separate thumbnail one), so both playback and thumbnailing share one
// place that knows how those three sources differ.
bool openGifSmart(AnimatedGIF &g, const char *name, GIF_DRAW_CALLBACK *drawCb) {
  if (sdAvailable) {
    String fullPath = String(SCAN_DIR) + "/" + name;
    return g.open(fullPath.c_str(), gifOpen, gifClose, gifRead, gifSeek, drawCb) != 0;
  }
  if (strcmp(name, "default.gif") == 0) {
    return g.openFLASH((uint8_t *)default_gif, default_gif_len, drawCb) != 0;
  }
  String fullPath = String(UPLOADS_DIR) + "/" + name;
  return g.open(fullPath.c_str(), lfsGifOpen, lfsGifClose, lfsGifRead, lfsGifSeek, drawCb) != 0;
}

// Where the next decoded GIF should be drawn — fullscreen during playback,
// or a small rect for a menu thumbnail. Reset to fullscreen after each use.
struct DrawTarget { int x, y, w, h; };
DrawTarget drawTarget = {0, 0, 240, 320};

// Set true whenever the canvas size or drawTarget for the next
// gif.playFrame() run might have changed (a new file opened, or the
// draw rect changed) — the scale/offset math in myDrawCallback() below
// only depends on those, both fixed for a whole file's playback, so
// recomputing it on every one of a frame's scanlines (as this used to
// do) was pure wasted CPU time repeated ~as many times as the frame is
// tall, for every single frame.
bool frameGeomDirty = true;

// Nearest-neighbor scale-to-fit, both up AND down, into drawTarget. Each
// source row/column maps to a *range* of output rows/columns — for
// upscaling that range spans several pixels (duplicated), for downscaling
// most source rows/columns map to an empty range (skipped), leaving
// exactly one representative source pixel per output pixel.
static void myDrawCallback(GIFDRAW *d) {
  uint8_t *pixels = d->pPixels;
  uint16_t *palette = d->pPalette;
  int lineW = d->iWidth;

  static float scale;
  static int frameOx, frameOy;
  if (frameGeomDirty) {
    int canvasW = gif.getCanvasWidth();
    int canvasH = gif.getCanvasHeight();
    scale = min((float)drawTarget.w / canvasW, (float)drawTarget.h / canvasH);
    frameOx = drawTarget.x + (int)((drawTarget.w - canvasW * scale) / 2);
    frameOy = drawTarget.y + (int)((drawTarget.h - canvasH * scale) / 2);
    frameGeomDirty = false;
  }

  // d->iY/iX = this frame's fixed corner offset on the canvas;
  // d->y = the scanline currently being drawn within the frame (0..iHeight-1).
  int srcRow = d->iY + d->y;
  int outRowStart = frameOy + (int)(srcRow * scale);
  int outRowEnd   = frameOy + (int)((srcRow + 1) * scale) - 1;
  if (outRowEnd < outRowStart) return; // this source row falls between two output rows
  if (outRowStart < drawTarget.y) outRowStart = drawTarget.y;
  if (outRowEnd >= drawTarget.y + drawTarget.h) outRowEnd = drawTarget.y + drawTarget.h - 1;
  if (outRowStart > outRowEnd) return;

  // Build one row of output pixels in memory, then blast the whole row to
  // the screen in a single burst per output line. Per-pixel drawPixel()
  // calls each pay their own SPI transaction overhead, which is slow
  // enough to be visible as a top-to-bottom "wipe" on larger frames.
  //
  // (DMA pixel pushes were tried here to overlap SPI transfer with
  // AnimatedGIF's decode of the next scanline, but hung this board during
  // the very first thumbnail decode — likely a DMA-channel conflict with
  // the SD card's own SPI/DMA usage on its separate bus. Reverted to
  // plain synchronous pushPixels().)
  static uint16_t lineBuf[MAX_DIM];
  int rowStartX = -1;
  int rowW = 0;

  for (int x = 0; x < lineW; x++) {
    int srcCol = d->iX + x;
    int outColStart = frameOx + (int)(srcCol * scale);
    int outColEnd   = frameOx + (int)((srcCol + 1) * scale) - 1;
    if (outColEnd < outColStart) continue;
    if (outColStart < drawTarget.x) outColStart = drawTarget.x;
    if (outColEnd >= drawTarget.x + drawTarget.w) outColEnd = drawTarget.x + drawTarget.w - 1;
    if (rowStartX < 0) rowStartX = outColStart;

    uint16_t c = palette[pixels[x]];
    for (int ox = outColStart; ox <= outColEnd; ox++) {
      lineBuf[ox - rowStartX] = c;
      rowW = ox - rowStartX + 1;
    }
  }
  if (rowW <= 0) return;

  // One source row can expand into several duplicate output rows when
  // upscaling — set the address window once for the whole block instead
  // of once per row, and just keep streaming the same line buffer into
  // it. Cuts SPI command overhead roughly in proportion to the scale
  // factor (e.g. 2x upscale = half as many setAddrWindow() calls).
  tft.startWrite();
  tft.setAddrWindow(rowStartX, outRowStart, rowW, outRowEnd - outRowStart + 1);
  for (int oy = outRowStart; oy <= outRowEnd; oy++) {
    tft.pushPixels(lineBuf, rowW);
  }
  tft.endWrite();
}

// Opens filename (or the built-in default when there's no SD) and returns
// whether it succeeded. Caller is responsible for gif.close() either way —
// AnimatedGIF may open the underlying file even when the GIF itself turns
// out to be invalid, and never releases it on that failure path itself.
bool gifOpenByIndex(const char *filename) {
  return openGifSmart(gif, filename, myDrawCallback);
}

// Decodes and draws just the first frame into a small rect — used for menu
// thumbnails. Leaves drawTarget reset to fullscreen when done.
void drawThumbnail(const char *filename, int x, int y, int w, int h) {
  fillR(x, y, w, h, TFT_BLACK);
  drawTarget = {x, y, w, h};

  if (gifOpenByIndex(filename)) {
    int delayMs = 0;
    frameGeomDirty = true;
    gif.playFrame(false, &delayMs, NULL);
  } else {
    drawStr("?", x + w / 2 - 4, y + h / 2 - 8, 2, TFT_RED, TFT_BLACK);
  }
  gif.close();

  drawTarget = {0, 0, tft.width(), tft.height()};
}

// ──────────────────────────────────────────────
// Web-facing thumbnails (raw RGB565, not full GIF bytes)
// ──────────────────────────────────────────────
// Decoding just the first frame into a small fixed buffer, instead of
// streaming the whole (possibly multi-MB) GIF file to the browser, cuts
// both SD read time and network payload down to a handful of KB per
// thumbnail. See /thumb below.
#define THUMB_DIM 80
static uint16_t thumbBuf[THUMB_DIM * THUMB_DIM];

static void myThumbCallback(GIFDRAW *d) {
  uint8_t *pixels = d->pPixels;
  uint16_t *palette = d->pPalette;
  int lineW = d->iWidth;

  int canvasW = thumbGif.getCanvasWidth();
  int canvasH = thumbGif.getCanvasHeight();
  float scale = min((float)THUMB_DIM / canvasW, (float)THUMB_DIM / canvasH);

  int frameOx = (int)((THUMB_DIM - canvasW * scale) / 2);
  int frameOy = (int)((THUMB_DIM - canvasH * scale) / 2);

  int srcRow = d->iY + d->y;
  int outRowStart = frameOy + (int)(srcRow * scale);
  int outRowEnd   = frameOy + (int)((srcRow + 1) * scale) - 1;
  if (outRowEnd < outRowStart) return;
  if (outRowStart < 0) outRowStart = 0;
  if (outRowEnd >= THUMB_DIM) outRowEnd = THUMB_DIM - 1;
  if (outRowStart > outRowEnd) return;

  for (int x = 0; x < lineW; x++) {
    int srcCol = d->iX + x;
    int outColStart = frameOx + (int)(srcCol * scale);
    int outColEnd   = frameOx + (int)((srcCol + 1) * scale) - 1;
    if (outColEnd < outColStart) continue;
    if (outColStart < 0) outColStart = 0;
    if (outColEnd >= THUMB_DIM) outColEnd = THUMB_DIM - 1;

    uint16_t c = palette[pixels[x]];
    for (int oy = outRowStart; oy <= outRowEnd; oy++) {
      uint16_t *row = thumbBuf + oy * THUMB_DIM;
      for (int ox = outColStart; ox <= outColEnd; ox++) row[ox] = c;
    }
  }
}

// Fullscreen playback with no header/status bar. Touch is split into four
// invisible quadrants:
//   top-left = back to menu, top-right = next GIF,
//   bottom-left = cycle rotation, bottom-right = cycle brightness.
// A GIF that finishes on its own (or the optional slideshow timer) just
// advances to the next one; there's always something on screen.
void runPlayer() {
  bool exitToMenu = false;
  bool needsClear = false; // the fillScreen below already covers the first GIF

  tft.setRotation(settingRotation);
  tft.fillScreen(TFT_BLACK); // menu chrome would otherwise show through letterboxing

  int consecutiveFailures = 0;
  uint32_t lastSdRetry = millis();

  while (!exitToMenu) {
    // While running on the built-in fallback GIF (card lost, or never
    // present), periodically re-probe so a reinserted card is picked up
    // without requiring a reboot.
    if (!sdAvailable && millis() - lastSdRetry > 5000) {
      lastSdRetry = millis();
      if (refreshSdAvailable()) {
        scanGifs();
        playIdx = 0;
        needsClear = true;
      }
    }

    drawTarget = {0, 0, tft.width(), tft.height()};
    frameGeomDirty = true; // canvas size + drawTarget are fixed for this whole file's playback

    // A GIF that doesn't fill the whole screen (different aspect ratio /
    // resolution than the one before it) only overwrites its own
    // letterboxed area — without this, the previous GIF's pixels linger
    // outside it after advancing. Skipped when replaying the same file in
    // a loop (natural completion, no advance) since nothing changed.
    if (needsClear) {
      tft.fillScreen(TFT_BLACK);
      needsClear = false;
    }

    strncpy(currentFN, gifList[playIdx].name, sizeof(currentFN) - 1);
    currentFN[sizeof(currentFN) - 1] = '\0';
    shouldQuit = false;

    if (!gifOpenByIndex(gifList[playIdx].name)) {
      gif.close();
      Serial.printf("[PLAY] Failed: %s\n", currentFN);

      // Every file in the current list failed to open in a row — almost
      // certainly the card was pulled out from under us mid-playback,
      // not that this one file is corrupt. Confirm and fall back to the
      // built-in GIF instead of cycling failures forever.
      if (sdAvailable && ++consecutiveFailures >= gifCount && !refreshSdAvailable()) {
        Serial.println("[PLAY] SD lost mid-playback — falling back to built-in GIF");
        scanGifs();
        playIdx = 0;
        consecutiveFailures = 0;
        needsClear = true;
        continue;
      }

      delay(200); // avoid hammering a bad file in a tight retry loop
      playIdx = (playIdx + 1) % gifCount;
      needsClear = true;
      continue;
    }
    consecutiveFailures = 0;

    Serial.printf("[PLAY] %s (%dx%d)\n",
                  currentFN, (int)gif.getFrameWidth(), (int)gif.getFrameHeight());
    playing = true;
    uint32_t enteredAt = millis();
    bool advance = false;
    int delayMs = 0;
    uint32_t frameStart = millis();

    while (gif.playFrame(false, &delayMs, NULL) != 0) {
      // delayMs is the time budget for this WHOLE frame (decode + render +
      // pause), not extra time to wait on top of however long decode/render
      // just took — sleeping the full amount here (as this used to do) adds
      // decode/render time on top of every single frame's delay, so
      // playback drifts progressively slower than the GIF's authored pace,
      // worse on complex/high-resolution frames.
      uint32_t elapsed = millis() - frameStart;
      int remaining = delayMs - (int)elapsed;

      int sx, sy;
      if (pollTap(&sx, &sy) || shouldQuit) {
        shouldQuit = false;
        // Touch is always in the fixed physical/native orientation (see
        // toScreen()) — compare against native dimensions here, NOT
        // tft.width()/height(), which swap when rotated content is drawn.
        bool top = sy < NATIVE_H / 2;
        bool left = sx < NATIVE_W / 2;
        if (top && left) {
          exitToMenu = true;
        } else if (top && !left) {
          advance = true;
        } else if (!top && left) {
          settingRotation = (settingRotation + 1) % 4;
          saveSettings();
          applyRotation();
          // geometry changed — restart this GIF fresh at the new size
        } else {
          static const uint8_t levels[] = {100, 75, 50, 25, 10};
          int cur = 0;
          for (int i = 0; i < 5; i++) if (levels[i] == settingBrightness) cur = i;
          settingBrightness = levels[(cur + 1) % 5];
          saveSettings();
          applyBrightness();
          frameStart = millis();
          continue; // brightness doesn't affect layout — keep playing this frame stream
        }
        break;
      }
      if (settingSlideshowSec > 0 && millis() - enteredAt > settingSlideshowSec * 1000UL) {
        advance = true;
        break;
      }
      if (remaining > 0) vTaskDelay(pdMS_TO_TICKS(remaining));
      // else: decode + render already ate the whole frame budget (or more)
      // — move on immediately rather than compounding the lag further.
      frameStart = millis();
    }

    gif.close();
    playing = false;

    if (exitToMenu) break;
    if (advance) {
      playIdx = (playIdx + 1) % gifCount;
      needsClear = true;
    }
    // else: natural completion, or a rotation change — loop back around
    // and replay/continue at playIdx (unchanged).
  }

  scanGifs();
  drawMenu();
}

// ──────────────────────────────────────────────
// Menu screen — 2x2 grid of thumbnails, paged 4 at a time
// ──────────────────────────────────────────────
static const int TOP_BAR = 32;
static const int BOTTOM_BAR = 32;

void drawMenu() {
  currentScreen = SCREEN_MENU;
  // Menu/settings UI always stays in the fixed native orientation — only
  // GIF playback content itself follows the rotation setting.
  if (tft.getRotation() != 0) {
    tft.setRotation(0);
  }
  int W = tft.width(), H = tft.height();
  fillR(0, 0, W, H, TFT_BLACK);

  fillR(0, 0, W, TOP_BAR, TFT_BLUE);
  drawStr("CYD GIF Player", 4, 9, 1, TFT_WHITE, TFT_BLUE);
  tft.drawRect(W - 66, 4, 62, TOP_BAR - 8, TFT_WHITE);
  drawStr("Settings", W - 62, 9, 1, TFT_WHITE, TFT_BLUE);

  int gridBottom = H - BOTTOM_BAR;

  if (gifCount == 0) {
    drawStr("No GIFs", W / 2 - 28, (TOP_BAR + gridBottom) / 2, 2, TFT_DARKGREY, TFT_BLACK);
    return;
  }

  int totalPages = (gifCount + 3) / 4;
  if (menuPage >= totalPages) menuPage = totalPages - 1;
  if (menuPage < 0) menuPage = 0;

  int cellW = W / 2;
  int cellH = (gridBottom - TOP_BAR) / 2;
  int startIdx = menuPage * 4;

  for (int i = 0; i < 4; i++) {
    int idx = startIdx + i;
    if (idx >= gifCount) continue;
    int col = i % 2, row = i / 2;
    int cx = col * cellW, cy = TOP_BAR + row * cellH;
    const int pad = 4, labelH = 12;

    drawThumbnail(gifList[idx].name, cx + pad, cy + pad, cellW - pad * 2, cellH - pad * 2 - labelH);
    tft.drawRect(cx, cy, cellW, cellH, TFT_DARKGREY);

    char label[24];
    trunc(label, gifList[idx].name, sizeof(label));
    drawStr(label, cx + pad, cy + cellH - labelH, 1, TFT_CYAN, TFT_BLACK);
  }

  fillR(0, gridBottom, W, BOTTOM_BAR, TFT_BLUE);
  if (totalPages > 1) {
    if (menuPage > 0) drawStr("<", 10, gridBottom + 8, 2, TFT_WHITE, TFT_BLUE);
    char pg[16];
    snprintf(pg, sizeof(pg), "%d/%d", menuPage + 1, totalPages);
    drawStr(pg, W / 2 - 14, gridBottom + 9, 1, TFT_WHITE, TFT_BLUE);
    if (menuPage < totalPages - 1) drawStr(">", W - 20, gridBottom + 8, 2, TFT_WHITE, TFT_BLUE);
  }
}

void handleMenuTap(int sx, int sy) {
  int W = tft.width(), H = tft.height();

  // The whole top bar opens Settings — nothing else up there is
  // interactive. Touch target matches the drawn bar exactly (32px); this
  // used to be padded taller to compensate for an unreliable calibration
  // — no longer needed now that calibration is stable.
  if (sy < TOP_BAR) {
    Serial.println("[ACTION] Menu: top bar -> open Settings");
    drawSettings();
    return;
  }

  int gridBottom = H - BOTTOM_BAR;
  if (sy >= gridBottom) {
    int totalPages = (gifCount + 3) / 4;
    if (sx < 44 && menuPage > 0) {
      Serial.println("[ACTION] Menu: bottom bar -> page prev");
      menuPage--; drawMenu();
    } else if (sx > W - 44 && menuPage < totalPages - 1) {
      Serial.println("[ACTION] Menu: bottom bar -> page next");
      menuPage++; drawMenu();
    } else {
      Serial.println("[ACTION] Menu: bottom bar -> no-op (edge/no more pages)");
    }
    return;
  }

  int cellW = W / 2;
  int cellH = (gridBottom - TOP_BAR) / 2;
  int col = sx / cellW;
  int row = (sy - TOP_BAR) / cellH;
  int idx = menuPage * 4 + row * 2 + col;
  if (idx >= 0 && idx < gifCount) {
    playIdx = idx;
    Serial.printf("[ACTION] Menu: grid cell (row=%d col=%d) -> play #%d %s\n", row, col, idx, gifList[idx].name);
    runPlayer(); // blocking; returns here via drawMenu() when the user exits
  } else {
    Serial.printf("[ACTION] Menu: grid cell (row=%d col=%d) -> no-op (idx=%d out of range)\n", row, col, idx);
  }
}

// ──────────────────────────────────────────────
// Settings screen
// ──────────────────────────────────────────────
enum SettingsRow {
  SET_TOUCH_HELP, SET_BRIGHTNESS, SET_SLIDESHOW, SET_CALIBRATE,
  SET_WIFI, SET_SD, SET_BACK, SET_ROW_COUNT
};

void clearSdGifs() {
  if (!sdAvailable) return;
  File d = SD.open(SCAN_DIR);
  while (true) {
    File f = d.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String path = String(SCAN_DIR) + "/" + String(f.name());
      f.close();
      SD.remove(path.c_str());
    } else {
      f.close();
    }
  }
  d.close();
  scanGifs();
}

// LittleFS counterpart to clearSdGifs() — clears only the uploaded
// fallback GIFs, not the SD card (entirely separate storage). Doesn't
// touch settingDefaultGifRemoved; the built-in GIF's visibility is only
// ever changed by explicitly "deleting" it via /delete.
void clearLfsGifs() {
  if (!LittleFS.exists(UPLOADS_DIR)) return;
  File d = LittleFS.open(UPLOADS_DIR);
  while (true) {
    File f = d.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String path = String(UPLOADS_DIR) + "/" + String(f.name());
      f.close();
      LittleFS.remove(path.c_str());
    } else {
      f.close();
    }
  }
  d.close();
  scanGifs();
}

// Waits for a single raw touch-and-release, ignoring the normal
// pollTap()/toScreen() pipeline entirely — calibration has to work with
// whatever the CURRENT (possibly wrong) calibration is, so it reads the
// XPT2046 directly instead.
void waitForRawTap(int *rawX, int *rawY) {
  while (!ts.touched()) delay(10);
  TS_Point p = ts.getPoint();
  // Swapped to match toScreen(): "rawX" here means "the raw reading that
  // tracks screen X" (this panel's controller Y axis), and vice versa.
  *rawX = p.y;
  *rawY = p.x;
  while (ts.touched()) delay(10); // wait for release before returning
  delay(200); // debounce before the next target can be tapped
}

// Two-point linear calibration: show a crosshair at two known screen
// positions (inset from the edges — tapping the exact bezel is unreliable),
// record the raw ADC reading at each, then extrapolate what the reading
// would be at screen edges 0 and (NATIVE_W/H - 1).
void runTouchCalibration() {
  tft.setRotation(0);
  const int targetX[2] = {30, NATIVE_W - 30};
  const int targetY[2] = {30, NATIVE_H - 30};
  int rawX[2], rawY[2];

  for (int i = 0; i < 2; i++) {
    tft.fillScreen(TFT_BLACK);
    drawStr("Touch Calibration", 10, 100, 2, TFT_WHITE, TFT_BLACK);
    drawStr(i == 0 ? "Tap the crosshair (1/2)" : "Tap the crosshair (2/2)",
            10, 130, 1, TFT_YELLOW, TFT_BLACK);
    int tx = targetX[i], ty = targetY[i];
    tft.drawFastHLine(tx - 12, ty, 24, TFT_RED);
    tft.drawFastVLine(tx, ty - 12, 24, TFT_RED);
    tft.drawCircle(tx, ty, 8, TFT_RED);

    waitForRawTap(&rawX[i], &rawY[i]);
  }

  // Extrapolate raw ADC value at screen edge 0 and edge (max-1) from the
  // two (screen, raw) sample points.
  int32_t newX0   = rawX[0] + (int32_t)((0 - targetX[0]) * (float)(rawX[1] - rawX[0]) / (targetX[1] - targetX[0]));
  int32_t newXMax = rawX[0] + (int32_t)(((NATIVE_W - 1) - targetX[0]) * (float)(rawX[1] - rawX[0]) / (targetX[1] - targetX[0]));
  int32_t newY0   = rawY[0] + (int32_t)((0 - targetY[0]) * (float)(rawY[1] - rawY[0]) / (targetY[1] - targetY[0]));
  int32_t newYMax = rawY[0] + (int32_t)(((NATIVE_H - 1) - targetY[0]) * (float)(rawY[1] - rawY[0]) / (targetY[1] - targetY[0]));

  // Reject a bad result (e.g. both taps landing at nearly the same spot,
  // or extrapolating past the ADC's real range) instead of saving a
  // calibration that makes every tap on an axis land on the same clamped
  // value — that would silently break touch everywhere, with no way back
  // to this screen to fix it if the broken axis was Y.
  Serial.printf("[CAL] raw samples: (%d,%d) (%d,%d) -> extrapolated X0=%d Xmax=%d Y0=%d Ymax=%d\n",
                rawX[0], rawY[0], rawX[1], rawY[1], newX0, newXMax, newY0, newYMax);

  if (!calibrationLooksValid(newX0, newXMax, newY0, newYMax)) {
    Serial.println("[CAL] FAILED validation — keeping previous calibration");
    tft.fillScreen(TFT_BLACK);
    drawStr("Calibration failed", 10, 130, 2, TFT_RED, TFT_BLACK);
    drawStr("Tap the crosshairs precisely", 10, 160, 1, TFT_YELLOW, TFT_BLACK);
    delay(2000);
    return; // keep whatever calibration was already in effect
  }

  calRawXAtScreen0 = newX0;
  calRawXAtScreenMax = newXMax;
  calRawYAtScreen0 = newY0;
  calRawYAtScreenMax = newYMax;
  saveSettings();
  Serial.println("[CAL] SAVED");

  tft.fillScreen(TFT_BLACK);
  drawStr("Calibration saved!", 20, 140, 2, TFT_GREEN, TFT_BLACK);
  delay(1200);
}

void drawSettings() {
  currentScreen = SCREEN_SETTINGS;
  if (tft.getRotation() != 0) {
    tft.setRotation(0);
  }
  int W = tft.width(), H = tft.height();
  fillR(0, 0, W, H, TFT_BLACK);
  fillR(0, 0, W, TOP_BAR, TFT_BLUE);
  drawStr("Settings", 4, 9, 2, TFT_WHITE, TFT_BLUE);

  int rowH = (H - TOP_BAR) / SET_ROW_COUNT;
  char buf[40];

  for (int i = 0; i < SET_ROW_COUNT; i++) {
    int y = TOP_BAR + i * rowH;
    fillR(0, y, W, rowH, TFT_BLACK);
    tft.drawFastHLine(0, y, W, TFT_DARKGREY);

    const char *label = "";
    switch (i) {
      case SET_TOUCH_HELP:
        label = "Touch Controls";
        snprintf(buf, sizeof(buf), "Tap to view");
        break;
      case SET_BRIGHTNESS:
        label = "Brightness";
        snprintf(buf, sizeof(buf), "%d%%", settingBrightness);
        break;
      case SET_SLIDESHOW:
        label = "Slideshow";
        if (settingSlideshowSec == 0) snprintf(buf, sizeof(buf), "Off (single GIF)");
        else snprintf(buf, sizeof(buf), "Every %us", settingSlideshowSec);
        break;
      case SET_CALIBRATE:
        label = "Touch Calibration";
        snprintf(buf, sizeof(buf), "Tap to recalibrate");
        break;
      case SET_WIFI:
        label = "Wi-Fi";
        if (!settingWifiEnabled) {
          snprintf(buf, sizeof(buf), "Disabled - tap to enable");
        } else if (WiFi.isConnected()) {
          snprintf(buf, sizeof(buf), "%s / %s.local",
                   WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
        } else {
          snprintf(buf, sizeof(buf), "Not connected - tap to disable");
        }
        break;
      case SET_SD: {
        label = "SD card";
        uint32_t freeMB = sdAvailable ? (uint32_t)((SD.totalBytes() - SD.usedBytes()) >> 20) : 0;
        snprintf(buf, sizeof(buf), "%d GIFs, %uMB free", gifCount, freeMB);
        break;
      }
      case SET_BACK:
        label = "< Back to menu";
        buf[0] = '\0';
        break;
    }

    drawStr(label, 8, y + rowH / 2 - 9, 1, i == SET_BACK ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
    if (buf[0]) {
      drawStr(buf, 8, y + rowH / 2 + 3, 1, TFT_CYAN, TFT_BLACK);
    }
  }
}

static const char *settingsRowName(int row) {
  switch (row) {
    case SET_TOUCH_HELP: return "TOUCH_HELP";
    case SET_BRIGHTNESS: return "BRIGHTNESS";
    case SET_SLIDESHOW:  return "SLIDESHOW";
    case SET_CALIBRATE:  return "CALIBRATE";
    case SET_WIFI:       return "WIFI";
    case SET_SD:         return "SD";
    case SET_BACK:       return "BACK";
    default:             return "?";
  }
}

void handleSettingsTap(int sx, int sy) {
  int H = tft.height();
  if (sy < TOP_BAR) {
    Serial.println("[ACTION] Settings: top bar -> no-op");
    return;
  }

  int rowH = (H - TOP_BAR) / SET_ROW_COUNT;
  // Integer-division rowH can leave a few leftover pixels at the bottom
  // of the screen unassigned to any row (e.g. 288/7 truncates to 41,
  // leaving row index 7 reachable at sy=319) — clamp instead of rejecting,
  // so the very edge of the screen still resolves to the last real row.
  int row = constrain((sy - TOP_BAR) / rowH, 0, SET_ROW_COUNT - 1);

  Serial.printf("[ACTION] Settings: row=%d (%s)\n", row, settingsRowName(row));

  // SD clear-all and Wi-Fi credential reset were removed from here — both
  // are single, easy-to-misclick taps on a touchscreen with no real undo
  // (a credential reset forces re-provisioning; it isn't reachable over
  // the web UI anymore once it fires, unlike SD clear). If you actually
  // need to re-provision Wi-Fi, do it by physically holding down the
  // boot/reset combination or reflashing — not from this screen. Toggling
  // Wi-Fi on/off (below) is a different, safe, fully reversible action —
  // it doesn't touch saved credentials at all.
  if (row == SET_SD) return;

  switch (row) {
    case SET_TOUCH_HELP:
      drawTouchHelp();
      break;
    case SET_WIFI:
      settingWifiEnabled = !settingWifiEnabled;
      saveSettings();
      Serial.printf("[WIFI] %s — restarting to apply\n", settingWifiEnabled ? "enabled" : "disabled");
      tft.fillScreen(TFT_BLACK);
      drawStr(settingWifiEnabled ? "Enabling Wi-Fi..." : "Disabling Wi-Fi...", 10, 140, 1, TFT_YELLOW, TFT_BLACK);
      delay(400);
      ESP.restart();
      break;
    case SET_BRIGHTNESS: {
      static const uint8_t levels[] = {100, 75, 50, 25, 10};
      int cur = 0;
      for (int i = 0; i < 5; i++) if (levels[i] == settingBrightness) cur = i;
      settingBrightness = levels[(cur + 1) % 5];
      saveSettings();
      applyBrightness();
      drawSettings();
      break;
    }
    case SET_SLIDESHOW: {
      static const uint16_t opts[] = {0, 5, 10, 30, 60};
      int cur = 0;
      for (int i = 0; i < 5; i++) if (opts[i] == settingSlideshowSec) cur = i;
      settingSlideshowSec = opts[(cur + 1) % 5];
      saveSettings();
      drawSettings();
      break;
    }
    case SET_CALIBRATE:
      runTouchCalibration();
      drawSettings();
      break;
    case SET_BACK:
      drawMenu();
      break;
  }
}

// Purely informational — shows what each invisible touch quadrant does
// during playback (see runPlayer()'s quadrant handling) without needing
// to actually be playing something to find out. Only "< Back to Settings"
// at the bottom is tappable; the quadrant illustration itself is static.
void drawTouchHelp() {
  currentScreen = SCREEN_TOUCH_HELP;
  if (tft.getRotation() != 0) tft.setRotation(0);
  int W = tft.width(), H = tft.height();
  fillR(0, 0, W, H, TFT_BLACK);

  fillR(0, 0, W, TOP_BAR, TFT_BLUE);
  drawStr("Touch Controls", 4, 9, 2, TFT_WHITE, TFT_BLUE);

  int gridTop = TOP_BAR;
  int gridBottom = H - BOTTOM_BAR;
  int midY = gridTop + (gridBottom - gridTop) / 2;
  int midX = W / 2;

  tft.drawFastHLine(0, midY, W, TFT_DARKGREY);
  tft.drawFastVLine(midX, gridTop, gridBottom - gridTop, TFT_DARKGREY);

  drawStr("Back to", 14, gridTop + 24, 1, TFT_WHITE, TFT_BLACK);
  drawStr("Menu", 14, gridTop + 38, 1, TFT_WHITE, TFT_BLACK);

  drawStr("Next", midX + 14, gridTop + 24, 1, TFT_WHITE, TFT_BLACK);
  drawStr("GIF", midX + 14, gridTop + 38, 1, TFT_WHITE, TFT_BLACK);

  drawStr("Rotate", 14, midY + 24, 1, TFT_WHITE, TFT_BLACK);

  drawStr("Brightness", midX + 14, midY + 24, 1, TFT_WHITE, TFT_BLACK);

  fillR(0, gridBottom, W, BOTTOM_BAR, TFT_BLACK);
  tft.drawFastHLine(0, gridBottom, W, TFT_DARKGREY);
  drawStr("< Back to Settings", 8, gridBottom + BOTTOM_BAR / 2 - 4, 1, TFT_YELLOW, TFT_BLACK);
}

void handleTouchHelpTap(int sx, int sy) {
  int H = tft.height();
  if (sy >= H - BOTTOM_BAR) {
    Serial.println("[ACTION] TouchHelp: back to settings");
    drawSettings();
  }
}

// ──────────────────────────────────────────────
// Provisioning HTML
// ──────────────────────────────────────────────
const char provHTML[] PROGMEM =
  "<!DOCTYPE html><html><head>"
  "<meta charset=\"utf-8\">"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>CYD Setup</title>"
  "<style>"
  "*{margin:0;padding:0;box-sizing:border-box}"
  "body{font-family:system-ui,sans-serif;background:#0f3460;color:#eee;"
  "display:flex;justify-content:center;align-items:center;"
  "min-height:100vh;padding:20px}"
  ".card{background:#16213e;border-radius:16px;padding:30px;"
  "width:100%;max-width:340px;box-shadow:0 4px 20px rgba(0,0,0,.4)}"
  "h1{text-align:center;margin-bottom:6px}"
  ".sub{text-align:center;color:#aaa;margin-bottom:20px;font-size:.9em}"
  "label{display:block;font-size:.85em;color:#999;margin-bottom:4px}"
  "input{width:100%;padding:12px;margin-bottom:16px;"
  "border:2px solid #0a2a4a;border-radius:8px;"
  "background:#1a1a2e;color:#eee;font-size:1em}"
  "input:focus{outline:none;border-color:#667eea}"
  "button{width:100%;padding:14px;border:none;border-radius:8px;"
  "background:#667eea;color:#fff;font-size:1em;"
  "font-weight:600;cursor:pointer}"
  "button:active{background:#5568d3}"
  "#msg{text-align:center;margin-top:14px;min-height:22px;font-size:.9em}"
  ".nets{max-height:180px;overflow-y:auto;margin-bottom:16px;"
  "border:2px solid #0a2a4a;border-radius:8px}"
  ".net{display:flex;justify-content:space-between;align-items:center;"
  "padding:10px 12px;cursor:pointer;border-bottom:1px solid #0a2a4a;"
  "font-size:.92em}"
  ".net:last-child{border-bottom:none}"
  ".net:active,.net.sel{background:#0a2a4a}"
  ".net .ssid{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
  ".net .meta{color:#888;font-size:.85em;flex-shrink:0;margin-left:8px}"
  ".scanning{padding:14px;text-align:center;color:#888;font-size:.9em}"
  ".rescan{background:none;border:none;color:#667eea;font-size:.85em;"
  "width:auto;padding:0 0 16px;cursor:pointer;display:block;"
  "margin:0 auto 16px;text-decoration:underline}"
  "</style></head><body><div class=\"card\">"
  "<h1>CYD Wi-Fi Setup</h1>"
  "<p class=\"sub\">Pick a network or enter one manually</p>"
  "<div id=\"nets\" class=\"nets\"><div class=\"scanning\">Scanning...</div></div>"
  "<button type=\"button\" class=\"rescan\" id=\"rescan\">Rescan</button>"
  "<form id=\"f\">"
  "<label>Wi-Fi SSID</label>"
  "<input id=\"s\" name=\"ssid\" required autofocus>"
  "<label>Password</label>"
  "<input id=\"p\" name=\"pass\" type=\"password\">"
  "<button type=\"submit\">Save &amp; Connect</button>"
  "</form><div id=\"msg\"></div></div>"
  "<script>"
  "function esc(s){return s.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\")"
  ".replace(/>/g,\"&gt;\").replace(/\\\"/g,\"&quot;\");}"
  "function bars(r){return r>-60?\"\\u2582\\u2584\\u2586\\u2588\":"
  "r>-70?\"\\u2582\\u2584\\u2586\":r>-80?\"\\u2582\\u2584\":\"\\u2582\";}"
  "function scan(){"
  "var el=document.getElementById(\"nets\");"
  "el.innerHTML=\"<div class=scanning>Scanning...</div>\";"
  "poll();"
  "}"
  "function poll(){"
  "var el=document.getElementById(\"nets\");"
  "fetch(\"/scan\").then(function(r){return r.json()}).then(function(d){"
  "if(d.status===\"scanning\"){setTimeout(poll,700);return;}"
  "if(!d.networks||!d.networks.length){"
  "el.innerHTML=\"<div class=scanning>No networks found</div>\";return;}"
  "el.innerHTML=d.networks.map(function(n){"
  "var safe=esc(n.ssid);"
  "return \"<div class=net data-ssid=\\\"\"+safe+\"\\\">"
  "<span class=ssid>\"+safe+\"</span>"
  "<span class=meta>\"+(n.secure?\"\\ud83d\\udd12 \":\"\")+bars(n.rssi)+\"</span></div>\";"
  "}).join(\"\");"
  "}).catch(function(){"
  "el.innerHTML=\"<div class=scanning>Scan failed</div>\";"
  "});"
  "}"
  "document.getElementById(\"nets\").addEventListener(\"click\",function(e){"
  "var row=e.target.closest(\".net\");"
  "if(!row)return;"
  "document.querySelectorAll(\".net.sel\").forEach(function(x){x.classList.remove(\"sel\")});"
  "row.classList.add(\"sel\");"
  "document.getElementById(\"s\").value=row.dataset.ssid;"
  "document.getElementById(\"p\").focus();"
  "});"
  "document.getElementById(\"rescan\").addEventListener(\"click\",scan);"
  "document.getElementById(\"f\").addEventListener(\"submit\",function(e){"
  "e.preventDefault();"
  "var s=document.getElementById(\"s\").value,"
  "p=document.getElementById(\"p\").value,"
  "msg=document.getElementById(\"msg\");"
  "fetch(\"/provision\",{"
  "method:\"POST\",body:new FormData(document.getElementById(\"f\"))}"
  ").then(function(r){return r.json()})"
  ".then(function(d){"
  "msg.innerHTML=d.ok?"
  "\"<span style=color:#4f4>Connected! Rebooting...</span>\":"
  "\"<span style=color:#f66>Failed</span>\";"
  "if(d.ok)setTimeout(function(){location.href=\"/\"},2000);"
  "}).catch(function(){msg.innerHTML=\"<span style=color:#f66>Err</span>\";});"
  "});"
  "scan();"
  "</script></body></html>";

// ──────────────────────────────────────────────
// Upload handler
// ──────────────────────────────────────────────
static File uploadFileHandle;
static uint32_t uploadTotal = 0;

static String uploadPath;

// Decided once per upload (at idx==0) from sdAvailable at that moment,
// then stuck to for the rest of that upload even if sdAvailable somehow
// changes mid-transfer — the two backends below are never mixed within
// a single upload.
static bool uploadToSD = true;

// ESPAsyncWebServer calls onUpload() incrementally as file data streams in
// (across possibly many calls for one request), but the actual HTTP
// response has to be sent from the *request* handler registered alongside
// it (see the "/upload" route below) — that's the one guaranteed to run
// exactly once, after the whole request (including the file) is done.
// So onUpload() only records what happened here; it never calls
// req->send() itself. uploadError empty == success. This used to be unset
// on every failure path (SD unavailable, SD.open() failure, a partial/
// failed write), which meant the request handler always reported success
// regardless of what actually happened — the real cause of "uploads
// silently fail" without any error surfacing to the browser.
String uploadError = "";

// Writes the upload straight to the SD card, under SCAN_DIR.
static void onUploadSD(const String &fname, size_t idx, uint8_t *data, size_t len, bool final) {
  if (!idx) {
    if (uploadFileHandle) uploadFileHandle.close();
    uploadPath = String(SCAN_DIR) + "/upload_" + String(millis()) + ".gif";
    uploadFileHandle = SD.open(uploadPath.c_str(), FILE_WRITE);
    uploadTotal = 0;
    Serial.printf("[UPLOAD/SD] %s\n", fname.c_str());
    if (!uploadFileHandle) {
      Serial.println("[UPLOAD/SD] Failed to open file for writing");
      uploadError = "Could not open file on SD card";
      return;
    }
  }
  if (!uploadFileHandle) return; // already rejected earlier in this same upload
  if (len > 0) {
    uploadTotal += len;
    size_t written = uploadFileHandle.write(data, len);
    if (written != len) {
      Serial.printf("[UPLOAD/SD] Write failed (%u of %u bytes)\n", (unsigned)written, (unsigned)len);
      uploadFileHandle.close();
      uploadFileHandle = File();
      SD.remove(uploadPath.c_str());
      uploadError = "SD write failed";
      return;
    }
    if (uploadTotal > MAX_UPLOAD_BYTES) {
      uploadFileHandle.close();
      uploadFileHandle = File();
      SD.remove(uploadPath.c_str());
      uploadError = "File too large (max 8 MB)";
      return;
    }
  }
  if (final) {
    uploadFileHandle.close();
    uploadFileHandle = File();
    Serial.printf("[UPLOAD/SD] %s (%d bytes)\n", fname.c_str(), uploadTotal);
  }
}

// Writes the upload into LittleFS's UPLOADS_DIR — the no-SD fallback,
// completely separate storage from onUploadSD() above. Capped at
// LFS_UPLOAD_CAP_BYTES total across everything already in that folder
// (not per file), since it shares the flash chip with the web UI itself.
static uint32_t uploadLfsUsedAtStart = 0;
static void onUploadLFS(const String &fname, size_t idx, uint8_t *data, size_t len, bool final) {
  if (!idx) {
    if (uploadFileHandle) uploadFileHandle.close();
    if (!LittleFS.exists(UPLOADS_DIR)) LittleFS.mkdir(UPLOADS_DIR);
    uploadPath = String(UPLOADS_DIR) + "/upload_" + String(millis()) + ".gif";
    uploadLfsUsedAtStart = lfsUploadsUsedBytes();
    uploadFileHandle = LittleFS.open(uploadPath.c_str(), FILE_WRITE);
    uploadTotal = 0;
    Serial.printf("[UPLOAD/LFS] %s\n", fname.c_str());
    if (!uploadFileHandle) {
      Serial.println("[UPLOAD/LFS] Failed to open file for writing");
      uploadError = "Could not open file in flash storage";
      return;
    }
  }
  if (!uploadFileHandle) return; // already rejected earlier in this same upload
  if (len > 0) {
    uploadTotal += len;
    size_t written = uploadFileHandle.write(data, len);
    if (written != len) {
      Serial.printf("[UPLOAD/LFS] Write failed (%u of %u bytes)\n", (unsigned)written, (unsigned)len);
      uploadFileHandle.close();
      uploadFileHandle = File();
      LittleFS.remove(uploadPath.c_str());
      uploadError = "Flash write failed";
      return;
    }
    if (uploadLfsUsedAtStart + uploadTotal > LFS_UPLOAD_CAP_BYTES) {
      uploadFileHandle.close();
      uploadFileHandle = File();
      LittleFS.remove(uploadPath.c_str());
      uploadError = "Storage limit reached (500KB, no SD card) - delete a GIF first";
      return;
    }
  }
  if (final) {
    uploadFileHandle.close();
    uploadFileHandle = File();
    Serial.printf("[UPLOAD/LFS] %s (%d bytes)\n", fname.c_str(), uploadTotal);
  }
}

void onUpload(AsyncWebServerRequest *req, const String &fname, size_t idx,
              uint8_t *data, size_t len, bool final) {
  if (!idx) {
    uploadError = "";
    uploadToSD = sdAvailable;
  }
  if (uploadError.length()) return; // already failed earlier in this same upload

  if (uploadToSD) onUploadSD(fname, idx, data, len, final);
  else onUploadLFS(fname, idx, data, len, final);
}

// ──────────────────────────────────────────────
// JSON helpers
// ──────────────────────────────────────────────
const char* buildStatusJSON() {
  static char buf[256];
  // In no-SD mode, "free" means what's left of the fixed upload budget,
  // not the LittleFS partition's real free space (most of which is the
  // web UI's own assets, not available for GIFs regardless).
  uint32_t freeKB = sdAvailable
    ? (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024)
    : (LFS_UPLOAD_CAP_BYTES - min(lfsUploadsUsedBytes(), (uint32_t)LFS_UPLOAD_CAP_BYTES)) / 1024;
  snprintf(buf, sizeof(buf),
    "{\"file\":\"%s\",\"rssi\":%d,\"playing\":%s,\"gifs\":%d,\"freeKB\":%u,\"sd\":%s}",
    currentFN,
    WiFi.isConnected() ? WiFi.RSSI() : 0,
    playing ? "true" : "false",
    gifCount,
    freeKB,
    sdAvailable ? "true" : "false");
  return buf;
}

// ──────────────────────────────────────────────
// Setup
// ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== CYD GIF Player ===");

  loadSettings();

  // TFT
  tft.init();
  tft.setRotation(settingRotation);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // Only affects pushPixels() (used for GIF frames), not drawPixel() —
  // matches AnimatedGIF's little-endian RGB565 palette.
  tft.setSwapBytes(true);

  // Backlight — PWM instead of the library's default on/off drive, so
  // brightness can actually be adjusted (and turned fully off).
  ledcSetup(BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_LEDC_FREQ, BACKLIGHT_LEDC_RES);
  ledcAttachPin(TFT_BL, BACKLIGHT_LEDC_CHANNEL);
  applyBrightness();

  // Touch SPI (own bus, separate from SD)
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  Serial.println("Touch OK");

  // Escape hatch: press (not hold-through-reset!) the BOOT button within
  // the next couple seconds to force touch calibration, bypassing the
  // Settings menu entirely. GPIO0 is the ESP32's UART-download strapping
  // pin — holding it low during the actual reset pulse puts the chip in
  // the ROM bootloader instead of running this firmware at all, so this
  // window only opens now, safely after app code is already executing.
  //
  // A short press (release before WIFI_RESET_HOLD_MS) triggers touch
  // calibration, same as always. Holding it through WIFI_RESET_HOLD_MS
  // instead wipes saved Wi-Fi credentials — deliberately a hold, not a
  // tap, matching why this isn't a Settings-menu button (see the
  // handleSettingsTap() comment on SET_SD): a credential reset forces
  // re-provisioning with no undo, so it needs a harder-to-misfire trigger
  // than a single touchscreen tap. Only the Wi-Fi NVS namespace is
  // cleared — calibration/brightness/etc. are untouched.
  #define WIFI_RESET_HOLD_MS 3000
  pinMode(0, INPUT_PULLUP);
  tft.fillScreen(TFT_BLACK);
  drawStr("CYD GIF Player", 10, 40, 2, TFT_WHITE, TFT_BLACK);
  drawStr("Press BOOT now for", 10, 90, 1, TFT_YELLOW, TFT_BLACK);
  drawStr("touch calibration...", 10, 105, 1, TFT_YELLOW, TFT_BLACK);
  drawStr("(hold 3s to reset Wi-Fi)", 10, 120, 1, TFT_DARKGREY, TFT_BLACK);
  bool calRequested = false;
  bool wifiResetRequested = false;
  uint32_t pressStart = 0;
  for (uint32_t waitStart = millis();
       millis() - waitStart < 2500 || pressStart != 0;
       delay(20)) {
    bool down = digitalRead(0) == LOW;
    if (down && pressStart == 0) {
      pressStart = millis();
    } else if (down && millis() - pressStart >= WIFI_RESET_HOLD_MS) {
      wifiResetRequested = true;
      break;
    } else if (!down && pressStart != 0) {
      calRequested = true; // released before reaching the hold threshold
      break;
    }
  }
  if (wifiResetRequested) {
    Serial.println("[BOOT] BOOT held 3s -> resetting Wi-Fi credentials");
    tft.fillScreen(TFT_BLACK);
    drawStr("Wi-Fi credentials", 10, 90, 1, TFT_YELLOW, TFT_BLACK);
    drawStr("cleared. Restarting...", 10, 105, 1, TFT_YELLOW, TFT_BLACK);
    resetWifiCredentials();
    delay(1000);
    ESP.restart();
  }
  if (calRequested) {
    Serial.println("[BOOT] BOOT pressed post-boot -> forcing touch calibration");
    runTouchCalibration();
  }

  // AnimatedGIF must be initialized once before any gif.open() calls —
  // begin() resets its internal state, wiping out whatever open() set up.
  gif.begin();
  thumbGif.begin();

  // LittleFS
  if (!LittleFS.begin(true)) Serial.println("LittleFS: skip");

  // SD (own bus, separate from touch; optional — falls back to a
  // built-in GIF from flash if absent)
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  refreshSdAvailable();
  if (!sdAvailable) {
    Serial.println("SD FAIL - continuing with built-in default GIF");
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.println("No SD card");
    tft.println("Using built-in GIF");
    delay(1200);
  } else {
    Serial.printf("SD OK (%u MB)\n", (SD.totalBytes() - SD.usedBytes()) >> 20);
    if (!SD.exists(SCAN_DIR)) {
      SD.mkdir(SCAN_DIR);
      Serial.println("[SCAN] Created " SCAN_DIR);
    }
  }

  // ── Scan GIFs ── unconditional and first — nothing about GIF
  // playback depends on Wi-Fi, and scanning here keeps it that way
  // regardless of which branch below runs.
  scanGifs();

  if (!settingWifiEnabled) {
    // WIFI_OFF fully tears down the underlying lwIP TCP/IP stack that
    // AsyncTCP/ESPAsyncWebServer needs — registering routes or calling
    // httpServer.begin() afterward crashes with "Invalid mbox" and
    // reboots (which then hits this same disabled state and crashes
    // again, forever). So when Wi-Fi is off, skip the web server
    // entirely for this boot too, not just the connection attempt.
    Serial.println("Mode: WIFI DISABLED");
    WiFi.mode(WIFI_OFF);
    tft.fillScreen(TFT_BLACK);
    drawStr("CYD GIF Player", 4, 30, 2, TFT_WHITE, TFT_BLACK);
    drawStr("Wi-Fi disabled", 4, 70, 2, TFT_YELLOW, TFT_BLACK);
    drawStr("(enable in Settings)", 4, 105, 1, TFT_WHITE, TFT_BLACK);
    delay(1000);
  } else {
  // ── Load Wi-Fi creds ──
  loadWifiCreds();

  if (!hasCredentials) {
    Serial.println("Mode: PROVISIONING");
    provisionStart = millis();

    // AP_STA, not plain AP — scanning for nearby networks (below) needs
    // the station radio active too, even though we never connect it to
    // anything ourselves during provisioning.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(PROVISION_AP_SSID, "", 6, 0, 4);
    delay(500);

    tft.fillScreen(TFT_BLACK);
    drawStr("CYD GIF Player", 4, 30, 2, TFT_WHITE, TFT_BLACK);
    drawStr("Setup Mode", 4, 70, 2, TFT_YELLOW, TFT_BLACK);
    drawStr("Connect to:", 4, 120, 1, TFT_CYAN, TFT_BLACK);
    drawStr(PROVISION_AP_SSID, 4, 145, 2, TFT_GREEN, TFT_BLACK);
    drawStr("Visit 192.168.4.1", 4, 190, 1, TFT_WHITE, TFT_BLACK);

    // Captive portal: redirect every DNS query to our own IP, and answer
    // every HTTP request (not just "/") with the provisioning page. That
    // combination is what makes Android/iOS/Windows/macOS auto-detect a
    // captive portal and pop the setup page on their own — those OSes probe
    // a handful of well-known URLs (e.g. connectivitycheck.gstatic.com,
    // captive.apple.com) right after joining, and only trigger the popup
    // when the response isn't what they expect for "real" internet access.
    // Without this, the AP works exactly the same, just without the popup —
    // visiting 192.168.4.1 manually (as the screen still says) always did.
    DNSServer dnsServer;
    dnsServer.start(53, "*", WiFi.softAPIP());

    httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html", provHTML);
    });
    httpServer.on("/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
      // WiFi.scanNetworks() (blocking) is a several-second full-channel
      // active scan — calling it directly from an AsyncWebServer handler
      // blocks the async task for that whole time, which is long enough
      // to trip the task watchdog and reboot the chip. scanNetworks(true)
      // (async) kicks the scan off and returns immediately instead; the
      // client polls this same endpoint until scanComplete() says done.
      int16_t n = WiFi.scanComplete();
      if (n == WIFI_SCAN_FAILED) { // no scan running or queued yet — start one
        WiFi.scanNetworks(true);
        req->send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
      }
      if (n == WIFI_SCAN_RUNNING) {
        req->send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
      }

      String json = "{\"status\":\"done\",\"networks\":[";
      bool first = true;
      for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue; // hidden network — nothing useful to show
        bool dup = false;
        for (int j = 0; j < i; j++) { if (WiFi.SSID(j) == ssid) { dup = true; break; } }
        if (dup) continue; // same network seen on multiple channels/APs — keep the first (strongest, since scan results are RSSI-sorted)
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        if (!first) json += ",";
        first = false;
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
      }
      json += "]}";
      WiFi.scanDelete();
      req->send(200, "application/json", json);
    });
    httpServer.on("/provision", HTTP_POST, [](AsyncWebServerRequest *req) {
      String ssid = req->arg("ssid");
      String pass = req->arg("pass");
      if (ssid.length() == 0) {
        req->send(400, "application/json",
          "{\"ok\":false,\"error\":\"Empty SSID\"}");
        return;
      }
      saveWifiCreds(ssid.c_str(), pass.c_str());
      WiFi.softAPdisconnect(true);
      req->send(200, "application/json", "{\"ok\":true}");
      delay(200);
      ESP.restart();
    });
    httpServer.onNotFound([](AsyncWebServerRequest *req) {
      req->send(200, "text/html", provHTML);
    });
    httpServer.begin();
    Serial.printf("AP %s  →  192.168.4.1\n", PROVISION_AP_SSID);

    while (millis() - provisionStart < PROVISION_AP_TIMEOUT_MS) {
      dnsServer.processNextRequest();
      delay(10);
    }

    Serial.println("[PROV] Timeout → retrying saved creds");
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    loadWifiCreds();
    if (connectWiFi(wifiSSID, wifiPass, 10000)) {
      Serial.println("Connected on retry");
    } else {
      Serial.println("Retry failed, restarting");
      ESP.restart();
    }
  } else {
    Serial.printf("Mode: STATION  SSID=%s\n", wifiSSID);
    tft.fillScreen(TFT_BLACK);
    drawStr("CYD GIF Player", 4, 30, 2, TFT_WHITE, TFT_BLACK);
    drawStr("Connecting...", 4, 70, 2, TFT_YELLOW, TFT_BLACK);

    if (!connectWiFi(wifiSSID, wifiPass, 10000)) {
      // The saved network just isn't reachable right now (device carried
      // somewhere else, router down, etc.) — that used to restart the
      // device, which retried the same connect-and-fail on every boot
      // forever, never once reaching the menu. Credentials are still good
      // for later, so just carry on without the network instead: SD
      // playback and touch don't need it at all, and the ESP32 WiFi stack
      // keeps retrying this connection in the background on its own, so it
      // can still come up reachable later in this same session if the
      // network reappears.
      Serial.println("Connect failed — continuing offline");
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      drawStr("Offline mode", 4, 70, 2, TFT_YELLOW, TFT_BLACK);
      drawStr("(saved Wi-Fi not found)", 4, 105, 1, TFT_WHITE, TFT_BLACK);
      delay(1200);
    } else {
      drawStr("Connected!", 4, 70, 2, TFT_GREEN, TFT_BLACK);
      drawStr(WiFi.localIP().toString().c_str(), 4, 105, 1, TFT_WHITE, TFT_BLACK);
      drawStr("Loading...", 4, 140, 1, TFT_YELLOW, TFT_BLACK);
      Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    }
  }

  // ── mDNS ── reachable as http://cydgifplayer.local/ instead of the raw
  // IP, on networks/OSes with mDNS (Bonjour/Avahi) support — most desktop
  // and mobile OSes do out of the box.
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_HOSTNAME);
  } else {
    Serial.println("mDNS: failed to start");
  }

  // ── Web server ──
  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (LittleFS.exists("/index.html")) {
      req->send(LittleFS, "/index.html", "text/html");
    } else {
      String html = "<html><body style=\"background:#000;color:#fff;"
        "font-family:sans-serif;padding:20px\">"
        "<h1>CYD GIF Player</h1>"
        "<p>Upload a GIF:</p>"
        "<input type=\"file\" accept=\".gif\" id=\"g\""
        " onchange=\"go(this)\">"
        "<p id=\"s\"></p>"
        "<script>function go(el){"
        "var f=el.files[0];if(!f)return;"
        "document.getElementById(\"s\").textContent=\"Uploading...\";"
        "var x=new XMLHttpRequest();"
        "x.open(\"POST\",\"/upload\",true);x.send(f);"
        "x.onload=function(){"
        "document.getElementById(\"s\").textContent="
        "x.status==200?\"Sent!\":\"Error \"+x.status}"
        "}</script></body></html>";
      req->send(200, "text/html", html);
    }
  });

  httpServer.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      // Runs once, after onUpload() has processed the whole file (or
      // bailed out early on it) — uploadError is what onUpload() actually
      // recorded, not a re-check of conditions that may have since changed.
      if (uploadError.length()) {
        int code = uploadError == "File too large (max 8 MB)" ? 413
                  : uploadError == "No SD card inserted" ? 503 : 500;
        r->send(code, "application/json",
          "{\"status\":\"error\",\"message\":\"" + uploadError + "\"}");
        return;
      }
      r->send(200, "application/json",
        "{\"status\":\"success\",\"bytes\":" + String(uploadTotal) + "}");
    },
    onUpload
  );

  httpServer.on("/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", buildStatusJSON());
  });

  httpServer.on("/browse", HTTP_GET, [](AsyncWebServerRequest *r) {
    // Serve from the in-memory list (kept fresh by periodic scanGifs()
    // calls in the main loop) rather than re-reading the SD card here —
    // this request handler runs on a separate async task, and opening a
    // second directory/file handle on the SD card while playback is
    // actively reading it from the main loop is a real race that can
    // silently return an empty listing.
    String json = "{\"count\":" + String(gifCount) + ",\"gifs\":[";
    for (int i = 0; i < gifCount; i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + String(gifList[i].name) + "\",\"size\":" + String((uint32_t)gifList[i].size) + "}";
    }
    json += "]}";
    r->send(200, "application/json", json);
  });

  httpServer.on("/next", HTTP_GET, [](AsyncWebServerRequest *r) {
    shouldQuit = true;
    r->send(200, "application/json", "{\"status\":\"cycling\"}");
  });

  httpServer.on("/clear", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (sdAvailable) clearSdGifs();
    else clearLfsGifs();
    if (currentScreen == SCREEN_MENU) drawMenu();
    Serial.println("[CLEAR] Removed all files");
    r->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  httpServer.on("/file", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("name")) {
      r->send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Missing name param\"}");
      return;
    }
    String fname = r->getParam("name")->value();
    if (fname.length() == 0 || fname.indexOf('/') >= 0 ||
        fname.indexOf('\\') >= 0 || fname.indexOf("..") >= 0) {
      r->send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
      return;
    }

    if (sdAvailable) {
      String path = String(SCAN_DIR) + "/" + fname;
      if (!SD.exists(path.c_str())) {
        r->send(404, "application/json",
          "{\"status\":\"error\",\"message\":\"File not found\"}");
        return;
      }
      AsyncWebServerResponse *response = r->beginResponse(SD, path, "image/gif");
      // Filenames on this device never change contents in place (upload
      // creates a new name, delete removes it) — safe for the browser to
      // cache aggressively and skip re-fetching on repeat file-manager visits.
      response->addHeader("Cache-Control", "public, max-age=604800, immutable");
      r->send(response);
      return;
    }

    // No-SD fallback: default.gif isn't a file at all — serve it straight
    // out of the flash byte array — everything else comes from LittleFS.
    if (fname == "default.gif") {
      AsyncWebServerResponse *response =
        r->beginResponse(200, "image/gif", (const uint8_t *)default_gif, default_gif_len);
      response->addHeader("Cache-Control", "public, max-age=604800, immutable");
      r->send(response);
      return;
    }
    String path = String(UPLOADS_DIR) + "/" + fname;
    if (!LittleFS.exists(path.c_str())) {
      r->send(404, "application/json",
        "{\"status\":\"error\",\"message\":\"File not found\"}");
      return;
    }
    AsyncWebServerResponse *response = r->beginResponse(LittleFS, path, "image/gif");
    response->addHeader("Cache-Control", "public, max-age=604800, immutable");
    r->send(response);
  });

  // Small fixed-size raw RGB565 thumbnail (first frame only) instead of
  // the full GIF — see myThumbCallback for why. Response body is exactly
  // THUMB_DIM*THUMB_DIM uint16_t pixels, row-major, no header.
  httpServer.on("/thumb", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("name")) {
      r->send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Missing name param\"}");
      return;
    }
    String fname = r->getParam("name")->value();
    if (fname.length() == 0 || fname.indexOf('/') >= 0 ||
        fname.indexOf('\\') >= 0 || fname.indexOf("..") >= 0) {
      r->send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
      return;
    }
    memset(thumbBuf, 0, sizeof(thumbBuf));
    if (openGifSmart(thumbGif, fname.c_str(), myThumbCallback)) {
      int delayMs = 0;
      thumbGif.playFrame(false, &delayMs, NULL);
      thumbGif.close();
    }
    AsyncResponseStream *response = r->beginResponseStream("application/octet-stream");
    response->addHeader("Cache-Control", "public, max-age=604800, immutable");
    response->write((const uint8_t *)thumbBuf, sizeof(thumbBuf));
    r->send(response);
  });

  httpServer.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("file")) {
      r->send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Missing file param\"}");
      return;
    }
    String fname = r->getParam("file")->value();
    // Reject anything that isn't a plain filename within /gifs.
    if (fname.length() == 0 || fname.indexOf('/') >= 0 ||
        fname.indexOf('\\') >= 0 || fname.indexOf("..") >= 0) {
      r->send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
      return;
    }
    // Deleting a file that's still open elsewhere (the main loop's active
    // decode holds its own File handle) is filesystem-dependent territory —
    // could just fail, could corrupt the in-progress playback, and there's
    // no mutex protecting the SD bus between this async task and the main
    // loop's reads regardless. Simplest safe answer: refuse.
    if (playing && fname == String(currentFN)) {
      r->send(409, "application/json",
        "{\"status\":\"error\",\"message\":\"Can't delete a GIF that's currently playing\"}");
      return;
    }

    bool ok;
    if (sdAvailable) {
      String path = String(SCAN_DIR) + "/" + fname;
      ok = SD.remove(path.c_str());
    } else if (fname == "default.gif") {
      // Not a real file — "deleting" it just hides it from the list.
      ok = !settingDefaultGifRemoved; // already-removed reads as "not found"
      settingDefaultGifRemoved = true;
      saveSettings();
    } else {
      String path = String(UPLOADS_DIR) + "/" + fname;
      ok = LittleFS.remove(path.c_str());
    }

    scanGifs();
    if (currentScreen == SCREEN_MENU) drawMenu();
    Serial.printf("[DELETE] %s -> %s\n", fname.c_str(), ok ? "ok" : "failed");
    if (ok) {
      r->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      r->send(404, "application/json",
        "{\"status\":\"error\",\"message\":\"File not found\"}");
    }
  });

  // Static assets for the web UI's client-side GIF re-encoder
  // (gif.js/gif.worker.js for encoding, gifuct.js for decoding).
  httpServer.serveStatic("/", LittleFS, "/");

  httpServer.begin();
  Serial.println("Web server ready (port 80)");
  } // else settingWifiEnabled

  // Show menu
  delay(500);
  scanGifs();
  drawMenu();
  Serial.println("=== READY ===");
}

// ──────────────────────────────────────────────
// Loop
// ──────────────────────────────────────────────
void loop() {
  int sx, sy;
  bool tapped = pollTap(&sx, &sy);
  if (tapped) Serial.printf("[TAP] sx=%d sy=%d screen=%d\n", sx, sy, (int)currentScreen);

  if (currentScreen == SCREEN_MENU) {
    if (tapped) handleMenuTap(sx, sy);

    // Periodic rescan — runPlayer() is a blocking call entered directly
    // from handleMenuTap(), so this only ever runs while on the menu.
    // Redrawing (and scanGifs()'s menuPage reset) only when the file list
    // actually changed avoids a full-screen flicker, and a surprise jump
    // back to page 1, every 15s while just sitting on the menu.
    static uint32_t lastScan = 0;
    if (millis() - lastScan > 15000) {
      lastScan = millis();
      int savedPage = menuPage;
      uint32_t before = gifListSignature();
      refreshSdAvailable();
      scanGifs();
      if (gifListSignature() != before) {
        drawMenu();
      } else {
        menuPage = savedPage;
      }
    }
  } else if (currentScreen == SCREEN_SETTINGS) {
    if (tapped) handleSettingsTap(sx, sy);
  } else if (currentScreen == SCREEN_TOUCH_HELP) {
    if (tapped) handleTouchHelpTap(sx, sy);
  }

  delay(50);
}
