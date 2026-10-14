#include <assert.h>
#include <stdio.h>
#ifndef ADAPTER_SKETCH
#define ADAPTER_SKETCH "../firmware/bottlesumo_competition/bottlesumo_competition.ino"
#endif
#include ADAPTER_SKETCH

int main() {
  clockMs = 100; setup();
  assert(!hardwareFault && pwm[10] == 0 && pwm[13] == 0);
  for (int pin : QRE_PINS) adc[pin] = 700;
  advanceMillis = true; Serial.commands.push_back('f'); loop(); advanceMillis = false;
  assert(calibrationMode == 'f'); // Logging advanced millis; capture must not finish early.
  for (int i = 0; i < 601; ++i) { clockMs += 5; loop(); }
  assert(floorCaptured && !calibrationMode && !calibrated);
  for (int pin : QRE_PINS) adc[pin] = 3200;
  Serial.commands.push_back('e'); loop(); assert(calibrationMode == 'e');
  for (int i = 0; i < 601; ++i) { clockMs += 5; loop(); }
  assert(calibrated && !Preferences::bytes.empty());

  // Default hardware guard must block even a calibrated, fresh, edge-free start.
  for (int pin : QRE_PINS) adc[pin] = 700;
  levels[BUTTON1_PIN] = HIGH;
  for (int i = 0; i < 10; ++i) {
    clockMs += 10;
    for (auto &r : ranges) r = {100, clockMs};
    loop();
  }
  assert(controller.state == State::Stopped && pwm[10] == 0 && pwm[13] == 0);

  // Reversing a running wheel writes zero before DIR, and waits a full 25 ms.
  clockMs = 10000; motors[0] = {}; motors[0].direction = 1;
  motors[0].duty = 120; pwm[10] = 120; levels[11] = HIGH;
  assert(setWheel(0, -100, clockMs)); assert(pwm[10] == 0 && levels[11] == HIGH);
  assert(setWheel(0, -100, clockMs + 24)); assert(pwm[10] == 0 && levels[11] == HIGH);
  assert(setWheel(0, -100, clockMs + 25)); assert(pwm[10] == 12 && levels[11] == LOW);

  // One completed single-shot precedes any startup at the next address.
  Wire.writes.clear(); tofIndex = 0; tofBusy = false; lastTofPoll = 0;
  serviceTof(10000); assert(tofBusy && tofStarting && tofIndex == 0);
  serviceTof(10002); assert(!tofStarting && tofIndex == 0);
  serviceTof(10004); assert(!tofBusy && tofIndex == 1);
  for (const auto &w : Wire.writes) {
    assert(std::get<0>(w) == 0x30);
    assert(!(std::get<1>(w) == 0 && std::get<2>(w) == 2)); // Never back-to-back mode.
  }
  serviceTof(10006); assert(std::get<0>(Wire.writes.back()) == 0x31);
  Wire.shortRead = true; controller.start(10000); pwm[10] = pwm[13] = 100;
  serviceTof(10008);
  assert(hardwareFault && controller.state == State::Stopped && pwm[10] == 0 && pwm[13] == 0);
  Wire.shortRead = false;

  // A held physical stop has priority over active strategy and a start button.
  controller.start(clockMs); levels[BUTTON2_PIN] = HIGH; pwm[10] = pwm[13] = 100;
  loop(); assert(controller.state == State::Stopped && pwm[10] == 0 && pwm[13] == 0);
  puts("competition adapter assertions passed (host stubs; no hardware proof)");
}
