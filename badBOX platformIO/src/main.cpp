/*
 * ============================================================================
 *  Unified OS — flipBOX + doomBOX
 *  Single main.cpp: one setup()/loop(), one display object, two subsystems.
 *
 *  Architecture follows "flipBOX + doomBOX Merge Display Integration Procedure":
 *    - Boots into flipBOX by default (or last NVS-persisted mode)
 *    - Mode switch only via options-menu button at index 4
 *    - Strict mutual-exclusion handoff of the async flush task
 *    - 100% of original flipBOX and doomBOX functionality preserved
 *    - RFID on SS=15 / RST=16 (OLED keeps I2C 21/22)
 * ============================================================================
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_timer.h>
#include <soc/soc.h>
#include <soc/gpio_reg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Preferences.h>
#include <SPI.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>
#include <RF24.h>
#include <math.h>
#include <esp_wifi.h>
#include "esp_bt_main.h"

// doomBOX feature headers (existing project files)
#include "../include/icon.h"
#include "../include/doombox_logo.h"
#include "flipBOX_logo.h"
#include "../include/neopixel.h"
#include "../include/setting.h"
#include "../include/sourapple.h"
#include "../include/reaper_ui.h"
#include "../include/sourdroid.h"
#include "../include/blescan.h"
#include "../include/ble_inspector.h"
#include "../include/ble_spammer.h"
#include "../include/ble_spoofer.h"
#include "../include/swiftpair.h"
#include "../include/flipperzero_detector.h"
#include "../include/meshtastic_detector.h"
#include "../include/meshcore_detector.h"
#include "../include/airtag_detector.h"
#include "../include/airtag_spoofer.h"
#include "../include/tile_detector.h"
#include "../include/smarttag_detector.h"
#include "../include/rayban_detector.h"
#include "../include/wifiscan.h"
#include "../include/client_sniffer.h"
#include "../include/deauth.h"
#include "../include/deauth_scanner.h"
#include "../include/handshake_capture.h"
#include "../include/beacon_spam.h"
#include "../include/pwnagotchi_detector.h"
#include "../include/pindefs.h"
#include "../include/sigkill.h"
#include "../include/about.h"
#include "../include/pwnagotchi_spam.h"
#include "../include/level_system.h"
#include "../include/nyanbox_detector.h"
#include "../include/nyanbox_advertiser.h"
#include "../include/evil_portal.h"
#include "../include/legal_disclaimer.h"
#include "../include/cardskimmer_detector.h"
#include "../include/axon_detector.h"
#include "../include/drone_detector.h"
#include "../include/drone_spoofer.h"
#include "../include/flock_detector.h"
#include "../include/device_scout.h"
#include "../include/pineapple_detector.h"
#include "../include/display_mirror.h"
#include "../include/password.h"
#include "../include/radio_manager.h"
#include "../include/superscanner.h"
#include "../include/rfid.h"

// ============================================================================
//  1. BUILD OPTIONS (override with -D in platformio.ini if you like)
// ============================================================================
#ifndef FB_I2C_HZ
#define FB_I2C_HZ 1000000UL
#endif
#ifndef FB_ASYNC_DISPLAY
#define FB_ASYNC_DISPLAY 1
#endif
#ifndef FB_SHOW_FPS
#define FB_SHOW_FPS 0
#endif
#ifndef FB_DEBOUNCE_US
#define FB_DEBOUNCE_US 5000UL
#endif
#ifndef FB_MAX_DT_US
#define FB_MAX_DT_US 50000UL
#endif
#ifndef FB_FAST_FLUSH
#define FB_FAST_FLUSH 1
#endif
#ifndef FB_I2C_CHUNK
#define FB_I2C_CHUNK 256
#endif
#ifndef FB_LATE_LATCH
#define FB_LATE_LATCH 1
#endif
#ifndef FB_LATCH_MARGIN_US
#define FB_LATCH_MARGIN_US 400UL
#endif
#ifndef FB_PANEL_LATENCY_US
#define FB_PANEL_LATENCY_US 5000UL
#endif
#ifndef FB_OLED_OSC
#define FB_OLED_OSC 0xF0
#endif

// ============================================================================

// ============================================================================
//  1b. SYSTEM MODE (mutual exclusion of the two subsystems)
// ============================================================================
enum SystemMode : uint8_t {
  SYS_FLIPBOX = 0,
  SYS_DOOMBOX = 1
};
SystemMode gSystemMode = SYS_FLIPBOX;   // default first boot

static const char* kNvsNamespace = "unifiedOS";
static const char* kNvsModeKey   = "sysMode";

static void loadSystemMode() {
  Preferences p;
  p.begin(kNvsNamespace, true);
  uint8_t v = p.getUChar(kNvsModeKey, (uint8_t)SYS_FLIPBOX);
  p.end();
  gSystemMode = (v == (uint8_t)SYS_DOOMBOX) ? SYS_DOOMBOX : SYS_FLIPBOX;
}

static void saveSystemMode(SystemMode m) {
  Preferences p;
  p.begin(kNvsNamespace, false);
  p.putUChar(kNvsModeKey, (uint8_t)m);
  p.end();
  gSystemMode = m;
}

//  2. GAME REGISTRY  --  ONE line per game:  X(symbol, "menu label")
// ============================================================================
#define FB_GAME_LIST(X)                                    \
  X(F1Racing,       "F1 Racing")                           \
  X(FastAndFurious, "Fast and Furious")                    \
  X(TokyoDrift,     "Tokyo Drift")                         \
  X(TwoFast2Furious, "2 Fast 2 Furious") \
  X(HillClimb, "Hill Climb Racing") \
  X(MiamiRun, "Miami Run")

// ============================================================================
//  3. FLIPBOX GAME CONTRACT v1
// ============================================================================
// ----- FLIPBOX GAME CONTRACT v1 : BEGIN ------------------------------------
#include <Arduino.h>
#include <U8g2lib.h>

namespace fb {

constexpr uint8_t SCREEN_W = 128;
constexpr uint8_t SCREEN_H = 64;

constexpr uint8_t KEY_UP     = 1u << 0;
constexpr uint8_t KEY_DOWN   = 1u << 1;
constexpr uint8_t KEY_LEFT   = 1u << 2;
constexpr uint8_t KEY_RIGHT  = 1u << 3;
constexpr uint8_t KEY_SELECT = 1u << 4;
constexpr uint8_t KEY_X      = 1u << 5;

struct Input {
  uint8_t held;
  uint8_t pressed;
  uint8_t released;
};

struct Frame {
  uint32_t nowMs;
  uint32_t dtUs;
  uint32_t index;
  uint16_t fps;
};

enum Status : uint8_t { GAME_RUN = 0, GAME_EXIT = 1 };

struct Game {
  void   (*init)();
  Status (*update)(const Input& in, const Frame& f);
  void   (*render)(U8G2& d);
  void   (*shutdown)();
};

}  // namespace fb

#define FB_GAME(id, initFn, updateFn, renderFn, shutdownFn)                    \
  extern const fb::Game fbGame_##id;                                           \
  const fb::Game fbGame_##id = { initFn, updateFn, renderFn, shutdownFn }
// ----- FLIPBOX GAME CONTRACT v1 : END --------------------------------------

namespace fb {
uint32_t keyPressedAtUs(uint8_t keyMask);
uint32_t frameStartUs();
uint32_t displayLatencyUs();
}

// Weak references
#define FB_DECLARE_WEAK(id, label) extern const fb::Game fbGame_##id __attribute__((weak));
FB_GAME_LIST(FB_DECLARE_WEAK)



// ============================================================================
//  4. HARDWARE  (file scope – shared by both subsystems)
// ============================================================================
constexpr uint8_t PIN_SDA    = 21;
constexpr uint8_t PIN_SCL    = 22;
constexpr uint8_t PIN_UP     = 26;
constexpr uint8_t PIN_DOWN   = 33;
constexpr uint8_t PIN_LEFT   = 25;
constexpr uint8_t PIN_RIGHT  = 27;
constexpr uint8_t PIN_SELECT = 32;
constexpr uint8_t PIN_X      = 4;

// Real global object — modules use: extern U8G2_... u8g2;
// A reference alias does NOT satisfy that extern and caused LoadProhibited in clearBuffer.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
// flipBOX code keeps using the name gDisp
U8G2_SSD1306_128X64_NONAME_F_HW_I2C& gDisp = u8g2;

RF24 radios[] = { RF24(RADIO_CE_PIN_1, RADIO_CSN_PIN_1) };
Adafruit_NeoPixel pixels(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
uint8_t oledBrightness = 128;

// File-scope display sync primitives (needed by handoff from both subsystems)
#if FB_ASYNC_DISPLAY
SemaphoreHandle_t gFlushDone = nullptr;
TaskHandle_t      gFlushTaskHandle = nullptr;
#endif

namespace {

// (hardware moved to file scope above)

volatile uint32_t gFlushDurUs   = 12000;
volatile uint32_t gFlushStartUs = 0;
volatile bool     gFlushBusy    = false;
uint32_t          gWorkUs       = 3000;
uint32_t          gFrameNowUs   = 0;

#if FB_ASYNC_DISPLAY
uint8_t gShadow[1024] __attribute__((aligned(4)));
#endif

#if FB_FAST_FLUSH
uint16_t gFlushChunk = 32;
bool     gNeedWindow = true;

bool fastFlush() {
  uint8_t a8 = gDisp.getU8x8()->i2c_address;
  if (a8 == 255) a8 = 0x78;
  const uint8_t addr = (uint8_t)(a8 >> 1);

  if (gNeedWindow) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0x20); Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0x21); Wire.write((uint8_t)0x00); Wire.write((uint8_t)127);
    Wire.write((uint8_t)0x22); Wire.write((uint8_t)0x00); Wire.write((uint8_t)7);
    if (Wire.endTransmission() != 0) return false;
    gNeedWindow = false;
  }
  for (uint16_t off = 0; off < sizeof(gShadow); off = (uint16_t)(off + gFlushChunk)) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)0x40);
    Wire.write(gShadow + off, (size_t)gFlushChunk);
    if (Wire.endTransmission() != 0) {
      gNeedWindow = true;
      return false;
    }
  }
  return true;
}
#endif

#if FB_ASYNC_DISPLAY
void flushTask(void*) {
#if !FB_FAST_FLUSH
  u8x8_t* const u8x8 = gDisp.getU8x8();
#endif
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if FB_FAST_FLUSH
    fastFlush();
#else
    for (uint8_t row = 0; row < 8; ++row) {
      u8x8_DrawTile(u8x8, 0, row, 16, gShadow + (row << 7));
    }
#endif
    uint32_t d = micros() - gFlushStartUs;
    if (d > 60000UL) d = 60000UL;
    gFlushDurUs = (uint32_t)((gFlushDurUs * 3UL + d) >> 2);
    gFlushBusy  = false;
    xSemaphoreGive(gFlushDone);
  }
}
#endif  // FB_ASYNC_DISPLAY

inline void presentFrame() {
#if FB_ASYNC_DISPLAY
  xSemaphoreTake(gFlushDone, portMAX_DELAY);
  memcpy(gShadow, gDisp.getBufferPtr(), sizeof(gShadow));
  gFlushStartUs = micros();
  gFlushBusy    = true;
  xTaskNotifyGive(gFlushTaskHandle);
#else
  const uint32_t t0 = micros();
  gDisp.sendBuffer();
  uint32_t d = micros() - t0;
  if (d > 60000UL) d = 60000UL;
  gFlushDurUs = (uint32_t)((gFlushDurUs * 3UL + d) >> 2);
#endif
}

inline void waitForLatch() {
#if FB_ASYNC_DISPLAY && FB_LATE_LATCH
  const uint32_t workEst = gWorkUs + (gWorkUs >> 2) + FB_LATCH_MARGIN_US;
  for (;;) {
    if (!gFlushBusy) return;
    const int32_t remain = (int32_t)(gFlushStartUs + gFlushDurUs - workEst - micros());
    if (remain <= 0 || remain > 100000) return;
    if (remain > 1500 && portTICK_PERIOD_MS == 1) vTaskDelay(1);
  }
#endif
}

// ============================================================================
//  5b. MODE HANDOFF  (MD Step 3 – exact ordering required)
// ============================================================================
void handoffToDoombox_impl() {
#if FB_ASYNC_DISPLAY
  if (gFlushDone)       xSemaphoreTake(gFlushDone, portMAX_DELAY);
  if (gFlushTaskHandle) vTaskSuspend(gFlushTaskHandle);
#endif
  gDisp.clearBuffer();
  gDisp.sendBuffer();
}

void handoffToFlipbox_impl() {
  gDisp.clearBuffer();
  gDisp.sendBuffer();
#if FB_ASYNC_DISPLAY
  if (gFlushTaskHandle) vTaskResume(gFlushTaskHandle);
  if (gFlushDone)       xSemaphoreGive(gFlushDone);
#endif
}

// ============================================================================
//  6. INPUT
// ============================================================================
constexpr uint8_t kKeyCount = 6;

constexpr bool pinHigh(uint32_t lo, uint32_t hi, uint8_t pin) {
  return (pin < 32) ? (((lo >> pin) & 1u) != 0u) : (((hi >> (pin - 32)) & 1u) != 0u);
}

inline uint8_t readRaw() {
  const uint32_t lo = REG_READ(GPIO_IN_REG);
  const uint32_t hi = REG_READ(GPIO_IN1_REG);
  return (uint8_t)((pinHigh(lo, hi, PIN_UP)     ? 0 : fb::KEY_UP)    |
                   (pinHigh(lo, hi, PIN_DOWN)   ? 0 : fb::KEY_DOWN)  |
                   (pinHigh(lo, hi, PIN_LEFT)   ? 0 : fb::KEY_LEFT)  |
                   (pinHigh(lo, hi, PIN_RIGHT)  ? 0 : fb::KEY_RIGHT) |
                   (pinHigh(lo, hi, PIN_SELECT) ? 0 : fb::KEY_SELECT) |
                   (pinHigh(lo, hi, PIN_X)      ? 0 : fb::KEY_X));
}

fb::Input gIn = {0, 0, 0};

portMUX_TYPE      gInMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t           gSt      = 0;
uint8_t           gPressL  = 0;
uint8_t           gRelL    = 0;
uint32_t          gChangeUs[kKeyCount] = {0, 0, 0, 0, 0, 0};
volatile uint32_t gPressUs[kKeyCount]  = {0, 0, 0, 0, 0, 0};

uint8_t           gBlocked = 0;
uint8_t           gPrevCur = 0;

inline void sampleLocked(uint32_t nowUs) {
  const uint8_t diff = (uint8_t)(readRaw() ^ gSt);
  if (!diff) return;
  const uint8_t raw = (uint8_t)(gSt ^ diff);
  for (uint8_t i = 0; i < kKeyCount; ++i) {
    const uint8_t bit = (uint8_t)(1u << i);
    if ((diff & bit) && (uint32_t)(nowUs - gChangeUs[i]) >= FB_DEBOUNCE_US) {
      gSt ^= bit;
      gChangeUs[i] = nowUs;
      if (raw & bit) { gPressL |= bit; gPressUs[i] = nowUs; }
      else           { gRelL   |= bit; }
    }
  }
}

void inputTimerCb(void*) {
  portENTER_CRITICAL(&gInMux);
  sampleLocked(micros());
  portEXIT_CRITICAL(&gInMux);
}

void inputInit() {
  pinMode(PIN_UP,     INPUT_PULLUP);
  pinMode(PIN_DOWN,   INPUT_PULLUP);
  pinMode(PIN_LEFT,   INPUT_PULLUP);
  pinMode(PIN_RIGHT,  INPUT_PULLUP);
  pinMode(PIN_SELECT, INPUT_PULLUP);
  pinMode(PIN_X,      INPUT_PULLUP);
  gSt      = readRaw();
  gPrevCur = gSt;
  gBlocked = gSt;

  esp_timer_create_args_t args = {};
  args.callback = inputTimerCb;
  args.name     = "fbKeys";
  esp_timer_handle_t h = nullptr;
  if (esp_timer_create(&args, &h) == ESP_OK) esp_timer_start_periodic(h, 1000);
}

inline void gateInput(uint8_t mask)  { gBlocked |= mask; }
inline void gateHeldInputs()         { gBlocked |= gPrevCur; }

inline void pollInput(uint32_t nowUs) {
  uint8_t cur, pL, rL;
  portENTER_CRITICAL(&gInMux);
  sampleLocked(nowUs);
  cur = gSt;
  pL  = gPressL;
  rL  = gRelL;
  gPressL = 0;
  gRelL   = 0;
  portEXIT_CRITICAL(&gInMux);

  const uint8_t wasBlocked = gBlocked;
  gBlocked &= (uint8_t)(cur & ~rL);
  gIn.held     = (uint8_t)(cur & ~gBlocked);
  gIn.pressed  = (uint8_t)(pL  & ~gBlocked);
  gIn.released = (uint8_t)(rL  & ~wasBlocked);
  gPrevCur = cur;
}

// ============================================================================
//  7. SETTINGS  (NVS / Preferences)
// ============================================================================
Preferences gPrefs;

struct Settings {
  uint8_t brightness;      // 1 … 100
  bool    passwordOn;
  bool    airplane;
  bool    troll;
};

Settings gCfg = { 80, false, false, false };

void applyBrightness() {
  // Map 1-100 → 8-255 so the panel never goes fully black
  uint8_t c = (uint8_t)(8 + ((uint16_t)(gCfg.brightness - 1) * 247) / 99);
  gDisp.setContrast(c);
}

void loadSettings() {
  gPrefs.begin("flipbox", true);
  gCfg.brightness  = gPrefs.getUChar("bri", 80);
  if (gCfg.brightness < 1)  gCfg.brightness = 1;
  if (gCfg.brightness > 100) gCfg.brightness = 100;
  gCfg.passwordOn  = gPrefs.getBool("pwd", false);
  gCfg.airplane    = gPrefs.getBool("air", false);
  gCfg.troll       = gPrefs.getBool("trl", false);
  gPrefs.end();
}

void saveSettings() {
  gPrefs.begin("flipbox", false);
  gPrefs.putUChar("bri", gCfg.brightness);
  gPrefs.putBool ("pwd", gCfg.passwordOn);
  gPrefs.putBool ("air", gCfg.airplane);
  gPrefs.putBool ("trl", gCfg.troll);
  gPrefs.end();
}

// ============================================================================
//  8. UI STATE MACHINE
// ============================================================================
enum UiMode : uint8_t {
  UI_PASSWORD = 0,   // boot gate
  UI_MENU,
  UI_SETTINGS,
  UI_BRIGHTNESS,
  UI_GAME
};

UiMode   gMode          = UI_MENU;
uint8_t  gSel           = 0;
uint8_t  gTop           = 0;
bool     gMenuDirty     = true;
uint32_t gMsgUntil      = 0;

const fb::Game* gActive = nullptr;

// Hold tracking for long-press actions
uint32_t gHoldStartMs   = 0;     // when the relevant button first went down
uint8_t  gHoldKey       = 0;     // which key we are currently tracking
bool     gHoldArmed     = false; // true while a qualifying hold is in progress

// Settings grid
constexpr uint8_t kOptCount = 6;
uint8_t  gOptSel        = 0;     // 0-5
bool     gSettingsDirty = true;

// Open / close animation for the settings panel (0 = closed, 255 = fully open)
uint8_t  gSetAnim       = 0;
bool     gSetAnimIn     = true;  // true = opening, false = closing

// Brightness editor
uint8_t  gBriTemp       = 80;

// Password sequence: UP DOWN UP DOWN
constexpr uint8_t kPwdLen = 4;
const uint8_t kPwdSeq[kPwdLen] = {
  fb::KEY_UP, fb::KEY_DOWN, fb::KEY_UP, fb::KEY_DOWN
};
uint8_t  gPwdIdx        = 0;
bool     gPwdWrong      = false;
uint32_t gPwdWrongUntil = 0;

// Troll-mode disable hold
uint32_t gTrollHoldStart = 0;
bool     gTrollHoldArmed = false;

// Simple PRNG for troll visuals (xorshift)
uint32_t gRng = 0xA5A5C3C3u;
inline uint32_t rnd() {
  gRng ^= gRng << 13;
  gRng ^= gRng >> 17;
  gRng ^= gRng << 5;
  return gRng;
}

// ============================================================================
//  9. GAME TABLE
// ============================================================================
struct Entry {
  const char*     label;
  const fb::Game* game;
};

#define FB_TABLE_ROW(id, label) { label, &fbGame_##id },
const Entry kGames[] = { FB_GAME_LIST(FB_TABLE_ROW) };
constexpr uint8_t kGameCount = (uint8_t)(sizeof(kGames) / sizeof(kGames[0]));
static_assert(kGameCount >= 1, "FB_GAME_LIST needs at least one entry");

constexpr uint8_t kVisibleRows = 3;

// Game timing
uint32_t gLastUs    = 0;
uint32_t gFrameIdx  = 0;
uint32_t gFpsT0     = 0;
uint32_t gFpsFrames = 0;
uint16_t gFps       = 0;
fb::Frame gFrame    = {0, 0, 0, 0};

inline char digitChar(uint8_t n) { return (n < 10) ? (char)('0' + n) : '+'; }

// ============================================================================
// 10. DRAWING HELPERS (premium monochrome UI)
// ============================================================================
void drawRoundedBox(int x, int y, int w, int h, int r) {
  // Soft rounded rectangle (filled)
  gDisp.drawBox(x + r, y, w - 2 * r, h);
  gDisp.drawBox(x, y + r, w, h - 2 * r);
  gDisp.drawDisc(x + r, y + r, r);
  gDisp.drawDisc(x + w - r - 1, y + r, r);
  gDisp.drawDisc(x + r, y + h - r - 1, r);
  gDisp.drawDisc(x + w - r - 1, y + h - r - 1, r);
}

void drawRoundedFrame(int x, int y, int w, int h, int r) {
  gDisp.drawHLine(x + r, y, w - 2 * r);
  gDisp.drawHLine(x + r, y + h - 1, w - 2 * r);
  gDisp.drawVLine(x, y + r, h - 2 * r);
  gDisp.drawVLine(x + w - 1, y + r, h - 2 * r);
  // corner arcs approximated with pixels
  gDisp.drawPixel(x + 1, y + 1);
  gDisp.drawPixel(x + w - 2, y + 1);
  gDisp.drawPixel(x + 1, y + h - 2);
  gDisp.drawPixel(x + w - 2, y + h - 2);
  gDisp.drawPixel(x + 2, y);
  gDisp.drawPixel(x + w - 3, y);
  gDisp.drawPixel(x + 2, y + h - 1);
  gDisp.drawPixel(x + w - 3, y + h - 1);
  gDisp.drawPixel(x, y + 2);
  gDisp.drawPixel(x + w - 1, y + 2);
  gDisp.drawPixel(x, y + h - 3);
  gDisp.drawPixel(x + w - 1, y + h - 3);
}

// Progress bar for long-press feedback
void drawHoldBar(uint8_t progress /*0-100*/, int y = 60) {
  const int barW = 100;
  const int barX = (128 - barW) / 2;
  gDisp.setDrawColor(1);
  gDisp.drawFrame(barX, y, barW, 4);
  int fill = (barW - 2) * progress / 100;
  if (fill > 0) gDisp.drawBox(barX + 1, y + 1, fill, 2);
}

