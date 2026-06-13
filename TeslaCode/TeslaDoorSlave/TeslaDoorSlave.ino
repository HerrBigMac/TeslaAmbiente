#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

// =======================================================
// EINSTELLUNGEN SLAVE
// HIER PRO TUER ANPASSEN:
// 'F' = vorne rechts
// 'G' = vorne links
// 'R' = hinten links
// 'L' = hinten rechts
// =======================================================

#define DEVICE_SIDE 'F'

#define LED_PIN 21
#define NUM_LEDS 130
#define LED_TYPE NEO_GRB + NEO_KHZ800
#define ESPNOW_CHANNEL 6
#define OTA_AP_PASS "12345678"
#define OTA_DEFAULT_MINUTES 10
#define OTA_MAX_MINUTES 30
#define LED_ACK_PACKET_TYPE 0xA8
#define POWER_FADE_MS 1200
#define POWER_FADE_FRAME_MS 16
#define COLOR_FADE_MS 450

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, LED_TYPE);
Preferences prefs;
WebServer server(80);

typedef struct __attribute__((packed)) {
  uint8_t magic;      // 0xA7
  uint8_t version;    // 2
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
  uint8_t packetType; // 0xA8
  uint8_t magic;      // 0xA7
  uint8_t version;    // 1
  char device;
  char target;
  uint32_t sequence;
  uint8_t accepted;
} LedAckPacket;

typedef struct __attribute__((packed)) {
  uint8_t magic;   // 0xC6
  uint8_t version; // 1
  uint8_t command; // 3 = Door OTA AP starten
  uint8_t minutes;
  uint32_t nonce;
} SystemCommand;

LedCommand currentCmd;
LedCommand lastSavedCmd;

uint32_t lastPacketMs = 0;
uint32_t lastSequence = 0;
uint32_t lastEffectMs = 0;
uint32_t lastStatusMs = 0;
uint32_t lastSaveRequestMs = 0;
uint32_t otaApUntilMs = 0;
uint32_t powerFadeStartMs = 0;
uint32_t lastPowerFadeFrameMs = 0;
uint32_t colorFadeStartMs = 0;

uint16_t effectStep = 0;
int scannerPos = 0;
int scannerDir = 1;
uint8_t powerFadeMode = 0;
bool colorFadeActive = false;
bool pendingSave = false;
bool hasLoadedState = false;
bool otaApActive = false;

uint8_t fadeOutR = 0; uint8_t fadeOutG = 0; uint8_t fadeOutB = 0;
uint8_t fadeOutBrightness = 0;
uint8_t colorFadeStartR = 0; uint8_t colorFadeStartG = 0; uint8_t colorFadeStartB = 0;

void setupWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdt_config = { .timeout_ms = 5000, .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(5, true);
  esp_task_wdt_add(NULL);
#endif
}

uint16_t activeLedStart() {
  if (currentCmd.ledStart >= NUM_LEDS) return 0;
  if (currentCmd.ledStart > currentCmd.ledEnd) return 0;
  return currentCmd.ledStart;
}

uint16_t activeLedEnd() {
  if (currentCmd.ledEnd >= NUM_LEDS) return NUM_LEDS - 1;
  if (currentCmd.ledStart > currentCmd.ledEnd) return NUM_LEDS - 1;
  return currentCmd.ledEnd;
}

bool ledInActiveRange(int index) { return index >= activeLedStart() && index <= activeLedEnd(); }

void setPixel(int index, uint32_t color) {
  if (index < 0 || index >= NUM_LEDS) return;
  if (!ledInActiveRange(index)) return;
  strip.setPixelColor(index, color);
}

void showStrip() {
  uint16_t start = activeLedStart(); uint16_t end = activeLedEnd();
  for (int i = 0; i < NUM_LEDS; i++) { if (i < start || i > end) strip.setPixelColor(i, 0); }
  strip.show();
}

uint32_t makeColor(uint8_t r, uint8_t g, uint8_t b) { return strip.Color(r, g, b); }
uint32_t color1() { return makeColor(currentCmd.r1, currentCmd.g1, currentCmd.b1); }
uint32_t color2() { return makeColor(currentCmd.r2, currentCmd.g2, currentCmd.b2); }
uint32_t color3() { return makeColor(currentCmd.r3, currentCmd.g3, currentCmd.b3); }

