// wifi_defaults.h — compile-time bench credentials, merged from movement_test.
//
// Copy wifi_local.h.example to wifi_local.h (gitignored) or generate it with
// ./scripts/make-wifi-header.sh --sketch bottlesumo_arduino. Runtime config in
// NVS overrides these defaults once the web UI has saved one.
#pragma once

#if __has_include("wifi_local.h")
#include "wifi_local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef AP_SSID
#define AP_SSID "BottleSumo"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "bottlesumo"
#endif
