# ESP32 LG Modbus Gateway Telnet Bridge (ESP-IDF v5.x via PlatformIO)

Production-oriented ESP32 firmware that:
- Talks Modbus RTU over RS-485 to LG PMBUSB00A.
- Polls and caches HVAC points for multiple indoor units (zones).
- Exposes a telnet JSON API for read/write operations.
- Exposes a d3net-style web interface and JSON API for monitoring/configuration.
- Persists runtime configuration in NVS.
- Supports Wi-Fi STA and fallback SoftAP provisioning web page.

## Features

- Modbus RTU function codes implemented:
  - `0x01` Read Coils
  - `0x02` Read Discrete Inputs
  - `0x03` Read Holding Registers
  - `0x04` Read Input Registers
  - `0x05` Write Single Coil
  - `0x06` Write Single Register
- Retry + backoff, timeout handling, CRC checking, exception handling.
- Single in-flight Modbus transaction with mutex.
- HVAC mapping helpers:
  - `coil/discrete = N * 16 + offset`
  - `holding/input = N * 20 + offset`
- Polling round-robin with configurable interval.
- Write-verify flow for `power`, `mode`, `fan_speed`, `setpoint`.
- Safety controls:
  - setpoint clamp (`16.0..30.0C` default)
  - mode change rate-limit (`10s` default)
- Configuration in NVS:
  - Wi-Fi
  - Modbus UART/pins/baud/slave/timeout/retries
  - HVAC zones + `idu_address_base`
  - log level/device id

## Project Layout

- `platformio.ini`
- `CMakeLists.txt`
- `partitions.csv`
- `sdkconfig.defaults`
- `main/`
- `components/modbus/`
- `data/` (SPIFFS web files: `index.html`, `config.html`, `monitor.html`)

## Wiring

### ESP32 <-> MAX3485

- ESP32 `UART2 TX` -> MAX3485 `DI`
- ESP32 `UART2 RX` -> MAX3485 `RO`
- ESP32 GPIO (default `GPIO4`) -> MAX3485 `DE` and `RE` tied together
- ESP32 `3V3` -> MAX3485 `VCC`
- ESP32 `GND` -> MAX3485 `GND`

### MAX3485 <-> LG PMBUSB00A (CH1)

- MAX3485 `A` -> Gateway `A+`
- MAX3485 `B` -> Gateway `B-`

### Grounding Note

Use a common reference and proper RS-485 wiring/termination practices. Keep bus topology clean (avoid long star branches), verify A/B polarity, and add termination only where appropriate for the bus length/topology.

## Build / Flash (PlatformIO)

From `examples/lg_hvac_modbus_telnet`:

```bash
pio run
pio run -t upload --upload-port <PORT>
pio run -t buildfs
pio run -t uploadfs --upload-port <PORT>
pio device monitor -b 115200 -p <PORT>
```

Recommended first flash sequence:

1. `pio run -t upload --upload-port <PORT>`
2. `pio run -t uploadfs --upload-port <PORT>`

In VS Code PlatformIO extension:
- Open `examples/lg_hvac_modbus_telnet` as the project folder.
- Use Build / Upload / Monitor from the PlatformIO sidebar.

## Optional ESP-IDF CLI Build

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

To also flash web assets with ESP-IDF CLI:

```bash
idf.py -p <PORT> spiffs-flash
```

## Default Runtime Parameters

- Modbus: `9600 8N1`, slave `1`, timeout `300ms`, retries `3`
- UART2 pins: `TX=GPIO17`, `RX=GPIO16`, `DE=GPIO4`
- Poll interval: `3000ms` per round-robin step
- Zones: `1:0,1:1,1:2` (format `slave:central_address`)
- `idu_address_base=0`

## Wi-Fi Behavior

- If Wi-Fi credentials exist in NVS: starts STA and connects.
- If not configured: starts SoftAP and serves the full web UI.
  - SSID: `LGHVAC-SETUP-XXXXXX`
  - Connect and open `http://192.168.4.1`
  - Configure settings in `/config`; firmware saves to NVS and reboots.

## Web Interface (d3net-style mirror)

- `GET /` Home dashboard (network/system/zones)
- `GET /config` Wi-Fi + Modbus + HVAC configuration
- `GET /monitor` zone monitor and command panel

Pages are served from SPIFFS (`data/*.html`) similar to the d3net pattern.

### Web JSON API

- `GET /api/status`
- `GET /api/config`
- `POST /api/config/save`
- `GET /api/wifi/scan`
- `GET /api/zones`
- `POST /api/hvac/cmd`
- `POST /api/hvac/read`

Example HVAC command payload:

```json
{"index":0,"field":"setpoint","value":"23.5"}
```

## Telnet API (port 23)

Responses are JSON lines.

Commands:
- `HELP`
- `STATUS`
- `ZONES`
- `GET <zone_index>`
- `READ <zone_index>` (forces immediate poll)
- `SET <zone_index> <power|mode|fan|setpoint> <value>`
- `CONFIG GET`
- `CONFIG SET WIFI <ssid> <pass>`
- `CONFIG SET ZONES <csv>`
- `CONFIG SET POLL_MS <ms>`
- `CONFIG SET IDU_BASE <0|1>`
- `CONFIG SET SLAVE <1..247>`
- `CONFIG SAVE`
- `REBOOT`
- `QUIT`

Example:

```text
SET 0 power 1
SET 0 mode 4
SET 0 fan 3
SET 0 setpoint 23.5
GET 0
```

## Register Model and Addressing

Protocol addresses in Modbus frames are **zero-based**.

Helpers in `address_map.c`:
- `get_coil_address(zone, offset)`
- `get_discrete_address(zone, offset)`
- `get_holding_address(zone, offset)`
- `get_input_address(zone, offset)`
- `modbus_style_to_protocol()` for `40001` style to protocol address conversion.

Examples:

For `N=0`:
- power coil = `0*16+1 = 1`
- mode reg = `0*20+1 = 1`
- setpoint reg = `0*20+3 = 3`

For `N=2`:
- power coil = `2*16+1 = 33`
- mode reg = `2*20+1 = 41`
- setpoint reg = `2*20+3 = 43`

## HVAC Point Mapping

- `power`: coil offset `1`
- `mode`: holding offset `1` (`0 cool, 1 dry, 2 fan, 3 auto, 4 heat`)
- `fan_speed`: holding offset `2` (`1 low, 2 mid, 3 high, 4 auto`)
- `setpoint`: holding offset `3` (`x10`, default clamp `160..300`)
- `error_code`: input offset `1`
- `room_temperature`: input offset `2` (`x10`)
- `connected`: discrete offset `1`
- `alarm`: discrete offset `2`

## Example Configurations

### Multi V style (`idu_address_base=0`)

- `zones = 1:0,1:1,1:2`

### Single/Multi style (`idu_address_base=1`)

- `zones = 1:1,1:2,1:3`

## Notes

- OTA is not enabled yet. Add HTTPS OTA flow using `esp_https_ota` and `ota_url` from config.
- Optional serial CLI can be added later; telnet JSON API is fully functional now.
