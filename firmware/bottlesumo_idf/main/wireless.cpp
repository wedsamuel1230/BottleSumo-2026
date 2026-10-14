#include "robot.h"
#include "network_config.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

namespace BottleSumo {
static NetworkSettings settings{};
static const uint8_t *robotMac = settings.robot_mac, *receiverMac = settings.receiver_mac;
static const char *key = settings.ota_token;
static psa_key_id_t authKey = 0;
static bool receiverRole = false, peerReady = false;
static std::atomic<bool> reconnect{false};
static bool stationConfigured = false;
static QueueHandle_t inbound, rxTelemetry;
static std::atomic<bool> sending{false};
static std::atomic<uint32_t> sendOk{0}, sendFail{0};
static uint32_t udpFail = 0, lastCommand = 0; static bool commandSeen = false;

static bool hmac(const void *data, size_t size, uint8_t output[32]) {
  size_t length = 0;
  return psa_mac_compute(authKey, PSA_ALG_HMAC(PSA_ALG_SHA_256), static_cast<const uint8_t *>(data),
                         size, output, 32, &length) == PSA_SUCCESS && length == 32;
}
static bool equalBytes(const uint8_t *a, const uint8_t *b, size_t size) {
  uint8_t difference = 0; for (size_t i = 0; i < size; ++i) difference |= a[i] ^ b[i]; return difference == 0;
}
#ifndef BOTTLESUMO_RECEIVER
static bool unhex(const char *text, uint8_t *out, size_t size) {
  if (strlen(text) != size * 2) return false;
  auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
    (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
  for (size_t i = 0; i < size; ++i) {
    const int a = digit(text[2*i]), b = digit(text[2*i+1]); if (a < 0 || b < 0) return false;
    out[i] = a * 16 + b;
  }
  return true;
}
#endif
static void sent(const esp_now_send_info_t *, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) sendOk++; else sendFail++;
  sending = false;
}
static void received(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
  const auto *expected = receiverRole ? robotMac : receiverMac;
  if (!info || memcmp(info->src_addr, expected, 6)) return;
  if (receiverRole) {
    if (validTelemetry(data, length)) xQueueOverwrite(rxTelemetry, data);
  } else if (length == sizeof(DriveCommand)) {
    ReceivedDrive d{}; memcpy(&d.command, data, length); d.received_ms = nowMs();
    xQueueOverwrite(inbound, &d);
  }
}
QueueHandle_t receivedTelemetryQueue() { return rxTelemetry; }
bool storeWirelessConfig(const NetworkSettings &value) {
  if (!validNetworkSettings(value)) return false;
  nvs_handle_t nvs;
  if (nvs_open("wireless", NVS_READWRITE, &nvs) != ESP_OK) return false;
  esp_err_t result = nvs_set_blob(nvs, "cfg", &value, sizeof(value));
  if (result == ESP_OK) result = nvs_commit(nvs);
  nvs_close(nvs); return result == ESP_OK;
}
static void wifiEvent(void *, esp_event_base_t, int32_t event, void *) {
  if (event == WIFI_EVENT_STA_DISCONNECTED || event == WIFI_EVENT_STA_START) reconnect = stationConfigured;
  else if (event == WIFI_EVENT_STA_CONNECTED) reconnect = false;
}
void wirelessMaintain(bool stopped) {
  static uint32_t attempted = 0;
  if (stopped && stationConfigured && reconnect && nowMs() - attempted >= 5000) {
    attempted = nowMs();
    if (esp_wifi_connect() == ESP_OK) reconnect = false;
  }
}
bool sendEspnow(const void *data, size_t size) {
  if (!peerReady) return false;
  bool idle = false;
  if (!sending.compare_exchange_strong(idle, true)) { sendFail++; return false; }
  if (esp_now_send(receiverRole ? robotMac : receiverMac, static_cast<const uint8_t *>(data), size) != ESP_OK) {
    sending = false; sendFail++; return false;
  }
  return true;
}
static void acceptDrive(const ReceivedDrive &received) {
  const auto &d = received.command;
  Telemetry t{}; if (!xQueuePeek(telemetryQueue, &t, 0)) return;
  if (!(t.flags & Bench) || !(t.flags & Calibrated) || !(t.flags & HardwareConfirmed) ||
      (t.flags & (Fault | Updating)) || otaRequested) return;
  if (!validDriveHeader(d, bootId) || (commandSeen && !newer(d.sequence, lastCommand)) ||
      !driveLeaseLive(nowMs(), received.received_ms)) return;
  uint8_t signature[32];
  if (!hmac(&d, offsetof(DriveCommand, hmac), signature) || !equalBytes(signature, d.hmac, sizeof(signature))) return;
  commandSeen = true; lastCommand = d.sequence; xQueueOverwrite(commandQueue, &received);
}

#ifndef BOTTLESUMO_RECEIVER
static esp_err_t statusHandler(httpd_req_t *request) {
  Telemetry t{}; xQueuePeek(telemetryQueue, &t, 0);
  char json[144]; snprintf(json, sizeof(json), "{\"boot\":%lu,\"state\":%u,\"flags\":%u,\"uptime_ms\":%lu}",
    static_cast<unsigned long>(bootId), t.state, t.flags, static_cast<unsigned long>(t.uptime_ms));
  httpd_resp_set_type(request, "application/json"); return httpd_resp_sendstr(request, json);
}
static esp_err_t otaHandler(httpd_req_t *request) {
  auto reject = [request](const char *status, const char *message) {
    httpd_resp_set_status(request, status); return httpd_resp_sendstr(request, message);
  };
  Telemetry t{}; xQueuePeek(telemetryQueue, &t, 0);
  if (!(t.flags & OtaWindow) || (t.flags & Updating) || t.applied[0] || t.applied[1] ||
      (!(t.flags & Bench) && t.state != 0)) return reject("409 Conflict", "Stop and hold BUTTON2 for 2 seconds first");
  const auto *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || request->content_len < 32 || static_cast<size_t>(request->content_len) > partition->size)
    return reject("413 Payload Too Large", "Image size is outside OTA slot");
  char boot[16], digestText[65], authText[65]; uint8_t expectedDigest[32], expectedAuth[32], auth[32];
  char actualBoot[16]; snprintf(actualBoot, sizeof(actualBoot), "%lu", static_cast<unsigned long>(bootId));
  if (httpd_req_get_hdr_value_str(request, "X-Boot", boot, sizeof(boot)) != ESP_OK || strcmp(boot, actualBoot) ||
      httpd_req_get_hdr_value_str(request, "X-Image-SHA256", digestText, sizeof(digestText)) != ESP_OK ||
      httpd_req_get_hdr_value_str(request, "X-Auth", authText, sizeof(authText)) != ESP_OK ||
      !unhex(digestText, expectedDigest, 32) || !unhex(authText, expectedAuth, 32))
    return reject("403 Forbidden", "Invalid update authentication");
  char signedText[128]; int size = snprintf(signedText, sizeof(signedText), "ota\n%s\n%d\n%s", boot, request->content_len, digestText);
  if (size <= 0 || size >= static_cast<int>(sizeof(signedText)) || !hmac(signedText, size, auth) || !equalBytes(auth, expectedAuth, 32))
    return reject("403 Forbidden", "Invalid update authentication");
  if (static_cast<int32_t>(otaWindowUntil.load() - nowMs()) <= 0) return reject("409 Conflict", "Update window expired");
  bool expected = false;
  if (otaLocked || !otaRequested.compare_exchange_strong(expected, true))
    return reject("409 Conflict", "Another maintenance operation is active");
  otaWindowUntil = 0;
  const uint32_t lockStart = nowMs();
  while (!otaLocked && nowMs() - lockStart < 250) vTaskDelay(pdMS_TO_TICKS(5));
  if (!otaLocked) { otaRequested = false; return reject("409 Conflict", "Control did not confirm stopped outputs"); }
  esp_ota_handle_t handle{}; esp_err_t result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) result = ESP_FAIL;
  uint8_t buffer[4096]; int remaining = request->content_len;
  while (result == ESP_OK && remaining > 0) {
    const int count = httpd_req_recv(request, reinterpret_cast<char *>(buffer), std::min<int>(remaining, sizeof(buffer)));
    if (count <= 0) { result = ESP_FAIL; break; }
    result = esp_ota_write(handle, buffer, count);
    if (psa_hash_update(&hash, buffer, count) != PSA_SUCCESS) result = ESP_FAIL;
    remaining -= count;
  }
  uint8_t actualDigest[32]{}; size_t digestLength = 0;
  if (psa_hash_finish(&hash, actualDigest, sizeof(actualDigest), &digestLength) != PSA_SUCCESS ||
      digestLength != sizeof(actualDigest) || !equalBytes(actualDigest, expectedDigest, 32)) result = ESP_FAIL;
  psa_hash_abort(&hash);
  if (result == ESP_OK) result = esp_ota_end(handle);
  else if (handle) esp_ota_abort(handle);
  if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) {
    otaLocked = false; otaRequested = false;
    return reject("400 Bad Request", "Update failed; inspect boot state before retrying");
  }
  httpd_resp_sendstr(request, "Image verified; rebooting. New boot performs self-test before confirmation.");
  vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); return ESP_OK;
}
#endif