// ============================================================================
// 11. TROLL MODE OVERLAY — realistic digital glitch (boxes, lines, binary)
// ============================================================================
// Frame-timed flicker: intensity changes every few frames so it feels alive
static uint8_t  gGlitchPhase  = 0;
static uint32_t gGlitchLastMs = 0;

void applyTrollOverlay() {
  if (!gCfg.troll) return;

  const uint32_t now = millis();
  // Advance phase ~12–20 Hz so flicker reads as real corruption, not static art
  if (now - gGlitchLastMs >= 50 + (rnd() % 40)) {
    gGlitchLastMs = now;
    gGlitchPhase++;
    // Reseed occasionally so patterns don't loop visibly
    if ((gGlitchPhase & 7) == 0) gRng ^= now;
  }

  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.setFontMode(1);

  // --- 1) Full-frame blackout / invert flash (rare) ---
  if ((gGlitchPhase & 0x1F) == 0 && (rnd() & 3) == 0) {
    u8g2.drawBox(0, 0, 128, 64);
    return;  // pure flash frame
  }
  if ((gGlitchPhase & 0x0F) == 3 && (rnd() & 7) == 0) {
    // partial screen wipe
    int y = (int)(rnd() % 48);
    int h = 8 + (int)(rnd() % 24);
    u8g2.drawBox(0, y, 128, h);
  }

  // --- 2) Solid rectangular blocks (tearing / block noise) ---
  const int nBlocks = 2 + (int)(rnd() % 5);
  for (int i = 0; i < nBlocks; ++i) {
    int w = 4 + (int)(rnd() % 40);
    int h = 2 + (int)(rnd() % 14);
    int x = (int)(rnd() % (128 - w + 1));
    int y = (int)(rnd() % (64 - h + 1));
    if (rnd() & 1)
      u8g2.drawBox(x, y, w, h);
    else
      u8g2.drawFrame(x, y, w, h);
  }

  // --- 3) Hard horizontal tear lines (1–3 px tall bands) ---
  const int nH = 2 + (int)(rnd() % 4);
  for (int i = 0; i < nH; ++i) {
    int y = (int)(rnd() % 64);
    int h = 1 + (int)(rnd() % 3);
    int x0 = (int)(rnd() % 32);
    int w  = 64 + (int)(rnd() % (128 - x0));
    for (int dy = 0; dy < h; ++dy) {
      if (y + dy < 64) u8g2.drawHLine(x0, y + dy, w);
    }
  }

  // --- 4) Vertical slice lines ---
  const int nV = 1 + (int)(rnd() % 3);
  for (int i = 0; i < nV; ++i) {
    int x = (int)(rnd() % 128);
    int y0 = (int)(rnd() % 32);
    int h = 16 + (int)(rnd() % 48);
    u8g2.drawVLine(x, y0, h);
    if ((rnd() & 1) && x + 1 < 128) u8g2.drawVLine(x + 1, y0, h);
  }

  // --- 5) Binary / garbage text strings ---
  const int nStr = 1 + (int)(rnd() % 4);
  char buf[18];
  for (int s = 0; s < nStr; ++s) {
    const int len = 4 + (int)(rnd() % 12);
    for (int c = 0; c < len && c < 17; ++c)
      buf[c] = (rnd() & 1) ? '1' : '0';
    buf[len < 17 ? len : 17] = 0;
    int tx = (int)(rnd() % 100);
    int ty = 6 + (int)(rnd() % 58);
    u8g2.drawStr(tx, ty, buf);
  }

  // Occasional short hex-looking garbage
  if ((rnd() & 3) == 0) {
    static const char hexc[] = "0123456789ABCDEF";
    char hx[9];
    for (int i = 0; i < 8; ++i) hx[i] = hexc[rnd() % 16];
    hx[8] = 0;
    u8g2.drawStr((int)(rnd() % 90), 6 + (int)(rnd() % 58), hx);
  }

  // --- 6) Fine static: sparse single pixels ---
  const int nPix = 8 + (int)(rnd() % 20);
  for (int i = 0; i < nPix; ++i)
    u8g2.drawPixel((int)(rnd() % 128), (int)(rnd() % 64));

  // --- 7) Scanline flicker: every other line in a band ---
  if ((gGlitchPhase & 3) == 1) {
    int y0 = (int)(rnd() % 50);
    int h  = 6 + (int)(rnd() % 14);
    for (int y = y0; y < y0 + h && y < 64; y += 2)
      u8g2.drawHLine(0, y, 128);
  }
}

