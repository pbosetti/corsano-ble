# corsano-cli

Prints bracelet data as CSV on stdout (header + rows, flushed per row).
Banner, progress and errors go to stderr, so it pipes cleanly into
[complot](https://github.com/pbosetti/complot).

## Build

```sh
cmake -B build -G Ninja && cmake --build build --target corsano-cli
./build/cli/corsano-cli --help
```

## Commands

```sh
corsano-cli scan [-d 5]                       # name,address,rssi
corsano-cli info -a ADDR                      # key,value incl. plan  (-n NAME instead of -a ADDR works everywhere)
corsano-cli live -a ADDR [--set-time] [--duration S]   # LiveHrm every 2 s: t,hr,hr_q,steps,energy,avg_hr,battery
corsano-cli live -a ADDR --realtime [-p 1] [--sampling S]  # RealtimePing: t,device_t,hr,...,spo2,...
corsano-cli history -a ADDR [--file NAME] [--raw PATH] [--max-bytes 38000] [--erase]
corsano-cli follow -a ADDR [--files LIST] [--interval S] [--keep] [--hrm] [--raw-dir DIR] [--plan NAME] [--no-set-time] [--duration S]
corsano-cli plan -a ADDR [--set NAME] [--ppg2-rate HZ]   # key,value: plan, plan_id, ppg2_rate_hz, ppg1_rate
corsano-cli profile -a ADDR [--set] [--save [--force]] [--profile FILE]   # key: value
corsano-cli profile --create [--force] [--profile FILE]   # commented template, no bracelet
corsano-cli calibrate -a ADDR [--profile FILE] [--no-profile] [--timeout S]   # t,progress,movement,quality,finished
corsano-cli raw -a ADDR -x ID[:HEX] [-x ...]   # id,request,response (hex); see "raw" below
```

**Device selection.** `-a ADDR` or `-n NAME` on every command that talks to a
bracelet. If neither is given, the environment variable `CORSANO_ADDR` (an
address or a name) is used, e.g. `export CORSANO_ADDR=287-2B`. The command
fails if none of the three is set.

`live` uses workout mode by default: on FW 8.12 it is the only source of
continuous HR, but each run logs a workout on the bracelet. With `--realtime`,
`NA` is printed for hr/spo2 when their quality is 0; on FW 8.12 only one ping
arrives, ~12 s after connecting. Ctrl-C stops cleanly and leaves the bracelet's
realtime setting as it was.

`history` downloads a file stored on the bracelet. `--file` is one of
`activity` (default), `ppg`, `hrv`, `workout`, `sleep`, `logs`, `ppg2`, `ecg`,
`bioz`, `greenteg`, `stress`, `acc`. `--raw PATH` also saves the downloaded
bytes verbatim. Only `activity`, `ppg` and `stress` have a decoder and print CSV
(`ppg`: `t_ms,chunk,ppg,acc,acc_x,acc_y,acc_z,hr,hr_q`, one row per 25 Hz
sample; `stress`: `time,cognitive_zone,cognitive_zone_q,predicted_zone,predicted_transition,zone_hour,cortisol,sc_stress,sc_stress_q`, raw units); the other files need `--raw PATH`, print nothing on stdout, and refuse
`--erase` since their format is not understood yet. For decoded files
`--erase` removes only the bytes that were decoded.

On macOS the address is the CoreBluetooth UUID shown by `corsano-cli scan`; on
Linux it is the MAC. The bracelet must be paired first (PIN 999999).

## Firmware 8.41+ authentication

Automatic: on firmware 8.41 or later, every command that connects
authenticates first (older firmware is sent nothing). The host id defaults to
this machine's name; `--host-id ID` overrides it, and using the same one across
runs is recommended. `info` prints `host_id,<id>` and `authenticated,<0|1>`. No
licence is involved.

## follow

A thin printer over `Corsano::Monitor` (background thread: reconnects with
backoff, polls files, decodes). Output is a long CSV `time,field,value` (time =
Unix epoch seconds, 3 decimals; integers print without decimals, floats with
2; one value per line, flushed per batch). `status.*` samples (`status.connected`,
`status.dropped`) are printed like any other. Monitor errors are printed to
stderr whenever they change. `-a`/`-n` go straight to the Monitor.

**WARNING: unless `--keep` is given, `follow` ERASES the downloaded records from
the bracelet** (only the bytes that were decoded). With `--keep` nothing is
erased and records are deduplicated by time.

Options:

* `--hrm`: add the workout-mode LiveHrm stream (HR every 2 s). Off by default:
  while workout mode is on the bracelet writes **no Activity records** (no
  SpO2, respiration, steps, temperature). It logs a workout on the bracelet and
  is stopped on exit.
* `--files LIST`: files polled in a loop, default `activity,stress,bioz`.
  Decoded: `activity`, `stress`, `ppg`, `bioz`, `acc`. Others (`ppg2`, `ecg`,
  `hrv`, `workout`, `sleep`, `logs`, `greenteg`) are accepted only with
  `--raw-dir DIR` (and not with `--keep`).
* `--raw-dir DIR`: append every polled file's bytes to `DIR/<name>.bin` (decoded
  files too); nothing extra is printed.
