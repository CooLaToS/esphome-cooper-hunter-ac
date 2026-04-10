# Cooper & Hunter AC ESPHome Controller

Local control for a Cooper & Hunter mini-split using ESPHome and a BSS138-based UART level shifter. This setup replaces the original YD6800 WiFi module and exposes the indoor unit as a Home Assistant climate entity.

This project is provided as-is, without warranty. Use it at your own risk.

## What It Does

- Local Home Assistant control — no cloud dependency  
- UART-based control through a custom ESPHome external component  
- Power, mode, target temperature, fan speed, swing control  
- Sleep preset, health/ionizer toggle, front-panel display on/off  
- Off-timer and on-timer support (0–1440 min, 1-minute resolution)  
- Timer remaining exposed in Home Assistant  
- Optional onboard OLED display (IP, mode, temperatures)  
- ESPHome web interface for local access  
- Boot button on GPIO9 toggles power (restores last active mode or OFF)  

## Hardware

Tested hardware:

- ESP32-C3 board with OLED  
- Standard ESP32 dev board  
- BSS138 4-channel bidirectional level shifter  
- Cooper & Hunter indoor unit using the original WiFi module connector  

## Wiring

### ESP32-C3 → Level Shifter

| ESP32-C3 | Level Shifter | Purpose |
|---|---|---|
| `5V` / `VIN` | `HV` | 5V power |
| `3V3` | `LV` | 3.3V side power |
| `GND` | `GND` | Common ground |
| `GPIO3` | `LV1` | UART TX to AC |
| `GPIO4` | `LV2` | UART RX from AC |

### Level Shifter → AC

| Level Shifter | AC | Purpose |
|---|---|---|
| `HV` | Red `5V` | Power |
| `GND` | Black `GND` | Ground |
| `HV1` | Blue `AC RX` | AC receives commands |
| `HV2` | Yellow `AC TX` | AC sends status |

OLED pins used by the sample config:

- `GPIO5` = `SDA`
- `GPIO6` = `SCL`

## Repository Layout

```text
.
├── esp32-c3-cooperhunter.yaml
├── secrets.example.yaml
├── init_secrets.py
├── LICENSE
├── components/
│   └── cooper_hunter_ac/
│       ├── __init__.py
│       ├── climate.py
│       └── cooper_hunter_ac.h
```

## Setup

1. Generate `secrets.yaml` automatically:

```python
python3 init_secrets.py
```

2. Edit `secrets.yaml` and set your Wi‑Fi credentials.  
3. Flash the device:

### Example for macOS; adjust --device for your system
```zsh
esphome run esp32-c3-cooperhunter.yaml --device /dev/cu.usbmodem101
```

4. After the first flash, use OTA or `esphome logs esp32-c3-cooperhunter.yaml` to find the IP.  
5. Open the ESPHome web UI or adopt the device from Home Assistant.

The generated secrets include:

- `cooper_hunter_api_key` – ESPHome API encryption key  
- `cooper_hunter_ota_password` – OTA password  
- `web_user` – defaults to `admin`  
- `web_pass` – random web password  

## Home Assistant

The device exposes:

| Entity | Type | Description |
|--------|------|-------------|
| `climate.cooper_hunter_ac` | Climate | Mode, temp, fan, swing, sleep preset |
| `switch.cooper_hunter_display` | Switch | AC front panel display on/off |
| `switch.cooper_hunter_health` | Switch | Health/ionizer on/off |
| `switch.cooper_hunter_on_timer` | Switch | Selects on-timer vs off-timer mode |
| `number.cooper_hunter_timer` | Number | Timer duration (0–1440 min) |
| `sensor.cooper_hunter_timer_remaining` | Sensor | Live timer countdown |
| `sensor.cooper_hunter_wifi_signal` | Sensor | Wi‑Fi signal strength |
| `binary_sensor.cooper_hunter_boot_button` | Binary sensor | Onboard GPIO9 power toggle |

## Notes

- Custom component requires `framework: arduino`.  
- Protocol is `9600 8N1` UART with `AA AA 12` framing.  
- Target temperature is encoded as `setpoint_c - 16` in the low 5 bits of the temperature byte.  
- Timer resolution is 1 minute (confirmed by hardware test).  
- Off‑timer and on‑timer use different minute byte ordering in the packets; the component handles this when parsing and sending timer values.  
- Only the features listed above are implemented; things like turbo/eco/quiet or extra vane presets would need extra reverse‑engineering.

## Safety

This is an unofficial project and not affiliated with Cooper & Hunter. Double‑check wiring (especially 5V and UART lines) before powering the unit.