uint8_t scaledByte(uint8_t value, uint8_t scale) { return ((uint16_t)value * scale) / 255; }
uint32_t dimRGB(uint8_t r, uint8_t g, uint8_t b, uint8_t scale) { return makeColor(scaledByte(r, scale), scaledByte(g, scale), scaledByte(b, scale)); }
uint32_t dimColor1(uint8_t scale) { return dimRGB(currentCmd.r1, currentCmd.g1, currentCmd.b1, scale); }

bool isStaticColorCommand(const LedCommand &cmd) { return cmd.power != 0 && cmd.mode == 0 && cmd.effect == 1; }
bool isBlindSpotDoorOrange(const LedCommand &cmd) { return isStaticColorCommand(cmd) && cmd.r1 == 255 && cmd.g1 == 120 && cmd.b1 == 0; }

uint8_t colorFadeAmount() {
  if (!colorFadeActive) return 255;
  uint32_t elapsed = millis() - colorFadeStartMs;
  if (elapsed >= COLOR_FADE_MS) { colorFadeActive = false; return 255; }
  return (uint32_t)elapsed * 255 / COLOR_FADE_MS;
}

uint8_t easeFadeByte(uint8_t amount) { uint32_t t = amount; return (t * t * (765 - (2 * t))) / 65025; }

void clearStrip() { strip.clear(); showStrip(); }

void setAll(uint32_t c) {
  for (int i = 0; i < NUM_LEDS; i++) { setPixel(i, c); }
  showStrip();
}

uint16_t effectInterval() {
  uint8_t spd = currentCmd.speed;
  if (spd < 1) spd = 1;
  return map(spd, 1, 255, 160, 5);
}

bool effectTick() {
  uint32_t now = millis();
  if (now - lastEffectMs >= effectInterval()) { lastEffectMs = now; effectStep++; return true; }
  return false;
}

uint32_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) return makeColor(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return makeColor(0, pos * 3, 255 - pos * 3); }
  pos -= 170; return makeColor(pos * 3, 255 - pos * 3, 0);
}

String otaSsid() { return "Tesla-Door-" + String(DEVICE_SIDE) + "-OTA"; }

bool commandContentChanged(const LedCommand &a, const LedCommand &b) {
  return a.power != b.power || a.mode != b.mode || a.effect != b.effect ||
         a.r1 != b.r1 || a.g1 != b.g1 || a.b1 != b.b1 ||
         a.r2 != b.r2 || a.g2 != b.g2 || a.b2 != b.b2 ||
         a.r3 != b.r3 || a.g3 != b.g3 || a.b3 != b.b3 ||
         a.brightness != b.brightness || a.speed != b.speed ||
         a.intensity != b.intensity || a.progress != b.progress ||
         a.ledStart != b.ledStart || a.ledEnd != b.ledEnd;
}

void saveLastCommandNow() {
  prefs.begin("ledstate", false);
  prefs.putBytes("cmd", &currentCmd, sizeof(currentCmd));
  prefs.end();
  lastSavedCmd = currentCmd;
  pendingSave = false;
}

bool loadLastCommand() {
  prefs.begin("ledstate", true);
  size_t len = prefs.getBytesLength("cmd");
  if (len != sizeof(currentCmd)) { prefs.end(); return false; }
  prefs.getBytes("cmd", &currentCmd, sizeof(currentCmd));
  prefs.end();
  if (currentCmd.magic != 0xA7 || currentCmd.version != 2) return false;
  lastSavedCmd = currentCmd;
  return true;
}

