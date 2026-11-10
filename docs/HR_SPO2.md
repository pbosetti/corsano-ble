# Reading heart rate and SpO2

> **Measured on FW 8.12 (bracelet "287-2B", 2026-11-10).**
> - **Profile and calibration matter.** A placeholder profile (invalid wrist
>   2, wrong skin type) produced only 103/106 alerts instead of SpO2. After
>   setting the real profile (`set_user_profile`) and re-running Wearing
>   Optimization (quality 50) the 106 alerts stopped and SpO2 97 % appeared.
>   Set the profile, calibrate, keep still; night wear gives the most SpO2.
> - **Workout mode suspends Activity logging.** While it is on, no Activity
>   records are written, so no SpO2, respiration, steps or temperature
>   (verified in a 15-min session). Choose: HR every 2 s (workout) **or**
>   HR + SpO2 + resp + steps + temp once a minute (Activity, the default for
>   `follow` and `Monitor`).
> - **Live HR: use workout mode (route 2).** `LiveHrm` arrives every 2 s.
>   `corsano-cli live -a <ADDR>`. Each run logs a workout on the
>   bracelet.
> - **Realtime ping (route 1): one ping ~12 s after connecting, then
>   nothing.** The period can't be changed (cmd 223 answers `1`).
> - **Activity history (route 4): works.** One record per minute while worn
>   (every 5 min otherwise), HR in each worn record.
> - **SpO2: rare but real.** None in 1235 Activity records over 4 days (SpO2
>   sampling 1800 s). After sampling was set to 60 s, one record had SpO2 97 %
>   (quality 1) with respiration 15.75/min (12:30, at rest). Setting 30 s
>   only interleaves empty records (HR 0, temp 0) and yields no SpO2, so use
>   60 s or more. Cmd 216 `[1]` (special mode) is accepted but produced
>   nothing in 2 min. SpO2 needs stillness; expect few values while awake.

The firmware computes HR and SpO2 itself from the optical (PPG) sensor. The
host only has to collect the values. Four routes exist, listed from most to
least useful for live plotting. Byte layouts are in [PROTOCOL.md](PROTOCOL.md).
None of these routes has been checked on hardware yet.

## 1. Realtime ping: live HR + SpO2 (recommended)

The bracelet pushes a `RealtimePing` (id 222) every *period* seconds. It
carries `heart_rate`, `spo2`, a 0–4 quality value for each, respiration rate,
core body temperature, movement and battery.

```cpp
b.set_notification_callback([](const Corsano::Notification &n) {
  if (auto *p = std::get_if<Corsano::RealtimePing>(&n))
    use(p->heart_rate, p->heart_rate_quality, p->spo2, p->spo2_quality);
});
b.set_realtime_period(std::chrono::seconds(1)); // 1..255 s
b.set_realtime_enabled(true);
```

- **Ping rate vs. measurement rate.** The ping repeats the latest values at
  the period you set. How often the values themselves are *re-measured*
  depends on the vital sampling rate (see 3). A 1 s ping can therefore
  show the same SpO2 for a while.
- **Filter on quality.** When quality is 0 the value is junk. The CLI prints
  `NA` in that case, so complot skips the point.
- **Persistence.** The setting probably stays on the bracelet after you
  disconnect. Turn it off explicitly to save battery.
- **CLI:** `corsano-cli live -a <ADDR> --realtime`.

## 2. Live HRM during workout: HR only

`set_workout_mode(true)` should make the bracelet emit `LiveHrm` (id 160)
notifications with HR, its quality, steps, energy and the running average HR.
They carry no SpO2. This route is unverified: the SDK decodes the message, but
the sample app never shows what triggers it. Stop with
`set_workout_mode(false)`. CLI: `corsano-cli live -a <ADDR>`.

## 3. Vital sampling rate: how often the bracelet measures

`set_vital_sampling_rate(VitalType::PulseRate | VitalType::Spo2, interval)`
sets the on-device measurement interval. The SDK offers 0 (off), 30, 60, 300
and 1800 s. A shorter interval drains the battery faster.

This setting governs both route 1 (how fresh the values are) and route 4 (how
dense the history is). SpO2 needs the user to keep still, so expect gaps or
quality 0 while moving.

## 4. Activity file: history, works while disconnected

The bracelet logs one 38-byte `ActivityRecord` per measurement interval. Each
record holds a timestamp, HR, SpO2, their qualities, steps, respiration,
temperatures, battery and a wear flag. The Corsano data SDK builds its HR and
SpO2 summaries from this file.

```cpp
auto raw = b.download_file(Corsano::FileId::Activity, 38 * 1000);
auto act = Corsano::decode_activity(raw);
// ... persist act.records ...
b.erase_file(Corsano::FileId::Activity, act.consumed_bytes); // optional
```

`RegularPing.activity_size` tells you how many bytes are waiting. Use this
route to backfill gaps after a disconnect. CLI:
`corsano-cli history -a <ADDR> [--erase]`.

## Not covered

- **Raw PPG waveforms** (`FileId::Ppg`, `Ppg2`). The PPG chunks also contain
  three per-chunk HR estimates (`bpm0..2` with qualities). Getting SpO2 from
  raw PPG would mean re-implementing Corsano's algorithm. Decoding is not
  implemented.
- **Spot measurement** (`StartSpecialMode`, payload 1). It probably starts an
  on-demand measurement whose result shows up in routes 1 or 4. Unverified.
  Use `command(216, {1})` to try it.

## Recommended setup for SpO2

Based on developer.corsano.com and the FW 8.12 sessions above:

1. **Set the user profile**: `corsano-cli profile --set`, from
   `corsano-profile.txt`. The docs say "wrist is mandatory for every metric".
   A placeholder profile (invalid wrist, wrong skin type) produced the
   103/106 alerts.
2. **Run Wearing Optimization**: `corsano-cli calibrate`. Sit or lie still,
   with the bracelet snug a finger above the wrist bone. Aim for quality
   ≥ 70; 50 is the pass mark.
3. **Choose a plan** that measures SpO2 often enough. Typical measures it
   every 30 min, Hospital every 10 s. `MonitorConfig::plan` re-applies the
   plan on every connect.
4. **Set bedtime and risetime** (GMT). The sleep window runs from 2 h before
   bedtime to 3 h after risetime, and SpO2 is mostly a night-time
   measurement.
5. **Keep still while measuring.** 104 is the motion alert. 105 (venous
   pooling) discards the data: don't rest the arm under the body.

Filter: use `spo2` only when `spo2_q` is 2–4. Treat 0–1 as informational,
and 103–106 as alerts (see PROTOCOL.md).

The SDK's "SpO2 spot measurement" records a value from an **external**
oximeter (cmd 247). The bracelet does not measure on demand.
