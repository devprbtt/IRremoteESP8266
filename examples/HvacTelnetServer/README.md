# IR Server Telnet (ESP32)

ESP32 IR bridge with:
- Telnet JSON API (one command per line, one JSON response).
- Web UI for WiFi/Ethernet, emitters, devices, DINplug, diagnostics, API reference, and OTA.
- Standard HVAC protocols (IRremoteESP8266 `IRac`) plus `CUSTOM` commands.
- Optional IR receiver logging and IR learn flow for custom command capture.
- Explicit firmware/filesystem version tracking with UI mismatch warning.

## Quick start
- Build with PlatformIO using `examples/HvacTelnetServer/platformio.ini`.
- Flash your board (for WT32, use the matching ESP32 environment you already use).
- On first boot (or if station connection fails), AP mode starts as `IR-Server-Setup`.
- Open `http://192.168.4.1/` and configure networking and emitters.

## Web UI
- `/config`: WiFi, hostname, telnet port, optional web password, DS18B20, IR receiver, Ethernet (WT32 LAN8720).
- `/emitters`: add/remove IR LED GPIO emitters.
- `/devices`: add/edit device profiles, including HVAC entries, `CUSTOM` command sets, and DINplug button mappings.
- `/devices/test`: send test commands for standard HVAC and custom device profiles.
- `/raw/test` (from Test page): send ad-hoc raw code (`pronto`, `gc`, `racepoint`, `rawhex`).
- `/dinplug`: DINplug gateway config + test.
- `/system`: Monitor, live stats, API reference, firmware updates, and config backup/restore in one page.
- `/config/download`: download current config JSON.

## Versioning
- Current firmware version: `0.2.0`
- Current filesystem/UI version: `0.2.0`
- The firmware exposes:
  - `firmware_version`
  - `filesystem_version`
  - `filesystem_version_expected`
  - `version_match`
- If the running UI/filesystem does not match what the firmware expects, the web UI shows a warning banner.
- Normal updates do not erase saved configuration by themselves. A mismatch warning means you should upload the matching `firmware.bin` or `spiffs.bin`, not that your settings were wiped.

## Custom protocol workflow
In **Add Device Profile** (`/devices`):
1. Select protocol `CUSTOM`.
2. Add one or more commands (name + encoding + code).
3. Use **Learn** on a command row to capture from IR receiver.
4. Add a profile name so the custom device is easy to identify in the list.
5. Click **Add Device Profile** after your command list is ready.

Notes:
- Supported custom encodings: `pronto`, `gc`, `racepoint`, `rawhex`.
- Learn flow supports cancel and has a 10s timeout.
- Learned/custom commands are shown in the device list and can be deleted/edited.
- DINplug button actions can target custom commands via `custom:<name>`.

Typical custom profile examples:
- `TV`
- `Projector`
- `Receiver`
- `Screen`

## IR receiver
Configurable in `/config`:
- Enable/disable receiver.
- GPIO selection.
- Log mode: `auto`, `pronto`, `rawhex`.

Runtime behavior:
- Captures are printed to Serial.
- Captures are available through the learn flow and diagnostics tooling. Live monitor logging is enabled again, but the retained in-memory history is intentionally shorter to reduce memory churn.

## DS18B20 temperature sensors
Configure DS18B20 in `/config`:
- Enable/disable DS18B20 bus.
- OneWire GPIO pin.
- Sensor read interval (seconds).

Runtime behavior:
- Sensors are discovered at boot and indexed (`0..N-1`).
- Each HVAC can use `current_temp_source`:
  - `setpoint`: `current_temp` mirrors setpoint.
  - `sensor`: `current_temp` comes from selected DS18B20 index.
- If a configured sensor is unavailable, firmware falls back to setpoint for state continuity.

## Telnet JSON API
Default telnet port is `4998` (configurable).

### Basic commands
List config summary:
```json
{"cmd":"list"}
```

