#pragma once

// ==================== Wi-Fi ====================
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// ==================== OTA (прошивка по Wi-Fi без кнопки BOOT) ====================
#define OTA_HOSTNAME  "cube-esp32"

// ==================== Часовой пояс (NTP) ====================
// Киев: UTC+2 (зима) / UTC+3 (лето), правило POSIX TZ для автоматического перехода:
#define TZ_INFO       "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.nist.gov"

// ==================== Погода (Open-Meteo, без API-ключа) ====================
// По умолчанию: Киев (укажите свои координаты и название города)
#define CITY_NAME     "KYIV"
#define LATITUDE      "50.4501"
#define LONGITUDE     "30.5234"
#define WEATHER_UPDATE_INTERVAL_MS (15UL * 60UL * 1000UL) // 15 минут

// ==================== Датчик SHT30 (I2C) ====================
#define SHT30_VCC_PIN 25 // Живлення VIN датчика від GPIO25
#define SHT30_SDA_PIN 21
#define SHT30_SCL_PIN 22
#define SHT30_ADDR_1  0x44
#define SHT30_ADDR_2  0x45
#define SENSOR_UPDATE_INTERVAL_MS 10000UL // 10 секунд

// --- Динамическая компенсация прогрева платы (Вариант 2) ---
// На холодном старте датчик завышает на ~1.0C, а после полного прогрева — на ~4.0C.
// Кривая плавно меняет поправку от SHT30_OFFSET_START до SHT30_OFFSET_WARM:
#define SHT30_OFFSET_START        -1.0f // Поправка сразу после холодного включения (°C)
#define SHT30_OFFSET_WARM         -4.0f // Поправка после полного прогрева устройства (°C)
#define SHT30_WARMUP_TAU_MIN      10.0f // Постоянная времени прогрева в минутах (~95% за 3*TAU = 30 мин)
#define SHT30_COMPENSATE_HUMIDITY 1     // 1 = пересчитывать влажность по формуле Магнуса с учётом нагрева

// ==================== Экраны ====================
// Интервал автоматического переключения экранов (мс)
#define SCREEN_SWITCH_INTERVAL_MS 8000UL // 8 секунд
#define DISPLAY_ROTATION          0      // 0..3
