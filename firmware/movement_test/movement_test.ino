// BottleSumo 2026 — movement test, with Wi-Fi OTA
//
// Arduino IDE 2.x: select Tools > Board > BottleSumo 2026 (ESP32-S3 N16R8)
// (see arduino/bottlesumo-n16r8/README.md). Every flash, PSRAM, partition and
// USB setting is pinned by that board profile.
//
// The first flash of an OTA-capable build must still go over USB: it writes the
// bootloader, the otadata partition and the partition table. Every later build
// can go over Wi-Fi (see README.md).
//
// Motors stay idle until a command arrives. Wheels off the ground.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Preferences.h>

#include "balance.h"
#include "fg.h"

// ---------------------------------------------------------------------------
// Credentials and OTA identity, kept out of git.
// Copy wifi_local.h.example to wifi_local.h in this folder and fill it in.
// Without that file the sketch still builds and runs, with Wi-Fi off.
// ---------------------------------------------------------------------------
#if __has_include("wifi_local.h")
#include "wifi_local.h"
#define BS_WIFI_CONFIGURED 1
#endif

#ifdef BS_WIFI_CONFIGURED
constexpr bool HAVE_STA_CREDENTIALS = true;
#else
constexpr bool HAVE_STA_CREDENTIALS = false;
#endif

#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "bottlesumo-move"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

// ---------------------------------------------------------------------------
// Pin pairs — which pair is PWM and which is DIR is not assumed
//
// Eagle v2 netlist: PWM1 = GPIO10, DIR1 = GPIO11, PWM2 = GPIO13, DIR2 = GPIO14
// (motor connectors X1/X2, pin 4 = PWM, pin 5 = DIR). Final bench harness:
//   white  -> GPIO9/12   motor FG (input, never driven here)
//   yellow -> GPIO10/13  motor PWM
//   orange -> GPIO11/14  motor DIR
// 'c' swaps the two pairs at runtime and 'w' sweeps every combination, so the
// bench can re-check the map after a re-crimp.
// ---------------------------------------------------------------------------
constexpr uint8_t PAIR_A[2] = {11, 14};  // left, right
constexpr uint8_t PAIR_B[2] = {10, 13};  // left, right
constexpr uint8_t PIN_LED[2] = {48, 47};

// Motor listing: PWM input 15-25 kHz. 20 kHz keeps the driver out of the
// audible band and matches the native firmware.
constexpr uint32_t PWM_HZ = 20000;
constexpr uint8_t PWM_RES_BITS = 8;
constexpr uint8_t DUTY_MAX = 255;

// Which pair carries the PWM input. false = PAIR_B drives PWM (GPIO10/13, the
// yellow wires); true = PAIR_A drives PWM (GPIO11/14, the orange DIR wires).
// The earlier default assumed a crossed harness and put the 20 kHz carrier on
// the DIR wires: direction then never changed and the PWM input only saw a
// static level, so every command pivoted. The final harness is straight.
bool pwmOnPairA = false;

// Bench-measured driver behaviour: a PWM input held low ran the wheel at full
// speed, so the input is active-low and a commanded duty of 0 must still stop
// the wheel. 'p' flips this at runtime.
bool pwmActiveLow = true;

// Level each DIR pin needs for that wheel to push the robot forwards. The two
// gearboxes are mounted mirrored, so a straight command needs *opposite*
// levels: the original movement sketch drove DRIVE = (1, 0). Same level on both
// pins spins the robot in place. '1' / '2' flip one wheel, 'd' flips both.
bool dirForwardHigh[2] = {true, false};

// FG speed feedback: the listing states 9 pulses per motor revolution. The
// board's FG path (series R4/R5, pull-down R6/R7) has no documented values, so
// treat rpm as diagnostic until the divider and the FG voltage are measured.
constexpr uint8_t FG_PULSES_PER_REV = 9;
constexpr uint32_t FG_STALE_US = 400000;   // no edge for 0.4 s -> 0 rpm
constexpr uint32_t FG_SAMPLE_MS = 100;
bench::BalanceConfig balanceCfg;           // ±40 counts, 2 rpm deadband, step 1