void setDefaultOffState() {
  currentCmd.magic = 0xA7; currentCmd.version = 2; currentCmd.target = DEVICE_SIDE;
  currentCmd.power = 0; currentCmd.mode = 0; currentCmd.effect = 0;
  currentCmd.r1 = 0; currentCmd.g1 = 0; currentCmd.b1 = 0;
  currentCmd.r2 = 0; currentCmd.g2 = 0; currentCmd.b2 = 0;
  currentCmd.r3 = 0; currentCmd.g3 = 0; currentCmd.b3 = 0;
  currentCmd.brightness = 0; currentCmd.speed = 120; currentCmd.intensity = 120;
  currentCmd.progress = 0; currentCmd.ledStart = 0; currentCmd.ledEnd = NUM_LEDS - 1;
  currentCmd.sequence = 0;
  lastSavedCmd = currentCmd;
}

void effectOff() { clearStrip(); }

void effectStatic() {
  if (!colorFadeActive) { setAll(color1()); return; }
  uint8_t amount = colorFadeAmount();
  uint8_t r = colorFadeStartR + (((int16_t)currentCmd.r1 - colorFadeStartR) * amount) / 255;
  uint8_t g = colorFadeStartG + (((int16_t)currentCmd.g1 - colorFadeStartG) * amount) / 255;
  uint8_t b = colorFadeStartB + (((int16_t)currentCmd.b1 - colorFadeStartB) * amount) / 255;
  setAll(makeColor(r, g, b));
}

void effectBreathing() {
  if (!effectTick()) return;
  float phase = (sin(effectStep * 0.06) + 1.0) / 2.0;
  setAll(dimColor1(8 + phase * 247));
}

void effectBlink() {
  if (!effectTick()) return;
  if ((effectStep % 2) == 0) setAll(color1()); else clearStrip();
}

void effectRainbow() {
  if (!effectTick()) return;
  for (int i = 0; i < NUM_LEDS; i++) setPixel(i, wheel((i * 256 / NUM_LEDS + effectStep) & 255));
  showStrip();
}

void effectColorWipe() {
  if (!effectTick()) return;
  int pos = effectStep % (NUM_LEDS + 15);
  strip.clear();
  for (int i = 0; i < pos && i < NUM_LEDS; i++) setPixel(i, color1());
  showStrip();
}

void effectScanner() {
  if (!effectTick()) return;
  strip.clear();
  for (int i = 0; i < NUM_LEDS; i++) {
    int distance = abs(i - scannerPos);
    if (distance == 0) setPixel(i, color1());
    else if (distance < 8) setPixel(i, dimColor1(255 - distance * 30));
  }
  showStrip();
  scannerPos += scannerDir;
  if (scannerPos >= NUM_LEDS - 1) { scannerPos = NUM_LEDS - 1; scannerDir = -1; }
  if (scannerPos <= 0) { scannerPos = 0; scannerDir = 1; }
}

void effectTheaterChase() {
  if (!effectTick()) return;
  strip.clear();
  for (int i = 0; i < NUM_LEDS; i++) { if ((i + effectStep) % 3 == 0) setPixel(i, color1()); }
  showStrip();
}

void effectRunningLights() {
  if (!effectTick()) return;
  for (int i = 0; i < NUM_LEDS; i++) {
    float level = (sin((i + effectStep) * 0.25) + 1.0) / 2.0;
    setPixel(i, dimColor1(level * 255));
  }
  showStrip();
}

void effectSparkle() {
  if (!effectTick()) return;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t old = strip.getPixelColor(i);
    uint8_t r = (uint8_t)(old >> 16) * 180 / 255;
    uint8_t g = (uint8_t)(old >> 8) * 180 / 255;
    uint8_t b = (uint8_t)(old) * 180 / 255;
    setPixel(i, makeColor(r, g, b));
  }
  int sparkles = map(currentCmd.intensity, 0, 255, 1, 12);
  for (int s = 0; s < sparkles; s++) setPixel(random(NUM_LEDS), color1());
  showStrip();
}

void effectFire() {
  if (!effectTick()) return;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t heat = random(80, 255); uint8_t flicker = random(0, 80);
    setPixel(i, makeColor(255, constrain(heat - flicker, 20, 160), random(0, 20)));
  }
  showStrip();
}

void effectPolice() {
  if (!effectTick()) return;
  bool phase = ((effectStep / 5) % 2) == 0;
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i < NUM_LEDS / 2) setPixel(i, phase ? makeColor(255,0,0) : makeColor(0,0,255));
    else setPixel(i, phase ? makeColor(0,0,255) : makeColor(255,0,0));
  }
  showStrip();
}

