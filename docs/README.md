# BottleSumo documentation

Use this page as the documentation index. Documents are grouped by the task
they support, and image assets are kept in separate folders so the reference
pages stay easy to scan.

## Competition

- [ESP-IDF wireless and debugging guide](competition/esp-idf-wireless.md) - active N16R8 firmware, FreeRTOS timings, UDP/ESP-NOW, bench dashboard and OTA.

- [2026 table-sumo rules](competition/table-sumo-rules-2026.md) - neutral project transcription of the attached Rev.0 rules document.
- [Sensor and strategy design v1](competition/table-sumo-design-v1.md) - research and design proposal with its approval boundary.
- [Design evidence ledger](competition/table-sumo-evidence-ledger.md) - source and evidence register for the design proposal.
- [Competition firmware strategy and commissioning](competition/competition-firmware.md) - autonomous v2 program, three-head ToF wiring, Rev.1 rule analysis and physical validation gates.
- [bottlesumo_ai assembly timeline](bottlesumo_ai-assembly-timeline.md) - ordered parametric chassis and assembly definition.
- [ADR 0001: one-piece parametric PLA chassis](adr/0001-one-piece-parametric-pla-chassis.md) - decision record for the mechanical architecture.
- [bottlesumo_ai Fusion isometric capture](../3D%20model/bottlesumo_ai/images/bottlesumo_ai-v10-iso-top-right.png) - latest MCP-generated assembly view.
- [bottlesumo_ai verification report](bottlesumo_ai-verification-report.md) - current Fusion evidence and remaining physical gates.

## Hardware

- [Active N16R8 pinout](hardware/esp32-s3-devkitc-1-n16r8-pinout.md) - native ESP-IDF controller and three-head mapping.

- [ESP32-S3-DEVKITC-1-N32R16 pinout](hardware/esp32-s3-devkitc-1-n32r16-pinout.md) - board, GPIO, motor FG, LED, and firmware audit.
- [24GP-2430 motor reference](hardware/motor-24gp-2430-reference.md) - motor listing values, FG notes, and wiring cautions.

## CI and development

- [PlatformIO firmware management](ci/platformio-firmware.md) - all native ESP-IDF and Arduino projects, their pinned toolchains, and build commands.
- [Legacy Arduino CI review](ci/arduino-ci-review.md) - historical validation record and residual hardware risk.
- [Arduino sensor library matrix](ci/arduino-libraries.md) - candidate library names and versions; active dependencies are pinned in PlatformIO project configs.

## Images

### Competition

- [Arena layout](images/competition/arena-layout.png)
- [Bottle dimensions](images/competition/bottle-dimensions.png)
- [Intentional-push ruling](images/competition/intentional-push-ruling.png)

### Motor

- [64:1 performance table](images/motor/motor-24gp-2430-64-to-1-specification.png)
- [Motor dimensions](images/motor/motor-24gp-2430-dimensions.png)
- [FG pulse description](images/motor/motor-24gp-2430-fg-pulse-description.png)
- [Motor wiring and FG](images/motor/motor-24gp-2430-wire-and-fg.png)
