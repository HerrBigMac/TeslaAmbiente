#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Preferences.h>

// =======================================================
// Tesla Ambiente Master
// Empfaengt VehicleBus + ChassisBus per ESP-NOW
// und sendet LED-Kommandos an Tueren + Dashboard.
// =======================================================
//
// Ziele bleiben 1 Zeichen, damit verbaute Rear-Slaves kompatibel bleiben:
// 'L' = hinten rechts, bestehend
// 'R' = hinten links, bestehend
// 'F' = vorne rechts
// 'G' = vorne links
// 'D' = Dashboard
// 'A' = alle
//
// Start-Standard: Rot, Effekt statisch.
// Auto-Sleep: alle LEDs aus, wenn VehicleBus mirrorsFolded/vehicleAwake=false meldet.
// Wake: Standard/Preset wieder starten.
// CAN-Helligkeit optional: displayBrightnessPercent vom VehicleBus.
// Totwinkel/Blinker/Laden/Autopilot/Tuer/Wake-Goodbye sind im Weboverlay schaltbar.
// Autopilot: Dashboard komplett blau, danach Rueckkehr zur Ursprungsfarbe.
// =======================================================

#define ESPNOW_CHANNEL 6

// WLAN fuer Weboverlay
// Bei Bedarf auf dein Heimnetz aendern.
#define WIFI_AP_MODE true
#define WIFI_AP_SSID "Tesla-Ambiente"
#define WIFI_AP_PASS "12345678"

// Optional fuer Heimnetz:
#define WIFI_STA_SSID ""
#define WIFI_STA_PASS ""

// Dashboard-Laenge laut Angabe
#define DASH_NUM_LEDS 122
#define DASH_MAX_LEDS 144
#define DOOR_NUM_LEDS 130

// LED Effekte muessen zu deinen Slaves passen
#define EFFECT_OFF 0
#define EFFECT_STATIC 1
#define EFFECT_BLINDSPOT 20
#define EFFECT_DASH_QUARTER 21
#define EFFECT_CHARGE_SOC 22
#define EFFECT_DASH_WELCOME 23
#define EFFECT_DASH_GOODBYE 24

#define MODE_NORMAL 0
#define MODE_BLINDSPOT 1

#define TARGET_REAR_RIGHT 'L'
#define TARGET_REAR_LEFT 'R'
#define TARGET_FRONT_RIGHT 'F'
#define TARGET_FRONT_LEFT 'G'
#define TARGET_DASH 'D'
#define TARGET_ALL 'A'

#define LED_ACK_PACKET_TYPE 0xA8
#define DASH_SETTINGS_PACKET_TYPE 0xC7
#define LED_ACK_RETRY_MS 90
#define LED_ACK_MAX_ATTEMPTS 3
#define LED_REFRESH_INTERVAL_MS 500

// Im aktuellen Aufbau steuert der Dash/CAN-C6 den Dash direkt.
// Der Master sendet daher nur Einstellungen an den Dash/CAN-C6, aber keine D-Ziel-LED-Kommandos.
#define MASTER_SENDS_DASH_LED_COMMANDS false

// Globale Leistungsbegrenzung:
// 255 intern bedeutet nur 30 Prozent echte LED-Helligkeit.
// So bleibt "100%" im Web/CAN dein neues sicheres Maximum.
#define MAX_LED_BRIGHTNESS_PERCENT 15
#define MAX_LED_BRIGHTNESS ((255 * MAX_LED_BRIGHTNESS_PERCENT) / 100)

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

const byte DNS_PORT = 53;

// =======================================================
// Pakete von Readern
// =======================================================

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
  uint8_t magic;
  uint8_t version;
  char target;
  uint8_t power;
  uint8_t mode;
  uint8_t effect;
  uint8_t r1;
  uint8_t g1;
  uint8_t b1;
  uint8_t r2;
  uint8_t g2;
  uint8_t b2;
  uint8_t r3;
  uint8_t g3;
  uint8_t b3;
  uint8_t brightness;
  uint8_t speed;
  uint8_t intensity;
  uint8_t progress;
  uint16_t ledStart;
  uint16_t ledEnd;
  uint32_t sequence;
} LedCommand;

typedef struct __attribute__((packed)) {
  uint8_t packetType;  // 0xA8
  uint8_t magic;       // 0xA7
  uint8_t version;     // 1
  char device;         // antwortender Slave: L/R/F/G
  char target;         // Ziel aus dem empfangenen LedCommand
  uint32_t sequence;
  uint8_t accepted;
} LedAckPacket;

typedef struct __attribute__((packed)) {
  uint8_t magic;       // 0xC6
  uint8_t version;     // 1
  uint8_t command;     // 1 = Combined-C6 OTA AP, 2 = Dashboard OTA AP, 3 = Door OTA AP
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

struct PendingLedAck {
  bool active;
  char target;
  uint32_t sequence;
  uint8_t attempts;
  uint32_t lastSendMs;
  LedCommand cmd;
};

struct Preset {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t effect;
  uint8_t speed;
  uint8_t intensity;
  uint8_t manualBrightness;
  bool autoBrightness;
};

struct FeatureSettings {
  bool chargeDashEnabled;
  bool autopilotDashEnabled;
  uint8_t autopilotR;
  uint8_t autopilotG;
  uint8_t autopilotB;
  bool blindSpotDashEnabled;
  bool blindSpotOnlyWithBlinker;
  uint8_t blindSpotDashPercent;
  uint16_t dashLedCount;
  bool blinkerDashEnabled;
  uint8_t blinkerDashPercent;
  bool doorOpenHighlightEnabled;
  bool welcomeAnimationEnabled;
  bool goodbyeAnimationEnabled;
};

struct DoorLedRange {
  uint16_t start;
  uint16_t end;
};

VehiclePacket vehicle = {};
ChassisPacket chassis = {};
Preset current = {};
Preset presets[5];
Preset zoneConfig[5];
FeatureSettings features = {};
DoorLedRange doorLedRanges[4];
PendingLedAck pendingLedAcks[4] = {};

uint32_t ledSequence = 1;
uint32_t lastVehicleMs = 0;
uint32_t lastChassisMs = 0;
uint32_t lastLedRefreshMs = 0;
uint32_t lastDebugMs = 0;
uint32_t lastDashSettingsSendMs = 0;
uint32_t espNowSendOk = 0;
uint32_t espNowSendFail = 0;
uint32_t ledAckOk = 0;
uint32_t ledAckRetry = 0;
uint32_t ledAckGiveUp = 0;
uint32_t lastStateHash = 0;
uint32_t recoveryUntilMs = 0;
uint32_t lastRecoverySendMs = 0;
uint32_t welcomeUntilMs = 0;
uint32_t goodbyeUntilMs = 0;
bool goodbyeActive = false;
bool welcomePlayedForWake = false;
esp_now_send_status_t lastSendStatus = ESP_NOW_SEND_FAIL;

bool hasVehicle = false;
bool hasChassis = false;
bool carAwake = false;
bool lastCarAwake = false;
uint8_t activePreset = 0;
uint8_t defaultPreset = 0;
char selectedTarget = TARGET_ALL;
bool webOverrideActive = false;
bool webPowerOff = false;

// =======================================================
// Hilfen
// =======================================================

uint8_t clampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return (uint8_t)value;
}

uint8_t percentToBrightness(float percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return clampByte((int)(percent * MAX_LED_BRIGHTNESS / 100.0f));
}

uint8_t limitBrightness(uint8_t brightness) {
  return map(brightness, 0, 255, 0, MAX_LED_BRIGHTNESS);
}

uint8_t effectiveBrightness(bool forceConfiguredBrightness) {
  if (!forceConfiguredBrightness && current.autoBrightness && hasVehicle) {
    return percentToBrightness(vehicle.displayBrightnessPercent);
  }

  return limitBrightness(current.manualBrightness);
}

uint8_t effectiveBrightnessFor(const Preset &preset, bool forceConfiguredBrightness) {
  if (!forceConfiguredBrightness && preset.autoBrightness && hasVehicle) {
    return percentToBrightness(vehicle.displayBrightnessPercent);
  }

  return limitBrightness(preset.manualBrightness);
}

String colorToHex(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return String(buf);
}

void parseHexColor(String hex, uint8_t &r, uint8_t &g, uint8_t &b) {
  hex.trim();
  if (hex.startsWith("#")) hex.remove(0, 1);
  if (hex.length() != 6) return;

  long value = strtol(hex.c_str(), NULL, 16);
  r = (value >> 16) & 0xFF;
  g = (value >> 8) & 0xFF;
  b = value & 0xFF;
}

String urlDecode(String s) {
  s.replace("+", " ");
  s.replace("%23", "#");
  s.replace("%2523", "#");
  s.replace("%2F", "/");
  s.replace("%3A", ":");
  return s;
}

