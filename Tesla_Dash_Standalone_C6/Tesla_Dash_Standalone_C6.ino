#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include "driver/twai.h"

// =====================================================
// Tesla Dash Standalone Controller fuer ESP32-C6
// Liest VehicleBus + ChassisBus selbst und steuert den
// Dashboard-LED-Streifen direkt. Kein Master, kein ESP-NOW.
// =====================================================

#define VEHICLE_CAN_TX_PIN GPIO_NUM_1
#define VEHICLE_CAN_RX_PIN GPIO_NUM_2

#define CHASSIS_CAN_TX_PIN  GPIO_NUM_15
#define CHASSIS_CAN_RX_PIN  GPIO_NUM_14

#define LED_PIN 4
#define NUM_LEDS 122
#define DEFAULT_DASH_LED_COUNT 122
#define LED_TYPE NEO_GRB + NEO_KHZ800

#define MAX_LED_BRIGHTNESS_PERCENT 15
#define MAX_LED_BRIGHTNESS ((255 * MAX_LED_BRIGHTNESS_PERCENT) / 100)
#define MIN_AUTO_LED_BRIGHTNESS 2
#define CHARGE_GREEN_AT_PERCENT 75
#define CHASSIS_CAN_TIMEOUT_MS 5000
#define DASH_TRANSITION_MS 250
#define DASH_TRANSITION_FRAME_MS 18
#define CHARGE_ANIMATION_TIMEOUT_MS 1800000UL

const char* WIFI_SSID = "Tesla-Dash-C6";
const char* WIFI_PASS = "12345678";

WebServer server(80);
Preferences prefs;
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, LED_TYPE);

// CAN IDs
#define ID_BLINKER          0x3F5
#define ID_BRIGHTNESS       0x273
#define ID_GEAR             0x118
#define ID_VCLEFT_STATUS    0x102
#define ID_VCRIGHT_STATUS   0x103
#define ID_BMS_STATUS       0x212
#define ID_BMS_SOC          0x292
#define ID_LIFTGATE_STATUS  0x142
#define ID_DAS_STATUS       0x399

#define MIRROR_UNKNOWN      0
#define MIRROR_FOLDED       1
#define MIRROR_UNFOLDED     2
#define MIRROR_FOLDING      3
#define MIRROR_UNFOLDING    4

#define GEAR_UNKNOWN        7
#define GEAR_P              1
#define GEAR_R              2
#define GEAR_N              3
#define GEAR_D              4

#define BMS_CHARGE_DISCONNECTED 0
#define BMS_CHARGE_NO_POWER     1
#define BMS_ABOUT_TO_CHARGE     2
#define BMS_CHARGING            3
#define BMS_CHARGE_COMPLETE     4
#define BMS_CHARGE_STOPPED      5

#define LATCH_CLOSED        2
#define DISPLAY_BRIGHTNESS_MIN_RAW 0x0B
#define DISPLAY_BRIGHTNESS_MAX_RAW 0xC8

twai_handle_t vehicleTwai = NULL;
twai_handle_t chassisTwai = NULL;

unsigned long lastAnyRelevantCanSeen = 0;
unsigned long lastBlinkerFrameSeen = 0;
unsigned long lastChargeFrameSeen = 0;
unsigned long chargeAnimationStartMs = 0;
unsigned long lastBmsSocSeen = 0;
unsigned long last399Seen = 0;
unsigned long lastDashEffectMs = 0;
unsigned long welcomeUntilMs = 0;
unsigned long goodbyeUntilMs = 0;

bool carAwake = true;
bool lastCarAwake = true;
bool welcomePlayedForWake = false;
bool goodbyeActive = false;
uint16_t dashEffectStep = 0;

uint32_t dashCurrentPixels[NUM_LEDS];
uint32_t dashStartPixels[NUM_LEDS];
uint32_t dashTargetPixels[NUM_LEDS];
bool dashImmediatePixels[NUM_LEDS];
uint32_t dashLastTargetHash = 0;
unsigned long dashTransitionStartMs = 0;
unsigned long lastDashTransitionFrameMs = 0;
bool dashTransitionActive = false;
bool dashCurrentValid = false;
uint8_t lastDashBrightness = 255;
bool dashBrightnessDirty = false;

