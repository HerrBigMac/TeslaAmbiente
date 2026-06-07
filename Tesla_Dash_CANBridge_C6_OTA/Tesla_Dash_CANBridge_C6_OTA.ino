#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Adafruit_NeoPixel.h>
#include "driver/twai.h"

// =====================================================
// Tesla Dash CAN-Bridge fuer ESP32-C6 Super Mini
// VehicleBus + ChassisBus auf einem ESP32-C6
// Steuert Dash LEDs direkt und sendet CAN-Daten per ESP-NOW
// an den Ambiente Master fuer die Tueren.
// =====================================================
//
// WICHTIG:
// - ESP32-C6 hat zwei TWAI Controller, braucht aber trotzdem
//   pro CAN-Bus einen externen CAN-Transceiver.
// - VehicleBus und ChassisBus duerfen NICHT elektrisch verbunden werden.
// - Der Sketch nutzt den TWAI-V2-Treiber mit zwei getrennten Handles.
// - Wenn dein Arduino-ESP32-Core twai_driver_install_v2 nicht kennt,
//   ist der Core fuer duales TWAI auf dem C6 zu alt.
//
// OTA:
// - Im Normalbetrieb kein Access Point.
// - Der Master sendet per Button "CAN Reader Update-Modus"
//   command 1. Dann startet dieser ESP fuer 10 Minuten den AP.
// - Browser: http://192.168.4.1/update
// =====================================================

// =======================
// Pins ESP32-C6 Super Mini
// =======================
// Passe diese Pins an dein Board/Wiring an.
// Je Bus ein Transceiver, z.B. SN65HVD230/TJA1051/TCAN332.
#define VEHICLE_CAN_TX_PIN GPIO_NUM_1
#define VEHICLE_CAN_RX_PIN GPIO_NUM_2

#define CHASSIS_CAN_TX_PIN  GPIO_NUM_15
#define CHASSIS_CAN_RX_PIN  GPIO_NUM_14

#define LED_PIN 4
#define NUM_LEDS 122
#define DEFAULT_DASH_LED_COUNT 122
#define LED_TYPE NEO_GRB + NEO_KHZ800

// 100 Prozent im Code entsprechen real 15 Prozent LED-Leistung.
#define MAX_LED_BRIGHTNESS_PERCENT 15
#define MAX_LED_BRIGHTNESS ((255 * MAX_LED_BRIGHTNESS_PERCENT) / 100)
#define MIN_AUTO_LED_BRIGHTNESS 2
#define CHARGE_GREEN_AT_PERCENT 75
#define DASH_SETTINGS_PACKET_TYPE 0xC7
#define CHASSIS_CAN_TIMEOUT_MS 5000
#define DASH_TRANSITION_MS 250
#define DASH_TRANSITION_FRAME_MS 18
#define CHARGE_ANIMATION_TIMEOUT_MS 1800000UL

// =======================
// ESP-NOW + OTA AP
// =======================
#define ESPNOW_CHANNEL 6
#define OTA_AP_SSID "Tesla-Dash"
#define OTA_AP_PASS "12345678"
#define OTA_DEFAULT_MINUTES 10

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
WebServer server(80);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, LED_TYPE);

// =======================
// CAN IDs VehicleBus
// =======================
#define ID_BLINKER          0x3F5
#define ID_BRIGHTNESS       0x273
#define ID_GEAR             0x118
#define ID_VCLEFT_STATUS    0x102
#define ID_VCRIGHT_STATUS   0x103
#define ID_BMS_STATUS       0x212
#define ID_BMS_SOC          0x292
#define ID_LIFTGATE_STATUS  0x142
#define ID_CP_CHARGE_STATUS 0x13D

// =======================
// CAN IDs ChassisBus
// =======================
#define ID_DAS_STATUS       0x399

// =======================
// Konstanten
// =======================
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

#define CP_CHARGE_ENABLED       5
#define CP_CHARGE_TYPE_DC       1
#define CP_CHARGE_TYPE_AC       2

#define LATCH_CLOSED        2

#define DISPLAY_BRIGHTNESS_MIN_RAW 0x0B
#define DISPLAY_BRIGHTNESS_MAX_RAW 0xC8

// =======================
// Pakete an Master
// Muss im Master exakt gleich sein.
// =======================
struct VehiclePacket {
  uint8_t packetType;

  bool blinkerLeftPulse;
  bool blinkerRightPulse;

  uint8_t displayBrightnessRaw;
  float displayBrightnessPercent;

  uint8_t gear;
  bool carInPark;

  uint8_t mirrorLeftState;
  uint8_t mirrorRightState;
  bool mirrorLeftFolded;
  bool mirrorRightFolded;
  bool mirrorsFolded;
  bool mirrorsUnfolded;

  bool vehicleAwake;

  uint32_t counter;
  uint32_t lastVehicleCanAgeMs;

  bool chargingActive;
  uint8_t chargeStatus;
  uint8_t batterySocPercent;
  uint32_t lastChargeCanAgeMs;

  bool doorFrontLeftOpen;
  bool doorFrontRightOpen;
  bool doorRearLeftOpen;
  bool doorRearRightOpen;
  bool trunkOpen;
};

struct ChassisPacket {
  uint8_t packetType;
  uint8_t autopilotState;
  bool autopilotActive;
  uint8_t blindLeftRaw;
  uint8_t blindRightRaw;
  bool blindLeftActive;
  bool blindRightActive;
  uint32_t counter;
};

typedef struct __attribute__((packed)) {
  uint8_t magic;       // 0xC6
  uint8_t version;     // 1
  uint8_t command;     // 1 = OTA AP starten
  uint8_t minutes;     // Laufzeit
  uint32_t nonce;
} SystemCommand;

