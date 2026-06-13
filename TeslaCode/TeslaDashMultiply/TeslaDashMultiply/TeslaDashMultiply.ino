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

#define VEHICLE_CAN_TX_PIN GPIO_NUM_1
#define VEHICLE_CAN_RX_PIN GPIO_NUM_2
#define CHASSIS_CAN_TX_PIN  GPIO_NUM_15
#define CHASSIS_CAN_RX_PIN  GPIO_NUM_14

#define LED_PIN 4
#define NUM_LEDS 122
#define DEFAULT_DASH_LED_COUNT 122
#define LED_TYPE NEO_GRB + NEO_KHZ800

#define MAX_LED_BRIGHTNESS_PERCENT 80
#define MAX_LED_BRIGHTNESS ((255 * MAX_LED_BRIGHTNESS_PERCENT) / 100)
#define MIN_AUTO_LED_BRIGHTNESS 2
#define CHARGE_GREEN_AT_PERCENT 75
#define DASH_SETTINGS_PACKET_TYPE 0xC7
#define CHASSIS_CAN_TIMEOUT_MS 5000
#define DASH_TRANSITION_MS 250
#define DASH_TRANSITION_FRAME_MS 18
#define CHARGE_ANIMATION_TIMEOUT_MS 1800000UL

#define DASH_STATUS_PACKET_TYPE 0xC8
#define DASH_MODE_OFF       0
#define DASH_MODE_BASE      1
#define DASH_MODE_BLINKER   2
#define DASH_MODE_BLIND     3
#define DASH_MODE_AUTOPILOT 4
#define DASH_MODE_CHARGING  5
#define DASH_MODE_WELCOME   6
#define DASH_MODE_GOODBYE   7
#define DASH_MODE_DOOR      8

#define ESPNOW_CHANNEL 6
#define OTA_AP_SSID "Tesla-Dash"
#define OTA_AP_PASS "12345678"
#define OTA_DEFAULT_MINUTES 10

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
WebServer server(80);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, LED_TYPE);

#define ID_BLINKER          0x3F5
#define ID_BRIGHTNESS       0x273
#define ID_GEAR             0x118
#define ID_VCLEFT_STATUS    0x102
#define ID_VCRIGHT_STATUS   0x103
#define ID_BMS_STATUS       0x212
#define ID_BMS_SOC          0x292
#define ID_LIFTGATE_STATUS  0x142
#define ID_CP_CHARGE_STATUS 0x13D
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

#define CP_CHARGE_ENABLED       5
#define CP_CHARGE_TYPE_DC       1
#define CP_CHARGE_TYPE_AC       2

#define LATCH_CLOSED        2
#define DISPLAY_BRIGHTNESS_MIN_RAW 0x0B
#define DISPLAY_BRIGHTNESS_MAX_RAW 0xC8

struct VehiclePacket {
  uint8_t packetType;
  bool blinkerLeftPulse; bool blinkerRightPulse;
  uint8_t displayBrightnessRaw; float displayBrightnessPercent;
  uint8_t gear; bool carInPark;
  uint8_t mirrorLeftState; uint8_t mirrorRightState;
  bool mirrorLeftFolded; bool mirrorRightFolded;
  bool mirrorsFolded; bool mirrorsUnfolded;
  bool vehicleAwake;
  uint32_t counter; uint32_t lastVehicleCanAgeMs;
  bool chargingActive; uint8_t chargeStatus; uint8_t batterySocPercent; uint32_t lastChargeCanAgeMs;
  bool doorFrontLeftOpen; bool doorFrontRightOpen; bool doorRearLeftOpen; bool doorRearRightOpen; bool trunkOpen;
};

struct ChassisPacket {
  uint8_t packetType;
  uint8_t autopilotState; bool autopilotActive;
  uint8_t blindLeftRaw; uint8_t blindRightRaw;
  bool blindLeftActive; bool blindRightActive;
  uint32_t counter;
};

typedef struct __attribute__((packed)) {
  uint8_t magic;    // 0xC6
  uint8_t version;  // 1
  uint8_t command;  // 1=OTA AP, 4=Restart
  uint8_t minutes;
  uint32_t nonce;
} SystemCommand;

typedef struct __attribute__((packed)) {
  uint8_t packetType; uint8_t version;
  uint8_t baseR; uint8_t baseG; uint8_t baseB;
  uint8_t baseEffect; uint8_t baseSpeed; uint8_t baseIntensity;
  uint8_t manualBrightness; bool autoBrightness; bool powerOff;
  bool chargeDashEnabled; bool autopilotDashEnabled;
  uint8_t autopilotR; uint8_t autopilotG; uint8_t autopilotB;
  bool blindSpotDashEnabled; bool blindSpotOnlyWithBlinker; uint8_t blindSpotDashPercent;
  uint16_t dashLedCount;
  bool doorOpenHighlightEnabled; bool welcomeAnimationEnabled; bool goodbyeAnimationEnabled;
  bool blinkerDashEnabled; uint8_t blinkerDashPercent;
} DashSettingsPacket;

typedef struct __attribute__((packed)) {
  uint8_t packetType; uint8_t dashMode;
  uint8_t vehicleBusState; uint8_t chassisBusState;
  uint32_t vehicleCanAgeMs; uint32_t chassisCanAgeMs;
  uint8_t brightnessPct; uint8_t gear; uint8_t soc;
  bool vehicleAwake; bool chargingActive;
  bool blinkerLeft; bool blinkerRight;
  bool blindLeft; bool blindRight; bool autopilot;
} DashStatusPacket;

twai_handle_t vehicleTwai = NULL;
twai_handle_t chassisTwai = NULL;
VehiclePacket vehicleData = {};
ChassisPacket chassisData = {};
uint32_t vehicleSendCounter = 0;
uint32_t chassisSendCounter = 0;