// Zustandsdefinitionen für das fehlerfreie Rendering
enum DashVisualState { VISUAL_OFF, VISUAL_CHARGE, VISUAL_WELCOME, VISUAL_GOODBYE, VISUAL_DOOR, VISUAL_AUTOPILOT, VISUAL_BLIND, VISUAL_BASE };
DashVisualState lastVisualState = VISUAL_OFF;
uint8_t lastRenderedBrightness = 0;

// Einstellungen (geladen aus NVS)
uint8_t baseR = 255; uint8_t baseG = 0; uint8_t baseB = 0;
uint8_t baseEffect = 1;
uint8_t manualBrightness = 120;
bool autoBrightness = true;
bool dashPowerOff = false;
bool chargeDashEnabled = true;
bool autopilotDashEnabled = true;
uint8_t autopilotR = 0; uint8_t autopilotG = 70; uint8_t autopilotB = 255;
bool blindSpotDashEnabled = true;
bool blindSpotOnlyWithBlinker = true;
uint8_t blindSpotDashPercent = 25;
uint16_t activeDashLedCount = DEFAULT_DASH_LED_COUNT;
bool doorOpenHighlightEnabled = true;
bool welcomeAnimationEnabled = true;
bool goodbyeAnimationEnabled = true;

// Fahrzeugstatus
bool blinkerLeftPulse = false;
bool blinkerRightPulse = false;
uint8_t displayBrightnessRaw = 0xC8;
float displayBrightnessPercent = 100.0;
uint8_t gear = GEAR_UNKNOWN;
bool carInPark = false;
uint8_t mirrorLeftState = MIRROR_UNKNOWN;
uint8_t mirrorRightState = MIRROR_UNKNOWN;
bool mirrorLeftFolded = false;
bool mirrorRightFolded = false;
bool mirrorsFolded = false;
bool vehicleAwake = true;
uint8_t batterySocPercent = 0;
uint8_t chargeStatus = BMS_CHARGE_DISCONNECTED;
bool chargingActive = false;

bool doorFrontLeftOpen = false;
bool doorFrontRightOpen = false;
bool doorRearLeftOpen = false;
bool doorRearRightOpen = false;
bool trunkOpen = false;

uint8_t autopilotState = 0;
bool autopilotActive = false;
uint8_t blindLeftRaw = 0; uint8_t blindRightRaw = 0;
bool blindLeftActive = false; bool blindRightActive = false;

// OPTIMIERUNG: Schnelles Laden der CAN-Signale im Speicher
uint32_t extractIntelSignal(const uint8_t *data, uint8_t startBit, uint8_t bitLength) {
  uint64_t raw;
  memcpy(&raw, data, 8);
  uint64_t mask = (1ULL << bitLength) - 1ULL;
  return (raw >> startBit) & mask;
}

float displayBrightnessRawToPercent(uint8_t raw) {
  if (raw <= DISPLAY_BRIGHTNESS_MIN_RAW) return 0.0f;
  if (raw >= DISPLAY_BRIGHTNESS_MAX_RAW) return 100.0f;
  return (raw - DISPLAY_BRIGHTNESS_MIN_RAW) * 100.0f / (DISPLAY_BRIGHTNESS_MAX_RAW - DISPLAY_BRIGHTNESS_MIN_RAW);
}

bool latchMeansDoorOpen(uint8_t latchStatus) { return latchStatus != LATCH_CLOSED; }
bool latchMeansTrunkOpen(uint8_t latchStatus) { return latchStatus == 1 || latchStatus == 3 || latchStatus == 4 || latchStatus == 5; }

