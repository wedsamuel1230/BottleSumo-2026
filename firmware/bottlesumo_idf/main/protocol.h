#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BottleSumo {
constexpr uint32_t TELEMETRY_MAGIC = 0x42534D54, COMMAND_MAGIC = 0x4253434D;
constexpr uint16_t PROTOCOL_VERSION = 1;
enum Flags : uint8_t { Fault = 1, Calibrated = 2, HardwareConfirmed = 4,
                      Updating = 8, Bench = 16, OtaWindow = 32 };
struct __attribute__((packed)) Telemetry {
  uint32_t magic = TELEMETRY_MAGIC;
  uint16_t version = PROTOCOL_VERSION, size = sizeof(Telemetry);
  uint32_t boot, sequence, uptime_ms, state_elapsed_ms;
  uint16_t qre[4], distance[3], range_age_ms[3];
  int16_t requested[2], applied[2];
  uint32_t fg[2], control_max_us, deadline_misses, espnow_ok, espnow_fail, udp_fail;
  uint8_t state, edges, flags, reserved = 0;
};
struct __attribute__((packed)) DriveCommand {
  uint32_t magic;
  uint16_t version, type; // 1=drive, 2=stop. Accepted only in physically selected bench mode.
  uint32_t boot, sequence;
  int16_t left, right;
  uint8_t hmac[32]; // HMAC-SHA256 of the first 20 bytes using ota_token from local config.
};
static_assert(sizeof(Telemetry) == 84);
static_assert(sizeof(DriveCommand) == 52);
struct __attribute__((packed)) NetworkSettings {
  uint32_t magic = 0x42534e43;
  uint16_t version = 1, size = sizeof(NetworkSettings);
  char wifi_password[64], ota_token[65], sta_ssid[33], sta_password[64];
  uint8_t robot_mac[6], receiver_mac[6], pmk[16], lmk[16], channel;
};
static_assert(sizeof(NetworkSettings) == 279);
inline bool validNetworkSettings(const NetworkSettings &s) {
  if (s.magic != 0x42534e43 || s.version != 1 || s.size != sizeof(s) || s.channel < 1 || s.channel > 11) return false;
  auto length = [](const char *text, size_t size) -> size_t {
    const char *end = static_cast<const char *>(std::memchr(text, 0, size));
    return end ? static_cast<size_t>(end - text) : size;
  };
  auto printable = [](const char *text, size_t size) {
    for (size_t i = 0; i < size; ++i) if (text[i] < 32 || text[i] > 126) return false;
    return true;
  };
  const size_t wifi = length(s.wifi_password, 64), token = length(s.ota_token, 65);
  const size_t ssid = length(s.sta_ssid, 33), password = length(s.sta_password, 64);
  if (wifi < 8 || wifi > 63 || !printable(s.wifi_password, wifi) || token != 64 || ssid > 32 ||
      (ssid ? password < 8 || password > 63 : password != 0) || !printable(s.sta_password, password)) return false;
  for (size_t i = 0; i < ssid; ++i) if (static_cast<uint8_t>(s.sta_ssid[i]) < 32 || s.sta_ssid[i] == 127) return false;
  for (size_t i = 0; i < token; ++i) {
    const char c = s.ota_token[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  if ((s.robot_mac[0] & 1) || (s.receiver_mac[0] & 1)) return false;
  uint8_t pmk = 0, lmk = 0; for (unsigned i = 0; i < 16; ++i) { pmk |= s.pmk[i]; lmk |= s.lmk[i]; }
  return pmk && lmk;
}
inline bool parseNetworkLine(const char *text, size_t length, NetworkSettings &settings) {
  if (length != 4 + sizeof(settings)*2 || std::memcmp(text, "NET:", 4)) return false;
  NetworkSettings decoded; auto *bytes = reinterpret_cast<uint8_t *>(&decoded);
  auto digit = [](char c) { return c >= '0' && c <= '9' ? c-'0' :
    (c >= 'a' && c <= 'f' ? c-'a'+10 : (c >= 'A' && c <= 'F' ? c-'A'+10 : -1)); };
  for (size_t i = 0; i < sizeof(settings); ++i) {
    const int a = digit(text[4+i*2]), b = digit(text[5+i*2]); if (a < 0 || b < 0) return false;
    bytes[i] = a*16+b;
  }
  if (!validNetworkSettings(decoded)) return false;
  settings = decoded; return true;
}
inline bool validTelemetry(const void *data, size_t size) {
  if (size != sizeof(Telemetry)) return false;
  Telemetry packet; std::memcpy(&packet, data, size);
  return packet.magic == TELEMETRY_MAGIC && packet.version == PROTOCOL_VERSION && packet.size == size;
}
inline bool newer(uint32_t sequence, uint32_t previous) {
  const uint32_t delta = sequence - previous;
  return delta != 0 && delta < 0x80000000u;
}
inline bool driveLeaseLive(uint32_t now, uint32_t received) { return now - received < 150; }
inline bool validDriveHeader(const DriveCommand &d, uint32_t boot) {
  return d.magic == COMMAND_MAGIC && d.version == PROTOCOL_VERSION && d.boot == boot &&
    (d.type == 1 || d.type == 2) && d.left >= -255 && d.left <= 255 && d.right >= -255 && d.right <= 255;
}
}
