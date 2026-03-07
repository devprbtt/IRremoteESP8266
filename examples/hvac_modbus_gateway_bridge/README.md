# ESP32 HVAC Modbus Gateway Bridge (ESP-IDF v5.x via PlatformIO)

Production-oriented ESP32 firmware that:
- Talks Modbus RTU over RS-485 to LG PMBUSB00A, Midea GW3-MOD, Daikin DTA116A51, and Hitachi HC-A(8/16/64)MB.
- Polls and caches HVAC points for multiple indoor units (zones).
- Exposes a telnet JSON API for read/write operations.
- Exposes a d3net-style web interface and JSON API for monitoring/configuration.
- Persists runtime configuration in NVS.
- Supports Wi-Fi STA and fallback SoftAP provisioning web page.
- Supports OTA for both firmware app partition and SPIFFS filesystem partition.

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
- HVAC zones + `idu_address_base` + `gateway_type`
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

From `examples/hvac_modbus_gateway_bridge`:

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
- Open `examples/hvac_modbus_gateway_bridge` as the project folder.
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

- Modbus default: `9600 8N1`, slave `1`, timeout `300ms`, retries `3`
- Per-gateway documented serial defaults:
  - LG PMBUSB00A: `9600 8N1`
  - Midea GW3-MOD: `9600 8N1`
  - Samsung MIM-B19N / MIM-B19NT: `9600 8E1`
  - Hitachi HC-A(8/16/64)MB: `9600 8N1`
- UART2 pins: `TX=GPIO17`, `RX=GPIO16`, `DE=GPIO4`
- Poll interval: `3000ms` per round-robin step
- Zones: `1:0,1:1,1:2` (format `slave:central_address`)
- `idu_address_base=0`
- `gateway_type=lg_pmbusb00a`

## Gateway Selection

Set the active mapping profile to match your gateway:

- `lg_pmbusb00a` (LG PMBUSB00A)
- `midea_gw3_mod` (Midea GW3-MOD)
- `daikin_dta116a51` (Daikin DTA116A51 / d3net Modbus map)
- `hitachi_hca_mb` (Hitachi HC-A(8/16/64)MB indoor-unit map)
- `samsung_mim_b19n` (Samsung MIM-B19N / MIM-B19NT indoor-unit map)

Ways to set:

- Web config: `/config` page, field `gateway_type`
- Telnet: `CONFIG SET GATEWAY <lg|midea|daikin|hitachi|samsung>` then `CONFIG SAVE` and reboot

Serial settings:
- `/config` now exposes `baud`, `parity`, and `stop_bits`
- Selecting a gateway applies the documented default serial format for that gateway
- These values remain user-overridable before saving

Samsung notes:
- Indoor unit PDU address formula in this profile: `50 + (UI * 50) + offset`
- Supported core fields:
  - `+0` communication status
  - `+2` power
  - `+3` mode
  - `+4` fan speed
  - `+8` set temperature (`C x10`)
  - `+9` room temperature (`C x10`)
  - `+14` integrated error code
- Generic mode mapping:
  - cool=`1`, dry=`2`, fan=`3`, auto=`0`, heat=`4`
- Generic fan mapping:
  - low=`1`, mid=`2`, high=`3`, auto=`0`

## Wi-Fi Behavior

- If Wi-Fi credentials exist in NVS: starts STA and connects.
- If not configured: starts SoftAP and serves the full web UI.
  - SSID: `HVACGW-SETUP-XXXXXX`
  - Connect and open `http://192.168.4.1`
  - Configure settings in `/config`; firmware saves to NVS and reboots.

## Web Interface (d3net-style mirror)

- `GET /` Home dashboard (network/system/zones)
- `GET /config` Wi-Fi + Modbus + HVAC configuration
- `GET /monitor` zone monitor and command panel
- `GET /firmware` OTA page (d3net-style progress bars and upload actions)

Pages are served from SPIFFS (`data/*.html`) similar to the d3net pattern.

### Web JSON API

- `GET /api/status`
- `GET /api/config`
- `POST /api/config/save`
- `GET /api/wifi/scan`
- `GET /api/zones`
- `GET /api/hvac/meta` (gateway-specific writable schema + online zones)
- `POST /api/hvac/cmd`
- `POST /api/hvac/read`
- `GET /api/ota/status`
- `POST /api/ota/start`
- `POST /firmware/update` (raw firmware `.bin` upload)
- `POST /spiffs/update` (raw `spiffs.bin` upload)

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

## OTA (Firmware + Filesystem)

OTA URLs are stored in config:
- `ota.firmware_url`
- `ota.filesystem_url`

You can start OTA from the web config page or API:

```json
POST /api/ota/start
{"type":"firmware","url":"https://host/fw.bin"}
```

```json
POST /api/ota/start
{"type":"filesystem","url":"https://host/spiffs.bin"}
```

Status:

```json
GET /api/ota/status
```

On successful OTA, device schedules automatic reboot.

For filesystem OTA, the image must be a SPIFFS binary sized for the `spiffs` partition in `partitions.csv`.

Direct browser upload OTA is also available from `/firmware`:
- Upload firmware `.bin` with client-side progress bar
- Upload `spiffs.bin` with client-side progress bar

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

## Hitachi HC-A(8/16/64)MB Mapping

This profile uses the manual formula: `addr = 2000 + (indoor_address * 32) + offset`.

- Read source: holding registers, 32-word block per indoor address
- `connected`: offset `0` (`exist`)
- `power`: status offset `9`, write order offset `3` (`0 stop`, `1 run`)
- `mode`: status offset `10`, write order offset `4`
  - Hitachi: `0 cool, 1 dry, 2 fan, 3 heat, 4 auto`
  - API mode: `0 cool, 1 dry, 2 fan, 3 auto, 4 heat`
- `fan_speed`: status offset `11`, write order offset `5`
  - Hitachi: `0 low, 1 medium, 2 high, 3 high2, 4 auto`
  - API fan: `1 low, 2 mid, 3 high, 4 auto`
- `setpoint`: status offset `12`, write order offset `6`
  - Hitachi units are integer Celsius; firmware rounds API tenths to nearest whole degree.
- `room_temperature`: offset `24` (ambient temperature, converted to x10)
- `error_code`: offset `19` (alarm code)
- `alarm`: true when operation condition offset `22` is alarm (`3`) or alarm code is non-zero

## Midea GW3-MOD Mapping Notes

Implemented profile based on GW3-MOD mapping tables:

- Discrete (FC02): `10001 + N*8` on/off, `10002 + N*8` fault, `10003 + N*8` online
- Input (FC04): `30001 + N*32` mode, `30002 + N*32` fan, `30003 + N*32` setpoint, `30006 + N*32` room temp, `30007 + N*32` error
- Holding control (FC10 preferred): starts at `40002 + N*25` for mode/fan/setpoint control triplet

Firmware behavior for Midea writes:

- Uses multi-register write (`0x10`) for mode/fan/setpoint control block
- Performs read-back verification from input/discrete status

## Example Configurations

### Multi V style (`idu_address_base=0`)

- `zones = 1:0,1:1,1:2`

### Single/Multi style (`idu_address_base=1`)

- `zones = 1:1,1:2,1:3`

## Notes

- Optional serial CLI can be added later; telnet JSON API is fully functional now.