unsigned long lastVehicleSend = 0; unsigned long lastChassisSend = 0;
unsigned long lastVehicleTwaiCheck = 0; unsigned long lastChassisTwaiCheck = 0;
unsigned long twaiOffSinceMs[2] = {0,0}; unsigned long twaiSoftResetMs[2] = {0,0};
unsigned long lastAnyRelevantCanSeen = 0; unsigned long lastBlinkerFrameSeen = 0;
unsigned long lastChargeFrameSeen = 0; unsigned long chargeAnimationStartMs = 0;
unsigned long lastBmsSocSeen = 0; unsigned long last399Seen = 0;
unsigned long lastDebug = 0; unsigned long otaApUntilMs = 0;
unsigned long lastDashEffectMs = 0; unsigned long lastDashStatusSend = 0;
uint8_t dashCurrentMode = DASH_MODE_OFF;
unsigned long welcomeUntilMs = 0; unsigned long goodbyeUntilMs = 0;
bool otaApActive = false; bool carAwake = true; bool lastCarAwake = true;
bool welcomePlayedForWake = false; bool goodbyeActive = false;
uint16_t dashEffectStep = 0;
uint32_t dashCurrentPixels[NUM_LEDS]; uint32_t dashStartPixels[NUM_LEDS];
uint32_t dashTargetPixels[NUM_LEDS]; bool dashImmediatePixels[NUM_LEDS];
uint32_t dashLastTargetHash = 0; unsigned long dashTransitionStartMs = 0;
unsigned long lastDashTransitionFrameMs = 0;
bool dashTransitionActive = false; bool dashCurrentValid = false;
uint8_t lastDashBrightness = 255; bool dashBrightnessDirty = false;
bool lastBlindLeftShown = false; bool lastBlindRightShown = false;
bool lastBlinkerLeftShown = false; bool lastBlinkerRightShown = false;

uint8_t baseR=255,baseG=0,baseB=0,baseEffect=1,baseSpeed=120,baseIntensity=120,manualBrightness=120;
bool autoBrightness=true, dashPowerOff=false;
bool chargeDashEnabled=true, autopilotDashEnabled=true;
uint8_t autopilotR=0, autopilotG=70, autopilotB=255;
bool blindSpotDashEnabled=true, blindSpotOnlyWithBlinker=true;
uint8_t blindSpotDashPercent=25;
bool blinkerDashEnabled=false; uint8_t blinkerDashPercent=25;
uint16_t activeDashLedCount=DEFAULT_DASH_LED_COUNT;
bool doorOpenHighlightEnabled=true, welcomeAnimationEnabled=true, goodbyeAnimationEnabled=true;

bool blinkerLeftPulse=false, blinkerRightPulse=false;
uint8_t displayBrightnessRaw=0xC8; float displayBrightnessPercent=100.0;
uint8_t gear=GEAR_UNKNOWN; bool carInPark=false;
uint8_t mirrorLeftState=MIRROR_UNKNOWN, mirrorRightState=MIRROR_UNKNOWN;
bool mirrorLeftFolded=false, mirrorRightFolded=false, mirrorsFolded=false, mirrorsUnfolded=false;
bool vehicleAwake=true;
uint8_t batterySocPercent=0, chargeStatus=BMS_CHARGE_DISCONNECTED;
uint8_t cpHvChargeStatus=0, cpEvseChargeType=0;
bool cpChargingActive=false, chargingActive=false;
bool doorFrontLeftOpen=false, doorFrontRightOpen=false, doorRearLeftOpen=false, doorRearRightOpen=false, trunkOpen=false;
uint8_t autopilotState=0; bool autopilotActive=false;
uint8_t blindLeftRaw=0, blindRightRaw=0; bool blindLeftActive=false, blindRightActive=false;

uint32_t extractIntelSignal(const uint8_t *data, uint8_t startBit, uint8_t bitLength) {
  uint64_t raw = 0;
  for (int i = 0; i < 8; i++) raw |= ((uint64_t)data[i]) << (8*i);
  uint64_t mask = (1ULL << bitLength) - 1ULL;
  return (raw >> startBit) & mask;
}

float displayBrightnessRawToPercent(uint8_t raw) {
  if (raw <= DISPLAY_BRIGHTNESS_MIN_RAW) return 0.0f;
  if (raw >= DISPLAY_BRIGHTNESS_MAX_RAW) return 100.0f;
  return (raw - DISPLAY_BRIGHTNESS_MIN_RAW) * 100.0f / (DISPLAY_BRIGHTNESS_MAX_RAW - DISPLAY_BRIGHTNESS_MIN_RAW);
}

bool latchMeansDoorOpen(uint8_t s) { return s != LATCH_CLOSED; }
bool latchMeansTrunkOpen(uint8_t s) { return s==1||s==3||s==4||s==5; }
bool anyDoorOpen() { return doorFrontLeftOpen||doorFrontRightOpen||doorRearLeftOpen||doorRearRightOpen||trunkOpen; }
bool anyMirrorUnfoldedOrUnfolding() { return mirrorLeftState==MIRROR_UNFOLDED||mirrorRightState==MIRROR_UNFOLDED||mirrorLeftState==MIRROR_UNFOLDING||mirrorRightState==MIRROR_UNFOLDING; }