int targetToIndex(char target) {
  switch (target) {
    case TARGET_REAR_LEFT: return 0;
    case TARGET_REAR_RIGHT: return 1;
    case TARGET_FRONT_LEFT: return 2;
    case TARGET_FRONT_RIGHT: return 3;
    case TARGET_DASH: return 4;
    default: return -1;
  }
}

char indexToTarget(int index) {
  switch (index) {
    case 0: return TARGET_REAR_LEFT;
    case 1: return TARGET_REAR_RIGHT;
    case 2: return TARGET_FRONT_LEFT;
    case 3: return TARGET_FRONT_RIGHT;
    case 4: return TARGET_DASH;
    default: return TARGET_ALL;
  }
}

const char* targetToText(char target) {
  switch (target) {
    case TARGET_REAR_LEFT: return "Hinten links";
    case TARGET_REAR_RIGHT: return "Hinten rechts";
    case TARGET_FRONT_LEFT: return "Vorne links";
    case TARGET_FRONT_RIGHT: return "Vorne rechts";
    case TARGET_DASH: return "Dashboard";
    case TARGET_ALL: return "Alle";
    default: return "Alle";
  }
}

int doorAckIndex(char target) {
  switch (target) {
    case TARGET_REAR_LEFT: return 0;
    case TARGET_REAR_RIGHT: return 1;
    case TARGET_FRONT_LEFT: return 2;
    case TARGET_FRONT_RIGHT: return 3;
    default: return -1;
  }
}

bool isDoorTarget(char target) {
  return doorAckIndex(target) >= 0;
}

char parseTarget(String value) {
  value.trim();
  if (value.length() == 0) return TARGET_ALL;

  char target = value.charAt(0);
  if (targetToIndex(target) >= 0 || target == TARGET_ALL) {
    return target;
  }

  return TARGET_ALL;
}

Preset getSelectedConfig() {
  int index = targetToIndex(selectedTarget);
  if (index >= 0) return zoneConfig[index];
  return current;
}

void applyPresetToAllZones(const Preset &preset) {
  for (int i = 0; i < 5; i++) {
    zoneConfig[i] = preset;
  }
  current = preset;
}

// =======================================================
// Speicher
// =======================================================

void setFactoryPreset(Preset &p) {
  p.r = 255;
  p.g = 0;
  p.b = 0;
  p.effect = EFFECT_STATIC;
  p.speed = 120;
  p.intensity = 120;
  p.manualBrightness = 120;
  p.autoBrightness = true;
}

void setFactoryFeatures() {
  features.chargeDashEnabled = true;
  features.autopilotDashEnabled = true;
  features.autopilotR = 0;
  features.autopilotG = 70;
  features.autopilotB = 255;
  features.blindSpotDashEnabled = true;
  features.blindSpotOnlyWithBlinker = true;
  features.blindSpotDashPercent = 25;
  features.dashLedCount = DASH_NUM_LEDS;
  features.blinkerDashEnabled = false;
  features.blinkerDashPercent = 25;
  features.doorOpenHighlightEnabled = true;
  features.welcomeAnimationEnabled = true;
  features.goodbyeAnimationEnabled = true;
}

void setFactoryDoorLedRanges() {
  for (int i = 0; i < 4; i++) {
    doorLedRanges[i].start = 0;
    doorLedRanges[i].end = DOOR_NUM_LEDS - 1;
  }
}

void loadConfig() {
  prefs.begin("ambimaster", true);
  defaultPreset = prefs.getUChar("default", 0);
  if (defaultPreset > 4) defaultPreset = 0;

  for (int i = 0; i < 5; i++) {
    String key = "preset" + String(i);
    if (prefs.getBytesLength(key.c_str()) == sizeof(Preset)) {
      prefs.getBytes(key.c_str(), &presets[i], sizeof(Preset));
    } else {
      setFactoryPreset(presets[i]);
    }
  }

  for (int i = 0; i < 5; i++) {
    String key = "zone" + String(i);
    if (prefs.getBytesLength(key.c_str()) == sizeof(Preset)) {
      prefs.getBytes(key.c_str(), &zoneConfig[i], sizeof(Preset));
    } else {
      zoneConfig[i] = presets[defaultPreset];
    }
  }

  if (prefs.getBytesLength("features") == sizeof(FeatureSettings)) {
    prefs.getBytes("features", &features, sizeof(FeatureSettings));
  } else {
    setFactoryFeatures();
  }

  if (prefs.getBytesLength("doorRanges") == sizeof(doorLedRanges)) {
    prefs.getBytes("doorRanges", &doorLedRanges, sizeof(doorLedRanges));
  } else {
    setFactoryDoorLedRanges();
  }

  if (features.blindSpotDashPercent < 1 || features.blindSpotDashPercent > 50) {
    features.blindSpotDashPercent = 25;
  }

  if (features.blinkerDashPercent < 1 || features.blinkerDashPercent > 50) {
    features.blinkerDashPercent = 25;
  }

  if (features.dashLedCount < 1 || features.dashLedCount > DASH_MAX_LEDS) {
    features.dashLedCount = DASH_NUM_LEDS;
  }

  for (int i = 0; i < 4; i++) {
    if (doorLedRanges[i].start >= DOOR_NUM_LEDS) doorLedRanges[i].start = 0;
    if (doorLedRanges[i].end >= DOOR_NUM_LEDS) doorLedRanges[i].end = DOOR_NUM_LEDS - 1;
    if (doorLedRanges[i].start > doorLedRanges[i].end) {
      doorLedRanges[i].start = 0;
      doorLedRanges[i].end = DOOR_NUM_LEDS - 1;
    }
  }

  prefs.end();

  activePreset = defaultPreset;
  current = presets[activePreset];
  selectedTarget = TARGET_ALL;
}

void saveFeatureSettings() {
  prefs.begin("ambimaster", false);
  prefs.putBytes("features", &features, sizeof(FeatureSettings));
  prefs.putBytes("doorRanges", &doorLedRanges, sizeof(doorLedRanges));
  prefs.end();
}

void savePreset(uint8_t index) {
  if (index > 4) return;
  presets[index] = current;

  prefs.begin("ambimaster", false);
  String key = "preset" + String(index);
  prefs.putBytes(key.c_str(), &presets[index], sizeof(Preset));
  prefs.end();
}

void saveDefaultPreset(uint8_t index) {
  if (index > 4) return;
  defaultPreset = index;
  prefs.begin("ambimaster", false);
  prefs.putUChar("default", defaultPreset);
  prefs.end();
}

void saveZoneConfig(uint8_t index) {
  if (index > 4) return;

  prefs.begin("ambimaster", false);
  String key = "zone" + String(index);
  prefs.putBytes(key.c_str(), &zoneConfig[index], sizeof(Preset));
  prefs.end();
}

void saveAllZoneConfigsSingleSession() {
  prefs.begin("ambimaster", false);
  for (int i = 0; i < 5; i++) {
    String key = "zone" + String(i);
    prefs.putBytes(key.c_str(), &zoneConfig[i], sizeof(Preset));
  }
  prefs.end();
}

void saveAllZoneConfigs() {
  saveAllZoneConfigsSingleSession();
}

void saveSelectedTargetConfig() {
  int index = targetToIndex(selectedTarget);
  if (index >= 0) {
    zoneConfig[index] = current;
    saveZoneConfig(index);
  } else {
    for (int i = 0; i < 5; i++) {
      zoneConfig[i] = current;
    }
    saveAllZoneConfigs();
  }
}

void printCurrentWebConfig(const char *label) {
  Serial.print(label);
  Serial.print(" | target=");
  Serial.print(selectedTarget);
  Serial.print(" ");
  Serial.print(targetToText(selectedTarget));
  Serial.print(" rgb=");
  Serial.print(current.r);
  Serial.print(",");
  Serial.print(current.g);
  Serial.print(",");
  Serial.print(current.b);
  Serial.print(" effect=");
  Serial.print(current.effect);
  Serial.print(" brightness=");
  Serial.print(current.manualBrightness);
  Serial.print(" autoBrightness=");
  Serial.println(current.autoBrightness);
}

uint32_t mixHash(uint32_t hash, uint32_t value) {
  hash ^= value + 0x9E3779B9 + (hash << 6) + (hash >> 2);
  return hash;
}

