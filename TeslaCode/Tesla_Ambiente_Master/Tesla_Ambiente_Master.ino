#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>

// =======================================================
// Tesla Ambiente Master — BLE + ESP-NOW + Web-Overlay
// Empfaengt CAN-Daten per ESP-NOW, sendet LED-Kommandos
// an Tueren + Dashboard, BLE-Schnittstelle fuer iOS App.
// =======================================================

#define ESPNOW_CHANNEL 6
#define WIFI_AP_MODE true
#define WIFI_AP_SSID "Tesla-Ambiente"
#define WIFI_AP_PASS "12345678"
#define WIFI_STA_SSID ""
#define WIFI_STA_PASS ""

#define DASH_NUM_LEDS 122
#define DASH_MAX_LEDS 144
#define DOOR_NUM_LEDS 130

#define EFFECT_OFF 0
#define EFFECT_STATIC 1
#define EFFECT_BLINDSPOT 20

#define MODE_NORMAL 0
#define MODE_BLINDSPOT 1

#define TARGET_REAR_RIGHT 'L'
#define TARGET_REAR_LEFT  'R'
#define TARGET_FRONT_RIGHT 'F'
#define TARGET_FRONT_LEFT  'G'
#define TARGET_DASH 'D'
#define TARGET_ALL  'A'

#define LED_ACK_PACKET_TYPE     0xA8
#define DASH_SETTINGS_PACKET_TYPE 0xC7
#define DASH_STATUS_PACKET_TYPE   0xC8

#define DASH_MODE_OFF       0
#define DASH_MODE_BASE      1
#define DASH_MODE_BLINKER   2
#define DASH_MODE_BLIND     3
#define DASH_MODE_AUTOPILOT 4
#define DASH_MODE_CHARGING  5
#define DASH_MODE_WELCOME   6
#define DASH_MODE_GOODBYE   7
#define DASH_MODE_DOOR      8

#define LED_ACK_RETRY_MS 90
#define LED_ACK_MAX_ATTEMPTS 3
#define LED_REFRESH_INTERVAL_MS 500
#define MASTER_SENDS_DASH_LED_COMMANDS false

#define MAX_LED_BRIGHTNESS_PERCENT 15
#define MAX_LED_BRIGHTNESS ((255 * MAX_LED_BRIGHTNESS_PERCENT) / 100)

// =======================================================
// BLE UUIDs — MUSS identisch mit iOS App sein
// =======================================================
#define BLE_SERVICE_UUID         "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define BLE_LED_CMD_UUID         "BEB5483E-36E1-4688-B7F5-EA07361B26A8"
#define BLE_VEHICLE_STATUS_UUID  "BEB5483E-36E1-4688-B7F5-EA07361B26A9"
#define BLE_FEATURE_SETTINGS_UUID "BEB5483E-36E1-4688-B7F5-EA07361B26AA"
#define BLE_OTA_CTRL_UUID        "BEB5483E-36E1-4688-B7F5-EA07361B26AB"
#define BLE_OTA_DATA_UUID        "BEB5483E-36E1-4688-B7F5-EA07361B26AC"
#define BLE_DEVICE_INFO_UUID     "BEB5483E-36E1-4688-B7F5-EA07361B26AD"
#define BLE_PRESETS_UUID         "BEB5483E-36E1-4688-B7F5-EA07361B26AE"

#define BLE_DEVICE_NAME "Tesla-Ambiente"
#define BLE_OTA_CHUNK_SIZE 128

// =======================================================
// OTA-Status-Bytes fuer BLE
// =======================================================
#define BLE_OTA_STATUS_IDLE       0
#define BLE_OTA_STATUS_RECEIVING  1
#define BLE_OTA_STATUS_SUCCESS    2
#define BLE_OTA_STATUS_ERROR      3
#define BLE_OTA_STATUS_VERIFYING  4

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
const byte DNS_PORT = 53;

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
  uint8_t r1; uint8_t g1; uint8_t b1;
  uint8_t r2; uint8_t g2; uint8_t b2;
  uint8_t r3; uint8_t g3; uint8_t b3;
  uint8_t brightness;
  uint8_t speed;
  uint8_t intensity;
  uint8_t progress;
  uint16_t ledStart;
  uint16_t ledEnd;
  uint32_t sequence;
} LedCommand;

typedef struct __attribute__((packed)) {
  uint8_t packetType;
  uint8_t magic;
  uint8_t version;
  char device;
  char target;
  uint32_t sequence;
  uint8_t accepted;
} LedAckPacket;

typedef struct __attribute__((packed)) {
  uint8_t magic;
  uint8_t version;
  uint8_t command;
  uint8_t minutes;
  uint32_t nonce;
} SystemCommand;

typedef struct __attribute__((packed)) {
  uint8_t packetType;
  uint8_t version;
  uint8_t baseR; uint8_t baseG; uint8_t baseB;
  uint8_t baseEffect;
  uint8_t baseSpeed;
  uint8_t baseIntensity;
  uint8_t manualBrightness;
  bool autoBrightness;
  bool powerOff;
  bool chargeDashEnabled;
  bool autopilotDashEnabled;
  uint8_t autopilotR; uint8_t autopilotG; uint8_t autopilotB;
  bool blindSpotDashEnabled;
  bool blindSpotOnlyWithBlinker;
  uint8_t blindSpotDashPercent;
  uint16_t dashLedCount;
  bool doorOpenHighlightEnabled;
  bool welcomeAnimationEnabled;
  bool goodbyeAnimationEnabled;
  bool blinkerDashEnabled;
  uint8_t blinkerDashPercent;
} DashSettingsPacket;

typedef struct __attribute__((packed)) {
  uint8_t packetType;
  uint8_t dashMode;
  uint8_t vehicleBusState;
  uint8_t chassisBusState;
  uint32_t vehicleCanAgeMs;
  uint32_t chassisCanAgeMs;
  uint8_t brightnessPct;
  uint8_t gear;
  uint8_t soc;
  bool vehicleAwake;
  bool chargingActive;
  bool blinkerLeft;
  bool blinkerRight;
  bool blindLeft;
  bool blindRight;
  bool autopilot;
} DashStatusPacket;

