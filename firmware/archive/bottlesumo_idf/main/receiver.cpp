#ifdef BOTTLESUMO_RECEIVER
#include "robot.h"
#include "driver/usb_serial_jtag.h"
#include "nvs_flash.h"
#include <cstdio>
#include <cstring>

extern "C" void app_main() {
  using namespace BottleSumo;
  ESP_ERROR_CHECK(nvs_flash_init());
  usb_serial_jtag_driver_config_t usb{}; usb.rx_buffer_size = 1024; usb.tx_buffer_size = 512;
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
  const bool connected = wirelessInit(true);
  char line[564]{}; size_t used = 0; bool overflow = false;
  while (true) {
    wirelessMaintain(true); // Associate to robot AP so ESP-NOW follows AP+STA channel changes.
    Telemetry t;
    if (connected && xQueueReceive(receivedTelemetryQueue(), &t, 0)) {
      char output[sizeof(t)*2 + 4] = "T:";
      const auto *bytes = reinterpret_cast<const uint8_t *>(&t);
      for (size_t i = 0; i < sizeof(t); ++i) snprintf(output + 2 + i*2, 3, "%02x", bytes[i]);
      output[2 + sizeof(t)*2] = '\n';
      usb_serial_jtag_write_bytes(output, 3 + sizeof(t)*2, pdMS_TO_TICKS(5));
    }
    char c;
    for (unsigned i = 0; i < 128 && usb_serial_jtag_read_bytes(&c, 1, 0) == 1; ++i) {
      if (c == '\r') continue;
      if (c == '\n') {
        if (!overflow && used >= 4 && !memcmp(line, "NET:", 4)) {
          NetworkSettings settings;
          const bool saved = parseNetworkLine(line, used, settings) && storeWirelessConfig(settings);
          const char *reply = saved ? "NET:OK\n" : "NET:REJECT\n";
          usb_serial_jtag_write_bytes(reply, strlen(reply), pdMS_TO_TICKS(100));
        }
        if (!overflow && used == 106 && line[0] == 'C' && line[1] == ':') {
          DriveCommand command{}; auto *bytes = reinterpret_cast<uint8_t *>(&command); bool valid = true;
          for (size_t j = 0; j < sizeof(command); ++j) {
            auto hex = [](char v) { return v >= '0' && v <= '9' ? v-'0' : (v >= 'a' && v <= 'f' ? v-'a'+10 : -1); };
            const int a = hex(line[2+j*2]), b = hex(line[3+j*2]);
            if (a < 0 || b < 0) { valid = false; break; } bytes[j] = a*16+b;
          }
          if (valid) sendEspnow(&command, sizeof(command));
        }
        used = 0; overflow = false;
      } else if (used < sizeof(line)-1) line[used++] = c;
      else overflow = true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
#endif