uint32_t computeStateHash() {
  uint32_t hash = 2166136261UL;

  hash = mixHash(hash, webOverrideActive);
  hash = mixHash(hash, webPowerOff);
  hash = mixHash(hash, hasVehicle);
  hash = mixHash(hash, hasChassis);
  hash = mixHash(hash, vehicle.vehicleAwake);
  hash = mixHash(hash, vehicle.mirrorsFolded);
  hash = mixHash(hash, vehicle.chargingActive);
  hash = mixHash(hash, vehicle.batterySocPercent);
  hash = mixHash(hash, chassis.autopilotActive);
  hash = mixHash(hash, chassis.blindLeftActive);
  hash = mixHash(hash, chassis.blindRightActive);
  hash = mixHash(hash, vehicle.blinkerLeftPulse);
  hash = mixHash(hash, vehicle.blinkerRightPulse);
  hash = mixHash(hash, vehicle.doorFrontLeftOpen);
  hash = mixHash(hash, vehicle.doorFrontRightOpen);
  hash = mixHash(hash, vehicle.doorRearLeftOpen);
  hash = mixHash(hash, vehicle.doorRearRightOpen);
  hash = mixHash(hash, vehicle.trunkOpen);
  hash = mixHash(hash, features.chargeDashEnabled);
  hash = mixHash(hash, features.autopilotDashEnabled);
  hash = mixHash(hash, features.autopilotR);
  hash = mixHash(hash, features.autopilotG);
  hash = mixHash(hash, features.autopilotB);
  hash = mixHash(hash, features.blindSpotDashEnabled);
  hash = mixHash(hash, features.blindSpotOnlyWithBlinker);
  hash = mixHash(hash, features.blindSpotDashPercent);
  hash = mixHash(hash, features.dashLedCount);
  hash = mixHash(hash, features.blinkerDashEnabled);
  hash = mixHash(hash, features.blinkerDashPercent);
  hash = mixHash(hash, features.doorOpenHighlightEnabled);
  hash = mixHash(hash, features.welcomeAnimationEnabled);
  hash = mixHash(hash, features.goodbyeAnimationEnabled);
  hash = mixHash(hash, (uint32_t)effectiveBrightnessFor(zoneConfig[0], false));
  hash = mixHash(hash, (uint32_t)effectiveBrightnessFor(zoneConfig[1], false));
  hash = mixHash(hash, (uint32_t)effectiveBrightnessFor(zoneConfig[2], false));
  hash = mixHash(hash, (uint32_t)effectiveBrightnessFor(zoneConfig[3], false));
  hash = mixHash(hash, (uint32_t)effectiveBrightnessFor(zoneConfig[4], false));

  for (int i = 0; i < 5; i++) {
    hash = mixHash(hash, zoneConfig[i].r);
    hash = mixHash(hash, zoneConfig[i].g);
    hash = mixHash(hash, zoneConfig[i].b);
    hash = mixHash(hash, zoneConfig[i].effect);
    hash = mixHash(hash, zoneConfig[i].speed);
    hash = mixHash(hash, zoneConfig[i].intensity);
    hash = mixHash(hash, zoneConfig[i].manualBrightness);
    hash = mixHash(hash, zoneConfig[i].autoBrightness);
  }

  for (int i = 0; i < 4; i++) {
    hash = mixHash(hash, doorLedRanges[i].start);
    hash = mixHash(hash, doorLedRanges[i].end);
  }

  return hash;
}

// =======================================================
// LED senden
// =======================================================

void rememberPendingLedAck(const LedCommand &cmd) {
  if (cmd.target == TARGET_ALL) {
    char doorTargets[] = {TARGET_REAR_LEFT, TARGET_REAR_RIGHT, TARGET_FRONT_LEFT, TARGET_FRONT_RIGHT};

    for (int i = 0; i < 4; i++) {
      LedCommand doorCmd = cmd;
      doorCmd.target = doorTargets[i];

      pendingLedAcks[i].active = true;
      pendingLedAcks[i].target = doorTargets[i];
      pendingLedAcks[i].sequence = doorCmd.sequence;
      pendingLedAcks[i].attempts = 1;
      pendingLedAcks[i].lastSendMs = millis();
      pendingLedAcks[i].cmd = doorCmd;
    }

    return;
  }

  int index = doorAckIndex(cmd.target);
  if (index < 0) return;

  pendingLedAcks[index].active = true;
  pendingLedAcks[index].target = cmd.target;
  pendingLedAcks[index].sequence = cmd.sequence;
  pendingLedAcks[index].attempts = 1;
  pendingLedAcks[index].lastSendMs = millis();
  pendingLedAcks[index].cmd = cmd;
}

void sendRawLedCommand(const LedCommand &cmd) {
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&cmd, sizeof(cmd));
  if (result != ESP_OK) {
    Serial.print("LED Sendefehler target ");
    Serial.print(cmd.target);
    Serial.print(": ");
    Serial.println(result);
  }
}

void processLedAckRetries() {
  uint32_t now = millis();

  for (int i = 0; i < 4; i++) {
    PendingLedAck &pending = pendingLedAcks[i];
    if (!pending.active) continue;
    if (now - pending.lastSendMs < LED_ACK_RETRY_MS) continue;

    if (pending.attempts >= LED_ACK_MAX_ATTEMPTS) {
      pending.active = false;
      ledAckGiveUp++;
      Serial.print("LED ACK fehlt, gebe auf target ");
      Serial.print(pending.target);
      Serial.print(" seq ");
      Serial.println(pending.sequence);
      continue;
    }

    pending.attempts++;
    pending.lastSendMs = now;
    ledAckRetry++;
    sendRawLedCommand(pending.cmd);

    Serial.print("LED ACK Retry ");
    Serial.print(pending.attempts);
    Serial.print("/");
    Serial.print(LED_ACK_MAX_ATTEMPTS);
    Serial.print(" target ");
    Serial.print(pending.target);
    Serial.print(" seq ");
    Serial.println(pending.sequence);
  }
}

void handleLedAck(const LedAckPacket &ack) {
  if (ack.magic != 0xA7 || ack.version != 1 || ack.accepted == 0) return;

  int index = doorAckIndex(ack.device);
  if (index < 0) return;

  PendingLedAck &pending = pendingLedAcks[index];
  if (!pending.active) return;
  if (pending.target != ack.device) return;
  if (pending.sequence != ack.sequence) return;

  pending.active = false;
  ledAckOk++;
}

void sendLedCommand(
  char target,
  uint8_t power,
  uint8_t mode,
  uint8_t effect,
  uint8_t r1,
  uint8_t g1,
  uint8_t b1,
  uint8_t r2,
  uint8_t g2,
  uint8_t b2,
  uint8_t brightness,
  uint8_t progress
) {
  LedCommand cmd = {};
  cmd.magic = 0xA7;
  cmd.version = 2;
  cmd.target = target;
  cmd.power = power;
  cmd.mode = mode;
  cmd.effect = effect;

  cmd.r1 = r1;
  cmd.g1 = g1;
  cmd.b1 = b1;

  cmd.r2 = r2;
  cmd.g2 = g2;
  cmd.b2 = b2;

  cmd.r3 = 0;
  cmd.g3 = 0;
  cmd.b3 = 0;

  cmd.brightness = brightness;
  cmd.speed = current.speed;
  cmd.intensity = current.intensity;
  cmd.progress = progress;
  int rangeIndex = targetToIndex(target);
  if (rangeIndex >= 0 && rangeIndex < 4) {
    cmd.ledStart = doorLedRanges[rangeIndex].start;
    cmd.ledEnd = doorLedRanges[rangeIndex].end;
  } else {
    cmd.ledStart = 0;
    cmd.ledEnd = DOOR_NUM_LEDS - 1;
  }
  cmd.sequence = ledSequence++;

  sendRawLedCommand(cmd);
  rememberPendingLedAck(cmd);
}

void rememberSendResult(esp_err_t result, char target) {
  if (result != ESP_OK) {
    Serial.print("LED Sendefehler target ");
    Serial.print(target);
    Serial.print(": ");
    Serial.println(result);
  }
}

void sendDashSettings() {
  Preset &dashPreset = zoneConfig[4];

  DashSettingsPacket packet = {};
  packet.packetType = DASH_SETTINGS_PACKET_TYPE;
  packet.version = 1;
  packet.baseR = dashPreset.r;
  packet.baseG = dashPreset.g;
  packet.baseB = dashPreset.b;
  packet.baseEffect = dashPreset.effect;
  packet.baseSpeed = dashPreset.speed;
  packet.baseIntensity = dashPreset.intensity;
  packet.manualBrightness = dashPreset.manualBrightness;
  packet.autoBrightness = dashPreset.autoBrightness;
  packet.powerOff = webPowerOff;
  packet.chargeDashEnabled = features.chargeDashEnabled;
  packet.autopilotDashEnabled = features.autopilotDashEnabled;
  packet.autopilotR = features.autopilotR;
  packet.autopilotG = features.autopilotG;
  packet.autopilotB = features.autopilotB;
  packet.blindSpotDashEnabled = features.blindSpotDashEnabled;
  packet.blindSpotOnlyWithBlinker = features.blindSpotOnlyWithBlinker;
  packet.blindSpotDashPercent = features.blindSpotDashPercent;
  packet.dashLedCount = features.dashLedCount;
  packet.doorOpenHighlightEnabled = features.doorOpenHighlightEnabled;
  packet.welcomeAnimationEnabled = features.welcomeAnimationEnabled;
  packet.goodbyeAnimationEnabled = features.goodbyeAnimationEnabled;

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(packet));
  if (result != ESP_OK) {
    Serial.print("DashSettings Sendefehler: ");
    Serial.println(result);
  }
}