typedef struct __attribute__((packed)) {
  uint8_t blinkerLeft;
  uint8_t blinkerRight;
  uint8_t gear;
  uint8_t batterySOC;
  uint8_t chargingActive;
  uint8_t vehicleAwake;
  uint8_t doorFL; uint8_t doorFR;
  uint8_t doorRL; uint8_t doorRR;
  uint8_t trunkOpen;
  uint8_t autopilotActive;
  uint8_t blindLeft; uint8_t blindRight;
  uint8_t displayBrightness;
  uint8_t dashMode;
  uint8_t mirrorsFolded;
  uint32_t vehicleCanAgeMs;
} BLEVehicleStatus;

typedef struct __attribute__((packed)) {
  uint8_t command;
  uint8_t target;
  uint32_t totalSize;
  uint16_t chunkSize;
  uint32_t checksum;
} BLEOTAControl;

struct PendingLedAck {
  bool active;
  char target;
  uint32_t sequence;
  uint8_t attempts;
  uint32_t lastSendMs;
  LedCommand cmd;
};

struct Preset {
  uint8_t r; uint8_t g; uint8_t b;
  uint8_t effect;
  uint8_t speed;
  uint8_t intensity;
  uint8_t manualBrightness;
  bool autoBrightness;
};

struct FeatureSettings {
  bool chargeDashEnabled;
  bool autopilotDashEnabled;
  uint8_t autopilotR; uint8_t autopilotG; uint8_t autopilotB;
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
DashStatusPacket dashStatus = {};
uint32_t lastDashStatusMs = 0;
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

NimBLEServer*         bleServer       = nullptr;
NimBLEService*        bleService      = nullptr;
NimBLECharacteristic* bleCharLEDCmd      = nullptr;
NimBLECharacteristic* bleCharVehicle     = nullptr;
NimBLECharacteristic* bleCharFeatures    = nullptr;
NimBLECharacteristic* bleCharOTACtrl     = nullptr;
NimBLECharacteristic* bleCharOTAData     = nullptr;
NimBLECharacteristic* bleCharDeviceInfo  = nullptr;
NimBLECharacteristic* bleCharPresets     = nullptr;

bool bleClientConnected = false;
uint32_t lastBleVehicleNotifyMs = 0;
uint32_t bleVehicleNotifyIntervalMs = 500;

bool bleOtaActive = false;
uint32_t bleOtaTotalSize = 0;
uint32_t bleOtaReceived = 0;

void refreshLedState(bool force);
void sendDashSettings();
void sendLedCommand(char, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
void notifyBLEVehicleStatus();
String buildDeviceInfoString();

uint8_t clampByte(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

uint8_t percentToBrightness(float pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return clampByte((int)(pct * MAX_LED_BRIGHTNESS / 100.0f));
}

uint8_t limitBrightness(uint8_t b) {
  return map(b, 0, 255, 0, MAX_LED_BRIGHTNESS);
}

uint8_t effectiveBrightness(bool force) {
  if (!force && current.autoBrightness && hasVehicle)
    return percentToBrightness(vehicle.displayBrightnessPercent);
  return limitBrightness(current.manualBrightness);
}

uint8_t effectiveBrightnessFor(const Preset &p, bool force) {
  if (!force && p.autoBrightness && hasVehicle)
    return percentToBrightness(vehicle.displayBrightnessPercent);
  return limitBrightness(p.manualBrightness);
}

String colorToHex(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8]; snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b); return String(buf);
}

void parseHexColor(String hex, uint8_t &r, uint8_t &g, uint8_t &b) {
  hex.trim();
  if (hex.startsWith("#")) hex.remove(0, 1);
  if (hex.length() != 6) return;
  long v = strtol(hex.c_str(), NULL, 16);
  r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
}

String urlDecode(String s) {
  s.replace("+", " "); s.replace("%23", "#"); s.replace("%2F", "/"); s.replace("%3A", ":");
  return s;
}

int targetToIndex(char target) {
  switch (target) {
    case TARGET_REAR_LEFT: return 0; case TARGET_REAR_RIGHT: return 1;
    case TARGET_FRONT_LEFT: return 2; case TARGET_FRONT_RIGHT: return 3;
    case TARGET_DASH: return 4; default: return -1;
  }
}

char indexToTarget(int i) {
  switch (i) {
    case 0: return TARGET_REAR_LEFT; case 1: return TARGET_REAR_RIGHT;
    case 2: return TARGET_FRONT_LEFT; case 3: return TARGET_FRONT_RIGHT;
    case 4: return TARGET_DASH; default: return TARGET_ALL;
  }
}

const char* targetToText(char t) {
  switch (t) {
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
    case TARGET_REAR_LEFT: return 0; case TARGET_REAR_RIGHT: return 1;
    case TARGET_FRONT_LEFT: return 2; case TARGET_FRONT_RIGHT: return 3;
    default: return -1;
  }
}

bool isDoorTarget(char t) { return doorAckIndex(t) >= 0; }

char parseTarget(String val) {
  val.trim();
  if (val.length() == 0) return TARGET_ALL;
  char t = val.charAt(0);
  if (targetToIndex(t) >= 0 || t == TARGET_ALL) return t;
  return TARGET_ALL;
}

Preset getSelectedConfig() {
  int idx = targetToIndex(selectedTarget);
  return (idx >= 0) ? zoneConfig[idx] : current;
}

void applyPresetToAllZones(const Preset &p) {
  for (int i = 0; i < 5; i++) zoneConfig[i] = p;
  current = p;
}

const char* dashModeToText(uint8_t m) {
  switch (m) {
    case DASH_MODE_OFF: return "Aus"; case DASH_MODE_BASE: return "Basis";
    case DASH_MODE_BLINKER: return "Blinker"; case DASH_MODE_BLIND: return "Totwinkel";
    case DASH_MODE_AUTOPILOT: return "Autopilot"; case DASH_MODE_CHARGING: return "Laden";
    case DASH_MODE_WELCOME: return "Willkommen"; case DASH_MODE_GOODBYE: return "Goodbye";
    case DASH_MODE_DOOR: return "Tuer offen"; default: return "?";
  }
}

const char* twaiStateText(uint8_t s) {
  switch (s) {
    case 0: return "Gestoppt"; case 1: return "OK";
    case 2: return "Bus-Off"; case 3: return "Recovery"; default: return "?";
  }
}

uint32_t mixHash(uint32_t hash, uint32_t val) {
  hash ^= val + 0x9E3779B9 + (hash << 6) + (hash >> 2); return hash;
}

uint32_t computeStateHash() {
  uint32_t h = 2166136261UL;
  h = mixHash(h, webOverrideActive); h = mixHash(h, webPowerOff);
  h = mixHash(h, hasVehicle); h = mixHash(h, vehicle.vehicleAwake);
  h = mixHash(h, vehicle.mirrorsFolded); h = mixHash(h, vehicle.chargingActive);
  h = mixHash(h, vehicle.batterySocPercent); h = mixHash(h, chassis.autopilotActive);
  h = mixHash(h, chassis.blindLeftActive); h = mixHash(h, chassis.blindRightActive);
  h = mixHash(h, vehicle.blinkerLeftPulse); h = mixHash(h, vehicle.blinkerRightPulse);
  h = mixHash(h, vehicle.doorFrontLeftOpen); h = mixHash(h, vehicle.doorFrontRightOpen);
  h = mixHash(h, vehicle.doorRearLeftOpen); h = mixHash(h, vehicle.doorRearRightOpen);
  h = mixHash(h, vehicle.trunkOpen);
  h = mixHash(h, (uint32_t)effectiveBrightnessFor(zoneConfig[0], false));
  for (int i = 0; i < 5; i++) {
    h = mixHash(h, zoneConfig[i].r); h = mixHash(h, zoneConfig[i].g);
    h = mixHash(h, zoneConfig[i].b); h = mixHash(h, zoneConfig[i].effect);
    h = mixHash(h, zoneConfig[i].speed); h = mixHash(h, zoneConfig[i].intensity);
    h = mixHash(h, zoneConfig[i].manualBrightness);
  }
  return h;
}

void setFactoryPreset(Preset &p) {
  p.r = 255; p.g = 0; p.b = 0; p.effect = EFFECT_STATIC;
  p.speed = 120; p.intensity = 120; p.manualBrightness = 120; p.autoBrightness = true;
}

void setFactoryFeatures() {
  features.chargeDashEnabled = true; features.autopilotDashEnabled = true;
  features.autopilotR = 0; features.autopilotG = 70; features.autopilotB = 255;
  features.blindSpotDashEnabled = true; features.blindSpotOnlyWithBlinker = true;
  features.blindSpotDashPercent = 25; features.dashLedCount = DASH_NUM_LEDS;
  features.blinkerDashEnabled = false; features.blinkerDashPercent = 25;
  features.doorOpenHighlightEnabled = true; features.welcomeAnimationEnabled = true;
  features.goodbyeAnimationEnabled = true;
}

void setFactoryDoorLedRanges() {
  for (int i = 0; i < 4; i++) { doorLedRanges[i].start = 0; doorLedRanges[i].end = DOOR_NUM_LEDS - 1; }
}

void loadConfig() {
  prefs.begin("ambimaster", true);
  defaultPreset = prefs.getUChar("default", 0);
  if (defaultPreset > 4) defaultPreset = 0;
  for (int i = 0; i < 5; i++) {
    String k = "preset" + String(i);
    if (prefs.getBytesLength(k.c_str()) == sizeof(Preset)) prefs.getBytes(k.c_str(), &presets[i], sizeof(Preset));
    else setFactoryPreset(presets[i]);
  }
  for (int i = 0; i < 5; i++) {
    String k = "zone" + String(i);
    if (prefs.getBytesLength(k.c_str()) == sizeof(Preset)) prefs.getBytes(k.c_str(), &zoneConfig[i], sizeof(Preset));
    else zoneConfig[i] = presets[defaultPreset];
  }
  if (prefs.getBytesLength("features") == sizeof(FeatureSettings)) prefs.getBytes("features", &features, sizeof(FeatureSettings));
  else setFactoryFeatures();
  if (prefs.getBytesLength("doorRanges") == sizeof(doorLedRanges)) prefs.getBytes("doorRanges", &doorLedRanges, sizeof(doorLedRanges));
  else setFactoryDoorLedRanges();
  prefs.end();
  if (features.blindSpotDashPercent < 1 || features.blindSpotDashPercent > 50) features.blindSpotDashPercent = 25;
  if (features.blinkerDashPercent < 1 || features.blinkerDashPercent > 50) features.blinkerDashPercent = 25;
  if (features.dashLedCount < 1 || features.dashLedCount > DASH_MAX_LEDS) features.dashLedCount = DASH_NUM_LEDS;
  for (int i = 0; i < 4; i++) {
    if (doorLedRanges[i].start >= DOOR_NUM_LEDS) doorLedRanges[i].start = 0;
    if (doorLedRanges[i].end >= DOOR_NUM_LEDS) doorLedRanges[i].end = DOOR_NUM_LEDS - 1;
    if (doorLedRanges[i].start > doorLedRanges[i].end) { doorLedRanges[i].start = 0; doorLedRanges[i].end = DOOR_NUM_LEDS - 1; }
  }
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

void savePreset(uint8_t idx) {
  if (idx > 4) return;
  presets[idx] = current;
  prefs.begin("ambimaster", false);
  String k = "preset" + String(idx);
  prefs.putBytes(k.c_str(), &presets[idx], sizeof(Preset));
  prefs.end();
}

void saveDefaultPreset(uint8_t idx) {
  if (idx > 4) return;
  defaultPreset = idx;
  prefs.begin("ambimaster", false);
  prefs.putUChar("default", defaultPreset);
  prefs.end();
}

void saveZoneConfig(uint8_t idx) {
  if (idx > 4) return;
  prefs.begin("ambimaster", false);
  String k = "zone" + String(idx);
  prefs.putBytes(k.c_str(), &zoneConfig[idx], sizeof(Preset));
  prefs.end();
}

void saveAllZoneConfigs() {
  prefs.begin("ambimaster", false);
  for (int i = 0; i < 5; i++) {
    String k = "zone" + String(i);
    prefs.putBytes(k.c_str(), &zoneConfig[i], sizeof(Preset));
  }
  prefs.end();
}

void saveSelectedTargetConfig() {
  int idx = targetToIndex(selectedTarget);
  if (idx >= 0) { zoneConfig[idx] = current; saveZoneConfig(idx); }
  else { for (int i = 0; i < 5; i++) zoneConfig[i] = current; saveAllZoneConfigs(); }
}

void rememberPendingLedAck(const LedCommand &cmd) {
  if (cmd.target == TARGET_ALL) {
    char dt[] = {TARGET_REAR_LEFT, TARGET_REAR_RIGHT, TARGET_FRONT_LEFT, TARGET_FRONT_RIGHT};
    for (int i = 0; i < 4; i++) {
      LedCommand dc = cmd; dc.target = dt[i];
      pendingLedAcks[i] = {true, dt[i], dc.sequence, 1, millis(), dc};
    }
    return;
  }
  int idx = doorAckIndex(cmd.target);
  if (idx < 0) return;
  pendingLedAcks[idx] = {true, cmd.target, cmd.sequence, 1, millis(), cmd};
}

void sendRawLedCommand(const LedCommand &cmd) {
  esp_err_t r = esp_now_send(broadcastAddress, (uint8_t*)&cmd, sizeof(cmd));
  if (r == ESP_OK) espNowSendOk++; else espNowSendFail++;
}

void sendLedCommand(char target, uint8_t power, uint8_t mode, uint8_t effect,
                    uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2,
                    uint8_t brightness, uint8_t progress) {
  LedCommand cmd = {};
  cmd.magic = 0xA7; cmd.version = 2; cmd.target = target;
  cmd.power = power; cmd.mode = mode; cmd.effect = effect;
  cmd.r1 = r1; cmd.g1 = g1; cmd.b1 = b1;
  cmd.r2 = r2; cmd.g2 = g2; cmd.b2 = b2;
  cmd.r3 = 0; cmd.g3 = 0; cmd.b3 = 0;
  cmd.brightness = brightness; cmd.speed = current.speed; cmd.intensity = current.intensity;
  cmd.progress = progress;
  int ri = targetToIndex(target);
  if (ri >= 0 && ri < 4) { cmd.ledStart = doorLedRanges[ri].start; cmd.ledEnd = doorLedRanges[ri].end; }
  else { cmd.ledStart = 0; cmd.ledEnd = DOOR_NUM_LEDS - 1; }
  cmd.sequence = ledSequence++;
  sendRawLedCommand(cmd);
  rememberPendingLedAck(cmd);
}

void sendLedCommandFromBLE(const LedCommand &inCmd) {
  LedCommand cmd = inCmd;
  cmd.sequence = ledSequence++;
  sendRawLedCommand(cmd);
  rememberPendingLedAck(cmd);
}

void processLedAckRetries() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    PendingLedAck &p = pendingLedAcks[i];
    if (!p.active || now - p.lastSendMs < LED_ACK_RETRY_MS) continue;
    if (p.attempts >= LED_ACK_MAX_ATTEMPTS) { p.active = false; ledAckGiveUp++; continue; }
    p.attempts++; p.lastSendMs = now; ledAckRetry++;
    sendRawLedCommand(p.cmd);
  }
}

