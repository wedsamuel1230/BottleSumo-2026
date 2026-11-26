#include "VL53L0X.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
std::array<uint8_t, 256> regs;
int transfers, fail_on, adds, removes;
bool fail_add, fail_remove, spad_stuck, interrupt_stuck, start_pending;
int64_t now_us;
uint16_t device_address;
void reset()
{
  regs.fill(0);
  regs[0xC0] = 0xEE;
  regs[0x91] = 0xA5;
  transfers = fail_on = adds = removes = 0;
  fail_add = fail_remove = spad_stuck = interrupt_stuck = start_pending = false;
  now_us = 0;
}
void setup(VL53L0X& sensor)
{
  assert(sensor.begin(reinterpret_cast<void*>(1)));
  assert(sensor.init());
  assert(sensor.setMeasurementTimingBudget(20000));
}
bool fails() { return ++transfers == fail_on; }
}

int64_t esp_timer_get_time() { now_us += 1000; return now_us; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* cfg, i2c_master_dev_handle_t* device)
{
  assert(bus && cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7 && cfg->scl_speed_hz == 400000);
  ++adds;
  if (fail_add) { return ESP_FAIL; }
  device_address = cfg->device_address;
  *device = reinterpret_cast<void*>(2);
  return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
  assert(device);
  ++removes;
  return fail_remove ? ESP_FAIL : ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t* data, size_t len, int timeout)
{
  assert(device && timeout == 3 && len >= 2);
  if (fails()) { return ESP_FAIL; }
  std::memcpy(regs.data() + data[0], data + 1, len - 1);
  return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t* tx, size_t txlen, uint8_t* rx, size_t rxlen, int timeout)
{
  assert(device && timeout == 3 && txlen == 1);
  if (fails()) { return ESP_FAIL; }
  std::memcpy(rx, regs.data() + tx[0], rxlen);
  if (tx[0] == 0x83) { rx[0] = spad_stuck ? 0 : 1; }
  if (tx[0] == 0x92) { rx[0] = 0x85; }
  if (tx[0] == 0x13) { rx[0] = interrupt_stuck ? 0 : 7; }
  if (tx[0] == 0x00) { rx[0] = start_pending ? 1 : 0; }
  if (tx[0] == 0x14 && rxlen == 12) { rx[0] = 0x58; rx[10] = 0x04; rx[11] = 0xD2; }
  return ESP_OK;
}

int main()
{
  int init_transfers;
  reset();
  {
    VL53L0X sensor;
    assert(!sensor.startSingle());
    setup(sensor);
    init_transfers = transfers;
    assert(device_address == 0x29);
    assert(!sensor.setAddress(0x02));
    assert(sensor.healthy());
    assert(sensor.setAddress(0x30));
    assert(device_address == 0x30 && adds == 2 && removes == 1);
    int before = transfers;
    assert(sensor.startSingle() && transfers - before == 8);
    assert(!sensor.startSingle());
    assert(!sensor.setAddress(0x31));
    assert(!sensor.setMeasurementTimingBudget(20000));
    bool ready = true;
    start_pending = true;
    before = transfers;
    assert(sensor.dataReady(ready) && !ready && transfers - before == 1);
    start_pending = false;
    interrupt_stuck = true;
    assert(sensor.dataReady(ready) && !ready);
    uint16_t mm = 99; uint8_t status = 99;
    assert(!sensor.readResult(mm, status) && mm == 99 && status == 99);
    interrupt_stuck = false;
    assert(sensor.readResult(mm, status) && mm == 1234 && status == 11);
    assert(regs[0x0B] == 1 && sensor.startSingle());
  }
  assert(removes == 2);
  // Every init/budget transaction must fail closed and suppress later transfers.
  for (int i = 1; i <= init_transfers; ++i)
  {
    reset(); fail_on = i;
    VL53L0X sensor;
    assert(sensor.begin(reinterpret_cast<void*>(1)));
    bool ok = sensor.init();
    if (ok) { ok = sensor.setMeasurementTimingBudget(20000); }
    assert(!ok && !sensor.healthy() && transfers == i);
    assert(!sensor.startSingle() && transfers == i);
  }
  // Startup write failure, readiness failure, frame failure, interrupt-clear failure.
  for (int i = 1; i <= 12; ++i)
  {
    reset();
    VL53L0X sensor; setup(sensor);
    const int before = transfers;
    fail_on = before + i;
    bool ok = sensor.startSingle();
    if (ok) { uint16_t mm = 99; uint8_t status = 99; ok = sensor.readResult(mm, status); assert(mm == 99 && status == 99); }
    assert(!ok && !sensor.healthy() && transfers == before + i);
    bool ready = true;
    assert(!sensor.dataReady(ready) && !ready);
    assert(!sensor.startSingle() && transfers == before + i);
  }
  for (bool spad : {true, false})
  {
    reset(); spad_stuck = spad; interrupt_stuck = !spad;
    VL53L0X sensor;
    assert(sensor.begin(reinterpret_cast<void*>(1)));
    assert(!sensor.init() && !sensor.healthy() && now_us < 110000);
  }
  for (int stage = 0; stage < 3; ++stage)
  {
    reset();
    VL53L0X sensor;
    if (stage == 0) { fail_add = true; assert(!sensor.begin(reinterpret_cast<void*>(1))); }
    else
    {
      setup(sensor);
      if (stage == 1) { fail_remove = true; }
      else { fail_add = true; }
      assert(!sensor.setAddress(0x31));
    }
    assert(!sensor.healthy() && !sensor.begin(reinterpret_cast<void*>(1)));
  }
  std::printf("VL53L0X host checks passed (%d init/budget fault points, 12 runtime fault points)\n", init_transfers);
}