Get one device state:
```json
{"cmd":"get","id":"1"}
```

Get all device states:
```json
{"cmd":"get_all"}
```

Get runtime/version status:
```json
{"cmd":"status"}
```

Send standard HVAC command:
```json
{"cmd":"send","id":"1","power":"on","mode":"cool","temp":24,"fan":"auto","light":"true"}
```

Send custom device command by registered command name:
```json
{"cmd":"send","id":"5","command_name":"power_on"}
```

Send raw ad-hoc code:
```json
{"cmd":"raw","emitter":0,"encoding":"pronto","code":"0000 006D 0000 0022 ..."}
{"cmd":"raw","emitter":0,"encoding":"gc","code":"sendir,1:1,1,38000,1,1,172,172,..."}
{"cmd":"raw","emitter":0,"encoding":"racepoint","code":"0000000000009470..."}
{"cmd":"raw","emitter":0,"encoding":"rawhex","code":"0156 00AC 0015 ..."}
```

### Response shape
Standard HVAC state response includes runtime fields like:
- `power`, `mode`, `setpoint`, `current_temp`, `fan`, `light`

Example:
```json
{"type":"state","id":"1","protocol":"MITSUBISHI_AC","custom":false,"power":"on","mode":"cool","setpoint":24,"current_temp":24,"fan":"auto","light":"on"}
```

Custom device state response includes custom-focused fields:
- `type`, `id`, `protocol: "CUSTOM"`, `custom: true`
- `custom_commands` (name + encoding)
- For `send`, response may also include `encoding` and `command_name`

Example:
```json
{"type":"state","id":"5","protocol":"CUSTOM","custom":true,"custom_commands":[{"name":"power_on","encoding":"pronto"},{"name":"power_off","encoding":"pronto"}],"encoding":"pronto","command_name":"power_on"}
```

Behavior notes:
- `send` returns immediate acknowledgement as JSON.
- `get` returns a single state object.
- `get_all` returns an array of state objects.
- `status` returns runtime diagnostics and version info.
- On telnet client connect/reconnect, current states are pushed automatically.
- State changes from non-telnet sources (for example DINplug actions) are also broadcast to telnet clients.
- `/system` now includes an API tab with these commands and examples for third-party integrations.

## HTTP API
All HTTP endpoints are local to the device and, when web authentication is enabled, require the same authentication used by the browser UI.

### Read-only endpoints
Get the full persisted configuration JSON:
- `GET /api/config`

Get one device state by firmware `id`:
- `GET /api/device/get?id=1`

Get all device states:
- `GET /api/device/get_all`

Get live runtime/system status:
- `GET /api/status`

Returned fields include:
- `firmware_version`
- `filesystem_version`
- `filesystem_version_expected`
- `version_match`
- `network_mode`
- `ip`
- `hostname`
- `uptime_ms`
- `telnet_port`
- `dinplug_status`
- `emitter_count`
- `hvac_count`
- `dinplug_bindings_used`
- `dinplug_bindings_total`
- `ir_receiver_enabled`
- `ir_receiver_gpio`
- `heap_free`
- `heap_min_free`
- `heap_max_alloc`
- `telnet_clients_active`
- `telnet_clients`
- `temp_sensors_enabled`
- `temp_sensor_count`
- `temp_sensor_precision`
- `temp_sensors`
- `ethernet_enabled`
- `wifi_rssi`

Example:
```json
{
  "network_mode": "WiFi STA",
  "ip": "192.168.51.10",
  "hostname": "ir-server",
  "uptime_ms": 123456,
  "telnet_port": 4998,
  "dinplug_status": "connected",
  "emitter_count": 1,
  "hvac_count": 3,
  "dinplug_bindings_used": 5,
  "dinplug_bindings_total": 24,
  "heap_free": 213456,
  "heap_min_free": 190120,
  "heap_max_alloc": 131060,
  "telnet_clients_active": 2,
  "wifi_rssi": -58
}
```