void handleLedAck(const LedAckPacket &ack) {
  if (ack.magic != 0xA7 || ack.version != 1 || ack.accepted == 0) return;
  int idx = doorAckIndex(ack.device);
  if (idx < 0) return;
  PendingLedAck &p = pendingLedAcks[idx];
  if (!p.active || p.target != ack.device || p.sequence != ack.sequence) return;
  p.active = false; ledAckOk++;
}

void sendDashSettings() {
  Preset &dp = zoneConfig[4];
  DashSettingsPacket pkt = {};
  pkt.packetType = DASH_SETTINGS_PACKET_TYPE; pkt.version = 1;
  pkt.baseR = dp.r; pkt.baseG = dp.g; pkt.baseB = dp.b;
  pkt.baseEffect = dp.effect; pkt.baseSpeed = dp.speed; pkt.baseIntensity = dp.intensity;
  pkt.manualBrightness = dp.manualBrightness; pkt.autoBrightness = dp.autoBrightness;
  pkt.powerOff = webPowerOff;
  pkt.chargeDashEnabled = features.chargeDashEnabled;
  pkt.autopilotDashEnabled = features.autopilotDashEnabled;
  pkt.autopilotR = features.autopilotR; pkt.autopilotG = features.autopilotG; pkt.autopilotB = features.autopilotB;
  pkt.blindSpotDashEnabled = features.blindSpotDashEnabled;
  pkt.blindSpotOnlyWithBlinker = features.blindSpotOnlyWithBlinker;
  pkt.blindSpotDashPercent = features.blindSpotDashPercent;
  pkt.dashLedCount = features.dashLedCount;
  pkt.doorOpenHighlightEnabled = features.doorOpenHighlightEnabled;
  pkt.welcomeAnimationEnabled = features.welcomeAnimationEnabled;
  pkt.goodbyeAnimationEnabled = features.goodbyeAnimationEnabled;
  pkt.blinkerDashEnabled = features.blinkerDashEnabled;
  pkt.blinkerDashPercent = features.blinkerDashPercent;
  esp_now_send(broadcastAddress, (uint8_t*)&pkt, sizeof(pkt));
}