void sendPresetCommand(
  char target,
  const Preset &preset,
  uint8_t power,
  uint8_t mode,
  uint8_t effect,
  uint8_t r1,
  uint8_t g1,
  uint8_t b1,
  uint8_t r2,
  uint8_t g2,
  uint8_t b2,
  uint8_t brightness,
  uint8_t progress,
  int speedOverride = -1,
  int intensityOverride = -1
) {
  LedCommand cmd = {};
  cmd.magic = 0xA7;
  cmd.version = 2;
  cmd.target = target;
  cmd.power = power;
  cmd.mode = mode;
  cmd.effect = effect;
  cmd.r1 = r1;
  cmd.g1 = g1;
  cmd.b1 = b1;
  cmd.r2 = r2;
  cmd.g2 = g2;
  cmd.b2 = b2;
  cmd.r3 = 0;
  cmd.g3 = 0;
  cmd.b3 = 0;
  cmd.brightness = brightness;
  cmd.speed = speedOverride >= 0 ? clampByte(speedOverride) : preset.speed;
  cmd.intensity = intensityOverride >= 0 ? clampByte(intensityOverride) : preset.intensity;
  cmd.progress = progress;
  int rangeIndex = targetToIndex(target);
  if (rangeIndex >= 0 && rangeIndex < 4) {
    cmd.ledStart = doorLedRanges[rangeIndex].start;
    cmd.ledEnd = doorLedRanges[rangeIndex].end;
  } else {
    cmd.ledStart = 0;
    cmd.ledEnd = DOOR_NUM_LEDS - 1;
  }
  cmd.sequence = ledSequence++;

  sendRawLedCommand(cmd);
  rememberPendingLedAck(cmd);
}

void sendOffAll() {
  sendLedCommand(TARGET_ALL, 0, MODE_NORMAL, EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
}

void sendOffDoors() {
  sendLedCommand(TARGET_REAR_LEFT, 0, MODE_NORMAL, EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
  sendLedCommand(TARGET_REAR_RIGHT, 0, MODE_NORMAL, EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
  sendLedCommand(TARGET_FRONT_LEFT, 0, MODE_NORMAL, EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
  sendLedCommand(TARGET_FRONT_RIGHT, 0, MODE_NORMAL, EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0);
}

void sendRecoveryStatic() {
  // Sendet mehrere Zielcodes, damit auch ein falsch gesetzter DEVICE_SIDE
  // oder ein alter gespeicherter Blinkzustand ueberschrieben wird.
  uint8_t recoveryBrightness = limitBrightness(180);
  sendLedCommand(TARGET_ALL, 1, MODE_NORMAL, EFFECT_STATIC, 0, 255, 0, 0, 0, 0, recoveryBrightness, 0);
  sendLedCommand(TARGET_FRONT_LEFT, 1, MODE_NORMAL, EFFECT_STATIC, 0, 255, 0, 0, 0, 0, recoveryBrightness, 0);
  sendLedCommand(TARGET_FRONT_RIGHT, 1, MODE_NORMAL, EFFECT_STATIC, 0, 255, 0, 0, 0, 0, recoveryBrightness, 0);
  sendLedCommand(TARGET_REAR_LEFT, 1, MODE_NORMAL, EFFECT_STATIC, 0, 255, 0, 0, 0, 0, recoveryBrightness, 0);
  sendLedCommand(TARGET_REAR_RIGHT, 1, MODE_NORMAL, EFFECT_STATIC, 0, 255, 0, 0, 0, 0, recoveryBrightness, 0);
  sendLedCommand(TARGET_DASH, 1, MODE_NORMAL, EFFECT_STATIC, 0, 255, 0, 0, 0, 0, recoveryBrightness, 0);
}

void sendBaseToDoors(uint8_t brightness) {
  for (int i = 0; i < 4; i++) {
    Preset &preset = zoneConfig[i];
    sendPresetCommand(indexToTarget(i), preset, 1, MODE_NORMAL, preset.effect, preset.r, preset.g, preset.b, 0, 0, 0, effectiveBrightnessFor(preset, false), 0);
  }
}

bool doorOpenHighlightApplies(char target) {
  if (!features.doorOpenHighlightEnabled) return false;

  switch (target) {
    case TARGET_REAR_LEFT:
      return vehicle.doorRearLeftOpen || vehicle.trunkOpen;
    case TARGET_REAR_RIGHT:
      return vehicle.doorRearRightOpen || vehicle.trunkOpen;
    case TARGET_FRONT_LEFT:
      return vehicle.doorFrontLeftOpen;
    case TARGET_FRONT_RIGHT:
      return vehicle.doorFrontRightOpen;
    default:
      return false;
  }
}

void sendBaseToDoorsExcept(bool skipRearLeft, bool skipRearRight, bool skipFrontLeft, bool skipFrontRight) {
  for (int i = 0; i < 4; i++) {
    char target = indexToTarget(i);
    if (skipRearLeft && target == TARGET_REAR_LEFT) continue;
    if (skipRearRight && target == TARGET_REAR_RIGHT) continue;
    if (skipFrontLeft && target == TARGET_FRONT_LEFT) continue;
    if (skipFrontRight && target == TARGET_FRONT_RIGHT) continue;

    Preset &preset = zoneConfig[i];
    sendPresetCommand(target, preset, 1, MODE_NORMAL, preset.effect, preset.r, preset.g, preset.b, 0, 0, 0, effectiveBrightnessFor(preset, false), 0);
  }
}

void sendBaseToDoorsKeepingHighlights(bool skipBlindLeft, bool skipBlindRight) {
  sendBaseToDoorsExcept(
    doorOpenHighlightApplies(TARGET_REAR_LEFT),
    doorOpenHighlightApplies(TARGET_REAR_RIGHT),
    doorOpenHighlightApplies(TARGET_FRONT_LEFT) || skipBlindLeft,
    doorOpenHighlightApplies(TARGET_FRONT_RIGHT) || skipBlindRight
  );
}

void sendBaseToDash(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &preset = zoneConfig[4];
  sendPresetCommand(TARGET_DASH, preset, 1, MODE_NORMAL, preset.effect, preset.r, preset.g, preset.b, 0, 0, 0, effectiveBrightnessFor(preset, false), 0);
}

void sendDoorOpenHighlight(char target, const Preset &preset) {
  sendPresetCommand(target, preset, 1, MODE_NORMAL, EFFECT_STATIC, 255, 90, 0, 0, 0, 0, effectiveBrightnessFor(preset, false), 0);
}

void sendBlindSpotDoorHighlight(char target, const Preset &preset) {
  sendPresetCommand(target, preset, 1, MODE_NORMAL, EFFECT_STATIC, 255, 120, 0, 0, 0, 0, effectiveBrightnessFor(preset, false), 0);
}

void sendDoorOpenHighlights() {
  if (vehicle.doorRearLeftOpen) {
    sendDoorOpenHighlight(TARGET_REAR_LEFT, zoneConfig[0]);
  }

  if (vehicle.doorRearRightOpen) {
    sendDoorOpenHighlight(TARGET_REAR_RIGHT, zoneConfig[1]);
  }

  if (vehicle.doorFrontLeftOpen) {
    sendDoorOpenHighlight(TARGET_FRONT_LEFT, zoneConfig[2]);
  }

  if (vehicle.doorFrontRightOpen) {
    sendDoorOpenHighlight(TARGET_FRONT_RIGHT, zoneConfig[3]);
  }

  if (vehicle.trunkOpen) {
    sendDoorOpenHighlight(TARGET_REAR_LEFT, zoneConfig[0]);
    sendDoorOpenHighlight(TARGET_REAR_RIGHT, zoneConfig[1]);
  }
}

void sendBlindSpotLeft(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];

  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_BLINDSPOT, EFFECT_DASH_QUARTER, 255, 120, 0, dashPreset.r, dashPreset.g, dashPreset.b, effectiveBrightnessFor(dashPreset, false), 100, -1, features.blindSpotDashPercent);
}

void sendBlindSpotRight(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];

  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_BLINDSPOT, EFFECT_DASH_QUARTER, 255, 120, 0, dashPreset.r, dashPreset.g, dashPreset.b, effectiveBrightnessFor(dashPreset, false), 25, -1, features.blindSpotDashPercent);
}

void sendBlindSpotBoth(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];

  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_BLINDSPOT, EFFECT_DASH_QUARTER, 255, 120, 0, dashPreset.r, dashPreset.g, dashPreset.b, effectiveBrightnessFor(dashPreset, false), 50, -1, features.blindSpotDashPercent);
}

void sendBlinkerDashLeft(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_NORMAL, EFFECT_DASH_QUARTER, 0, 255, 0, dashPreset.r, dashPreset.g, dashPreset.b, effectiveBrightnessFor(dashPreset, false), 100, -1, features.blinkerDashPercent);
}

