# HvacTelnetServer (ESP32)

This example provides:
- Telnet server that accepts one JSON command per line and replies with JSON.
- Web UI to configure WiFi, emitters, standard HVAC registrations, and config backups.
- OTA firmware updates via ArduinoOTA and web firmware upload.
- Optional web UI authentication (admin password) and a built-in HVAC test page.
- Raw IR send support for Pronto, GlobalCache, and Racepoint hex formats.

## Quick start
- Build for `esp32dev` with PlatformIO using `examples/HvacTelnetServer/platformio.ini`.
- On first boot (or if WiFi fails) the device starts an open AP `IR-HVAC-Setup`.
- Browse to `http://192.168.4.1/` and configure WiFi, emitters, and HVAC entries.

## Web UI capabilities
- **Config** (`/config`): WiFi, hostname, telnet port, optional web password.
- **Emitters** (`/emitters`): add/remove IR LED GPIOs.
- **HVACs** (`/hvacs`): register and edit HVAC entries (protocol, emitter, model).
- **Test HVAC** (`/hvacs/test`): send a JSON command from the browser, including `light`.
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
- `current_temp` mirrors `setpoint` (no ambient sensor input).

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
