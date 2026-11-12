#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <esp_system.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>

#include "config.h"

TFT_eSPI tft = TFT_eSPI();
// Reusable 228x90 16-bit off-screen sprite (~41 KB RAM) to render all UI zones without flicker or black flashes
TFT_eSprite panel = TFT_eSprite(&tft);
bool panelReady = false;

// RTC memory preserved across OTA updates and software resets so the device tracks cumulative warmup time
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcWarmupSeconds;
#define RTC_WARMUP_MAGIC 0xC0BE2026UL

// ==================== Data State ====================
struct WeatherData {
    bool valid = false;
    float temp = 0.0f;
    float apparentTemp = 0.0f;
    float tempMin = 0.0f;
    float tempMax = 0.0f;
    int humidity = 0;
    float windSpeed = 0.0f;
    int pressureMmHg = 760;
    int weatherCode = 0;
};

struct RoomData {
    bool connected = false;
    uint8_t address = 0;
    float rawTemp = 0.0f;
    float rawHumidity = 0.0f;
    float temp = 0.0f;
    float humidity = 0.0f;
    float currentOffset = 0.0f;
    uint16_t statusReg = 0;
    bool heaterOn = false;
};

enum ScreenMode {
    SCREEN_DASHBOARD = 0, // Main MeteoCube dashboard (Clock + Weather + Indoor)
    SCREEN_WEATHER   = 1, // Detailed outdoor weather screen
    SCREEN_ROOM      = 2, // Detailed indoor climate screen (SHT30)
    SCREEN_COUNT     = 3
};

WeatherData weather;
RoomData room;
ScreenMode currentScreen = SCREEN_DASHBOARD;

unsigned long lastScreenSwitchMs = 0;
unsigned long lastWeatherUpdateMs = 0;
unsigned long lastSensorUpdateMs = 0;
unsigned long lastClockTickMs = 0;
unsigned long lastUptimeTrackMs = 0;
bool otaInitialized = false;
bool otaInProgress = false;

// ==================== MeteoCube UI Color Palette (RGB565) ====================
#define C_BG        0x0000 // Deep pure black
#define C_PANEL     0x10A3 // Dark slate card background (#101418)
#define C_PANEL_2   0x1925 // Secondary card/badge surface (#182428)
#define C_BORDER    0x29C7 // Subtle card border
#define C_CYAN      0x07FF // Neon cyan
#define C_SKY       0x3DDF // Soft sky blue
#define C_ORANGE    0xFD20 // Primary accent orange (#FFA500)
#define C_AMBER     0xFE60 // Warm amber
#define C_GREEN     0x07E8 // Neon green
#define C_MINT      0x27F3 // Mint green
#define C_PINK      0xF993 // Coral pink
#define C_RED       0xF800 // Red
#define C_WHITE     0xFFFF // Pure white
#define C_GRAY      0x8430 // Muted gray text
#define C_DARKGRAY  0x3186 // Progress bar track

// ==================== SHT30 Dynamic Thermal Drift Compensation ====================
float getCurrentTempOffset() {
    float warmupMin = (float)rtcWarmupSeconds / 60.0f;
    float tau = (SHT30_WARMUP_TAU_MIN > 0.1f) ? SHT30_WARMUP_TAU_MIN : 0.1f;
    // Exponential thermal warmup curve from SHT30_OFFSET_START (t=0) toward SHT30_OFFSET_WARM
    float factor = 1.0f - expf(-warmupMin / tau);
    return SHT30_OFFSET_START + (SHT30_OFFSET_WARM - SHT30_OFFSET_START) * factor;
}

float compensateHumidityMagnus(float rawTempC, float realTempC, float rawRh) {
    // Saturation vapor pressure via the Magnus formula
    float esRaw  = 6.112f * expf((17.62f * rawTempC)  / (243.12f + rawTempC));
    float esReal = 6.112f * expf((17.62f * realTempC) / (243.12f + realTempC));
    if (esReal <= 0.01f) return rawRh;
    float realRh = rawRh * (esRaw / esReal);
    if (realRh > 100.0f) realRh = 100.0f;
    if (realRh < 0.0f)   realRh = 0.0f;
    return realRh;
}

