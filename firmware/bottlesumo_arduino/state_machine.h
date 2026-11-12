// state_machine.h — the whole decision logic in one explicit, logged switch.
//
// HOW TO EDIT
//   1. Add or rename a state in `enum class State` and in `stateName()`.
//   2. Give it exactly one `case` in `Machine::tick()`.
//   3. Move between states only through `change()`, which appends to the
//      history ring buffer. Nothing changes state silently.
//   4. Drive levels live in `Tuning`; they are runtime-editable from the web
//      UI and survive a reboot. The numbers in `Tuning{}` are defaults only.
//
// PRIORITY ORDER (highest first), implemented in tick():
//   emergency / round timeout  >  stale sensor data  >  edge escape
//   >  per-state behaviour.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

namespace BS {

// ---------------------------------------------------------------------------
// States. Values are stable: they appear in telemetry and the web UI.
// ---------------------------------------------------------------------------
enum class State : uint8_t {
  Stopped = 0,
  Starting = 1,
  Search   = 2,
  Approach = 3,
  Push     = 4,
  Escape   = 5,
  Turn     = 6,
  Hold     = 7,
  Unstick  = 8,
  // 9..24 are reserved for display-only codes (search phases, bench,
  // calibration, OTA). Opening/Carry/Fault/Updating keep values outside that
  // range so a state can never be mistaken for a display code.
  Opening  = 26,  // scripted opening run: straight, 60 deg, straight
  Carry    = 27,  // slow arc while pushing: drag the bottle away
  Updating = 30,
  Fault    = 31,
};

// Telemetry display state. States 0..8 map directly; Updating reuses the
// IDF's display code 24 and Fault uses 25.
inline uint8_t stateDisplay(State s) {
  switch (s) {
    case State::Updating: return DISPLAY_UPDATING;
    case State::Fault:    return 25;
    default:              return uint8_t(s);
  }
}

inline const char *stateName(State s) {
  switch (s) {
    case State::Stopped:  return "stopped";
    case State::Starting: return "starting";
    case State::Search:   return "search";
    case State::Approach: return "approach";
    case State::Push:     return "push";
    case State::Escape:   return "escape";
    case State::Turn:     return "turn";
    case State::Hold:     return "hold";
    case State::Unstick:  return "unstick";
    case State::Opening:  return "opening";
    case State::Carry:    return "carry";
    case State::Fault:    return "fault";
    case State::Updating: return "updating";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Transition reasons. One per row in the history log, so a match can be
// replayed after the fact.
// ---------------------------------------------------------------------------
enum class Reason : uint8_t {
  None = 0,
  ButtonStart, SerialStart, WebStart,
  ButtonStop, SerialStop, WebStop,
  StartDelayDone, RoundTimeUp, Emergency,
  EdgeSeen, EdgeCleared, EscapeTimeout, EscapeReverseDir,
  LostCenterTarget, PushTimeUp, HoldDone, UnstickDone, TurnDone,
  TargetSeen, TargetClose, NoTarget,
  StaleData, SensorFault, FaultCleared,
  CalibrationDone, CalibrationFailed, CalibrationCancelled,
  OtaStart, OtaDone, OtaFailed,
  OpeningDone, OpeningAbort, CarryStart, CarryDone, BottleLost,
  EscapeBudget,
};

inline const char *reasonName(Reason r) {
  switch (r) {
    case Reason::None:                 return "-";
    case Reason::ButtonStart:          return "button start";
    case Reason::SerialStart:          return "serial start";
    case Reason::WebStart:             return "web start";
    case Reason::ButtonStop:           return "button stop";
    case Reason::SerialStop:           return "serial stop";
    case Reason::WebStop:              return "web stop";
    case Reason::StartDelayDone:       return "start delay done";
    case Reason::RoundTimeUp:          return "round time up";
    case Reason::Emergency:            return "emergency stop";
    case Reason::EdgeSeen:             return "edge seen";
    case Reason::EdgeCleared:          return "edge cleared";
    case Reason::EscapeTimeout:        return "escape timeout";
    case Reason::EscapeReverseDir:     return "escape reverse direction";
    case Reason::LostCenterTarget:     return "lost center target";
    case Reason::PushTimeUp:           return "push time up";
    case Reason::HoldDone:             return "hold done";
    case Reason::UnstickDone:          return "unstick done";
    case Reason::TurnDone:             return "turn done";
    case Reason::TargetSeen:           return "target seen";
    case Reason::TargetClose:          return "target close";
    case Reason::NoTarget:             return "no target";
    case Reason::StaleData:            return "stale sensor data";
    case Reason::SensorFault:          return "sensor fault";
    case Reason::FaultCleared:         return "fault cleared";
    case Reason::CalibrationDone:      return "calibration done";
    case Reason::CalibrationFailed:    return "calibration failed";
    case Reason::CalibrationCancelled: return "calibration cancelled";
    case Reason::OtaStart:             return "ota start";
    case Reason::OtaDone:              return "ota done";
    case Reason::OtaFailed:            return "ota failed";
    case Reason::OpeningDone:          return "opening done";
    case Reason::OpeningAbort:         return "opening abort: bottle in range";
    case Reason::CarryStart:           return "carry start";
    case Reason::CarryDone:            return "carry done";
    case Reason::BottleLost:           return "bottle lost";
    case Reason::EscapeBudget:         return "escape budget exhausted";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Perception types
// ---------------------------------------------------------------------------
struct Range { uint16_t mm = 0; uint32_t sampledMs = 0; };
struct Drive { int16_t left = 0; int16_t right = 0; };

// ---------------------------------------------------------------------------
// Tuning — runtime-editable drive levels.
//
// `powerScale` is the "voltage" knob: 100 = full configured PWM, 50 = half.
// Every commanded PWM is multiplied by powerScale/100 and clamped to maxPwm,
// so one slider changes the effective motor voltage without recompiling.
// ---------------------------------------------------------------------------
struct Tuning {
  uint16_t powerScale    = 100;  // % of configured PWM, applied to everything
  uint16_t pushPwm       = 175;  // center target <= 150 mm
  uint16_t approachNear  = 115;  // center target 151..299 mm
  uint16_t approachFar   = 145;  // center target 300..900 mm
  uint16_t searchTurn    = 80;   // pivot while scanning, and side-target turn
  uint16_t searchAdvance = 100;  // straight segment of the search pattern
  uint16_t escapePwm     = 105;  // reverse away from an edge
  uint16_t turnPwm       = 85;   // pivot after an escape
  uint16_t unstickPwm    = 100;  // reverse out of a long push
  int16_t  pushSteerMax  = 25;   // max differential correction while pushing
  uint16_t maxPwm        = 255;  // absolute clamp, never above 255

  // ---- Opening run -------------------------------------------------------
  // The start box is fixed by the rules, so a short open-loop script is more
  // reliable than searching: straight, pivot ~60 deg, straight again lands the
  // robot next to the bottle. Measured on last year's table; re-time it here.
  bool     openingEnabled    = true;
  uint16_t openingPwm        = 140;
  uint16_t openingForwardMs  = 1400;  // leg 1
  uint16_t openingTurnMs     = 380;   // ~60 deg at openingPwm, measure yours
  int8_t   openingTurnDir    = 1;     // +1 = right, -1 = left
  uint16_t openingForward2Ms = 1200;  // leg 2
  uint16_t openingAbortMm    = 300;   // centre sees the bottle this close -> stop the script

  // ---- Carry -------------------------------------------------------------
  // After contact, arc slowly instead of pushing straight, so the bottle is
  // dragged along and the opponent cannot line up a clean approach.
  bool     carryEnabled  = true;
  uint16_t carryAfterMs  = 700;   // contact time before the arc starts
  uint16_t carryPwm      = 120;
  int16_t  carryBias     = 45;    // differential that makes the arc
  uint16_t carrySweepMs  = 900;   // swap the arc side this often
  uint16_t carryMaxMs    = 4000;  // leave Carry and go back to Push

  // ---- Edge robustness ---------------------------------------------------
  uint8_t  edgeDebounceMs     = 25;  // a raw edge must persist this long
  uint8_t  maxEscapesPerRound = 12;  // 0 = unlimited; above this -> Fault

  // Scales the magnitude and keeps the sign, so steering corrections survive.
  int16_t apply(int16_t pwm) const {
    const int32_t magnitude = pwm < 0 ? -int32_t(pwm) : int32_t(pwm);
    int32_t v = magnitude * int32_t(powerScale) / 100;
    if (v > int32_t(maxPwm)) v = int32_t(maxPwm);
    return int16_t(pwm < 0 ? -v : v);
  }
};

// ---------------------------------------------------------------------------
// Transition history — a ring buffer you can read back over the web UI.
// ---------------------------------------------------------------------------
constexpr uint8_t HISTORY_MAX = 64;

struct Transition {
  uint32_t atMs;
  State from, to;
  Reason reason;
  uint8_t edges;
  uint16_t qre[4];
};

struct History {
  Transition items[HISTORY_MAX];
  uint8_t head = 0;   // next write index
  uint8_t count = 0;  // number of valid entries

  void push(uint32_t now, State from, State to, Reason why, uint8_t edges, const uint16_t qre[4]) {
    Transition &t = items[head];
    t.atMs = now;
    t.from = from;
    t.to = to;
    t.reason = why;
    t.edges = edges;
    for (uint8_t i = 0; i < 4; ++i) t.qre[i] = qre ? qre[i] : 0;
    head = uint8_t((head + 1) % HISTORY_MAX);
    if (count < HISTORY_MAX) count++;
  }

  // i = 0 returns the newest transition.
  const Transition *newest(uint8_t i) const {
    if (i >= count) return nullptr;
    const uint8_t idx = uint8_t((head + HISTORY_MAX - 1 - i) % HISTORY_MAX);
    return &items[idx];
  }
};

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------
struct Machine {
  State state = State::Stopped;
  uint32_t roundStart = 0;
  uint32_t stateStart = 0;
  uint32_t clearStart = 0;
  bool holdAfterEscape = false;
  bool clearing = false;
  int8_t escapeDirection = 0;
  int8_t turnDirection = 1;
  uint8_t escapeCount = 0;      // per round; a runaway count means the
                                // threshold is wrong, not that the table moved
  uint32_t contactStart = 0;    // first contact of this engagement, survives
                                // a Push <-> Carry cycle
  uint8_t carryCycles = 0;

  Tuning tuning;
  History history;

  // Frame context, bound once per control tick so every log entry carries the
  // sensor picture that caused the transition.
  uint32_t now_ = 0;
  uint8_t edges_ = 0;
  uint16_t qre_[4] = {};

  void beginFrame(uint32_t now, uint8_t edges, const uint16_t qre[4]) {
    now_ = now;
    edges_ = edges;
    for (uint8_t i = 0; i < 4; ++i) qre_[i] = qre ? qre[i] : 0;
  }

  void change(State next, Reason why, bool resetStart = true) {
    history.push(now_, state, next, why, edges_, qre_);
    state = next;
    if (resetStart) stateStart = now_;
  }

  void startRound() {
    if (state != State::Stopped) return;
    roundStart = now_;
    escapeCount = 0;
    carryCycles = 0;
    contactStart = now_;
    change(State::Starting, Reason::ButtonStart);
  }

  void stopRound(Reason why) {
    if (state == State::Stopped) return;
    change(State::Stopped, why);
  }

  void faultRound(Reason why) {
    if (state == State::Fault) return;
    change(State::Fault, why);
  }

  void clearFault() {
    if (state != State::Fault) return;
    change(State::Stopped, Reason::FaultCleared);
  }

  // One control step. `emergency` is the stop button, a fault or an update.
  Drive tick(const Range (&r)[3], bool emergency) {
    if (state == State::Stopped || state == State::Fault || state == State::Updating) return {};

    // ---- Global overrides -------------------------------------------------
    if (emergency || now_ - roundStart >= ROUND_MS) { stopRound(Reason::Emergency); return {}; }

    if (state != State::Starting) {
      for (uint8_t i = 0; i < 3; ++i) {
        if (!r[i].sampledMs || now_ - r[i].sampledMs > RANGE_STALE_MS) {
          // A head that stops reporting ends the round; it is not a hardware
          // fault, so BUTTON1 can start again once the data is fresh.
          stopRound(Reason::StaleData);
          return {};
        }
      }
    }

    const bool front = (edges_ & 0x03) != 0;
    const bool rear  = (edges_ & 0x0C) != 0;
    if (front && rear) { stopRound(Reason::Emergency); return {}; }

    // ---- Edge handling (overrides every state except Escape) --------------
    if (edges_) {
      const int8_t direction = front ? -1 : 1;
      if (state != State::Escape) {
        // A per-round budget turns "the threshold is wrong" into a visible
        // fault instead of an endless reverse loop.
        if (tuning.maxEscapesPerRound && escapeCount >= tuning.maxEscapesPerRound) {
          faultRound(Reason::EscapeBudget);
          return {};
        }
        ++escapeCount;
        holdAfterEscape = (state == State::Push || state == State::Carry || state == State::Hold);
        escapeDirection = direction;
        turnDirection = (edges_ & 0x05) ? 1 : -1;
        change(State::Escape, Reason::EdgeSeen);
      } else if (direction != escapeDirection) {
        stopRound(Reason::EscapeReverseDir);
        return {};
      }
      clearing = false;
    }

    // ---- Per-state behaviour ---------------------------------------------
    switch (state) {
      case State::Starting:
        if (now_ - roundStart < START_DELAY_MS) return {};
        if (tuning.openingEnabled) {
          // The opening is timed from the moment it starts, not from the button.
          change(State::Opening, Reason::StartDelayDone);
          return openingDrive(r);
        }
        change(State::Search, Reason::StartDelayDone, false);
        return behave(r);

      case State::Opening:
        return openingDrive(r);

      case State::Carry:
        return carryDrive(r);

      case State::Escape: {
        if (now_ - stateStart >= 300) { stopRound(Reason::EscapeTimeout); return {}; }
        if (!edges_) {
          if (!clearing) { clearStart = now_; clearing = true; }
          if (now_ - clearStart >= 40) {
            change(holdAfterEscape ? State::Hold : State::Turn, Reason::EdgeCleared);
            return {};
          }
        }
        const int16_t pwm = escapeDirection * tuning.apply(tuning.escapePwm);
        return Drive{pwm, pwm};
      }

      case State::Hold:
        if (now_ - stateStart < SURVIVE_MS) return {};
        change(State::Search, Reason::HoldDone, false);
        return behave(r);

      case State::Unstick: {
        if (now_ - stateStart < 150) {
          const int16_t pwm = -tuning.apply(tuning.unstickPwm);
          return Drive{pwm, pwm};
        }
        change(State::Turn, Reason::UnstickDone);
        return {};
      }

      case State::Turn:
        if (now_ - stateStart < 250) {
          const int16_t pwm = tuning.apply(tuning.turnPwm);
          return Drive{int16_t(turnDirection * pwm), int16_t(-turnDirection * pwm)};
        }
        change(State::Search, Reason::TurnDone, false);
        return behave(r);

      case State::Push: {
        if (!r[1].mm) { change(State::Hold, Reason::LostCenterTarget); return {}; }
        if (now_ - contactStart >= 6000) {
          change(State::Unstick, Reason::PushTimeUp);
          const int16_t pwm = -tuning.apply(tuning.unstickPwm);
          return Drive{pwm, pwm};
        }
        if (tuning.carryEnabled && now_ - stateStart >= tuning.carryAfterMs) {
          change(State::Carry, Reason::CarryStart);
          return carryDrive(r);
        }
        return targetDrive(r);
      }

      case State::Approach:
        if (!r[1].mm) { change(State::Search, Reason::NoTarget, false); return searchDrive(r); }
        return targetDrive(r);

      case State::Search:
        return behave(r);

      case State::Stopped:
      case State::Fault:
      case State::Updating:
        return {};
    }
    return {};
  }

  // True when the machine is in a match round (not stopped / fault / updating).
  bool active() const { return state != State::Stopped && state != State::Fault && state != State::Updating; }

private:
  // Normal (non-edge) behaviour: a center target wins, otherwise scan.
  Drive behave(const Range (&r)[3], Reason why = Reason::None) {
    if (r[1].mm) return targetDrive(r, why);
    return searchDrive(r);
  }

  // Opening run: straight, pivot, straight — open loop on purpose, because the
  // start box is fixed and last year's field test showed this lands next to the
  // bottle. A centre reading inside openingAbortMm ends the script early so the
  // robot cannot drive past it.
  Drive openingDrive(const Range (&r)[3]) {
    if (r[1].mm && r[1].mm <= tuning.openingAbortMm) return behave(r, Reason::OpeningAbort);
    const uint32_t t = now_ - stateStart;
    const uint32_t leg1 = tuning.openingForwardMs;
    const uint32_t pivotEnd = leg1 + tuning.openingTurnMs;
    const uint32_t leg2End = pivotEnd + tuning.openingForward2Ms;
    if (t < leg1 || (t >= pivotEnd && t < leg2End)) {
      const int16_t pwm = tuning.apply(int16_t(tuning.openingPwm));
      return Drive{pwm, pwm};
    }
    if (t < pivotEnd) {
      const int16_t pwm = tuning.apply(int16_t(tuning.turnPwm));
      return Drive{int16_t(tuning.openingTurnDir * pwm), int16_t(-tuning.openingTurnDir * pwm)};
    }
    change(State::Search, Reason::OpeningDone, false);
    return behave(r);
  }

  // Carry: both wheels forward with a differential that alternates every
  // carrySweepMs, so the bottle is dragged along an arc instead of straight.
  // The side ToF readings pull the arc back toward the bottle when it drifts
  // off centre, which is what keeps contact through the sweep.
  Drive carryDrive(const Range (&r)[3]) {
    if (!r[1].mm) { change(State::Search, Reason::BottleLost, false); return searchDrive(r); }
    if (now_ - stateStart >= tuning.carryMaxMs) {
      ++carryCycles;
      change(State::Push, Reason::CarryDone);
      return targetDrive(r);
    }
    const uint16_t sweepMs = tuning.carrySweepMs ? tuning.carrySweepMs : 1;
    const bool flip = ((now_ - stateStart) / sweepMs) & 1;
    int16_t steer = flip ? tuning.carryBias : int16_t(-tuning.carryBias);
    if (r[0].mm && r[0].mm + 60 < r[1].mm) steer -= 20;
    if (r[2].mm && r[2].mm + 60 < r[1].mm) steer += 20;
    const int16_t speed = tuning.apply(int16_t(tuning.carryPwm));
    const int16_t steerOut = tuning.apply(steer);
    return Drive{int16_t(speed + steerOut), int16_t(speed - steerOut)};
  }

  // Center target acquired: push when close, approach when far.
  // The transition is logged only when the state actually changes — before this
  // guard the history ring buffer filled with one self-transition per 5 ms tick.
  Drive targetDrive(const Range (&r)[3], Reason why = Reason::None) {
    const bool close = r[1].mm <= 150;
    const State next = close ? State::Push : State::Approach;
    if (next != state) {
      if (close) {
        stateStart = now_;
        if (state != State::Carry) contactStart = now_;
      }
      change(next, why == Reason::None ? (close ? Reason::TargetClose : Reason::TargetSeen) : why, false);
    }

    int16_t steer = 0;
    if (r[0].mm && r[0].mm + 60 < r[1].mm) steer = -tuning.pushSteerMax;
    if (r[2].mm && r[2].mm + 60 < r[1].mm) steer = tuning.pushSteerMax;
    const int16_t speed = tuning.apply(
      close ? tuning.pushPwm : (r[1].mm < 300 ? tuning.approachNear : tuning.approachFar));
    const int16_t steerOut = tuning.apply(steer);
    return Drive{int16_t(speed + steerOut), int16_t(speed - steerOut)};
  }

  // No center target: turn toward a side target, otherwise run the search pattern.
  Drive searchDrive(const Range (&r)[3]) {
    if (r[0].mm || r[2].mm) {
      const bool left = r[0].mm && (!r[2].mm || r[0].mm <= r[2].mm);
      const int16_t pwm = tuning.apply(tuning.searchTurn);
      return left ? Drive{int16_t(-pwm), pwm} : Drive{pwm, int16_t(-pwm)};
    }
    const uint32_t elapsed = now_ - roundStart - START_DELAY_MS;
    const int16_t direction = (elapsed / 1000) % 2 ? -1 : 1;
    const uint32_t phase = elapsed % 1000;
    if (phase < 320) {
      const int16_t pwm = tuning.apply(tuning.searchTurn);
      return Drive{int16_t(-direction * pwm), int16_t(direction * pwm)};
    }
    if (phase < 500 || phase >= 800) return {};   // observe a stationary target
    const int16_t pwm = tuning.apply(tuning.searchAdvance);
    return Drive{pwm, pwm};
  }
};

// Display state for telemetry / web UI. Kept compatible with the desktop tool.
inline uint8_t searchDisplayPhase(uint32_t now, uint32_t roundStart) {
  const uint32_t phase = (now - roundStart - START_DELAY_MS) % 1000;
  if (phase < 320) return DISPLAY_SEARCH_TURN;
  if (phase < 500 || phase >= 800) return DISPLAY_SEARCH_OBSERVE;
  return DISPLAY_SEARCH_ADVANCE;
}

} // namespace BS