typedef struct __attribute__((packed)) {
  uint8_t packetType;  // 0xC7
  uint8_t version;     // 1
  uint8_t baseR;
  uint8_t baseG;
  uint8_t baseB;
  uint8_t baseEffect;
  uint8_t baseSpeed;
  uint8_t baseIntensity;
  uint8_t manualBrightness;
  bool autoBrightness;
  bool powerOff;
  bool chargeDashEnabled;
  bool autopilotDashEnabled;
  uint8_t autopilotR;
  uint8_t autopilotG;
  uint8_t autopilotB;
  bool blindSpotDashEnabled;
  bool blindSpotOnlyWithBlinker;
  uint8_t blindSpotDashPercent;
  uint16_t dashLedCount;
  bool doorOpenHighlightEnabled;
  bool welcomeAnimationEnabled;
  bool goodbyeAnimationEnabled;
} DashSettingsPacket;

twai_handle_t vehicleTwai = NULL;
twai_handle_t chassisTwai = NULL;

VehiclePacket vehicleData = {};
ChassisPacket chassisData = {};

uint32_t vehicleSendCounter = 0;
uint32_t chassisSendCounter = 0;

unsigned long lastVehicleSend = 0;
unsigned long lastChassisSend = 0;
unsigned long lastAnyRelevantCanSeen = 0;
unsigned long lastBlinkerFrameSeen = 0;
unsigned long lastChargeFrameSeen = 0;
unsigned long chargeAnimationStartMs = 0;
unsigned long lastBmsSocSeen = 0;
unsigned long last399Seen = 0;
unsigned long lastDebug = 0;
unsigned long otaApUntilMs = 0;
unsigned long lastDashEffectMs = 0;
unsigned long welcomeUntilMs = 0;
unsigned long goodbyeUntilMs = 0;
bool otaApActive = false;
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
bool lastBlindLeftShown = false;
bool lastBlindRightShown = false;

uint8_t baseR = 255;
uint8_t baseG = 0;
uint8_t baseB = 0;
uint8_t baseEffect = 1;
uint8_t baseSpeed = 120;
uint8_t baseIntensity = 120;
uint8_t manualBrightness = 120;
bool autoBrightness = true;
bool dashPowerOff = false;

bool chargeDashEnabled = true;
bool autopilotDashEnabled = true;
uint8_t autopilotR = 0;
uint8_t autopilotG = 70;
uint8_t autopilotB = 255;
bool blindSpotDashEnabled = true;
bool blindSpotOnlyWithBlinker = true;
uint8_t blindSpotDashPercent = 25;
uint16_t activeDashLedCount = DEFAULT_DASH_LED_COUNT;
bool doorOpenHighlightEnabled = true;
bool welcomeAnimationEnabled = true;
bool goodbyeAnimationEnabled = true;

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
bool mirrorsUnfolded = false;
bool vehicleAwake = true;

uint8_t batterySocPercent = 0;
uint8_t chargeStatus = BMS_CHARGE_DISCONNECTED;
uint8_t cpHvChargeStatus = 0;
uint8_t cpEvseChargeType = 0;
bool cpChargingActive = false;
bool chargingActive = false;

bool doorFrontLeftOpen = false;
bool doorFrontRightOpen = false;
bool doorRearLeftOpen = false;
bool doorRearRightOpen = false;
bool trunkOpen = false;

uint8_t autopilotState = 0;
bool autopilotActive = false;
uint8_t blindLeftRaw = 0;
uint8_t blindRightRaw = 0;
bool blindLeftActive = false;
bool blindRightActive = false;

// =====================================================
// Hilfen
// =====================================================

uint32_t extractIntelSignal(const uint8_t *data, uint8_t startBit, uint8_t bitLength) {
  uint64_t raw = 0;

  for (int i = 0; i < 8; i++) {
    raw |= ((uint64_t)data[i]) << (8 * i);
  }

  uint64_t mask = (1ULL << bitLength) - 1ULL;
  return (raw >> startBit) & mask;
}

float displayBrightnessRawToPercent(uint8_t raw) {
  if (raw <= DISPLAY_BRIGHTNESS_MIN_RAW) return 0.0f;
  if (raw >= DISPLAY_BRIGHTNESS_MAX_RAW) return 100.0f;

  return (raw - DISPLAY_BRIGHTNESS_MIN_RAW) * 100.0f /
         (DISPLAY_BRIGHTNESS_MAX_RAW - DISPLAY_BRIGHTNESS_MIN_RAW);
}

bool latchMeansDoorOpen(uint8_t latchStatus) {
  return latchStatus != LATCH_CLOSED;
}

bool latchMeansTrunkOpen(uint8_t latchStatus) {
  return latchStatus == 1 || latchStatus == 3 || latchStatus == 4 || latchStatus == 5;
}

bool anyDoorOpen() {
  return doorFrontLeftOpen ||
         doorFrontRightOpen ||
         doorRearLeftOpen ||
         doorRearRightOpen ||
         trunkOpen;
}

bool anyMirrorUnfoldedOrUnfolding() {
  return mirrorLeftState == MIRROR_UNFOLDED ||
         mirrorRightState == MIRROR_UNFOLDED ||
         mirrorLeftState == MIRROR_UNFOLDING ||
         mirrorRightState == MIRROR_UNFOLDING;
}

void updateMirrorLogic() {
  mirrorLeftFolded = (mirrorLeftState == MIRROR_FOLDED);
  mirrorRightFolded = (mirrorRightState == MIRROR_FOLDED);

  mirrorsFolded =
    mirrorLeftState == MIRROR_FOLDED &&
    mirrorRightState == MIRROR_FOLDED &&
    !anyDoorOpen();

  mirrorsUnfolded =
    mirrorLeftState == MIRROR_UNFOLDED &&
    mirrorRightState == MIRROR_UNFOLDED;

  if (anyDoorOpen() || anyMirrorUnfoldedOrUnfolding()) {
    vehicleAwake = true;
  } else if (mirrorsFolded) {
    vehicleAwake = false;
  }
}

void updateChargingActive() {
  bool wasCharging = chargingActive;
  chargingActive = (chargeStatus == BMS_CHARGING) || cpChargingActive;

  if (chargingActive && !wasCharging) {
    chargeAnimationStartMs = millis();
    resetDashEffect();
  }

  if (!chargingActive) {
    chargeAnimationStartMs = 0;
  }
}