void sendOffAll() {
  sendLedCommand(TARGET_ALL, 0, MODE_NORMAL, EFFECT_OFF, 0,0,0, 0,0,0, 0, 0);
}

void sendOffDoors() {
  char dt[] = {TARGET_REAR_LEFT, TARGET_REAR_RIGHT, TARGET_FRONT_LEFT, TARGET_FRONT_RIGHT};
  for (int i = 0; i < 4; i++) sendLedCommand(dt[i], 0, MODE_NORMAL, EFFECT_OFF, 0,0,0, 0,0,0, 0,0);
}

void sendBaseToDoors(uint8_t brightness) {
  for (int i = 0; i < 4; i++) {
    Preset &p = zoneConfig[i];
    LedCommand cmd = {};
    cmd.magic = 0xA7; cmd.version = 2; cmd.target = indexToTarget(i);
    cmd.power = 1; cmd.mode = MODE_NORMAL; cmd.effect = p.effect;
    cmd.r1 = p.r; cmd.g1 = p.g; cmd.b1 = p.b;
    cmd.brightness = effectiveBrightnessFor(p, false);
    cmd.speed = p.speed; cmd.intensity = p.intensity;
    cmd.ledStart = doorLedRanges[i].start; cmd.ledEnd = doorLedRanges[i].end;
    cmd.sequence = ledSequence++;
    sendRawLedCommand(cmd);
    rememberPendingLedAck(cmd);
  }
}

