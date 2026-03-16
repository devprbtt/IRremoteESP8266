# IR Server Telnet (ESP32)

ESP32 IR bridge with:
- Telnet JSON API (one command per line, one JSON response).
- Web UI for WiFi/Ethernet, emitters, devices, DINplug, monitor, API reference, and OTA.
- Standard HVAC protocols (IRremoteESP8266 `IRac`) plus `CUSTOM` commands.
- Optional IR receiver logging and IR learn flow for custom command capture.

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
- Captures are added to the Monitor tab in `/system` when monitor logging is enabled.

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
- On telnet client connect/reconnect, current states are pushed automatically.
- State changes from non-telnet sources (for example DINplug actions) are also broadcast to telnet clients.
- `/system` now includes an API tab with these commands and examples for third-party integrations.

## IR learn API
Used by the web UI and available for direct calls:
- `POST /api/ir/learn/start` (body: `encoding=<pronto|gc|racepoint|rawhex>`)
- `GET /api/ir/learn/poll`
- `POST /api/ir/learn/cancel`

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