const char* chargeStatusToText(uint8_t s) {
  switch (s) {
    case BMS_CHARGE_DISCONNECTED: return "DISCONNECTED";
    case BMS_CHARGE_NO_POWER: return "NO_POWER";
    case BMS_ABOUT_TO_CHARGE: return "ABOUT_TO_CHARGE";
    case BMS_CHARGING: return "CHARGING";
    case BMS_CHARGE_COMPLETE: return "COMPLETE";
    case BMS_CHARGE_STOPPED: return "STOPPED";
    default: return "OTHER";
  }
}

// =====================================================
// Dash LED Logik
// =====================================================

uint8_t scaledByte(uint8_t value, uint8_t scale) {
  return ((uint16_t)value * scale) / 255;
}

uint8_t effectiveBrightness() {
  if (!autoBrightness) {
    return map(manualBrightness, 0, 255, 0, MAX_LED_BRIGHTNESS);
  }

  uint8_t canBrightness = map((int)displayBrightnessPercent, 0, 100, 0, 255);
  uint8_t brightness = map(canBrightness, 0, 255, 0, MAX_LED_BRIGHTNESS);
  if (displayBrightnessPercent > 0.5f && brightness < MIN_AUTO_LED_BRIGHTNESS) {
    return MIN_AUTO_LED_BRIGHTNESS;
  }
  return brightness;
}

void updateDashBrightness() {
  uint8_t brightness = effectiveBrightness();
  if (brightness != lastDashBrightness) {
    lastDashBrightness = brightness;
    dashBrightnessDirty = true;
  }
  strip.setBrightness(brightness);
}

void flushDashBrightnessIfNeeded() {
  if (!dashBrightnessDirty) return;
  strip.show();
  dashBrightnessDirty = false;
}

uint32_t dashColor(uint8_t r, uint8_t g, uint8_t b) {
  return strip.Color(r, g, b);
}

uint16_t dashLedCount() {
  if (activeDashLedCount < 1) return DEFAULT_DASH_LED_COUNT;
  if (activeDashLedCount > NUM_LEDS) return NUM_LEDS;
  return activeDashLedCount;
}

uint8_t dashRed(uint32_t c) {
  return (uint8_t)(c >> 16);
}

uint8_t dashGreen(uint32_t c) {
  return (uint8_t)(c >> 8);
}

uint8_t dashBlue(uint32_t c) {
  return (uint8_t)c;
}

uint32_t dashBlendColor(uint32_t from, uint32_t to, uint8_t amount) {
  uint8_t r = dashRed(from) + (((int16_t)dashRed(to) - dashRed(from)) * amount) / 255;
  uint8_t g = dashGreen(from) + (((int16_t)dashGreen(to) - dashGreen(from)) * amount) / 255;
  uint8_t b = dashBlue(from) + (((int16_t)dashBlue(to) - dashBlue(from)) * amount) / 255;
  return dashColor(r, g, b);
}

uint32_t dashTargetHash() {
  uint32_t hash = 2166136261UL;
  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) {
    hash ^= dashTargetPixels[i];
    hash *= 16777619UL;
    hash ^= dashImmediatePixels[i];
    hash *= 16777619UL;
  }
  return hash;
}

void dashCaptureCurrent() {
  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) {
    dashCurrentPixels[i] = strip.getPixelColor(i);
  }
  for (int i = count; i < NUM_LEDS; i++) {
    dashCurrentPixels[i] = 0;
  }
  dashCurrentValid = true;
  dashLastTargetHash = 0;
}

void dashShowImmediateTarget() {
  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) {
    dashCurrentPixels[i] = dashTargetPixels[i];
    strip.setPixelColor(i, dashCurrentPixels[i]);
  }
  for (int i = count; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, 0);
  }
  strip.show();
  dashBrightnessDirty = false;
  dashCurrentValid = true;
  dashTransitionActive = false;
}

bool dashRenderTransition() {
  if (!dashTransitionActive) return false;

  uint32_t now = millis();
  if (now - lastDashTransitionFrameMs < DASH_TRANSITION_FRAME_MS) return true;
  lastDashTransitionFrameMs = now;

  uint32_t elapsed = now - dashTransitionStartMs;
  uint8_t amount = elapsed >= DASH_TRANSITION_MS ? 255 : (elapsed * 255UL) / DASH_TRANSITION_MS;

  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) {
    if (dashImmediatePixels[i]) {
      dashCurrentPixels[i] = dashTargetPixels[i];
    } else {
      dashCurrentPixels[i] = dashBlendColor(dashStartPixels[i], dashTargetPixels[i], amount);
    }
    strip.setPixelColor(i, dashCurrentPixels[i]);
  }
  for (int i = count; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, 0);
  }

  strip.show();
  dashBrightnessDirty = false;

  if (amount >= 255) {
    dashTransitionActive = false;
  }

  return true;
}

void dashApplyTarget(bool smooth = true) {
  uint32_t hash = dashTargetHash();

  if (dashCurrentValid && hash == dashLastTargetHash) {
    if (!dashRenderTransition()) {
      flushDashBrightnessIfNeeded();
    }
    return;
  }

  dashLastTargetHash = hash;

  if (!smooth || !dashCurrentValid) {
    dashShowImmediateTarget();
    return;
  }

  if (dashTransitionActive) {
    lastDashTransitionFrameMs = 0;
    dashRenderTransition();
  }

  uint16_t count = dashLedCount();
  for (int i = 0; i < count; i++) {
    dashStartPixels[i] = dashCurrentPixels[i];
  }

  dashTransitionStartMs = millis();
  lastDashTransitionFrameMs = 0;
  dashTransitionActive = true;
  dashRenderTransition();
}

void dashOff() {
  for (int i = 0; i < NUM_LEDS; i++) {
    dashTargetPixels[i] = 0;
    dashImmediatePixels[i] = false;
  }
  dashApplyTarget(true);
}