// ============================================================================
// 12. MENU DRAWING
// ============================================================================
void drawMenu() {
  gDisp.clearBuffer();
  gDisp.setFontMode(1);
  gDisp.setDrawColor(1);

  // Header
  gDisp.setFont(u8g2_font_7x13B_tr);
  gDisp.drawStr(2, 11, "flipBOX");
  gDisp.setFont(u8g2_font_5x7_tr);
  gDisp.drawGlyph(111, 11, digitChar((uint8_t)(gSel + 1)));
  gDisp.drawGlyph(116, 11, '/');
  gDisp.drawGlyph(121, 11, digitChar(kGameCount));
  gDisp.drawHLine(0, 14, 128);

  // Airplane indicator (tiny plane icon when active)
  if (gCfg.airplane) {
    gDisp.setFont(u8g2_font_5x7_tr);
    gDisp.drawStr(78, 11, "AIR");
  }

  const bool    scroll = kGameCount > kVisibleRows;
  const uint8_t listW  = scroll ? 124 : 128;
  for (uint8_t r = 0; r < kVisibleRows; ++r) {
    const uint8_t idx = (uint8_t)(gTop + r);
    if (idx >= kGameCount) break;
    const uint8_t y   = (uint8_t)(17 + 15 * r);
    const bool    sel = (idx == gSel);
    if (sel) {
      gDisp.drawBox(0, y, listW, 14);
      gDisp.setDrawColor(0);
      gDisp.setFont(u8g2_font_6x13B_tr);
    } else {
      gDisp.setDrawColor(1);
      gDisp.setFont(u8g2_font_6x13_tr);
    }
    gDisp.drawGlyph(4, y + 11, digitChar((uint8_t)(idx + 1)));
    gDisp.drawStr(16, y + 11, kGames[idx].label);
    gDisp.setDrawColor(1);
  }

  if (scroll) {
    constexpr uint8_t trackY = 17, trackH = 44;
    uint8_t thumbH = (uint8_t)((trackH * kVisibleRows) / kGameCount);
    if (thumbH < 5) thumbH = 5;
    constexpr uint8_t range = (kGameCount > kVisibleRows) ? (uint8_t)(kGameCount - kVisibleRows) : (uint8_t)1;
    const uint8_t thumbY = (uint8_t)(trackY + ((trackH - thumbH) * gTop) / range);
    gDisp.drawVLine(127, trackY, trackH);
    gDisp.drawBox(125, thumbY, 3, thumbH);
  }

  if (gMsgUntil) {
    gDisp.setDrawColor(0);
    gDisp.drawBox(14, 20, 100, 24);
    gDisp.setDrawColor(1);
    gDisp.drawFrame(14, 20, 100, 24);
    gDisp.drawFrame(15, 21, 98, 22);
    gDisp.setFont(u8g2_font_6x13B_tr);
    gDisp.drawStr(25, 37, "NOT INSTALLED");
  }

  // Hold-UP progress bar (entering settings) – only after 500 ms
  if (gHoldArmed && gHoldKey == fb::KEY_UP && gMode == UI_MENU) {
    uint32_t held = millis() - gHoldStartMs;
    if (held >= 500) {
      if (held > 1000) held = 1000;
      // map 500→1000 ms into 0→100 %
      drawHoldBar((uint8_t)((held - 500) * 100 / 500));
    }
  }

  // Troll-kill progress bar
  if (gTrollHoldArmed && gCfg.troll) {
    uint32_t held = millis() - gTrollHoldStart;
    if (held > 4000) held = 4000;
    drawHoldBar((uint8_t)(held / 40), 54);
  }
}

void menuMove(int8_t dir) {
  if (dir < 0) gSel = gSel ? (uint8_t)(gSel - 1) : (uint8_t)(kGameCount - 1);
  else         gSel = (uint8_t)((gSel + 1 >= kGameCount) ? 0 : gSel + 1);
  if (gSel < gTop)                          gTop = gSel;
  else if (gSel >= gTop + kVisibleRows)     gTop = (uint8_t)(gSel - kVisibleRows + 1);
  gMsgUntil  = 0;
  gMenuDirty = true;
}

// ============================================================================
// 13. SETTINGS SCREEN  (pure GUI 2×3 icon grid – no text)
// ============================================================================
const bool kOptSelectable[kOptCount] = {
  true, true, true, true, true, false  // index 4 = mode switch
};

void optCentre(uint8_t idx, int& cx, int& cy) {
  const int col = idx % 3;
  const int row = idx / 3;
  // Slightly larger spacing, more margin from edges
  cx = 22 + col * 42;   // 22, 64, 106
  cy = 22 + row * 28;   // 22, 50
}

// Clean single-pixel ring (no double outline clutter)
static void drawCleanRing(int cx, int cy, int r, bool selected) {
  if (selected) {
    u8g2.drawDisc(cx, cy, r);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx, cy, r - 2);
    u8g2.setDrawColor(1);
    u8g2.drawCircle(cx, cy, r - 2);
  } else {
    u8g2.drawCircle(cx, cy, r);
  }
}

static void drawIconSun(int cx, int cy) {
  u8g2.drawDisc(cx, cy, 2);
  // 8 short rays
  u8g2.drawPixel(cx,     cy - 6); u8g2.drawPixel(cx,     cy - 5);
  u8g2.drawPixel(cx,     cy + 5); u8g2.drawPixel(cx,     cy + 6);
  u8g2.drawPixel(cx - 6, cy);     u8g2.drawPixel(cx - 5, cy);
  u8g2.drawPixel(cx + 5, cy);     u8g2.drawPixel(cx + 6, cy);
  u8g2.drawPixel(cx - 4, cy - 4); u8g2.drawPixel(cx + 4, cy - 4);
  u8g2.drawPixel(cx - 4, cy + 4); u8g2.drawPixel(cx + 4, cy + 4);
}

static void drawIconLock(int cx, int cy) {
  // shackle
  u8g2.drawCircle(cx, cy - 3, 3);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(cx, cy - 2, 2);
  u8g2.setDrawColor(1);
  // body
  u8g2.drawRBox(cx - 4, cy - 1, 9, 7, 1);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(cx, cy + 2);
  u8g2.setDrawColor(1);
}

static void drawIconPlane(int cx, int cy) {
  // fuselage
  u8g2.drawHLine(cx - 6, cy, 13);
  u8g2.drawHLine(cx - 6, cy + 1, 13);
  // wings
  u8g2.drawLine(cx - 1, cy, cx - 5, cy - 4);
  u8g2.drawLine(cx,     cy, cx - 4, cy - 4);
  u8g2.drawLine(cx - 1, cy + 1, cx - 5, cy + 5);
  u8g2.drawLine(cx,     cy + 1, cx - 4, cy + 5);
  // tail
  u8g2.drawLine(cx + 5, cy, cx + 7, cy - 3);
  u8g2.drawLine(cx + 5, cy + 1, cx + 7, cy + 3);
}

static void drawIconTroll(int cx, int cy) {
  u8g2.drawCircle(cx, cy, 5);
  // eyes
  u8g2.drawPixel(cx - 2, cy - 1);
  u8g2.drawPixel(cx + 2, cy - 1);
  // grin
  u8g2.drawHLine(cx - 3, cy + 2, 7);
  u8g2.drawPixel(cx - 3, cy + 1);
  u8g2.drawPixel(cx + 3, cy + 1);
}

static void drawIconMode(int cx, int cy) {
  // split circle
  u8g2.drawCircle(cx, cy, 6);
  u8g2.drawVLine(cx, cy - 5, 11);
  // left: small skull
  u8g2.drawDisc(cx - 3, cy - 1, 1);
  u8g2.drawPixel(cx - 3, cy + 2);
  // right: radio waves
  u8g2.drawPixel(cx + 2, cy);
  u8g2.drawPixel(cx + 4, cy - 2);
  u8g2.drawPixel(cx + 4, cy + 2);
  u8g2.drawPixel(cx + 5, cy);
}