Get UI/integration metadata and limits:
- `GET /api/meta`

Returned fields include:
- `firmware_version`
- `filesystem_version`
- `filesystem_version_expected`
- `version_match`
- `protocols`
- `gpio_options`
- `din_actions`
- `toggle_modes`
- `mode_overrides`
- `light_modes`
- `max_custom_commands`
- `max_dinplug_buttons`
- `max_dinplug_bindings_total`
- `dinplug_bindings_used`
- `dinplug_bindings_available`
- `max_temp_sensors`
- `max_emitters`
- `max_hvacs`

Scan for nearby WiFi networks:
- `GET /api/wifi/scan`

Example response:
```json
{
  "networks": [
    {"ssid": "Office", "rssi": -52},
    {"ssid": "Guest", "rssi": -71}
  ]
}
```

### Device control endpoints
These HTTP endpoints mirror the telnet control API and support both standard HVAC profiles and custom command profiles.

Send a standard HVAC command or a custom profile command:
- `POST /api/device/send`

Accepted as either:
- JSON body, content type `application/json`
- form fields in the POST body

Standard HVAC example:
```bash
curl -X POST http://ir-server.local/api/device/send \
  -H "Content-Type: application/json" \
  -d '{"id":"1","power":"on","mode":"cool","temp":24,"fan":"auto","light":"true"}'
```

Custom profile example using a registered command:
```bash
curl -X POST http://ir-server.local/api/device/send \
  -H "Content-Type: application/json" \
  -d '{"id":"5","command_name":"power_on"}'
```

Form-encoded example:
```bash
curl -X POST http://ir-server.local/api/device/send \
  -d "id=1" \
  -d "power=on" \
  -d "mode=cool" \
  -d "temp=24" \
  -d "fan=auto" \
  -d "light=true"
```

Supported standard HVAC fields:
- `id`
- `power`
- `mode`
- `temp`
- `fan`
- `swingv`
- `swingh`
- `light`
- `quiet`
- `turbo`
- `econo`
- `filter`
- `clean`
- `beep`
- `sleep`
- `clock`
- `celsius`
- `model`

Supported custom profile fields:
- `id`
- `command_name`
- `command`
- `code`
- `encoding`
- optional state fields like `mode`, `temp`, `fan`, `light`

Response behavior:
- success returns the same state JSON shape as telnet `send`
- application-level errors return JSON such as:
```json
{"ok":false,"error":"unknown_id"}
{"ok":false,"error":"unknown_custom_command"}
{"ok":false,"error":"send_failed"}
```

Send raw IR without a configured device profile:
- `POST /api/device/raw`

Accepted as either JSON or form data.

Example:
```bash
curl -X POST http://ir-server.local/api/device/raw \
  -H "Content-Type: application/json" \
  -d '{"emitter":0,"encoding":"pronto","code":"0000 006D 0000 0022 ..."}'
```

Form-encoded example:
```bash
curl -X POST http://ir-server.local/api/device/raw \
  -d "emitter=0" \
  -d "encoding=pronto" \
  -d "code=0000 006D 0000 0022 ..."
```

Response examples:
```json
{"ok":true}
{"ok":false,"error":"invalid_emitter"}
{"ok":false,"error":"send_failed"}
```

### Configuration write endpoint
Replace the full saved configuration and reboot:
- `POST /api/config/save`
- body: raw JSON matching the exported config format
- content type: `application/json`

Success response:
```json
{"ok":true,"rebooting":true}
```

Failure responses:
```json
{"ok":false,"error":"missing_body"}
{"ok":false,"error":"invalid_json","detail":"..."}
{"ok":false,"error":"config_write_failed"}
```

Example:
```bash
curl -X POST http://ir-server.local/api/config/save \
  -H "Content-Type: application/json" \
  --data @config.json
```

### Monitor API
Live monitor logging is enabled again in this build, but the retained history is intentionally shorter.

`GET /api/monitor` returns the current enabled state, category filters, and the most recent retained lines.