// ==================== SHT30 (I2C with Internal Heater Verification) ====================
static uint8_t sht30Crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int j = len; j > 0; --j) {
        crc ^= *data++;
        for (int i = 8; i > 0; --i) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static bool probeSHT30Address(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

static bool readSHT30Status(uint8_t addr, uint16_t &statusOut) {
    Wire.beginTransmission(addr);
    Wire.write(0xF3);
    Wire.write(0x2D);
    if (Wire.endTransmission() != 0) return false;

    delay(2);
    if (Wire.requestFrom(addr, (uint8_t)3) != 3) return false;

    uint8_t b[3];
    b[0] = Wire.read();
    b[1] = Wire.read();
    b[2] = Wire.read();

    if (sht30Crc8(b, 2) != b[2]) return false;

    statusOut = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

static bool disableSHT30Heater(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(0x30);
    Wire.write(0x66);
    return (Wire.endTransmission() == 0);
}

void updateSHT30() {
    uint8_t addr = 0;
    if (probeSHT30Address(SHT30_ADDR_1)) {
        addr = SHT30_ADDR_1;
    } else if (probeSHT30Address(SHT30_ADDR_2)) {
        addr = SHT30_ADDR_2;
    } else {
        room.connected = false;
        room.address = 0;
        return;
    }

    uint16_t status = 0;
    if (readSHT30Status(addr, status)) {
        room.statusReg = status;
        room.heaterOn = (status & (1 << 13)) != 0;
        if (room.heaterOn) {
            disableSHT30Heater(addr);
            delay(5);
            if (readSHT30Status(addr, status)) {
                room.statusReg = status;
                room.heaterOn = (status & (1 << 13)) != 0;
            }
        }
    }

    Wire.beginTransmission(addr);
    Wire.write(0x24);
    Wire.write(0x0B);
    if (Wire.endTransmission() != 0) {
        room.connected = false;
        return;
    }

    delay(8);

    if (Wire.requestFrom(addr, (uint8_t)6) != 6) {
        room.connected = false;
        return;
    }

    uint8_t buf[6];
    for (int i = 0; i < 6; i++) {
        buf[i] = Wire.read();
    }

    if (sht30Crc8(&buf[0], 2) != buf[2] || sht30Crc8(&buf[3], 2) != buf[5]) {
        return;
    }

    uint16_t rawT = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t rawH = ((uint16_t)buf[3] << 8) | buf[4];

    room.rawTemp = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    room.rawHumidity = 100.0f * ((float)rawH / 65535.0f);
    room.currentOffset = getCurrentTempOffset();
    room.temp = room.rawTemp + room.currentOffset;

    #if SHT30_COMPENSATE_HUMIDITY
    room.humidity = compensateHumidityMagnus(room.rawTemp, room.temp, room.rawHumidity);
    #else
    room.humidity = room.rawHumidity;
    #endif

    room.address = addr;
    room.connected = true;

    Serial.printf("SHT30: rawT=%.2fC, offset=%.2fC -> T=%.2fC | rawH=%.1f%% -> H=%.1f%% | warmup=%lus\n",
                  room.rawTemp, room.currentOffset, room.temp, room.rawHumidity, room.humidity,
                  (unsigned long)rtcWarmupSeconds);
}

// ==================== Weather (Open-Meteo + ArduinoJson v7) ====================
const char* weatherCodeToText(int code) {
    switch (code) {
        case 0: return "CLEAR SKY";
        case 1: return "MAINLY CLEAR";
        case 2: return "PARTLY CLOUDY";
        case 3: return "OVERCAST";
        case 45:
        case 48: return "FOGGY";
        case 51:
        case 53:
        case 55: return "DRIZZLE";
        case 61:
        case 63:
        case 65: return "RAIN";
        case 71:
        case 73:
        case 75: return "SNOW";
        case 80:
        case 81:
        case 82: return "SHOWERS";
        case 95:
        case 96:
        case 99: return "STORM";
        default: return "CLOUDY";
    }
}

void updateWeather() {
    if (WiFi.status() != WL_CONNECTED) return;

    String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE +
                 "&longitude=" + LONGITUDE +
                 "&current=temperature_2m,relative_humidity_2m,apparent_temperature,surface_pressure,weather_code,wind_speed_10m"
                 "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1";

    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(url)) return;

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            JsonObject cur = doc["current"];
            if (!cur.isNull()) {
                weather.temp = cur["temperature_2m"] | 0.0f;
                weather.apparentTemp = cur["apparent_temperature"] | 0.0f;
                weather.humidity = cur["relative_humidity_2m"] | 0;
                weather.windSpeed = cur["wind_speed_10m"] | 0.0f;
                float pHpa = cur["surface_pressure"] | 1013.25f;
                weather.pressureMmHg = (int)(pHpa * 0.750062f + 0.5f);
                weather.weatherCode = cur["weather_code"] | 0;
                weather.valid = true;
            }
            JsonObject daily = doc["daily"];
            if (!daily.isNull()) {
                weather.tempMax = daily["temperature_2m_max"][0] | weather.temp;
                weather.tempMin = daily["temperature_2m_min"][0] | weather.temp;
            }
        }
    }
    http.end();
}

// ==================== UI Primitives & Vector Icons (Supports Both TFT_eSPI and TFT_eSprite) ====================

void drawProgressBarOn(TFT_eSPI &g, int x, int y, int w, int h, int percent, uint16_t fillColor) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int r = h / 2;
    g.fillRoundRect(x, y, w, h, r, C_DARKGRAY);
    int fillW = (w * percent) / 100;
    if (fillW >= h) {
        g.fillRoundRect(x, y, fillW, h, r, fillColor);
    } else if (fillW > 0) {
        g.fillCircle(x + r, y + r, r, fillColor);
    }
}

void drawThermometerIconOn(TFT_eSPI &g, int x, int y, uint16_t color) {
    g.drawRoundRect(x + 3, y, 6, 13, 3, C_WHITE);
    g.fillCircle(x + 6, y + 14, 5, color);
    g.fillRect(x + 5, y + 5, 2, 8, color);
}

void drawDropletIconOn(TFT_eSPI &g, int x, int y, uint16_t color) {
    g.fillCircle(x + 6, y + 12, 5, color);
    g.fillTriangle(x + 6, y + 2, x + 1, y + 11, x + 11, y + 11, color);
    g.fillCircle(x + 4, y + 11, 1, C_WHITE);
}