void drawOptIcon(uint8_t idx, bool selected, int animOffsetY) {
  int cx, cy;
  optCentre(idx, cx, cy);
  cy += animOffsetY;

  const bool blank = !kOptSelectable[idx];
  const int r = 12;

  u8g2.setDrawColor(1);
  if (blank) {
    // Hoverable empty slot — ring only, no action
    drawCleanRing(cx, cy, r, selected);
    if (!selected) {
      u8g2.drawHLine(cx - 2, cy, 5);
    }
    return;
  }

  drawCleanRing(cx, cy, r, selected);

  // Glyphs drawn in the hole (selected rings already inverted centre)
  if (selected) {
    // centre is clear (draw color 1 on black hole)
    u8g2.setDrawColor(1);
  }

  switch (idx) {
    case 0: drawIconSun(cx, cy); break;
    case 1: drawIconLock(cx, cy); break;
    case 2: drawIconPlane(cx, cy); break;
    case 3: drawIconTroll(cx, cy); break;
    case 4: drawIconMode(cx, cy); break;
    default: break;
  }

  // ON badge — small filled corner dot
  bool isOn = false;
  if (idx == 1) isOn = gCfg.passwordOn;
  if (idx == 2) isOn = gCfg.airplane;
  if (idx == 3) isOn = gCfg.troll;
  if (isOn) {
    u8g2.setDrawColor(1);
    int bx = cx + r - 1;
    int by = cy - r + 1;
    if (selected) {
      u8g2.drawDisc(bx, by, 2);
      u8g2.setDrawColor(0);
      u8g2.drawDisc(bx, by, 1);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawDisc(bx, by, 2);
    }
  }
}

// 0 = none, 1..100 = fill of bottom hold bar (drawn after icons)
uint8_t gOptHoldBarPct = 0;

void drawSettings() {
  u8g2.clearBuffer();
  u8g2.setFontMode(1);
  u8g2.setDrawColor(1);

  // Roll-down: icons start above and settle into place
  int offsetY = 0;
  if (gSetAnim < 255) {
    offsetY = (int)((255 - gSetAnim) * (-36) / 255);  // from -36 toward 0
  }

  for (uint8_t i = 0; i < kOptCount; ++i) {
    drawOptIcon(i, i == gOptSel, offsetY);
  }

  // Single non-flicker hold progress bar (open or close)
  if (gOptHoldBarPct > 0) {
    const int barX = 24, barW = 80, barY = 60;
    u8g2.drawFrame(barX, barY, barW, 4);
    int fill = (barW - 2) * (int)gOptHoldBarPct / 100;
    if (fill > 0) u8g2.drawBox(barX + 1, barY + 1, fill, 2);
  }
}

void drawBrightness() {
  gDisp.clearBuffer();
  gDisp.setFontMode(1);
  gDisp.setDrawColor(1);

  gDisp.setFont(u8g2_font_7x13B_tr);
  gDisp.drawStr(28, 12, "BRIGHTNESS");
  gDisp.drawHLine(0, 15, 128);

  // Big percentage
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)gBriTemp);
  gDisp.setFont(u8g2_font_logisoso16_tr);
  int tw = gDisp.getStrWidth(buf);
  gDisp.drawStr((128 - tw) / 2, 40, buf);

  // Slider track
  const int trackX = 14, trackW = 100, trackY = 48;
  gDisp.drawFrame(trackX, trackY, trackW, 8);
  int fill = (trackW - 2) * gBriTemp / 100;
  if (fill > 0) gDisp.drawBox(trackX + 1, trackY + 1, fill, 6);

  // Tick marks at 25/50/75
  for (int p = 25; p <= 75; p += 25) {
    int x = trackX + (trackW * p / 100);
    gDisp.drawVLine(x, trackY - 2, 2);
  }

  gDisp.setFont(u8g2_font_4x6_tr);
  gDisp.drawStr(2, 63, "UP/DN adjust  SEL save  X cancel");
}

// ============================================================================
// 15. PASSWORD GATE
// ============================================================================
void drawPassword() {
  gDisp.clearBuffer();
  gDisp.setFontMode(1);
  gDisp.setDrawColor(1);

  gDisp.setFont(u8g2_font_7x13B_tr);
  gDisp.drawStr(22, 14, "ENTER CODE");
  gDisp.drawHLine(0, 17, 128);

  // Four dots / filled when entered
  for (uint8_t i = 0; i < kPwdLen; ++i) {
    int x = 28 + i * 20;
    if (i < gPwdIdx) gDisp.drawDisc(x, 34, 5);
    else             gDisp.drawCircle(x, 34, 5);
  }

  if (gPwdWrong) {
    gDisp.setFont(u8g2_font_6x13B_tr);
    gDisp.drawStr(34, 54, "WRONG");
  } else {
    gDisp.setFont(u8g2_font_5x7_tr);
    gDisp.drawStr(18, 54, "UP  DN  UP  DN");
  }
}

// ============================================================================
// 16. GAME ENTER / LEAVE
// ============================================================================
void enterGame(const fb::Game* g) {
  gActive = g;
  gMode   = UI_GAME;
  gateHeldInputs();

  gDisp.setDrawColor(1);
  gDisp.setFontMode(0);
  gDisp.setBitmapMode(0);
  gDisp.setFontDirection(0);
  gDisp.setFontPosBaseline();

  gFrameIdx  = 0;
  gFpsFrames = 0;
  gFps       = 0;
  gLastUs    = micros();
  gFpsT0     = gLastUs;
  if (g->init) g->init();
}

void leaveGame() {
  if (gActive && gActive->shutdown) gActive->shutdown();
  gActive = nullptr;
  gMode   = UI_MENU;
  gateHeldInputs();
  gMsgUntil  = 0;
  gMenuDirty = true;
}

// ============================================================================
// 17. MODE RUNNERS
// ============================================================================
void runPassword() {
  if (gPwdWrong && (int32_t)(millis() - gPwdWrongUntil) >= 0) {
    gPwdWrong = false;
    gPwdIdx   = 0;
  }

  const uint8_t p = gIn.pressed;
  if (p && !gPwdWrong) {
    uint8_t expected = kPwdSeq[gPwdIdx];
    if (p & expected) {
      gPwdIdx++;
      if (gPwdIdx >= kPwdLen) {
        gMode = UI_MENU;
        gMenuDirty = true;
        gateHeldInputs();
      }
    } else if (p & (fb::KEY_UP | fb::KEY_DOWN | fb::KEY_LEFT | fb::KEY_RIGHT | fb::KEY_SELECT | fb::KEY_X)) {
      gPwdWrong = true;
      gPwdWrongUntil = millis() + 800;
      gPwdIdx = 0;
    }
    gateInput(p);
  }

  drawPassword();
  applyTrollOverlay();
  presentFrame();
}

void runMenu() {
  // Global troll-kill (hold SELECT 4 s) even while on the menu
  if (gCfg.troll) {
    if (gIn.held & fb::KEY_SELECT) {
      if (!gTrollHoldArmed) {
        gTrollHoldArmed = true;
        gTrollHoldStart = millis();
        gMenuDirty = true;
      } else {
        uint32_t held = millis() - gTrollHoldStart;
        if (held >= 4000) {
          gCfg.troll = false;
          saveSettings();
          gTrollHoldArmed = false;
          gateInput(fb::KEY_SELECT);
          gMenuDirty = true;
        } else {
          gMenuDirty = true;
        }
      }
    } else {
      if (gTrollHoldArmed) { gTrollHoldArmed = false; gMenuDirty = true; }
    }
  }

  // --- long-press UP to open settings (bar only after 500 ms) ---
  if (gIn.held & fb::KEY_UP) {
    if (!gHoldArmed || gHoldKey != fb::KEY_UP) {
      gHoldArmed   = true;
      gHoldKey     = fb::KEY_UP;
      gHoldStartMs = millis();
    } else {
      uint32_t held = millis() - gHoldStartMs;
      if (held >= 1000) {
        // open settings
        gHoldArmed     = false;
        gMode          = UI_SETTINGS;
        gOptSel        = 0;
        gSetAnim       = 0;
        gSetAnimIn     = true;
        gSettingsDirty = true;
        gateInput(fb::KEY_UP);
        return;
      }
      // redraw only once the bar is visible (>= 500 ms)
      if (held >= 500) gMenuDirty = true;
    }
  } else {
    if (gHoldArmed && gHoldKey == fb::KEY_UP) {
      gHoldArmed = false;
      gMenuDirty = true;   // clear bar if it was showing
    }
  }

  // Short-press navigation always works (not blocked by hold tracking)
  const uint8_t p = gIn.pressed;
  if (p) {
    if (p & (fb::KEY_UP | fb::KEY_LEFT)) {
      menuMove(-1);
      // do NOT gate UP here – allow the hold timer to keep running
      if (p & fb::KEY_LEFT) gateInput(fb::KEY_LEFT);
    } else if (p & (fb::KEY_DOWN | fb::KEY_RIGHT)) {
      menuMove(+1);
      gateInput(p & (fb::KEY_DOWN | fb::KEY_RIGHT));
    } else if (p & fb::KEY_SELECT) {
      gateInput(fb::KEY_SELECT);
      const fb::Game* g = kGames[gSel].game;
      if (g && g->update && g->render) {
        enterGame(g);
        return;
      }
      gMsgUntil = millis() + 1200u;
      if (gMsgUntil == 0) gMsgUntil = 1;
      gMenuDirty = true;
    }
  }

  if (gMsgUntil && (int32_t)(millis() - gMsgUntil) >= 0) {
    gMsgUntil  = 0;
    gMenuDirty = true;
  }

  if (gMenuDirty) {
    gMenuDirty = false;
    drawMenu();
    applyTrollOverlay();
    presentFrame();
  }
}

