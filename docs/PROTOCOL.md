# Corsano bracelet BLE protocol

Reverse engineered from the Android SDK `com.corsano.sdk:ble:1.1.29` (public
on Maven Central, decompiled with jadx). Not yet verified on hardware unless
marked otherwise.

## GATT

| Role | UUID | Properties |
|---|---|---|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca3e` | advertised; scan filter |
| RX (requests) | `6e400002-…` | write **with response** |
| TX (file stream) | `6e400003-…` | notify |
| DATA_INFO (responses + pings) | `6e400004-…` | notify |

The SDK subscribes to TX first, then DATA_INFO. It does not negotiate MTU. The
link must be bonded, with passkey `999999`, before commands are accepted.

## Framing

All multi-byte integers are **little endian**.

- Request (RX): `[id:u8][payload…]`. There is no CRC and no length field.
- Response / notification (DATA_INFO): `[id:u8][payload…][crc8:u8]`.
- `crc8`: poly `0x07`, init `0xFF`, no reflection, no xorout, computed over every
  byte before the CRC. Check value for `"123456789"`: `0xFB`. Frames shorter
  than 2 bytes skip the check.
- "Set" commands answer with one status byte: `100` = OK, `101` = fail
  (FW 8.12 also uses `1` for a rejected set).
- FW 8.12 pads every response payload to 4 bytes. Decoders read only the
  prefix they need.

Only one command may be in flight. A DATA_INFO frame whose `id` equals the
pending command's id is that command's response; anything else is an
unsolicited notification. Exception: if id `110` (bonding state) arrives while
a command is pending and its payload byte is `101`, the command fails with
"pairing required". Response timeout is 15 s.

## Commands

`←` is the response payload, after the id and before the CRC.

| Id | Name | Request payload | Response |
|---|---|---|---|
| 16 | SetWorkoutMode (start) | – | status |
| 21 | EraseAllFiles | – | status |
| 22 | UpdateTime | see below | status |
| 42 | SetHeight | `cm:u8` | status |
| 43 | SetWeight | `kg:u8` | status |
| 44 | SetProfile | `age:u8, year_hi:u8, year_lo:u8, month, day, gender, wrist, skin` | status. Age in full years at the time of the call; the year is big endian, unlike everything else. Example (1971-02-28, male, right wrist, olive skin, set on 2026-11-10): `37 07 B3 02 1C 00 01 03` |
| 45 | EraseFromFile | `file:u8, bytes:u32` | status |
| 46 | GetFileSize | `file:u8` | `size:u32` (FW 7.x); `file:u8, size:u32` (FW 8.12) |
| 48 | StartStreamingFileWithSize | `file:u8, size:u32` | none, data arrives on TX |
| 50 | GetBatteryLevel | – | `percent:u8, mV:u16, state:u8` charging if state ∈ {1,3,11} |
| 54 | GetHeight | – | `cm:u8` (may be padded) |
| 55 | GetWeight | – | `kg:u8` (may be padded) |
| 56 | GetBirthdate | – | `year:u16 LE, month, day` (`B3 07 02 1C` = 1971-02-28) |
| 57 | GetProfile | – | `age, gender, wrist, skin` (`37 00 01 03`; may be padded) |
| 63 | GetFwVersion | – | 4 bytes; major = b[2], minor = b[3] (if minor ≥ 100, minor /= 10) |
| 70 | GetSerialNumber | – | ASCII, cut at first NUL |
| 110 | GetBondingState | – | `100` bonded / `101` not bonded |
| 149 | SetPlan | `plan:u8, ppg2_rate:u8, ppg1_rate:u8` | status |
| 150 | GetPlan | – | `plan:u8, ppg2_rate:u8, ppg1_rate:u8` (e.g. `07 01 3c 00`: plan 7 = TypicalMulticolor, 32 Hz, 60; trailing byte is the CRC) |
| 155 | WorkoutStop | – | workout stats, ignored |
| 216 | StartSpecialMode | `1` = spot measurement, `4` = Wearing Optimization | status |
| 217 | StopSpecialMode | `4` | status |
| 246 | GetSpecialMode | mode as `u32` LE (`04 00 00 00`) | mode 4: `4, ?, finished, movement, progress %, quality` |
| 220 | SetRealtimeEnable | `0/1:u8` | status |
| 221 | GetRealtimeEnable | – | `u8`, enabled if `== 1` |
| 223 | SetRealtimePeriod | `seconds:u8` | status. **FW 8.12 answers `1` and keeps its period (25 s)**: rejected, payload format unknown. The SDK sample app ignores the result. |
| 224 | GetRealtimePeriod | – | `seconds:u8` |
| 234 | SetVitalsSamplingRate | `type:u8, seconds:u16` | status |
| 235 | GetVitalsSamplingRate | `type:u8` | `type:u8, seconds:u16` |

`Plan` values and `ppg2_rate` (1 = 32 Hz ... 5 = 512 Hz): see `Plan` and
`Ppg2Rate` in `protocol.hpp`. `ppg1_rate` is 60 in the SDK; meaning unknown.

`VitalType`: 1 activity, 2 pulse rate, 3 respiration rate, 4 SpO2,
5 temperature, 9 NIBP. SDK presets for the interval: 0 (off), 30, 60, 300,
1800 s.

Profile enums: gender 0 male, 1 female, 2 unspecified; wrist 0 left, 1 right;
skin 1 very fair, 2 fair, 3 olive, 4 light brown, 5 brown, 6 black brown.

Wearing Optimization (special mode 4) tunes the LED power to the wearer.
GetSpecialMode(4) examples: `04 01 00 01 53 32` running (movement 1, 83 %,
quality 50); `04 00 01 00 64 32` finished (`finished` is byte 2 `== 1`,
100 %, quality 50; quality >= 50 passes). Progress restarts on movement.

### SpO2 quality codes

`spo2_q` / `spo2_quality` use 0 (bad) .. 4 (very good); larger values are
alerts: 103 low confidence (fit, movement), 104 motion (keep still),
105 venous pooling (data discarded), 106 skin condition (other arm,
recalibrate). Source: developer.corsano.com, Android Data Models, Quality.

UpdateTime payload (8 bytes): UTC `year-2000, month(1-12), day, hour, min,
sec` as u8 each, then the host's local UTC offset in minutes as an int16.

## Unsolicited notifications (DATA_INFO)

The offsets below count from the first payload byte, i.e. after the id.

**125 RegularPing** (periodic): `skip:u8, battery:u8, ppg:u16, activity:u16,
hrv:u16, workout:u16, sleep:u16, logs:u16`. The u16 fields are bytes pending
in each file.

**222 RealtimePing** (when enabled with 220/223): `timestamp_s:u32,
hr:u8, hr_q:u8, resp:u8 (/4 → breaths/min), resp_q:u8, spo2:u8 (%),
spo2_q:u8, core_temp:u16 (/100 → °C), movement:u8, battery:u8`.

**160 LiveHrm** (expected in workout mode): `hr:u8, hr_q:u8, steps:u16,
energy:u16, max_hr:u8, speed:u8, skip:u8, duration:u16, battery:u8,
avg_hr:u8, avg_speed:u8`.

**50 Charging event**: same layout as the GetBatteryLevel response. It counts
as a notification only when no command 50 is pending.

Quality fields go from 0 (bad) to 4 (very good).

## File download

1. `46 [file]` → `available:u32`.
2. `n = min(available, requested)`, rounded **down** to a multiple of the
   record size (no rounding if the size is 0). Stop if `n == 0`.
3. Write `48 [file, n:u32]`. When the write completes, collect TX notifications
   until `n` bytes have arrived. Fail after 15 s with no data.
4. Optionally `45 [file, consumed:u32]` to free the bytes on the bracelet. The
   SDK's data layer does this after decoding.

File ids and record sizes: PPG 1/256 (transfer unit; v3 chunks are 481 B and can straddle two downloads, so download, decode, erase `consumed`, repeat), Activity 2/38, HRV 3/7, Workout 4/40,
Sleep 5/0, Logs 6/0, PPG2 9/0, ECG 13/0, BioZ 14/0, Greenteg 18/15,
Stress 21/18, Acc 22/0 (0 = variable).

### Record splitting (fixed-size files)

Every record ends with its own CRC-8, computed over the preceding
`size-1` bytes. Walk the buffer:

```
locked = false
while remaining >= size:
    ok = crc_ok(record at +0)
    if ok and not locked:          # need up to 3 consecutive good records
        if remaining >= 2*size: ok = ok and crc_ok(+size)
        if remaining >= 3*size: ok = ok and crc_ok(+2*size)
    locked = ok
    if ok: decode record, advance size
    else:  advance 1 byte