void updateMirrorLogic() {
  mirrorLeftFolded = (mirrorLeftState==MIRROR_FOLDED);
  mirrorRightFolded = (mirrorRightState==MIRROR_FOLDED);
  mirrorsFolded = mirrorLeftState==MIRROR_FOLDED && mirrorRightState==MIRROR_FOLDED && !anyDoorOpen();
  mirrorsUnfolded = mirrorLeftState==MIRROR_UNFOLDED && mirrorRightState==MIRROR_UNFOLDED;
  if (anyDoorOpen()||anyMirrorUnfoldedOrUnfolding()) vehicleAwake=true;
  else if (mirrorsFolded) vehicleAwake=false;
}

void resetDashEffect() { lastDashEffectMs=0; dashEffectStep=0; }

void updateChargingActive() {
  bool wasCharging = chargingActive;
  chargingActive = (chargeStatus==BMS_CHARGING)||cpChargingActive;
  if (chargingActive&&!wasCharging) { chargeAnimationStartMs=millis(); resetDashEffect(); }
  if (!chargingActive) chargeAnimationStartMs=0;
}

uint8_t scaledByte(uint8_t v, uint8_t s) { return ((uint16_t)v*s)/255; }

uint8_t effectiveBrightness() {
  if (!autoBrightness) return map(manualBrightness, 0, 255, 0, MAX_LED_BRIGHTNESS);
  uint8_t b = map(map((int)displayBrightnessPercent, 0, 100, 0, 255), 0, 255, 0, MAX_LED_BRIGHTNESS);
  if (displayBrightnessPercent>0.5f && b<MIN_AUTO_LED_BRIGHTNESS) return MIN_AUTO_LED_BRIGHTNESS;
  return b;
}

void updateDashBrightness() {
  uint8_t b = effectiveBrightness();
  if (b!=lastDashBrightness) { lastDashBrightness=b; dashBrightnessDirty=true; }
  strip.setBrightness(b);
}

void flushDashBrightnessIfNeeded() {
  if (!dashBrightnessDirty) return;
  strip.show(); dashBrightnessDirty=false;
}

uint32_t dashColor(uint8_t r, uint8_t g, uint8_t b) { return strip.Color(r,g,b); }
uint16_t dashLedCount() { if (activeDashLedCount<1) return DEFAULT_DASH_LED_COUNT; if (activeDashLedCount>NUM_LEDS) return NUM_LEDS; return activeDashLedCount; }
uint8_t dashRed(uint32_t c) { return (uint8_t)(c>>16); }
uint8_t dashGreen(uint32_t c) { return (uint8_t)(c>>8); }
uint8_t dashBlue(uint32_t c) { return (uint8_t)c; }

uint32_t dashBlendColor(uint32_t from, uint32_t to, uint8_t amount) {
  uint8_t r = dashRed(from)+(((int16_t)dashRed(to)-dashRed(from))*amount)/255;
  uint8_t g = dashGreen(from)+(((int16_t)dashGreen(to)-dashGreen(from))*amount)/255;
  uint8_t b = dashBlue(from)+(((int16_t)dashBlue(to)-dashBlue(from))*amount)/255;
  return dashColor(r,g,b);
}

uint32_t dashTargetHash() {
  uint32_t hash=2166136261UL; uint16_t count=dashLedCount();
  for (int i=0;i<count;i++) { hash^=dashTargetPixels[i]; hash*=16777619UL; hash^=dashImmediatePixels[i]; hash*=16777619UL; }
  return hash;
}

void dashCaptureCurrent() {
  uint16_t count=dashLedCount();
  for (int i=0;i<count;i++) dashCurrentPixels[i]=strip.getPixelColor(i);
  for (int i=count;i<NUM_LEDS;i++) dashCurrentPixels[i]=0;
  dashCurrentValid=true; dashLastTargetHash=0;
}

