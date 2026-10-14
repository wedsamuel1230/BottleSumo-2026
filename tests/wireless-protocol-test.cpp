#include "../firmware/bottlesumo_idf/main/protocol.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace BottleSumo;
int main() {
  Telemetry t{}; t.boot = 5;
  assert(validTelemetry(&t, sizeof(t))); assert(!validTelemetry(&t, sizeof(t)-1));
  t.version = 2; assert(!validTelemetry(&t, sizeof(t)));
  DriveCommand d{}; d.magic = COMMAND_MAGIC; d.version = 1; d.type = 1; d.boot = 5; d.left = 120; d.right = -120;
  assert(validDriveHeader(d, 5)); assert(!validDriveHeader(d, 6));
  d.left = 256; assert(!validDriveHeader(d, 5)); d.left = 0;
  d.type = 3; assert(!validDriveHeader(d, 5));
  assert(newer(1, UINT32_MAX)); assert(!newer(5, 5)); assert(!newer(4, 5));
  assert(driveLeaseLive(1149, 1000)); assert(!driveLeaseLive(1150, 1000));
  assert(driveLeaseLive(98, UINT32_MAX-50)); assert(!driveLeaseLive(99, UINT32_MAX-50));
  NetworkSettings s{}; std::strcpy(s.wifi_password, "public-test-fixture");
  std::memset(s.ota_token, 'a', 64); s.pmk[0] = s.lmk[0] = 1; s.channel = 6;
  assert(validNetworkSettings(s));
  std::strcpy(s.sta_ssid, "test-router"); assert(!validNetworkSettings(s));
  std::strcpy(s.sta_password, "public-test-fixture"); assert(validNetworkSettings(s));
  std::string line = "NET:"; const auto *bytes = reinterpret_cast<const uint8_t *>(&s);
  char hex[3]; for (size_t i = 0; i < sizeof(s); ++i) { std::snprintf(hex, sizeof(hex), "%02x", bytes[i]); line += hex; }
  NetworkSettings decoded{}; assert(parseNetworkLine(line.data(), line.size(), decoded));
  assert(!std::memcmp(&s, &decoded, sizeof(s)));
  assert(!parseNetworkLine(line.data(), line.size()-1, decoded));
  line[9] = 'z'; assert(!parseNetworkLine(line.data(), line.size(), decoded));
  s.robot_mac[0] = 1; assert(!validNetworkSettings(s)); s.robot_mac[0] = 0;
  std::memset(s.wifi_password, 'x', sizeof(s.wifi_password)); assert(!validNetworkSettings(s));
  puts("wireless framing, replay and lease assertions passed");
}