consumed = position at end
```

### Activity record (38 bytes)

| Off | Field | Type |
|---|---|---|
| 0 | timestamp_s | u32 |
| 4 | hr, hr_q | u8, u8 |
| 6 | steps | u16 |
| 8 | activity_type, activity_count | u8, u8 |
| 10 | spo2 (%) | u8 |
| 11 | energy | u16 |
| 13 | resp (/4), resp_q | u8, u8 |
| 15 | battery: abs(value) = %, negative = charging | i8 |
| 16 | nibp_systolic, nibp_diastolic | u8, u8 |
| 18 | spo2_q | u8 |
| 19 | (reserved ×2), cardiac_arrest, (reserved ×2) | u8 ×5 |
| 24 | reserved | 7 bytes |
| 31 | temp1, temp2 (/100 → °C) | i16, i16 |
| 35 | reserved | u8 |
| 36 | wearing | u8 |
| 37 | crc8 | u8 |

### Stress record (18 bytes)

`FileId::Stress` (21), called "emography" in the SDK
(`EmographyFileTransferRequest`). It uses the fixed-size CRC-8 framing above.
Units are raw; the SDK does not scale them.

| Off | Field | Type |
|---|---|---|
| 0 | timestamp_s | u32 |
| 4 | cognitive_zone | u8 |
| 5 | predicted_cognitive_zone | u8 |
| 6 | predicted_transition_time | u16 |
| 8 | cognitive_zone_hour | u8 |
| 9 | cortisol_contribution | u32 |
| 13 | cognitive_zone_quality | u8 |
| 14 | skin_conductance_stress | u16 |
| 16 | skin_conductance_stress_quality | u8 |
| 17 | crc8 | u8 |

## PPG file

`FileId::Ppg` (1). Raw optical signal at 25 Hz, with per-sample motion and
three on-device HR estimates per chunk. Source: `PpgFileTransferRequest`
and splitter `g0` in the SDK.

### Chunk framing

A chunk starts with the 6-byte sync marker `00 FF AA 4D 4D 54`. The byte that
follows is the protocol version, which fixes the chunk size:

| Version | Chunk size | Sample size |
|---|---|---|
| 2 | 256 B | 3 B |
| 3 | 481 B | 6 B |

The last 4 bytes of a chunk are a **CRC-32** (IEEE/zlib: reflected poly
`0xEDB88320`, init `0xFFFFFFFF`, xorout `0xFFFFFFFF`, check value
`0xCBF43926` for `"123456789"`) over the preceding `size-4` bytes, stored
little endian.

Splitting:

```
pos = 0
loop:
    if remaining < 7: stop
    if data[pos:pos+6] != marker or version unknown: pos += 1; continue
    if remaining < size(version): stop        # incomplete: do not consume
    if crc32(data[pos : pos+size-4]) != u32 at pos+size-4: pos += 1; continue
    decode chunk; pos += size