void effectProgressBar() {
  int lit = map(constrain(currentCmd.progress, 0, 100), 0, 100, 0, NUM_LEDS);
  strip.clear();
  for (int i = 0; i < lit; i++) setPixel(i, color1());
  showStrip();
}

void effectSoftFade() {
  if (!effectTick()) return;
  float phase = (sin(effectStep * 0.035) + 1.0) / 2.0;
  setAll(dimColor1(phase * 255));
}

void effectStrobe() {
  if (!effectTick()) return;
  if ((effectStep % 2) == 0) setAll(color1()); else clearStrip();
}

void effectMeteorRain() {
  if (!effectTick()) return;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t old = strip.getPixelColor(i);
    setPixel(i, makeColor((uint8_t)(old>>16)*170/255, (uint8_t)(old>>8)*170/255, (uint8_t)(old)*170/255));
  }
  int meteorSize = map(currentCmd.intensity, 0, 255, 4, 18);
  int pos = effectStep % (NUM_LEDS + meteorSize);
  for (int j = 0; j < meteorSize; j++) {
    int p = pos - j;
    if (p >= 0 && p < NUM_LEDS) setPixel(p, dimColor1(255 - (j * 255 / meteorSize)));
  }
  showStrip();
}

void effectTwoColorFade() {
  if (!effectTick()) return;
  float phase = (sin(effectStep * 0.04) + 1.0) / 2.0;
  setAll(makeColor(currentCmd.r1*(1.0-phase)+currentCmd.r2*phase, currentCmd.g1*(1.0-phase)+currentCmd.g2*phase, currentCmd.b1*(1.0-phase)+currentCmd.b2*phase));
}

void effectThreeColorFade() {
  if (!effectTick()) return;
  uint16_t phase = effectStep % 768;
  uint8_t r, g, b;
  if (phase < 256) { float t = phase/255.0; r=currentCmd.r1*(1-t)+currentCmd.r2*t; g=currentCmd.g1*(1-t)+currentCmd.g2*t; b=currentCmd.b1*(1-t)+currentCmd.b2*t; }
  else if (phase < 512) { float t=(phase-256)/255.0; r=currentCmd.r2*(1-t)+currentCmd.r3*t; g=currentCmd.g2*(1-t)+currentCmd.g3*t; b=currentCmd.b2*(1-t)+currentCmd.b3*t; }
  else { float t=(phase-512)/255.0; r=currentCmd.r3*(1-t)+currentCmd.r1*t; g=currentCmd.g3*(1-t)+currentCmd.g1*t; b=currentCmd.b3*(1-t)+currentCmd.b1*t; }
  setAll(makeColor(r, g, b));
}

void effectBlindSpot() {
  if (!effectTick()) return;
  if ((effectStep % 2) == 0) setAll(color1()); else setAll(color2());
}

uint8_t powerFadeScale() {
  if (powerFadeMode == 0) return 255;
  uint32_t elapsed = millis() - powerFadeStartMs;
  if (elapsed >= POWER_FADE_MS) { powerFadeMode = 0; return (powerFadeMode == 2) ? 0 : 255; }
  uint8_t progress = easeFadeByte((uint32_t)elapsed * 255 / POWER_FADE_MS);
  return powerFadeMode == 1 ? progress : 255 - progress;
}

void runPowerFadeOut() {
  uint32_t now = millis();
  if (lastPowerFadeFrameMs != 0 && now - lastPowerFadeFrameMs < POWER_FADE_FRAME_MS) return;
  lastPowerFadeFrameMs = now;
  uint8_t scale = powerFadeScale();
  if (scale == 0) { strip.setBrightness(0); clearStrip(); return; }
  strip.setBrightness(fadeOutBrightness);
  setAll(dimRGB(fadeOutR, fadeOutG, fadeOutB, scale));
}