* `--plan NAME`: apply a measurement plan on every (re)connect (see `plan`).
* `--interval S`: minimum seconds per pass; default 0 = back-to-back.
* `--no-set-time`: do not sync the bracelet clock on connect (default: sync).
* `--keep`, `--duration S`; Ctrl-C or SIGTERM stop cleanly.

Fields (time of `hrm.*` is the host clock; the others use the record time):

* `hrm.`: `hr`, `hr_q`, `steps`, `energy`, `avg_hr`, `battery`
* `activity.`: `hr`, `hr_q`, `spo2`, `spo2_q`, `resp`, `resp_q`, `steps`,
  `temp1`, `temp2`, `battery`, `charging`, `wearing`
* `stress.`: `cognitive_zone`, `cognitive_zone_q`, `predicted_zone`,
  `predicted_transition`, `zone_hour`, `cortisol`, `sc_stress`, `sc_stress_q`
* `bioz.z`: bioimpedance samples
* `acc.x`, `acc.y`, `acc.z`: accelerometer samples
* `ppg.`: `ppg`, `acc` (+ `acc_x`, `acc_y`, `acc_z` for protocol v3) per 25 Hz
  sample; `hr`, `hr_q` once per 25 samples
* `status.connected` (0/1 on every change), `status.dropped` (samples lost to a
  full queue)

Quality columns are not turned into `NA` here; filter on the `_q` fields.

## plan

`plan -a ADDR` prints the current measurement plan as `key,value`
(`plan` name, `plan_id`, `ppg2_rate_hz`, `ppg1_rate`). `--set NAME` changes it
first; `--ppg2-rate HZ` (32, 64, 128, 256, 512; default 32) sets the raw PPG2
rate together with it. `info` also prints `plan,<name>`.

| name | id | measures | autonomy |
|------|----|----------|----------|
| `stand-by` | 0 | | |
| `max-battery` | 1 | HR, steps, temperature 1/min | 15 d |
| `typical` | 2 | max-battery + respiration 1/min, SpO2 1/30 min | 7.5 d |
| `hospital` | 3 | HR, respiration, SpO2 1/10 s | 6 d |
| `hz25` | 4 | deprecated | |
| `hospital-raw` | 5 | hospital + continuous raw PPG2/acc @32 Hz | 3.5 d |
| `disabled` | 6 | | |
| `typical-multicolor` | 7 | typical + raw PPG2/acc 2 min every 30 min (B2 only) | 5.5 d |
| `hospital-multicolor` | 8 | hospital, multicolor (B2 only) | 4 d |
| `multicolor-intermittent-raw` | 9 | + continuous raw green PPG2 (B2 only) | 3 d |
| `multicolor-intermittent` | 10 | B2 only | 5 d |
| `continuous-5min` | 11 | B2 only | 6 d |