void runSettings() {
  // Global troll-kill also works here
  if (gCfg.troll) {
    if (gIn.held & fb::KEY_SELECT) {
      if (!gTrollHoldArmed) {
        gTrollHoldArmed = true;
        gTrollHoldStart = millis();
        gSettingsDirty = true;
      } else if (millis() - gTrollHoldStart >= 4000) {
        gCfg.troll = false;
        saveSettings();
        gTrollHoldArmed = false;
        gateInput(fb::KEY_SELECT);
        gSettingsDirty = true;
      } else {
        gSettingsDirty = true;
      }
    } else if (gTrollHoldArmed) {
      gTrollHoldArmed = false;
      gSettingsDirty = true;
    }
  }

  // Animation
  if (gSetAnimIn && gSetAnim < 255) {
    gSetAnim = (uint8_t)min(255, (int)gSetAnim + 18);
    gSettingsDirty = true;
  } else if (!gSetAnimIn && gSetAnim > 0) {
    gSetAnim = (uint8_t)max(0, (int)gSetAnim - 22);
    gSettingsDirty = true;
    if (gSetAnim == 0) {
      gMode = UI_MENU;
      gMenuDirty = true;
      gateHeldInputs();
      return;
    }
  }

  // Long-press DOWN to exit (bar after 500 ms, full at 1000 ms)
  gOptHoldBarPct = 0;
  if (gIn.held & fb::KEY_DOWN) {
    if (!gHoldArmed || gHoldKey != fb::KEY_DOWN) {
      gHoldArmed   = true;
      gHoldKey     = fb::KEY_DOWN;
      gHoldStartMs = millis();
    } else {
      uint32_t held = millis() - gHoldStartMs;
      if (held >= 1000) {
        gHoldArmed = false;
        gOptHoldBarPct = 0;
        gSetAnimIn = false;
        gSettingsDirty = true;
        gateInput(fb::KEY_DOWN);
      } else if (held >= 500) {
        gOptHoldBarPct = (uint8_t)((held - 500) * 100 / 500);
        gSettingsDirty = true;
      }
    }
  } else {
    if (gHoldArmed && gHoldKey == fb::KEY_DOWN) {
      gHoldArmed = false;
      gOptHoldBarPct = 0;
      gSettingsDirty = true;
    }
  }

  // Short-press navigation always works (not blocked by hold tracking)
  const uint8_t p = gIn.pressed;
  if (p && gSetAnimIn) {
    if (p & fb::KEY_LEFT) {
      if (gOptSel % 3 > 0) gOptSel--;
      else gOptSel = (uint8_t)(gOptSel + 2);
      gSettingsDirty = true;
      gateInput(fb::KEY_LEFT);
    } else if (p & fb::KEY_RIGHT) {
      if (gOptSel % 3 < 2) gOptSel++;
      else gOptSel = (uint8_t)(gOptSel - 2);
      gSettingsDirty = true;
      gateInput(fb::KEY_RIGHT);
    } else if (p & fb::KEY_UP) {
      if (gOptSel >= 3) gOptSel = (uint8_t)(gOptSel - 3);
      else gOptSel = (uint8_t)(gOptSel + 3);
      gSettingsDirty = true;
      gateInput(fb::KEY_UP);
    } else if (p & fb::KEY_DOWN) {
      // short press moves; long press is tracked above
      if (gOptSel < 3) gOptSel = (uint8_t)(gOptSel + 3);
      else gOptSel = (uint8_t)(gOptSel - 3);
      gSettingsDirty = true;
      // do NOT gate DOWN – allow the hold timer to keep running
    } else if (p & fb::KEY_SELECT) {
      gateInput(fb::KEY_SELECT);
      if (kOptSelectable[gOptSel]) {
        switch (gOptSel) {
          case 0: // brightness
            gBriTemp = gCfg.brightness;
            gMode = UI_BRIGHTNESS;
            break;
          case 1: // password toggle
            gCfg.passwordOn = !gCfg.passwordOn;
            saveSettings();
            gSettingsDirty = true;
            break;
          case 2: // airplane
            gCfg.airplane = !gCfg.airplane;
            saveSettings();
            gSettingsDirty = true;
            break;
          case 3: // troll
            gCfg.troll = !gCfg.troll;
            saveSettings();
            gSettingsDirty = true;
            break;
          case 4: // MODE SWITCH -> doomBOX
            saveSystemMode(SYS_DOOMBOX);
            // enterDoomboxMode() is invoked from loop() on mode change
            break;
        }
      }
    }
  }

  if (gSettingsDirty || gSetAnim < 255) {
    gSettingsDirty = false;
    drawSettings();
    applyTrollOverlay();
    presentFrame();
  }
}

void runBrightness() {
  const uint8_t p = gIn.pressed;
  bool dirty = false;

  if (p & fb::KEY_UP) {
    if (gBriTemp < 100) { gBriTemp++; dirty = true; }
    gateInput(fb::KEY_UP);
  }
  if (p & fb::KEY_DOWN) {
    if (gBriTemp > 1) { gBriTemp--; dirty = true; }
    gateInput(fb::KEY_DOWN);
  }
  // also support held for faster scrub
  static uint32_t lastRepeat = 0;
  if ((gIn.held & (fb::KEY_UP | fb::KEY_DOWN)) && (millis() - lastRepeat > 40)) {
    lastRepeat = millis();
    if ((gIn.held & fb::KEY_UP) && gBriTemp < 100) { gBriTemp++; dirty = true; }
    if ((gIn.held & fb::KEY_DOWN) && gBriTemp > 1) { gBriTemp--; dirty = true; }
  }

  if (p & fb::KEY_SELECT) {
    gCfg.brightness = gBriTemp;
    applyBrightness();
    saveSettings();
    gMode = UI_SETTINGS;
    gSettingsDirty = true;
    gateInput(fb::KEY_SELECT);
    return;
  }
  if (p & fb::KEY_X) {
    gMode = UI_SETTINGS;
    gSettingsDirty = true;
    gateInput(fb::KEY_X);
    return;
  }

  // live preview
  uint8_t c = (uint8_t)(8 + ((uint16_t)(gBriTemp - 1) * 247) / 99);
  gDisp.setContrast(c);

  drawBrightness();
  applyTrollOverlay();
  presentFrame();
  (void)dirty;
}

#if FB_SHOW_FPS
void drawFpsOverlay() {
  const uint8_t* const prevFont = gDisp.getU8g2()->font;
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)gFps);
  gDisp.setFont(u8g2_font_5x7_tr);
  const uint8_t w = (uint8_t)gDisp.getStrWidth(buf);
  gDisp.setDrawColor(0);
  gDisp.drawBox(128 - w - 3, 0, w + 3, 9);
  gDisp.setDrawColor(1);
  gDisp.drawStr(128 - w - 1, 8, buf);
  gDisp.setFont(prevFont);
}
#endif

inline void runGame(uint32_t nowUs) {
  // Troll-mode kill: hold SELECT 4 seconds
  if (gCfg.troll) {
    if (gIn.held & fb::KEY_SELECT) {
      if (!gTrollHoldArmed) {
        gTrollHoldArmed = true;
        gTrollHoldStart = millis();
      } else if (millis() - gTrollHoldStart >= 4000) {
        gCfg.troll = false;
        saveSettings();
        gTrollHoldArmed = false;
        gateInput(fb::KEY_SELECT);
      }
    } else {
      gTrollHoldArmed = false;
    }
  }

  gFrameNowUs = nowUs;
  uint32_t dt = nowUs - gLastUs;
  gLastUs = nowUs;
  if (dt > FB_MAX_DT_US) dt = FB_MAX_DT_US;

  gFrame.nowMs = millis();
  gFrame.dtUs  = dt;
  gFrame.index = gFrameIdx++;
  gFrame.fps   = gFps;

  if (gActive->update(gIn, gFrame) == fb::GAME_EXIT) {
    leaveGame();
    return;
  }

  gDisp.clearBuffer();
  gActive->render(gDisp);
#if FB_SHOW_FPS
  drawFpsOverlay();
#endif
  applyTrollOverlay();

  // progress bar while killing troll
  if (gTrollHoldArmed) {
    uint32_t held = millis() - gTrollHoldStart;
    if (held > 4000) held = 4000;
    drawHoldBar((uint8_t)(held / 40), 58);
  }

  uint32_t work = micros() - nowUs;
  if (work > 50000UL) work = 50000UL;
  gWorkUs = (uint32_t)(gWorkUs - (gWorkUs >> 3) + (work >> 3));
  presentFrame();

  ++gFpsFrames;
  const uint32_t el = nowUs - gFpsT0;
  if (el >= 500000UL) {
    gFps       = (uint16_t)((gFpsFrames * 1000000UL) / el);
    gFpsFrames = 0;
    gFpsT0     = nowUs;
  }
}

}  // namespace

// Global wrappers so doomBOX host (outside anon ns) can call handoff
void handoffToDoombox() { handoffToDoombox_impl(); }
void handoffToFlipbox() { handoffToFlipbox_impl(); }

// ============================================================================
//  LAUNCHER EXTRAS
// ============================================================================
namespace fb {

uint32_t keyPressedAtUs(uint8_t keyMask) {
  for (uint8_t i = 0; i < kKeyCount; ++i)
    if (keyMask & (1u << i)) return gPressUs[i];
  return 0;
}

uint32_t frameStartUs() { return gFrameNowUs; }

uint32_t displayLatencyUs() {
  uint32_t l = gWorkUs + (gWorkUs >> 2) + FB_LATCH_MARGIN_US
             + gFlushDurUs
             + FB_PANEL_LATENCY_US;
  if (l > 100000UL) l = 100000UL;
  return l;
}

}  // namespace fb


// ============================================================================
//  8b. doomBOX HOST  (100% original functionality preserved)
// ============================================================================
// The following is the original doomBOX control flow adapted to the shared
// display object.  flushTask is suspended while this runs (handoffToDoombox).
// RFID: RC522 on SS=15 / RST=16 (see pindefs.h).

bool dangerousActionsEnabled = true;
const char* nyanboxVersion = "unified";
static unsigned long lastActivity = 0;

// Button debounce state (doomBOX style)
unsigned long upLastMillis = 0, upNextRepeat = 0, upDebounceTime = 0;
bool upPressed = false;
unsigned long downLastMillis = 0, downNextRepeat = 0, downDebounceTime = 0;
bool downPressed = false;
bool selPrev = false, rightPrev = false, leftPrev = false;
unsigned long selDebounceTime = 0, rightDebounceTime = 0, leftDebounceTime = 0;
const unsigned long initialDelay = 500, repeatInterval = 250, debounceDelay = 200;

static bool needsRedraw = true;
static bool advertiserBootPending = true;
static bool doomboxInitialized = false;

void updateLastActivity() { lastActivity = millis(); }
void updateSleepTimeout(unsigned long) {}
bool anyButtonPressed() {
  return digitalRead(BUTTON_PIN_UP) == LOW || digitalRead(BUTTON_PIN_DOWN) == LOW ||
         digitalRead(BUTTON_PIN_CENTER) == LOW || digitalRead(BUTTON_PIN_RIGHT) == LOW ||
         digitalRead(BUTTON_PIN_LEFT) == LOW;
}
void wakeDisplay() {}
void checkIdle() {}

const int ITEM_HEIGHT = 16, ITEM_SPACING = 2, TEXT_X = 28, SELECTION_X = 4;
const int SELECTION_WIDTH = 120, ICON_X = 8;

#define EEPROM_ADDR_AIRPLANE   20
#define EEPROM_ADDR_TROLL      21
#define EEPROM_ADDR_BRIGHTNESS 1
#define OPTIONS_HOLD_MS        1000
#define TROLL_DISABLE_HOLD_MS  4000
#define BRIGHTNESS_MIN_PCT     5
#define BRIGHTNESS_MAX_PCT     100

bool airplaneModeEnabled = false;
bool trollModeEnabled    = false;
bool optionsMenuActive   = false;
int  optionsSelected     = 0;
int  optionsSubMenu      = 0;
int  brightnessPercent   = 50;
unsigned long optionsOpenAnimStart = 0;

unsigned long upHoldStart = 0;   bool upHolding = false;
unsigned long downHoldStart = 0; bool downHolding = false;
unsigned long selHoldStart = 0;  bool selHolding = false;

void loadOptionsFromEEPROM() {
  uint8_t air = EEPROM.read(EEPROM_ADDR_AIRPLANE);
  uint8_t tr  = EEPROM.read(EEPROM_ADDR_TROLL);
  airplaneModeEnabled = (air == 1);
  trollModeEnabled    = (tr == 1);
  uint8_t b = EEPROM.read(EEPROM_ADDR_BRIGHTNESS);
  if (b == 0xFF) b = 128;
  oledBrightness = b;
  brightnessPercent = (int)map(oledBrightness, 0, 255, 0, 100);
  u8g2.setContrast(oledBrightness);
}
void saveBrightness() {
  oledBrightness = (uint8_t)map(brightnessPercent, 0, 100, 0, 255);
  if (oledBrightness < 13) oledBrightness = 13;
  u8g2.setContrast(oledBrightness);
  EEPROM.write(EEPROM_ADDR_BRIGHTNESS, oledBrightness);
  EEPROM.commit();
}
void setAirplaneMode(bool on) {
  airplaneModeEnabled = on;
  EEPROM.write(EEPROM_ADDR_AIRPLANE, on ? 1 : 0);
  EEPROM.commit();
}
void setTrollMode(bool on) {
  trollModeEnabled = on;
  EEPROM.write(EEPROM_ADDR_TROLL, on ? 1 : 0);
  EEPROM.commit();
}

enum AppMenuState { APP_MAIN, APP_BLE, APP_WIFI, APP_OTHER, APP_LEVEL, APP_OPTIONS };
AppMenuState currentState = APP_MAIN;