Returned fields:
- `enabled`
- `filters.telnet`
- `filters.state`
- `filters.dinplug`
- `filters.ir`
- `lines`

Example:
```json
{
  "enabled": true,
  "filters": {
    "telnet": true,
    "state": true,
    "dinplug": true,
    "ir": false
  },
  "lines": [
    "[2026-03-25 11:59:00] [123456 ms] wifi: reconnected ip=192.168.51.10"
  ]
}
```

Set monitor enabled state and category filters:
- `POST /monitor/toggle`
- form body fields:
  - `enabled=0|1`
  - `telnet=0|1`
  - `state=0|1`
  - `dinplug=0|1`
  - `ir=0|1`

Current behavior:
- returns `ok: true` and applies the requested enabled state and category filters

Response:
```json
{
  "ok": true,
  "enabled": true,
  "filters": {
    "telnet": true,
    "state": true,
    "dinplug": false,
    "ir": false
  }
}
```

Clear the in-memory monitor log:
- `POST /monitor/clear`

### Persisted diagnostics
Get the latest persisted diagnostics snapshot:
- `GET /api/diagnostics`
- add `?download=1` to force a `diagnostics.json` download in the browser

What it contains:
- `saved_at`
- `uptime_ms`
- `firmware_version`
- `filesystem_version`
- `boot_count`
- `reset_reason`
- `network_mode`
- `ip`, `gateway`, `subnet`, `dns`
- `wifi_rssi`
- `heap_free`, `heap_min_free`, `heap_max_alloc`
- `telnet_clients_active`
- `dinplug_status`
- `recent_lines`
- `trend_samples`

How it works:
- the firmware keeps a small rolling snapshot in SPIFFS
- it also records lightweight runtime trend samples once per minute
- snapshots are debounced to avoid constant flash writes
- this is meant to preserve the latest useful breadcrumbs if the board later locks up and you need to hard power-cycle it
- it is not a full crash dump, so events that happen after the CPU has already stopped executing cannot be recorded

Response:
```json
{"ok":true}
```

### IR learn API
Used by the web UI and available for direct calls.

Start a learn session:
- `POST /api/ir/learn/start`
- form body: `encoding=<pronto|gc|racepoint|rawhex>`

Success response:
```json
{"ok":true,"active":true,"encoding":"pronto"}
```

Disabled receiver response:
```json
{"ok":false,"error":"ir_receiver_disabled"}
```

Poll learn status:
- `GET /api/ir/learn/poll`

Fields:
- `ok`
- `active`
- `ready`
- `encoding`
- `elapsed_ms`
- `error` when failed or cancelled
- `code` when ready

Examples:
```json
{"ok":true,"active":true,"ready":false,"encoding":"pronto","elapsed_ms":1420}
{"ok":true,"active":false,"ready":true,"encoding":"pronto","elapsed_ms":3221,"code":"0000 006D 0000 0022 ..."}
{"ok":true,"active":false,"ready":false,"encoding":"pronto","elapsed_ms":10015,"error":"timeout"}
```

Cancel a learn session:
- `POST /api/ir/learn/cancel`

### Maintenance HTTP endpoints
These are used by the web UI rather than third-party automation, but they are part of the device HTTP surface:

- `POST /firmware/update`
  - multipart form upload field: `firmware`
- `POST /spiffs/update`
  - multipart form upload field: `filesystem`
- `POST /system/factory-reset`
  - clears saved config and persisted runtime state, then reboots

### HTTP integration notes
- `GET /api/config` returns the same JSON structure used by `/config/download`.
- `GET /api/device/get` and `GET /api/device/get_all` return the same state objects used by the telnet API.
- `POST /api/device/send` and `POST /api/device/raw` reuse the same backend logic as telnet commands.
- `POST /api/config/save` replaces the full config; it is not a partial patch endpoint.
- `/api/monitor` is read-only; use `/monitor/toggle` and `/monitor/clear` to manage monitor behavior.
- The web UI in `/system#api` includes copyable examples and a read-only API tester for safe endpoints.