void drawPinIconOn(TFT_eSPI &g, int x, int y, uint16_t color) {
    g.fillCircle(x + 5, y + 5, 4, color);
    g.fillTriangle(x + 1, y + 7, x + 9, y + 7, x + 5, y + 13, color);
    g.fillCircle(x + 5, y + 5, 1, C_PANEL);
}

void drawWifiBadgeOn(TFT_eSPI &g, int x, int y, bool connected) {
    if (connected) {
        g.fillRoundRect(x, y, 42, 16, 4, 0x0320);
        g.drawRoundRect(x, y, 42, 16, 4, C_GREEN);
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_GREEN, 0x0320);
        g.drawString("WIFI", x + 21, y + 8, 1);
    } else {
        g.fillRoundRect(x, y, 42, 16, 4, 0x3000);
        g.drawRoundRect(x, y, 42, 16, 4, C_RED);
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_ORANGE, 0x3000);
        g.drawString("OFF", x + 21, y + 8, 1);
    }
}

void drawWeatherIconOn(TFT_eSPI &g, int cx, int cy, int code, bool large) {
    int s = large ? 18 : 10;

    if (code == 0 || code == 1) {
        for (int a = 0; a < 360; a += 45) {
            float rad = a * 0.0174533f;
            int x1 = cx + (int)(cosf(rad) * (s + 2));
            int y1 = cy + (int)(sinf(rad) * (s + 2));
            int x2 = cx + (int)(cosf(rad) * (s + (large ? 7 : 4)));
            int y2 = cy + (int)(sinf(rad) * (s + (large ? 7 : 4)));
            g.drawLine(x1, y1, x2, y2, C_ORANGE);
            if (large) g.drawLine(x1 + 1, y1, x2 + 1, y2, C_AMBER);
        }
        g.fillCircle(cx, cy, s - 1, C_ORANGE);
        g.fillCircle(cx, cy, s - 3, C_AMBER);
        return;
    }

    if (code == 2) {
        int sunX = cx - (large ? 8 : 5);
        int sunY = cy - (large ? 6 : 4);
        g.fillCircle(sunX, sunY, large ? 10 : 6, C_ORANGE);
        g.fillCircle(sunX, sunY, large ? 8 : 4, C_AMBER);
    }

    uint16_t cloudCol = (code >= 60) ? C_SKY : C_WHITE;
    int cw = large ? 32 : 18;
    int ch = large ? 12 : 7;
    g.fillRoundRect(cx - cw / 2, cy, cw, ch, ch / 2, cloudCol);
    g.fillCircle(cx - cw / 4, cy, large ? 9 : 5, cloudCol);
    g.fillCircle(cx + cw / 6, cy - (large ? 3 : 2), large ? 11 : 6, cloudCol);

    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        int dy = cy + ch + (large ? 3 : 2);
        for (int i = -1; i <= 1; i++) {
            int dx = cx + i * (large ? 9 : 5);
            g.drawLine(dx, dy, dx - 2, dy + (large ? 6 : 4), C_CYAN);
            if (large) g.drawLine(dx + 1, dy, dx - 1, dy + 6, C_CYAN);
        }
    } else if (code >= 71 && code <= 77) {
        int dy = cy + ch + (large ? 5 : 3);
        for (int i = -1; i <= 1; i++) {
            g.fillCircle(cx + i * (large ? 9 : 5), dy, large ? 2 : 1, C_WHITE);
        }
    } else if (code >= 95) {
        int ly = cy + ch;
        g.fillTriangle(cx + 2, ly, cx - 5, ly + (large ? 7 : 4), cx + 1, ly + (large ? 7 : 4), C_AMBER);
        g.fillTriangle(cx - 1, ly + (large ? 5 : 3), cx + 4, ly + (large ? 5 : 3), cx - 3, ly + (large ? 12 : 8), C_ORANGE);
    }
}

// Bottom page indicator bar (228x12 at screen coordinates (6, 226))
void renderFooterIndicator(int activePage) {
    TFT_eSPI &g = panelReady ? (TFT_eSPI &)panel : tft;
    int ox = panelReady ? 0 : 6;
    int oy = panelReady ? 0 : 226;

    g.fillRect(ox, oy, 228, 12, C_BG);
    int y = oy + 7;
    int totalW = (SCREEN_COUNT - 1) * 16 + 12;
    int x = ox + 114 - totalW / 2;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (i == activePage) {
            g.fillRoundRect(x, y - 3, 14, 6, 3, C_ORANGE);
            x += 20;
        } else {
            g.fillCircle(x + 3, y, 2, C_DARKGRAY);
            x += 12;
        }
    }
    if (panelReady) {
        panel.pushSprite(6, 226, 0, 0, 228, 12);
    }
}

