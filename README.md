# ESP32 Web Debugger (Pure ESP-IDF)

A web-based GPIO / bus / analog debugger **and hardware benchmark suite** for the ESP32 family, written entirely in **ESP-IDF v5.3** (no Arduino). Flash it, join its Wi-Fi network (or let it join yours), open the page and you get live pin control, an oscilloscope, UART/I2C tools and one-click benchmarks for every major block of the chip: CPU, RAM, flash, crypto accelerators, RTOS, GPIO, UART, I2C, LEDs, ULP / LP core, die temperature and Wi-Fi.

The same source tree builds for every Wi-Fi capable ESP32 variant. What each chip can do (usable pins, ADC channels, temperature sensor, ULP flavour, crypto blocks, ...) is detected from ESP-IDF's SOC capability macros, and the UI and the benchmark list adapt to it. Chip-specific tests simply report "not available on this chip".

---

## Features

- 🔌 **GPIO control** – input / output / PWM per pin, edge counter with debounce, strapping and input-only pins flagged. Pins used by flash, PSRAM, the console, USB and the UART/I2C buses are hidden automatically for the chip you run on.
- 🧲 **PWM** – independent frequency (1 Hz – 40 kHz) and duty per pin, LEDC timers shared between pins of equal frequency.
- 📈 **Oscilloscope** – any ADC1 pin of the chip, hardware-calibrated millivolts when the chip supports it, min/max/avg.
- 🔧 **UART terminal** (selectable baud, RX pushed live) and 🔍 **I2C scanner**.
- 🌡️ **Live telemetry** – die temperature, heap, uptime, CPU clock, Wi-Fi RSSI.
- 🏁 **Benchmark suite** – see below. Results can be exported as JSON/CSV and compared against a saved baseline.
- 📡 AP + station Wi-Fi with automatic reconnect; UI served from SPIFFS.

## Supported chips

Built (and, where noted, run) with ESP-IDF v5.3.1. Capabilities below come from the generated `sdkconfig` of each build.

