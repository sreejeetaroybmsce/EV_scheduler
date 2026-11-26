# Smart EV Charging / V2G Scheduler

A NodeMCU ESP8266 demo that simulates smart charging and vehicle-to-grid (V2G) scheduling for three EVs over one simulated night. The firmware balances battery targets, departure times, electricity prices, and a two-charger grid limit.

It also includes a browser dashboard that shows live state through the Web Serial API or runs from a built-in recorded demonstration.

## How It Works

- Simulates 12 hourly slots from 18:00 to 05:00.
- Treats slots 0-4 (18:00-22:00) as peak hours at 10 currency units per unit.
- Treats later hours as off-peak at 4 currency units per unit.
- Charges at 10 percentage points of state of charge (SoC) per hour.
- Discharges at 8 percentage points per hour while respecting the vehicle reserve.
- Limits simultaneous charging to two vehicles.
- Prioritizes urgent charging so a vehicle reaches its target before departure.
- Uses V2G discharge during peak hours when the vehicle has spare battery.
- Compares the smart cost with a peak-only charging strategy and reports savings.

## Hardware

- NodeMCU ESP8266 development board
- 2 LEDs
  - Green: one or more EVs are charging
  - Red: one or more EVs are discharging
- 3 switches or momentary buttons for EV plug state
- USB cable for power, firmware upload, and serial communication

### Pin Mapping

| Function | NodeMCU GPIO label |
|---|---|
| Green charging LED | D1 |
| Red V2G LED | D2 |
| V1 plug switch | D5 |
| V2 plug switch | D6 |
| V3 plug switch | D7 |

The switch pins use `INPUT_PULLUP`. Connect each switch between its pin and GND. Pressing or closing the switch pulls the pin LOW, which the firmware interprets as plugged in.

Add suitable series resistors for the LEDs. Add a pull-down or debounce hardware where needed if the switches are noisy.

## Vehicle Configuration

The default scenario is defined in `src/main.cpp`:

| Vehicle | Initial SoC | Target | Reserve | Departure | V2G |
|---|---:|---:|---:|---|---|
| V1 | 90% | 80% | 60% | End of night | Enabled |
| V2 | 20% | 90% | 60% | 01:00 | Enabled |
| V3 | 30% | 80% | 60% | End of night | Enabled |

Modify `CONFIG` in `src/main.cpp` to experiment with other SoC targets, departure times, reserves, switches, or V2G availability.

## Build and Upload

This project uses PlatformIO with the Arduino framework.

1. Install [Visual Studio Code](https://code.visualstudio.com/) and the PlatformIO IDE extension.
2. Open this project directory in VS Code.
3. Connect the NodeMCU by USB.
4. Upload the firmware:

```sh
pio run --target upload
```

Or use the PlatformIO upload button in VS Code.

To view the serial output:

```sh
pio device monitor
```

The serial monitor uses 115200 baud. It displays a live station view using terminal screen-clearing sequences.

## Browser Dashboard

Open `web/index.html` in Chrome or Microsoft Edge.

### Recorded Demo

Select **Play recorded demo** to run the dashboard without hardware. The demo includes plug and unplug events and a complete overnight cycle.

### Live Board

1. Upload and start the firmware.
2. Open `web/index.html`.
3. Select **Connect board**.
4. Choose the NodeMCU serial port in the browser dialog.

The dashboard reads the board's JSON snapshots. Each snapshot starts with `@` and contains the current phase, hour, tariff, charging/discharging LEDs, EV SoC, and scheduler decisions.

Web Serial is supported in Chromium-based browsers on HTTPS or localhost. It may require local hosting instead of opening the file directly:

```sh
python3 -m http.server 8000 --directory web
```

Then visit `http://localhost:8000`.

## Simulation Timing

The firmware cycles through three phases:

- **Arriving:** 6 seconds
- **Night:** 5 seconds per simulated hour
- **Morning:** 12 seconds

After the morning summary, the firmware resets and starts a new evening. These intervals are defined by `ARRIVE_MS`, `SLOT_INTERVAL_MS`, and `RESULT_MS` in `src/main.cpp`.

## Project Layout

```text
platformio.ini  PlatformIO board and serial configuration
src/main.cpp    Scheduler, EV state, LEDs, switches, and dashboard output
web/index.html  Browser dashboard and recorded demo
include/        Optional project headers
lib/            Optional project libraries
test/           Reserved for tests
```