// ==================== Dashboard Middle Block (Clock + Date/IP Bar, 228x86 at (6, 42)) ====================
void renderHeroClockBlock() {
    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo, 5);

    char hhStr[4] = "--";
    char mmStr[4] = "--";
    char ssStr[4] = "--";
    char dayStr[16] = "SYNC";
    char dateStr[20] = "Syncing NTP...";

    static const char* DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char* MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

    if (hasTime) {
        snprintf(hhStr, sizeof(hhStr), "%02d", timeinfo.tm_hour);
        snprintf(mmStr, sizeof(mmStr), "%02d", timeinfo.tm_min);
        snprintf(ssStr, sizeof(ssStr), "%02d", timeinfo.tm_sec);
        snprintf(dayStr, sizeof(dayStr), "%s", DAYS[timeinfo.tm_wday % 7]);
        snprintf(dateStr, sizeof(dateStr), "%02d %s %04d",
                 timeinfo.tm_mday, MONTHS[timeinfo.tm_mon % 12], timeinfo.tm_year + 1900);
    }

    bool blinkOn = ((millis() / 500) % 2) == 0;

    TFT_eSPI &g = panelReady ? (TFT_eSPI &)panel : tft;
    int ox = panelReady ? 0 : 6;
    int oy = panelReady ? 0 : 42;

    // Compose the entire 228x86 region in sprite RAM before pushing to eliminate flicker
    g.fillRect(ox, oy, 228, 86, C_BG);

    // Hours (white)
    g.setTextDatum(MR_DATUM);
    g.setTextColor(C_WHITE, C_BG);
    g.drawString(hhStr, ox + 88, oy + 31, 7);

    // Blinking colon
    uint16_t colonCol = blinkOn ? C_CYAN : C_DARKGRAY;
    g.fillCircle(ox + 96, oy + 19, 3, colonCol);
    g.fillCircle(ox + 96, oy + 41, 3, colonCol);

    // Minutes (accent orange)
    g.setTextDatum(ML_DATUM);
    g.setTextColor(C_ORANGE, C_BG);
    g.drawString(mmStr, ox + 104, oy + 31, 7);

    // Weekday badge and seconds on the right
    g.fillRoundRect(ox + 176, oy + 4, 48, 20, 5, C_PANEL_2);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(C_CYAN, C_PANEL_2);
    g.drawString(dayStr, ox + 200, oy + 14, 2);

    g.setTextDatum(MC_DATUM);
    g.setTextColor(C_AMBER, C_BG);
    g.drawString(ssStr, ox + 200, oy + 44, 4);

    // Date and local IP bar at the bottom of the clock block (y = 64..86)
    g.fillRoundRect(ox, oy + 64, 228, 22, 6, C_PANEL);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(C_WHITE, C_PANEL);
    g.drawString(dateStr, ox + 10, oy + 75, 2);

    g.setTextDatum(MR_DATUM);
    if (WiFi.status() == WL_CONNECTED) {
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString(WiFi.localIP().toString(), ox + 220, oy + 75, 1);
    } else {
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("No WiFi", ox + 220, oy + 75, 1);
    }

    if (panelReady) {
        panel.pushSprite(6, 42, 0, 0, 228, 86);
    }
}

// ==================== Dashboard Bottom Cards (Room + Outdoor, 228x90 at (6, 134)) ====================
void renderDashboardBottomCards() {
    TFT_eSPI &g = panelReady ? (TFT_eSPI &)panel : tft;
    int ox = panelReady ? 0 : 6;
    int oy = panelReady ? 0 : 134;

    g.fillRect(ox, oy, 228, 90, C_BG);

    // Left card: ROOM (SHT30)
    g.fillRoundRect(ox, oy + 2, 111, 86, 10, C_PANEL);
    g.drawRoundRect(ox, oy + 2, 111, 86, 10, C_BORDER);
    g.fillRoundRect(ox + 8, oy + 8, 50, 14, 4, 0x0320);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(C_GREEN, 0x0320);
    g.drawString("ROOM", ox + 33, oy + 15, 1);

    if (room.connected) {
        char compBuf[12];
        snprintf(compBuf, sizeof(compBuf), "%+.1fC", room.currentOffset);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString(compBuf, ox + 104, oy + 15, 1);

        drawThermometerIconOn(g, ox + 8, oy + 28, C_ORANGE);
        char rtBuf[16];
        snprintf(rtBuf, sizeof(rtBuf), "%.1fC", room.temp);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_WHITE, C_PANEL);
        g.drawString(rtBuf, ox + 24, oy + 38, 4);

        drawDropletIconOn(g, ox + 8, oy + 58, C_CYAN);
        char rhBuf[16];
        snprintf(rhBuf, sizeof(rhBuf), "%.0f%%", room.humidity);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_CYAN, C_PANEL);
        g.drawString(rhBuf, ox + 24, oy + 68, 2);

        drawProgressBarOn(g, ox + 58, oy + 64, 44, 7, (int)room.humidity, C_CYAN);
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("No SHT30", ox + 55, oy + 42, 2);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("GPIO 21/22/25", ox + 55, oy + 62, 1);
    }

    // Right card: OUTDOOR (Weather)
    g.fillRoundRect(ox + 117, oy + 2, 111, 86, 10, C_PANEL);
    g.drawRoundRect(ox + 117, oy + 2, 111, 86, 10, C_BORDER);
    g.fillRoundRect(ox + 125, oy + 8, 62, 14, 4, 0x0210);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(C_CYAN, 0x0210);
    g.drawString("OUTDOOR", ox + 156, oy + 15, 1);

    if (weather.valid) {
        char otBuf[16];
        snprintf(otBuf, sizeof(otBuf), "%+.1fC", weather.temp);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_AMBER, C_PANEL);
        g.drawString(otBuf, ox + 126, oy + 38, 4);

        char feelBuf[22];
        snprintf(feelBuf, sizeof(feelBuf), "Feel %+.0fC  %d%%", weather.apparentTemp, weather.humidity);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString(feelBuf, ox + 126, oy + 60, 1);

        char windBuf[22];
        snprintf(windBuf, sizeof(windBuf), "Wind %.1f km/h", weather.windSpeed);
        g.setTextColor(C_MINT, C_PANEL);
        g.drawString(windBuf, ox + 126, oy + 74, 1);
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("Syncing...", ox + 172, oy + 50, 2);
    }

    if (panelReady) {
        panel.pushSprite(6, 134, 0, 0, 228, 90);
    }
}