void sendBlinkerDashRight(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_NORMAL, EFFECT_DASH_QUARTER, 0, 255, 0, dashPreset.r, dashPreset.g, dashPreset.b, effectiveBrightnessFor(dashPreset, false), 25, -1, features.blinkerDashPercent);
}

void sendBlinkerDashBoth(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_NORMAL, EFFECT_DASH_QUARTER, 0, 255, 0, dashPreset.r, dashPreset.g, dashPreset.b, effectiveBrightnessFor(dashPreset, false), 50, -1, features.blinkerDashPercent);
}

void sendAutopilotDash(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_NORMAL, EFFECT_STATIC, features.autopilotR, features.autopilotG, features.autopilotB, 0, 0, 0, effectiveBrightnessFor(dashPreset, false), 0);
}

void sendChargeDash(uint8_t brightness) {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  uint8_t soc = vehicle.batterySocPercent;
  if (soc > 100) soc = 100;

  if (soc >= 75) {
    sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_NORMAL, EFFECT_CHARGE_SOC, 0, 255, 0, 0, 0, 0, effectiveBrightnessFor(dashPreset, false), soc);
  } else {
    sendPresetCommand(TARGET_DASH, dashPreset, 1, MODE_NORMAL, EFFECT_CHARGE_SOC, 255, 0, 0, 0, 0, 0, effectiveBrightnessFor(dashPreset, false), soc);
  }
}

void sendWelcomeDash() {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  sendPresetCommand(
    TARGET_DASH,
    dashPreset,
    1,
    MODE_NORMAL,
    EFFECT_DASH_WELCOME,
    dashPreset.r,
    dashPreset.g,
    dashPreset.b,
    0,
    0,
    0,
    effectiveBrightnessFor(dashPreset, false),
    0
  );
}

void sendGoodbyeDash() {
  if (!MASTER_SENDS_DASH_LED_COMMANDS) return;
  Preset &dashPreset = zoneConfig[4];
  sendPresetCommand(
    TARGET_DASH,
    dashPreset,
    1,
    MODE_NORMAL,
    EFFECT_DASH_GOODBYE,
    dashPreset.r,
    dashPreset.g,
    dashPreset.b,
    0,
    0,
    0,
    effectiveBrightnessFor(dashPreset, false),
    0
  );
}

void refreshLedState(bool immediate) {
  if (recoveryUntilMs > millis()) return;

  uint32_t stateHash = computeStateHash();
  if (!immediate && stateHash == lastStateHash) return;
  if (!immediate && millis() - lastLedRefreshMs < LED_REFRESH_INTERVAL_MS) return;

  lastStateHash = stateHash;
  lastLedRefreshMs = millis();

  bool chargingActive =
    features.chargeDashEnabled &&
    hasVehicle &&
    vehicle.chargingActive &&
    vehicle.lastChargeCanAgeMs < 5000;
  bool leftBlinkerActive = hasVehicle && vehicle.blinkerLeftPulse;
  bool rightBlinkerActive = hasVehicle && vehicle.blinkerRightPulse;

  carAwake = vehicle.vehicleAwake && !vehicle.mirrorsFolded;
  if (hasVehicle && vehicle.trunkOpen) carAwake = true;

  if (!hasVehicle) {
    carAwake = true;
  }

  uint8_t baseBrightness = effectiveBrightness(false);
  uint8_t alertBrightness = baseBrightness;

  if (webPowerOff) {
    sendOffAll();
    return;
  }

  if (chargingActive) {
    goodbyeActive = false;
    goodbyeUntilMs = 0;

    if (!carAwake) {
      sendOffDoors();
    } else {
      sendBaseToDoorsKeepingHighlights(false, false);
      if (features.doorOpenHighlightEnabled) {
        sendDoorOpenHighlights();
      }
    }

    sendChargeDash(effectiveBrightness(false));
    return;
  }

  if (features.welcomeAnimationEnabled && welcomeUntilMs > millis()) {
    if (carAwake) {
      sendBaseToDoorsKeepingHighlights(false, false);
      if (features.doorOpenHighlightEnabled) {
        sendDoorOpenHighlights();
      }
    }
    sendWelcomeDash();
    return;
  }

  if (goodbyeActive) {
    if (goodbyeUntilMs > millis()) {
      sendOffDoors();
      sendGoodbyeDash();
      return;
    }

    goodbyeActive = false;
    goodbyeUntilMs = 0;
    sendOffAll();
    return;
  }

  if (!carAwake) {
    sendOffAll();
    return;
  }

  bool blindLeftAllowed =
    features.blindSpotDashEnabled &&
    hasChassis &&
    chassis.blindLeftActive &&
    (!features.blindSpotOnlyWithBlinker || leftBlinkerActive);

  bool blindRightAllowed =
    features.blindSpotDashEnabled &&
    hasChassis &&
    chassis.blindRightActive &&
    (!features.blindSpotOnlyWithBlinker || rightBlinkerActive);

  sendBaseToDoorsKeepingHighlights(blindLeftAllowed, blindRightAllowed);
  if (features.doorOpenHighlightEnabled) {
    sendDoorOpenHighlights();
  }

  if (blindLeftAllowed) {
    sendBlindSpotDoorHighlight(TARGET_FRONT_LEFT, zoneConfig[2]);
  }

  if (blindRightAllowed) {
    sendBlindSpotDoorHighlight(TARGET_FRONT_RIGHT, zoneConfig[3]);
  }

  // Web Override setzt nur die Grundfarbe. Fahrzeugfunktionen auf dem Dash
  // haben weiterhin Vorrang, sofern sie im Web aktiviert sind.
  if (features.autopilotDashEnabled && hasChassis && chassis.autopilotActive) {
    sendAutopilotDash(alertBrightness);
  } else if (blindLeftAllowed && blindRightAllowed) {
    sendBaseToDash(baseBrightness);
    sendBlindSpotBoth(alertBrightness);
  } else if (blindLeftAllowed) {
    sendBaseToDash(baseBrightness);
    sendBlindSpotLeft(alertBrightness);
  } else if (blindRightAllowed) {
    sendBaseToDash(baseBrightness);
    sendBlindSpotRight(alertBrightness);
  } else if (features.blinkerDashEnabled && leftBlinkerActive && rightBlinkerActive) {
    sendBaseToDash(baseBrightness);
    sendBlinkerDashBoth(alertBrightness);
  } else if (features.blinkerDashEnabled && leftBlinkerActive) {
    sendBaseToDash(baseBrightness);
    sendBlinkerDashLeft(alertBrightness);
  } else if (features.blinkerDashEnabled && rightBlinkerActive) {
    sendBaseToDash(baseBrightness);
    sendBlinkerDashRight(alertBrightness);
  } else {
    sendBaseToDash(baseBrightness);
  }
}

// =======================================================
// ESP-NOW Empfang
// =======================================================

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (len < 1) return;

  uint8_t packetType = incomingData[0];

  if (packetType == LED_ACK_PACKET_TYPE && len == sizeof(LedAckPacket)) {
    LedAckPacket ack;
    memcpy(&ack, incomingData, sizeof(ack));
    handleLedAck(ack);
    return;
  }

  if (packetType == 1 && len == sizeof(VehiclePacket)) {
    memcpy(&vehicle, incomingData, sizeof(vehicle));
    hasVehicle = true;
    lastVehicleMs = millis();
  }

  if (packetType == 2 && len == sizeof(ChassisPacket)) {
    memcpy(&chassis, incomingData, sizeof(chassis));
    hasChassis = true;
    lastChassisMs = millis();
  }
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  lastSendStatus = status;
  if (status == ESP_NOW_SEND_SUCCESS) {
    espNowSendOk++;
  } else {
    espNowSendFail++;
  }
}

// =======================================================
// Web UI
// =======================================================

