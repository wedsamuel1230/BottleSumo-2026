#include "robot.h"
#include "strategy.h"
#include "VL53L0X.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <algorithm>
#include <cstdio>

namespace BottleSumo {
QueueHandle_t telemetryQueue, commandQueue;
std::atomic<bool> otaRequested{false}, otaLocked{false};
std::atomic<uint32_t> otaWindowUntil{0};
std::atomic<uint32_t> radioHeartbeat{0};
uint32_t bootId;
uint32_t nowMs() { return esp_timer_get_time() / 1000; }

#ifndef BOTTLESUMO_RECEIVER
constexpr bool HARDWARE_CONFIRMED = false, FG_CONNECTED = false;
constexpr int FORWARD_LEVEL[2] = {1, 1}, BENCH_MAX_PWM = 120;
constexpr gpio_num_t PWM_PINS[2] = {GPIO_NUM_10, GPIO_NUM_13};
constexpr gpio_num_t DIR_PINS[2] = {GPIO_NUM_11, GPIO_NUM_14};
constexpr gpio_num_t FG_PINS[2] = {GPIO_NUM_9, GPIO_NUM_12};
constexpr gpio_num_t XSHUT[3] = {GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_15};
constexpr adc_channel_t QRE_CHANNELS[4] = {ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_3, ADC_CHANNEL_4};
constexpr uint32_t CAL_MAGIC = 0x42534D33;
struct CalibrationRecord { uint32_t magic; Calibration values; };
struct SensorFrame { Range ranges[3]; };
struct Motor { int direction = -1, duty = 0; uint32_t zero_since = 0; };
static QueueHandle_t sensorQueue, serviceQueue, saveQueue;
static std::atomic<bool> faulted{false};
static std::atomic<bool> storagePending{false}, bootVerified{false};
static std::atomic<uint32_t> fg[2];
static adc_oneshot_unit_handle_t adc;
static Calibration calibration;
static bool calibrated = false, floorCaptured = false, bench = false;
static Motor motors[2];

static bool check(esp_err_t error) {
  if (error == ESP_OK) return true;
  faulted = true; return false;
}
static void IRAM_ATTR fgInterrupt(void *arg) {
  fg[reinterpret_cast<uintptr_t>(arg)].fetch_add(1, std::memory_order_relaxed);
}
static bool setWheel(unsigned i, int requested, uint32_t now) {
  auto &m = motors[i];
  auto duty = [i](int value) {
    return check(ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(i), value)) &&
           check(ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(i)));
  };
  if (!requested) {
    if (m.duty) m.zero_since = now;
    m.duty = 0; return duty(0);
  }
  const int direction = requested > 0 ? 1 : -1;
  if (direction != m.direction) {
    if (m.duty) { m.duty = 0; m.zero_since = now; return duty(0); }
    if (now - m.zero_since < 25) return true;
    if (!check(gpio_set_level(DIR_PINS[i], direction > 0 ? FORWARD_LEVEL[i] : !FORWARD_LEVEL[i]))) return false;
    m.direction = direction;
  }
  const int desired = std::min(255, std::abs(requested));
  m.duty = desired > m.duty ? std::min(desired, m.duty + 12) : desired;
  return duty(m.duty);
}
static bool stopOutputs(uint32_t now) {
  const bool left = setWheel(0, 0, now), right = setWheel(1, 0, now);
  // A driver failure must still remove the PWM signal at the pin.
  if (!left || !right) {
    for (unsigned i = 0; i < 2; ++i) {
      ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(i), 0);
      gpio_set_direction(PWM_PINS[i], GPIO_MODE_OUTPUT); gpio_set_level(PWM_PINS[i], 0);
    }
  }
  return left && right;
}
static void sensorTask(void *) {
  i2c_master_bus_config_t cfg{};
  cfg.i2c_port = I2C_NUM_0; cfg.sda_io_num = GPIO_NUM_17; cfg.scl_io_num = GPIO_NUM_16;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT; cfg.glitch_ignore_cnt = 7;
  i2c_master_bus_handle_t bus;
  if (!check(i2c_new_master_bus(&cfg, &bus))) { vTaskDelete(nullptr); return; }
  VL53L0X sensors[3];
  for (auto pin : XSHUT) { gpio_set_direction(pin, GPIO_MODE_OUTPUT); gpio_set_level(pin, 0); }
  vTaskDelay(pdMS_TO_TICKS(10));
  for (unsigned i = 0; i < 3; ++i) {
    gpio_set_direction(XSHUT[i], GPIO_MODE_INPUT); vTaskDelay(pdMS_TO_TICKS(10));
    if (!sensors[i].begin(bus) || !sensors[i].init() || !sensors[i].setAddress(0x30 + i) ||
        !sensors[i].setMeasurementTimingBudget(33000)) {
      faulted = true; vTaskDelete(nullptr); return;
    }
  }
  SensorFrame frame{};
  constexpr unsigned schedule[] = {1, 0, 1, 2}; // Center twice per cycle for earlier front acquisition.
  uint8_t hits[3]{}; unsigned slot = 0;
  while (!faulted) {
    if (otaLocked) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    const unsigned index = schedule[slot]; auto &sensor = sensors[index];
    if (!sensor.startSingle()) { faulted = true; break; }
    const uint32_t start = nowMs(); bool ready = false;
    while (!ready && nowMs() - start < 80) {
      if (!sensor.dataReady(ready)) { faulted = true; break; }
      if (!ready) vTaskDelay(pdMS_TO_TICKS(1));
    }
    uint16_t mm; uint8_t status;
    if (!ready || faulted || !sensor.readResult(mm, status)) { faulted = true; break; }
    const bool target = status == 11 && mm >= 35 && mm <= 900;
    hits[index] = target ? std::min<int>(2, hits[index] + 1) : 0;
    frame.ranges[index] = {static_cast<uint16_t>(hits[index] == 2 ? mm : 0), nowMs()};
    xQueueOverwrite(sensorQueue, &frame); slot = (slot + 1) % 4;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  vTaskDelete(nullptr);
}
static void controlTask(void *) {
  Controller controller; SensorFrame sensors{}; Telemetry telemetry{};
  telemetry.boot = bootId;
  bool stable = false, raw = false; uint32_t changed = 0, stopHeld = 0;
  bool stopTracking = false, windowArmed = false, clearing = false;
  uint8_t edges = 0; char calibrationMode = 0; uint32_t calibrationStart = 0;
  uint8_t previousState = 0; uint32_t stateChanged = nowMs(), lastBegan = stateChanged;
  ReceivedDrive drive{}; bool driveValid = false;
  TickType_t wake = xTaskGetTickCount();
  check(esp_task_wdt_add(nullptr));
  while (true) {
    const int64_t began = esp_timer_get_time(); const uint32_t now = began / 1000;
    xQueuePeek(sensorQueue, &sensors, 0);
    uint16_t qre[4]; uint8_t nextEdges = 0;
    for (unsigned i = 0; i < 4; ++i) {
      int value = 0; check(adc_oneshot_read(adc, QRE_CHANNELS[i], &value)); qre[i] = value;
      if (calibrated && calibration.edge(i, value, edges & (1 << i))) nextEdges |= 1 << i;
    }
    edges = nextEdges;
    const bool stop = gpio_get_level(GPIO_NUM_42);
    if (stop) {
      controller.stop(); driveValid = false; calibrationMode = 0;
      if (!stopTracking) { stopHeld = now; stopTracking = true; }
      if (!windowArmed && now - stopHeld >= 2000 && !otaLocked) {
        otaWindowUntil = now + 60000; windowArmed = true;
      }
    } else { stopTracking = windowArmed = false; }
    if (otaRequested) {
      controller.stop(); driveValid = false; calibrationMode = 0;
      if (stopOutputs(now)) otaLocked = true;
    } else otaLocked = false;
    char command;
    if (!otaRequested && xQueueReceive(serviceQueue, &command, 0)) {
      if (command == 'x') { controller.stop(); driveValid = false; calibrationMode = 0; }
      if ((command == 'f' || command == 'e') && controller.state == State::Stopped && !driveValid &&
          (command != 'e' || floorCaptured)) {
        calibrationMode = command; calibrated = false; calibrationStart = now;
        for (unsigned i = 0; i < 4; ++i) {
          (command == 'f' ? calibration.floorMin : calibration.edgeMin)[i] = 4095;
          (command == 'f' ? calibration.floorMax : calibration.edgeMax)[i] = 0;
        }
      }
    }
    if (calibrationMode && !otaRequested) {
      for (unsigned i = 0; i < 4; ++i) {
        auto &low = (calibrationMode == 'f' ? calibration.floorMin : calibration.edgeMin)[i];
        auto &high = (calibrationMode == 'f' ? calibration.floorMax : calibration.edgeMax)[i];
        low = std::min(low, qre[i]); high = std::max(high, qre[i]);
      }
      if (now - calibrationStart >= 3000) {
        if (calibrationMode == 'f') floorCaptured = true;
        else if (calibration.valid()) {
          // NVS flash writes run in the service task, with movement interlocked.
          CalibrationRecord record{CAL_MAGIC, calibration}; storagePending = true;
          xQueueOverwrite(saveQueue, &record); clearing = true;
        }
        calibrationMode = 0;
      }
    }
    if (clearing && !storagePending) { clearing = false; calibrated = calibration.valid() && !faulted; }
    const bool pressed = gpio_get_level(GPIO_NUM_41);
    if (pressed != raw) { raw = pressed; changed = now; }
    if (now - changed >= 30 && stable != pressed) {
      stable = pressed;
      bool fresh = true;
      for (auto r : sensors.ranges) if (!r.sampledMs || now - r.sampledMs > RANGE_STALE_MS) fresh = false;
      if (pressed && !bench && !stop && !otaRequested && !calibrationMode && !clearing &&
          !faulted && bootVerified && calibrated && HARDWARE_CONFIRMED && !edges && fresh && controller.state == State::Stopped)
        controller.start(now);
    }
    ReceivedDrive incoming;
    if (xQueueReceive(commandQueue, &incoming, 0)) { drive = incoming; driveValid = incoming.command.type == 1; }
    if (stop || otaRequested) driveValid = false;
    Drive requested{};
    if (bench) {
      if (driveValid && driveLeaseLive(now, drive.received_ms) && !stop && !otaRequested && !faulted &&
          calibrated && bootVerified && HARDWARE_CONFIRMED && !calibrationMode && !clearing && !edges) {
        requested = {static_cast<int16_t>(std::clamp<int>(drive.command.left, -BENCH_MAX_PWM, BENCH_MAX_PWM)),
                     static_cast<int16_t>(std::clamp<int>(drive.command.right, -BENCH_MAX_PWM, BENCH_MAX_PWM))};
      } else driveValid = false;
    } else requested = controller.tick(now, edges, sensors.ranges, stop || faulted || otaRequested);
    if (!setWheel(0, requested.left, now) || !setWheel(1, requested.right, now)) stopOutputs(now);
    uint8_t displayState = otaLocked ? 24 : (calibrationMode == 'f' ? 22 :
      (calibrationMode == 'e' ? 23 : (bench ? (requested.left || requested.right ? 21 : 20) : static_cast<uint8_t>(controller.state))));
    if (!bench && !calibrationMode && !otaLocked && controller.state == State::Search) {
      const uint32_t phase = (now - controller.roundStart - START_DELAY_MS) % 1000;
      displayState = (sensors.ranges[0].mm || sensors.ranges[2].mm) ? 9 :
        (phase < 320 ? 10 : (phase < 500 || phase >= 800 ? 11 : 12));
    }
    if (displayState != previousState) { previousState = displayState; stateChanged = now; }
    telemetry.sequence++; telemetry.uptime_ms = now; telemetry.state_elapsed_ms = now - stateChanged;
    telemetry.state = displayState; telemetry.edges = edges;
    telemetry.flags = (faulted ? Fault : 0) | (calibrated ? Calibrated : 0) | (HARDWARE_CONFIRMED ? HardwareConfirmed : 0) |
      (otaLocked ? Updating : 0) | (bench ? Bench : 0) | (static_cast<int32_t>(otaWindowUntil.load() - now) > 0 ? OtaWindow : 0);
    for (unsigned i = 0; i < 4; ++i) telemetry.qre[i] = qre[i];
    for (unsigned i = 0; i < 3; ++i) {
      telemetry.distance[i] = sensors.ranges[i].mm;
      telemetry.range_age_ms[i] = std::min<uint32_t>(65535, now - sensors.ranges[i].sampledMs);
    }
    telemetry.requested[0] = requested.left; telemetry.requested[1] = requested.right;
    for (unsigned i = 0; i < 2; ++i) { telemetry.applied[i] = motors[i].direction * motors[i].duty; telemetry.fg[i] = fg[i]; }
    const uint32_t duration = esp_timer_get_time() - began;
    telemetry.control_max_us = std::max(telemetry.control_max_us, duration);
    if (duration > 5000 || now - lastBegan > 7) telemetry.deadline_misses++;
    lastBegan = now;
    xQueueOverwrite(telemetryQueue, &telemetry);
    gpio_set_level(GPIO_NUM_40, faulted ? (now / 100) % 2 : controller.state != State::Stopped);
    esp_task_wdt_reset();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}
static void serviceTask(void *) {
  usb_serial_jtag_driver_config_t usb{}; usb.rx_buffer_size = 1024; usb.tx_buffer_size = 512;
  check(usb_serial_jtag_driver_install(&usb));
  char line[564]{}; size_t used = 0; bool overflow = false;
  while (true) {
    if (!bootVerified && nowMs() >= 5000) {
      SensorFrame sample{}; bool healthy = !faulted && xQueuePeek(sensorQueue, &sample, 0);
      for (auto range : sample.ranges) if (!range.sampledMs || nowMs() - range.sampledMs > RANGE_STALE_MS) healthy = false;
      esp_ota_img_states_t state;
      if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        // OTA candidates must retain working network tasks; first USB provisioning can be offline.
        if (!radioHeartbeat || nowMs() - radioHeartbeat.load() > 200) healthy = false;
        if (healthy) healthy = check(esp_ota_mark_app_valid_cancel_rollback());
        else { esp_ota_mark_app_invalid_rollback_and_reboot(); faulted = true; }
      }
      bootVerified = healthy;
    }
    CalibrationRecord record;
    if (xQueueReceive(saveQueue, &record, 0)) {
      nvs_handle_t nvs; esp_err_t error = nvs_open("bottlesumo", NVS_READWRITE, &nvs);
      if (error == ESP_OK) {
        error = nvs_set_blob(nvs, "qre", &record, sizeof(record));
        if (error == ESP_OK) error = nvs_commit(nvs);
        nvs_close(nvs);
      }
      check(error);
      storagePending = false;
    }
    char command;
    if (usb_serial_jtag_read_bytes(&command, 1, pdMS_TO_TICKS(10)) == 1) {
      if (command == '\r') continue;
      if (command == '\n') {
        if (used >= 4 && !memcmp(line, "NET:", 4)) {
          NetworkSettings value{}; Telemetry t{}; xQueuePeek(telemetryQueue, &t, 0);
          bool saved = !overflow && parseNetworkLine(line, used, value) && !t.applied[0] && !t.applied[1] &&
            ((t.flags & Bench) || t.state == 0) && !otaLocked;
          bool expected = false;
          saved = saved && otaRequested.compare_exchange_strong(expected, true);
          if (saved) {
            const uint32_t start = nowMs();
            while (!otaLocked && nowMs() - start < 250) vTaskDelay(pdMS_TO_TICKS(5));
            saved = otaLocked && storeWirelessConfig(value);
            otaLocked = false; otaRequested = false;
          }
          const char *reply = saved ? "NET:OK\n" : "NET:REJECT\n";
          usb_serial_jtag_write_bytes(reply, strlen(reply), pdMS_TO_TICKS(100));
        }
        used = 0; overflow = false;
      } else if (used < sizeof(line)-1) line[used++] = command;
      else overflow = true;
      // Single-letter commands only at line start; never interpret credential hex as commands.
      if (used == 1 && (command == 'f' || command == 'e' || command == 'x')) {
        xQueueSend(serviceQueue, &command, 0); used = 0;
      }
      if (used == 1 && (command == 'q' || command == '?')) {
        Telemetry t{}; xQueuePeek(telemetryQueue, &t, 0);
        printf("state=%u elapsed=%lu flags=%u edges=%u max_control_us=%lu misses=%lu; f=floor e=void x=stop\n",
          t.state, static_cast<unsigned long>(t.state_elapsed_ms), t.flags, t.edges,
          static_cast<unsigned long>(t.control_max_us), static_cast<unsigned long>(t.deadline_misses));
        used = 0;
      }
    }
  }
}
#endif
}