struct MenuItem {
  const char* name;
  const unsigned char* icon;
  void (*setup)();
  void (*loop)();
  void (*cleanup)();
};
MenuItem* currentMenuItems = nullptr;
int currentMenuSize = 0;
int item_selected = 0;
int mainMenuColumn = 0;

constexpr uint8_t BUTTON_UP = BUTTON_PIN_UP, BUTTON_SEL = BUTTON_PIN_CENTER;
constexpr uint8_t BUTTON_DOWN = BUTTON_PIN_DOWN, BUTTON_RIGHT = BUTTON_PIN_RIGHT;
constexpr uint8_t BUTTON_LEFT = BUTTON_PIN_LEFT;

static unsigned long appStartTime = 0, lastXPReward = 0;
static const char* currentAppName = "";
static bool inApplication = false;
static int pendingXP = 0;
const unsigned long XP_REWARD_INTERVAL = 1000;

bool justPressed(uint8_t pin, bool &prev, unsigned long &debounceTime) {
  bool now = digitalRead(pin) == LOW;
  unsigned long t = millis();
  if (now != prev && (t - debounceTime) > debounceDelay) {
    debounceTime = t; prev = now; return now;
  }
  return false;
}

bool isDangerousApp(const char* n) {
  return strstr(n,"Deauth")||strstr(n,"Beacon")||strstr(n,"Spam")||
         strstr(n,"Spoofer")||strstr(n,"Evil Portal")||strstr(n,"SigKill")||
         strstr(n,"Deauther");
}
bool isReconApp(const char* n) {
  return strstr(n,"Scan")||strstr(n,"Detector")||strstr(n,"Inspector")||strstr(n,"Scout");
}
bool isOffensiveApp(const char* n) { return isDangerousApp(n); }
bool shouldShowApp(const char* n) {
  return !isDangerousApp(n) || isDangerousActionsEnabled();
}
void noCleanup() {}

// Forward decls for cleanup helpers provided by modules
extern void cleanupBLE();
extern void cleanupWiFi();
extern void cleanupRadio();
extern void cleanupEvilPortal();
extern void cleanupDroneDetector();
extern void cleanupDroneSpoofer();
extern void cleanupFlockDetector();
extern void cleanupDeviceScout();
extern void wifiscanCleanup();
extern void clientSnifferCleanup();
extern void superscannerCleanup();
extern void rfidCleanup();
extern void reaperUiCleanup();
extern void aboutCleanup();

// Exact original menu tables (function names match existing .cpp modules)
MenuItem mainMenu[] = {
  { "WiFi",  bitmap_icon_wifi,     nullptr, nullptr, noCleanup },
  { "BLE",   bitmap_icon_ble,      nullptr, nullptr, noCleanup },
  { "Other", bitmap_icon_analyzer, nullptr, nullptr, noCleanup }
};
constexpr int MAIN_MENU_SIZE = 3;

// RFID entry — RC522 SS=15 RST=16, SPI shared with NRF24.
MenuItem shortcutMenu[] = {
  { "RFID",   bitmap_icon_rfid,  rfidSetup,     rfidLoop,     rfidCleanup },
  { "Reaper", bitmap_icon_skull, reaperUiSetup, reaperUiLoop, reaperUiCleanup }
};
constexpr int SHORTCUT_MENU_SIZE = 2;

MenuItem wifiMenu[] = {
  { "Scanner",             nullptr, wifiscanSetup,           wifiscanLoop,           wifiscanCleanup },
  { "Deauther",            nullptr, deauthSetup,             deauthLoop,             cleanupWiFi },
  { "Handshake Capture",   nullptr, handshakeCaptureSetup,   handshakeCaptureLoop,   cleanupWiFi },
  { "Client Sniffer",      nullptr, clientSnifferSetup,      clientSnifferLoop,      clientSnifferCleanup },
  { "Deauth Scanner",      nullptr, deauthScannerSetup,      deauthScannerLoop,      cleanupWiFi },
  { "Beacon Spam",         nullptr, beaconSpamSetup,         beaconSpamLoop,         cleanupWiFi },
  { "Evil Portal",         nullptr, evilPortalSetup,         evilPortalLoop,         cleanupEvilPortal },
  { "Pineapple Detector",  nullptr, pineappleDetectorSetup,  pineappleDetectorLoop,  cleanupWiFi },
  { "Pwnagotchi Detector", nullptr, pwnagotchiDetectorSetup, pwnagotchiDetectorLoop, cleanupWiFi },
  { "Pwnagotchi Spam",     nullptr, pwnagotchiSpamSetup,     pwnagotchiSpamLoop,     cleanupWiFi },
  { "Back",                nullptr, nullptr,                 nullptr,                noCleanup }
};
constexpr int WIFI_MENU_SIZE = sizeof(wifiMenu) / sizeof(wifiMenu[0]);

MenuItem bleMenu[] = {
  { "BLE Scan",              nullptr, blescanSetup,              blescanLoop,              cleanupBLE },
  { "BLE Inspector",         nullptr, bleInspectorSetup,         bleInspectorLoop,         cleanupBLE },
  { "doomBOX Detector",      nullptr, nyanboxDetectorSetup,      nyanboxDetectorLoop,      cleanupBLE },
  { "Flipper Zero Detector", nullptr, flipperZeroDetectorSetup,  flipperZeroDetectorLoop,  cleanupBLE },
  { "Axon Detector",         nullptr, axonDetectorSetup,         axonDetectorLoop,         cleanupBLE },
  { "Meshtastic Detector",   nullptr, meshtasticDetectorSetup,   meshtasticDetectorLoop,   cleanupBLE },
  { "MeshCore Detector",     nullptr, meshcoreDetectorSetup,     meshcoreDetectorLoop,     cleanupBLE },
  { "Skimmer Detector",      nullptr, cardskimmerDetectorSetup,  cardskimmerDetectorLoop,  cleanupBLE },
  { "AirTag Detector",       nullptr, airtagDetectorSetup,       airtagDetectorLoop,       cleanupBLE },
  { "AirTag Spoofer",        nullptr, airtagSpooferSetup,        airtagSpooferLoop,        cleanupBLE },
  { "SmartTag Detector",     nullptr, smarttagDetectorSetup,     smarttagDetectorLoop,     cleanupBLE },
  { "Tile Detector",         nullptr, tileDetectorSetup,         tileDetectorLoop,         cleanupBLE },
  { "RayBan Detector",       nullptr, raybanDetectorSetup,       raybanDetectorLoop,       cleanupBLE },
  { "BLE Spammer",           nullptr, bleSpamSetup,              bleSpamLoop,              cleanupBLE },
  { "Swift Pair",            nullptr, swiftpairSpamSetup,        swiftpairSpamLoop,        cleanupBLE },
  { "Sour Apple",            nullptr, sourappleSetup,            sourappleLoop,            cleanupBLE },
  { "Sour Droid",            nullptr, sourDroidSetup,            sourDroidLoop,            cleanupBLE },
  { "BLE Spoofer",           nullptr, bleSpooferSetup,           bleSpooferLoop,           cleanupBLE },
  { "Back",                  nullptr, nullptr,                   nullptr,                  noCleanup }
};
constexpr int BLE_MENU_SIZE = sizeof(bleMenu) / sizeof(bleMenu[0]);

MenuItem otherMenu[] = {
  { "SigKill",         nullptr, sigkillSetup,         sigkillLoop,         cleanupRadio },
  { "Superscanner",    nullptr, superscannerSetup,    superscannerLoop,    superscannerCleanup },
  { "Drone Detector",  nullptr, droneDetectorSetup,   droneDetectorLoop,   cleanupDroneDetector },
  { "Drone Spoofer",   nullptr, droneSpooferSetup,    droneSpooferLoop,    cleanupDroneSpoofer },
  { "Flock Detector",  nullptr, flockDetectorSetup,   flockDetectorLoop,   cleanupFlockDetector },
  { "Device Scout",    nullptr, deviceScoutSetup,     deviceScoutLoop,     cleanupDeviceScout },
  { "Setting",         nullptr, settingSetup,         settingLoop,         noCleanup },
  { "About",           nullptr, aboutSetup,           aboutLoop,           aboutCleanup },
  { "Back",            nullptr, nullptr,              nullptr,             noCleanup }
};
constexpr int OTHER_MENU_SIZE = sizeof(otherMenu) / sizeof(otherMenu[0]);

int getVisibleMenuSize() {
  if (currentState == APP_MAIN) {
    if (mainMenuColumn == 1) {
      int c = 0;
      for (int i = 0; i < SHORTCUT_MENU_SIZE; i++)
        if (shouldShowApp(shortcutMenu[i].name)) c++;
      return c;
    }
    if (airplaneModeEnabled) return 1;
    return MAIN_MENU_SIZE;
  }
  int c = 0;
  for (int i = 0; i < currentMenuSize; i++)
    if (shouldShowApp(currentMenuItems[i].name)) c++;
  return c;
}

MenuItem* getVisibleMenuItem(int visibleIndex) {
  if (currentState == APP_MAIN && mainMenuColumn == 1) {
    int vc = 0;
    for (int i = 0; i < SHORTCUT_MENU_SIZE; i++) {
      if (shouldShowApp(shortcutMenu[i].name)) {
        if (vc == visibleIndex) return &shortcutMenu[i];
        vc++;
      }
    }
    return &shortcutMenu[0];
  }
  if (currentState == APP_MAIN && mainMenuColumn == 0 && airplaneModeEnabled)
    return &mainMenu[2];
  int vc = 0;
  for (int i = 0; i < currentMenuSize; i++) {
    if (shouldShowApp(currentMenuItems[i].name)) {
      if (vc == visibleIndex) return &currentMenuItems[i];
      vc++;
    }
  }
  return currentMenuItems ? &currentMenuItems[0] : &mainMenu[0];
}

void startAppTracking(const char* name) {
  currentAppName = name; inApplication = true;
  appStartTime = lastXPReward = millis(); pendingXP = 0;
}
void stopAppTracking() { inApplication = false; currentAppName = ""; }
void updateAppXP() {
  if (!inApplication) return;
  if (millis() - lastXPReward >= XP_REWARD_INTERVAL) {
    pendingXP++; lastXPReward = millis();
  }
}

void enterMenu(AppMenuState st) {
  const char* prevName = nullptr;
  if (currentMenuItems && currentMenuSize > 0 &&
      item_selected >= 0 && item_selected < getVisibleMenuSize()) {
    MenuItem* p = getVisibleMenuItem(item_selected);
    if (p) prevName = p->name;
  }
  currentState = st;
  switch (st) {
    case APP_MAIN:  currentMenuItems = mainMenu;  currentMenuSize = MAIN_MENU_SIZE;  mainMenuColumn = 0; break;
    case APP_WIFI:  currentMenuItems = wifiMenu;  currentMenuSize = WIFI_MENU_SIZE;  break;
    case APP_BLE:   currentMenuItems = bleMenu;   currentMenuSize = BLE_MENU_SIZE;   break;
    case APP_OTHER: currentMenuItems = otherMenu; currentMenuSize = OTHER_MENU_SIZE; break;
    default: break;
  }
  if (st == APP_MAIN) {
    if (!advertiserBootPending) startNyanboxAdvertiser();
  } else {
    stopNyanboxAdvertiser();
  }
  item_selected = 0;
  if (prevName && st != APP_MAIN) {
    for (int i = 0; i < getVisibleMenuSize(); i++)
      if (strcmp(getVisibleMenuItem(i)->name, prevName) == 0) { item_selected = i; break; }
  }
  needsRedraw = true;
}

