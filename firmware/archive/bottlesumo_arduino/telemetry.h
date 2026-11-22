// telemetry.h — the UDP snapshot packet.
//
// The layout is byte-compatible with firmware/bottlesumo_idf/main/protocol.h,
// so tools/bottlesumo_debug can read this firmware's stream unchanged.
// ESP-NOW and authenticated bench driving are not part of this Arduino build;
// the web UI replaces them for tuning and OTA. The espnow_ok/espnow_fail
// counters stay in the struct (always zero here) so the packet size is stable.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace BS {

constexpr uint32_t TELEMETRY_MAGIC = 0x42534D54; // 'BSMT'
constexpr uint16_t PROTOCOL_VERSION = 1;

enum Flags : uint8_t {
  Fault             = 1,
  Calibrated        = 2,
  HardwareConfirmed = 4,
  Updating          = 8,
  Bench             = 16,
  OtaWindow         = 32,
};

struct __attribute__((packed)) Telemetry {
  uint32_t magic = TELEMETRY_MAGIC;
  uint16_t version = PROTOCOL_VERSION, size = sizeof(Telemetry);
  uint32_t boot, sequence, uptime_ms, state_elapsed_ms;
  uint16_t qre[4], distance[3], range_age_ms[3];
  int16_t requested[2], applied[2];
  uint32_t fg[2], control_max_us, deadline_misses, espnow_ok, espnow_fail, udp_fail;
  uint8_t state, edges, flags, reserved = 0;
};
static_assert(sizeof(Telemetry) == 84, "Telemetry layout changed");

inline bool validTelemetry(const void *data, size_t size) {
  if (size != sizeof(Telemetry)) return false;
  Telemetry packet;
  memcpy(&packet, data, size);
  return packet.magic == TELEMETRY_MAGIC && packet.version == PROTOCOL_VERSION && packet.size == size;
}

} // namespace BS