void dashShowImmediateTarget() {
  uint16_t count=dashLedCount();
  for (int i=0;i<count;i++) { dashCurrentPixels[i]=dashTargetPixels[i]; strip.setPixelColor(i,dashCurrentPixels[i]); }
  for (int i=count;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  strip.show(); dashBrightnessDirty=false; dashCurrentValid=true; dashTransitionActive=false;
}

bool dashRenderTransition() {
  if (!dashTransitionActive) return false;
  uint32_t now=millis();
  if (now-lastDashTransitionFrameMs<DASH_TRANSITION_FRAME_MS) return true;
  lastDashTransitionFrameMs=now;
  uint32_t elapsed=now-dashTransitionStartMs;
  uint8_t amount=elapsed>=DASH_TRANSITION_MS?255:(elapsed*255UL)/DASH_TRANSITION_MS;
  uint16_t count=dashLedCount();
  for (int i=0;i<count;i++) {
    dashCurrentPixels[i]=dashImmediatePixels[i]?dashTargetPixels[i]:dashBlendColor(dashStartPixels[i],dashTargetPixels[i],amount);
    strip.setPixelColor(i,dashCurrentPixels[i]);
  }
  for (int i=count;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  strip.show(); dashBrightnessDirty=false;
  if (amount>=255) dashTransitionActive=false;
  return true;
}

void dashApplyTarget(bool smooth=true) {
  uint32_t hash=dashTargetHash();
  if (dashCurrentValid&&hash==dashLastTargetHash) { if (!dashRenderTransition()) flushDashBrightnessIfNeeded(); return; }
  dashLastTargetHash=hash;
  if (!smooth||!dashCurrentValid) { dashShowImmediateTarget(); return; }
  if (dashTransitionActive) { lastDashTransitionFrameMs=0; dashRenderTransition(); }
  uint16_t count=dashLedCount();
  for (int i=0;i<count;i++) dashStartPixels[i]=dashCurrentPixels[i];
  dashTransitionStartMs=millis(); lastDashTransitionFrameMs=0;
  dashTransitionActive=true; dashRenderTransition();
}

void dashOff() {
  for (int i=0;i<NUM_LEDS;i++) { dashTargetPixels[i]=0; dashImmediatePixels[i]=false; }
  dashApplyTarget(true);
}

void dashSetAll(uint32_t c) {
  uint16_t count=dashLedCount();
  for (int i=0;i<NUM_LEDS;i++) { dashTargetPixels[i]=i<count?c:0; dashImmediatePixels[i]=false; }
  dashApplyTarget(true);
}

bool dashEffectTick(uint16_t intervalMs=28) {
  uint32_t now=millis();
  if (now-lastDashEffectMs>=intervalMs) { lastDashEffectMs=now; dashEffectStep++; return true; }
  return false;
}

void dashBase() { dashSetAll(dashColor(baseR,baseG,baseB)); }

void dashSegment(bool left, bool right, uint8_t percent, uint32_t c, uint32_t bg, bool immediateSegment) {
  if (percent<1) percent=1; if (percent>50) percent=50;
  uint16_t count=dashLedCount();
  int segment=max(1,(count*percent)/100);
  for (int i=0;i<NUM_LEDS;i++) { dashTargetPixels[i]=i<count?bg:0; dashImmediatePixels[i]=false; }
  if (right) { for (int i=0;i<segment;i++) { dashTargetPixels[i]=c; dashImmediatePixels[i]=immediateSegment; } }
  if (left) { for (int i=count-segment;i<count;i++) { dashTargetPixels[i]=c; dashImmediatePixels[i]=immediateSegment; } }
  dashApplyTarget(true);
}

uint16_t chargeSweepIntervalMs() {
  if (cpEvseChargeType==CP_CHARGE_TYPE_DC) return 12;
  if (cpEvseChargeType==CP_CHARGE_TYPE_AC) return 65;
  return 30;
}

void dashChargeSweep() {
  dashTransitionActive=false;
  uint8_t soc=min((uint8_t)100, batterySocPercent);
  uint16_t count=dashLedCount();
  int lit=map(soc,0,100,0,count);
  bool isGreen=soc>=CHARGE_GREEN_AT_PERCENT;
  uint32_t base=strip.Color(isGreen?0:242, isGreen?255:0, 0);
  uint32_t pulse=strip.Color(isGreen?0:255, isGreen?255:0, 0);
  if (lit<=0) { dashOff(); return; }
  if (!dashEffectTick(chargeSweepIntervalMs())) { flushDashBrightnessIfNeeded(); return; }
  int sweepWidth=min(10,max(3,lit/5));
  int sweepHead=dashEffectStep%(lit+sweepWidth);
  strip.clear();
  for (int i=0;i<lit;i++) strip.setPixelColor(i,base);
  for (int j=0;j<sweepWidth;j++) { int p=sweepHead-j; if (p>=0&&p<lit) strip.setPixelColor(p,pulse); }
  strip.show(); dashBrightnessDirty=false; dashCaptureCurrent();
}

void dashWelcome() {
  dashTransitionActive=false;
  if (!dashEffectTick(28)) { flushDashBrightnessIfNeeded(); return; }
  uint16_t count=dashLedCount();
  int cL=(count/2)-1, cR=count/2, maxR=(count/2)+2;
  int radius=map(constrain((int)dashEffectStep,0,42),0,42,0,maxR);
  strip.clear();
  for (int r=0;r<=radius;r++) {
    if (cL-r>=0) strip.setPixelColor(cL-r,dashColor(baseR,baseG,baseB));
    if (cR+r<count) strip.setPixelColor(cR+r,dashColor(baseR,baseG,baseB));
  }
  strip.show(); dashBrightnessDirty=false; dashCaptureCurrent();
}

void dashGoodbye() {
  dashTransitionActive=false;
  if (!dashEffectTick(28)) { flushDashBrightnessIfNeeded(); return; }
  uint16_t count=dashLedCount();
  int cL=(count/2)-1, cR=count/2, maxR=(count/2)+2;
  int radius=map(constrain((int)dashEffectStep,0,42),0,42,maxR,0);
  strip.clear();
  for (int r=0;r<=radius;r++) {
    if (cL-r>=0) strip.setPixelColor(cL-r,dashColor(baseR,baseG,baseB));
    if (cR+r<count) strip.setPixelColor(cR+r,dashColor(baseR,baseG,baseB));
  }
  strip.show(); dashBrightnessDirty=false; dashCaptureCurrent();
}

void refreshDash() {
  updateDashBrightness();
  if (dashPowerOff) { dashCurrentMode=DASH_MODE_OFF; dashOff(); return; }
  bool chargeNow=chargeDashEnabled&&chargingActive&&lastChargeFrameSeen>0&&
                 millis()-lastChargeFrameSeen<5000&&chargeAnimationStartMs>0&&
                 millis()-chargeAnimationStartMs<CHARGE_ANIMATION_TIMEOUT_MS;
  if (chargeNow) { dashCurrentMode=DASH_MODE_CHARGING; dashChargeSweep(); return; }
  if (welcomeUntilMs>millis()) { dashCurrentMode=DASH_MODE_WELCOME; dashWelcome(); return; }
  if (goodbyeActive) {
    if (goodbyeUntilMs>millis()) { dashCurrentMode=DASH_MODE_GOODBYE; dashGoodbye(); return; }
    goodbyeActive=false; dashCurrentMode=DASH_MODE_OFF; dashOff(); return;
  }
  if (!carAwake) { dashCurrentMode=DASH_MODE_OFF; dashOff(); return; }
  bool leftDoorOpen=doorFrontLeftOpen||doorRearLeftOpen;
  bool rightDoorOpen=doorFrontRightOpen||doorRearRightOpen;
  bool blindLeftAllowed=blindSpotDashEnabled&&blindLeftActive&&(!blindSpotOnlyWithBlinker||blinkerLeftPulse);
  bool blindRightAllowed=blindSpotDashEnabled&&blindRightActive&&(!blindSpotOnlyWithBlinker||blinkerRightPulse);
  uint32_t bg=dashColor(baseR,baseG,baseB);
  bool blinkerLeftShown=blinkerDashEnabled&&blinkerLeftPulse;
  bool blinkerRightShown=blinkerDashEnabled&&blinkerRightPulse;
  if ((lastBlindLeftShown||lastBlindRightShown)&&!(blindLeftAllowed||blindRightAllowed)) dashLastTargetHash=0;
  if ((lastBlinkerLeftShown||lastBlinkerRightShown)&&!(blinkerLeftShown||blinkerRightShown)) dashLastTargetHash=0;
  if (doorOpenHighlightEnabled&&(leftDoorOpen||rightDoorOpen)) {
    dashCurrentMode=DASH_MODE_DOOR;
    dashSegment(leftDoorOpen,rightDoorOpen,25,dashColor(255,120,0),bg,true);
    lastBlindLeftShown=lastBlindRightShown=lastBlinkerLeftShown=lastBlinkerRightShown=false;
  } else if (autopilotDashEnabled&&autopilotActive) {
    dashCurrentMode=DASH_MODE_AUTOPILOT;
    dashSetAll(dashColor(autopilotR,autopilotG,autopilotB));
    lastBlindLeftShown=lastBlindRightShown=lastBlinkerLeftShown=lastBlinkerRightShown=false;
  } else if (blindLeftAllowed||blindRightAllowed) {
    dashCurrentMode=DASH_MODE_BLIND;
    dashSegment(blindLeftAllowed,blindRightAllowed,blindSpotDashPercent,dashColor(255,120,0),bg,true);
    lastBlindLeftShown=blindLeftAllowed; lastBlindRightShown=blindRightAllowed;
    lastBlinkerLeftShown=lastBlinkerRightShown=false;
  } else if (blinkerLeftShown||blinkerRightShown) {
    dashCurrentMode=DASH_MODE_BLINKER;
    dashSegment(blinkerLeftShown,blinkerRightShown,blinkerDashPercent,dashColor(255,140,0),bg,true);
    lastBlindLeftShown=lastBlindRightShown=false;
    lastBlinkerLeftShown=blinkerLeftShown; lastBlinkerRightShown=blinkerRightShown;
  } else {
    dashCurrentMode=DASH_MODE_BASE; dashBase();
    lastBlindLeftShown=lastBlindRightShown=lastBlinkerLeftShown=lastBlinkerRightShown=false;
  }
}

void setupTwaiBus(const char *name, twai_handle_t *handle, int controllerId, gpio_num_t txPin, gpio_num_t rxPin) {
  twai_general_config_t g_config=TWAI_GENERAL_CONFIG_DEFAULT_V2(controllerId,txPin,rxPin,TWAI_MODE_LISTEN_ONLY);
  g_config.tx_queue_len=0; g_config.rx_queue_len=32;
  twai_timing_config_t t_config=TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install_v2(&g_config,&t_config,&f_config,handle)!=ESP_OK) { delay(1000); ESP.restart(); }
  if (twai_start_v2(*handle)!=ESP_OK) { delay(1000); ESP.restart(); }
  Serial.print("TWAI ready "); Serial.print(name); Serial.print(" TX="); Serial.print((int)txPin); Serial.print(" RX="); Serial.println((int)rxPin);
}

void setupCAN() {
  setupTwaiBus("VehicleBus",&vehicleTwai,0,VEHICLE_CAN_TX_PIN,VEHICLE_CAN_RX_PIN);
  setupTwaiBus("ChassisBus",&chassisTwai,1,CHASSIS_CAN_TX_PIN,CHASSIS_CAN_RX_PIN);
}

void startOtaAp(uint8_t minutes) {
  if (minutes<1) minutes=OTA_DEFAULT_MINUTES;
  if (minutes>30) minutes=30;
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(OTA_AP_SSID,OTA_AP_PASS,ESPNOW_CHANNEL);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (!otaApActive) server.begin();
  otaApActive=true; otaApUntilMs=millis()+(uint32_t)minutes*60000UL;
  Serial.print("OTA AP "); Serial.print(minutes); Serial.println(" min");
}

void stopOtaAp() {
  if (!otaApActive) return;
  server.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL,WIFI_SECOND_CHAN_NONE); esp_wifi_set_ps(WIFI_PS_NONE);
  otaApActive=false; otaApUntilMs=0;
}

