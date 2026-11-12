# ESP32 Desk Cube — Clock, Weather & Indoor Climate Station

A compact ESP32 desktop station inspired by GeekMagic SmallTV, featuring a 1.3" 240×240 IPS `ST7789` display, an `SHT30` I2C temperature/humidity sensor, live Open-Meteo weather forecasts, zero-flicker partial sprite rendering, dynamic self-heating thermal compensation, and Wi-Fi Over-The-Air (`ArduinoOTA`) firmware updates.

<!-- TODO: Replace with a real photo or short GIF of the working cube -->
<!-- ![Desk Cube Demo](docs/demo.jpg) -->

---

## Features

- **3 Auto-Cycling UI Screens** (every 8 s, flicker-free):
  1. **Dashboard** — NTP-synchronized two-tone clock (`HH:MM:SS`), weekday/date/IP bar, plus live indoor (`SHT30`) and outdoor summary cards.
  2. **Weather Detail** — Custom vector weather icons, current & apparent ("feels like") temperature, daily min/max, and progress bars for humidity, wind speed, and atmospheric pressure (`mmHg`) via the [Open-Meteo API](https://open-meteo.com/) (no API key required).
  3. **Indoor Climate** — Room temperature and relative humidity with comfort badges (`COOL / COMFORT / WARM`, `DRY / OPTIMAL / HUMID`), raw vs. compensated readings, and `SHT30` internal heater status.
- **Zero-Flicker Rendering on RAM-Constrained ESP32**:
  - A full 240×240×16-bit frame buffer requires `115.2 KB` of contiguous DRAM, which fragments the heap and starves `WiFi.begin()`.
  - Instead, all four UI zones are rendered off-screen into a single reusable `228×90` 16-bit `TFT_eSprite` (`~41 KB`) and pushed directly over previous frames without calling `tft.fillScreen()`.
- **SHT30 Heater Guard & Thermal Drift Compensation**:
  - Reads status register `0xF32D` (bit 13) and sends disable command `0x3066` if the sensor's internal heater is active, with CRC-8 (`0x31`) verification on all frames.
  - Compensates PCB/ESP32 self-heating (`+1.0 °C` cold start drifting to `+4.0 °C` at steady state) using an exponential warmup curve (`τ = 10 min`) persisted across soft resets/OTA updates in `RTC_NOINIT_ATTR` memory.
  - Recalculates relative humidity using the **Magnus saturation vapor pressure formula** so RH remains physically consistent after temperature correction.
- **OTA Firmware Updates (`ArduinoOTA`)**:
  - Uses `min_spiffs.csv` (`1.9 MB` per OTA app slot) to fit Wi-Fi + HTTPClient + ArduinoOTA + `TFT_eSPI`, displaying an on-screen progress bar during Wi-Fi flashing.

---

## Hardware & Wiring

| Component | Notes |
|---|---|
| **MCU** | ESP32-WROOM-32 (DevKit V1, 4 MB Flash) |
| **Display** | 1.3" 240×240 IPS `ST7789` (7-pin module **without CS pin**) |
| **Sensor** | `SHT30` I2C digital temperature & humidity sensor (`0x44` / `0x45`) |

### Pinout

#### ST7789 Display (SPI Mode 3, CS-less)

| Display Pin | ESP32 GPIO | Notes |
|---|---|---|
| `VCC` | `3.3V` | Regulated 3.3 V |
| `GND` | `GND` | Common ground |
| `SCK` (`SCL`) | `GPIO18` | Pulled `HIGH` before `RES` pulse for `SPI_MODE3` sync |
| `SDA` (`MOSI`) | `GPIO23` | Hardware VSPI MOSI (`10 MHz` SPI clock) |
| `RES` | `GPIO4` | Hardware reset |
| `DC` | `GPIO16` | Data / Command |
| `BLK` | *Not connected* | Backlight always on |

> **Why `SPI_MODE3` & `10 MHz`?** CS-less ST7789 breakouts rely on clock idle polarity to stay synchronized after reset. `setup()` drives `SCK` high before pulsing `RES`, and `platformio.ini` configures `-DTFT_SPI_MODE=SPI_MODE3` and `-DTFT_CS=-1` strictly via `build_flags` (no `User_Setup.h` edits required).

#### SHT30 Sensor (I2C, GPIO-powered)

| SHT30 Pin | ESP32 GPIO | Notes |
|---|---|---|
| `VIN` | `GPIO25` | Powered directly from `GPIO25` (< 1.5 mA load) |
| `GND` | `GND` | Common ground |
| `SDA` | `GPIO21` | I2C data |
| `SCL` | `GPIO22` | I2C clock |

---

## Thermal Compensation Model

When mounted near an active ESP32 with Wi-Fi enabled, the ambient temperature inside the enclosure rises gradually until thermal equilibrium. The firmware applies a time-dependent correction:

$$\Delta T(t) = \Delta T_{\text{start}} + (\Delta T_{\text{warm}} - \Delta T_{\text{start}})\left(1 - e^{-t / \tau}\right)$$

- Warmup uptime `t` is stored in `RTC_NOINIT_ATTR` (`rtcWarmupSeconds` validated by a magic header), resetting only on true power-on (`ESP_RST_POWERON`) and surviving OTA reboots.
- Relative humidity is corrected from the raw sensor reading $T_{\text{raw}}, RH_{\text{raw}}$ to the compensated temperature $T_{\text{real}}$ via saturation vapor pressure:

$$RH_{\text{real}} = RH_{\text{raw}} \cdot \frac{e_s(T_{\text{raw}})}{e_s(T_{\text{real}})}, \quad e_s(T) = 6.112 \cdot \exp\left(\frac{17.62 \cdot T}{243.12 + T}\right)$$

All coefficients (`SHT30_OFFSET_START`, `SHT30_OFFSET_WARM`, `SHT30_WARMUP_TAU_MIN`) are tunable in [include/config.h](include/config.h).

---

## Project Structure

```text
cube/
├── include/
│   ├── config.h           # City/NTP timezone, pins, thermal curve & UI timings
│   ├── secrets.example.h  # Template for Wi-Fi credentials
│   └── secrets.h          # Local Wi-Fi credentials (gitignored)
├── src/
│   └── main.cpp           # Display driver init, SHT30 I2C, Open-Meteo, OTA & UI
├── platformio.ini         # ESP32 build flags (TFT_eSPI, partitions, OTA settings)
├── LICENSE                # MIT License
└── README.md
```

---

## Getting Started

### 1. Configure Wi-Fi & Location

1. Copy `include/secrets.example.h` to `include/secrets.h`:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```
2. Set `WIFI_SSID` and `WIFI_PASSWORD` inside `include/secrets.h`.
3. *(Optional)* Update `CITY_NAME`, `LATITUDE`, `LONGITUDE`, and `TZ_INFO` in [include/config.h](include/config.h).

### 2. First Flash (via USB)

In [platformio.ini](platformio.ini), temporarily comment out `upload_protocol = espota` and `upload_port = ...` for the initial USB upload (needed once to flash the `min_spiffs.csv` partition table and `ArduinoOTA` receiver):

```bash
pio run --target upload
```

### 3. Subsequent Flashes (via Wi-Fi OTA)

Once booted, the cube prints its IP address on the startup splash screen and on the Dashboard screen. Set that IP in [platformio.ini](platformio.ini):

```ini
upload_protocol = espota
upload_port = 192.168.0.100
```

Run `pio run --target upload` (or click **Upload** in VS Code PlatformIO) to update firmware wirelessly without touching the `BOOT` button.

---

## Tech Stack

- **Platform / Framework**: ESP32 / Arduino (`PlatformIO`)
- **Language**: C / C++
- **Libraries**: `bodmer/TFT_eSPI@^2.5.43`, `bblanchon/ArduinoJson@^7.0.0`, `ArduinoOTA`, `HTTPClient`, `Wire`
- **External Services**: NTP (`pool.ntp.org`), Open-Meteo Forecast API

---

## License

This project is licensed under the [MIT License](LICENSE).