int16_t wheelTrim[2] = {0, 0};             // duty offset from the balance loop
bench::TrimTable trimTable;                // fixed offsets, four duty points
constexpr int8_t TRIM_TABLE_LIMIT = 40;
Preferences trimPrefs;
uint16_t wheelRpm[2] = {0, 0};
uint16_t wheelHz[2] = {0, 0};
uint32_t fgRawPeriod[2] = {0, 0};          // last edge gap, unfiltered
bench::PeriodGate fgGate[2];
bool balanceOn = true;
uint32_t lastFgSampleMs = 0;

// Command duty for f/b/l/r and for the sweep.
uint8_t runDuty = 120;

// ---------------------------------------------------------------------------
// Bench access point and control page
//
// The robot publishes its own WPA2 network so a phone or laptop at the bench
// can drive it without joining the house Wi-Fi. A drive command is a lease: the
// page repeats it while a button is held, and the wheels stop when the lease
// expires, so a closed tab or a dropped client cannot leave the car running.
// ---------------------------------------------------------------------------
#ifndef AP_SSID
#define AP_SSID "BottleSumo-Bench"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "bottlesumo"   // WPA2 needs 8+ characters
#endif
constexpr uint8_t AP_CHANNEL = 6;      // fixed: a STA connect can move the AP
constexpr uint8_t AP_MAX_CLIENTS = 4;
constexpr uint32_t WEB_LEASE_MS = 400; // command repeats every 100 ms

WebServer web(80);
bool webDriveActive = false;
uint32_t lastWebCommandMs = 0;
uint8_t webClients = 0;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Drive {
  int left;   // signed duty, positive = that wheel forwards
  int right;
};

Drive drive = {0, 0};
bool otaBusy = false;
bool pwmReady = false;

enum DemoPhase : uint8_t {
  DEMO_IDLE,
  DEMO_FORWARD,
  DEMO_STOP1,
  DEMO_LEFT,
  DEMO_STOP2,
  DEMO_RIGHT,
  DEMO_STOP3,
  DEMO_BACKWARD,
  DEMO_END,
};

DemoPhase demoPhase = DEMO_IDLE;
uint32_t demoStartMs = 0;

enum SweepPhase : uint8_t { SWEEP_IDLE, SWEEP_RUN, SWEEP_PAUSE, SWEEP_END };
SweepPhase sweepPhase = SWEEP_IDLE;
uint8_t sweepStep = 0;               // bit0 = pair, bit1 = polarity
uint32_t sweepPhaseStartMs = 0;
bool pwmOnPairAAtStart = true;
bool pwmActiveLowAtStart = true;

uint32_t lastWifiAttemptMs = 0;
bool wifiWasConnected = false;
bool otaStarted = false;

// ---------------------------------------------------------------------------
// Pin roles
// ---------------------------------------------------------------------------
void stopMotion();
uint8_t pwmPin(uint8_t wheel) { return pwmOnPairA ? PAIR_A[wheel] : PAIR_B[wheel]; }
uint8_t dirPin(uint8_t wheel) { return pwmOnPairA ? PAIR_B[wheel] : PAIR_A[wheel]; }

uint8_t idleDuty() { return pwmActiveLow ? DUTY_MAX : 0; }

uint8_t rawDuty(uint8_t duty) {
  return pwmActiveLow ? uint8_t(DUTY_MAX - duty) : duty;
}

// Park every candidate pin before a role change: a PWM input left low is full
// speed on an active-low driver.
void parkAllPins() {
  for (uint8_t i = 0; i < 2; ++i) {
    pinMode(pwmPin(i), OUTPUT);
    digitalWrite(pwmPin(i), idleDuty() >= 128 ? HIGH : LOW);
    pinMode(dirPin(i), OUTPUT);
    digitalWrite(dirPin(i), LOW);
  }
}

void attachPwm() {
  pwmReady = ledcAttach(pwmPin(0), PWM_HZ, PWM_RES_BITS) &&
             ledcAttach(pwmPin(1), PWM_HZ, PWM_RES_BITS);
  if (!pwmReady) Serial.println(F("PWM attach failed; falling back to on/off"));
}

