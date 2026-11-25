# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESP-IDF **v6.0.1** project for an **ESP32-S3 DevKit** (academy homework, Lesson 6.5 "Multicore, ISRs, Shared Resources"). The requirements are in `TASK.md`. Check every change against it. The project is a fresh template: `main/main.c` holds only an empty `app_main`, and all source lives in `main/` (`main/CMakeLists.txt` registers `main.c` only, so add any new `.c` file to `SRCS`).

## Build / flash (Linux)

IDF is installed by EIM. `export.sh` fails (no venv there), and EIM's activate script defines `idf.py` only as an alias, which non-interactive shells ignore. Shell state doesn't persist between calls, so use this in the same command every time:

```bash
. ~/.espressif/tools/activate_idf_v6.0.1.sh > /dev/null 2>&1 && python "$IDF_PATH/tools/idf.py" build
```

- `sdkconfig` is gitignored. If it's missing, run `idf.py set-target esp32s3` before the first build.
- Flash with `idf.py -p /dev/ttyACM0 flash` (or `/dev/ttyUSB0`, depending on which USB port is used. Check `ls /dev/tty{ACM,USB}*` first).
- Never run `idf.py monitor`. It's interactive and will hang, so ask the user to run it (`! idf.py monitor`) or capture a timed log via `build-ops`.
- clangd reads `compile_commands.json` from the hardcoded `build/` dir in `.vscode/settings.json`, so IDE diagnostics are only valid after a first build.
- A `.devcontainer` (image `espressif/idf`) exists as an alternative environment. It isn't the default.

## What the task requires (architecture)

Three cooperating pieces share two resources. Each sharing pattern is the point of the lesson:

- **Part counter** (`volatile` global). Button 1's ISR increments it, and tasks on both cores read it. On the dual-core S3, protect it with a `portMUX_TYPE` spinlock (`portENTER_CRITICAL_ISR` in the ISR, `portENTER_CRITICAL` in tasks) or an atomic. Disabling interrupts on one core isn't enough.
- **Report buffer** (one global `char[]`). Task 1 is pinned to core 0 (`xTaskCreatePinnedToCore`). Once per second it reads the counter, computes parts/min, and *formats* the buffer. Task 2 is pinned to core 1. Once per second it reads the ADC (`esp_adc/adc_oneshot.h`) and *appends* to the buffer. Guard all buffer access with one mutex, and never touch the buffer from an ISR.
- **Emergency stop**. Button 2 (or BOOT = GPIO0) triggers an ISR that wakes Task 3, the highest-priority task, which otherwise blocks forever (semaphore or task notification with `...FromISR` + `portYIELD_FROM_ISR`, no polling). Task 3 turns on the alarm LED, writes `SYSTEM HALTED!` into the buffer under the mutex, and stops further conveyor operation (counter updates and reports).
- ISRs stay minimal: no logging, no mutexes, no blocking calls. Debounce the buttons.

Pins (`#define`s at the top of `main/main.c`): Button 1 = GPIO4, Button 2 = GPIO5 and BOOT = GPIO0 (both buttons go to GND, with internal pull-ups and falling-edge interrupts), alarm LED = GPIO2 (active high), ADC = GPIO1 (ADC1 channel 0). Avoid the strapping pins and the USB D-/D+ pins GPIO19/20. Use only ADC1 channels.

Task 1 wakes Task 2 with a task notification after it formats the buffer. That's what keeps the order "format, then append" fixed across the two cores.

## Simulator

`diagram.json` + `wokwi.toml` run the built firmware (`build/flasher_args.json`) in the Wokwi VS Code extension (F1 → "Wokwi: Start Simulator"). Rebuild first. In the simulator, the pot stands in for the ADC sensor, and BOOT is an external button on GPIO0. When a pin changes, update `main/main.c`, `diagram.json`, `docs/wiring.svg` and the README wiring table together.

Automated test on Wokwi's servers (presses the buttons, turns the pot, checks the serial text and the LED pin). Build first. It needs `wokwi-cli` and `WOKWI_CLI_TOKEN` (from wokwi.com/dashboard/ci) in the environment. Never write the token into a repo file:

```bash
wokwi-cli --timeout 30000 --scenario test/wokwi-scenario.yaml .
```

Exit code 0 means every step passed, and 42 is a timeout (an expected text never appeared). Wokwi buttons bounce on press and on release. The parts ISR debounces on both edges (see `parts_isr`), so keep that when changing it. In Wokwi the calibrated mV value is wrong (full scale reads ~5090 mV), but the raw value is fine.

## Repo

GitHub repo `eoffers51/Lesson_6_5` (public, `main`, HTTPS remote). The README has the wiring picture, the simulator steps and the test table. Keep the table in step with `test/wokwi-scenario.yaml`. There is no saved online wokwi.com project yet, so the README has no Simulate badge.
