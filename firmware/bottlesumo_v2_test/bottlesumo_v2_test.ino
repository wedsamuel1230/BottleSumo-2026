/*
 * BottleSumo v2 - ESP32-S3-DevKitC-1 electrical/function test
 * Target: ESP32-S3-WROOM-2
 *
 * Required Arduino IDE library:
 *   VL53L0X by Pololu
 *
 * Safety:
 *   - All motor outputs start at PWM=0.
 *   - The motor test is never started automatically. Send 'm', then 'y'.
 *   - Remove the mechanical load / lift the wheels before running the motor
 *     test. Keep the motor-driver 12 V side away from ESP32 GPIOs.
 *   - GPIO18 is the VL53L0X GPIO/interrupt input. XSHUT is fixed to 3V3 in
 *     the schematic, so this sketch does not drive XSHUT.
 */

#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>

// ------------------------- Pin map from bottlesumo_v2_doc.txt ----------------
constexpr uint8_t QRE1_PIN = 1;
constexpr uint8_t QRE2_PIN = 2;
constexpr uint8_t QRE3_PIN = 4;
constexpr uint8_t QRE4_PIN = 5;

constexpr uint8_t MOTOR1_SIGNAL_PIN = 9;
constexpr uint8_t MOTOR1_PWM_PIN = 10;
constexpr uint8_t MOTOR1_DIR_PIN = 11;
constexpr uint8_t MOTOR2_SIGNAL_PIN = 12;
constexpr uint8_t MOTOR2_PWM_PIN = 13;
constexpr uint8_t MOTOR2_DIR_PIN = 14;

constexpr uint8_t I2C_SCL_PIN = 16;
constexpr uint8_t I2C_SDA_PIN = 17;
constexpr uint8_t VL53L0X_GPIO_PIN = 18;

constexpr uint8_t STATUS_LED_PIN = 40;
constexpr uint8_t BUTTON1_PIN = 41;
constexpr uint8_t BUTTON2_PIN = 42;

constexpr uint8_t QRE_PINS[] = {QRE1_PIN, QRE2_PIN, QRE3_PIN, QRE4_PIN};
constexpr uint8_t QRE_COUNT = sizeof(QRE_PINS) / sizeof(QRE_PINS[0]);

constexpr uint8_t TOF_I2C_ADDRESS = 0x29;
constexpr uint8_t PWM_MAX = 255;
constexpr uint8_t MOTOR_TEST_DUTY = 255;       // 25% of 8-bit PWM
constexpr uint32_t LIVE_REPORT_PERIOD_MS = 2000;

VL53L0X tof;
bool tofReady = false;
bool motorTestPending = false;
uint32_t lastLiveReportMs = 0;

volatile uint32_t motor1Edges = 0;
volatile uint32_t motor2Edges = 0;

void IRAM_ATTR motor1EdgeISR() {
  ++motor1Edges;
}

void IRAM_ATTR motor2EdgeISR() {
  ++motor2Edges;
}

void printHelp();
void scanI2C();
void initTof();
void printTofReading();
void printQreReadings();
void printButtonReadings();
void printMotorSignals(bool resetEdgeCounters);
void ledSelfTest();
void stopAllMotors();
void runMotorTest();

void setup() {
  Serial.begin(115200);

  // USB CDC may take a moment to enumerate on the ESP32-S3.
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("BottleSumo v2 - ESP32-S3 electrical/function test"));
  Serial.println(F("Target: ESP32-S3-WROOM-2 / ESP32-DevKitC-1"));
  Serial.println(F("Motor outputs are disabled at startup."));

  // QRE1113 modules: 3V3/GND/OUT. INPUT keeps the external circuit visible.
  for (uint8_t i = 0; i < QRE_COUNT; ++i) {
    pinMode(QRE_PINS[i], INPUT);
  }

  // Motor signal pins are inputs from the motor controller/feedback circuit.
  pinMode(MOTOR1_SIGNAL_PIN, INPUT);
  pinMode(MOTOR2_SIGNAL_PIN, INPUT);

  // PWM and DIR are logic-level outputs to the motor controller.
  pinMode(MOTOR1_PWM_PIN, OUTPUT);
  pinMode(MOTOR1_DIR_PIN, OUTPUT);
  pinMode(MOTOR2_PWM_PIN, OUTPUT);
  pinMode(MOTOR2_DIR_PIN, OUTPUT);
  stopAllMotors();

  // External pull-down resistors are present in the schematic.
  pinMode(BUTTON1_PIN, INPUT);
  pinMode(BUTTON2_PIN, INPUT);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  // GPIO18 is the VL53L0X GPIO/interrupt line. XSHUT is hard-wired to 3V3.
  pinMode(VL53L0X_GPIO_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(MOTOR1_SIGNAL_PIN), motor1EdgeISR, RISING);
  attachInterrupt(digitalPinToInterrupt(MOTOR2_SIGNAL_PIN), motor2EdgeISR, RISING);

  // Wire.begin() takes SDA first and SCL second.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  scanI2C();
  initTof();
  ledSelfTest();
  printQreReadings();
  printButtonReadings();
  printMotorSignals(false);
  printHelp();
}