void refreshLedState(bool force) {
  uint32_t hash = computeStateHash();
  if (!force && hash == lastStateHash) return;
  lastStateHash = hash;
  if (webPowerOff) { sendOffAll(); return; }
  bool vehicleOk = hasVehicle && (millis() - lastVehicleMs < 5000);
  bool awake = vehicleOk && vehicle.vehicleAwake && !vehicle.mirrorsFolded;
  if (webOverrideActive || !vehicleOk) {
    if (!webPowerOff) sendBaseToDoors(effectiveBrightness(false));
    return;
  }
  if (!awake) {
    if (carAwake) sendOffAll();
    return;
  }
  sendBaseToDoors(effectiveBrightness(false));
}

class BLEServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) override {
    bleClientConnected = true;
    Serial.println("BLE: Client verbunden");
    notifyBLEVehicleStatus();
  }
  void onDisconnect(NimBLEServer* pServer) override {
    bleClientConnected = false;
    Serial.println("BLE: Client getrennt, starte Advertising...");
    NimBLEDevice::startAdvertising();
  }
};

class BLELEDCmdCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.size() != sizeof(LedCommand)) return;
    LedCommand cmd;
    memcpy(&cmd, val.data(), sizeof(LedCommand));
    if (cmd.magic != 0xA7 || cmd.version != 2) return;
    sendLedCommandFromBLE(cmd);
    int idx = targetToIndex(cmd.target);
    if (idx >= 0) {
      zoneConfig[idx].r = cmd.r1; zoneConfig[idx].g = cmd.g1; zoneConfig[idx].b = cmd.b1;
      zoneConfig[idx].effect = cmd.effect;
      zoneConfig[idx].brightness = cmd.brightness;
      zoneConfig[idx].speed = cmd.speed; zoneConfig[idx].intensity = cmd.intensity;
      saveZoneConfig(idx);
    } else if (cmd.target == TARGET_ALL) {
      for (int i = 0; i < 5; i++) {
        zoneConfig[i].r = cmd.r1; zoneConfig[i].g = cmd.g1; zoneConfig[i].b = cmd.b1;
        zoneConfig[i].effect = cmd.effect;
        zoneConfig[i].speed = cmd.speed; zoneConfig[i].intensity = cmd.intensity;
      }
      saveAllZoneConfigs();
    }
    webOverrideActive = true;
    webPowerOff = (cmd.power == 0);
    lastStateHash = 0;
  }
};

class BLEFeatureSettingsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.size() != sizeof(DashSettingsPacket)) return;
    DashSettingsPacket pkt;
    memcpy(&pkt, val.data(), sizeof(DashSettingsPacket));
    if (pkt.packetType != 0xC7 || pkt.version != 1) return;
    zoneConfig[4].r = pkt.baseR; zoneConfig[4].g = pkt.baseG; zoneConfig[4].b = pkt.baseB;
    zoneConfig[4].effect = pkt.baseEffect; zoneConfig[4].speed = pkt.baseSpeed;
    zoneConfig[4].intensity = pkt.baseIntensity;
    zoneConfig[4].manualBrightness = pkt.manualBrightness;
    zoneConfig[4].autoBrightness = pkt.autoBrightness;
    features.chargeDashEnabled = pkt.chargeDashEnabled;
    features.autopilotDashEnabled = pkt.autopilotDashEnabled;
    features.autopilotR = pkt.autopilotR; features.autopilotG = pkt.autopilotG; features.autopilotB = pkt.autopilotB;
    features.blindSpotDashEnabled = pkt.blindSpotDashEnabled;
    features.blindSpotOnlyWithBlinker = pkt.blindSpotOnlyWithBlinker;
    features.blindSpotDashPercent = pkt.blindSpotDashPercent;
    features.dashLedCount = pkt.dashLedCount;
    features.doorOpenHighlightEnabled = pkt.doorOpenHighlightEnabled;
    features.welcomeAnimationEnabled = pkt.welcomeAnimationEnabled;
    features.goodbyeAnimationEnabled = pkt.goodbyeAnimationEnabled;
    features.blinkerDashEnabled = pkt.blinkerDashEnabled;
    features.blinkerDashPercent = pkt.blinkerDashPercent;
    webPowerOff = pkt.powerOff;
    saveFeatureSettings();
    saveZoneConfig(4);
    sendDashSettings();
    lastStateHash = 0;
    pChar->setValue((uint8_t*)val.data(), val.size());
  }
  void onRead(NimBLECharacteristic* pChar) override {
    DashSettingsPacket pkt = {};
    Preset &dp = zoneConfig[4];
    pkt.packetType = 0xC7; pkt.version = 1;
    pkt.baseR = dp.r; pkt.baseG = dp.g; pkt.baseB = dp.b;
    pkt.baseEffect = dp.effect; pkt.baseSpeed = dp.speed; pkt.baseIntensity = dp.intensity;
    pkt.manualBrightness = dp.manualBrightness; pkt.autoBrightness = dp.autoBrightness;
    pkt.powerOff = webPowerOff;
    pkt.chargeDashEnabled = features.chargeDashEnabled;
    pkt.autopilotDashEnabled = features.autopilotDashEnabled;
    pkt.autopilotR = features.autopilotR; pkt.autopilotG = features.autopilotG; pkt.autopilotB = features.autopilotB;
    pkt.blindSpotDashEnabled = features.blindSpotDashEnabled;
    pkt.blindSpotOnlyWithBlinker = features.blindSpotOnlyWithBlinker;
    pkt.blindSpotDashPercent = features.blindSpotDashPercent;
    pkt.dashLedCount = features.dashLedCount;
    pkt.doorOpenHighlightEnabled = features.doorOpenHighlightEnabled;
    pkt.welcomeAnimationEnabled = features.welcomeAnimationEnabled;
    pkt.goodbyeAnimationEnabled = features.goodbyeAnimationEnabled;
    pkt.blinkerDashEnabled = features.blinkerDashEnabled;
    pkt.blinkerDashPercent = features.blinkerDashPercent;
    pChar->setValue((uint8_t*)&pkt, sizeof(pkt));
  }
};

class BLEOTACtrlCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.size() < sizeof(BLEOTAControl)) return;
    BLEOTAControl ctrl;
    memcpy(&ctrl, val.data(), sizeof(BLEOTAControl));
    if (ctrl.command == 1) {
      bleOtaTotalSize = ctrl.totalSize;
      bleOtaReceived = 0;
      if (!Update.begin(ctrl.totalSize)) { sendBLEOTAStatus(BLE_OTA_STATUS_ERROR, 0, 1); return; }
      bleOtaActive = true;
      sendBLEOTAStatus(BLE_OTA_STATUS_RECEIVING, 0, 0);
    } else if (ctrl.command == 2) {
      if (Update.end(true)) {
        sendBLEOTAStatus(BLE_OTA_STATUS_SUCCESS, 100, 0);
        bleOtaActive = false;
        delay(500); ESP.restart();
      } else {
        sendBLEOTAStatus(BLE_OTA_STATUS_ERROR, 0, 2);
        bleOtaActive = false;
      }
    } else if (ctrl.command == 3) {
      if (bleOtaActive) { Update.abort(); bleOtaActive = false; }
      sendBLEOTAStatus(BLE_OTA_STATUS_IDLE, 0, 0);
    } else if (ctrl.command == 10) {
      sendBLEOTAStatus(BLE_OTA_STATUS_IDLE, 0, 0);
    }
  }
  void sendBLEOTAStatus(uint8_t status, uint8_t progress, uint8_t errorCode) {
    if (!bleCharOTACtrl) return;
    uint8_t buf[3] = {status, progress, errorCode};
    bleCharOTACtrl->setValue(buf, 3);
    if (bleClientConnected) bleCharOTACtrl->notify();
  }
};

class BLEOTADataCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    if (!bleOtaActive) return;
    std::string val = pChar->getValue();
    if (val.empty()) return;
    size_t written = Update.write((uint8_t*)val.data(), val.size());
    if (written != val.size()) {
      Update.abort(); bleOtaActive = false;
      if (bleCharOTACtrl && bleClientConnected) {
        uint8_t buf[3] = {BLE_OTA_STATUS_ERROR, 0, 3};
        bleCharOTACtrl->setValue(buf, 3); bleCharOTACtrl->notify();
      }
      return;
    }
    bleOtaReceived += written;
    uint8_t progress = (bleOtaTotalSize > 0) ? (uint8_t)((bleOtaReceived * 100UL) / bleOtaTotalSize) : 0;
    if (bleCharOTACtrl && bleClientConnected) {
      uint8_t buf[3] = {BLE_OTA_STATUS_RECEIVING, progress, 0};
      bleCharOTACtrl->setValue(buf, 3); bleCharOTACtrl->notify();
    }
  }
};

class BLEDeviceInfoCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* pChar) override {
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    uint32_t uptime = millis() / 1000;
    uint32_t freeHeap = ESP.getFreeHeap();
    memcpy(buf + 0,  &uptime,       4);
    memcpy(buf + 4,  &freeHeap,     4);
    memcpy(buf + 8,  &espNowSendOk, 4);
    memcpy(buf + 12, &espNowSendFail, 4);
    const char* ver = "1.0.0-BLE";
    strncpy((char*)buf + 16, ver, 16);
    pChar->setValue(buf, 32);
  }
};

String buildDeviceInfoString() {
  return "TeslaAmbiente Master v1.0.0-BLE | Uptime:" + String(millis()/1000) + "s | FreeHeap:" + String(ESP.getFreeHeap());
}

void notifyBLEVehicleStatus() {
  if (!bleCharVehicle || !bleClientConnected) return;
  BLEVehicleStatus status = {};
  if (hasVehicle) {
    status.blinkerLeft  = vehicle.blinkerLeftPulse;
    status.blinkerRight = vehicle.blinkerRightPulse;
    status.gear         = vehicle.gear;
    status.batterySOC   = vehicle.batterySocPercent;
    status.chargingActive = vehicle.chargingActive;
    status.vehicleAwake = vehicle.vehicleAwake;
    status.doorFL       = vehicle.doorFrontLeftOpen;
    status.doorFR       = vehicle.doorFrontRightOpen;
    status.doorRL       = vehicle.doorRearLeftOpen;
    status.doorRR       = vehicle.doorRearRightOpen;
    status.trunkOpen    = vehicle.trunkOpen;
    status.displayBrightness = (uint8_t)vehicle.displayBrightnessPercent;
    status.mirrorsFolded = vehicle.mirrorsFolded;
    status.vehicleCanAgeMs = hasVehicle ? (millis() - lastVehicleMs) : 0xFFFFFFFF;
  }
  if (hasChassis) {
    status.autopilotActive = chassis.autopilotActive;
    status.blindLeft    = chassis.blindLeftActive;
    status.blindRight   = chassis.blindRightActive;
  }
  bool hasDash = (lastDashStatusMs > 0 && millis() - lastDashStatusMs < 5000);
  status.dashMode = hasDash ? dashStatus.dashMode : DASH_MODE_OFF;
  bleCharVehicle->setValue((uint8_t*)&status, sizeof(status));
  bleCharVehicle->notify();
}

