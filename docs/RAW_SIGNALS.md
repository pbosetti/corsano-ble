# Raw signals: status and next steps

Goal: decode the raw recordings stored on the bracelet, in this order of
priority: PPG, ECG, bio-impedance, accelerometer, PPG2. All of them already
download as raw bytes with `Bracelet::download_file()` and
`corsano-cli history --file <name> --raw out.bin`.

## Where the formats are documented

| Signal | FileId | Format known? | Source |
|---|---|---|---|
| PPG | 1 | **yes** | decoder in the BLE SDK (`PpgFileTransferRequest`); spec in [PROTOCOL.md](PROTOCOL.md#ppg-file) |
| ECG | 13 | no | |
| BioZ | 14 | no | |
| Accelerometer | 22 | no | |
| PPG2 | 9 | no | |

For the four unknown formats, the BLE SDK returns the bytes unparsed. The
`data` SDK (`com.corsano.sdk:data`, also decompiled) doesn't decode them
either. It saves them as `.wiff` chunk files, merges them and hands them on,
presumably to the Corsano cloud (`api.health.cloud.corsano.com`). Its only
native code, `libfxc.so`, is Philips' sleep-metrics engine, not a WIFF
parser. **Nothing on the phone side knows these formats.**

## How to get the four unknown formats

**1. Ask Corsano first.** You hold an SDK license. Ask for the `.wiff`
specification, or at least for the ECG, BioZ, accelerometer and PPG2 file
layouts. It is the cheapest route and the only one that gives authoritative
scaling (µV, Ω, g).

**2. Otherwise, reverse engineer from recordings with known stimuli.** The
recipe is the same for every signal:

1. Start a recording (commands below), apply a known stimulus, then dump the
   file with `corsano-cli history --file <name> --raw <name>.bin`. Repeat
   2–3 times with different durations.
2. Look for framing. PPG1 uses a sync marker, a version byte, a timestamp and
   a trailing CRC-32, so try the same first: search for repeated byte
   patterns, then test whether CRC-32 or CRC-8 matches at candidate chunk
   ends. The chunk size is the distance between markers.
3. Find the header fields. Look for a u32 epoch timestamp close to the
   recording time (it is set by `set_time()`), a monotonic chunk index, and
   a sample count.
4. Find the sample width and channels. Try int16 or int24, little endian,
   with 1–3 interleaved channels, and plot each guess. The right one shows
   the stimulus waveform. The sample rate is samples per chunk divided by
   the timestamp difference between chunks.
5. Fix the scaling against a reference: a clinical ECG, a known resistor
   load for BioZ, gravity (1 g) for the accelerometer, or a pulse oximeter
   for PPG2.
6. Write the decoder following the pattern below, plus a test built from a
   real dump checked into `tests/data/`.

| Signal | Start recording | Stimulus for step 1 |
|---|---|---|
| ECG | cmd 34 `[2]` start, cmd 35 `[2]` stop (`APP_CMD_START/STOP_PPG2_ECG_MEASUREMENT`, measurement ECG = 2). The user touches the electrode. | 30 s finger on the electrode at rest. Look for QRS spikes at the known HR, cross-checked with the PPG/realtime HR. |
| BioZ | cmd 192 `[1, emography, bioz]`, values 1 off, 2 bedtime→risetime, 3 continuous | Continuous for 10 min, still vs. deep breathing. Respiration modulates impedance. |
| Accelerometer | cmd 192 `[2, value]`, same values | Bracelet lying flat, then on each edge (±1 g per axis), then shaken at a known rhythm. |
| PPG2 | cmd 149 `SetPlan [plan, ppg2_freq, ppg1_rate]` with a raw plan, e.g. 5 "Hospital Raw" or 9 "Continuous Green Intermittent Multicolor Raw"; `ppg2_freq` 1..5 = 32/64/128/256/512 Hz. Also measurement ids 1/4/5/8 (PPG, PPG+ECG, …) for cmd 34. | Same session as a PPG1 recording: the green channel must correlate with PPG1. Red and IR differ under breath-hold (SpO2 drop). |

Cautions:
- Raw plans drain the battery and fill the flash in hours. Restore the plan
  the bracelet had before (`GetPlan`, cmd 150) when you're done.
- Never `--erase` a file until its decoder is trusted.

## Decoder pattern (applies to every new signal)

Every decodable file gets the same five pieces:

1. **Spec**: a section in [PROTOCOL.md](PROTOCOL.md) with byte offsets,
   framing and scaling.
2. **C++** in `include/corsano/files.hpp` and `src/files.cpp`:
   - `struct <Name>Chunk` (or `Record`) holding decoded values in
     natural units.
   - `Decoded<<Name>Chunk> decode_<name>(std::span<const uint8_t>)`, which
     never throws on corrupt data, resyncs, stops before an incomplete chunk
     and returns `consumed_bytes`, which is safe to pass to `erase_file()`.
   - Framing goes through one of the shared splitters in `src/files.cpp`:
     fixed-size records with a CRC-8 (Activity), or a sync marker with a
     CRC-32 (PPG). Add a new splitter only for a genuinely new framing.
3. **Tests**: offline Catch2 cases built from the spec, in
   `tests/test_files.cpp`.
4. **C/Python**: `corsano_<name>_sample_t` (one flat row per sample, with an
   absolute timestamp) plus `corsano_decode_<name>()`, with a matching
   ctypes `Structure` and a `decode_<name>()` function. The flat form maps
   directly onto CSV and numpy.
5. **CLI**: a column set for `corsano-cli history --file <name>`.
