// control.h — the one manager for the match: explicit states, one tick loop.
#pragma once
#include <stdint.h>
#include "config.h"

namespace bs {

enum class State : uint8_t {
  Init = 0,
  Idle,
  Debug,
  StartDelay,
  Route,
  Search,
  Attack,
  Escape,
  Hold,
  Failed,
  RoundEnd,
};

enum class Division : uint8_t { None = 0, A, B };

const char *stateName(State state);
const char *divisionName(Division division);

// Copy for the web layer. Written by the control task, read by the network
// task, so every field is plain data.
struct Snapshot {
  State state = State::Init;
  Division division = Division::None;
  uint32_t stateMs = 0;
  uint32_t roundMs = 0;
  uint8_t edges = 0;
  uint16_t ir[4] = {};
  uint16_t threshold[4] = {};
  uint16_t tofMm = 0;
  bool tofFresh = false;
  bool calibrated = false;
  bool tofOk = false;
  int8_t calibrationProgress = -1; // 0..100 while capturing
  bool inhibited = false;
  uint16_t controlMaxUs = 0;
  uint32_t controlMisses = 0;
  char event[24] = "boot";
};

enum class CommandKind : uint8_t {
  StartA,
  StartB,
  Stop,
  DebugEnter,
  DebugExit,
  Drive, // Debug only: left/right percent, refreshed as a dead-man lease
  CalibrateFloor,
  CalibrateVoid,
  Manual,
  ClearManual,
};

struct Command {
  CommandKind kind = CommandKind::Stop;
  uint8_t channel = 0;
  uint16_t value = 0;
  bool high = true;
  int8_t left = 0;
  int8_t right = 0;
};

void controlBegin();
void controlPost(const Command &command);
void controlTofSample(uint16_t mm, uint32_t at);
Snapshot controlSnapshot();
void controlSetInhibited(bool inhibited);
void controlTask(void *arg);

} // namespace bs