// ==================== SCREEN 1: Main MeteoCube Dashboard ====================
void drawDashboardScreen() {
    TFT_eSPI &g = panelReady ? (TFT_eSPI &)panel : tft;
    int ox = panelReady ? 0 : 6;
    int oy = panelReady ? 0 : 6;

    // 1. Top header bar (228x30 at (6, 6))
    g.fillRect(ox, oy, 228, 30, C_BG);
    g.fillRoundRect(ox, oy, 228, 30, 8, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 30, 8, C_BORDER);

    drawPinIconOn(g, ox + 8, oy + 8, C_ORANGE);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(C_WHITE, C_PANEL);
    g.drawString(CITY_NAME, ox + 22, oy + 15, 2);

    if (weather.valid) {
        drawWeatherIconOn(g, ox + 112, oy + 14, weather.weatherCode, false);
        char outTempBuf[16];
        snprintf(outTempBuf, sizeof(outTempBuf), "%+.1fC", weather.temp);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_CYAN, C_PANEL);
        g.drawString(outTempBuf, ox + 128, oy + 15, 2);
    } else {
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("--.-C", ox + 118, oy + 15, 2);
    }

    drawWifiBadgeOn(g, ox + 180, oy + 7, WiFi.status() == WL_CONNECTED);
    if (panelReady) {
        panel.pushSprite(6, 6, 0, 0, 228, 30);
    }

    // 2. Middle block (Clock + Date/IP, 228x86 at (6, 42))
    renderHeroClockBlock();

    // 3. Bottom block (Room & Outdoor cards, 228x90 at (6, 134))
    renderDashboardBottomCards();

    // 4. Page indicator dots
    renderFooterIndicator(SCREEN_DASHBOARD);
}

// ==================== SCREEN 2: Detailed Weather ====================
void drawWeatherScreen() {
    TFT_eSPI &g = panelReady ? (TFT_eSPI &)panel : tft;
    int ox = panelReady ? 0 : 6;

    // 1. Top header bar (228x30 at (6, 6))
    int oy = panelReady ? 0 : 6;
    g.fillRect(ox, oy, 228, 30, C_BG);
    g.fillRoundRect(ox, oy, 228, 30, 8, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 30, 8, C_BORDER);
    drawPinIconOn(g, ox + 8, oy + 8, C_CYAN);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(C_WHITE, C_PANEL);
    g.drawString(CITY_NAME, ox + 22, oy + 15, 2);

    g.setTextDatum(MR_DATUM);
    g.setTextColor(C_AMBER, C_PANEL);
    g.drawString(weather.valid ? weatherCodeToText(weather.weatherCode) : "WEATHER", ox + 220, oy + 15, 2);
    if (panelReady) {
        panel.pushSprite(6, 6, 0, 0, 228, 30);
    }

    // 2. Middle block (228x86 at (6, 42)): Large weather icon + current temperature
    oy = panelReady ? 0 : 42;
    g.fillRect(ox, oy, 228, 86, C_BG);
    g.fillRoundRect(ox, oy, 228, 86, 10, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 86, 10, C_BORDER);

    if (weather.valid) {
        drawWeatherIconOn(g, ox + 42, oy + 38, weather.weatherCode, true);

        char tempStr[16];
        snprintf(tempStr, sizeof(tempStr), "%+.1f", weather.temp);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_WHITE, C_PANEL);
        g.drawString(tempStr, ox + 198, oy + 34, 6);
        g.setTextDatum(TL_DATUM);
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("C", ox + 202, oy + 12, 4);

        char subBuf[40];
        snprintf(subBuf, sizeof(subBuf), "Feels %+.1fC   Min %+.0f / Max %+.0fC",
                 weather.apparentTemp, weather.tempMin, weather.tempMax);
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_SKY, C_PANEL);
        g.drawString(subBuf, ox + 114, oy + 72, 2);
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("Waiting for weather...", ox + 114, oy + 43, 2);
    }
    if (panelReady) {
        panel.pushSprite(6, 42, 0, 0, 228, 86);
    }

    // 3. Bottom block (228x90 at (6, 134)): Humidity, wind & pressure progress bars
    oy = panelReady ? 0 : 134;
    g.fillRect(ox, oy, 228, 90, C_BG);
    g.fillRoundRect(ox, oy, 228, 90, 10, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 90, 10, C_BORDER);

    if (weather.valid) {
        // Humidity
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("HUMIDITY", ox + 10, oy + 16, 2);
        drawProgressBarOn(g, ox + 92, oy + 12, 80, 8, weather.humidity, C_CYAN);
        char hStr[12];
        snprintf(hStr, sizeof(hStr), "%d%%", weather.humidity);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_CYAN, C_PANEL);
        g.drawString(hStr, ox + 218, oy + 16, 2);

        // Wind speed
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("WIND", ox + 10, oy + 44, 2);
        int windPct = (int)(weather.windSpeed * 2.5f);
        drawProgressBarOn(g, ox + 92, oy + 40, 80, 8, windPct, C_GREEN);
        char wStr[16];
        snprintf(wStr, sizeof(wStr), "%.1f km/h", weather.windSpeed);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_GREEN, C_PANEL);
        g.drawString(wStr, ox + 218, oy + 44, 2);

        // Surface pressure (mmHg)
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("PRESSURE", ox + 10, oy + 72, 2);
        int pressPct = (weather.pressureMmHg - 730) * 100 / 50;
        drawProgressBarOn(g, ox + 92, oy + 68, 68, 8, pressPct, C_ORANGE);
        char pStr[16];
        snprintf(pStr, sizeof(pStr), "%d mm", weather.pressureMmHg);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString(pStr, ox + 218, oy + 72, 2);
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("Check WiFi connection", ox + 114, oy + 45, 2);
    }
    if (panelReady) {
        panel.pushSprite(6, 134, 0, 0, 228, 90);
    }

    renderFooterIndicator(SCREEN_WEATHER);
}