void runEffect() {
  if (powerFadeMode == 2) { runPowerFadeOut(); return; }
  if (currentCmd.power == 0 || currentCmd.effect == 0) { effectOff(); return; }
  strip.setBrightness(scaledByte(currentCmd.brightness, powerFadeScale()));
  if (currentCmd.mode == 1 || currentCmd.effect == 20) { effectBlindSpot(); return; }
  switch (currentCmd.effect) {
    case 1: effectStatic(); break;
    case 2: effectBreathing(); break;
    case 3: effectBlink(); break;
    case 4: effectRainbow(); break;
    case 5: effectColorWipe(); break;
    case 6: effectScanner(); break;
    case 7: effectTheaterChase(); break;
    case 8: effectRunningLights(); break;
    case 9: effectSparkle(); break;
    case 10: effectFire(); break;
    case 11: effectPolice(); break;
    case 12: effectProgressBar(); break;
    case 13: effectSoftFade(); break;
    case 14: effectStrobe(); break;
    case 15: effectMeteorRain(); break;
    case 16: effectTwoColorFade(); break;
    case 17: effectThreeColorFade(); break;
    case 18: case 19: effectRainbow(); break;
    case 20: effectBlindSpot(); break;
    default: effectStatic(); break;
  }
}

void resetEffectState() { lastEffectMs = 0; effectStep = 0; scannerPos = 0; scannerDir = 1; }

void startPowerFade(const LedCommand &oldCmd, const LedCommand &newCmd) {
  if (oldCmd.power == 0 && newCmd.power != 0) { powerFadeMode = 1; powerFadeStartMs = millis(); lastPowerFadeFrameMs = 0; return; }
  if (oldCmd.power != 0 && newCmd.power == 0) {
    powerFadeMode = 2; powerFadeStartMs = millis(); lastPowerFadeFrameMs = 0;
    fadeOutR = oldCmd.r1; fadeOutG = oldCmd.g1; fadeOutB = oldCmd.b1; fadeOutBrightness = oldCmd.brightness;
  }
}

void startColorFadeIfNeeded(const LedCommand &oldCmd, const LedCommand &newCmd) {
  colorFadeActive = false;
  if (!isStaticColorCommand(oldCmd) || !isStaticColorCommand(newCmd)) return;
  if (oldCmd.r1 == newCmd.r1 && oldCmd.g1 == newCmd.g1 && oldCmd.b1 == newCmd.b1) return;
  if (isBlindSpotDoorOrange(newCmd)) return;
  colorFadeStartR = oldCmd.r1; colorFadeStartG = oldCmd.g1; colorFadeStartB = oldCmd.b1;
  colorFadeStartMs = millis(); colorFadeActive = true;
}

void sendLedAck(const LedCommand &cmd) {
  LedAckPacket ack = {};
  ack.packetType = LED_ACK_PACKET_TYPE; ack.magic = 0xA7; ack.version = 1;
  ack.device = DEVICE_SIDE; ack.target = cmd.target;
  ack.sequence = cmd.sequence; ack.accepted = 1;
  esp_now_send(broadcastAddress, (uint8_t*)&ack, sizeof(ack));
}

bool handleSystemCommand(const uint8_t *incomingData, int len) {
  if (len != sizeof(SystemCommand)) return false;
  SystemCommand cmd; memcpy(&cmd, incomingData, sizeof(cmd));
  if (cmd.magic != 0xC6 || cmd.version != 1) return false;
  if (cmd.command == 3) {
    uint8_t minutes = cmd.minutes;
    if (minutes < 1) minutes = OTA_DEFAULT_MINUTES;
    if (minutes > OTA_MAX_MINUTES) minutes = OTA_MAX_MINUTES;
    String ssid = otaSsid();
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid.c_str(), OTA_AP_PASS, ESPNOW_CHANNEL);
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (!otaApActive) server.begin();
    otaApActive = true;
    otaApUntilMs = millis() + (uint32_t)minutes * 60000UL;
  }
  return true;
}