void setupBLE() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(512);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BLEServerCallbacks());
  bleService = bleServer->createService(BLE_SERVICE_UUID);
  bleCharLEDCmd = bleService->createCharacteristic(BLE_LED_CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  bleCharLEDCmd->setCallbacks(new BLELEDCmdCallback());
  bleCharVehicle = bleService->createCharacteristic(BLE_VEHICLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  bleCharFeatures = bleService->createCharacteristic(BLE_FEATURE_SETTINGS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  bleCharFeatures->setCallbacks(new BLEFeatureSettingsCallback());
  bleCharOTACtrl = bleService->createCharacteristic(BLE_OTA_CTRL_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  bleCharOTACtrl->setCallbacks(new BLEOTACtrlCallback());
  bleCharOTAData = bleService->createCharacteristic(BLE_OTA_DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  bleCharOTAData->setCallbacks(new BLEOTADataCallback());
  bleCharDeviceInfo = bleService->createCharacteristic(BLE_DEVICE_INFO_UUID, NIMBLE_PROPERTY::READ);
  bleCharDeviceInfo->setCallbacks(new BLEDeviceInfoCallback());
  bleService->start();
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinInterval(160);
  pAdv->setMaxInterval(320);
  pAdv->start();
  Serial.println("BLE gestartet: " BLE_DEVICE_NAME);
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (len < 1) return;
  uint8_t packetType = incomingData[0];
  if (packetType == LED_ACK_PACKET_TYPE) {
    if (len != sizeof(LedAckPacket)) return;
    LedAckPacket ack; memcpy(&ack, incomingData, sizeof(ack)); handleLedAck(ack); return;
  }
  if (packetType == 1 && len == sizeof(VehiclePacket)) {
    memcpy(&vehicle, incomingData, sizeof(vehicle)); lastVehicleMs = millis(); hasVehicle = true; return;
  }
  if (packetType == 2 && len == sizeof(ChassisPacket)) {
    memcpy(&chassis, incomingData, sizeof(chassis)); lastChassisMs = millis(); hasChassis = true; return;
  }
  if (packetType == DASH_STATUS_PACKET_TYPE && len == sizeof(DashStatusPacket)) {
    memcpy(&dashStatus, incomingData, sizeof(dashStatus)); lastDashStatusMs = millis(); return;
  }
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) { lastSendStatus = status; }

void setupEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW Init fehlgeschlagen"); delay(1000); ESP.restart(); }
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL; peerInfo.encrypt = false; peerInfo.ifidx = WIFI_IF_AP;
  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) { Serial.println("ESP-NOW Peer Fehler"); delay(1000); ESP.restart(); }
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  Serial.println("ESP-NOW bereit");
}

void setupWifi() {
  WiFi.persistent(false);
  if (WIFI_AP_MODE) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, ESPNOW_CHANNEL);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    Serial.print("AP: "); Serial.println(WiFi.softAPIP());
  } else {
    WiFi.mode(WIFI_STA);
    if (strlen(WIFI_STA_SSID) > 0) {
      WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
      int tries = 0;
      while (WiFi.status() != WL_CONNECTED && tries++ < 20) { delay(500); Serial.print("."); }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) Serial.println(WiFi.localIP());
    }
  }
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void handleRoot() {
  if (server.hasArg("t")) selectedTarget = parseTarget(server.arg("t"));
  current = getSelectedConfig();
  String page = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Tesla Ambiente</title><style>body{font-family:sans-serif;background:#0a0a0f;color:#f4f4f4;margin:0;padding:16px}";
  page += "h1{color:#e02020}a{color:#e02020}button{background:#e02020;color:white;border:none;padding:10px 16px;border-radius:8px;cursor:pointer;margin:4px}";
  page += ".card{background:#1a1a2e;padding:16px;border-radius:12px;margin-bottom:16px}";
  page += "label{display:block;margin-top:8px;color:#aaa}input[type=range],select{width:100%;margin-top:4px}</style></head><body>";
  page += "<h1>Tesla Ambiente</h1>";
  page += "<div class='card'><b>BLE:</b> " + String(bleClientConnected ? "iOS verbunden" : "wartet...") + " | ";
  page += "<b>Fahrzeug:</b> " + String(carAwake ? "wach" : "sleep") + " | ";
  page += "<b>SOC:</b> " + String(vehicle.batterySocPercent) + "% | ";
  page += "<b>Dashboard:</b> " + String(dashModeToText(dashStatus.dashMode)) + "</div>";
  page += "<div class='card'><h3>Schnell</h3><div>";
  page += "<a href='/off'><button>Alle aus</button></a>";
  page += "<a href='/on'><button>Startmodus</button></a>";
  page += "<a href='/auto'><button>Auto</button></a>";
  page += "<a href='/masterota'><button>Master OTA</button></a></div></div>";
  page += "<div class='card'><form action='/set' method='get'>";
  page += "<label>Zone</label><select name='target'>";
  char targets[] = {TARGET_ALL, TARGET_REAR_LEFT, TARGET_REAR_RIGHT, TARGET_FRONT_LEFT, TARGET_FRONT_RIGHT, TARGET_DASH};
  for (int i = 0; i < 6; i++) {
    page += "<option value='" + String((char)targets[i]) + "'" + (selectedTarget == targets[i] ? " selected" : "") + ">" + targetToText(targets[i]) + "</option>";
  }
  page += "</select>";
  page += "<label>Farbe</label><input type='color' name='color' value='" + colorToHex(current.r, current.g, current.b) + "'>";
  page += "<label>Effekt</label><select name='effect'>";
  for (int i = 1; i <= 20; i++) {
    page += "<option value='" + String(i) + "'" + (current.effect == i ? " selected" : "") + ">Effekt " + String(i) + "</option>";
  }
  page += "</select>";
  page += "<label>Helligkeit <input type='range' name='brightness' min='0' max='255' value='" + String(current.manualBrightness) + "'></label>";
  page += "<label><input type='checkbox' name='autoBrightness' value='1'" + String(current.autoBrightness ? " checked" : "") + "> Auto-Helligkeit</label>";
  page += "<br><button type='submit'>Uebernehmen</button></form></div>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleSet() {
  if (server.hasArg("target")) selectedTarget = parseTarget(server.arg("target"));
  current = getSelectedConfig();
  if (server.hasArg("color")) parseHexColor(urlDecode(server.arg("color")), current.r, current.g, current.b);
  if (server.hasArg("effect")) current.effect = clampByte(server.arg("effect").toInt());
  if (server.hasArg("brightness")) current.manualBrightness = clampByte(server.arg("brightness").toInt());
  current.autoBrightness = server.hasArg("autoBrightness");
  webOverrideActive = true; webPowerOff = false;
  saveSelectedTargetConfig(); sendDashSettings(); refreshLedState(true);
  server.sendHeader("Location", "/?t=" + String(selectedTarget)); server.send(303);
}

void handleOff() {
  webOverrideActive = true; webPowerOff = true; sendDashSettings(); sendOffAll();
  server.sendHeader("Location", "/"); server.send(303);
}

void handleOn() {
  webOverrideActive = true; webPowerOff = false;
  current = presets[defaultPreset]; activePreset = defaultPreset;
  applyPresetToAllZones(current); saveAllZoneConfigs(); sendDashSettings(); refreshLedState(true);
  server.sendHeader("Location", "/"); server.send(303);
}

void handleAuto() {
  webOverrideActive = false; webPowerOff = false; sendDashSettings(); refreshLedState(true);
  server.sendHeader("Location", "/"); server.send(303);
}

void handleStatusApi() {
  bool hasDash = (lastDashStatusMs > 0 && millis() - lastDashStatusMs < 5000);
  String json = "{\"ok\":" + String(hasDash ? "true" : "false");
  json += ",\"ble\":" + String(bleClientConnected ? "true" : "false");
  json += ",\"carAwake\":" + String(carAwake ? "true" : "false");
  json += ",\"soc\":" + String(vehicle.batterySocPercent);
  json += ",\"charging\":" + String(vehicle.chargingActive ? "true" : "false");
  if (hasDash) json += ",\"dashMode\":" + String(dashStatus.dashMode) + ",\"dashModeLabel\":\"" + dashModeToText(dashStatus.dashMode) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleMasterOta() {
  String page = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Master OTA</title></head><body style='font-family:system-ui;margin:24px;background:#0a0a0f;color:#f4f4f4'>";
  page += "<h1>Master Firmware Update</h1>";
  page += "<form method='POST' action='/masterupdate' enctype='multipart/form-data'>";
  page += "<input type='file' name='update' accept='.bin'><br><br><button type='submit'>Flashen</button></form>";
  page += "<p><a href='/'>Zurueck</a></p></body></html>";
  server.send(200, "text/html", page);
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/off", HTTP_GET, handleOff);
  server.on("/on", HTTP_GET, handleOn);
  server.on("/auto", HTTP_GET, handleAuto);
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/masterota", HTTP_GET, handleMasterOta);
  server.on("/masterupdate", HTTP_POST,
    []() { server.sendHeader("Connection", "close"); server.send(200, "text/plain", Update.hasError() ? "Fehler" : "OK, neustart..."); delay(500); ESP.restart(); },
    []() {
      HTTPUpload &upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); }
      else if (upload.status == UPLOAD_FILE_END) { Update.end(true); }
    }
  );
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();
  Serial.println("Webserver gestartet");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("======================================");
  Serial.println("Tesla Ambiente Master v1.0.0");
  Serial.println("ESP-NOW + Web-Overlay + BLE iOS App");
  Serial.println("======================================");
  memset(&vehicle, 0, sizeof(vehicle));
  memset(&chassis, 0, sizeof(chassis));
  memset(&dashStatus, 0, sizeof(dashStatus));
  loadConfig();
  setupWifi();
  setupEspNow();
  setupBLE();
  setupWebServer();
  Serial.println("Master bereit");
  Serial.print("WiFi AP IP: "); Serial.println(WiFi.softAPIP());
  Serial.println("BLE Advertising: " BLE_DEVICE_NAME);
}