String htmlPage() {
  current = getSelectedConfig();
  String checked = current.autoBrightness ? "checked" : "";
  bool advancedMode = server.arg("advanced") == "1";
  String advanced = advancedMode ? "block" : "none";
  String hex = colorToHex(current.r, current.g, current.b);
  String autopilotHex = colorToHex(features.autopilotR, features.autopilotG, features.autopilotB);

  String page;
  page.reserve(9000);
  page += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Tesla Ambiente</title><style>";
  page += "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#101214;color:#f4f4f4}";
  page += "main{max-width:760px;margin:auto;padding:22px}section{border-top:1px solid #30343a;padding:18px 0}";
  page += "label{display:block;margin:12px 0 6px;color:#cfd3da}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}";
  page += "input,select,button{font:inherit;border-radius:8px;border:1px solid #3a3f47;background:#191d22;color:#fff;padding:10px}";
  page += "input[type=color]{width:76px;height:48px;padding:4px}.wide{width:100%;box-sizing:border-box}";
  page += "button{background:#2d6cdf;border-color:#2d6cdf;cursor:pointer}.secondary{background:#242930;border-color:#3a3f47}";
  page += ".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}.tile{background:#191d22;padding:12px;border-radius:8px}";
  page += ".muted{color:#9aa3ad;font-size:13px}.ok{color:#65d88d}.warn{color:#ffd166}";
  page += "</style><script>";
  page += "function prep(f){if(f.hex){var c=f.color.value||'#ff0000';f.hex.value=c.replace('#','');}return true;}";
  page += "</script></head><body><main>";
  page += "<h1>Tesla Ambiente</h1>";

  page += "<section><div class='status'>";
  page += "<div class='tile'><div class='muted'>Auto</div><b class='" + String(carAwake ? "ok" : "warn") + "'>" + String(carAwake ? "wach" : "sleep/off") + "</b></div>";
  page += "<div class='tile'><div class='muted'>CAN Helligkeit</div><b>" + String(vehicle.displayBrightnessPercent, 1) + "%</b></div>";
  page += "<div class='tile'><div class='muted'>Laden</div><b>" + String(vehicle.chargingActive ? "aktiv" : "aus") + " | " + String(vehicle.batterySocPercent) + "%</b></div>";
  page += "<div class='tile'><div class='muted'>Autopilot</div><b>" + String(chassis.autopilotActive ? "aktiv" : "aus") + "</b></div>";
  page += "<div class='tile'><div class='muted'>Totwinkel</div><b>L:" + String(chassis.blindLeftActive) + " R:" + String(chassis.blindRightActive) + "</b></div>";
  page += "<div class='tile'><div class='muted'>ESP-NOW</div><b>OK:" + String(espNowSendOk) + " Fail:" + String(espNowSendFail) + "</b></div>";
  page += "<div class='tile'><div class='muted'>Tuer ACK</div><b>OK:" + String(ledAckOk) + " Retry:" + String(ledAckRetry) + " Aufgegeben:" + String(ledAckGiveUp) + "</b></div>";
  page += "<div class='tile'><div class='muted'>Paket</div><b>" + String(sizeof(LedCommand)) + " Byte | CH " + String(ESPNOW_CHANNEL) + "</b></div>";
  page += "<div class='tile'><div class='muted'>Web Override</div><b>" + String(webOverrideActive ? "aktiv" : "auto") + "</b></div>";
  page += "<div class='tile'><div class='muted'>LED Limit</div><b>" + String(MAX_LED_BRIGHTNESS_PERCENT) + "% max</b></div>";
  page += "</div></section>";

  page += "<form action='/features' method='get'>";
  page += "<section><h2>Fahrzeugfunktionen</h2>";
  page += "<label><input type='checkbox' name='charge' value='1' " + String(features.chargeDashEnabled ? "checked" : "") + "> Ladebalken am Dashboard</label>";
  page += "<label><input type='checkbox' name='ap' value='1' " + String(features.autopilotDashEnabled ? "checked" : "") + "> Autopilot am Dashboard</label>";
  page += "<label>Autopilot Farbe</label><div class='row'><input type='color' name='apColor' value='" + autopilotHex + "'></div>";
  page += "<label><input type='checkbox' name='blind' value='1' " + String(features.blindSpotDashEnabled ? "checked" : "") + "> Totwinkel am Dashboard</label>";
  page += "<label><input type='checkbox' name='blindBlink' value='1' " + String(features.blindSpotOnlyWithBlinker ? "checked" : "") + "> Totwinkel nur anzeigen, wenn auf dieser Seite geblinkt wird</label>";
  page += "<label>Totwinkel Anteil je Seite in Prozent</label><input class='wide' type='number' name='blindPct' min='1' max='50' value='" + String(features.blindSpotDashPercent) + "'>";
  page += "<label>Dashboard LED-Anzahl</label><input class='wide' type='number' name='dashCount' min='1' max='" + String(DASH_MAX_LEDS) + "' value='" + String(features.dashLedCount) + "'>";
  page += "<label><input type='checkbox' name='blinkDash' value='1' " + String(features.blinkerDashEnabled ? "checked" : "") + "> Normale Blinkeranzeige am Dashboard</label>";
  page += "<label>Blinker Anteil je Seite in Prozent</label><input class='wide' type='number' name='blinkPct' min='1' max='50' value='" + String(features.blinkerDashPercent) + "'>";
  page += "<label><input type='checkbox' name='door' value='1' " + String(features.doorOpenHighlightEnabled ? "checked" : "") + "> Geoeffnete Tuer orange markieren</label>";
  page += "<label><input type='checkbox' name='welcome' value='1' " + String(features.welcomeAnimationEnabled ? "checked" : "") + "> Begruessungsanimation</label>";
  page += "<label><input type='checkbox' name='goodbye' value='1' " + String(features.goodbyeAnimationEnabled ? "checked" : "") + "> Ausschaltanimation</label>";
  page += "<h3>Aktiver LED-Bereich je Tuer</h3>";
  page += "<p class='muted'>LED-Nummern direkt eingeben. Bei 130 LEDs ist der volle Bereich 0 bis 129.</p>";
  const char *rangeLabels[] = {"Hinten links R", "Hinten rechts L", "Vorne links G", "Vorne rechts F"};
  for (int i = 0; i < 4; i++) {
    page += "<div class='row'><b style='min-width:120px'>" + String(rangeLabels[i]) + "</b>";
    page += "<label>Start</label><input type='number' name='rangeStart" + String(i) + "' min='0' max='" + String(DOOR_NUM_LEDS - 1) + "' value='" + String(doorLedRanges[i].start) + "'>";
    page += "<label>Ende</label><input type='number' name='rangeEnd" + String(i) + "' min='0' max='" + String(DOOR_NUM_LEDS - 1) + "' value='" + String(doorLedRanges[i].end) + "'></div>";
  }
  page += "<button type='submit'>Funktionen speichern</button>";
  page += "<p class='muted'>Ladebalken hat weiterhin Prioritaet vor Autopilot, Totwinkel und Blinker, wenn er aktiviert ist.</p>";
  page += "</section></form>";

  page += "<form action='/set' method='get' onsubmit='return prep(this)'>";
  page += "<section><h2>Basis</h2>";
  page += "<label>Bereich</label><select class='wide' name='target'>";
  char targets[] = {TARGET_ALL, TARGET_REAR_LEFT, TARGET_REAR_RIGHT, TARGET_FRONT_LEFT, TARGET_FRONT_RIGHT, TARGET_DASH};
  for (int i = 0; i < 6; i++) {
    page += "<option value='";
    page += targets[i];
    page += "'";
    if (selectedTarget == targets[i]) page += " selected";
    page += ">";
    page += targetToText(targets[i]);
    page += "</option>";
  }
  page += "</select>";
  page += "<label>Farbe</label><input type='color' name='color' value='" + hex + "'>";
  page += "<label>Effekt</label><select class='wide' name='effect'>";
  for (int i = 1; i <= 20; i++) {
    page += "<option value='" + String(i) + "'";
    if (current.effect == i) page += " selected";
    page += ">Effekt " + String(i) + "</option>";
  }
  page += "</select>";
  page += "<label><input type='checkbox' name='autoBrightness' value='1' " + checked + "> Helligkeit automatisch vom CAN-Displaywert</label>";
  page += "<label>Eigene Helligkeit</label><input class='wide' type='range' name='brightness' min='0' max='255' value='" + String(current.manualBrightness) + "'>";
  page += "<p class='muted'>100% im Web/CAN entspricht real " + String(MAX_LED_BRIGHTNESS_PERCENT) + "% LED-Helligkeit.</p>";
  page += "<div class='row'><button type='submit'>Uebernehmen</button><a href='/?advanced=1&t=" + String(selectedTarget) + "'><button class='secondary' type='button'>Hex / RGB anzeigen</button></a></div>";
  page += "</section>";

  page += "<section style='display:" + advanced + "'><h2>Hex / RGB</h2>";
  if (advancedMode) {
    page += "<input type='hidden' name='adv' value='1'>";
  }
  page += "<label>Hex</label><input class='wide' name='hex' value='" + hex.substring(1) + "'>";
  page += "<div class='row'><div><label>R</label><input type='number' name='r' min='0' max='255' value='" + String(current.r) + "'></div>";
  page += "<div><label>G</label><input type='number' name='g' min='0' max='255' value='" + String(current.g) + "'></div>";
  page += "<div><label>B</label><input type='number' name='b' min='0' max='255' value='" + String(current.b) + "'></div></div>";
  page += "<label>Speed</label><input class='wide' type='range' name='speed' min='1' max='255' value='" + String(current.speed) + "'>";
  page += "<label>Intensity</label><input class='wide' type='range' name='intensity' min='0' max='255' value='" + String(current.intensity) + "'>";
  page += "</section></form>";

  page += "<section><h2>Presets</h2><div class='row'>";
  for (int i = 0; i < 5; i++) {
    page += "<a href='/load?p=" + String(i) + "&t=" + String(selectedTarget) + "'><button class='secondary'>Laden " + String(i + 1) + "</button></a>";
    page += "<a href='/save?p=" + String(i) + "&t=" + String(selectedTarget) + "'><button>Speichern " + String(i + 1) + "</button></a>";
    page += "<a href='/default?p=" + String(i) + "'><button class='secondary'>Start " + String(i + 1) + "</button></a>";
  }
  page += "</div><p class='muted'>Bereich: " + String(targetToText(selectedTarget)) + " | Aktiv: " + String(activePreset + 1) + " | Start: " + String(defaultPreset + 1) + "</p></section>";
  page += "<section><h2>Test</h2><div class='row'>";
  page += "<a href='/test?c=red'><button>Alle rot</button></a>";
  page += "<a href='/test?c=green'><button>Alle gruen</button></a>";
  page += "<a href='/test?c=blue'><button>Alle blau</button></a>";
  page += "<a href='/recover'><button>Recovery gruen</button></a>";
  page += "<a href='/off'><button class='secondary'>Alles aus</button></a>";
  page += "<a href='/on'><button>Startmodus</button></a>";
  page += "<a href='/auto'><button class='secondary'>Auto-Modus</button></a>";
  page += "</div></section>";
  page += "<section><h2>Updates</h2><div class='row'>";
  page += "<a href='/masterota'><button>Master Firmware hochladen</button></a>";
  page += "<a href='/dashupdate'><button>Dashboard Update-Modus</button></a>";
  page += "<a href='/doorupdate'><button>Tueren Update-Modus</button></a>";
  page += "<a href='/canupdate'><button>Dash/CAN C6 Update-Modus</button></a>";
  page += "</div>";
  page += "<p class='muted'>Master: Upload direkt auf diesem Weboverlay. Dashboard: Hotspot Tesla-Dash-OTA. Tueren: Tesla-Door-[Buchstabe]-OTA. Dash/CAN C6: Hotspot Tesla-CAN-C6. Passwort jeweils 12345678.</p>";
  page += "</section>";
  page += "</main></body></html>";
  return page;
}

