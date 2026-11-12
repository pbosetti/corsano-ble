# corsano-ble

[![CI](https://github.com/pbosetti/corsano-ble/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/pbosetti/corsano-ble/actions/workflows/ci.yml)

A C++20 library for reading Corsano bracelets over BLE, on macOS (CoreBluetooth),
Linux (BlueZ) and Windows 10/11 (WinRT). It is a clean-room reimplementation of what the Android SDK
does at the BLE level. It needs no Corsano SDK and no license key.

- [docs/PROTOCOL.md](docs/PROTOCOL.md) describes the wire protocol.
- [docs/HR_SPO2.md](docs/HR_SPO2.md) lists the ways to get heart rate and SpO2.
- [cli/README.md](cli/README.md) covers the `corsano-cli` CSV streamer, which
  feeds complot, and the `profile` / `calibrate` commands that set the wearer
  profile and run the SpO2 calibration (device fallback: `CORSANO_ADDR`).

## Build

```sh
# Linux only: sudo apt install libdbus-1-dev
# Windows: run from an "x64 Native Tools" prompt (Visual Studio 2022+ with C++)
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
| `include/corsano/protocol.hpp` | pure codec (`corsano::protocol` target, no BLE) |
| `include/corsano/files.hpp` | recorded-file decoders: Activity, Stress, PPG, BioZ, accelerometer, PPG2, ECG ([roadmap](docs/RAW_SIGNALS.md)) |
| `include/corsano/monitor.hpp` | `Corsano::Monitor`: background polling, tidy `Sample` queue, `poll()`/`wait()` |
| `include/corsano/bracelet.hpp` | `Corsano::Bracelet` (`corsano::corsano` target) |
| `include/corsano/corsano.h` | C17 API (`corsano_c` shared lib: `libcorsano_c.dylib` / `.so`) |
| `tests/` | Catch2: `corsano_tests` (offline), `corsano_online_tests`; C17 `corsano_c_tests` |
| `cli/` | `corsano-cli` |
| `python/` | ctypes package `corsano`, `examples/live_csv.py`, unittest suite ([README](python/README.md)) |

Python offline tests: `python3 -m unittest discover -s python/tests`.

## Using the library from a C++ project

### Fetching it with CMake

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_agent LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
FetchContent_Declare(corsano
  GIT_REPOSITORY <git URL of corsano-ble>   # or: SOURCE_DIR /path/to/corsano-ble
  GIT_TAG        main)
FetchContent_MakeAvailable(corsano)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE corsano::corsano)
```

- **Targets:**
  - `corsano::corsano`: everything (`Monitor`, `Bracelet`, decoders).
  - `corsano::protocol`: codec and file decoders only, with no BLE dependency.
  - `corsano::c`: the C shared library.
- **Dependencies:** SimpleBLE v1.4.0 is fetched automatically. On Debian/Ubuntu,
  install `libdbus-1-dev` first.
- **Extras:** tests, CLI and C library are off by default when corsano-ble is a
  subproject, so nothing extra is downloaded or built. Set
  `CORSANO_BUILD_CLI=ON` (etc.) before `FetchContent_MakeAvailable` if you want
  them.
- **Pairing:** the bracelet must be paired with the host first (see
  "Pairing" above). On macOS, the app running your program needs Bluetooth
  permission.

### Monitoring with `Corsano::Monitor`

`Monitor` runs a background thread. The thread keeps the bracelet connected
(reconnecting with backoff), polls and erases its recorded files, and queues
every value as a `Sample`:

| Member | Meaning |
|---|---|
| `time` | Unix epoch seconds, as a `double` |
| `field` | a static string such as `"activity.hr"`, `"activity.spo2"`, `"stress.sc_stress"`, `"bioz.z"`, `"status.connected"` |
| `value` | the value, as a `double` |
| `record` | samples decoded from the same record share it |

Field names are the same as in `corsano-cli follow`; see
[cli/README.md](cli/README.md).

**Set up** once, before your loop:

```cpp
#include <corsano/monitor.hpp>

Corsano::MonitorConfig cfg;              // defaults: Activity, Stress, BioZ
cfg.plan = {Corsano::Plan::Hospital};    // optional: re-applied on every reconnect
cfg.profile = Corsano::UserProfile{      // optional: needed for good SpO2
    std::chrono::year{1971} / 2 / 28, Corsano::Gender::Male,
    Corsano::Wrist::Right, Corsano::SkinType::Olive, 185, 79};
// cfg.host_id = "my-host";  // FW >= 8.41 authenticates automatically on
//                          // connect; the id defaults to this host's name
// cfg.live_hr = true;   // HR every 2 s (workout mode), but stops Activity
//                       // records (SpO2, respiration, steps) while on
// cfg.erase = false;    // keep data on the bracelet (deduplicates instead)

Corsano::Monitor mon("287-2B", cfg);     // advertised name or address
mon.start();                             // returns at once; connects in background
std::vector<Corsano::Sample> batch;
```

**Blocking: publish as soon as data arrives.** `wait()` returns when at least
one sample is queued, when the timeout expires, or when `stop()` is called:

```cpp
while (running) {
  batch.clear();
  mon.wait(batch, std::chrono::seconds(1));
  for (const auto &s : batch)
    publish(s.time, s.field, s.value);
}
```

**Non-blocking: publish at fixed timesteps.** `poll()` never blocks and
returns 0 or more samples:

```cpp
while (running) {                        // e.g. every 100 ms of your own loop
  batch.clear();
  if (mon.poll(batch) > 0)
    for (const auto &s : batch)
      publish(s.time, s.field, s.value);
  sleep_until_next_tick();
}
```

To publish one JSON object per record instead of one per value, group a batch
by `record`. Samples of a record are consecutive and share `time`:

```cpp
// with nlohmann::json in *your* project (corsano-ble has no JSON dependency)
nlohmann::json msg;
for (size_t i = 0; i < batch.size(); ++i) {
  const auto &s = batch[i];
  std::string_view f(s.field);
  auto dot = f.find('.');
  msg["time"] = s.time;
  msg[std::string(f.substr(0, dot))][std::string(f.substr(dot + 1))] = s.value;
  if (i + 1 == batch.size() || batch[i + 1].record != s.record) {
    publish(msg);
    msg.clear();
  }
}
```

**Other calls:**
- `mon.sync_clock()` sets the bracelet clock to the host clock. This also
  happens on every (re)connect.
- `mon.last_error()` returns the last problem the background thread hit; it
  keeps retrying on its own.
- `mon.stop()` (or the destructor) stops the thread, leaves workout mode and
  disconnects.

Link health arrives as ordinary samples: `status.connected` (0/1) and
`status.dropped` (when the queue overflows). The bracelet accepts a single
connection, so don't connect from another app while a `Monitor` runs.