uint32_t dashColor(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
uint8_t dashRed(uint32_t c) { return (c >> 16) & 0xFF; }
uint8_t dashGreen(uint32_t c) { return (c >> 8) & 0xFF; }
uint8_t dashBlue(uint32_t c) { return c & 0xFF; }

uint16_t dashLedCount() { return (activeDashLedCount > 0 && activeDashLedCount <= NUM_LEDS) ? activeDashLedCount : NUM_LEDS; }

// OPTIMIERUNG: Schnelle Farbmischung per Bit-Shifting ohne Divisionen
uint32_t dashBlendColor(uint32_t from, uint32_t to, uint8_t amount) {
  int16_t rDiff = (int16_t)dashRed(to) - dashRed(from);
  int16_t gDiff = (int16_t)dashGreen(to) - dashGreen(from);
  int16_t bDiff = (int16_t)dashBlue(to) - dashBlue(from);

  uint8_t r = dashRed(from) + ((rDiff * amount + 128) >> 8);
  uint8_t g = dashGreen(from) + ((gDiff * amount + 128) >> 8);
  uint8_t b = dashBlue(from) + ((bDiff * amount + 128) >> 8);
  return dashColor(r, g, b);
}

uint8_t effectiveBrightness() {
  if (autoBrightness) {
    float p = displayBrightnessPercent;
    if (p < 0.0f) p = 0.0f;
    if (p > 100.0f) p = 100.0f;
    uint8_t b = (uint8_t)(p * MAX_LED_BRIGHTNESS / 100.0f);
    if (b < MIN_AUTO_LED_BRIGHTNESS) b = MIN_AUTO_LED_BRIGHTNESS;
    return b;
  }
  return map(manualBrightness, 0, 255, 0, MAX_LED_BRIGHTNESS);
}

void flushDashBrightnessIfNeeded() {
  uint8_t b = effectiveBrightness();
  if (b != lastDashBrightness || dashBrightnessDirty) {
    strip.setBrightness(b);
    strip.show();
    lastDashBrightness = b;
    dashBrightnessDirty = false;
  }
}

uint32_t dashTargetHash() {
  uint32_t hash = 2166136261UL;
  uint16_t count = dashLedCount();
  for (uint16_t i = 0; i < count; i++) {
    uint32_t c = dashTargetPixels[i];
    hash ^= (c & 0xFF); hash *= 16777619UL;
    hash ^= ((c >> 8) & 0xFF); hash *= 16777619UL;
    hash ^= ((c >> 16) & 0xFF); hash *= 16777619UL;
    hash ^= dashImmediatePixels[i] ? 1 : 0; hash *= 16777619UL;
  }
  return hash;
}

void dashCaptureCurrent() {
  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) { dashCurrentPixels[i] = strip.getPixelColor(i); }
  dashCurrentValid = true;
}

void dashApplyTarget() {
  uint32_t h = dashTargetHash();
  if (h == dashLastTargetHash && !dashBrightnessDirty) return;
  dashLastTargetHash = h;

  uint16_t count = dashLedCount();
  if (!dashCurrentValid) dashCaptureCurrent();

  bool anyTransition = false;
  for (int i = 0; i < count; i++) {
    if (dashImmediatePixels[i]) {
      strip.setPixelColor(i, dashTargetPixels[i]);
    } else if (dashCurrentPixels[i] != dashTargetPixels[i]) {
      anyTransition = true;
    }
  }

  if (anyTransition) {
    for (int i = 0; i < count; i++) { dashStartPixels[i] = dashCurrentPixels[i]; }
    dashTransitionStartMs = millis();
    lastDashTransitionFrameMs = millis();
    dashTransitionActive = true;
  } else {
    for (int i = 0; i < count; i++) { strip.setPixelColor(i, dashTargetPixels[i]); }
    strip.setBrightness(effectiveBrightness());
    strip.show();
    dashBrightnessDirty = false;
    dashCaptureCurrent();
    dashTransitionActive = false;
  }
}

void dashRenderTransition() {
  if (!dashTransitionActive) return;
  unsigned long now = millis();
  if (now - lastDashTransitionFrameMs < DASH_TRANSITION_FRAME_MS) return;
  lastDashTransitionFrameMs = now;

  unsigned long elapsed = now - dashTransitionStartMs;
  if (elapsed >= DASH_TRANSITION_MS) {
    uint16_t count = dashLedCount();
    for (int i = 0; i < count; i++) { strip.setPixelColor(i, dashTargetPixels[i]); }
    strip.setBrightness(effectiveBrightness());
    strip.show();
    dashBrightnessDirty = false;
    dashCaptureCurrent();
    dashTransitionActive = false;
  } else {
    uint8_t amt = (elapsed * 255) / DASH_TRANSITION_MS;
    uint16_t count = dashLedCount();
    for (int i = 0; i < count; i++) {
      if (!dashImmediatePixels[i]) {
        uint32_t c = dashBlendColor(dashStartPixels[i], dashTargetPixels[i], amt);
        strip.setPixelColor(i, c);
      }
    }
    strip.setBrightness(effectiveBrightness());
    strip.show();
    dashBrightnessDirty = false;
  }
}

