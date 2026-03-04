# ESP32 Daikin D3Net Firmware Skeleton

This folder contains an ESP-IDF firmware implementation scaffold that mirrors the behavior of this Home Assistant integration and adds device UX services:

- unit discovery via `SystemStatus`
- per-unit capability/status loading
- periodic status polling
- `prepare -> modify status shadow -> commit` write flow
- throttled Modbus access and post-write read suppression
- Wi-Fi AP+STA mode
- provisioning web UI (scan APs with RSSI and connect)
- OTA upload endpoint with progress reporting
- telnet diagnostics server
- HVAC listing and command API

## Scope

This code talks to the Daikin DTA116A51 Modbus interface over Modbus RTU (RS485), not raw DIII-Net protocol.

## Project Layout

- `main/main.c`: app entry point and task orchestration.
- `main/d3net_gateway.[ch]`: gateway state machine and unit management.
- `main/d3net_codec.[ch]`: register/bitfield codec equivalent to integration logic.
- `main/wifi_manager.[ch]`: AP+STA startup, scan, and STA connect helpers.
- `main/web_server.[ch]`: web UI and REST API.
- `main/telnet_server.[ch]`: telnet monitor stream on port 23.
- `main/config_store.[ch]`: NVS persistence for STA credentials.

## Build

1. Install ESP-IDF (v5.x recommended).
2. From this folder run:

```bash
idf.py set-target esp32
idf.py build
```

## RS485 RTU Wiring

This firmware now includes a native RTU transport (`main/modbus_rtu.[ch]`) and uses it directly from `main.c`.

Default pin mapping in `main.c`:
- ESP32 TX: GPIO17
- ESP32 RX: GPIO16
- MAX3485 DE: GPIO4
- MAX3485 RE: GPIO5
- RS485 bus: adapter `A` and `B` to DTA116A51 RS485 `A/B`

If your board ties `RE` and `DE` together, either wire both to one GPIO and set both pins to that GPIO in `rtu_cfg`, or set `re_pin = -1` and wire `RE` low permanently.

Default serial settings in `main.c` (tuned to the DTA116A51 setup we validated):
- baud: 19200
- data bits: 8
- parity: None
- stop bits: 2
- slave ID: 1

Adjust these values in `modbus_rtu_config_t rtu_cfg` in `main/main.c` for your hardware.

## Web/API

- Setup AP defaults:
  - SSID: `DaikinD3Net-Setup`
  - Password: `daikinsetup`
- Open `http://192.168.4.1/` when connected to the setup AP.
- Endpoints:
  - `GET /api/status`
  - `GET /api/wifi/scan`
  - `POST /api/wifi/connect`
  - `POST /api/discover`
  - `GET /api/hvac`
  - `POST /api/hvac/cmd`
  - `POST /api/ota`

`/api/hvac/cmd` body example:

```json
{"index":0,"cmd":"setpoint","value":23.5}
```

Available `cmd` values:
- `power` (`value` 0/1)
- `mode` (`value` enum int)
- `setpoint` (`value` float C)
- `fan_speed` (`value` enum int)
- `fan_dir` (`value` enum int)
- `filter_reset`

## Timing Defaults

- Poll interval: 10s
- Modbus operation throttle: 25ms
- Post-write read suppression: 35s

These match the integration defaults and can be adjusted in `d3net_gateway.h`.
