#pragma once
// Host-only fault injection. These stubs do not simulate motor physics or optics.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <vector>
using std::min; using std::max;
constexpr int HIGH = 1, LOW = 0, INPUT = 1, OUTPUT = 2, RISING = 3;
#define IRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
inline void portENTER_CRITICAL(int *) {} inline void portEXIT_CRITICAL(int *) {}
inline void portENTER_CRITICAL_ISR(int *) {} inline void portEXIT_CRITICAL_ISR(int *) {}
inline uint32_t clockMs = 0;
inline bool advanceMillis = false;
inline std::map<int, int> levels, adc, pwm;
inline uint32_t millis() { return advanceMillis ? clockMs++ : clockMs; }
inline void delay(uint32_t ms) { clockMs += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { levels[pin] = value; }
inline int digitalRead(int pin) { return levels[pin]; }
inline int analogRead(int pin) { return adc[pin]; }
inline void analogReadResolution(int) {}
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
inline bool ledcAttach(int, int, int) { return true; }
inline bool ledcDetach(int) { return true; }
inline bool ledcWrite(int pin, int value) { pwm[pin] = value; return true; }
template<class T> T constrain(T value, T low, T high) { return min(high, max(low, value)); }
struct FakeSerial {
  std::deque<char> commands;
  void begin(int) {}
  int available() { return commands.size(); }
  char read() { char c = commands.front(); commands.pop_front(); return c; }
  void println(const char *) { ++clockMs; } // Exercise time advancing inside logging.
  template<class... T> void printf(const char *, T...) { ++clockMs; }
};
inline FakeSerial Serial;
struct FakeWire {
  uint8_t address = 0, reg = 0;
  std::vector<uint8_t> tx;
  std::deque<uint8_t> rx;
  std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> writes;
  bool shortRead = false, nack = false;
  void begin(int, int) {} void setClock(int) {} void setTimeOut(int) {}
  void beginTransmission(uint8_t a) { address = a; tx.clear(); }
  void write(uint8_t b) { tx.push_back(b); }
  int endTransmission(bool = true) {
    if (!tx.empty()) reg = tx[0];
    if (tx.size() == 2) writes.emplace_back(address, reg, tx[1]);
    return nack ? 2 : 0;
  }
  uint8_t requestFrom(uint8_t, uint8_t count) {
    rx.clear();
    if (shortRead) return 0;
    for (uint8_t i = 0; i < count; ++i) {
      uint8_t value = 0;
      if (reg == 0x13) value = 7;
      if (reg == 0x14 && i == 0) value = 0x58;
      if (reg == 0x14 && i == 10) value = 1;
      if (reg == 0x14 && i == 11) value = 244; // 500 mm.
      rx.push_back(value);
    }
    return count;
  }
  uint8_t read() { uint8_t value = rx.front(); rx.pop_front(); return value; }
};
inline FakeWire Wire;
struct VL53L0X {
  enum { SYSRANGE_START = 0, SYSTEM_INTERRUPT_CLEAR = 0x0B,
         RESULT_INTERRUPT_STATUS = 0x13, RESULT_RANGE_STATUS = 0x14 };
  int last_status = 0;
  void setTimeout(int) {} bool init() { return true; }
  void setAddress(int) {} bool setMeasurementTimingBudget(int) { return true; }
};
struct Preferences {
  inline static std::vector<uint8_t> bytes;
  bool begin(const char *, bool) { return true; } void end() {}
  size_t getBytes(const char *, void *data, size_t size) {
    if (bytes.size() != size) return 0;
    std::memcpy(data, bytes.data(), size); return size;
  }
  size_t putBytes(const char *, const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data); bytes.assign(p, p + size); return size;
  }
};
