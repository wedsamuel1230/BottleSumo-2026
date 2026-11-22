#pragma once
namespace BottleSumo {
enum class TransportMode { AP_UDP, ESP_NOW, BOTH };
inline constexpr TransportMode TELEMETRY_MODE = TransportMode::BOTH;
}
// Credentials are USB-provisioned into NVS; they are never embedded in OTA images.