void handleRoot() {
  if (server.hasArg("t")) {
    selectedTarget = parseTarget(server.arg("t"));
  }
  current = getSelectedConfig();
  server.send(200, "text/html", htmlPage());
}

void handleSet() {
  if (server.hasArg("target")) {
    selectedTarget = parseTarget(server.arg("target"));
  }

  current = getSelectedConfig();
  bool advancedSubmit = server.hasArg("adv");

  if (server.hasArg("color")) {
    String c = urlDecode(server.arg("color"));
    parseHexColor(c, current.r, current.g, current.b);
  }

  if (advancedSubmit && server.hasArg("hex") && server.arg("hex").length() > 0) {
    String h = urlDecode(server.arg("hex"));
    parseHexColor(h, current.r, current.g, current.b);
  }

  if (advancedSubmit && server.hasArg("r")) current.r = clampByte(server.arg("r").toInt());
  if (advancedSubmit && server.hasArg("g")) current.g = clampByte(server.arg("g").toInt());
  if (advancedSubmit && server.hasArg("b")) current.b = clampByte(server.arg("b").toInt());
  if (server.hasArg("effect")) current.effect = clampByte(server.arg("effect").toInt());
  if (server.hasArg("brightness")) current.manualBrightness = clampByte(server.arg("brightness").toInt());
  if (advancedSubmit && server.hasArg("speed")) current.speed = clampByte(server.arg("speed").toInt());
  if (advancedSubmit && server.hasArg("intensity")) current.intensity = clampByte(server.arg("intensity").toInt());
  current.autoBrightness = server.hasArg("autoBrightness");

  webOverrideActive = true;
  webPowerOff = false;
  saveSelectedTargetConfig();
  sendDashSettings();
  printCurrentWebConfig("Web Set");
  refreshLedState(true);
  server.sendHeader("Location", "/?t=" + String(selectedTarget));
  server.send(303);
}

void handleLoad() {
  uint8_t p = clampByte(server.arg("p").toInt());
  if (server.hasArg("t")) selectedTarget = parseTarget(server.arg("t"));

  if (p <= 4) {
    activePreset = p;
    current = presets[p];
    webOverrideActive = true;
    webPowerOff = false;
    saveSelectedTargetConfig();
    sendDashSettings();
    refreshLedState(true);
  }
  server.sendHeader("Location", "/?t=" + String(selectedTarget));
  server.send(303);
}

void handleSave() {
  uint8_t p = clampByte(server.arg("p").toInt());
  if (server.hasArg("t")) selectedTarget = parseTarget(server.arg("t"));

  if (p <= 4) {
    activePreset = p;
    current = getSelectedConfig();
    savePreset(p);
  }
  server.sendHeader("Location", "/?t=" + String(selectedTarget));
  server.send(303);
}

void handleDefault() {
  uint8_t p = clampByte(server.arg("p").toInt());
  if (p <= 4) saveDefaultPreset(p);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOff() {
  webOverrideActive = true;
  webPowerOff = true;
  sendDashSettings();
  sendOffAll();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOn() {
  webOverrideActive = true;
  webPowerOff = false;
  current = presets[defaultPreset];
  activePreset = defaultPreset;
  applyPresetToAllZones(current);
  saveAllZoneConfigs();
  sendDashSettings();
  refreshLedState(true);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleAuto() {
  webOverrideActive = false;
  webPowerOff = false;
  sendDashSettings();
  refreshLedState(true);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleTest() {
  String color = server.arg("c");
  uint8_t r = 255;
  uint8_t g = 0;
  uint8_t b = 0;

  if (color == "green") {
    r = 0;
    g = 255;
    b = 0;
  } else if (color == "blue") {
    r = 0;
    g = 70;
    b = 255;
  }

  webOverrideActive = true;
  webPowerOff = false;
  current.r = r;
  current.g = g;
  current.b = b;
  current.effect = EFFECT_STATIC;
  current.manualBrightness = 160;
  current.autoBrightness = false;
  current.speed = 120;
  current.intensity = 120;
  selectedTarget = TARGET_ALL;
  applyPresetToAllZones(current);
  saveAllZoneConfigs();
  sendDashSettings();
  printCurrentWebConfig("Web Test");
  refreshLedState(true);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRecover() {
  webOverrideActive = true;
  webPowerOff = false;
  current.r = 0;
  current.g = 255;
  current.b = 0;
  current.effect = EFFECT_STATIC;
  current.manualBrightness = 180;
  current.autoBrightness = false;
  current.speed = 120;
  current.intensity = 120;
  selectedTarget = TARGET_ALL;
  applyPresetToAllZones(current);
  saveAllZoneConfigs();
  sendDashSettings();

  recoveryUntilMs = millis() + 20000;
  lastRecoverySendMs = 0;
  sendRecoveryStatic();

  Serial.println("Recovery aktiv: sende 20s statisch gruen an alle Ziele");

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleFeatures() {
  features.chargeDashEnabled = server.hasArg("charge");
  features.autopilotDashEnabled = server.hasArg("ap");
  if (server.hasArg("apColor")) {
    String apColor = urlDecode(server.arg("apColor"));
    parseHexColor(apColor, features.autopilotR, features.autopilotG, features.autopilotB);
  }
  features.blindSpotDashEnabled = server.hasArg("blind");
  features.blindSpotOnlyWithBlinker = server.hasArg("blindBlink");
  features.blinkerDashEnabled = server.hasArg("blinkDash");
  features.doorOpenHighlightEnabled = server.hasArg("door");
  features.welcomeAnimationEnabled = server.hasArg("welcome");
  features.goodbyeAnimationEnabled = server.hasArg("goodbye");

  if (server.hasArg("blindPct")) {
    int pct = server.arg("blindPct").toInt();
    if (pct < 1) pct = 1;
    if (pct > 50) pct = 50;
    features.blindSpotDashPercent = pct;
  }

  if (server.hasArg("dashCount")) {
    int count = server.arg("dashCount").toInt();
    if (count < 1) count = 1;
    if (count > DASH_MAX_LEDS) count = DASH_MAX_LEDS;
    features.dashLedCount = count;
  }

  if (server.hasArg("blinkPct")) {
    int pct = server.arg("blinkPct").toInt();
    if (pct < 1) pct = 1;
    if (pct > 50) pct = 50;
    features.blinkerDashPercent = pct;
  }

  for (int i = 0; i < 4; i++) {
    String startName = "rangeStart" + String(i);
    String endName = "rangeEnd" + String(i);
    if (server.hasArg(startName) && server.hasArg(endName)) {
      int start = server.arg(startName).toInt();
      int end = server.arg(endName).toInt();

      if (start < 0) start = 0;
      if (end < 0) end = 0;
      if (start >= DOOR_NUM_LEDS) start = DOOR_NUM_LEDS - 1;
      if (end >= DOOR_NUM_LEDS) end = DOOR_NUM_LEDS - 1;

      if (start > end) {
        int tmp = start;
        start = end;
        end = tmp;
      }

      doorLedRanges[i].start = start;
      doorLedRanges[i].end = end;
    }
  }

  saveFeatureSettings();
  sendDashSettings();
  lastStateHash = 0;
  refreshLedState(true);

  server.sendHeader("Location", "/");
  server.send(303);
}

String masterOtaPage() {
  String page;
  page += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Master OTA</title></head><body style='font-family:system-ui;margin:24px;background:#101214;color:#f4f4f4'>";
  page += "<h1>Master Firmware Update</h1>";
  page += "<p>Firmware .bin auswaehlen und hochladen. Der Master startet danach automatisch neu.</p>";
  page += "<form method='POST' action='/masterota' enctype='multipart/form-data'>";
  page += "<input type='file' name='update'><button type='submit'>Master flashen</button></form>";
  page += "<p><a href='/'>Zurueck</a></p></body></html>";
  return page;
}

void sendOtaSystemCommand(uint8_t command, uint8_t minutes, const char *label) {
  SystemCommand cmd = {};
  cmd.magic = 0xC6;
  cmd.version = 1;
  cmd.command = command;
  cmd.minutes = minutes;
  cmd.nonce = millis();

  esp_err_t result = esp_now_send(
    broadcastAddress,
    (uint8_t*)&cmd,
    sizeof(cmd)
  );

  Serial.print(label);
  Serial.print(" OTA Befehl gesendet: ");
  Serial.print(minutes);
  Serial.print(" Minuten | result=");
  Serial.println(result);
}

void handleMasterOtaGet() {
  server.send(200, "text/html", masterOtaPage());
}

void handleMasterOtaPost() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "Update failed" : "Update ok, rebooting");
  delay(500);
  ESP.restart();
}

void handleMasterOtaUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.print("Master OTA start: ");
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
      Serial.print("Master OTA success: ");
      Serial.print(upload.totalSize);
      Serial.println(" bytes");
    } else {
      Update.printError(Serial);
    }
  }
}

void handleCanUpdate() {
  sendOtaSystemCommand(1, 10, "Dash/CAN-C6");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDashUpdate() {
  sendOtaSystemCommand(2, 10, "Dashboard");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDoorUpdate() {
  sendOtaSystemCommand(3, 10, "Tueren");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleCaptivePortal() {
  server.send(200, "text/html", htmlPage());
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/generate_204", handleCaptivePortal);
  server.on("/gen_204", handleCaptivePortal);
  server.on("/hotspot-detect.html", handleCaptivePortal);
  server.on("/library/test/success.html", handleCaptivePortal);
  server.on("/ncsi.txt", handleCaptivePortal);
  server.on("/connecttest.txt", handleCaptivePortal);
  server.on("/set", handleSet);
  server.on("/load", handleLoad);
  server.on("/save", handleSave);
  server.on("/default", handleDefault);
  server.on("/off", handleOff);
  server.on("/on", handleOn);
  server.on("/auto", handleAuto);
  server.on("/test", handleTest);
  server.on("/recover", handleRecover);
  server.on("/features", handleFeatures);
  server.on("/masterota", HTTP_GET, handleMasterOtaGet);
  server.on("/masterota", HTTP_POST, handleMasterOtaPost, handleMasterOtaUpload);
  server.on("/dashupdate", handleDashUpdate);
  server.on("/doorupdate", handleDoorUpdate);
  server.on("/canupdate", handleCanUpdate);
  server.onNotFound(handleCaptivePortal);
  server.begin();
}

// =======================================================
// Setup
// =======================================================

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Fehler");
    delay(1000);
    ESP.restart();
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_AP;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("ESP-NOW Broadcast Peer Fehler");
      delay(1000);
      ESP.restart();
    }
  }

  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  Serial.print("LedCommand Groesse: ");
  Serial.print(sizeof(LedCommand));
  Serial.println(" Byte");
}

void setupWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(100);

  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

#if WIFI_AP_MODE
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, ESPNOW_CHANNEL);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.print("Weboverlay AP: ");
  Serial.println(WIFI_AP_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Captive Portal DNS aktiv");
#else
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  Serial.print("Verbinde WLAN");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
#endif
}

