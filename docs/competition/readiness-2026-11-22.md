# Competition readiness — 2026-11-22

Status: **not ready**. Nothing below is a claim of a completed test; each row
states what was actually observed and what is still open.

## Firmware

| Item | State | Evidence |
| --- | --- | --- |
| Merged image (AP+STA, mDNS, ArduinoOTA, trim table, one ToF) | built, **not verified on the robot** | `arduino-cli compile -b esp32:esp32:bottlesumo_n16r8` clean; sha256 `c3561fc5…` |
| Robot reachable on the LAN | yes | `192.168.128.137`, `/api/status` has `ip` and `channel` |
| ToF restart endpoint | yes | `POST /api/tof-restart` returns `{"ok":1}` and re-runs `TofState::begin()` |
| ArduinoOTA | in the image, unexercised | literals present; no upload attempted |

## Sensors

| Item | State | Evidence |
| --- | --- | --- |
| QRE order | fixed in source | `PIN_QRE = {4,1,5,2}` (front-left, front-right, rear-left, rear-right), `CAL_MAGIC` bumped to `BSM5` |
| QRE readings | **not tested** | `/api/status` has never been read on the new image |
| QRE calibration | **not done** | needs floor and void capture on the real table |
| ToF head count | one (center, index 1, XSHUT GPIO7) | `TOF_HEAD_COUNT = 1` |
| ToF ranging | **failing** | `tofError` = `probe at 0x29` after a restart, `init` before it; `distance` all 0 |
| I2C bus | healthy, **not** shorted | the safe scan (run inside the ToF task, raw codes) acks only `0x31`; the earlier 40-address result was two tasks sharing `Wire` |
| ToF address | stale after a warm reset | the module stays at the assigned `0x31` because XSHUT does not power-cycle it; `begin()` probed `0x29` only, so init failed |
| QRE raw | present, suspicious | `qre` = 82 / 85 / 2533 / 2758 (front pair far lower than rear) |

## Drive

| Item | State | Evidence |
| --- | --- | --- |
| `FORWARD_LEVEL` | `{true,false}`, **unverified on this chassis** | matches the movement_test bench default (opposite levels drive straight); the previous `{true,true}` would spin |
| `PWM_ACTIVE_LOW` | `{true,true}`, unverified | matches the movement_test bench finding |
| Trim table | source only, untested on hardware | four duty points, NVS `trimtab` |
| Bench gate | `HARDWARE_CONFIRMED = false` blocks round start only; bench mode (SW2 at boot) is **not** gated | `canStartRound()` reads the flag; bench path does not |

## Gates before a competition round

1. Robot on the LAN and the new image confirmed by a `channel` field in
   `/api/status`.
2. ToF distance changes when an object is placed in front; `tofFresh` is 1.
3. QRE raw values respond on all four channels; floor and void capture pass
   with a gap of at least 300 counts each.
4. Direction check with the wheels off the ground (bench mode): forward drives
   straight, reverse backs up, spin left/right turn the right way.
5. Electrical confirmation of the motor wiring (the user's call) before
   `HARDWARE_CONFIRMED` is set true.
6. One full round: start, stop, edge escape, and the 120 s timeout.

## Open decisions

- `FORWARD_LEVEL`: keep `{true,false}` (bench default) or revert to `{true,true}`.
- Who performs the phone's Wi-Fi switch to `BottleSumo`: the user (one tap) or a
  blind ADB UI automation.