void writePwm(uint8_t wheel, uint8_t commanded) {
  if (pwmReady) {
    ledcWrite(pwmPin(wheel), rawDuty(commanded));
  } else {
    digitalWrite(pwmPin(wheel), rawDuty(commanded) >= 128 ? HIGH : LOW);
  }
}

// Swap which pair carries PWM, keeping both motors off through the change.
void setPwmPair(bool onPairA) {
  if (pwmOnPairA == onPairA) return;
  parkAllPins();
  ledcDetach(pwmPin(0));
  ledcDetach(pwmPin(1));
  pwmOnPairA = onPairA;
  parkAllPins();
  attachPwm();
  stopMotors();
}

void stopMotors() {
  drive.left = 0;
  drive.right = 0;
  wheelTrim[0] = 0;
  wheelTrim[1] = 0;
  fgGate[0].count = 0;
  fgGate[1].count = 0;
  // PWM only. Moving a DIR pin while the outputs are at rest is what turns a
  // parked robot into a spinning one when the PWM input is mis-polarised.
  writePwm(0, 0);
  writePwm(1, 0);
}

void applyOutputs() {
  if (otaBusy) {
    // An OTA write must never happen with the motors powered.
    if (drive.left != 0 || drive.right != 0) stopMotors();
    return;
  }

  const int duty[2] = {drive.left, drive.right};
  for (uint8_t i = 0; i < 2; ++i) {
    const int magnitude = abs(duty[i]);
    if (magnitude == 0) {
      // Rest the PWM input and leave DIR alone.
      writePwm(i, 0);
      continue;
    }
    const bool forwards = duty[i] > 0;
    digitalWrite(dirPin(i), (forwards == dirForwardHigh[i]) ? HIGH : LOW);
    const int16_t trim = int16_t(wheelTrim[i] + trimTable.at(i, magnitude));
    writePwm(i, uint8_t(bench::trimmedDuty(magnitude, trim)));
  }
}

// Two-motor speed match: every sample, nudge the faster wheel down and the
// slower one up until the rpm gap is inside the deadband. Without FG pulses
// both rpm stay 0 and the loop does nothing, so an unpopulated FG path cannot
// change the drive.
void sampleSpeeds(uint32_t nowMs) {
  if (nowMs - lastFgSampleMs < FG_SAMPLE_MS) return;
  lastFgSampleMs = nowMs;
  const uint32_t nowUs = micros();
  for (uint8_t i = 0; i < 2; ++i) {
    const uint32_t period = fgPeriodUs[i];
    const uint32_t last = fgLastUs[i];
    fgRawPeriod[i] = period;
    const uint32_t accepted = fgGate[i].update(period);
    const bool fresh = last != 0 && (nowUs - last) < FG_STALE_US;
    const bool valid = fresh && accepted != 0;
    wheelHz[i] = valid ? bench::hzFromPeriodUs(accepted) : 0;
    wheelRpm[i] =
        valid ? bench::rpmFromPeriodUs(accepted, FG_PULSES_PER_REV) : 0;
  }
  if (!balanceOn || drive.left == 0 || drive.right == 0) return;
  // No usable reading is a wiring fault, not a speed difference: drop any
  // offset instead of holding one learned from noise.
  if (!wheelRpm[0] || !wheelRpm[1]) {
    wheelTrim[0] = 0;
    wheelTrim[1] = 0;
    return;
  }
  bench::balanceStep(wheelTrim, wheelRpm[0], wheelRpm[1], balanceCfg);
}

void setStraight(int direction) {
  drive.left = direction * int(runDuty);
  drive.right = direction * int(runDuty);
}