void resetDashEffect() { dashEffectStep = 0; lastDashEffectMs = 0; }

bool dashEffectTick(uint32_t intervalMs) {
  unsigned long now = millis();
  if (lastDashEffectMs == 0 || now - lastDashEffectMs >= intervalMs) {
    lastDashEffectMs = now;
    dashEffectStep++;
    return true;
  }
  return false;
}

void dashOff() {
  dashTransitionActive = false;
  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) { dashTargetPixels[i] = 0; dashImmediatePixels[i] = true; }
  dashApplyTarget();
}

void dashSetAll(uint32_t color) {
  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) { dashTargetPixels[i] = color; dashImmediatePixels[i] = false; }
  dashApplyTarget();
}

void dashBase() {
  if (baseEffect == 0) dashOff();
  else dashSetAll(dashColor(baseR, baseG, baseB));
}

void dashSegment(bool leftActive, bool rightActive, uint8_t percent, uint32_t segColor, uint32_t bgColor, bool immediate) {
  uint16_t count = dashLedCount();
  uint16_t segLen = (count * percent) / 100;
  if (segLen < 1) segLen = 1;
  if (segLen > count / 2) segLen = count / 2;

  for (int i = 0; i < count; i++) {
    dashTargetPixels[i] = bgColor;
    dashImmediatePixels[i] = immediate;
  }
  if (leftActive) {
    for (int i = 0; i < segLen; i++) { dashTargetPixels[i] = segColor; }
  }
  if (rightActive) {
    for (int i = 0; i < segLen; i++) { dashTargetPixels[count - 1 - i] = segColor; }
  }
  dashApplyTarget();
}

void dashWelcome() {
  dashTransitionActive = false;
  if (!dashEffectTick(18)) { flushDashBrightnessIfNeeded(); return; }
  uint16_t count = dashLedCount();
  int centerLeft = (count / 2) - 1; int centerRight = count / 2;
  int maxRadius = count / 2 + 2;

  strip.clear();
  int r = dashEffectStep;
  if (r > maxRadius) r = maxRadius;

  for (int i = 0; i <= r; i++) {
    int left = centerLeft - i; int right = centerRight + i;
    if (left >= 0) strip.setPixelColor(left, dashColor(baseR, baseG, baseB));
    if (right < count) strip.setPixelColor(right, dashColor(baseR, baseG, baseB));
  }
  strip.setBrightness(effectiveBrightness());
  strip.show();
  dashBrightnessDirty = false;
  dashCaptureCurrent();
}

void dashGoodbye() {
  dashTransitionActive = false;
  if (!dashEffectTick(28)) { flushDashBrightnessIfNeeded(); return; }
  uint16_t count = dashLedCount();
  int centerLeft = (count / 2) - 1; int centerRight = count / 2;
  int maxRadius = count / 2 + 1;

  int progress = dashEffectStep;
  strip.clear();
  if (progress < maxRadius) {
    for (int i = progress; i < maxRadius; i++) {
      int left = centerLeft - i; int right = centerRight + i;
      if (left >= 0) strip.setPixelColor(left, dashColor(baseR, baseG, baseB));
      if (right < count) strip.setPixelColor(right, dashColor(baseR, baseG, baseB));
    }
  }
  strip.setBrightness(effectiveBrightness());
  strip.show();
  dashBrightnessDirty = false;
  dashCaptureCurrent();
}