void dashSetAll(uint32_t c) {
  uint16_t count = dashLedCount();
  for (int i = 0; i < NUM_LEDS; i++) {
    dashTargetPixels[i] = i < count ? c : 0;
    dashImmediatePixels[i] = false;
  }
  dashApplyTarget(true);
}

void resetDashEffect() {
  lastDashEffectMs = 0;
  dashEffectStep = 0;
}

bool dashEffectTick(uint16_t intervalMs = 28) {
  uint32_t now = millis();
  if (now - lastDashEffectMs >= intervalMs) {
    lastDashEffectMs = now;
    dashEffectStep++;
    return true;
  }
  return false;
}

void dashBase() {
  dashSetAll(dashColor(baseR, baseG, baseB));
}

void dashSegment(bool left, bool right, uint8_t percent, uint32_t c, uint32_t bg, bool immediateSegment) {
  if (percent < 1) percent = 1;
  if (percent > 50) percent = 50;

  uint16_t count = dashLedCount();
  int segment = max(1, (count * percent) / 100);
  for (int i = 0; i < NUM_LEDS; i++) {
    dashTargetPixels[i] = i < count ? bg : 0;
    dashImmediatePixels[i] = false;
  }

  if (right) {
    for (int i = 0; i < segment; i++) {
      dashTargetPixels[i] = c;
      dashImmediatePixels[i] = immediateSegment;
    }
  }

  if (left) {
    for (int i = count - segment; i < count; i++) {
      dashTargetPixels[i] = c;
      dashImmediatePixels[i] = immediateSegment;
    }
  }

  dashApplyTarget(true);
}

uint16_t chargeSweepIntervalMs() {
  if (cpEvseChargeType == CP_CHARGE_TYPE_DC) return 12;
  if (cpEvseChargeType == CP_CHARGE_TYPE_AC) return 65;
  return 30;
}

uint32_t dimDashColor(uint8_t r, uint8_t g, uint8_t b, uint8_t scale) {
  return dashColor(scaledByte(r, scale), scaledByte(g, scale), scaledByte(b, scale));
}

void dashChargeSweep() {
  dashTransitionActive = false;

  uint8_t soc = batterySocPercent;
  if (soc > 100) soc = 100;

  uint16_t count = dashLedCount();
  int lit = map(soc, 0, 100, 0, count);
  bool isGreen = soc >= CHARGE_GREEN_AT_PERCENT;
  uint8_t fullR = isGreen ? 0 : 255;
  uint8_t fullG = isGreen ? 255 : 0;
  uint8_t fullB = 0;
  uint32_t base = dimDashColor(fullR, fullG, fullB, 95);
  uint32_t pulse = dimDashColor(fullR, fullG, fullB, 255);

  if (lit <= 0) {
    dashOff();
    return;
  }

  if (!dashEffectTick(chargeSweepIntervalMs())) {
    flushDashBrightnessIfNeeded();
    return;
  }

  int sweepWidth = min(10, max(3, lit / 5));
  int sweepRange = lit + sweepWidth;
  int sweepHead = dashEffectStep % sweepRange;

  strip.clear();
  for (int i = 0; i < lit; i++) strip.setPixelColor(i, base);

  for (int j = 0; j < sweepWidth; j++) {
    int p = sweepHead - j;
      if (p >= 0 && p < lit) strip.setPixelColor(p, pulse);
  }

  strip.show();
  dashBrightnessDirty = false;
  dashCaptureCurrent();
}

void dashWelcome() {
  dashTransitionActive = false;

  if (!dashEffectTick(28)) {
    flushDashBrightnessIfNeeded();
    return;
  }

  uint16_t count = dashLedCount();
  int centerLeft = (count / 2) - 1;
  int centerRight = count / 2;
  int maxRadius = (count / 2) + 2;
  int radius = map(constrain((int)dashEffectStep, 0, 42), 0, 42, 0, maxRadius);

  strip.clear();
  for (int r = 0; r <= radius; r++) {
    int left = centerLeft - r;
    int right = centerRight + r;
    if (left >= 0) strip.setPixelColor(left, dashColor(baseR, baseG, baseB));
    if (right < count) strip.setPixelColor(right, dashColor(baseR, baseG, baseB));
  }
  strip.show();
  dashBrightnessDirty = false;
  dashCaptureCurrent();
}

void dashGoodbye() {
  dashTransitionActive = false;

  if (!dashEffectTick(28)) {
    flushDashBrightnessIfNeeded();
    return;
  }

  uint16_t count = dashLedCount();
  int centerLeft = (count / 2) - 1;
  int centerRight = count / 2;
  int maxRadius = (count / 2) + 2;
  int radius = map(constrain((int)dashEffectStep, 0, 42), 0, 42, maxRadius, 0);

  strip.clear();
  for (int r = 0; r <= radius; r++) {
    int left = centerLeft - r;
    int right = centerRight + r;
    if (left >= 0) strip.setPixelColor(left, dashColor(baseR, baseG, baseB));
    if (right < count) strip.setPixelColor(right, dashColor(baseR, baseG, baseB));
  }
  strip.show();
  dashBrightnessDirty = false;
  dashCaptureCurrent();
}