void setSpin(int8_t left, int8_t right) {
  drive.left = left < 0 ? -int(runDuty) : int(runDuty);
  drive.right = right < 0 ? -int(runDuty) : int(runDuty);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
void announce() {
  Serial.printf("duty=%u crossed=%d active-low=%d dir-fwd=%d/%d balance=%d\n",
                runDuty, pwmOnPairA ? 1 : 0, pwmActiveLow ? 1 : 0,
                dirForwardHigh[0] ? 1 : 0, dirForwardHigh[1] ? 1 : 0,
                balanceOn ? 1 : 0);
  Serial.printf("balance trim L=%d R=%d  table:", wheelTrim[0], wheelTrim[1]);
  for (uint8_t i = 0; i < bench::TrimTable::N; ++i) {
    Serial.printf(" %u=%d/%d", trimTable.duty[i], trimTable.trim[0][i],
                  trimTable.trim[1][i]);
  }
  Serial.println();
  Serial.printf("FG GPIO%u/%u %u ppr  L %u Hz %u rpm trim %d  R %u Hz %u rpm trim %d\n",
                FG_PINS[0], FG_PINS[1], FG_PULSES_PER_REV, wheelHz[0], wheelRpm[0],
                wheelTrim[0], wheelHz[1], wheelRpm[1], wheelTrim[1]);
  Serial.printf("PWM GPIO%u/%u  DIR GPIO%u/%u  (%s netlist)\n", pwmPin(0),
                pwmPin(1), dirPin(0), dirPin(1),
                pwmOnPairA ? "crossed against the" : "matching the");
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  f / b       both wheels forward / backward"));
  Serial.println(F("  l / r       spin left / spin right"));
  Serial.println(F("  s or x      stop (motor outputs at rest)"));
  Serial.println(F("  + / -       run duty +/- 16"));
  Serial.println(F("  k           toggle FG speed balance (equal rpm both wheels)"));
  Serial.println(F("  < / >       table left trim -1 / +1 at the current duty"));
  Serial.println(F("  , / .       table right trim -1 / +1 at the current duty"));
  Serial.println(F("  p           flip PWM polarity (active-low <-> active-high)"));
  Serial.println(F("  c           swap the PWM pair and the DIR pair"));
  Serial.println(F("  d           flip both DIR levels (both wheels reverse)"));
  Serial.println(F("  1 / 2       flip the left / right DIR level only"));
  Serial.println(F("  w           sweep all 4 pair/polarity combinations"));
  Serial.println(F("  a           run the timed forward/left/right/backward demo"));
  Serial.println(F("  i           print status"));
  Serial.println(F("  ?           this list"));
  Serial.printf("  web         join \"%s\" and open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  Serial.println();
}

// ---------------------------------------------------------------------------
// Wi-Fi + OTA + bench control page
// ---------------------------------------------------------------------------
void handleCmd();
void handleDuty();
void handleKey();
void sendStatusJson();
void handleCommand(char c);

// The control page lives in web_page.h: PlatformIO's .ino-to-C++ converter
// hoists anything shaped like a function definition, and the page's JavaScript
// `function send(q){...}` lines were hoisted as C++ prototypes.
#include "web_page.h"

void startAp() {
  // AP+STA: the control page is always available, and the station link to the
  // house network carries OTA. A STA connect can move the AP onto the router
  // channel; clients then have to reconnect.
  WiFi.mode(HAVE_STA_CREDENTIALS ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CLIENTS);
  WiFi.softAPsetHostname(OTA_HOSTNAME);
  Serial.printf("AP: \"%s\"  open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());

  web.on("/", []() { web.send_P(200, "text/html", PAGE); });
  web.on("/cmd", handleCmd);
  web.on("/duty", handleDuty);
  web.on("/key", handleKey);
  web.on("/status", sendStatusJson);
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
  Serial.println(F("web: control page up on port 80"));
}

void handleCmd() {
  if (otaBusy || sweepPhase != SWEEP_IDLE) {
    // An OTA write or the four-combination sweep owns the outputs.
    web.send(409, "text/plain", otaBusy ? "ota" : "sweep");
    return;
  }
  const String d = web.arg("d");
  if (d == "s") {
    webDriveActive = false;
    stopMotors();
    web.send(200, "text/plain", "stopped");
    return;
  }
  demoPhase = DEMO_IDLE;
  sweepPhase = SWEEP_IDLE;
  if (d == "f") setStraight(1);
  else if (d == "b") setStraight(-1);
  else if (d == "l") setSpin(-1, 1);
  else if (d == "r") setSpin(1, -1);
  else {
    web.send(400, "text/plain", "bad d");
    return;
  }
  webDriveActive = true;
  lastWebCommandMs = millis();
  web.send(200, "text/plain", "ok");
}

void handleDuty() {
  const long v = web.arg("v").toInt();
  if (v >= 32 && v <= DUTY_MAX) runDuty = uint8_t(v);
  web.send(200, "text/plain", String(runDuty));
}

// Every bench key on the page is the same character the serial monitor takes,
// so the two paths cannot drift: `w` sweeps, `p` flips polarity, `c` swaps the
// pin pairs, `1`/`2` flip one wheel's DIR level, `t` cycles the trim, `a` runs
// the timed demo, `s` stops.
void handleKey() {
  if (otaBusy) {
    web.send(409, "text/plain", "ota");
    return;
  }
  if (web.arg("k").length() == 0) {
    web.send(400, "text/plain", "bad k");
    return;
  }
  webDriveActive = false;
  const char key = web.arg("k")[0];
  // A key press must never fight a running sweep or demo; 'w' and 'a' start
  // their own sequence.
  if (key != 'w' && key != 'a') stopMotion();
  handleCommand(key);
  web.send(200, "text/plain", "ok");
}

void sendStatusJson() {
  char buf[576];
  char sta[24];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(sta, sizeof sta, "%s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(sta, sizeof sta, "off");
  }
  snprintf(buf, sizeof buf,
           "{\"duty\":%u,\"pair\":%d,\"low\":%d,\"l\":%d,\"r\":%d,"
           "\"dirL\":%d,\"dirR\":%d,\"hzL\":%u,\"hzR\":%u,\"rpmL\":%u,"
           "\"rpmR\":%u,\"perL\":%lu,\"perR\":%lu,\"trimL\":%d,\"trimR\":%d,"
           "\"tl\":[%d,%d,%d,%d],\"tr\":[%d,%d,%d,%d],\"bal\":%d,\"sweep\":%u,\"clients\":%u,"
           "\"ota\":%d,\"pwmHz\":%lu,\"ppr\":%u,\"db\":%u,\"lim\":%d,"
           "\"sta\":\"%s\"}",
           runDuty, pwmOnPairA ? 1 : 0, pwmActiveLow ? 1 : 0, drive.left,
           drive.right, dirForwardHigh[0] ? 1 : 0, dirForwardHigh[1] ? 1 : 0,
           wheelHz[0], wheelHz[1], wheelRpm[0], wheelRpm[1],
           static_cast<unsigned long>(fgRawPeriod[0]),
           static_cast<unsigned long>(fgRawPeriod[1]), wheelTrim[0],
           wheelTrim[1], trimTable.trim[0][0], trimTable.trim[0][1],
           trimTable.trim[0][2], trimTable.trim[0][3], trimTable.trim[1][0],
           trimTable.trim[1][1], trimTable.trim[1][2], trimTable.trim[1][3],
           balanceOn ? 1 : 0,
           sweepPhase == SWEEP_IDLE ? 0U : unsigned(sweepStep + 1),
           unsigned(webClients), otaBusy ? 1 : 0,
           static_cast<unsigned long>(PWM_HZ), FG_PULSES_PER_REV,
           balanceCfg.deadbandRpm, balanceCfg.limit, sta);
  web.send(200, "application/json", buf);
}

void connectWifi() {
#ifdef BS_WIFI_CONFIGURED
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMs < 5000) return;
  lastWifiAttemptMs = millis();
  Serial.printf("Wi-Fi: joining \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_AP_STA);        // never drop the bench AP
  WiFi.setSleep(false);          // keeps the OTA link responsive
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
}

void startOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }
  ArduinoOTA.onStart([]() {
    stopMotors();
    demoPhase = DEMO_IDLE;
    sweepPhase = SWEEP_IDLE;
    otaBusy = true;
    Serial.println(F("OTA: upload starting, motors stopped and locked out"));
  });
  ArduinoOTA.onEnd([]() {
    otaBusy = false;
    Serial.println(F("OTA: upload finished, rebooting"));
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    if (total == 0) return;
    const unsigned int percent = done * 100U / total;
    if (percent % 10U == 0U) Serial.printf("OTA: %u%%\n", percent);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaBusy = false;
    Serial.printf("OTA: error %u\n", static_cast<unsigned>(error));
    if (error == OTA_AUTH_ERROR) Serial.println(F("OTA: wrong password"));
    else if (error == OTA_BEGIN_ERROR) Serial.println(F("OTA: begin failed (partition table?)"));
    else if (error == OTA_CONNECT_ERROR) Serial.println(F("OTA: connection lost"));
    else if (error == OTA_RECEIVE_ERROR) Serial.println(F("OTA: receive failed"));
    else if (error == OTA_END_ERROR) Serial.println(F("OTA: image rejected"));
  });
  ArduinoOTA.begin();
  otaStarted = true;
}

void serviceNetwork() {
#ifdef BS_WIFI_CONFIGURED
  connectWifi();
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    wifiWasConnected = true;
    Serial.printf("Wi-Fi: %s  IP %s  RSSI %d dBm\n", WIFI_SSID,
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("OTA:  %s.local port 3232, password %s\n", OTA_HOSTNAME,
                  OTA_PASSWORD[0] != '\0' ? "set" : "NOT set");
    if (!otaStarted) startOta();
  } else if (!connected && wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println(F("Wi-Fi: link down, retrying every 5 s"));
  }
  if (otaStarted) ArduinoOTA.handle();
#endif

  // The bench control page is independent of the home network.
  web.handleClient();
  webClients = WiFi.softAPgetStationNum();
  if (webDriveActive && millis() - lastWebCommandMs > WEB_LEASE_MS) {
    webDriveActive = false;
    stopMotion();
    Serial.println(F("web: drive lease expired, stopped"));
  }
}

// ---------------------------------------------------------------------------
// Sweep: try every pair/polarity combination and say which one moved a wheel.
// ---------------------------------------------------------------------------
const char *pairLabel(uint8_t step) {
  return (step & 0x1) ? "PWM GPIO11/14, DIR GPIO10/13" : "PWM GPIO10/13, DIR GPIO11/14";
}

void startSweep() {
  if (otaBusy) return;
  pwmOnPairAAtStart = pwmOnPairA;
  pwmActiveLowAtStart = pwmActiveLow;
  sweepStep = 0;
  sweepPhase = SWEEP_RUN;
  sweepPhaseStartMs = millis();
  Serial.println(F("sweep: forward at the run duty, 1.5 s per combination."));
  Serial.println(F("sweep: note which line moves the wheels before the 0.8 s pause."));
  applySweepStep();
  sweepPhaseStartMs = millis();
}

void applySweepStep() {
  setPwmPair((sweepStep & 0x1) != 0);
  pwmActiveLow = (sweepStep & 0x2) != 0;
  parkAllPins();
  stopMotors();
  Serial.printf("sweep %u/4: %s, %s\n", unsigned(sweepStep + 1), pairLabel(sweepStep),
                pwmActiveLow ? "active-low" : "active-high");
  setStraight(1);
}

void serviceSweep() {
  if (sweepPhase == SWEEP_IDLE) return;
  const uint32_t elapsed = millis() - sweepPhaseStartMs;

  switch (sweepPhase) {
    case SWEEP_RUN:
      if (elapsed >= 1500) {
        stopMotors();
        ++sweepStep;
        sweepPhase = (sweepStep >= 4) ? SWEEP_END : SWEEP_PAUSE;
        sweepPhaseStartMs = millis();
      }
      break;
    case SWEEP_PAUSE:
      if (elapsed >= 800) {
        Serial.println(F("sweep: stopped"));
        sweepPhase = SWEEP_RUN;
        applySweepStep();
        sweepPhaseStartMs = millis();
      }
      break;
    case SWEEP_END:
      stopMotors();
      pwmActiveLow = pwmActiveLowAtStart;
      setPwmPair(pwmOnPairAAtStart);
      parkAllPins();
      stopMotors();
      sweepPhase = SWEEP_IDLE;
      Serial.println(F("sweep complete; previous settings restored."));
      Serial.println(F("Apply the working combination with 'c' and 'p'."));
      announce();
      break;
    case SWEEP_IDLE:
      break;
  }
}

// ---------------------------------------------------------------------------
// Serial commands
// ---------------------------------------------------------------------------
// A fixed bench offset per wheel. The balance loop is idle without FG, so this
// is the fallback that replaces the old hard-coded -6: adjust it by watching
// the wheels, and it stays until the next reboot or 'k' reset.
void saveTrimTable() {
  trimPrefs.begin("movetrim", false);
  trimPrefs.putBytes("tab", trimTable.trim, sizeof trimTable.trim);
  trimPrefs.end();
}

void loadTrimTable() {
  trimPrefs.begin("movetrim", true);
  trimPrefs.getBytes("tab", trimTable.trim, sizeof trimTable.trim);
  trimPrefs.end();
  for (uint8_t w = 0; w < 2; ++w) {
    for (uint8_t k = 0; k < bench::TrimTable::N; ++k) {
      if (trimTable.trim[w][k] > TRIM_TABLE_LIMIT) trimTable.trim[w][k] = TRIM_TABLE_LIMIT;
      if (trimTable.trim[w][k] < -TRIM_TABLE_LIMIT) trimTable.trim[w][k] = -TRIM_TABLE_LIMIT;
    }
  }
}

// Any stop must also leave the sweep's pin mapping behind. Stopping mid-sweep
// used to keep the pair and polarity of the last step, so the next command
// drove with a mapping nobody chose.
void stopMotion() {
  if (sweepPhase != SWEEP_IDLE) {
    pwmActiveLow = pwmActiveLowAtStart;
    setPwmPair(pwmOnPairAAtStart);
    parkAllPins();
    sweepPhase = SWEEP_IDLE;
  }
  demoPhase = DEMO_IDLE;
  stopMotors();
}

// '<' '>' trim the left wheel, ',' '.' the right one, at the table point
// nearest the current duty. The values persist in NVS.
void applyManualTrim(char key) {
  const uint8_t wheel = (key == '<' || key == '>') ? 0 : 1;
  const bool up = (key == '>' || key == '.');
  const uint8_t point = trimTable.pointFor(runDuty);
  int next = int(trimTable.trim[wheel][point]) + (up ? 1 : -1);
  if (next > TRIM_TABLE_LIMIT) next = TRIM_TABLE_LIMIT;
  if (next < -TRIM_TABLE_LIMIT) next = -TRIM_TABLE_LIMIT;
  trimTable.trim[wheel][point] = int8_t(next);
  saveTrimTable();
  Serial.printf("table duty %u: L=%d R=%d\n", trimTable.duty[point],
                trimTable.trim[0][point], trimTable.trim[1][point]);
  announce();
}

void handleCommand(char c) {
  switch (c) {
    case 'f': setStraight(1); announce(); Serial.println(F("forward")); break;
    case 'b': setStraight(-1); announce(); Serial.println(F("backward")); break;
    case 'l': setSpin(-1, 1); announce(); Serial.println(F("spin left")); break;
    case 'r': setSpin(1, -1); announce(); Serial.println(F("spin right")); break;
    case 's':
    case 'x':
      stopMotion();
      Serial.println(F("stop"));
      break;
    case '+':
      runDuty = runDuty > DUTY_MAX - 16 ? DUTY_MAX : uint8_t(runDuty + 16);
      announce();
      break;
    case '-':
      runDuty = runDuty < 32 ? 32 : uint8_t(runDuty - 16);
      announce();
      break;
    case '<':
    case '>':
    case ',':
    case '.':
      applyManualTrim(c);
      break;
    case 'k':
      balanceOn = !balanceOn;
      if (!balanceOn) {
        wheelTrim[0] = wheelTrim[1] = 0;
      }
      Serial.printf("FG balance %s\n", balanceOn ? "on" : "off");
      announce();
      break;
    case 'p':
      stopMotors();
      pwmActiveLow = !pwmActiveLow;
      parkAllPins();
      Serial.printf("PWM now %s; outputs parked\n",
                    pwmActiveLow ? "active-low" : "active-high");
      announce();
      break;
    case 'c':
      stopMotors();
      setPwmPair(!pwmOnPairA);
      Serial.printf("PWM pair now GPIO%u/%u\n", pwmPin(0), pwmPin(1));
      announce();
      break;
    case 'd':
      stopMotors();
      dirForwardHigh[0] = !dirForwardHigh[0];
      dirForwardHigh[1] = !dirForwardHigh[1];
      announce();
      break;
    case '1':
      stopMotors();
      dirForwardHigh[0] = !dirForwardHigh[0];
      announce();
      break;
    case '2':
      stopMotors();
      dirForwardHigh[1] = !dirForwardHigh[1];
      announce();
      break;
    case 'w':
      startSweep();
      break;
    case 'a':
      demoPhase = DEMO_FORWARD;
      demoStartMs = millis();
      Serial.println(F("demo: forward/left/right/backward, 1 s each"));
      break;
    case 'i':
      announce();
      Serial.printf("drive L=%d R=%d  demo=%u  sweep=%u  ota busy=%d\n", drive.left,
                    drive.right, static_cast<unsigned>(demoPhase),
                    static_cast<unsigned>(sweepPhase), otaBusy ? 1 : 0);
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("IP %s  RSSI %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
      } else {
        Serial.println(F("Wi-Fi: not connected"));
      }
      break;
    case '\r':
    case '\n':
    case ' ':
      break;
    default:
      printHelp();
      break;
  }
}

void serviceSerial() {
  while (Serial.available() > 0) handleCommand(char(Serial.read()));
}

// ---------------------------------------------------------------------------
// Demo: timed phases, advanced from loop() so OTA keeps being serviced.
// ---------------------------------------------------------------------------
void serviceDemo() {
  if (demoPhase == DEMO_IDLE) return;
  const uint32_t elapsed = millis() - demoStartMs;

  switch (demoPhase) {
    case DEMO_FORWARD:
      if (elapsed == 0) { setStraight(1); Serial.println(F("demo: forward")); }
      if (elapsed >= 1000) { stopMotors(); demoPhase = DEMO_STOP1; demoStartMs = millis(); }
      break;
    case DEMO_STOP1:
      if (elapsed >= 500) { setSpin(-1, 1); demoPhase = DEMO_LEFT; demoStartMs = millis(); Serial.println(F("demo: left")); }
      break;
    case DEMO_LEFT:
      if (elapsed >= 1000) { stopMotors(); demoPhase = DEMO_STOP2; demoStartMs = millis(); }
      break;
    case DEMO_STOP2:
      if (elapsed >= 500) { setSpin(1, -1); demoPhase = DEMO_RIGHT; demoStartMs = millis(); Serial.println(F("demo: right")); }
      break;
    case DEMO_RIGHT:
      if (elapsed >= 1000) { stopMotors(); demoPhase = DEMO_STOP3; demoStartMs = millis(); }
      break;
    case DEMO_STOP3:
      if (elapsed >= 500) { setStraight(-1); demoPhase = DEMO_BACKWARD; demoStartMs = millis(); Serial.println(F("demo: backward")); }
      break;
    case DEMO_BACKWARD:
      if (elapsed >= 1000) { stopMotors(); demoPhase = DEMO_END; Serial.println(F("demo: complete, stopped")); }
      break;
    case DEMO_END:
    case DEMO_IDLE:
      demoPhase = DEMO_IDLE;
      break;
  }
}

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < 2; ++i) { pinMode(PIN_LED[i], OUTPUT); digitalWrite(PIN_LED[i], LOW); }

  parkAllPins();

  fgBegin();

  const uint32_t waitStart = millis();
  while (!Serial && millis() - waitStart < 3000) delay(10);

  Serial.println();
  Serial.println(F("BottleSumo 2026 — movement test"));
  attachPwm();
  Serial.printf("PWM %lu Hz, %u-bit, attached=%d\n",
                static_cast<unsigned long>(PWM_HZ), PWM_RES_BITS, pwmReady ? 1 : 0);
  stopMotors();

#ifdef BS_WIFI_CONFIGURED
  Serial.printf("Wi-Fi credentials loaded for \"%s\"\n", WIFI_SSID);
#else
  Serial.println(F("No wifi_local.h: no station link, so no OTA."));
  Serial.println(F("The bench access point and control page still run."));
  Serial.println(F("Copy wifi_local.h.example to wifi_local.h and fill it in."));
#endif

  loadTrimTable();
  startAp();
  announce();
  printHelp();
}

void loop() {
  serviceNetwork();
  serviceSerial();
  serviceSweep();
  serviceDemo();
  sampleSpeeds(millis());
  applyOutputs();
}
