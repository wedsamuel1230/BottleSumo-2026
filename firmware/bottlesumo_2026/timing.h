// timing.h — per-task budget evidence, filled by the task loops.
#pragma once
#include <stdint.h>

namespace bs {

struct TaskTiming {
  volatile uint16_t maxUs = 0;
  volatile uint16_t lastUs = 0;
  volatile uint32_t misses = 0;
};

extern TaskTiming g_controlTiming;
extern TaskTiming g_tofTiming;
extern TaskTiming g_netTiming;

inline void timingRecord(TaskTiming &t, uint32_t elapsedUs, uint32_t budgetUs) {
  t.lastUs = uint16_t(elapsedUs > 65535 ? 65535 : elapsedUs);
  if (t.lastUs > t.maxUs) t.maxUs = t.lastUs;
  if (elapsedUs > budgetUs) t.misses = t.misses + 1;
}

} // namespace bs