| Chip | Cores / clock | Wi-Fi | Temp. sensor | ULP / LP | Crypto blocks in HW | Web UI |
|------|---------------|-------|--------------|----------|---------------------|--------|
| ESP32     | 2 × 240 MHz | b/g/n   | –   | ULP-FSM        | AES, SHA, RSA(MPI)                    | ✅ |
| ESP32-S2  | 1 × 240 MHz | b/g/n   | ✅  | ULP-RISC-V     | AES, SHA, RSA, HMAC, DS               | ✅ |
| ESP32-S3  | 2 × 240 MHz | b/g/n   | ✅  | ULP-RISC-V     | AES, SHA, RSA, HMAC, DS               | ✅ |
| ESP32-C3  | 1 × 160 MHz | b/g/n   | ✅  | –              | AES, SHA, RSA, HMAC, DS               | ✅ |
| ESP32-C2  | 1 × 120 MHz | b/g/n   | ✅  | –              | SHA, ECC                              | ✅ |
| ESP32-C6  | 1 × 160 MHz | b/g/n/**ax** | ✅ | LP core (RISC-V) | AES, SHA, RSA, ECC, HMAC, DS       | ✅ |
| ESP32-H2  | 1 × 96 MHz  | none    | ✅  | –              | AES, SHA, RSA, ECC, HMAC, DS          | headless: benchmarks run at boot and print on serial |

Anything ESP-IDF adds later should work unchanged as long as the SOC capability macros are there; only the per-chip files below might need a line:

- `sdkconfig.defaults.<target>` – CPU clock and which ULP flavour to build
- the pin policy in `main/gpio_mgr.c` (`is_reserved()`, `is_strapping()`) and the default bus pins in `main/Kconfig.projbuild`

## Quick start

```bash
# ESP-IDF v5.3.x environment active (. $IDF_PATH/export.sh)
idf.py set-target esp32s3        # or esp32, esp32s2, esp32c3, esp32c2, esp32c6, esp32h2
idf.py menuconfig                # ESP32 Web Debugger -> Wi-Fi: station SSID/password (optional)
idf.py -p /dev/ttyUSB0 flash monitor
```

Then either

- join the **`ESP32-Debugger`** access point (password `12345678`, change it in menuconfig) and browse to `http://192.168.4.1/`, or
- if you configured a station SSID, use the IP printed on the serial console (`web UI: http://<ip>/`).

The SPIFFS image with the web UI is built and flashed together with the firmware; to iterate on `data/index.html` only, run `idf.py flash` again (only changed partitions are written).

Without ESP-IDF installed you can use the official container:

```bash
docker run --rm -v $PWD:/project -w /project -u $(id -u):$(id -g) -e HOME=/tmp espressif/idf:v5.3.1 \
  idf.py set-target esp32c3 build
```

### Configuration (`idf.py menuconfig` → *ESP32 Web Debugger*)

| Option | Default | Meaning |
|--------|---------|---------|
| Enable Wi-Fi and the web UI | on | Off = headless build |
| Station SSID / password | empty | Network to join. Empty = access point only |
| Access point SSID / password / channel / max stations | `ESP32-Debugger` / `12345678` / 1 / 4 | Passwords shorter than 8 characters give an open AP |
| UART1 TX/RX, I2C SDA/SCL GPIO | per chip | Bus pins; these are hidden from the GPIO grid |
| Run all benchmarks at boot | off (on for chips without Wi-Fi) | Prints `BENCH_RESULT {json}` lines on the serial console |
| Plain LED GPIO / WS2812 GPIO | per chip (typical dev-kit pins) | Defaults for the LED benchmark, editable in the UI |

## Benchmarks

Open the **Benchmarks** tab: run a single test, or *Run all*. Every result is a table of named metrics with units; *Set baseline* remembers the current numbers and the next run shows the change per metric; *Export* gives JSON or CSV. Only one benchmark runs at a time, on its own task, and the live views pause meanwhile.

| Benchmark | What it measures | Needs |
|-----------|------------------|-------|
| **CPU** | Integer ALU, mul/div, float32 (FPU or soft-float), float64, 16×16 matrix MAC, bitwise CRC-32, shell sort – on **each core** and on all cores in parallel (scaling); measured vs configured clock; per-MHz scores | – |
| **RAM** | Heap statistics; internal RAM and PSRAM sequential write/read/memcpy/memset bandwidth, random-access latency, pattern + address-in-address integrity test | PSRAM tests only if PSRAM is enabled and present |
| **Flash** | JEDEC id and physical size; raw read, cache-mapped read, 4 KB erase time, page-program speed and read-back verify (on the dedicated `bench` partition only); SPIFFS write/read | – |
| **Crypto accelerators** | SHA-1/256/512, AES-128/256-CBC, AES-128-GCM, RSA-size modular exponentiation, ECDSA P-256 (keygen/sign/verify), hardware RNG; software SHA-256 reference with speed-up factor and digest cross-check; AES known-answer test; which blocks exist / are used by this build; flash-encryption and secure-boot state | – (runs in software where a block is missing) |
| **RTOS & heap** | Context switch, inter-core queue throughput, mutex, malloc/free speed, `esp_timer` jitter | – |
| **GPIO** | Output→input loopback (no wiring), toggle/read rate, interrupt latency in µs, optional sweep of all pins | **You pick a free pin**; it is driven. Reserved pins are refused |
| **UART** | Internal TX→RX loopback at 115200 / 921600 / 3 000 000 baud: integrity, effective rate, divider error | – (no wiring) |
| **I2C** | Bus scan time and responding addresses | – |
| **LEDs** | PWM fade on a plain LED (LEDC) and a colour sequence on a WS2812 RGB LED (RMT) incl. frame-time check | LED pins; no RMT on C2 |
| **ULP / LP coprocessor** | Loads a program into the ULP-FSM / ULP-RISC-V / LP core, verifies it ran, checks its 1 ms timekeeping against the main CPU and (RISC-V/LP) its loop rate | Chips with a ULP/LP core |
| **CPU temperature** | On-die sensor idle, under all-core load, and cool-down; heating rate | Chips with a temperature sensor (not the original ESP32) |
| **Wi-Fi** | Link info (RSSI, channel, protocols, TX power), gateway ping latency/loss, optional TCP upload throughput to a host you run (`iperf -s` or `nc -lk 5001 >/dev/null`), channel scan | Wi-Fi builds; ping needs a station connection |

On the serial console every benchmark also prints one line, handy for CI or scripting:

```
BENCH_RESULT {"id":"ram","name":"RAM","status":"ok","duration_ms":1400,"metrics":[{"n":"Internal RAM memcpy","v":1795,"u":"MB/s"},...],"notes":[]}
```

## HTTP / WebSocket API

| Endpoint | |
|----------|---|
| `GET /` | Web UI |
| `GET /api/info` | Chip identity, firmware, memory, features, crypto blocks, default pins, benchmark list (with availability), ADC pins, Wi-Fi status |
| `GET /api/bench` | Last result of every benchmark that ran since boot |
| `WS /ws` | Control + telemetry channel |

Commands sent to `/ws` (text):

| Command | Effect |
|---------|--------|
| `MODE:<pin>,INPUT\|OUTPUT\|PWM` | Pin mode (PWM starts at 1 kHz / 50 %) |
| `WRITE:<pin>,<0\|1>` | Drive an output |
| `PWM:<pin>,<Hz>[,<duty %>]` | Configure PWM |
| `OSCILLO:<pin>` | Select the ADC1 pin shown by the scope |
| `UART_SEND:<text>`, `UART_BAUD:<n>`, `UART_READ` | UART terminal |
| `I2C_SCAN` | Scan the bus |
| `{"cmd":"bench","id":"cpu"\|"all","params":{...}}` | Start a benchmark (params are per benchmark, e.g. `{"pin":4}` for GPIO) |

Messages pushed by the device are JSON: `{"pins":[...]}`, `{"scope":{...}}`, `{"sys":{...}}`, `{"wifi":{...}}`, `{"uart":"..."}`, `{"i2c":[...]}`, `{"bench":{"event":"start|done|finished|state",...}}`, `{"error":"..."}`. Malformed or oversized commands are answered with an error (or the connection is closed) – they cannot crash the device.

## Flash layout (`partitions.csv`, 2 MB image)

| Partition | Offset | Size | |
|-----------|--------|------|-|
| nvs | 0x9000 | 24 KB | |
| phy_init | 0xF000 | 4 KB | |
| factory (app) | 0x10000 | 1.44 MB | firmware is ~0.9–1.0 MB |
| spiffs | 0x180000 | 256 KB | web UI |
| bench | 0x1C0000 | 256 KB | scratch area for the flash erase/write benchmark – never holds user data |

> **Upgrading from 1.0:** the partition table changed, so flash everything once (`idf.py flash`) rather than OTA-updating the app only. Chips with more than 2 MB of flash work fine; the extra space is simply unused.

## Project layout

```
main/main.c            start-up and wiring
main/caps.c            chip identity + SOC capability detection (/api/info)
main/gpio_mgr.c        per-chip safe pin table, modes, PWM allocation, debounce
main/adc_scope.c       ADC1 sampling with calibration
main/buses.c           UART + I2C (new i2c_master driver)
main/temp_sensor.c     on-die temperature sensor (where present)
main/wifi_mgr.c        AP+STA, events, reconnect
main/web_server.c      HTTP, WebSocket, JSON API
main/services.c        background polling + telemetry
main/bench/            benchmark framework and one file per benchmark
main/ulp/              tiny coprocessor programs for the ULP benchmark (FSM / RISC-V / LP core)
data/index.html        the web UI (single file)
```

**Adding a benchmark:** create `main/bench/bench_foo.c` exporting a `const bench_def_t bench_def_foo` (id, name, group, description, optional `available()` check, `run()` that calls `bench_metric()` / `bench_text()` / `bench_note()` / `bench_fail()`), declare it in `bench.h`, add it to the registry in `bench.c` and to `main/CMakeLists.txt`. The UI picks it up automatically from `/api/info`.

## Building in CI

`.github/workflows/build.yml` builds ESP32, S2, S3, C3, C6 and C2 firmware in the `espressif/idf:v5.3.1` container and produces update/full-flash zips per chip.

## What has and hasn't been verified

- ✅ Compiles warning-free for ESP32, S2, S3, C3, C2, C6 and H2 with ESP-IDF v5.3.1 (all four ULP/LP-core variants included).
- ✅ Executed in the QEMU emulator (ESP32): all benchmarks, the web server, the WebSocket protocol (including malformed input), `/api/*`, SPIFFS serving, and the web UI driven in a real browser.
- ⚠️ In QEMU the GPIO benchmark's output→input loopback reports FAIL (QEMU does not route a pin's output back to its input), the UART loopback and ULP tests also fail there for the same kind of reason, and the SAR-ADC conversion is not emulated (the scope was exercised with the raw read stubbed). These need a real board to judge.
- ⚠️ **Not run on physical boards:** anything that depends on real silicon behaviour – Wi-Fi (scan/ping/throughput/reconnect), the ULP / LP-core programs, the temperature sensor, ADC calibration, WS2812/LED output, UART loopback, I2C on a real bus, PSRAM and real crypto/flash timings. They are written against the documented ESP-IDF APIs and compile for the right chips, but treat the first run on each chip as a test. Please open an issue with the `BENCH_RESULT` output if something looks wrong.

## Notes

- Benchmark kernels are compiled with `-O2` regardless of the project optimisation level (the default for this project is also `-O2`). Numbers from a build with a different optimiser setting, a different CPU clock, or while Wi-Fi is busy are not comparable.
- The Wi-Fi scan moves the radio off-channel; the web page may pause and reconnect for a second or two.
- The GPIO benchmark and the pin controls drive real pins. Do not use pins connected to relays, motors, strapping circuits, etc.

## 📬 License

MIT — use it, remix it, contribute if you want.
