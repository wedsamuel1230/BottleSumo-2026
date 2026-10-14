# BottleSumo desktop debugger

Use `uv` on macOS or Windows. UDP, localhost dashboard, configuration and OTA
use the standard library. USB provisioning and USB ESP-NOW transport run with
`pyserial==3.5` through `uv`.

From the repository root:

```sh
uv sync --frozen
uv run --frozen tools/bottlesumo_debug/debugger.py configure --robot-mac AA:BB:CC:DD:EE:01 --receiver-mac AA:BB:CC:DD:EE:02
uv run --frozen tools/bottlesumo_debug/debugger.py gui
uv run --frozen tools/bottlesumo_debug/debugger.py gui --serial /dev/cu.usbmodem123
uv run --frozen tools/bottlesumo_debug/debugger.py gui --serial COM5
```

The launchers use the same `uv` environment, set a consistent working
directory, start the dashboard, and pass extra arguments to `gui` (including
`--serial`, `--port 0`, `--no-open`). Port 8765 is the default; port 0 selects an
available localhost port. Open the printed `http://127.0.0.1:<port>` URL.

`configure` generates only `wireless.local.json`, a private local file;
never commit or share it. Credentials are provisioned into the device's NVS over
USB and are not embedded in a firmware header or an OTA image. Existing
credentials are preserved on reconfigure; only explicitly supplied MAC addresses
are changed. Unknown MACs default to zeros and must be replaced before encrypted
ESP-NOW pairing. The committed `wireless.example.json` contains no credentials.
Only the output path is printed. POSIX output mode is 0600; on Windows,
filesystem ACLs remain responsible for access control.

Initial setup: run `configure`, build and USB-flash the robot/receiver firmware,
then read each device's MAC address from its boot output. Run `configure` again
with both peer MAC addresses before provisioning. With each device stopped,
provision it explicitly over its own USB port and reset it after acknowledgement:

```sh
uv run --frozen tools/bottlesumo_debug/debugger.py provision --serial /dev/cu.usbmodem123
uv run --frozen tools/bottlesumo_debug/debugger.py provision --serial COM5
```

Use the same private JSON for both robot and receiver. Firmware locks outputs
while validating and storing the wireless settings, preserving existing sensor
calibration in NVS. The CLI sends one `NET:` frame and waits up to 15 seconds for
`NET:OK` or `NET:REJECT`. It never prints the frame or credentials, does not reset
the device, and does not retry after an ambiguous timeout. Inspect the device
before retrying. After manual reset, privately read the WPA2 password from the
local JSON, join the robot's **BottleSumo** AP, and start `gui`.

Optional router connection: privately edit `sta_ssid` and `sta_password` in
`wireless.local.json`, then rerun `configure` and explicitly provision both
devices again to update NVS. No password CLI argument is accepted. Leave both
fields blank to disable STA. SSID supports up to 32 UTF-8 bytes with no
controls/NUL; a configured SSID requires an 8–63 character printable ASCII WPA2
password. Reconfiguring an old seven-field file adds blank STA fields while
retaining every existing credential.

Select `TransportMode::AP_UDP`, `ESP_NOW`, or `BOTH` in the robot's public
`firmware/bottlesumo_idf/main/network_config.h` (`TELEMETRY_MODE`). There is no
desktop transport-mode setting to change the robot. The robot keeps its AP while
connecting to the optional router in AP+STA mode. The router's channel determines
the active AP channel; the ESP-NOW receiver follows it by associating with the
robot's **BottleSumo** AP. The receiver does not use the optional router STA
credentials. The AP supports two clients for a desktop plus receiver. Firmware
does not reconnect STA during an autonomous round.

UDP binds local port 4242 and sends `SUB\n` to `192.168.4.1:4242` once a second.
Allow the private-network firewall prompt. The ESP-NOW receiver instead emits
`T:<168 hex digits>` lines at 115200 baud and accepts `C:<104 hex digits>`.
Both use the same 84-byte telemetry parser, boot/sequence ordering, gap count,
and last-received age. Sequence arithmetic handles uint32 wrap. Local packet
age is **not one-way latency**; clocks are not synchronized.

The dashboard shows QRE, distance and range age, requested/applied PWM, FG,
action state and duration, control maximum time, missed deadlines and transport
counters. State timeline starts derive from `uptime_ms - state_elapsed_ms`;
state numbers correspond to the firmware enum. Short states between received
packets can be missed. Graphs retain the most recent 240 accepted packets.

Use physically selected bench mode, calibrated sensors and confirmed hardware
before driving. Fault/updating flags or telemetry older than 300 ms disable
control. Hold the button or W/A/S/D; Space stops. Button release, pointer cancel,
window blur, page hiding and navigation request STOP. Commands refresh every
40 ms; the server retains only the current command for 100 ms, rejects reordered
HTTP requests, and sends STOP on lease expiry or telemetry/boot changes. Firmware
owns the final 150 ms lease, edge/physical-stop overrides and ±120 bench limit.
A browser/OS/network stall cannot guarantee a delivered STOP; the firmware lease
must remain enabled. Requested PWM is a motor command, **not measured vehicle
speed**. FG counts are shown without assuming a shaft ratio or calibration.

OTA is an explicit CLI operation while the robot's physical OTA window is open:

```sh
uv run --frozen tools/bottlesumo_debug/debugger.py ota --firmware path/to/bottlesumo.bin
```

The CLI reads `/status`, signs the current boot nonce, image length and SHA256,
then uploads raw bytes to `/ota`. Firmware verifies the physical window and
excludes autonomous rounds. No retry occurs after an ambiguous upload. Inspect
robot status and running image before trying again. Wi-Fi controls and OTA are
not available during an autonomous round. No token, HMAC, password or request
headers are logged. OTA is over the local AP; the firmware image is not encrypted
by this client.

Host-only verification:

```sh
uv run --frozen python -m unittest discover -s tests -p test_bottlesumo_debug.py -v
```

These tests do not validate physical wiring, motion, sensors, wireless pairing,
serial drivers, real OTA, or macOS/Windows launcher execution.