void applyCommand(const LedCommand &cmd) {
  if (cmd.magic != 0xA7 || cmd.version != 2) return;
  if (cmd.target != DEVICE_SIDE && cmd.target != 'A') return;
  if (cmd.sequence == lastSequence) { sendLedAck(cmd); return; }
  lastSequence = cmd.sequence;
  LedCommand normalizedCmd = cmd;
  if (normalizedCmd.ledStart >= NUM_LEDS) normalizedCmd.ledStart = 0;
  if (normalizedCmd.ledEnd >= NUM_LEDS) normalizedCmd.ledEnd = NUM_LEDS - 1;
  if (normalizedCmd.ledStart > normalizedCmd.ledEnd) { normalizedCmd.ledStart = 0; normalizedCmd.ledEnd = NUM_LEDS - 1; }
  bool changed = commandContentChanged(currentCmd, normalizedCmd);
  LedCommand oldCmd = currentCmd;
  currentCmd = normalizedCmd;
  lastPacketMs = millis();
  sendLedAck(normalizedCmd);
  if (changed) {
    pendingSave = true; lastSaveRequestMs = millis();
    startPowerFade(oldCmd, normalizedCmd);
    startColorFadeIfNeeded(oldCmd, normalizedCmd);
    if (oldCmd.power == normalizedCmd.power || (oldCmd.power == 0 && normalizedCmd.power != 0)) resetEffectState();
  }
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (handleSystemCommand(incomingData, len)) return;
  if (len != sizeof(LedCommand)) return;
  LedCommand incomingCmd;
  memcpy(&incomingCmd, incomingData, sizeof(incomingCmd));
  applyCommand(incomingCmd);
}

void setupWebOta() {
  server.on("/", HTTP_GET, []() {
    String page = "<!doctype html><html><body><h1>Door Slave ";
    page += DEVICE_SIDE; page += " OTA</h1><p>Seite: "; page += DEVICE_SIDE; page += "</p>";
    page += "<p>Effekt: " + String(currentCmd.effect) + " | Mode: " + String(currentCmd.mode) + "</p>";
    page += "<p><a href='/update'>OTA Update</a></p></body></html>";
    server.send(200, "text/html", page);
  });
  server.on("/update", HTTP_GET, []() {
    String page = "<!doctype html><html><body><h1>Door OTA</h1>";
    page += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    page += "<input type='file' name='update'><button type='submit'>Flashen</button></form></body></html>";
    server.send(200, "text/html", page);
  });
  server.on("/update", HTTP_POST,
    []() { server.sendHeader("Connection", "close"); server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK"); delay(500); ESP.restart(); },
    []() {
      HTTPUpload &upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      else if (upload.status == UPLOAD_FILE_WRITE) { esp_task_wdt_reset(); Update.write(upload.buf, upload.currentSize); }
      else if (upload.status == UPLOAD_FILE_END) { Update.end(true); }
    }
  );
}

void setup() {
  Serial.begin(115200);
  delay(300);
  setupWatchdog();
  strip.begin(); strip.setBrightness(0); strip.clear(); strip.show();
  hasLoadedState = loadLastCommand();
  if (!hasLoadedState) setDefaultOffState();
  strip.setBrightness(currentCmd.brightness);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
  setupWebOta();
  if (esp_now_init() != ESP_OK) { delay(1000); ESP.restart(); }
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL; peerInfo.encrypt = false;
  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) { delay(1000); ESP.restart(); }
  }
  esp_now_register_recv_cb(onDataRecv);
  resetEffectState();
  runEffect();
  Serial.print("Door Slave "); Serial.print(DEVICE_SIDE); Serial.println(" bereit");
}

void loop() {
  esp_task_wdt_reset();
  if (otaApActive) {
    server.handleClient();
    if (millis() > otaApUntilMs) {
      server.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA);
      esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE); esp_wifi_set_ps(WIFI_PS_NONE);
      otaApActive = false; otaApUntilMs = 0;
    }
  }
  runEffect();
  if (pendingSave && millis() - lastSaveRequestMs > 250) saveLastCommandNow();
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    Serial.print("Slave "); Serial.print(DEVICE_SIDE);
    Serial.print(" | Effekt "); Serial.print(currentCmd.effect);
    Serial.print(" | OTA AP "); Serial.println(otaApActive ? "an" : "aus");
  }
}