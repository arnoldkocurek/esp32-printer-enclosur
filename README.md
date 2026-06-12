# ESP32 Smart 3D Printer Enclosure Controller

Firmware for a temperature-controlled 3D printer enclosure, built around an **ESP32-WROOM-32** and written in **embedded C/C++** (Arduino framework, PlatformIO). The controller keeps the chamber at a target temperature using a PTC heater with hysteresis control, manages intake/exhaust fans with PWM, drives RGBW chamber lighting, and provides a full on-device UI on an ILI9341 TFT navigated with 4 push buttons.

> 🎓 Embedded systems course project (SMiW) at the Silesian University of Technology — hardware and firmware designed, built and programmed individually. Full schematic and PCB layout were designed as part of the project.

<!-- TODO: add a photo of the enclosure here -->
<!-- ![Enclosure photo](docs/enclosure.jpg) -->

## Features

- **Automatic temperature control** — bang-bang regulation with hysteresis (±0.3 °C) around a user-set target (10–80 °C), based on a DS18B20 sensor inside the chamber
- **Heater over-temperature protection** — a second DS18B20 mounted behind the heater cuts power above 115 °C and re-enables it below 110 °C
- **Smart airflow** — intake fan enforces a minimum 40% duty while heating (below that threshold the fan physically doesn't spin — found during testing), with a periodic "boost" cycle (50% for 15 s every 120 s); exhaust fan ramps proportionally (25–70%) when the chamber overshoots the setpoint
- **Quiet operation** — 25 kHz fan PWM (no audible whine) and ramped speed changes (~8–10%/s)
- **RGBW chamber lighting** — HSV color picker rendered as an interactive palette on the TFT, plus a separate white channel
- **4-page on-device UI** (320×240): Dashboard, manual Control, Light, Auto — tab-based navigation with focus model, button debouncing and press-and-hold auto-repeat
- **Flicker-free rendering** — partial redraws of value fields only; the color palette is rendered to an off-screen buffer and blitted; the cursor crosshair restores underlying pixels instead of redrawing the whole screen

## Hardware

| Component | Role |
|---|---|
| ESP32-WROOM-32 dev kit | MCU |
| ILI9341 TFT, 320×240 (SPI) | UI display |
| 2× DS18B20 (1-Wire) | chamber temp (T2) + heater guard temp (T1) |
| 12 V PTC heater + IRLZ44N MOSFET (low-side) | heating (ON/OFF) |
| 2× 12 V fans + IRLZ44N MOSFETs | intake / exhaust (PWM 25 kHz) |
| 12 V RGBW LED strip + 4× IRLZ44N MOSFETs | chamber lighting (PWM 25 kHz) |
| 12 V DC supply + LM2596 step-down (12 → 5 V) | power |
| 4× push buttons | UI navigation (internal pull-ups) |

### Pinout

| Signal | GPIO |
|---|---|
| TFT CS / DC / RST | 5 / 2 / 21 |
| TFT MOSI / CLK | 23 / 18 |
| DS18B20 bus (1-Wire) | 4 |
| Fan IN / Fan OUT (PWM) | 25 / 26 |
| Heater | 33 |
| LED R / G / B / W | 17 / 16 / 22 / 19 |
| Buttons ▲ ▼ ◀ ▶ | 13 / 32 / 27 / 14 |

*Note: the TFT MISO line is unused — GPIO 19 is repurposed for the LED strip's white channel.*

## UI

- **DASH** — live overview: both temperatures, fan duty, heater state (view-only)
- **CTRL** — manual fan speed (10% steps, wrap-around 0↔100%) and heater toggle; touching manual controls automatically disables AUTO mode
- **LIGHT** — HUE×SAT palette with crosshair cursor + value/white sliders (changing VAL re-renders the palette)
- **AUTO** — setpoint adjustment (0.5 °C steps) and AUTO on/off, with live readout of both sensors, heater state and fan duty

Navigation: ◀/▶ switch tabs or change values, ▲/▼ move between tab bar and page rows. Holding a button auto-repeats after 350 ms.

## Control logic (AUTO mode)

```
every 1 s:
  read T1 (heater), T2 (box)
  if T1 > 115 °C → heater lockout until T1 < 110 °C
  hysteresis on T2:  ON when T2 ≤ SET − 0.3,  OFF when T2 ≥ SET + 0.3
  fan IN:  heating → ≥40% (fan startup threshold), boost to 50% for 15 s every 120 s
           cooling → 30–50% proportional to overshoot
  fan OUT: 25–70% proportional to overshoot in the 0.5–3.0 °C band
  all fan changes ramped (~8–10%/s) for quiet operation
```

## Problems encountered & solutions

- **IRL540N MOSFET didn't switch reliably at 3.3 V** gate drive → replaced with logic-level **IRLZ44N** (low R<sub>DS(on)</sub>, switches fully from an ESP32 GPIO)
- **Screen flicker on full redraws** → partial redraw of value fields only, off-screen palette buffer, crosshair drawn over the buffer with background restoration
- **Intake fan wouldn't start below ~40% PWM** → enforced minimum duty in the AUTO algorithm
- **Reversed up/down buttons on the physical board** → remapped in `#define`s with a consistent on-screen legend

## Build

The project uses the Arduino framework via [PlatformIO](https://platformio.org/) (recommended) or the Arduino IDE.

```bash
pio run -t upload
```

Libraries (resolved automatically by PlatformIO from `platformio.ini`):
- Adafruit GFX Library
- Adafruit ILI9341
- OneWire
- DallasTemperature

> ⚠️ The code uses the `ledcSetup`/`ledcAttachPin` LEDC API from Arduino-ESP32 core 2.x. `platformio.ini` pins the platform version accordingly. Update the sensor addresses in `src/main.cpp` (`sensor1`, `sensor2`) to match your DS18B20s.

## Author

Arnold Kocurek — B.Eng. in Teleinformatics, M.Sc. student in Electronics and Telecommunications (Wireless Devices and Systems) at the Silesian University of Technology.