void dashChargeSweep() {
  dashTransitionActive = false;
  if (!dashEffectTick(25)) { flushDashBrightnessIfNeeded(); return; }
  uint16_t count = dashLedCount();
  uint16_t fillTo = (count * batterySocPercent) / 100;
  uint32_t activeColor = (batterySocPercent >= CHARGE_GREEN_AT_PERCENT) ? dashColor(0, 255, 0) : dashColor(0, 100, 255);

  uint16_t sweepIdx = dashEffectStep % (count + 15);
  strip.clear();

  for (int i = 0; i < count; i++) {
    if (i < fillTo) {
      if (i <= sweepIdx && sweepIdx < i + 8) strip.setPixelColor(i, dashColor(255, 255, 255));
      else strip.setPixelColor(i, activeColor);
    } else {
      strip.setPixelColor(i, 0);
    }
  }
  strip.setBrightness(effectiveBrightness());
  strip.show();
  dashBrightnessDirty = false;
  dashCaptureCurrent();
}

// =====================================================
// GEFIXTE UND OPTIMIERTE ZUSTANDSMASCHINE
// =====================================================
void refreshDash() {
  if (dashTransitionActive) {
    dashRenderTransition();
    return;
  }

  uint8_t currentBright = effectiveBrightness();
  bool chargeNow = chargeDashEnabled && chargingActive && lastChargeFrameSeen > 0 &&
                   millis() - lastChargeFrameSeen < 5000 &&
                   chargeAnimationStartMs > 0 &&
                   millis() - chargeAnimationStartMs < CHARGE_ANIMATION_TIMEOUT_MS;

  // Logischen Zustand ermitteln
  DashVisualState currentVisualState = VISUAL_BASE;
  if (dashPowerOff) currentVisualState = VISUAL_OFF;
  else if (chargeNow) currentVisualState = VISUAL_CHARGE;
  else if (welcomeUntilMs > millis()) currentVisualState = VISUAL_WELCOME;
  else if (goodbyeActive) currentVisualState = VISUAL_GOODBYE;
  else if (!carAwake) currentVisualState = VISUAL_OFF;
  else if (doorOpenHighlightEnabled && (doorFrontLeftOpen || doorFrontRightOpen || doorRearLeftOpen || doorRearRightOpen)) currentVisualState = VISUAL_DOOR;
  else if (autopilotDashEnabled && autopilotActive) currentVisualState = VISUAL_AUTOPILOT;
  else if (blindSpotDashEnabled && (blindLeftActive || blindRightActive) && (!blindSpotOnlyWithBlinker || blinkerLeftPulse || blinkerRightPulse)) currentVisualState = VISUAL_BLIND;

  // FEHLERBEHEBUNG: Bei Zustandswechsel Cache invalidieren, um Einfrieren zu verhindern!
  if (currentVisualState != lastVisualState || currentBright != lastRenderedBrightness) {
    dashLastTargetHash = 0; 
    dashCurrentValid = false;
    if (lastVisualState == VISUAL_CHARGE || lastVisualState == VISUAL_WELCOME || lastVisualState == VISUAL_GOODBYE) {
      strip.clear(); // Alten Frame im RAM komplett verwerfen
    }
    lastVisualState = currentVisualState;
    lastRenderedBrightness = currentBright;
    resetDashEffect();
  }

  // Zustand ausführen
  switch (currentVisualState) {
    case VISUAL_OFF:
      dashOff();
      break;
    case VISUAL_CHARGE:
      dashChargeSweep();
      break;
    case VISUAL_WELCOME:
      dashWelcome();
      break;
    case VISUAL_GOODBYE:
      if (goodbyeUntilMs > millis()) {
        dashGoodbye();
      } else {
        goodbyeActive = false;
        dashOff();
      }
      break;
    case VISUAL_DOOR: {
      uint32_t bg = dashColor(baseR, baseG, baseB);
      dashSegment(doorFrontLeftOpen || doorRearLeftOpen, doorFrontRightOpen || doorRearRightOpen, 25, dashColor(255, 120, 0), bg, true);
      break;
    }
    case VISUAL_AUTOPILOT:
      dashSetAll(dashColor(autopilotR, autopilotG, autopilotB));
      break;
    case VISUAL_BLIND: {
      uint32_t bg = dashColor(baseR, baseG, baseB);
      bool leftBlind = blindLeftActive && (!blindSpotOnlyWithBlinker || blinkerLeftPulse);
      bool rightBlind = blindRightActive && (!blindSpotOnlyWithBlinker || blinkerRightPulse);
      dashSegment(leftBlind, rightBlind, blindSpotDashPercent, dashColor(255, 120, 0), bg, true);
      break;
    }
    case VISUAL_BASE:
      dashBase();
      break;
  }
}