void refreshDash() {
  updateDashBrightness();

  if (dashPowerOff) {
    dashOff();
    return;
  }

  bool chargeNow = chargeDashEnabled && chargingActive && lastChargeFrameSeen > 0 &&
                   millis() - lastChargeFrameSeen < 5000 &&
                   chargeAnimationStartMs > 0 &&
                   millis() - chargeAnimationStartMs < CHARGE_ANIMATION_TIMEOUT_MS;

  if (chargeNow) {
    dashChargeSweep();
    return;
  }

  if (welcomeUntilMs > millis()) {
    dashWelcome();
    return;
  }

  if (goodbyeActive) {
    if (goodbyeUntilMs > millis()) {
      dashGoodbye();
      return;
    }
    goodbyeActive = false;
    dashOff();
    return;
  }

  if (!carAwake) {
    dashOff();
    return;
  }

  bool leftDoorOpen = doorFrontLeftOpen || doorRearLeftOpen;
  bool rightDoorOpen = doorFrontRightOpen || doorRearRightOpen;
  bool blindLeftAllowed = blindSpotDashEnabled && blindLeftActive &&
                          (!blindSpotOnlyWithBlinker || blinkerLeftPulse);
  bool blindRightAllowed = blindSpotDashEnabled && blindRightActive &&
                           (!blindSpotOnlyWithBlinker || blinkerRightPulse);
  uint32_t bg = dashColor(baseR, baseG, baseB);

  bool blindWasShown = lastBlindLeftShown || lastBlindRightShown;
  bool blindIsShown = blindLeftAllowed || blindRightAllowed;
  if (blindWasShown && !blindIsShown) {
    dashLastTargetHash = 0;
  }

  if (doorOpenHighlightEnabled && (leftDoorOpen || rightDoorOpen)) {
    dashSegment(leftDoorOpen, rightDoorOpen, 25, dashColor(255, 120, 0), bg, true);
    lastBlindLeftShown = false;
    lastBlindRightShown = false;
  } else if (autopilotDashEnabled && autopilotActive) {
    dashSetAll(dashColor(autopilotR, autopilotG, autopilotB));
    lastBlindLeftShown = false;
    lastBlindRightShown = false;
  } else if (blindLeftAllowed || blindRightAllowed) {
    dashSegment(blindLeftAllowed, blindRightAllowed, blindSpotDashPercent, dashColor(255, 120, 0), bg, true);
    lastBlindLeftShown = blindLeftAllowed;
    lastBlindRightShown = blindRightAllowed;
  } else {
    dashBase();
    lastBlindLeftShown = false;
    lastBlindRightShown = false;
  }
}

// =====================================================
// Setup TWAI
// =====================================================

void setupTwaiBus(
  const char *name,
  twai_handle_t *handle,
  int controllerId,
  gpio_num_t txPin,
  gpio_num_t rxPin
) {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
    txPin,
    rxPin,
    TWAI_MODE_LISTEN_ONLY
  );
  g_config.controller_id = controllerId;
  g_config.tx_queue_len = 0;

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t result = twai_driver_install_v2(
    &g_config,
    &t_config,
    &f_config,
    handle
  );
  if (result != ESP_OK) {
    Serial.print("TWAI install failed ");
    Serial.print(name);
    Serial.print(": ");
    Serial.println(result);
    delay(1000);
    ESP.restart();
  }

  result = twai_start_v2(*handle);
  if (result != ESP_OK) {
    Serial.print("TWAI start failed ");
    Serial.print(name);
    Serial.print(": ");
    Serial.println(result);
    delay(1000);
    ESP.restart();
  }

  Serial.print("TWAI ready ");
  Serial.print(name);
  Serial.print(" TX=");
  Serial.print((int)txPin);
  Serial.print(" RX=");
  Serial.println((int)rxPin);
}

void setupCAN() {
  setupTwaiBus("VehicleBus", &vehicleTwai, 0, VEHICLE_CAN_TX_PIN, VEHICLE_CAN_RX_PIN);
  setupTwaiBus("ChassisBus", &chassisTwai, 1, CHASSIS_CAN_TX_PIN, CHASSIS_CAN_RX_PIN);
}

// =====================================================
// ESP-NOW + OTA
// =====================================================

void startOtaAp(uint8_t minutes);
void stopOtaAp();
void handleSystemCommand(const uint8_t *incomingData, int len);

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len);
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len);
#endif

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init fehlgeschlagen");
    delay(1000);
    ESP.restart();
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("ESP-NOW Broadcast Peer Fehler");
      delay(1000);
      ESP.restart();
    }
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW bereit");
}

void startOtaAp(uint8_t minutes) {
  if (minutes < 1) minutes = OTA_DEFAULT_MINUTES;
  if (minutes > 30) minutes = 30;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(OTA_AP_SSID, OTA_AP_PASS, ESPNOW_CHANNEL);
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (!otaApActive) {
    server.begin();
  }

  otaApActive = true;
  otaApUntilMs = millis() + (uint32_t)minutes * 60000UL;

  Serial.print("OTA AP aktiv fuer ");
  Serial.print(minutes);
  Serial.println(" Minuten");
  Serial.print("SSID: ");
  Serial.println(OTA_AP_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
}

void stopOtaAp() {
  if (!otaApActive) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);

  otaApActive = false;
  otaApUntilMs = 0;

  Serial.println("OTA AP deaktiviert");
}

void handleSystemCommand(const uint8_t *incomingData, int len) {
  if (len != sizeof(SystemCommand)) return;

  SystemCommand cmd;
  memcpy(&cmd, incomingData, sizeof(cmd));

  if (cmd.magic != 0xC6) return;
  if (cmd.version != 1) return;

  if (cmd.command == 1) {
    startOtaAp(cmd.minutes);
  }
}

