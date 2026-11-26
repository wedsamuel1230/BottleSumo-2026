#pragma once
#include <stdint.h>

namespace BottleSumo {
constexpr uint32_t START_DELAY_MS = 1000;
constexpr uint32_t ROUND_MS = 120000;
constexpr uint32_t RANGE_STALE_MS = 250;
constexpr uint32_t SURVIVE_MS = 3500;
enum class State { Stopped, Starting, Search, Approach, Push, Escape, Turn, Hold, Unstick };
struct Range { uint16_t mm = 0; uint32_t sampledMs = 0; };
struct Drive { int16_t left = 0; int16_t right = 0; };
struct Controller {
  State state = State::Stopped;
  uint32_t roundStart = 0, stateStart = 0, clearStart = 0;
  bool clearing = false, holdAfterEscape = false;
  int8_t escapeDirection = 0, turnDirection = 1;
  void start(uint32_t now) { roundStart = stateStart = now; state = State::Starting; }
  void stop() { state = State::Stopped; }
  Drive tick(uint32_t now, uint8_t edges, const Range (&r)[3], bool emergency = false) {
    if (state == State::Stopped) return {};
    if (emergency || now - roundStart >= ROUND_MS) { stop(); return {}; }
    if (state == State::Starting && now - roundStart < START_DELAY_MS) return {};
    for (const auto &range : r) {
      if (now - range.sampledMs > RANGE_STALE_MS) { stop(); return {}; }
    }
    const bool front = edges & 3, rear = edges & 12;
    if (front && rear) { stop(); return {}; }
    if (edges) {
      const int8_t direction = front ? -1 : 1;
      if (state != State::Escape) {
        holdAfterEscape = state == State::Push || state == State::Hold;
        escapeDirection = direction;
        turnDirection = (edges & 5) ? 1 : -1;
        state = State::Escape; stateStart = now;
      } else if (direction != escapeDirection) { stop(); return {}; }
      clearing = false;
    }
    if (state == State::Escape) {
      if (now - stateStart >= 300) { stop(); return {}; }
      if (!edges) {
        if (!clearing) { clearStart = now; clearing = true; }
        if (now - clearStart >= 40) {
          state = holdAfterEscape ? State::Hold : State::Turn;
          stateStart = now;
          return {};
        }
      }
      return {static_cast<int16_t>(escapeDirection * 105),
              static_cast<int16_t>(escapeDirection * 105)};
    }
    if (state == State::Hold) {
      if (now - stateStart < SURVIVE_MS) return {};
      state = State::Search;
    }
    if (state == State::Unstick) {
      if (now - stateStart < 150) return {-100, -100};
      state = State::Turn; stateStart = now;
    }
    if (state == State::Turn) {
      if (now - stateStart < 250)
        return {static_cast<int16_t>(turnDirection * 85),
                static_cast<int16_t>(-turnDirection * 85)};
      state = State::Search;
    }
    if (state == State::Push && !r[1].mm) {
      state = State::Hold; stateStart = now; return {};
    }
    if (state == State::Push && now - stateStart >= 6000) {
      state = State::Unstick; stateStart = now; return {-100, -100};
    }
    if (r[1].mm) {
      const bool close = r[1].mm <= 150;
      if (close && state != State::Push) stateStart = now;
      state = close ? State::Push : State::Approach;
      int16_t steer = 0;
      if (r[0].mm && r[0].mm + 60 < r[1].mm) steer = -25;
      if (r[2].mm && r[2].mm + 60 < r[1].mm) steer = 25;
      const int16_t speed = close ? 175 : (r[1].mm < 300 ? 115 : 145);
      return {static_cast<int16_t>(speed + steer), static_cast<int16_t>(speed - steer)};
    }
    state = State::Search;
    if (r[0].mm || r[2].mm) {
      const bool left = r[0].mm && (!r[2].mm || r[0].mm <= r[2].mm);
      return left ? Drive{-80, 80} : Drive{80, -80};
    }
    const uint32_t elapsed = now - roundStart - START_DELAY_MS;
    const int16_t direction = (elapsed / 1000) % 2 ? -1 : 1;
    const uint32_t phase = elapsed % 1000;
    if (phase < 320)
      return {static_cast<int16_t>(-direction * 80), static_cast<int16_t>(direction * 80)};
    if (phase < 500 || phase >= 800) return {}; // Observe a stationary target between short moves.
    return {100, 100};
  }
};

struct Calibration {
  uint16_t floorMin[4] = {}, floorMax[4] = {}, edgeMin[4] = {}, edgeMax[4] = {};
  bool valid() const {
    for (uint8_t i = 0; i < 4; ++i) {
      if (floorMin[i] > floorMax[i] || edgeMin[i] > edgeMax[i] ||
          floorMax[i] > 4095 || edgeMax[i] > 4095) return false;
      const int gap = edgeMin[i] > floorMax[i] ? edgeMin[i] - floorMax[i]
                                             : floorMin[i] - edgeMax[i];
      if (gap < 300) return false;
    }
    return true;
  }
  bool edge(uint8_t i, uint16_t adc, bool previous) const {
    const bool high = edgeMin[i] > floorMax[i];
    const int threshold = high ? (edgeMin[i] + floorMax[i]) / 2
                               : (floorMin[i] + edgeMax[i]) / 2;
    return high ? adc > threshold + (previous ? -40 : 40)
                : adc < threshold + (previous ? 40 : -40);
  }
};
}  // namespace BottleSumo