#ifndef BOTTLESUMO_RECEIVER
extern "C" void app_main() {
  using namespace BottleSumo;
  bootId = esp_random();
  for (unsigned i = 0; i < 2; ++i) {
    gpio_set_direction(PWM_PINS[i], GPIO_MODE_OUTPUT); gpio_set_level(PWM_PINS[i], 0);
    gpio_set_direction(DIR_PINS[i], GPIO_MODE_OUTPUT); gpio_set_level(DIR_PINS[i], !FORWARD_LEVEL[i]);
  }
  gpio_set_direction(GPIO_NUM_41, GPIO_MODE_INPUT); gpio_set_direction(GPIO_NUM_42, GPIO_MODE_INPUT);
  gpio_set_direction(GPIO_NUM_40, GPIO_MODE_OUTPUT); gpio_set_direction(GPIO_NUM_18, GPIO_MODE_INPUT);
  bench = gpio_get_level(GPIO_NUM_42); // Physical boot selection; networking cannot change mode.
  check(nvs_flash_init()); // Never erase calibration automatically on init errors.
  if (esp_psram_get_size() != 8 * 1024 * 1024) faulted = true;
  nvs_handle_t nvs; CalibrationRecord record{}; size_t size = sizeof(record);
  if (nvs_open("bottlesumo", NVS_READONLY, &nvs) == ESP_OK) {
    const esp_err_t result = nvs_get_blob(nvs, "qre", &record, &size); nvs_close(nvs);
    if (result == ESP_OK && size == sizeof(record) && record.magic == CAL_MAGIC && record.values.valid()) {
      calibration = record.values; calibrated = floorCaptured = true;
    }
  }
  adc_oneshot_unit_init_cfg_t adcCfg{}; adcCfg.unit_id = ADC_UNIT_1;
  check(adc_oneshot_new_unit(&adcCfg, &adc));
  adc_oneshot_chan_cfg_t chan{}; chan.atten = ADC_ATTEN_DB_12; chan.bitwidth = ADC_BITWIDTH_12;
  for (auto ch : QRE_CHANNELS) check(adc_oneshot_config_channel(adc, ch, &chan));
  ledc_timer_config_t timer{}; timer.speed_mode = LEDC_LOW_SPEED_MODE; timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_0; timer.freq_hz = 20000; timer.clk_cfg = LEDC_AUTO_CLK; check(ledc_timer_config(&timer));
  for (unsigned i = 0; i < 2; ++i) {
    ledc_channel_config_t channel{}; channel.gpio_num = PWM_PINS[i]; channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = static_cast<ledc_channel_t>(i); channel.timer_sel = LEDC_TIMER_0; check(ledc_channel_config(&channel));
    gpio_set_direction(FG_PINS[i], GPIO_MODE_INPUT);
  }
  if (FG_CONNECTED) {
    check(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    for (unsigned i = 0; i < 2; ++i) { check(gpio_set_intr_type(FG_PINS[i], GPIO_INTR_POSEDGE));
      check(gpio_isr_handler_add(FG_PINS[i], fgInterrupt, reinterpret_cast<void *>(i))); }
  }
  sensorQueue = xQueueCreate(1, sizeof(SensorFrame)); telemetryQueue = xQueueCreate(1, sizeof(Telemetry));
  commandQueue = xQueueCreate(1, sizeof(ReceivedDrive)); serviceQueue = xQueueCreate(4, sizeof(char));
  saveQueue = xQueueCreate(1, sizeof(CalibrationRecord));
  configASSERT(sensorQueue && telemetryQueue && commandQueue && serviceQueue && saveQueue);
  configASSERT(xTaskCreatePinnedToCore(controlTask, "control", 6144, nullptr, 20, nullptr, 1) == pdPASS);
  configASSERT(xTaskCreatePinnedToCore(sensorTask, "tof", 6144, nullptr, 6, nullptr, 0) == pdPASS);
  configASSERT(xTaskCreatePinnedToCore(serviceTask, "service", 4096, nullptr, 2, nullptr, 0) == pdPASS);
  if (wirelessInit(false)) configASSERT(xTaskCreatePinnedToCore(wirelessTask, "wireless", 6144, nullptr, 3, nullptr, 0) == pdPASS);
}
#endif