void handleDashSettings(const uint8_t *incomingData, int len) {
  if (len != sizeof(DashSettingsPacket)) return;

  DashSettingsPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  if (packet.packetType != DASH_SETTINGS_PACKET_TYPE) return;
  if (packet.version != 1) return;

  bool baseChanged =
    baseR != packet.baseR ||
    baseG != packet.baseG ||
    baseB != packet.baseB ||
    baseEffect != packet.baseEffect ||
    baseSpeed != packet.baseSpeed ||
    baseIntensity != packet.baseIntensity ||
    manualBrightness != packet.manualBrightness ||
    autoBrightness != packet.autoBrightness ||
    dashPowerOff != packet.powerOff;

  baseR = packet.baseR;
  baseG = packet.baseG;
  baseB = packet.baseB;
  baseEffect = packet.baseEffect;
  baseSpeed = packet.baseSpeed;
  baseIntensity = packet.baseIntensity;
  manualBrightness = packet.manualBrightness;
  autoBrightness = packet.autoBrightness;
  dashPowerOff = packet.powerOff;
  chargeDashEnabled = packet.chargeDashEnabled;
  autopilotDashEnabled = packet.autopilotDashEnabled;
  autopilotR = packet.autopilotR;
  autopilotG = packet.autopilotG;
  autopilotB = packet.autopilotB;
  blindSpotDashEnabled = packet.blindSpotDashEnabled;
  blindSpotOnlyWithBlinker = packet.blindSpotOnlyWithBlinker;
  blindSpotDashPercent = constrain(packet.blindSpotDashPercent, 1, 50);
  activeDashLedCount = constrain(packet.dashLedCount, 1, NUM_LEDS);
  doorOpenHighlightEnabled = packet.doorOpenHighlightEnabled;
  welcomeAnimationEnabled = packet.welcomeAnimationEnabled;
  goodbyeAnimationEnabled = packet.goodbyeAnimationEnabled;

  if (baseChanged) {
    dashLastTargetHash = 0;
    dashBrightnessDirty = true;
  }

  Serial.print("DashSettings empfangen | AP blau=");
  Serial.print(autopilotDashEnabled);
  Serial.print(" Base RGB=");
  Serial.print(baseR);
  Serial.print(",");
  Serial.print(baseG);
  Serial.print(",");
  Serial.print(baseB);
  Serial.print(" autoBright=");
  Serial.print(autoBrightness);
  Serial.print(" manualBright=");
  Serial.print(manualBrightness);
  Serial.print(" powerOff=");
  Serial.print(dashPowerOff);
  Serial.print(" AP RGB=");
  Serial.print(autopilotR);
  Serial.print(",");
  Serial.print(autopilotG);
  Serial.print(",");
  Serial.print(autopilotB);
  Serial.print(" charge=");
  Serial.print(chargeDashEnabled);
  Serial.print(" blind=");
  Serial.print(blindSpotDashEnabled);
  Serial.print(" leds=");
  Serial.println(activeDashLedCount);
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (len >= 1 && incomingData[0] == DASH_SETTINGS_PACKET_TYPE) {
    handleDashSettings(incomingData, len);
    return;
  }

  handleSystemCommand(incomingData, len);
}

String otaPage() {
  String page;
  page += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Tesla Dash CAN C6 OTA</title></head><body style='font-family:system-ui;margin:24px'>";
  page += "<h1>Tesla Dash CAN C6 OTA</h1>";
  page += "<p>Firmware .bin auswaehlen und hochladen.</p>";
  page += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  page += "<input type='file' name='update'><button type='submit'>Flash</button></form>";
  page += "<p><a href='/'>Status</a></p></body></html>";
  return page;
}

String statusPage() {
  String page;
  page += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<meta http-equiv='refresh' content='3'>";
  page += "<title>Tesla Dash CAN C6</title></head><body style='font-family:system-ui;margin:24px'>";
  page += "<h1>Tesla Dash CAN C6</h1>";
  page += "<p>Vehicle age: " + String(lastAnyRelevantCanSeen == 0 ? -1 : (int)(millis() - lastAnyRelevantCanSeen)) + " ms</p>";
  page += "<p>Chassis age: " + String(last399Seen == 0 ? -1 : (int)(millis() - last399Seen)) + " ms</p>";
  page += "<p>SOC: " + String(batterySocPercent) + "% | Charging: " + String(chargingActive) + " | " + String(chargeStatusToText(chargeStatus)) + "</p>";
  page += "<p>Awake: " + String(vehicleAwake) + " | Folded: " + String(mirrorsFolded) + "</p>";
  page += "<p>AP: " + String(autopilotActive) + " | Blind L/R: " + String(blindLeftActive) + "/" + String(blindRightActive) + "</p>";
  page += "<p>Settings: AP " + String(autopilotDashEnabled) + " RGB " + String(autopilotR) + "," + String(autopilotG) + "," + String(autopilotB) + " | Charge " + String(chargeDashEnabled) + " | Blind " + String(blindSpotDashEnabled) + " | LEDs " + String(activeDashLedCount) + "</p>";
  page += "<p>OTA AP ist zeitbegrenzt aktiv. Danach geht der C6 zurueck in den sparsamen ESP-NOW Modus.</p>";
  page += "<p><a href='/update'>OTA Update</a></p></body></html>";
  return page;
}

void setupWebOta() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", statusPage());
  });

  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", otaPage());
  });

  server.on(
    "/update",
    HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "Update failed" : "Update ok, rebooting");
      delay(500);
      ESP.restart();
    },
    []() {
      HTTPUpload &upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        Serial.print("OTA start: ");
        Serial.println(upload.filename);

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.print("OTA success: ");
          Serial.print(upload.totalSize);
          Serial.println(" bytes");
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  Serial.println("Web OTA vorbereitet. AP startet nur per Master-Befehl.");
}

void setupWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.print("ESP-NOW STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("OTA AP im Normalbetrieb aus. Start ueber Master-Webseite: ");
  Serial.println(OTA_AP_SSID);
}

// =====================================================
// CAN decode
// =====================================================