void loop() {
  server.handleClient();
  dnsServer.processNextRequest();
  uint32_t now = millis();
  if (bleClientConnected && now - lastBleVehicleNotifyMs >= bleVehicleNotifyIntervalMs) {
    lastBleVehicleNotifyMs = now;
    notifyBLEVehicleStatus();
  }
  processLedAckRetries();
  if (hasVehicle && now - lastVehicleMs > 10000) { hasVehicle = false; memset(&vehicle, 0, sizeof(vehicle)); }
  if (hasChassis && now - lastChassisMs > 10000) { hasChassis = false; memset(&chassis, 0, sizeof(chassis)); }
  bool awake = hasVehicle && vehicle.vehicleAwake && !vehicle.mirrorsFolded;
  carAwake = awake || webOverrideActive;
  if (carAwake != lastCarAwake) {
    lastCarAwake = carAwake;
    lastStateHash = 0;
    if (carAwake) {
      if (!welcomePlayedForWake) { welcomePlayedForWake = true; welcomeUntilMs = now + 2000; }
    } else {
      welcomePlayedForWake = false; welcomeUntilMs = 0;
      goodbyeActive = features.goodbyeAnimationEnabled;
      goodbyeUntilMs = goodbyeActive ? now + 2000 : 0;
    }
  }
  if (now - lastLedRefreshMs >= LED_REFRESH_INTERVAL_MS) { lastLedRefreshMs = now; refreshLedState(false); }
  if (now - lastDashSettingsSendMs >= 5000) { lastDashSettingsSendMs = now; sendDashSettings(); }
  if (now - lastDebugMs >= 5000) {
    lastDebugMs = now;
    Serial.printf("Master | BLE:%s | Awake:%s | SOC:%d%% | DashMode:%s | Heap:%d\n",
      bleClientConnected ? "JA" : "NEIN", carAwake ? "JA" : "NEIN",
      vehicle.batterySocPercent, dashModeToText(dashStatus.dashMode), ESP.getFreeHeap());
  }
}