void handleSystemCommand(const uint8_t *incomingData, int len) {
  if (len!=sizeof(SystemCommand)) return;
  SystemCommand cmd; memcpy(&cmd,incomingData,sizeof(cmd));
  if (cmd.magic!=0xC6||cmd.version!=1) return;
  if (cmd.command==1) startOtaAp(cmd.minutes);
  else if (cmd.command==4) { delay(100); ESP.restart(); }
}

void handleDashSettings(const uint8_t *incomingData, int len) {
  if (len!=sizeof(DashSettingsPacket)) return;
  DashSettingsPacket packet; memcpy(&packet,incomingData,sizeof(packet));
  if (packet.packetType!=DASH_SETTINGS_PACKET_TYPE||packet.version!=1) return;
  bool changed=baseR!=packet.baseR||baseG!=packet.baseG||baseB!=packet.baseB||
               manualBrightness!=packet.manualBrightness||autoBrightness!=packet.autoBrightness||
               dashPowerOff!=packet.powerOff;
  baseR=packet.baseR; baseG=packet.baseG; baseB=packet.baseB;
  baseEffect=packet.baseEffect; baseSpeed=packet.baseSpeed; baseIntensity=packet.baseIntensity;
  manualBrightness=packet.manualBrightness; autoBrightness=packet.autoBrightness; dashPowerOff=packet.powerOff;
  chargeDashEnabled=packet.chargeDashEnabled; autopilotDashEnabled=packet.autopilotDashEnabled;
  autopilotR=packet.autopilotR; autopilotG=packet.autopilotG; autopilotB=packet.autopilotB;
  blindSpotDashEnabled=packet.blindSpotDashEnabled; blindSpotOnlyWithBlinker=packet.blindSpotOnlyWithBlinker;
  blindSpotDashPercent=constrain(packet.blindSpotDashPercent,1,50);
  activeDashLedCount=constrain(packet.dashLedCount,1,NUM_LEDS);
  doorOpenHighlightEnabled=packet.doorOpenHighlightEnabled;
  welcomeAnimationEnabled=packet.welcomeAnimationEnabled; goodbyeAnimationEnabled=packet.goodbyeAnimationEnabled;
  blinkerDashEnabled=packet.blinkerDashEnabled; blinkerDashPercent=constrain(packet.blinkerDashPercent,1,50);
  if (changed) { dashLastTargetHash=0; dashBrightnessDirty=true; }
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#endif
  if (len>=1&&incomingData[0]==DASH_SETTINGS_PACKET_TYPE) { handleDashSettings(incomingData,len); return; }
  handleSystemCommand(incomingData,len);
}