void handleVehicleCAN(const twai_message_t &msg) {
  if (msg.extd) return;
  if (msg.rtr) return;

  const uint32_t id = msg.identifier;
  const uint8_t len = msg.data_length_code;
  const uint8_t *d = msg.data;

  bool relevantFrame = false;

  if (id == ID_BLINKER && len >= 8) {
    relevantFrame = true;

    uint8_t b0 = d[0];
    uint8_t b6 = d[6];

    blinkerLeftPulse =
      ((b0 & 0x03) != 0) ||
      ((b6 & 0x0C) != 0);

    blinkerRightPulse =
      ((b0 & 0x0C) != 0) ||
      ((b6 & 0x30) != 0);

    lastBlinkerFrameSeen = millis();
  } else if (id == ID_BRIGHTNESS && len >= 8) {
    relevantFrame = true;
    displayBrightnessRaw = d[4];
    displayBrightnessPercent = displayBrightnessRawToPercent(displayBrightnessRaw);
  } else if (id == ID_GEAR && len >= 8) {
    relevantFrame = true;
    gear = (d[2] >> 5) & 0x07;
    carInPark = (gear == GEAR_P);
  } else if (id == ID_VCLEFT_STATUS && len >= 8) {
    relevantFrame = true;

    uint8_t frontLatchStatus = d[0] & 0x0F;
    uint8_t rearLatchStatus = (d[0] >> 4) & 0x0F;

    doorFrontLeftOpen = latchMeansDoorOpen(frontLatchStatus);
    doorRearLeftOpen = latchMeansDoorOpen(rearLatchStatus);

    mirrorLeftState = (d[6] >> 4) & 0x07;
    updateMirrorLogic();
  } else if (id == ID_VCRIGHT_STATUS && len >= 8) {
    relevantFrame = true;

    uint8_t frontLatchStatus = d[0] & 0x0F;
    uint8_t rearLatchStatus = (d[0] >> 4) & 0x0F;

    doorFrontRightOpen = latchMeansDoorOpen(frontLatchStatus);
    doorRearRightOpen = latchMeansDoorOpen(rearLatchStatus);
    trunkOpen = latchMeansTrunkOpen(d[7] & 0x0F);

    mirrorRightState = (d[6] >> 4) & 0x07;
    updateMirrorLogic();
  } else if (id == ID_LIFTGATE_STATUS && len >= 1) {
    relevantFrame = true;
    uint8_t liftgateStatusIndex = d[0] & 0x03;
    if (liftgateStatusIndex == 0) {
      uint8_t liftgateState = (d[0] >> 3) & 0x0F;
      trunkOpen = (liftgateState != 5 && liftgateState != 8 && liftgateState != 9);
    }
  } else if (id == ID_BMS_SOC && len >= 8) {
    relevantFrame = true;
    uint16_t socRaw = extractIntelSignal(d, 10, 10);
    float socUi = socRaw * 0.1f;
    if (socUi < 0) socUi = 0;
    if (socUi > 100) socUi = 100;
    batterySocPercent = (uint8_t)(socUi + 0.5f);
    lastBmsSocSeen = millis();
  } else if (id == ID_BMS_STATUS && len >= 2) {
    relevantFrame = true;
    chargeStatus = (d[1] >> 3) & 0x07;
    updateChargingActive();
    lastChargeFrameSeen = millis();
  } else if (id == ID_CP_CHARGE_STATUS && len >= 6) {
    relevantFrame = true;
    cpHvChargeStatus = d[0] & 0x07;
    cpEvseChargeType = d[5] & 0x03;
    cpChargingActive = (cpHvChargeStatus == CP_CHARGE_ENABLED);
    updateChargingActive();
    lastChargeFrameSeen = millis();
  }

  if (relevantFrame) {
    lastAnyRelevantCanSeen = millis();

    if (!mirrorsFolded) {
      vehicleAwake = true;
    }

    if (anyDoorOpen() || anyMirrorUnfoldedOrUnfolding()) {
      vehicleAwake = true;
    }
  }
}

void handleChassisCAN(const twai_message_t &msg) {
  if (msg.extd) return;
  if (msg.rtr) return;
  if (msg.identifier != ID_DAS_STATUS || msg.data_length_code < 8) return;

  uint8_t b0 = msg.data[0];

  autopilotState = b0 & 0x0F;
  autopilotActive =
    autopilotState == 3 ||
    autopilotState == 4 ||
    autopilotState == 5;

  blindLeftRaw = (b0 >> 4) & 0x03;
  blindRightRaw = (b0 >> 6) & 0x03;

  blindLeftActive = blindLeftRaw > 0;
  blindRightActive = blindRightRaw > 0;

  last399Seen = millis();
}

void processCan() {
  twai_message_t msg;

  while (twai_receive_v2(vehicleTwai, &msg, 0) == ESP_OK) {
    handleVehicleCAN(msg);
  }

  while (twai_receive_v2(chassisTwai, &msg, 0) == ESP_OK) {
    handleChassisCAN(msg);
  }
}

// =====================================================
// ESP-NOW send
// =====================================================

void sendVehicleData() {
  vehicleData.packetType = 1;

  vehicleData.blinkerLeftPulse = blinkerLeftPulse;
  vehicleData.blinkerRightPulse = blinkerRightPulse;
  vehicleData.displayBrightnessRaw = displayBrightnessRaw;
  vehicleData.displayBrightnessPercent = displayBrightnessPercent;
  vehicleData.gear = gear;
  vehicleData.carInPark = carInPark;
  vehicleData.mirrorLeftState = mirrorLeftState;
  vehicleData.mirrorRightState = mirrorRightState;
  vehicleData.mirrorLeftFolded = mirrorLeftFolded;
  vehicleData.mirrorRightFolded = mirrorRightFolded;
  vehicleData.mirrorsFolded = mirrorsFolded;
  vehicleData.mirrorsUnfolded = mirrorsUnfolded;
  vehicleData.vehicleAwake = vehicleAwake;
  vehicleData.counter = vehicleSendCounter++;

  if (lastAnyRelevantCanSeen == 0) {
    vehicleData.lastVehicleCanAgeMs = 0xFFFFFFFF;
  } else {
    vehicleData.lastVehicleCanAgeMs = millis() - lastAnyRelevantCanSeen;
  }

  vehicleData.chargingActive = chargingActive;
  vehicleData.chargeStatus = chargeStatus;
  vehicleData.batterySocPercent = batterySocPercent;

  if (lastChargeFrameSeen == 0) {
    vehicleData.lastChargeCanAgeMs = 0xFFFFFFFF;
  } else {
    vehicleData.lastChargeCanAgeMs = millis() - lastChargeFrameSeen;
  }

  vehicleData.doorFrontLeftOpen = doorFrontLeftOpen;
  vehicleData.doorFrontRightOpen = doorFrontRightOpen;
  vehicleData.doorRearLeftOpen = doorRearLeftOpen;
  vehicleData.doorRearRightOpen = doorRearRightOpen;
  vehicleData.trunkOpen = trunkOpen;

  esp_now_send(broadcastAddress, (uint8_t*)&vehicleData, sizeof(vehicleData));
}