bool wirelessInit(bool receiver) {
  receiverRole = receiver;
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
  printf("%s STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", receiver ? "Receiver" : "Robot",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  nvs_handle_t nvs; size_t storedSize = sizeof(settings); esp_err_t loaded = ESP_FAIL;
  if (nvs_open("wireless", NVS_READONLY, &nvs) == ESP_OK) {
    loaded = nvs_get_blob(nvs, "cfg", &settings, &storedSize); nvs_close(nvs);
  }
  if (loaded != ESP_OK || storedSize != sizeof(settings) || !validNetworkSettings(settings)) {
    printf("Wireless disabled; USB provision configuration and reset.\n"); return false;
  }
  if (psa_crypto_init() != PSA_SUCCESS) return false;
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
  const psa_status_t imported = psa_import_key(&attributes, reinterpret_cast<const uint8_t *>(key), strlen(key), &authKey);
  psa_reset_key_attributes(&attributes);
  if (imported != PSA_SUCCESS) return false;
  auto ok = [](esp_err_t result) { return result == ESP_OK; };
  if (!ok(esp_netif_init()) || !ok(esp_event_loop_create_default())) return false;
  if (!esp_netif_create_default_wifi_sta()) return false;
  if (!receiver && !esp_netif_create_default_wifi_ap()) return false;
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  if (!ok(esp_wifi_init(&init)) || !ok(esp_wifi_set_storage(WIFI_STORAGE_RAM)) ||
      !ok(esp_wifi_set_mode(receiver ? WIFI_MODE_STA : WIFI_MODE_APSTA))) return false;
  if (!receiver) {
    wifi_config_t cfg{}; strcpy(reinterpret_cast<char *>(cfg.ap.ssid), "BottleSumo");
    strcpy(reinterpret_cast<char *>(cfg.ap.password), settings.wifi_password);
    cfg.ap.ssid_len = 10; cfg.ap.channel = settings.channel; cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (!ok(esp_wifi_set_config(WIFI_IF_AP, &cfg))) return false;
  }
  const char *ssid = receiver ? "BottleSumo" : settings.sta_ssid;
  const char *password = receiver ? settings.wifi_password : settings.sta_password;
  stationConfigured = *ssid != '\0';
  if (stationConfigured) {
    if (strlen(ssid) > 32 || strlen(password) < 8 || strlen(password) > 63) return false;
    wifi_config_t cfg{}; memcpy(cfg.sta.ssid, ssid, strlen(ssid)); memcpy(cfg.sta.password, password, strlen(password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; cfg.sta.failure_retry_cnt = 2;
    if (!ok(esp_wifi_set_config(WIFI_IF_STA, &cfg))) return false;
  }
  if (!ok(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr))) return false;
  if (!ok(esp_wifi_start()) || !ok(esp_wifi_set_ps(WIFI_PS_NONE)) ||
      !ok(esp_wifi_set_channel(settings.channel, WIFI_SECOND_CHAN_NONE))) return false;
  inbound = xQueueCreate(1, sizeof(ReceivedDrive)); rxTelemetry = xQueueCreate(1, sizeof(Telemetry));
  if (!inbound || !rxTelemetry) return false;
  const uint8_t *peerMac = receiver ? robotMac : receiverMac;
  if ((receiver || TELEMETRY_MODE != TransportMode::AP_UDP) &&
      (peerMac[0] || peerMac[1] || peerMac[2] || peerMac[3] || peerMac[4] || peerMac[5])) {
    if (!ok(esp_now_init()) || !ok(esp_now_set_pmk(settings.pmk)) ||
        !ok(esp_now_register_send_cb(sent)) || !ok(esp_now_register_recv_cb(received))) return false;
    esp_now_peer_info_t peer{}; memcpy(peer.peer_addr, peerMac, 6); memcpy(peer.lmk, settings.lmk, 16);
    peer.channel = 0; peer.ifidx = WIFI_IF_STA; peer.encrypt = true; // Follow the current AP+STA channel.
    if (!ok(esp_now_add_peer(&peer))) return false;
    peerReady = true;
  }
  if (TELEMETRY_MODE == TransportMode::ESP_NOW && !peerReady) return false;
#ifndef BOTTLESUMO_RECEIVER
  if (!receiver) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.task_priority = 3; config.core_id = 0;
    config.stack_size = 8192; config.max_open_sockets = 2; config.recv_wait_timeout = 5;
    httpd_handle_t server;
    if (!ok(httpd_start(&server, &config))) return false;
    httpd_uri_t status{}; status.uri = "/status"; status.method = HTTP_GET; status.handler = statusHandler;
    httpd_uri_t ota{}; ota.uri = "/ota"; ota.method = HTTP_PUT; ota.handler = otaHandler;
    if (!ok(httpd_register_uri_handler(server, &status)) || !ok(httpd_register_uri_handler(server, &ota))) return false;
  }
#endif
  return true;
}
void wirelessTask(void *) {
  const bool udpEnabled = TELEMETRY_MODE != TransportMode::ESP_NOW;
  const int fd = udpEnabled ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1;
  if (udpEnabled && fd < 0) { vTaskDelete(nullptr); return; }
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(4242); local.sin_addr.s_addr = INADDR_ANY;
  if (udpEnabled && bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local))) { close(fd); vTaskDelete(nullptr); return; }
  if (udpEnabled) fcntl(fd, F_SETFL, O_NONBLOCK);
  sockaddr_in subscriber{}; uint32_t subscribedAt = 0, sequence = 0;
  TickType_t wake = xTaskGetTickCount();
  while (true) {
    radioHeartbeat = nowMs();
    ReceivedDrive command;
    if (xQueueReceive(inbound, &command, 0)) acceptDrive(command);
    for (unsigned i = 0; udpEnabled && i < 4; ++i) {
      uint8_t data[64]; sockaddr_in from{}; socklen_t length = sizeof(from);
      const int receivedSize = recvfrom(fd, data, sizeof(data), 0, reinterpret_cast<sockaddr *>(&from), &length);
      if (receivedSize < 0) break;
      if (receivedSize == 4 && !memcmp(data, "SUB\n", 4)) { subscriber = from; subscribedAt = nowMs(); }
      else if (receivedSize == sizeof(DriveCommand)) {
        ReceivedDrive d{}; memcpy(&d.command, data, receivedSize); d.received_ms = nowMs(); acceptDrive(d);
      }
    }
    Telemetry t;
    if (xQueuePeek(telemetryQueue, &t, 0)) {
      t.sequence = sequence++; t.espnow_ok = sendOk; t.espnow_fail = sendFail; t.udp_fail = udpFail;
      wirelessMaintain(!(t.flags & Updating) && ((t.flags & Bench) || t.state == 0) && !t.applied[0] && !t.applied[1]);
      if (udpEnabled && subscriber.sin_port && nowMs() - subscribedAt < 3000 &&
          sendto(fd, &t, sizeof(t), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&subscriber), sizeof(subscriber)) != sizeof(t)) udpFail++;
      if (peerReady) sendEspnow(&t, sizeof(t));
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
  }
}
}