void setupWebOta() {
  server.on("/",HTTP_GET,[](){
    String p="<!doctype html><html><body><h1>Tesla Dash CAN-Bridge C6</h1>";
    p+="<p>SOC:"+String(batterySocPercent)+"% Awake:"+String(vehicleAwake)+" AP:"+String(autopilotActive)+"</p>";
    p+="<p><a href='/update'>OTA Update</a></p></body></html>";
    server.send(200,"text/html",p);
  });
  server.on("/update",HTTP_GET,[](){
    server.send(200,"text/html","<!doctype html><html><body><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><button>Flash</button></form></body></html>");
  });
  server.on("/update",HTTP_POST,
    [](){ server.sendHeader("Connection","close"); server.send(200,"text/plain",Update.hasError()?"FAIL":"OK"); delay(500); ESP.restart(); },
    [](){
      HTTPUpload &upload=server.upload();
      if (upload.status==UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if (upload.status==UPLOAD_FILE_WRITE) Update.write(upload.buf,upload.currentSize);
      else if (upload.status==UPLOAD_FILE_END) Update.end(true);
    }
  );
}

void setupWifi() {
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  esp_wifi_set_channel(ESPNOW_CHANNEL,WIFI_SECOND_CHAN_NONE); esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
}

void setupEspNow() {
  if (esp_now_init()!=ESP_OK) { delay(1000); ESP.restart(); }
  esp_now_peer_info_t peerInfo={};
  memcpy(peerInfo.peer_addr,broadcastAddress,6);
  peerInfo.channel=ESPNOW_CHANNEL; peerInfo.encrypt=false; peerInfo.ifidx=WIFI_IF_STA;
  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo)!=ESP_OK) { delay(1000); ESP.restart(); }
  }
  esp_now_register_recv_cb(onDataRecv);
}

void handleVehicleCAN(const twai_message_t &msg) {
  if (msg.extd||msg.rtr) return;
  const uint32_t id=msg.identifier;
  const uint8_t len=msg.data_length_code;
  const uint8_t *d=msg.data;
  bool relevant=false;
  if (id==ID_BLINKER&&len>=8) {
    relevant=true;
    blinkerLeftPulse=((d[0]&0x03)!=0)||((d[6]&0x0C)!=0);
    blinkerRightPulse=((d[0]&0x0C)!=0)||((d[6]&0x30)!=0);
    lastBlinkerFrameSeen=millis();
  } else if (id==ID_BRIGHTNESS&&len>=8) {
    relevant=true; displayBrightnessRaw=d[4];
    displayBrightnessPercent=displayBrightnessRawToPercent(displayBrightnessRaw);
  } else if (id==ID_GEAR&&len>=8) {
    relevant=true; gear=(d[2]>>5)&0x07; carInPark=(gear==GEAR_P);
  } else if (id==ID_VCLEFT_STATUS&&len>=8) {
    relevant=true;
    doorFrontLeftOpen=latchMeansDoorOpen(d[0]&0x0F);
    doorRearLeftOpen=latchMeansDoorOpen((d[0]>>4)&0x0F);
    mirrorLeftState=(d[6]>>4)&0x07; updateMirrorLogic();
  } else if (id==ID_VCRIGHT_STATUS&&len>=8) {
    relevant=true;
    doorFrontRightOpen=latchMeansDoorOpen(d[0]&0x0F);
    doorRearRightOpen=latchMeansDoorOpen((d[0]>>4)&0x0F);
    trunkOpen=latchMeansTrunkOpen(d[7]&0x0F);
    mirrorRightState=(d[6]>>4)&0x07; updateMirrorLogic();
  } else if (id==ID_LIFTGATE_STATUS&&len>=1) {
    relevant=true;
    if ((d[0]&0x03)==0) { uint8_t s=(d[0]>>3)&0x0F; trunkOpen=(s!=5&&s!=8&&s!=9); }
  } else if (id==ID_BMS_SOC&&len>=8) {
    relevant=true;
    float socUi=extractIntelSignal(d,10,10)*0.1f;
    batterySocPercent=(uint8_t)constrain(socUi+0.5f,0.0f,100.0f);
    lastBmsSocSeen=millis();
  } else if (id==ID_BMS_STATUS&&len>=2) {
    relevant=true; chargeStatus=(d[1]>>3)&0x07;
    updateChargingActive(); lastChargeFrameSeen=millis();
  } else if (id==ID_CP_CHARGE_STATUS&&len>=6) {
    relevant=true; cpHvChargeStatus=d[0]&0x07; cpEvseChargeType=d[5]&0x03;
    cpChargingActive=(cpHvChargeStatus==CP_CHARGE_ENABLED);
    updateChargingActive(); lastChargeFrameSeen=millis();
  }
  if (relevant) {
    lastAnyRelevantCanSeen=millis();
    if (!mirrorsFolded||anyDoorOpen()||anyMirrorUnfoldedOrUnfolding()) vehicleAwake=true;
  }
}