void sendChassisData() {
  chassisData.packetType = 2;
  chassisData.autopilotState = autopilotState;
  chassisData.autopilotActive = autopilotActive;
  chassisData.blindLeftRaw = blindLeftRaw;
  chassisData.blindRightRaw = blindRightRaw;
  chassisData.blindLeftActive = blindLeftActive;
  chassisData.blindRightActive = blindRightActive;
  chassisData.counter = chassisSendCounter++;

  esp_now_send(broadcastAddress, (uint8_t*)&chassisData, sizeof(chassisData));
}

void runFailsafes() {
  if (millis() - lastBlinkerFrameSeen > 1000) {
    blinkerLeftPulse = false;
    blinkerRightPulse = false;
  }

  if (lastChargeFrameSeen > 0 && millis() - lastChargeFrameSeen > 5000) {
    chargeStatus = BMS_CHARGE_DISCONNECTED;
    cpChargingActive = false;
    cpHvChargeStatus = 0;
    cpEvseChargeType = 0;
    updateChargingActive();
  }

  if (anyDoorOpen() || anyMirrorUnfoldedOrUnfolding()) {
    vehicleAwake = true;
  } else if (mirrorsFolded) {
    vehicleAwake = false;
  } else if (lastAnyRelevantCanSeen > 0 && millis() - lastAnyRelevantCanSeen > 30000) {
    vehicleAwake = false;
  }

  if (last399Seen > 0 && millis() - last399Seen > CHASSIS_CAN_TIMEOUT_MS) {
    autopilotState = 0;
    autopilotActive = false;
    blindLeftRaw = 0;
    blindRightRaw = 0;
    blindLeftActive = false;
    blindRightActive = false;
  }
}

void printDebug() {
  Serial.print("Combined C6 | awake=");
  Serial.print(vehicleAwake);
  Serial.print(" folded=");
  Serial.print(mirrorsFolded);
  Serial.print(" bright=");
  Serial.print(displayBrightnessPercent, 1);
  Serial.print("% soc=");
  Serial.print(batterySocPercent);
  Serial.print("% charge=");
  Serial.print(chargeStatusToText(chargeStatus));
  Serial.print(" cp=");
  Serial.print(cpHvChargeStatus);
  Serial.print(" type=");
  Serial.print(cpEvseChargeType);
  Serial.print(" chargeAnimMin=");
  if (chargeAnimationStartMs == 0) {
    Serial.print("-");
  } else {
    Serial.print((millis() - chargeAnimationStartMs) / 60000);
  }
  Serial.print(" doors FL=");
  Serial.print(doorFrontLeftOpen);
  Serial.print(" FR=");
  Serial.print(doorFrontRightOpen);
  Serial.print(" RL=");
  Serial.print(doorRearLeftOpen);
  Serial.print(" RR=");
  Serial.print(doorRearRightOpen);
  Serial.print(" trunk=");
  Serial.print(trunkOpen);
  Serial.print(" AP=");
  Serial.print(autopilotActive);
  Serial.print(" BlindL=");
  Serial.print(blindLeftActive);
  Serial.print(" BlindR=");
  Serial.println(blindRightActive);
}

// =====================================================
// Setup / Loop
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("======================================");
  Serial.println("Tesla Dash CAN-Bridge ESP32-C6 OTA");
  Serial.println("VehicleBus + ChassisBus + Dash LEDs + ESP-NOW");
  Serial.println("======================================");

  memset(&vehicleData, 0, sizeof(vehicleData));
  memset(&chassisData, 0, sizeof(chassisData));

  vehicleData.packetType = 1;
  vehicleData.displayBrightnessRaw = 0xC8;
  vehicleData.displayBrightnessPercent = 100.0f;
  vehicleData.gear = GEAR_UNKNOWN;
  vehicleData.vehicleAwake = true;
  vehicleData.chargeStatus = BMS_CHARGE_DISCONNECTED;

  strip.begin();
  strip.setBrightness(0);
  strip.clear();
  strip.show();

  setupWifi();
  setupEspNow();
  setupWebOta();
  setupCAN();

  dashOff();
}

void loop() {
  if (otaApActive) {
    server.handleClient();

    if (otaApUntilMs > 0 && (int32_t)(millis() - otaApUntilMs) >= 0) {
      stopOtaAp();
    }
  }

  processCan();
  runFailsafes();

  carAwake = vehicleAwake && !mirrorsFolded;
  bool chargeNow = chargeDashEnabled && chargingActive && lastChargeFrameSeen > 0 &&
                   millis() - lastChargeFrameSeen < 5000 &&
                   chargeAnimationStartMs > 0 &&
                   millis() - chargeAnimationStartMs < CHARGE_ANIMATION_TIMEOUT_MS;

  if (carAwake != lastCarAwake) {
    lastCarAwake = carAwake;

    if (carAwake) {
      goodbyeActive = false;
      goodbyeUntilMs = 0;
      if (welcomeAnimationEnabled && !welcomePlayedForWake) {
        welcomePlayedForWake = true;
        welcomeUntilMs = millis() + 2200;
      }
      resetDashEffect();
    } else {
      welcomePlayedForWake = false;
      welcomeUntilMs = 0;

      if (chargeNow) {
        goodbyeActive = false;
        goodbyeUntilMs = 0;
      } else if (goodbyeAnimationEnabled) {
        goodbyeActive = true;
        goodbyeUntilMs = millis() + 2200;
        resetDashEffect();
      } else {
        goodbyeActive = false;
        goodbyeUntilMs = 0;
      }
    }
  }

  refreshDash();

  if (millis() - lastVehicleSend >= 50) {
    lastVehicleSend = millis();
    sendVehicleData();
  }

  if (millis() - lastChassisSend >= 50) {
    lastChassisSend = millis();
    sendChassisData();
  }

  if (millis() - lastDebug >= 1000) {
    lastDebug = millis();
    printDebug();
  }
}