void runApp(MenuItem &mi) {
  if (!mi.setup) return;
  startAppTracking(mi.name);
  if (isReconApp(mi.name)) blinkColor(0, 0, 255);
  else if (isOffensiveApp(mi.name)) blinkColor(255, 0, 0);

  // RFID: RC522 now on SS=15 / RST=16 (pindefs.h). Shared SPI SCK=18 MISO=19 MOSI=23.
  // OLED remains on I2C 21/22. GPIO2 is not used.

  mi.setup();
  updateLastActivity();
  u8g2.setPowerSave(0);
  if (!mi.loop) return;
  while (digitalRead(BUTTON_SEL) == LOW);

  while (true) {
    updateAppXP();
    neopixelLoop();
    if (anyButtonPressed()) updateLastActivity();
    mi.loop();
    if (digitalRead(BUTTON_SEL) == LOW) {
      while (digitalRead(BUTTON_SEL) == LOW);
      if (mi.cleanup) mi.cleanup();
      break;
    }
  }
  stopBlinking();
  stopAppTracking();
  u8g2.clearBuffer();
}

void drawSelection(int x, int y, int w, int h, bool) {
  u8g2.drawBox(x, y + 2, 2, h - 4);
}

void applyDoomTrollOverlay() {
  if (!trollModeEnabled && !gCfg.troll) return;

  const uint32_t now = millis();
  static uint8_t  phase = 0;
  static uint32_t lastMs = 0;
  // local xorshift
  static uint32_t r = 0xC0FFEEu;
  auto rr = [&]() -> uint32_t {
    r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r;
  };
  if (now - lastMs >= 50 + (rr() % 40)) {
    lastMs = now;
    phase++;
    if ((phase & 7) == 0) r ^= now;
  }

  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.setFontMode(1);

  if ((phase & 0x1F) == 0 && (rr() & 3) == 0) {
    u8g2.drawBox(0, 0, 128, 64);
    return;
  }
  if ((phase & 0x0F) == 3 && (rr() & 7) == 0) {
    int y = (int)(rr() % 48);
    int h = 8 + (int)(rr() % 24);
    u8g2.drawBox(0, y, 128, h);
  }

  const int nBlocks = 2 + (int)(rr() % 5);
  for (int i = 0; i < nBlocks; ++i) {
    int w = 4 + (int)(rr() % 40);
    int h = 2 + (int)(rr() % 14);
    int x = (int)(rr() % (128 - w + 1));
    int y = (int)(rr() % (64 - h + 1));
    if (rr() & 1) u8g2.drawBox(x, y, w, h);
    else          u8g2.drawFrame(x, y, w, h);
  }

  const int nH = 2 + (int)(rr() % 4);
  for (int i = 0; i < nH; ++i) {
    int y = (int)(rr() % 64);
    int h = 1 + (int)(rr() % 3);
    int x0 = (int)(rr() % 32);
    int w  = 64 + (int)(rr() % (128 - x0));
    for (int dy = 0; dy < h; ++dy)
      if (y + dy < 64) u8g2.drawHLine(x0, y + dy, w);
  }

  const int nV = 1 + (int)(rr() % 3);
  for (int i = 0; i < nV; ++i) {
    int x = (int)(rr() % 128);
    int y0 = (int)(rr() % 32);
    int ht = 16 + (int)(rr() % 48);
    u8g2.drawVLine(x, y0, ht);
    if ((rr() & 1) && x + 1 < 128) u8g2.drawVLine(x + 1, y0, ht);
  }

  const int nStr = 1 + (int)(rr() % 4);
  char buf[18];
  for (int s = 0; s < nStr; ++s) {
    const int len = 4 + (int)(rr() % 12);
    for (int c = 0; c < len && c < 17; ++c)
      buf[c] = (rr() & 1) ? '1' : '0';
    buf[len < 17 ? len : 17] = 0;
    u8g2.drawStr((int)(rr() % 100), 6 + (int)(rr() % 58), buf);
  }
  if ((rr() & 3) == 0) {
    static const char hexc[] = "0123456789ABCDEF";
    char hx[9];
    for (int i = 0; i < 8; ++i) hx[i] = hexc[rr() % 16];
    hx[8] = 0;
    u8g2.drawStr((int)(rr() % 90), 6 + (int)(rr() % 58), hx);
  }

  const int nPix = 8 + (int)(rr() % 20);
  for (int i = 0; i < nPix; ++i)
    u8g2.drawPixel((int)(rr() % 128), (int)(rr() % 64));

  if ((phase & 3) == 1) {
    int y0 = (int)(rr() % 50);
    int h  = 6 + (int)(rr() % 14);
    for (int y = y0; y < y0 + h && y < 64; y += 2)
      u8g2.drawHLine(0, y, 128);
  }
}

void enterDoomboxMode() {
  handoffToDoombox();

  pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
  pinMode(BUTTON_PIN_CENTER, INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);

  // Wait until mode-switch SELECT (and any other key) is released, otherwise
  // doomboxLoop treats it as "open first item" and jumps straight into WiFi.
  {
    const unsigned long t0 = millis();
    while (millis() - t0 < 3000) {
      if (digitalRead(BUTTON_PIN_UP) != LOW && digitalRead(BUTTON_PIN_DOWN) != LOW &&
          digitalRead(BUTTON_PIN_LEFT) != LOW && digitalRead(BUTTON_PIN_RIGHT) != LOW &&
          digitalRead(BUTTON_PIN_CENTER) != LOW) {
        break;
      }
      delay(10);
    }
    delay(80);
  }
  // Reset edge detectors so no stale "just pressed"
  selPrev = rightPrev = leftPrev = false;
  upPressed = downPressed = false;
  upHolding = downHolding = selHolding = false;
  optionsMenuActive = false;
  mainMenuColumn = 0;
  item_selected = 0;
  currentState = APP_MAIN;

  if (!doomboxInitialized) {
    neopixelSetup();
    nrf24Begin();
    EEPROM.begin(512);
    oledBrightness = EEPROM.read(1);
    if (oledBrightness == 0xFF) oledBrightness = 128;
    dangerousActionsEnabled = true;

    // Display already begun by unified setup — do NOT call u8g2.begin() again
    u8g2.setContrast(oledBrightness);
    u8g2.setBitmapMode(1);
    loadOptionsFromEEPROM();
    updateLastActivity();

    // Opening cutscene (preserved)
    u8g2.clearBuffer();
    u8g2.drawBitmap(0, 0, 16, 64, logo_doombox);
    u8g2.sendBuffer();
    delay(2000);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    { const char* p = "presents";
      int16_t pw = u8g2.getUTF8Width(p);
      u8g2.setCursor((128 - pw) / 2, 34); u8g2.print(p); }
    u8g2.sendBuffer();
    delay(1500);

    u8g2.clearBuffer();
    u8g2.drawBitmap(0, 0, 16, 64, doombox_bitmap);
    u8g2.sendBuffer();
    delay(2000);

    levelSystemSetup();
    initNyanboxAdvertiser();
    if (passwordEnabled()) checkPasswordOnBoot();
    displayMirrorSetup();
    doomboxInitialized = true;
  }

  airplaneModeEnabled = gCfg.airplane;
  trollModeEnabled = gCfg.troll;
  enterMenu(APP_MAIN);
  needsRedraw = true;
  advertiserBootPending = false;
}

void exitDoomboxMode() {
  // Leave BLE / WiFi / NRF running (per clarified requirement)
  stopNyanboxAdvertiser();
  stopBlinking();
  handoffToFlipbox();
  // Wait for keys up so flipBOX does not see a residual SELECT
  {
    const unsigned long t0 = millis();
    while (millis() - t0 < 3000) {
      if (digitalRead(BUTTON_PIN_UP) != LOW && digitalRead(BUTTON_PIN_DOWN) != LOW &&
          digitalRead(BUTTON_PIN_LEFT) != LOW && digitalRead(BUTTON_PIN_RIGHT) != LOW &&
          digitalRead(BUTTON_PIN_CENTER) != LOW) {
        break;
      }
      delay(10);
    }
    delay(80);
  }
  gateHeldInputs();
}