## Home Assistant
- The bundled custom component lives under `examples/HvacTelnetServer/homeassistant/custom_components/hvactelnet`.
- The integration supports Zeroconf discovery once the custom component is installed in Home Assistant.
- Firmware advertises `_hvactelnet._tcp.local.` over mDNS on the telnet port.
- Climate entities are exposed as control entities.
- Custom profile commands are exposed as button entities.
- Diagnostic entities are exposed as sensor entities for items like:
  - firmware version
  - filesystem version
  - version status
  - WiFi RSSI
  - free heap
  - telnet client count
  - network mode

## DINplug
- Supports IP or DNS hostname gateway.
- Button actions support standard HVAC actions and `custom:<name>`.
- LED follow modes are supported per button mapping row.
- DINplug bindings use a shared global pool to keep ESP32 RAM usage bounded.
- Current total DINplug binding capacity: `24` bindings across all device profiles combined.
- Firmware/SPIFS updates do not wipe saved config by themselves; current settings remain unless you explicitly factory reset or erase flash.

HVAC editor fields for DINplug:
- `DINplug Keypad IDs (comma-separated)`: link one HVAC to one or more keypads.
- `Add Binding`: add DINplug button mappings only where needed, up to the remaining global pool capacity shown in the UI.
- `Keypad Button Actions` table:
  - `Keypad ID`: optional row filter (`0` means match any linked keypad).
  - `Button ID`: button/LED ID from DINplug.
  - `Press Action` / `Hold Action` and matching value fields.
  - `Toggle Power Mode`: mode used when `toggle_power` turns HVAC on.
  - `LED follows HVAC power`: disabled/on-when-on/on-when-off.

Action list:
- `none`
- `temp_up`, `temp_down`, `set_temp`
- `power_on`, `power_off`, `toggle_power`
- `mode_heat`, `mode_cool`, `mode_fan`, `mode_auto`, `mode_off`
- `custom:<name>` for commands defined on a `CUSTOM` HVAC

LED follow notes:
- Firmware sends `LED <keypad_id> <led_id> <state>`.
- `led_id` comes from the row `Button ID`.
- If row `Keypad ID` is `0`, LED updates are sent to all keypads linked to that HVAC.

## OTA
- ArduinoOTA is enabled at boot (`<hostname>.local`).
- If web password is set, the same password is used for ArduinoOTA.
- Web OTA in `/system` shows upload progress and then auto-attempts reconnect to the main page after reboot.

## Networking behavior
- If Ethernet is enabled, firmware tries Ethernet first; if link/IP fails, it falls back to WiFi/AP flow.
- In AP mode, captive portal endpoints redirect clients automatically to the web UI.
- After saving config that triggers reboot, the web page auto-attempts reconnection.

## WT32 Ethernet defaults
- PHY: `LAN8720`
- PHY address: `1`
- Power pin: `GPIO16`
- MDC: `GPIO23`
- MDIO: `GPIO18`
- Clock mode: `ETH_CLOCK_GPIO0_IN`

## Racepoint format notes
`racepoint`/Savant hex is accepted as contiguous hex or split with separators. Non-hex characters are ignored.

Decoder behavior:
- The first 16-bit word between `20000` and `60000` is treated as carrier frequency (Hz).
- Following 16-bit words are interpreted as mark/space durations in carrier cycles and converted to microseconds.
- Trailing `0000` words are ignored.

Example:
```text
000000000000948c015700AC00150041001500160015001600150041001500160015001600150016001500160015001600150041001500410015001600150041001500410015004100150041001500410015004100150041001500160015004100150041001500160015001600150016001500160015001600150041001500160015001600150041001500410015061100000000
```

## Access
- mDNS: `http://ir-server.local/` (default hostname).
- Setup AP portal: `http://192.168.4.1/`.