consumed = pos
```

This differs from the SDK in one place: before its first good chunk, the SDK
can step past an incomplete trailing chunk. Stopping there keeps
`consumed` safe for `erase_file`.

### Chunk layout

| Off | Field | Type |
|---|---|---|
| 0 | sync marker | 6 B |
| 6 | protocol version | u8 |
| 7 | reserved | 2 B |
| 9 | n_samples (nominal 75) | u16 |
| 11 | chunk index | u32 |
| 15 | timestamp, seconds | u32 |
| 19 | timestamp, milliseconds | u16 → `timestamp_ms = s*1000 + ms` |
| 21 | hr0, hr0_q, hr1, hr1_q, hr2, hr2_q | u8 ×6 |
| 27 | samples | `min(n_samples, (size-31)/sample_size)` × sample |
| size-4 | crc32 | u32 |

Sample:
- v2: `ppg:u16, acc:u8`
- v3: `ppg:u16, acc:u8, x:i8, y:i8, z:i8`; `acc_x/y/z = value*4`

`acc` expansion (8-bit compressed magnitude):
- `v >= 255` → 617
- `v >= 150` → `(v-150)*4 + 200`
- `v >= 100` → `(v-100)*2 + 100`
- otherwise `v`

Sample `i` is timestamped `timestamp_ms + 40*i`. The SDK tries to derive the
period from the next chunk's timestamp, but its condition (`d > 3007 &&
d < 2993`) can never be true, so the period is always 40 ms. Its HR is
`hr[min(i/25, 2)]`.

## Newer SDK (2.3.0) and firmware 8.x

The commands above come from SDK 1.1.29, which targets FW 7.97.
`com.corsano.sdk:ble:2.3.0` adds the following.

**Host authentication (FW ≥ 8.41 only, per the SDK constant
`FIRMWARE_VERSION_CMD_AUTHENTICATE_VERSION_*`).** Unverified: the test
bracelet runs 8.12.

| Id | Name | Request | Response |
|---|---|---|---|
| 250 | Authenticate | `0xA7` (AUTH_KEY 167), `0x16` (model id 22 = Android), `phone_id:8` | status |
| 251 | GetAuthStatus | `0` | `100` = authenticated |

`phone_id`: a 16-hex-char string is used as its 8 bytes; any other string is
replaced by `SHA-256(utf8)[0:8]`. The SDK licence key is **not** sent to the
bracelet. The data SDK posts it to `api.health.cloud.corsano.com`, which
returns `is_authorised`, the licence dates and a `raw_data_id_list`. That is
an app-side gate only.

**Initialization sequence.** SDK 2.x runs this on connect: FW version →
auth status → authenticate → set time → set plan → sampling rates (activity,
pulse rate, respiration, temperature, NIBP) → accelerometer plan →
BioZ/emography plan. It marks `SetVitalsSamplingRate` as "sent by the
initialization sequence, do not send directly". The Corsano app may
therefore overwrite sampling rates set by this library.

**FW 8.12 observations** (test bracelet "287-2B"):
- RealtimePing (222): sent once, ~12 s after connecting, then never again
  (90 s runs, with and without re-sending enable 220). `spo2_q` read 106 in
  one ping and 0 in another while `spo2` was 0, so its meaning is unclear.
- LiveHrm (160) in workout mode (16): every 2 s, layout as documented
  (`duration` +2 per frame). WorkoutStop (155) returns 15 bytes of stats.
- StartStreamingFileFromAddressWithSize (68) `[file, a:u32, b:u32]` streams
  from the **start** of the file for `a` bytes, like 48. No tail reads.
- GetFileSize (46) echoes the file id first (see the table).
- Activity file: 300 s records when not worn, 60 s when worn.
- SetRealtimePeriod (223) answers `1` for every tried payload (`05`, `1e`,
  `0500`, `3c00`) and the period stays at 25 s. SDK 2.3.0 sends the same
  `u8` payload, so the cause is unknown.
- GetRealtimePeriod (224) returns `19 xx 00 00`. The second byte changed
  from `00` to `0b` between sessions; its meaning is unknown.
- GetPlan (150): `07 01 3c` = plan 7 "Typical multicolor", PPG2 32 Hz,
  PPG1 rate 60.
- GetSessionStatus (152): 9 bytes, `mode, session, start:u32,
  duration:u16, status` (0 not started, 1 ongoing, 2 finished).
- GetBraceletStatus (156): `wearing, mode, battery %, charging state,
  idle, (skip), wearing quality`.

## OHR files (BioZ, accelerometer, PPG2)

Files `Bioz` (14), `Acc` (22) and `Ppg2` (9) share one framing. Documented by
Corsano (developer.corsano.com, android/data_models/raw_files); layouts below
verified on real recordings (FW 8.x, B2). Little endian, no CRC.

Record: `"OHR"` (4F 48 52), `len:u16`, `id:u8`, payload of `len-1` bytes;
total size `5 + len`.

| Id | Meaning |
|---|---|
| 0x0A | header: time/size; payload offset 12..15 = `u32` start time (epoch s) |
| 0x0B | version |
| 0x0C | host version |
| 0x0F | body: one channel chunk |

Channel chunk (payload of a body record):
`channel:u8, clen:u16, index:u8, quality:u8`, then for data channels
`body_position:u8, format:u8, samples (clen-4 bytes)`. Time channel 0x1E has
no body_position/format: `1E, 06 00, index, quality, timestamp:u32` (epoch s).

| Channel | Signal | Format | Samples |
|---|---|---|---|
| 0x3D | BioZ | 0x01 = 25 Hz | 24-bit unsigned, `(clen-4)/3` (25 per chunk) |
| 0x2B | Acc | 0x6E = 32 Hz | int16 x,y,z, `(clen-4)/6` (32 per chunk) |

Deviations from Corsano's doc:
- BioZ is channel **0x3D** on FW 8.x, the doc says 0x3E (the decoder accepts both).
- The time channel 0x1E is undocumented.
- Acc scale is about **512 counts/g** at rest (z about -514); unverified.

Timestamps: each chunk is 1 s. On FW 8.12 every data chunk is **followed** by
its 0x1E record (`chunk, time, chunk, time, …`), so a 0x1E timestamp belongs to
the chunk(s) **before** it. If several chunks precede one time record, they are
1 s apart, ending at that time. Chunks after the last time record are
extrapolated from it (or from the 0x0A start time); with no time info at all
they get 0. This matters after an erase: each poll's download starts with a
data chunk. Chunks of all `0xFF` are erased flash and are skipped.

Splitting: scan for `"OHR"`; `len == 0` or `len > 4096` resyncs by 1 byte; a
record that is not complete yet stops the scan (not consumed). A chunk whose
`clen` overruns its record is skipped but the record is consumed.