## profile and calibrate

SpO2 needs the wearer's profile and a one-off calibration (Wearing
Optimization, which tunes the LED power to the wearer). The profile file is
plain `key: value` text, default `./corsano-profile.txt`, or `--profile FILE`:

```
# corsano-profile.txt
birth: 1990-05-17
gender: female          # male | female | unspecified
wrist: left             # left | right
skin: light-brown       # very-fair | fair | olive | light-brown | brown | black-brown
height_cm: 170          # optional (0 = leave unchanged)
weight_kg: 62           # optional
```

`#` starts a comment; blank lines are ignored; skin and the other values are
case-insensitive and accept spaces or underscores (`Light Brown`). `birth`,
`gender`, `wrist` and `skin` are required. An unknown key or bad value is
reported with its line number, before any connection is made.

* `profile` prints the bracelet's profile in the same format. `--set` uploads
  the file first and prints the read-back. `--save` writes the bracelet's
  profile to the file and refuses to overwrite an existing one unless `--force`.
  `--create` writes a commented template (accepted values, date format and
  ranges in `#` comments) without touching the bracelet. Its values are empty,
  and empty values count as absent, so an unedited template can't be uploaded.
* `calibrate` uploads the file (if it exists, unless `--no-profile`), starts
  the calibration and prints `t,progress,movement,quality,finished` every 5 s.
  Sit or lie still, bracelet snug a finger above the wrist bone (2-4 min);
  progress restarts when you move. Exit 0 when finished with quality >= 50,
  2 when finished but failed, 1 on Ctrl-C or `--timeout S` (default 360), after
  asking the bracelet to stop.
* `follow --profile FILE` uploads the profile on every (re)connect. `info` also
  prints `birth`, `gender`, `wrist`, `skin`, `height_cm`, `weight_kg`.

**Privacy:** the profile file holds personal health data (birth date, height,
weight). `corsano-profile.txt` is listed in `.gitignore`; do not commit it.

### SpO2 alert codes

`spo2_q` / `activity.spo2_q` values beyond the 0..4 quality scale:

| code | meaning | remedy |
|------|---------|--------|
| 103 | low confidence | check fit, reduce movement: re-seat the bracelet |
| 104 | motion | keep still |
| 105 | venous pooling | data discarded |
| 106 | skin condition | wear it on the other arm and recalibrate |

## raw

`raw` sends any command and prints the response payload in hex. The
command id and CRC are stripped; on FW 8.12 replies are padded to 4 bytes.
Each `-x ID[:HEX]` is one command, sent in order over one connection. Multi-byte
values are little endian unless noted. Byte layouts are in
[docs/PROTOCOL.md](../docs/PROTOCOL.md). A set command answers `64` (100) for
OK; on FW 8.12, `01` means refused.

```sh
corsano-cli raw -n 287-2B -x 63 -x 50 -x 150 -x 235:04   # FW, battery, plan, SpO2 rate
```

File ids: `01` PPG, `02` Activity, `09` PPG2, `0d` ECG, `0e` BioZ, `15` Stress,
`16` Acc. Vital types: `01` activity, `02` pulse, `03` respiration, `04` SpO2,
`05` temperature, `09` NIBP. Recording values: `01` off, `02` bedtime→risetime,
`03` continuous.

### Device and time

