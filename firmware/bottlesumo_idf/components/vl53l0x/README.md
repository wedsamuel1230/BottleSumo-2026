# Native ESP-IDF VL53L0X component

Adapted from [Pololu VL53L0X 1.3.1](https://github.com/pololu/vl53l0x-arduino/tree/1.3.1),
using the local Arduino library whose `library.properties` reports `1.3.1`.
Initialization, default tuning, reference calibrations, and measurement-budget
calculations retain the vendor sequence. [LICENSE.txt](LICENSE.txt) preserves
both the complete Pololu and ST license notices.

The transport uses ESP-IDF `driver/i2c_master.h` and `esp_timer_get_time()`;
there is no Arduino or Wire dependency. Each transfer requests a 3 ms driver
timeout. This is an API timeout, not a measured hard real-time deadline; IDF
scheduler granularity and bus contention can affect elapsed time. SPAD and
reference-calibration polls have a 100 ms timeout each and run only during
initialization. Continuous measurement, blocking runtime range waits, and
unused upstream setters are omitted.

## Ownership and use

The sensor task creates the 400 kHz bus (SDA17/SCL16) and owns XSHUT6/7/15.
Initialize one head at a time while the remaining heads stay shut down, then
assign distinct addresses (0x30/0x31/0x32). Only that task accesses these objects.
The component owns its device handle and removes it on destruction; it never
removes the shared bus. Keep that bus alive until all sensor objects are destroyed.
Objects cannot be copied or moved.

1. `begin(bus)` attaches a device at the power-on 7-bit address 0x29.
2. `init()` performs the vendor initialization and reference calibration.
3. `setAddress(address)` writes the new address and recreates the IDF device
   handle. Valid addresses are 0x08 through 0x77.
4. `setMeasurementTimingBudget(20000)` applies the vendor timing calculation.
5. `startSingle()` restores the saved stop variable and starts one shot.
6. Call `dataReady(ready)` once per task iteration. A true return with
   `ready == false` means the shot is still starting or measuring.
7. When ready, `readResult(mm, rawStatus)` reads all 12 result bytes and clears
   the interrupt. `rawStatus = (frame[0] & 0x78) >> 3` is the device's raw status,
   not the ST API's translated status. The caller decides whether to use it.

`startSingle()` refuses a second shot until the preceding result is consumed.
Address and timing changes are refused during a shot. `readResult()` checks
readiness itself; its output values change only after the complete frame and
interrupt clear both succeed. The caller owns the runtime shot deadline and
must shut down/fault a head that never becomes ready.

Every transport error and initialization polling timeout permanently latches
`healthy() == false`; later calls cannot clear or hide it. Public operations
return false and issue no later I2C transfers after a latched fault. Recovery
requires destroying the object, resetting the head through XSHUT, and creating
a new object. Invalid arguments or operations return false without latching a
transport fault. A model-ID mismatch also rejects initialization.

## Host check

Run from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I firmware/bottlesumo_idf/components/vl53l0x/tests/stubs \
  -I firmware/bottlesumo_idf/components/vl53l0x/include \
  firmware/bottlesumo_idf/components/vl53l0x/VL53L0X.cpp \
  firmware/bottlesumo_idf/components/vl53l0x/tests/host_check.cpp \
  -o /tmp/bottlesumo-vl53l0x-host-check
/tmp/bottlesumo-vl53l0x-host-check
```

The fake transport checks successful addressing/ranging, full-frame extraction,
start-bit and interrupt readiness, output preservation on failures, every
initialization/budget transfer failure, all runtime transfer failures, address
handle errors, ownership cleanup, and both 100 ms initialization poll timeouts.
The stubs are used only by this host check; the ESP-IDF component build compiles
`VL53L0X.cpp` against the actual IDF headers. The host check does not prove
hardware readings, signal levels, crosstalk, or physical timing.