// ==================== SCREEN 3: Indoor Climate (SHT30) ====================
void drawRoomScreen() {
    TFT_eSPI &g = panelReady ? (TFT_eSPI &)panel : tft;
    int ox = panelReady ? 0 : 6;

    // 1. Top header bar (228x30 at (6, 6))
    int oy = panelReady ? 0 : 6;
    g.fillRect(ox, oy, 228, 30, C_BG);
    g.fillRoundRect(ox, oy, 228, 30, 8, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 30, 8, C_BORDER);

    g.setTextDatum(ML_DATUM);
    g.setTextColor(C_GREEN, C_PANEL);
    g.drawString("INDOOR", ox + 10, oy + 15, 2);

    g.setTextDatum(MR_DATUM);
    if (room.connected) {
        char hdrBuf[28];
        snprintf(hdrBuf, sizeof(hdrBuf), "COMP %+.1fC | %s",
                 room.currentOffset, room.heaterOn ? "H:ON!" : "H:OFF");
        g.setTextColor(room.heaterOn ? C_ORANGE : C_MINT, C_PANEL);
        g.drawString(hdrBuf, ox + 220, oy + 15, 2);
    } else {
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("NO SENSOR", ox + 220, oy + 15, 2);
    }
    if (panelReady) {
        panel.pushSprite(6, 6, 0, 0, 228, 30);
    }

    // 2. Middle block (228x86 at (6, 42)): Room temperature card
    oy = panelReady ? 0 : 42;
    g.fillRect(ox, oy, 228, 86, C_BG);
    g.fillRoundRect(ox, oy, 228, 86, 10, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 86, 10, C_BORDER);

    if (room.connected) {
        drawThermometerIconOn(g, ox + 12, oy + 14, C_ORANGE);
        g.setTextDatum(TL_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("ROOM TEMP", ox + 32, oy + 8, 2);

        const char* tStatus = (room.temp < 19.0f) ? "COOL" : (room.temp <= 25.5f ? "COMFORT" : "WARM");
        uint16_t tCol = (room.temp < 19.0f) ? C_CYAN : (room.temp <= 25.5f ? C_GREEN : C_ORANGE);
        g.fillRoundRect(ox + 156, oy + 7, 64, 16, 4, C_PANEL_2);
        g.setTextDatum(MC_DATUM);
        g.setTextColor(tCol, C_PANEL_2);
        g.drawString(tStatus, ox + 188, oy + 15, 1);

        char tBuf[16];
        snprintf(tBuf, sizeof(tBuf), "%.1f", room.temp);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_WHITE, C_PANEL);
        g.drawString(tBuf, ox + 32, oy + 50, 6);
        g.setTextDatum(TL_DATUM);
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("C", ox + 136, oy + 30, 4);

        char rawStr[20];
        snprintf(rawStr, sizeof(rawStr), "raw %.1fC", room.rawTemp);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString(rawStr, ox + 218, oy + 56, 1);

        int tPct = (int)((room.temp - 15.0f) * 5.0f);
        drawProgressBarOn(g, ox + 12, oy + 72, 204, 7, tPct, C_ORANGE);
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_ORANGE, C_PANEL);
        g.drawString("SHT30 Not Detected", ox + 114, oy + 43, 4);
    }
    if (panelReady) {
        panel.pushSprite(6, 42, 0, 0, 228, 86);
    }

    // 3. Bottom block (228x90 at (6, 134)): Relative humidity card
    oy = panelReady ? 0 : 134;
    g.fillRect(ox, oy, 228, 90, C_BG);
    g.fillRoundRect(ox, oy, 228, 90, 10, C_PANEL);
    g.drawRoundRect(ox, oy, 228, 90, 10, C_BORDER);

    if (room.connected) {
        drawDropletIconOn(g, ox + 12, oy + 14, C_CYAN);
        g.setTextDatum(TL_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("HUMIDITY", ox + 32, oy + 8, 2);

        const char* hStatus = (room.humidity < 35.0f) ? "DRY" : (room.humidity <= 65.0f ? "OPTIMAL" : "HUMID");
        uint16_t hCol = (room.humidity < 35.0f) ? C_ORANGE : (room.humidity <= 65.0f ? C_CYAN : C_PINK);
        g.fillRoundRect(ox + 156, oy + 7, 64, 16, 4, C_PANEL_2);
        g.setTextDatum(MC_DATUM);
        g.setTextColor(hCol, C_PANEL_2);
        g.drawString(hStatus, ox + 188, oy + 15, 1);

        char hBuf[16];
        snprintf(hBuf, sizeof(hBuf), "%.1f", room.humidity);
        g.setTextDatum(ML_DATUM);
        g.setTextColor(C_WHITE, C_PANEL);
        g.drawString(hBuf, ox + 32, oy + 50, 6);
        g.setTextDatum(TL_DATUM);
        g.setTextColor(C_CYAN, C_PANEL);
        g.drawString("%", ox + 136, oy + 30, 4);

        char rawHStr[20];
        snprintf(rawHStr, sizeof(rawHStr), "raw %.0f%%", room.rawHumidity);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString(rawHStr, ox + 218, oy + 56, 1);

        drawProgressBarOn(g, ox + 12, oy + 74, 204, 7, (int)room.humidity, C_CYAN);
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(C_GRAY, C_PANEL);
        g.drawString("VCC->25  SDA->21  SCL->22", ox + 114, oy + 45, 2);
    }
    if (panelReady) {
        panel.pushSprite(6, 134, 0, 0, 228, 90);
    }

    renderFooterIndicator(SCREEN_ROOM);
}