// OPTIMIERUNG: String-Speicherreservierung gegen RAM-Abstürze
String statusPage() {
  String page; page.reserve(2560);
  page = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Tesla Dash Standalone</title><style>body{font-family:sans-serif;margin:20px;background:#f0f2f5;color:#333;}";
  page += ".card{background:#fff;padding:15px;margin-bottom:15px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>";
  page += "<h1>Tesla Dash C6 Standalone</h1><div class='card'><h2>Status</h2>";
  page += "<p>Wach: " + String(carAwake ? "JA" : "NEIN") + "</p>";
  page += "<p>Helligkeit: " + String(effectiveBrightness()) + " (" + String(displayBrightnessPercent, 1) + "%)</p>";
  page += "<p>Batterie: " + String(batterySocPercent) + "% (Laden: " + String(chargingActive ? "JA" : "NEIN") + ")</p></div></body></html>";
  return page;
}

void loadSettings() {
  prefs.begin("dash", true);
  baseR = prefs.getUChar("baseR", 255); baseG = prefs.getUChar("baseG", 0); baseB = prefs.getUChar("baseB", 0);
  baseEffect = prefs.getUChar("baseEffect", 1); manualBrightness = prefs.getUChar("manBright", 120);
  autoBrightness = prefs.getBool("autoBright", true); dashPowerOff = prefs.getBool("powerOff", false);
  chargeDashEnabled = prefs.getBool("chgDash", true); autopilotDashEnabled = prefs.getBool("apDash", true);
  autopilotR = prefs.getUChar("apR", 0); autopilotG = prefs.getUChar("apG", 70); autopilotB = prefs.getUChar("apB", 255);
  blindSpotDashEnabled = prefs.getBool("bsDash", true); blindSpotOnlyWithBlinker = prefs.getBool("bsBlink", true);
  blindSpotDashPercent = prefs.getUChar("bsPct", 25); activeDashLedCount = prefs.getUInt("ledCount", DEFAULT_DASH_LED_COUNT);
  doorOpenHighlightEnabled = prefs.getBool("doorEn", true); welcomeAnimationEnabled = prefs.getBool("welEn", true);
  goodbyeAnimationEnabled = prefs.getBool("gbEn", true);
  prefs.end();
}

void setupTwai(twai_handle_t *handle, gpio_num_t tx, gpio_num_t rx, uint8_t controllerId) {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, TWAI_MODE_LISTEN_ONLY);
  g_config.controller_id = controllerId;
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  
  if (twai_driver_install_v2(&g_config, &t_config, &f_config, handle) != ESP_OK) {
    delay(1000); ESP.restart();
  }
  twai_start_v2(*handle);
}

