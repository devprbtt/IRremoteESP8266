# HvacTelnetServer (ESP32)

This example provides:
- Telnet server that accepts one JSON command per line and replies with JSON.
- Web UI to configure WiFi, emitters, standard HVAC registrations, and config backups.
- OTA firmware updates via ArduinoOTA and web firmware upload.
- Optional web UI authentication (admin password) and a built-in HVAC test page.
- Raw IR send support for Pronto, GlobalCache, and Racepoint hex formats.
- DINplug telnet integration for keypad-driven HVAC actions, including per-button LED follow modes.

## Quick start
- Build for `esp32dev` with PlatformIO using `examples/HvacTelnetServer/platformio.ini`.
- On first boot (or if WiFi fails) the device starts an open AP `IR-HVAC-Setup`.
- Browse to `http://192.168.4.1/` and configure WiFi, emitters, and HVAC entries.

## Web UI capabilities
- **Config** (`/config`): WiFi, hostname, telnet port, optional web password, WireGuard, and DS18B20 bus settings.
- **Config** (`/config`): WiFi, hostname, telnet port, optional web password, WireGuard, DS18B20 bus, and IR receiver log settings.
- **Config** (`/config`): WiFi, hostname, telnet port, optional web password, WireGuard, DS18B20 bus, IR receiver log settings, and Ethernet (WT32 LAN8720).
- **Emitters** (`/emitters`): add/remove IR LED GPIOs.
- **HVACs** (`/hvacs`): register and edit HVAC entries (protocol, emitter, model), current temperature source policy, DINplug keypads, and keypad button actions.
- **Test HVAC** (`/hvacs/test`): send a JSON command from the browser, including `light`.
- **DINplug** (`/dinplug`): configure DINplug gateway (`IP` or DNS hostname), connect-on-boot, and test connection.
- **Monitor** (`/monitor`): live telnet monitor log for RX/TX and DINplug activity.
- **Config backup**: download/upload `/config.json` at `/config/download` and `/config/upload`.
- **Firmware OTA** (`/firmware`): upload a `.bin` firmware image from the browser.

## OTA firmware update
- **ArduinoOTA** is enabled at boot. Hostname is `<hostname>.local` (`ir-server.local` by default).
- If a web admin password is set, the same password is also used for ArduinoOTA.
- **Web OTA**: open `/firmware`, upload a firmware `.bin`, and the device flashes and reboots.

> Note: There is no web form for custom HVAC code maps. Custom HVAC entries must be added by
> editing the JSON config (see below) and uploading it.

## Telnet JSON
Connect to port `4998` (or the configured telnet port) and send one JSON command per line.
Recommended line terminator is LF (`0x0A`). CRLF is also accepted.

List emitters & HVAC registrations:
```
{"cmd":"list"}
```

Send standard HVAC:
```
{"cmd":"send","id":"1","power":"on","mode":"cool","temp":24,"fan":"auto","light":"true"}
```

Query one HVAC state:
```
{"cmd":"get","id":"1"}
```

Query all HVAC states:
```
{"cmd":"get_all"}
```

Send custom HVAC (from config) using temp map or explicit code:
```
{"cmd":"send","id":"custom1","temp":18}
{"cmd":"send","id":"custom1","encoding":"pronto","code":"0000 006D 0000 0022 ..."}
{"cmd":"send","id":"custom1","encoding":"racepoint","code":"0000000000009470..."}
```

Raw ad-hoc Pronto/GC/Racepoint:
```
{"cmd":"raw","emitter":0,"encoding":"pronto","code":"0000 006D 0000 0022 ..."}
{"cmd":"raw","emitter":0,"encoding":"gc","code":"sendir,1:1,1,38000,1,1,172,172,22,64,..."}
{"cmd":"raw","emitter":0,"encoding":"racepoint","code":"0000000000009470..."}
```

State responses are one-line JSON objects like:
```
{"type":"state","id":"1","power":"on","mode":"cool","setpoint":24,"current_temp":24,"fan":"auto","light":"on"}
```

Behavior notes:
- `send` returns an immediate full `type:"state"` acknowledgement.
- `get` returns one `type:"state"` object.
- `get_all` returns a JSON array of `type:"state"` objects.
- On telnet client connect/reconnect, the server immediately pushes one `type:"state"` line for each registered HVAC.
- `current_temp` is per-HVAC configurable:
  - `setpoint`: current temp equals setpoint.
  - `sensor`: current temp comes from DS18B20 sensor index configured for that HVAC (fallback to setpoint if sensor is unavailable).
- When HVAC state changes from non-telnet sources (e.g. DINplug keypad actions), monitor logs include `TX state {...}` JSON entries.