// ==================== Screen Dispatcher (No fillScreen Calls) ====================
void renderCurrentScreen() {
    if (otaInProgress) return;
    switch (currentScreen) {
        case SCREEN_DASHBOARD:
            drawDashboardScreen();
            break;
        case SCREEN_WEATHER:
            drawWeatherScreen();
            break;
        case SCREEN_ROOM:
            drawRoomScreen();
            break;
        default:
            break;
    }
}

// ==================== OTA Setup (Wi-Fi Firmware Updates Without BOOT Button) ====================
void setupOTA() {
    if (otaInitialized || WiFi.status() != WL_CONNECTED) return;

    ArduinoOTA.setHostname(OTA_HOSTNAME);

    ArduinoOTA.onStart([]() {
        otaInProgress = true;
        tft.fillScreen(C_BG);
        tft.fillRoundRect(12, 40, 216, 160, 12, C_PANEL);
        tft.drawRoundRect(12, 40, 216, 160, 12, C_CYAN);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(C_CYAN, C_PANEL);
        tft.drawString("OTA UPDATE", 120, 75, 4);
        tft.setTextColor(C_GRAY, C_PANEL);
        tft.drawString("Flashing via Wi-Fi...", 120, 105, 2);
        drawProgressBarOn(tft, 28, 135, 184, 14, 0, C_ORANGE);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        int pct = (total > 0) ? (progress * 100) / total : 0;
        drawProgressBarOn(tft, 28, 135, 184, 14, pct, C_ORANGE);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d %%", pct);
        tft.setTextDatum(MC_DATUM);
        tft.setTextPadding(70);
        tft.setTextColor(C_WHITE, C_PANEL);
        tft.drawString(buf, 120, 172, 4);
        tft.setTextPadding(0);
    });

    ArduinoOTA.onEnd([]() {
        tft.fillRoundRect(12, 40, 216, 160, 12, C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(C_GREEN, C_PANEL);
        tft.drawString("COMPLETE!", 120, 105, 4);
        tft.setTextColor(C_WHITE, C_PANEL);
        tft.drawString("Rebooting...", 120, 140, 2);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        otaInProgress = false;
        Serial.printf("OTA Error[%u]\n", error);
        tft.fillScreen(C_BG);
        renderCurrentScreen();
    });

    ArduinoOTA.begin();
    otaInitialized = true;
    Serial.printf("OTA ready! Hostname: %s.local | IP: %s\n",
                  OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== ESP32 MeteoCube + Thermal Comp + Zero-Flicker ===");

    // Initialize warmup timer in RTC memory: reset to 0s only on cold power-on,
    // keeping accumulated thermal warmup time across OTA updates and soft resets.
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || rtcMagic != RTC_WARMUP_MAGIC || rtcWarmupSeconds > 864000UL) {
        rtcMagic = RTC_WARMUP_MAGIC;
        rtcWarmupSeconds = 0;
        Serial.println("Cold boot detected -> warmup timer reset to 0s");
    } else {
        Serial.printf("Warm reboot detected -> keeping warmup timer at %lus\n", (unsigned long)rtcWarmupSeconds);
    }

    // 1. Power SHT30 VIN via GPIO25 before initializing the I2C bus
    pinMode(SHT30_VCC_PIN, OUTPUT);
    digitalWrite(SHT30_VCC_PIN, HIGH);
    delay(25);

    // 2. Initialize I2C for SHT30 (GPIO21 = SDA, GPIO22 = SCL)
    Wire.begin(SHT30_SDA_PIN, SHT30_SCL_PIN);

    // 3. Force SCK=HIGH (SPI_MODE3) and pulse RES for CS-less ST7789 sync
    pinMode(TFT_SCLK, OUTPUT);
    digitalWrite(TFT_SCLK, HIGH);
    pinMode(TFT_MOSI, OUTPUT);
    digitalWrite(TFT_MOSI, HIGH);
    pinMode(TFT_DC, OUTPUT);
    digitalWrite(TFT_DC, HIGH);
    #if (TFT_RST >= 0)
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, LOW);
    delay(50);
    digitalWrite(TFT_RST, HIGH);
    delay(150);
    #endif

    // 4. Initialize 240x240 ST7789 IPS display
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);

    // Allocate reusable 228x90 16-bit sprite buffer (~41 KB RAM) for flicker-free zone updates
    panel.setColorDepth(16);
    panelReady = (panel.createSprite(228, 90) != nullptr);
    Serial.printf("Panel sprite 228x90: %s\n", panelReady ? "OK (Zero-Flicker enabled)" : "FAILED");

    // 5. Startup splash screen
    tft.fillScreen(C_BG);
    tft.fillRoundRect(16, 28, 208, 184, 14, C_PANEL);
    tft.drawRoundRect(16, 28, 208, 184, 14, C_BORDER);

    drawWeatherIconOn(tft, 120, 72, 2, true);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_WHITE, C_PANEL);
    tft.drawString("METEOCUBE", 120, 118, 4);
    tft.setTextColor(C_ORANGE, C_PANEL);
    tft.drawString("ESP32 STATION", 120, 142, 2);

    tft.setTextColor(C_GRAY, C_PANEL);
    tft.drawString("Connecting Wi-Fi...", 120, 170, 2);
    drawProgressBarOn(tft, 36, 188, 168, 8, 15, C_CYAN);

    // 6. Connect to Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(OTA_HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiStart = millis();
    int progress = 15;
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 7000) {
        delay(250);
        progress = min(90, progress + 3);
        drawProgressBarOn(tft, 36, 188, 168, 8, progress, C_CYAN);
        Serial.print(".");
    }
    Serial.println();

    if (probeSHT30Address(SHT30_ADDR_1)) disableSHT30Heater(SHT30_ADDR_1);
    if (probeSHT30Address(SHT30_ADDR_2)) disableSHT30Heater(SHT30_ADDR_2);
    updateSHT30();

    if (WiFi.status() == WL_CONNECTED) {
        drawProgressBarOn(tft, 36, 188, 168, 8, 100, C_GREEN);
        configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
        setupOTA();

        tft.fillRect(24, 160, 192, 24, C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(C_GREEN, C_PANEL);
        String ipMsg = String("OTA IP: ") + WiFi.localIP().toString();
        tft.drawString(ipMsg, 120, 170, 2);

        updateWeather();
        lastWeatherUpdateMs = millis();
        delay(1000);
    }

    // Clear the screen exactly ONCE before entering the main loop;
    // subsequent updates overwrite zones via the 228x90 panel sprite with zero black flashes.
    tft.fillScreen(C_BG);

    unsigned long now = millis();
    lastSensorUpdateMs = now;
    lastScreenSwitchMs = now;
    lastClockTickMs = now;
    lastUptimeTrackMs = now;
    renderCurrentScreen();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!otaInitialized) {
            setupOTA();
        }
        ArduinoOTA.handle();
    }

    if (otaInProgress) {
        return;
    }

    unsigned long now = millis();

    // 1. Track cumulative thermal warmup uptime (once per second)
    if (now - lastUptimeTrackMs >= 1000) {
        uint32_t elapsedSec = (now - lastUptimeTrackMs) / 1000;
        lastUptimeTrackMs += elapsedSec * 1000;
        if (rtcWarmupSeconds < 36000UL) {
            rtcWarmupSeconds += elapsedSec;
        }
    }

    // 2. Poll SHT30 sensor (every 10 seconds)
    if (now - lastSensorUpdateMs >= SENSOR_UPDATE_INTERVAL_MS) {
        lastSensorUpdateMs = now;
        updateSHT30();
        if (currentScreen == SCREEN_ROOM) {
            drawRoomScreen();
        } else if (currentScreen == SCREEN_DASHBOARD) {
            renderDashboardBottomCards();
        }
    }

    // 3. Refresh outdoor forecast (every 15 minutes)
    if (now - lastWeatherUpdateMs >= WEATHER_UPDATE_INTERVAL_MS) {
        lastWeatherUpdateMs = now;
        updateWeather();
        if (currentScreen == SCREEN_WEATHER) {
            drawWeatherScreen();
        } else if (currentScreen == SCREEN_DASHBOARD) {
            drawDashboardScreen();
        }
    }

    // 4. Auto-cycle active UI tab (flicker-free sprite block swap)
    if (now - lastScreenSwitchMs >= SCREEN_SWITCH_INTERVAL_MS) {
        lastScreenSwitchMs = now;
        currentScreen = static_cast<ScreenMode>((currentScreen + 1) % SCREEN_COUNT);
        renderCurrentScreen();
    }

    // 5. Update clock digits and blinking colon every 500 ms on the dashboard
    if (currentScreen == SCREEN_DASHBOARD && (now - lastClockTickMs >= 500)) {
        lastClockTickMs = now;
        renderHeroClockBlock();
    }
}