void handleChassisCAN(const twai_message_t &msg) {
  if (msg.extd||msg.rtr||msg.identifier!=ID_DAS_STATUS||msg.data_length_code<8) return;
  uint8_t b0=msg.data[0];
  autopilotState=b0&0x0F;
  autopilotActive=(autopilotState==3||autopilotState==4||autopilotState==5);
  blindLeftRaw=(b0>>4)&0x03; blindRightRaw=(b0>>6)&0x03;
  blindLeftActive=blindLeftRaw>0; blindRightActive=blindRightRaw>0;
  last399Seen=millis();
}

void checkTwaiHealth(twai_handle_t handle, int busIdx, const char* name, unsigned long* lastCheck) {
  unsigned long now=millis();
  if (now-*lastCheck<5000) return; *lastCheck=now;
  twai_status_info_t info;
  if (twai_get_status_info_v2(handle,&info)!=ESP_OK) return;
  if (info.state==TWAI_STATE_RUNNING) {
    twaiOffSinceMs[busIdx]=0;
    unsigned long lastRx=(busIdx==0)?lastAnyRelevantCanSeen:last399Seen;
    unsigned long otherRx=(busIdx==0)?last399Seen:lastAnyRelevantCanSeen;
    bool otherFresh=otherRx>0&&now-otherRx<30000;
    if (((lastRx>0&&now-lastRx>120000)||(lastRx==0&&now>120000))&&otherFresh&&
        (twaiSoftResetMs[busIdx]==0||now-twaiSoftResetMs[busIdx]>120000)) {
      twaiSoftResetMs[busIdx]=now; twai_stop_v2(handle); delay(5); twai_start_v2(handle);
    }
    return;
  }
  if (info.state==TWAI_STATE_BUS_OFF) {
    if (!twaiOffSinceMs[busIdx]) twaiOffSinceMs[busIdx]=now;
    else if (now-twaiOffSinceMs[busIdx]>=1000) { twaiOffSinceMs[busIdx]=0; twai_initiate_recovery_v2(handle); }
  } else if (info.state==TWAI_STATE_STOPPED) {
    twaiOffSinceMs[busIdx]=0; twai_start_v2(handle);
  }
}

void processCan() {
  checkTwaiHealth(vehicleTwai,0,"VehicleBus",&lastVehicleTwaiCheck);
  checkTwaiHealth(chassisTwai,1,"ChassisBus",&lastChassisTwaiCheck);
  twai_message_t msg;
  while (twai_receive_v2(vehicleTwai,&msg,0)==ESP_OK) handleVehicleCAN(msg);
  while (twai_receive_v2(chassisTwai,&msg,0)==ESP_OK) handleChassisCAN(msg);
}

void sendDashStatus() {
  DashStatusPacket pkt={};
  pkt.packetType=DASH_STATUS_PACKET_TYPE; pkt.dashMode=dashCurrentMode;
  twai_status_info_t vI={},cI={};
  pkt.vehicleBusState=(twai_get_status_info_v2(vehicleTwai,&vI)==ESP_OK)?(uint8_t)vI.state:0xFF;
  pkt.chassisBusState=(twai_get_status_info_v2(chassisTwai,&cI)==ESP_OK)?(uint8_t)cI.state:0xFF;
  pkt.vehicleCanAgeMs=(lastAnyRelevantCanSeen==0)?0xFFFFFFFF:(uint32_t)(millis()-lastAnyRelevantCanSeen);
  pkt.chassisCanAgeMs=(last399Seen==0)?0xFFFFFFFF:(uint32_t)(millis()-last399Seen);
  pkt.brightnessPct=(uint8_t)constrain((int)displayBrightnessPercent,0,100);
  pkt.gear=gear; pkt.soc=batterySocPercent; pkt.vehicleAwake=vehicleAwake;
  pkt.chargingActive=chargingActive; pkt.blinkerLeft=blinkerLeftPulse; pkt.blinkerRight=blinkerRightPulse;
  pkt.blindLeft=blindLeftActive; pkt.blindRight=blindRightActive; pkt.autopilot=autopilotActive;
  esp_now_send(broadcastAddress,(uint8_t*)&pkt,sizeof(pkt));
}

