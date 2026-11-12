#pragma once

// ==================== Wi-Fi ====================
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// ==================== OTA (Wi-Fi Upload Without BOOT Button) ====================
#define OTA_HOSTNAME  "cube-esp32"

// ==================== Timezone & NTP ====================
// Default: Kyiv (UTC+2 winter / UTC+3 summer, POSIX TZ rule for automatic DST)
#define TZ_INFO       "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.nist.gov"

// ==================== Weather (Open-Meteo, No API Key Required) ====================
// Default: Kyiv (replace with your city name and coordinates)
#define CITY_NAME     "KYIV"
#define LATITUDE      "50.4501"
#define LONGITUDE     "30.5234"
#define WEATHER_UPDATE_INTERVAL_MS (15UL * 60UL * 1000UL) // 15 minutes

// ==================== SHT30 Sensor (I2C) ====================
#define SHT30_VCC_PIN 25 // Power SHT30 VIN directly from GPIO25 (< 1.5 mA load)
#define SHT30_SDA_PIN 21
#define SHT30_SCL_PIN 22
#define SHT30_ADDR_1  0x44
#define SHT30_ADDR_2  0x45
#define SENSOR_UPDATE_INTERVAL_MS 10000UL // 10 seconds

// --- Dynamic Enclosure Warmup Thermal Compensation ---
// Inside the enclosure, self-heating causes ~+1.0C error at cold start and drifts to ~+4.0C at steady state.
// An exponential curve smoothly transitions the offset from SHT30_OFFSET_START to SHT30_OFFSET_WARM:
#define SHT30_OFFSET_START        -1.0f // Temperature offset right after cold power-on (deg C)
#define SHT30_OFFSET_WARM         -4.0f // Steady-state temperature offset after full warmup (deg C)
#define SHT30_WARMUP_TAU_MIN      10.0f // Thermal warmup time constant in minutes (~95% settled at 3*TAU = 30 min)
#define SHT30_COMPENSATE_HUMIDITY 1     // 1 = recalculate relative humidity using the Magnus formula

// ==================== Display ====================
#define SCREEN_SWITCH_INTERVAL_MS 8000UL // Auto-cycle screen interval (ms)
#define DISPLAY_ROTATION          0      // 0..3