void loop() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == '\r' || command == '\n' || command == ' ') {
      continue;
    }

    switch (command) {
      case '?':
        printHelp();
        break;
      case 'q':
        printQreReadings();
        break;
      case 'b':
        printButtonReadings();
        break;
      case 'i':
        scanI2C();
        printTofReading();
        break;
      case 't':
        printTofReading();
        break;
      case 's':
        printMotorSignals(true);
        break;
      case 'l':
        ledSelfTest();
        break;
      case 'm':
        motorTestPending = true;
        Serial.println(F("Motor test armed. Check the wheels are lifted, then send 'y'."));
        Serial.println(F("Send 'x' to cancel."));
        break;
      case 'y':
        if (motorTestPending) {
          runMotorTest();
        } else {
          Serial.println(F("No motor test is armed. Send 'm' first."));
        }
        break;
      case 'x':
        motorTestPending = false;
        stopAllMotors();
        Serial.println(F("Motor test cancelled; motor outputs stopped."));
        break;
      default:
        Serial.println(F("Unknown command. Send '?' for help."));
        break;
    }
  }

  if (millis() - lastLiveReportMs >= LIVE_REPORT_PERIOD_MS) {
    lastLiveReportMs = millis();
    Serial.printf("[LIVE] BTN1=%d BTN2=%d VL53_GPIO18=%d | ",
                  digitalRead(BUTTON1_PIN), digitalRead(BUTTON2_PIN),
                  digitalRead(VL53L0X_GPIO_PIN));
    printMotorSignals(false);
    printTofReading();
  }
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands (send in Serial Monitor at 115200 baud):"));
  Serial.println(F("  ?  show this help"));
  Serial.println(F("  q  read all QRE1113 inputs as digital + ADC"));
  Serial.println(F("  b  read BUTTON1/2; HIGH means pressed"));
  Serial.println(F("  i  scan I2C and read VL53L0X"));
  Serial.println(F("  t  read one VL53L0X distance"));
  Serial.println(F("  s  show motor signal levels and edge counters"));
  Serial.println(F("  l  blink GPIO40 status LED"));
  Serial.println(F("  m  arm the motor test; y starts it, x cancels/stops"));
  Serial.println();
}

void scanI2C() {
  uint8_t deviceCount = 0;
  Serial.println(F("I2C scan on SDA=GPIO17, SCL=GPIO16:"));

  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("  found 0x%02X", address);
      if (address == TOF_I2C_ADDRESS) {
        Serial.print(F(" (expected VL53L0X)"));
      }
      Serial.println();
      ++deviceCount;
    }
  }

  if (deviceCount == 0) {
    Serial.println(F("  no I2C device found; check 3V3, GND, SDA, and SCL."));
  } else {
    Serial.printf("  total devices: %u\n", deviceCount);
  }
}

void initTof() {
  tof.setTimeout(250);

  Wire.beginTransmission(TOF_I2C_ADDRESS);
  const uint8_t addressCheck = Wire.endTransmission();
  if (addressCheck != 0) {
    Serial.println(F("VL53L0X not detected at 0x29; distance test unavailable."));
    tofReady = false;
    return;
  }

  if (!tof.init()) {
    Serial.println(F("VL53L0X init failed; check module power and I2C wiring."));
    tofReady = false;
    return;
  }

  // A moderate timing budget gives a stable test reading without being slow.
  tof.setMeasurementTimingBudget(33000);
  tof.startContinuous(100);
  tofReady = true;
  Serial.println(F("VL53L0X initialized; continuous ranging started."));
}

void printTofReading() {
  if (!tofReady) {
    Serial.println(F("VL53L0X: NOT READY"));
    return;
  }

  const uint16_t distanceMm = tof.readRangeContinuousMillimeters();
  if (tof.timeoutOccurred()) {
    Serial.println(F("VL53L0X: TIMEOUT"));
    return;
  }

  Serial.printf("VL53L0X: %u mm, GPIO18=%d\n", distanceMm,
                digitalRead(VL53L0X_GPIO_PIN));
}