## DS18B20 temperature sensors
Configure DS18B20 in **Config** (`/config`):
- Enable/disable DS18B20 bus.
- OneWire GPIO pin.
- Sensor read interval (seconds).

At runtime:
- The firmware discovers sensors on boot and stores them by index (`0..N-1`).
- HVAC entries can select `Current Temp Source` (`setpoint` or `sensor`) and `DS18B20 Sensor Index`.
- Sensor-driven temperature updates are periodically reflected in HVAC state and broadcast to telnet clients.

## Ethernet (WT32-ETH01)
Configure Ethernet in **Config** (`/config`):
- Enable/disable Ethernet (`LAN8720`).

When enabled, firmware tries Ethernet first. If no link/IP is obtained, it falls back to WiFi STA or setup AP logic.

WT32 defaults used by firmware:
- PHY: `LAN8720`
- PHY address: `1`
- Power pin: `GPIO16`
- MDC: `GPIO23`
- MDIO: `GPIO18`
- Clock mode: `ETH_CLOCK_GPIO0_IN`

## IR receiver log input
Configure IR receiver in **Config** (`/config`):
- Enable/disable IR receiver.
- IR receiver GPIO pin.
- Log mode:
  - `auto`: tries protocol decode and logs protocol/code summary.
  - `pronto`: logs the received signal converted to Pronto hex (38 kHz base).
  - `rawhex`: logs raw timing words as hexadecimal.

At runtime, received IR messages are printed to serial and added to the Monitor log (`/monitor`) when monitor logging is enabled.

## DINplug integration
- Gateway supports `IP` or DNS hostname (e.g. DynDNS): set in `/dinplug`.
- `Test connection` button at `/dinplug` forces an immediate connect attempt and reports success/failure.
- Auto-connect on boot is optional.
- DINplug button events supported for HVAC automation: `PRESS` and `HOLD`.

HVAC editor (`/hvacs`) DINplug fields:
- `DINplug Keypad IDs (comma-separated)`: links one HVAC to multiple keypads.
- Keypad Button Actions table per row:
  - `Keypad ID`: optional per-row keypad filter. `0` means match any linked keypad.
  - `Button ID`: DINplug button/LED numeric id.
  - `Press/Hold Action` + `Value`: `Value` is used only for `temp_up`, `temp_down`, `set_temp`.
  - `Toggle Power Mode`: mode used when `toggle_power` turns the HVAC on (`auto`, `cool`, `heat`, `dry`, `fan`).
  - `LED follows HVAC power`:
    - `disabled`
    - `LED on when HVAC on`
    - `LED on when HVAC off`

DINplug action list:
- `none`
- `temp_up`, `temp_down`, `set_temp`
- `power_on`, `power_off`, `toggle_power`
- `mode_heat`, `mode_cool`, `mode_fan`, `mode_auto`, `mode_off`

LED follow implementation notes:
- Uses DINplug command `LED <keypad_id> <led_id> <state>`.
- `led_id` is taken from the row `Button ID`.
- If row `Keypad ID` is `0`, LED command is sent to all linked keypad IDs for that HVAC.

## Custom HVAC entries via config.json
Custom HVAC registrations are supported in the saved config (upload/download format). A custom
HVAC entry uses a protocol of `CUSTOM` (or `custom` flag in config) and specifies an IR encoding
(`pronto`, `gc`, or `racepoint`), an optional off code, and an optional temp-to-code map.

Example snippet:
```
{
  "hvacs": [
    {
      "id": "custom1",
      "protocol": "CUSTOM",
      "emitter": 0,
      "model": -1,
      "current_temp_source": "sensor",
      "temp_sensor_index": 0,
      "custom": {
        "encoding": "pronto",
        "off": "0000 006D ...",
        "temps": {
          "18": "0000 006D ...",
          "24": "0000 006D ..."
        }
      }
    }
  ]
}
```

## Racepoint format notes
Racepoint/Savant hex strings are accepted as a contiguous hex blob or with separators; non-hex
characters are ignored. The first 16-bit word between 20000 and 60000 is treated as the carrier
frequency in Hz. All following 16-bit words are treated as mark/space durations in carrier
cycles and converted to microseconds. Trailing 0000 words are ignored.

Example (from a Racepoint XML profile):
```
000000000000948c015700AC00150041001500160015001600150041001500160015001600150016001500160015001600150041001500410015001600150041001500410015004100150041001500410015004100150041001500160015004100150041001500160015001600150016001500160015001600150041001500160015001600150041001500410015061100000000
```

## Accessing the device
- Default hostname (mDNS): `ir-server.local`
- Default setup AP: `IR-HVAC-Setup` (browse to `http://192.168.4.1/` on first boot)
