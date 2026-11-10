# corsano (Python)

ctypes wrapper for `libcorsano_c`. Stdlib only, Python >= 3.9.

Build the library first (`cmake -Bbuild -GNinja && cmake --build build`), or
point `CORSANO_LIB` at it. Search order: `$CORSANO_LIB`, the package dir,
`../../build`.

```python
import corsano

with corsano.Bracelet(ADDR) as b:   # connects; closes on exit
    b.on_realtime(lambda p: print(p.heart_rate, p.spo2))
    b.set_realtime_period(1)
    b.set_realtime_enabled(True)
    time.sleep(30)
    b.set_realtime_enabled(False)
```

Download and decode a raw PPG file (25 Hz samples; Activity and Stress work alike via `decode_activity` / `decode_stress`):

```python
data = b.download_file(corsano.FileId.PPG, 481 * 100)
samples, used = corsano.decode_ppg(data)
print(samples[0].timestamp_ms, samples[0].ppg, samples[0].heart_rate)
b.erase_file(corsano.FileId.PPG, used)   # only what was decoded
```

Plans: `b.plan` returns `PlanSettings(plan, ppg2_rate, ppg1_rate)`;
`b.set_plan(corsano.Plan.HOSPITAL, corsano.Ppg2Rate.HZ32)` changes it.

Profile and SpO2 calibration: `b.user_profile` returns a `UserProfile(birth,
gender, wrist, skin, height_cm, weight_kg)`; `b.set_user_profile(p)` sets it.

```python
b.set_user_profile(corsano.UserProfile(
    datetime.date(1990, 5, 17), corsano.Gender.FEMALE, corsano.Wrist.LEFT,
    corsano.SkinType.LIGHT_BROWN, height_cm=170, weight_kg=62))
b.start_wearing_optimization()         # sit still; poll until finished
st = b.wearing_optimization_status()   # finished, progress_percent, quality, success
b.stop_wearing_optimization()
```

`Monitor(..., profile=p)` uploads it on every connect. `activity.spo2_q`
alerts: `SPO2_LOW_CONFIDENCE` (103), `SPO2_MOTION` (104),
`SPO2_VENOUS_POOLING` (105), `SPO2_SKIN_CONDITION` (106).

Monitor: a background thread keeps the bracelet connected, polls and erases
its files and queues tidy `Sample(time, field, value, record)` tuples:

```python
with corsano.Monitor(ADDR, plan=corsano.Plan.HOSPITAL) as m:  # starts on enter
    for s in m:                          # blocks; ends after m.stop()
        print(json.dumps(s._asdict()))
```

or at a fixed timestep:

```python
m = corsano.Monitor(ADDR, files=[corsano.FileId.ACTIVITY])
m.start()
while True:
    time.sleep(10)
    for s in m.poll():                   # non-blocking, up to 4096 samples
        print(s.time, s.field, s.value)
    if m.last_error:
        print("monitor:", m.last_error)
```

`status.connected` / `status.dropped` samples report connection changes and
queue overflow; field names match `corsano-cli follow`.

CSV for plotting:

    python examples/live_csv.py ADDR | complot hr --aes "x=t y=hr,avg_hr" -   # --realtime for pings

Command names and readable replies (for raw `b.command(id, payload)` use):

```python
corsano.find_command("get-plan")                    # CommandInfo(id=150, ...)
corsano.describe_reply(150, bytes.fromhex("07013c00"))
# 'plan=typical-multicolor ppg2=32Hz ppg1=60'; "" if unknown
[c.name for c in corsano.command_catalog()]
```

Caveats: callbacks run on the BLE backend thread (keep them short, guard
shared state); exceptions in them are printed to stderr, not propagated.
Methods on a closed `Bracelet` raise `ArgumentError`.
Tests: `python3 -I -m unittest discover -s python/tests -v`.

## Firmware 8.41+ authentication

Automatic on connect. The host id defaults to this host's name; set it with
`Bracelet(addr, host_id="...")` or `Monitor(addr, host_id="...")` and keep it
the same across runs. `bracelet.authenticated` reports the state.