void printQreReadings() {
  Serial.println(F("QRE1113 readings (digital HIGH/LOW plus raw ADC 0..4095):"));
  for (uint8_t i = 0; i < QRE_COUNT; ++i) {
    const int digitalValue = digitalRead(QRE_PINS[i]);
    const int analogValue = analogRead(QRE_PINS[i]);
    Serial.printf("  QRE%u GPIO%u: digital=%d, analog=%d\n", i + 1,
                  QRE_PINS[i], digitalValue, analogValue);
  }
}

void printButtonReadings() {
  Serial.printf("Buttons: BUTTON1 GPIO41=%s, BUTTON2 GPIO42=%s (HIGH=pressed)\n",
                digitalRead(BUTTON1_PIN) ? "PRESSED" : "released",
                digitalRead(BUTTON2_PIN) ? "PRESSED" : "released");
}

void printMotorSignals(bool resetEdgeCounters) {
  if (resetEdgeCounters) {
    noInterrupts();
    motor1Edges = 0;
    motor2Edges = 0;
    interrupts();
  }

  uint32_t motor1EdgeSnapshot;
  uint32_t motor2EdgeSnapshot;
  noInterrupts();
  motor1EdgeSnapshot = motor1Edges;
  motor2EdgeSnapshot = motor2Edges;
  interrupts();

  Serial.printf("Motor signals: SIG1 GPIO9=%d edges=%lu, SIG2 GPIO12=%d edges=%lu\n",
                digitalRead(MOTOR1_SIGNAL_PIN),
                static_cast<unsigned long>(motor1EdgeSnapshot),
                digitalRead(MOTOR2_SIGNAL_PIN),
                static_cast<unsigned long>(motor2EdgeSnapshot));
}

void ledSelfTest() {
  Serial.println(F("LED test on GPIO40..."));
  for (uint8_t i = 0; i < 3; ++i) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(150);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(150);
  }
}

void stopAllMotors() {
  analogWrite(MOTOR1_PWM_PIN, 0);
  analogWrite(MOTOR2_PWM_PIN, 0);
  digitalWrite(MOTOR1_DIR_PIN, LOW);
  digitalWrite(MOTOR2_DIR_PIN, LOW);
}

void setMotor(uint8_t pwmPin, uint8_t dirPin, bool forward, uint8_t duty) {
  digitalWrite(dirPin, forward ? HIGH : LOW);
  analogWrite(pwmPin, duty);
  Serial.printf("  GPIO%u DIR=%d, GPIO%u PWM duty=%u, output level=%d\n",
                dirPin, forward ? HIGH : LOW, pwmPin, duty,
                digitalRead(pwmPin));
}

bool runMotorPhase(const __FlashStringHelper* label, uint8_t pwmPin,
                   uint8_t dirPin, bool forward, uint32_t durationMs) {
  Serial.print(label);
  Serial.println(F("..."));
  noInterrupts();
  motor1Edges = 0;
  motor2Edges = 0;
  interrupts();

  setMotor(pwmPin, dirPin, forward, MOTOR_TEST_DUTY);
  const uint32_t phaseStart = millis();
  while (millis() - phaseStart < durationMs) {
    if (Serial.available() > 0 && Serial.read() == 'x') {
      stopAllMotors();
      Serial.println(F("Emergency stop received."));
      return false;
    }
    delay(5);
  }

  stopAllMotors();
  delay(250);
  printMotorSignals(false);
  return true;
}

void runMotorTest() {
  motorTestPending = false;
  Serial.println(F("Motor test starting: 25% PWM, 5000 ms per direction."));
  Serial.println(F("Send 'x' at any time for an emergency stop."));

  if (!runMotorPhase(F("Motor 1 forward"), MOTOR1_PWM_PIN, MOTOR1_DIR_PIN,
                     true, 5000)) {
    return;
  }
  if (!runMotorPhase(F("Motor 1 reverse"), MOTOR1_PWM_PIN, MOTOR1_DIR_PIN,
                     false, 5000)) {
    return;
  }
  if (!runMotorPhase(F("Motor 2 forward"), MOTOR2_PWM_PIN, MOTOR2_DIR_PIN,
                     true, 5000)) {
    return;
  }
  if (!runMotorPhase(F("Motor 2 reverse"), MOTOR2_PWM_PIN, MOTOR2_DIR_PIN,
                     false, 5000)) {
    return;
  }

  stopAllMotors();
  Serial.println(F("Motor test complete; all motor outputs stopped."));
}