void sendVehicleData() {
  vehicleData.packetType=1;
  vehicleData.blinkerLeftPulse=blinkerLeftPulse; vehicleData.blinkerRightPulse=blinkerRightPulse;
  vehicleData.displayBrightnessRaw=displayBrightnessRaw; vehicleData.displayBrightnessPercent=displayBrightnessPercent;
  vehicleData.gear=gear; vehicleData.carInPark=carInPark;
  vehicleData.mirrorLeftState=mirrorLeftState; vehicleData.mirrorRightState=mirrorRightState;
  vehicleData.mirrorLeftFolded=mirrorLeftFolded; vehicleData.mirrorRightFolded=mirrorRightFolded;
  vehicleData.mirrorsFolded=mirrorsFolded; vehicleData.mirrorsUnfolded=mirrorsUnfolded;
  vehicleData.vehicleAwake=vehicleAwake; vehicleData.counter=vehicleSendCounter++;
  vehicleData.lastVehicleCanAgeMs=(lastAnyRelevantCanSeen==0)?0xFFFFFFFF:(uint32_t)(millis()-lastAnyRelevantCanSeen);
  vehicleData.chargingActive=chargingActive; vehicleData.chargeStatus=chargeStatus;
  vehicleData.batterySocPercent=batterySocPercent;
  vehicleData.lastChargeCanAgeMs=(lastChargeFrameSeen==0)?0xFFFFFFFF:(uint32_t)(millis()-lastChargeFrameSeen);
  vehicleData.doorFrontLeftOpen=doorFrontLeftOpen; vehicleData.doorFrontRightOpen=doorFrontRightOpen;
  vehicleData.doorRearLeftOpen=doorRearLeftOpen; vehicleData.doorRearRightOpen=doorRearRightOpen;
  vehicleData.trunkOpen=trunkOpen;
  esp_now_send(broadcastAddress,(uint8_t*)&vehicleData,sizeof(vehicleData));
}

void sendChassisData() {
  chassisData.packetType=2; chassisData.autopilotState=autopilotState;
  chassisData.autopilotActive=autopilotActive;
  chassisData.blindLeftRaw=blindLeftRaw; chassisData.blindRightRaw=blindRightRaw;
  chassisData.blindLeftActive=blindLeftActive; chassisData.blindRightActive=blindRightActive;
  chassisData.counter=chassisSendCounter++;
  esp_now_send(broadcastAddress,(uint8_t*)&chassisData,sizeof(chassisData));
}

void runFailsafes() {
  if (millis()-lastBlinkerFrameSeen>1000) { blinkerLeftPulse=false; blinkerRightPulse=false; }
  if (lastChargeFrameSeen>0&&millis()-lastChargeFrameSeen>5000) {
    chargeStatus=BMS_CHARGE_DISCONNECTED; cpChargingActive=false; cpHvChargeStatus=0; cpEvseChargeType=0;
    updateChargingActive();
  }
  bool vehicleCanStale=(lastAnyRelevantCanSeen==0)||(millis()-lastAnyRelevantCanSeen>60000);
  if (vehicleCanStale) vehicleAwake=false;
  else if (anyDoorOpen()||anyMirrorUnfoldedOrUnfolding()) vehicleAwake=true;
  else if (mirrorsFolded) vehicleAwake=false;
  else if (millis()-lastAnyRelevantCanSeen>30000) vehicleAwake=false;
  if (last399Seen>0&&millis()-last399Seen>CHASSIS_CAN_TIMEOUT_MS) {
    autopilotState=0; autopilotActive=false; blindLeftRaw=0; blindRightRaw=0;
    blindLeftActive=false; blindRightActive=false;
  }
}

void setup() {
  Serial.begin(115200); delay(800);
  Serial.println("Tesla Dash CAN-Bridge ESP32-C6");
  memset(&vehicleData,0,sizeof(vehicleData));
  memset(&chassisData,0,sizeof(chassisData));
  vehicleData.packetType=1; vehicleData.displayBrightnessRaw=0xC8;
  vehicleData.displayBrightnessPercent=100.0f; vehicleData.gear=GEAR_UNKNOWN;
  vehicleData.vehicleAwake=true; vehicleData.chargeStatus=BMS_CHARGE_DISCONNECTED;
  strip.begin(); strip.setBrightness(0); strip.clear(); strip.show();
  setupWifi(); setupEspNow(); setupWebOta(); setupCAN();
  dashOff();
}

void loop() {
  if (otaApActive) {
    server.handleClient();
    if (otaApUntilMs>0&&(int32_t)(millis()-otaApUntilMs)>=0) stopOtaAp();
  }
  processCan(); runFailsafes();
  carAwake=vehicleAwake&&!mirrorsFolded;
  bool chargeNow=chargeDashEnabled&&chargingActive&&lastChargeFrameSeen>0&&
                 millis()-lastChargeFrameSeen<5000&&chargeAnimationStartMs>0&&
                 millis()-chargeAnimationStartMs<CHARGE_ANIMATION_TIMEOUT_MS;
  if (carAwake!=lastCarAwake) {
    lastCarAwake=carAwake;
    if (carAwake) {
      goodbyeActive=false; goodbyeUntilMs=0;
      if (welcomeAnimationEnabled&&!welcomePlayedForWake) { welcomePlayedForWake=true; welcomeUntilMs=millis()+2200; }
      resetDashEffect();
    } else {
      welcomePlayedForWake=false; welcomeUntilMs=0;
      if (chargeNow) { goodbyeActive=false; goodbyeUntilMs=0; }
      else if (goodbyeAnimationEnabled) { goodbyeActive=true; goodbyeUntilMs=millis()+2200; resetDashEffect(); }
      else { goodbyeActive=false; goodbyeUntilMs=0; }
    }
  }
  refreshDash();
  if (millis()-lastVehicleSend>=50) { lastVehicleSend=millis(); sendVehicleData(); }
  if (millis()-lastChassisSend>=50) { lastChassisSend=millis(); sendChassisData(); }
  if (millis()-lastDashStatusSend>=500) { lastDashStatusSend=millis(); sendDashStatus(); }
  if (millis()-lastDebug>=1000) {
    lastDebug=millis();
    Serial.print("C6 awake="); Serial.print(vehicleAwake);
    Serial.print(" soc="); Serial.print(batterySocPercent);
    Serial.print("% AP="); Serial.print(autopilotActive);
    Serial.print(" mode="); Serial.println(dashCurrentMode);
  }
}