void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println();
  Serial.println("======================================");
  Serial.println("Tesla Ambiente Master");
  Serial.println("ESP-NOW + Weboverlay");
  Serial.println("======================================");

  setFactoryPreset(current);
  loadConfig();

  setupWifi();
  setupEspNow();
  setupWeb();

  Serial.println("Master bereit");
  refreshLedState(true);
}

// =======================================================
// Loop
// =======================================================

void loop() {
#if WIFI_AP_MODE
  dnsServer.processNextRequest();
#endif
  server.handleClient();

  uint32_t now = millis();

  if (recoveryUntilMs > now) {
    if (now - lastRecoverySendMs >= 250) {
      lastRecoverySendMs = now;
      sendRecoveryStatic();
    }
    return;
  }

  if (hasVehicle && now - lastVehicleMs > 3000) {
    hasVehicle = false;
    vehicle.vehicleAwake = true;
    vehicle.mirrorsFolded = false;
  }

  if (hasChassis && now - lastChassisMs > 1500) {
    hasChassis = false;
    chassis.autopilotActive = false;
    chassis.blindLeftActive = false;
    chassis.blindRightActive = false;
  }

  carAwake = vehicle.vehicleAwake && !vehicle.mirrorsFolded;
  if (hasVehicle && vehicle.trunkOpen) carAwake = true;
  if (!hasVehicle) carAwake = true;
  bool chargingActive =
    features.chargeDashEnabled &&
    hasVehicle &&
    vehicle.chargingActive &&
    vehicle.lastChargeCanAgeMs < 5000;

  if (carAwake != lastCarAwake) {
    lastCarAwake = carAwake;
    if (carAwake) {
      goodbyeActive = false;
      goodbyeUntilMs = 0;
      lastStateHash = 0;

      if (!webOverrideActive) {
        current = presets[defaultPreset];
        activePreset = defaultPreset;
        applyPresetToAllZones(current);
      }
      if (features.welcomeAnimationEnabled && !welcomePlayedForWake) {
        welcomePlayedForWake = true;
        welcomeUntilMs = millis() + 2200;
        sendWelcomeDash();
      }
      refreshLedState(true);
    } else {
      welcomePlayedForWake = false;
      welcomeUntilMs = 0;
      sendOffDoors();

      if (chargingActive) {
        goodbyeActive = false;
        goodbyeUntilMs = 0;
        sendChargeDash(effectiveBrightness(false));
      } else if (features.goodbyeAnimationEnabled) {
        goodbyeActive = true;
        goodbyeUntilMs = millis() + 2200;
        sendGoodbyeDash();
      } else {
        goodbyeActive = false;
        goodbyeUntilMs = 0;
        sendOffAll();
      }
    }
  }

  refreshLedState(false);
  processLedAckRetries();

  if (now - lastDashSettingsSendMs > 2000) {
    lastDashSettingsSendMs = now;
    sendDashSettings();
  }

  if (now - lastDebugMs > 3000) {
    lastDebugMs = now;
    Serial.print("Master | awake=");
    Serial.print(carAwake);
    Serial.print(" vehicleAwake=");
    Serial.print(vehicle.vehicleAwake);
    Serial.print(" mirrorL=");
    Serial.print(vehicle.mirrorLeftState);
    Serial.print(" mirrorR=");
    Serial.print(vehicle.mirrorRightState);
    Serial.print(" folded=");
    Serial.print(vehicle.mirrorsFolded);
    Serial.print(" unfolded=");
    Serial.print(vehicle.mirrorsUnfolded);
    Serial.print(" autoBright=");
    Serial.print(current.autoBrightness);
    Serial.print(" bright=");
    Serial.print(effectiveBrightness(false));
    Serial.print(" AP=");
    Serial.print(chassis.autopilotActive);
    Serial.print(" blindL=");
    Serial.print(chassis.blindLeftActive);
    Serial.print(" blindR=");
    Serial.print(chassis.blindRightActive);
    Serial.print(" blinkL=");
    Serial.print(vehicle.blinkerLeftPulse);
    Serial.print(" blinkR=");
    Serial.print(vehicle.blinkerRightPulse);
    Serial.print(" charging=");
    Serial.print(vehicle.chargingActive);
    Serial.print(" soc=");
    Serial.print(vehicle.batterySocPercent);
    Serial.print("% doors FL=");
    Serial.print(vehicle.doorFrontLeftOpen);
    Serial.print(" FR=");
    Serial.print(vehicle.doorFrontRightOpen);
    Serial.print(" RL=");
    Serial.print(vehicle.doorRearLeftOpen);
    Serial.print(" RR=");
    Serial.print(vehicle.doorRearRightOpen);
    Serial.print(" trunk=");
    Serial.print(vehicle.trunkOpen);
    Serial.print("% chargeAge=");
    Serial.println(vehicle.lastChargeCanAgeMs);
  }
}
