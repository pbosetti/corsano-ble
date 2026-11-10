# corsano-ble

A C++20 library for reading Corsano bracelets over BLE, on macOS (CoreBluetooth) and
Linux (BlueZ). It is a clean-room reimplementation of what the Android SDK
does at the BLE level. It needs no Corsano SDK and no license key.

- [docs/PROTOCOL.md](docs/PROTOCOL.md) describes the wire protocol.
- [docs/HR_SPO2.md](docs/HR_SPO2.md) lists the ways to get heart rate and SpO2.
- [cli/README.md](cli/README.md) covers the `corsano-cli` CSV streamer, which
  feeds complot, and the `profile` / `calibrate` commands that set the wearer
  profile and run the SpO2 calibration (device fallback: `CORSANO_ADDR`).

## Build

```sh
# Linux only: sudo apt install libdbus-1-dev
cmake -Bbuild -G Ninja
cmake --build build
ctest --test-dir build                      # offline tests
CORSANO_ADDR=<addr or name> ./build/tests/corsano_online_tests   # needs a bracelet (CORSANO_ADDRESS also works)
```

Dependencies are fetched with FetchContent: SimpleBLE v1.4.0 (BUSL-1.1,
free for non-commercial use), Catch2 v3.7.1 and cxxopts v3.2.1.

## Documentation

```sh
cmake -Bbuild -DCORSANO_BUILD_DOCS=ON && cmake --build build --target doc
```

Requires Doxygen (Graphviz optional). Output: `build/doc/html/index.html`.

## Pairing

The bracelet only accepts commands from a bonded host. The passkey is `999999`.

- **macOS:** accept the system PIN dialog on first connect. The terminal app
  also needs Bluetooth permission (System Settings → Privacy & Security →
  Bluetooth).
- **Linux:** pair once with `bluetoothctl`, using `scan on`, then
  `pair <MAC>`, then `trust <MAC>`.

## Layout

| Path | Content |
|---|---|
| `include/corsano/protocol.hpp` | pure codec (`corsano_protocol` target, no BLE) |
| `include/corsano/files.hpp` | recorded-file decoders: Activity, Stress, PPG, BioZ, accelerometer ([roadmap](docs/RAW_SIGNALS.md)) |
| `include/corsano/monitor.hpp` | `Corsano::Monitor`: background polling, tidy `Sample` queue, `poll()`/`wait()` |
| `include/corsano/bracelet.hpp` | `Corsano::Bracelet` (`corsano` target) |
| `include/corsano/corsano.h` | C17 API (`corsano_c` shared lib: `libcorsano_c.dylib` / `.so`) |
| `tests/` | Catch2: `corsano_tests` (offline), `corsano_online_tests`; C17 `corsano_c_tests` |
| `cli/` | `corsano-cli` |
| `python/` | ctypes package `corsano`, `examples/live_csv.py`, unittest suite ([README](python/README.md)) |

Python offline tests: `python3 -m unittest discover -s python/tests`.