void doomboxLoop() {
  unsigned long currentTime = millis();

  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "MIRROR_ON")  { displayMirrorEnable(true);  needsRedraw = true; }
    else if (cmd == "MIRROR_OFF") displayMirrorEnable(false);
  }

  updateNyanboxAdvertiser();
  neopixelLoop();

  bool upNow   = digitalRead(BUTTON_PIN_UP) == LOW;
  bool downNow = digitalRead(BUTTON_PIN_DOWN) == LOW;
  bool selNow  = digitalRead(BUTTON_PIN_CENTER) == LOW;

  // Troll global disable (hold SELECT 4s)
  if (trollModeEnabled && selNow) {
    if (!selHolding) { selHolding = true; selHoldStart = currentTime; }
    else if (currentTime - selHoldStart >= TROLL_DISABLE_HOLD_MS) {
      setTrollMode(false); selHolding = false;
      while (digitalRead(BUTTON_PIN_CENTER) == LOW) delay(10);
      needsRedraw = true;
    }
  } else if (!selNow) selHolding = false;

  // ---- Options menu (original doomBOX options; mode-return via hold SEL) ----
  if (optionsMenuActive) {
    // Shared GUI + NVS with flipBOX — no text labels
    airplaneModeEnabled = gCfg.airplane;
    trollModeEnabled    = gCfg.troll;

    // Open roll-down animation
    if (gSetAnim < 255) {
      uint32_t el = millis() - optionsOpenAnimStart;
      gSetAnim = (uint8_t)((el >= 220) ? 255 : (el * 255 / 220));
    }

    // DOWN: short press = navigate, long press = exit (bar after 500 ms)
    gOptHoldBarPct = 0;
    static bool downNavArmed = false;
    if (downNow) {
      if (!downHolding) {
        downHolding = true;
        downHoldStart = currentTime;
        downNavArmed = true;  // allow one short-press move on release if < 500ms
      } else {
        uint32_t held = currentTime - downHoldStart;
        if (held >= 1000) {
          optionsMenuActive = false;
          downHolding = false;
          downNavArmed = false;
          gOptHoldBarPct = 0;
          needsRedraw = true;
          while (digitalRead(BUTTON_PIN_DOWN) == LOW) delay(10);
          return;
        } else if (held >= 500) {
          downNavArmed = false;  // this is a hold, not a short press
          gOptHoldBarPct = (uint8_t)((held - 500) * 100 / 500);
        }
      }
    } else if (downHolding) {
      // released
      uint32_t held = currentTime - downHoldStart;
      if (downNavArmed && held < 500) {
        // short press → move selection down a row
        if (gOptSel < 3) gOptSel = (uint8_t)(gOptSel + 3);
        else gOptSel = (uint8_t)(gOptSel - 3);
      }
      downHolding = false;
      downNavArmed = false;
      gOptHoldBarPct = 0;
    }

    if (justPressed(BUTTON_LEFT, leftPrev, leftDebounceTime)) {
      if (gOptSel % 3 > 0) gOptSel--;
      else gOptSel = (uint8_t)(gOptSel + 2);
    }
    if (justPressed(BUTTON_RIGHT, rightPrev, rightDebounceTime)) {
      if (gOptSel % 3 < 2) gOptSel++;
      else gOptSel = (uint8_t)(gOptSel - 2);
    }
    if (justPressed(BUTTON_UP, upPressed, upDebounceTime)) {
      if (gOptSel >= 3) gOptSel = (uint8_t)(gOptSel - 3);
      else gOptSel = (uint8_t)(gOptSel + 3);
    }
    // DOWN navigation handled above via short-press-on-release

    if (justPressed(BUTTON_SEL, selPrev, selDebounceTime)) {
      if (kOptSelectable[gOptSel]) {
        switch (gOptSel) {
          case 0: {
            gCfg.brightness = (uint8_t)(gCfg.brightness + 10);
            if (gCfg.brightness > 100) gCfg.brightness = 10;
            if (gCfg.brightness < 1) gCfg.brightness = 1;
            saveSettings();
            applyBrightness();
            break;
          }
          case 1:
            gCfg.passwordOn = !gCfg.passwordOn;
            saveSettings();
            break;
          case 2:
            gCfg.airplane = !gCfg.airplane;
            airplaneModeEnabled = gCfg.airplane;
            saveSettings();
            break;
          case 3:
            gCfg.troll = !gCfg.troll;
            trollModeEnabled = gCfg.troll;
            saveSettings();
            break;
          case 4:
            optionsMenuActive = false;
            gOptHoldBarPct = 0;
            saveSystemMode(SYS_FLIPBOX);
            exitDoomboxMode();
            return;
          default:
            break;  // index 5: hover only
        }
      }
    }

    drawSettings();
    if (gCfg.troll) applyDoomTrollOverlay();
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
    return;
  }

  // ---- Main / sub-menu navigation ----
  if (currentState == APP_MAIN) {
    if (upNow) {
      updateLastActivity();
      if (!upHolding) { upHolding = true; upHoldStart = currentTime; }
      else if (currentTime - upHoldStart >= 500) {
        needsRedraw = true;  // show open-options hold bar
      }
      if (upHolding && currentTime - upHoldStart >= OPTIONS_HOLD_MS) {
        optionsMenuActive = true; optionsSelected = 0; optionsSubMenu = 0;
        optionsOpenAnimStart = currentTime; gOptSel = 0; gSetAnim = 0;
        gOptHoldBarPct = 0;
        upHolding = false; needsRedraw = true;
        while (digitalRead(BUTTON_PIN_UP) == LOW) delay(10);
        return;
      }
    } else {
      if (upHolding && (currentTime - upHoldStart) < 300) {
        int vis = getVisibleMenuSize();
        if (vis > 0) item_selected = (item_selected + vis - 1) % vis;
        needsRedraw = true;
      }
      upHolding = false;
    }
    if (justPressed(BUTTON_DOWN, downPressed, downDebounceTime)) {
      int vis = getVisibleMenuSize();
      if (vis > 0) item_selected = (item_selected + 1) % vis;
      needsRedraw = true; updateLastActivity();
    }
    if (justPressed(BUTTON_LEFT, leftPrev, leftDebounceTime)) {
      mainMenuColumn = 0; item_selected = 0; needsRedraw = true;
    }
    if (justPressed(BUTTON_RIGHT, rightPrev, rightDebounceTime)) {
      mainMenuColumn = 1; item_selected = 0; needsRedraw = true;
    }
    if (justPressed(BUTTON_SEL, selPrev, selDebounceTime)) {
      MenuItem* mi = getVisibleMenuItem(item_selected);
      if (mi) {
        if (mainMenuColumn == 0) {
          if (strcmp(mi->name, "WiFi") == 0) enterMenu(APP_WIFI);
          else if (strcmp(mi->name, "BLE") == 0) enterMenu(APP_BLE);
          else if (strcmp(mi->name, "Other") == 0) enterMenu(APP_OTHER);
        } else if (mi->setup) {
          runApp(*mi); needsRedraw = true;
        }
      }
      updateLastActivity();
    }
  } else {
    // Sub-menu
    if (justPressed(BUTTON_UP, upPressed, upDebounceTime)) {
      int vis = getVisibleMenuSize();
      if (vis > 0) item_selected = (item_selected + vis - 1) % vis;
      needsRedraw = true; updateLastActivity();
    }
    if (justPressed(BUTTON_DOWN, downPressed, downDebounceTime)) {
      int vis = getVisibleMenuSize();
      if (vis > 0) item_selected = (item_selected + 1) % vis;
      needsRedraw = true; updateLastActivity();
    }
    if (justPressed(BUTTON_SEL, selPrev, selDebounceTime)) {
      MenuItem* mi = getVisibleMenuItem(item_selected);
      if (mi) {
        if (strcmp(mi->name, "Back") == 0) enterMenu(APP_MAIN);
        else if (mi->setup) { runApp(*mi); needsRedraw = true; }
      }
      updateLastActivity();
    }
    if (justPressed(BUTTON_LEFT, leftPrev, leftDebounceTime)) enterMenu(APP_MAIN);
  }

  // ---- Draw ----
  if (needsRedraw) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    if (currentState == APP_MAIN) {
      const int leftX = 2, rightX = 66;
      int drawIdx = 0;
      for (int i = 0; i < MAIN_MENU_SIZE; i++) {
        if (airplaneModeEnabled && i < 2) continue;
        int itemY = 6 + (drawIdx * (ITEM_HEIGHT + ITEM_SPACING));
        int textY = itemY + 11;
        bool focused = (mainMenuColumn == 0 && item_selected == drawIdx);
        if (focused) u8g2.drawBox(leftX, itemY + 2, 2, ITEM_HEIGHT - 4);
        if (mainMenu[i].icon) u8g2.drawXBMP(leftX + 4, itemY, 16, 16, mainMenu[i].icon);
        u8g2.drawStr(leftX + 22, textY, mainMenu[i].name);
        drawIdx++;
      }
      for (int i = 0; i < SHORTCUT_MENU_SIZE; i++) {
        if (!shouldShowApp(shortcutMenu[i].name)) continue;
        int itemY = 6 + (i * (ITEM_HEIGHT + ITEM_SPACING));
        int textY = itemY + 11;
        bool focused = (mainMenuColumn == 1 && item_selected == i);
        if (focused) u8g2.drawBox(rightX, itemY + 2, 2, ITEM_HEIGHT - 4);
        if (shortcutMenu[i].icon) u8g2.drawXBMP(rightX + 4, itemY, 16, 16, shortcutMenu[i].icon);
        u8g2.drawStr(rightX + 22, textY, shortcutMenu[i].name);
      }
    } else {
      int vis = getVisibleMenuSize();
      int start = (item_selected == 0) ? 0 :
                  (item_selected == vis - 1) ? max(0, vis - 3) : item_selected - 1;
      int highlight = item_selected - start;
      drawSelection(SELECTION_X, 6 + highlight * (ITEM_HEIGHT + ITEM_SPACING),
                    SELECTION_WIDTH, ITEM_HEIGHT, true);
      for (int i = 0; i < 3; i++) {
        int idx = start + i;
        if (idx < vis) {
          MenuItem* item = getVisibleMenuItem(idx);
          int itemY = 6 + (i * (ITEM_HEIGHT + ITEM_SPACING));
          u8g2.drawStr(TEXT_X, itemY + 11, item->name);
          if (item->icon) u8g2.drawXBMP(ICON_X, itemY, 16, 16, item->icon);
        }
      }
    }
    // Hold-UP progress to open options (same feel as flipBOX)
    if (currentState == APP_MAIN && upHolding) {
      uint32_t held = millis() - upHoldStart;
      if (held >= 500 && held < OPTIONS_HOLD_MS) {
        uint8_t pct = (uint8_t)((held - 500) * 100 / (OPTIONS_HOLD_MS - 500));
        u8g2.drawFrame(24, 60, 80, 4);
        int fill = 78 * pct / 100;
        if (fill > 0) u8g2.drawBox(25, 61, fill, 2);
      }
    }
    applyDoomTrollOverlay();
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
    needsRedraw = false;
  }
}

// ============================================================================
//  9. ARDUINO ENTRY POINTS
// ============================================================================

// ============================================================================

// flipBOX boot cutscene (same structure as doomBOX: RG → presents → product logo)
static bool flipboxCutsceneDone = false;

static void playFlipboxCutscene() {
  if (flipboxCutsceneDone) return;
  flipboxCutsceneDone = true;

  // Scene 1: RG logo
  u8g2.clearBuffer();
  u8g2.drawBitmap(0, 0, 16, 64, logo_doombox);
  u8g2.sendBuffer();
  delay(2000);

  // Scene 2: "presents"
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvR08_tr);
  {
    const char* p = "presents";
    int16_t pw = u8g2.getUTF8Width(p);
    u8g2.setCursor((128 - pw) / 2, 34);
    u8g2.print(p);
  }
  u8g2.sendBuffer();
  delay(1500);

  // Scene 3: flipBOX wordmark (100x31, 13 bytes/row), centered
  u8g2.clearBuffer();
  {
    const int x = (128 - FLIPBOX_W) / 2;
    const int y = (64 - FLIPBOX_H) / 2;
    u8g2.drawBitmap(x, y, FLIPBOX_ROW_BYTES, FLIPBOX_H, flipBOX_logo);
  }
  u8g2.sendBuffer();
  delay(2000);
}

void setup() {
  Serial.begin(115200);
  inputInit();
  loadSettings();
  loadSystemMode();   // restore last mode (default flipBOX)

#if FB_ASYNC_DISPLAY && FB_FAST_FLUSH
  {
    uint16_t chunk = FB_I2C_CHUNK;
    if (chunk > 1024) chunk = 1024;
    size_t got = Wire.setBufferSize((size_t)chunk + 1);
    if (got == 0) got = 128;
    while (chunk > 16 && (size_t)chunk + 1 > got) chunk = (uint16_t)(chunk >> 1);
    gFlushChunk = chunk;
  }
#endif

  // Single display begin for the whole firmware
  gDisp.begin();
  gDisp.setBusClock(FB_I2C_HZ);
  Wire.setClock(FB_I2C_HZ);

#if FB_OLED_OSC
  {
    u8x8_t* const u8x8 = gDisp.getU8x8();
    u8x8_cad_StartTransfer(u8x8);
    u8x8_cad_SendCmd(u8x8, 0x0D5);
    u8x8_cad_SendArg(u8x8, (uint8_t)FB_OLED_OSC);
    u8x8_cad_EndTransfer(u8x8);
  }
#endif

  applyBrightness();
  gDisp.clearBuffer();

#if FB_ASYNC_DISPLAY
  gFlushDone = xSemaphoreCreateBinary();
  xSemaphoreGive(gFlushDone);
  xTaskCreatePinnedToCore(flushTask, "fbOLED", 4096, nullptr, 2, &gFlushTaskHandle, 0);
#endif

  // Boot into the persisted subsystem
  if (gSystemMode == SYS_DOOMBOX) {
    enterDoomboxMode();
  } else {
    // Suspend async flush for blocking cutscene frames (same as doomBOX path)
#if FB_ASYNC_DISPLAY
    if (gFlushDone) xSemaphoreTake(gFlushDone, portMAX_DELAY);
    if (gFlushTaskHandle) vTaskSuspend(gFlushTaskHandle);
#endif
    playFlipboxCutscene();
#if FB_ASYNC_DISPLAY
    if (gFlushTaskHandle) vTaskResume(gFlushTaskHandle);
    if (gFlushDone) xSemaphoreGive(gFlushDone);
#endif
    if (gCfg.passwordOn) {
      gMode = UI_PASSWORD;
      gPwdIdx = 0;
    } else {
      gMode = UI_MENU;
      gMenuDirty = true;
    }
  }
}

void loop() {
  // Top-level branch on system mode (MD Step 2)
  if (gSystemMode == SYS_DOOMBOX) {
    doomboxLoop();
    // If doomboxLoop requested a return to flipBOX it already called
    // exitDoomboxMode() + saveSystemMode(SYS_FLIPBOX).  Restore UI state.
    if (gSystemMode == SYS_FLIPBOX) {
      if (gCfg.passwordOn) { gMode = UI_PASSWORD; gPwdIdx = 0; }
      else { gMode = UI_MENU; gMenuDirty = true; }
    }
    return;
  }

  // ----- SYS_FLIPBOX -----
  // Detect mode change requested from options (case 4)
  static SystemMode lastSeen = SYS_FLIPBOX;
  if (gSystemMode != lastSeen) {
    lastSeen = gSystemMode;
    if (gSystemMode == SYS_DOOMBOX) {
      enterDoomboxMode();
      return;
    }
  }

  if (gMode == UI_GAME) waitForLatch();
  const uint32_t now = micros();
  pollInput(now);

  switch (gMode) {
    case UI_PASSWORD:   runPassword();   break;
    case UI_MENU:       runMenu();       break;
    case UI_SETTINGS:   runSettings();   break;
    case UI_BRIGHTNESS: runBrightness(); break;
    case UI_GAME:       runGame(now);    break;
  }
}