void processCan() {
  twai_message_t msg;
  while (twai_receive_v2(vehicleTwai, &msg, 0) == ESP_OK) {
    lastAnyRelevantCanSeen = millis();
    switch (msg.identifier) {
      case ID_BLINKER:
        blinkerLeftPulse = (extractIntelSignal(msg.data, 0, 2) == 1);
        blinkerRightPulse = (extractIntelSignal(msg.data, 2, 2) == 1);
        lastBlinkerFrameSeen = millis();
        break;
      case ID_BRIGHTNESS:
        displayBrightnessRaw = msg.data[0];
        displayBrightnessPercent = displayBrightnessRawToPercent(displayBrightnessRaw);
        dashBrightnessDirty = true;
        break;
      case ID_GEAR:
        gear = extractIntelSignal(msg.data, 21, 3);
        carInPark = (gear == GEAR_P);
        break;
      case ID_VCLEFT_STATUS:
        doorFrontLeftOpen = latchMeansDoorOpen(extractIntelSignal(msg.data, 12, 3));
        doorRearLeftOpen = latchMeansDoorOpen(extractIntelSignal(msg.data, 15, 3));
        mirrorLeftState = extractIntelSignal(msg.data, 35, 3);
        mirrorLeftFolded = (mirrorLeftState == MIRROR_FOLDED || mirrorLeftState == MIRROR_FOLDING);
        break;
      case ID_VCRIGHT_STATUS:
        doorFrontRightOpen = latchMeansDoorOpen(extractIntelSignal(msg.data, 12, 3));
        doorRearRightOpen = latchMeansDoorOpen(extractIntelSignal(msg.data, 15, 3));
        break;
      case ID_BMS_STATUS: {
        chargeStatus = extractIntelSignal(msg.data, 48, 3);
        bool oldChargingActive = chargingActive;
        chargingActive = (chargeStatus == BMS_CHARGING);
        // FEHLERBEHEBUNG: Saubere Flankenerkennung beim Starten des Ladevorgangs
        if (chargingActive && !oldChargingActive) {
          chargeAnimationStartMs = millis();
        }
        lastChargeFrameSeen = millis();
        break;
      }
      case ID_BMS_SOC:
        batterySocPercent = (uint8_t)(extractIntelSignal(msg.data, 0, 10) * 0.1f);
        lastBmsSocSeen = millis();
        break;
      case ID_LIFTGATE_STATUS:
        trunkOpen = latchMeansTrunkOpen(extractIntelSignal(msg.data, 0, 4));
        break;
    }
  }

  while (twai_receive_v2(chassisTwai, &msg, 0) == ESP_OK) {
    if (msg.identifier == ID_DAS_STATUS) {
      autopilotState = extractIntelSignal(msg.data, 0, 4);
      autopilotActive = (autopilotState == 2 || autopilotState == 3 || autopilotState == 5);
      blindLeftRaw = extractIntelSignal(msg.data, 52, 2);
      blindRightRaw = extractIntelSignal(msg.data, 54, 2);
      blindLeftActive = (blindLeftRaw == 1 || blindLeftRaw == 2);
      blindRightActive = (blindRightRaw == 1 || blindRightRaw == 2);
      last399Seen = millis();
    }
  }
}

void runFailsafes() {
  if (millis() - lastBlinkerFrameSeen > 1000) { blinkerLeftPulse = false; blinkerRightPulse = false; }
  if (millis() - last399Seen > CHASSIS_CAN_TIMEOUT_MS) { autopilotActive = false; blindLeftActive = false; blindRightActive = false; }
  if (millis() - lastChargeFrameSeen > 10000) { chargingActive = false; }
}

void setup() {
  Serial.begin(115200);
  loadSettings();
  
  strip.begin();
  strip.setBrightness(effectiveBrightness());
  strip.show();
  dashCaptureCurrent();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", statusPage()); });
  server.begin();

  setupTwai(&vehicleTwai, VEHICLE_CAN_TX_PIN, VEHICLE_CAN_RX_PIN, 0);
  setupTwai(&chassisTwai, CHASSIS_CAN_TX_PIN, CHASSIS_CAN_RX_PIN, 1);
}

void loop() {
  server.handleClient();
  processCan();
  runFailsafes();

  vehicleAwake = (millis() - lastAnyRelevantCanSeen < 5000);
  mirrorsFolded = mirrorLeftFolded; 
  carAwake = vehicleAwake && !mirrorsFolded;

  bool chargeNow = chargeDashEnabled && chargingActive && lastChargeFrameSeen > 0 &&
                   millis() - lastChargeFrameSeen < 5000 &&
                   chargeAnimationStartMs > 0 &&
                   millis() - chargeAnimationStartMs < CHARGE_ANIMATION_TIMEOUT_MS;

  if (carAwake != lastCarAwake) {
    lastCarAwake = carAwake;
    if (carAwake) {
      goodbyeActive = false; goodbyeUntilMs = 0;
      if (welcomeAnimationEnabled && !welcomePlayedForWake) {
        welcomePlayedForWake = true; welcomeUntilMs = millis() + 2200;
      }
      resetDashEffect();
    } else {
      welcomePlayedForWake = false; welcomeUntilMs = 0;
      if (chargeNow) {
        goodbyeActive = false; goodbyeUntilMs = 0;
      } else if (goodbyeAnimationEnabled) {
        goodbyeActive = true; goodbyeUntilMs = millis() + 2200;
        resetDashEffect();
      } else {
        goodbyeActive = false; goodbyeUntilMs = 0;
      }
    }
  }

  refreshDash();
  delay(1);
}