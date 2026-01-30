# HvacTelnetServer (ESP32)

This example provides:
- Telnet server that accepts JSON commands and replies with JSON only.
- Web UI to manage WiFi, emitters, HVAC registrations, and config backups.
- Custom HVAC entries using Pronto or GlobalCache (GC) IR codes.

## Quick start
- Build for `esp32dev` with PlatformIO using `examples/HvacTelnetServer/platformio.ini`.
- On first boot the device starts an open AP `IR-HVAC-Setup`.
- Browse to `http://192.168.4.1/` and configure WiFi and emitters.

## Telnet JSON
Connect to port `4998` and send one JSON per line.

List:
```
{"cmd":"list"}
```

Send standard HVAC:
```
{"cmd":"send","id":"living","power":"on","mode":"cool","temp":24,"fan":"auto"}
```

Send custom HVAC (uses stored temp map or off code):
```
{"cmd":"send","id":"custom1","temp":18}
```

Raw ad-hoc Pronto/GC:
```
{"cmd":"raw","emitter":0,"encoding":"pronto","code":"0000 006D 0000 0022 ..."}
{"cmd":"raw","emitter":0,"encoding":"gc","code":"sendir,1:1,1,38000,1,1,172,172,22,64,..."}

## Accessing the device
- Default hostname (mDNS): `ir-server.local`
- Default setup AP: `IR-HVAC-Setup` (browse to `http://192.168.4.1/` on first boot)
```

All responses are JSON like:
```
{"ok":true}
```

## Config backup
- Download: `/config/download`
- Upload: `/config/upload`
- JSON schema matches `/api/config`.