| `-x` | Command | Reply / notes |
|---|---|---|
| `63` | firmware version | `?? ?? MA MI` → MA.MI (`0000080c` = 8.12; MI ≥ 100 is divided by 10) |
| `50` | battery | `%` `mV:u16` `state`, charging if 1, 3, 11 (`517d0f03` = 81 %, 3965 mV, charging) |
| `70` | serial number | ASCII |
| `156` | bracelet status | wearing, mode, battery %, charging, idle, -, wearing quality |
| `152` | session status | mode, session, start:u32, duration:u16, status (2 finished) |
| `51` | read clock | `u32` epoch seconds |
| `22:…` | set clock | 8 bytes (UTC fields + offset); easier: `live --set-time`, or `follow`/Monitor, which do it on connect |

### User profile (see `profile`)

| `-x` | Command | Reply / notes |
|---|---|---|
| `57` | get profile | age, gender (0 m, 1 f, 2 unspecified), wrist (0 l, 1 r), skin 1–6 |
| `56` | get birth date | `year:u16` month day |
| `54` / `55` | get height cm / weight kg | `u8` |
| `42:b9` / `43:4f` | set height 185 / weight 79 | status |
| `44:…` | set profile | age, year **high byte first**, month, day, gender, wrist, skin: use `profile --set` |
| `52` / `53` | get bedtime / risetime | `hh mm`, **GMT** |
| `40:141e` / `41:051e` | set bedtime 20:30 / risetime 05:30 GMT | status; the sleep window is bedtime −2 h … risetime +3 h |

### Plan, sampling, recording (see `plan`)

| `-x` | Command | Reply / notes |
|---|---|---|
| `150` | get plan | plan, PPG2 rate, ppg1 rate (`07013c` = typical-multicolor, 32 Hz, 60) |
| `149:03013c` | set plan Hospital | status |
| `235:04` | get SpO2 sampling | `04` `seconds:u16` (`043c00` = 60 s) |
| `234:043c00` | set SpO2 sampling 60 s | status; 30 s gives empty records |
| `193:01` / `193:02` / `193:04` | get stress+BioZ / acc / temp-raw recording | type, value(s) |
| `192:010303` | stress + BioZ continuous | status; `192:010101` = off |
| `192:0203` | accelerometer continuous | status |

### Live data

| `-x` | Command | Reply / notes |
|---|---|---|
| `221` / `220:01` | get / set realtime ping enable | FW 8.12: one ping per connection |
| `224` | get realtime period | `u8` seconds (25 on FW 8.12) |
| `223:05` | set realtime period | FW 8.12 answers `01` (refused) |
| `16` / `155` | workout start / stop | LiveHrm every 2 s while on; no Activity records |

### Files (see `history`, `follow`)

| `-x` | Command | Reply / notes |
|---|---|---|
| `46:02` | Activity file size | FW 8.12: `02` `size:u32`; FW 7.x: `size:u32` |
| `45:0226000000` | erase the first 38 bytes of Activity | `file` `bytes:u32`; prefer `history --erase` |

### Calibration and authentication

| `-x` | Command | Reply / notes |
|---|---|---|
| `216:04` / `217:04` | Wearing Optimization start / stop | status; prefer `calibrate` |
| `246:04000000` | Wearing Optimization status | `04 ? finished movement progress% quality` (≥ 50 passes) |
| `251:00` | auth status (FW ≥ 8.41) | `64` = authenticated; done automatically, see above |

### Careful

| `-x` | Command | Effect |
|---|---|---|
| `21` | erase all files | all recorded data lost |
| `24` | reset bracelet | resend time, profile and plan afterwards |
| `111` | shut down | stops measuring and advertising until put on the charger |

## complot

See [complot](https://github.com/pbosetti/complot) for live plotting. Example:

```sh
corsano-cli live -a ADDR | complot hr --aes "x=t y=hr,spo2" -

complot hr --aes "x=t y=hr,spo2" --exec "corsano-cli live -a ADDR"

corsano-cli follow -n 287-2B | complot vitals --aes "x=time y=value g=field" -

corsano-cli follow -n 287-2B | grep -E 'hrm.hr,|activity.spo2,' | complot vitals --aes "x=time y=value g=field" -
```
