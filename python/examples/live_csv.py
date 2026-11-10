#!/usr/bin/env python3
"""Stream live vitals as CSV (same format as `corsano-cli live`).

  python live_csv.py ADDR | complot hr --aes "x=t y=hr,avg_hr" -

Default: workout mode, LiveHrm every 2 s (the only continuous HR source on
FW 8.12; each run logs a workout on the bracelet). --realtime: RealtimePing
with SpO2/resp/temp, but FW 8.12 sends only one, ~12 s after connecting.
"""
import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import corsano  # noqa: E402


def na(v, q):
    return v if q else "NA"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("address")
    ap.add_argument("--realtime", action="store_true",
                    help="realtime pings instead of workout-mode LiveHrm")
    ap.add_argument("--period", type=int, default=1,
                    help="realtime: ping period in seconds (default 1)")
    ap.add_argument("--duration", type=float, default=0,
                    help="stop after N seconds (default: until Ctrl-C)")
    a = ap.parse_args()

    t0 = time.monotonic()
    lost = threading.Event()

    def now() -> str:
        return f"{time.monotonic() - t0:.3f}"

    def realtime_row(p: corsano.RealtimePing) -> None:
        print(f"{now()},{p.timestamp_s},"
              f"{na(p.heart_rate, p.heart_rate_quality)},{p.heart_rate_quality},"
              f"{na(p.spo2, p.spo2_quality)},{p.spo2_quality},"
              f"{p.respiration_rate:.2f},{p.respiration_rate_quality},"
              f"{p.core_body_temp_c:.2f},{p.movement_level},"
              f"{p.battery_percent}", flush=True)

    def hrm_row(h: corsano.LiveHrm) -> None:
        print(f"{now()},{h.heart_rate},{h.heart_rate_quality},{h.steps},"
              f"{h.energy},{h.average_heart_rate},{h.battery_percent}",
              flush=True)

    b = corsano.Bracelet(a.address)
    was_enabled = True  # realtime setting found on the bracelet, restored
    try:
        print(f"connecting to {a.address}...", file=sys.stderr)
        b.connect()
        b.on_disconnect(lost.set)
        if a.realtime:
            print("t,device_t,hr,hr_q,spo2,spo2_q,resp,resp_q,temp,movement,"
                  "battery", flush=True)
            b.on_realtime(realtime_row)
            try:
                b.set_realtime_period(a.period)
            except corsano.ProtocolError as e:  # FW 8.12 rejects it
                print(f"warning: {e}; using bracelet period "
                      f"{b.realtime_period} s", file=sys.stderr)
            was_enabled = b.realtime_enabled
            b.set_realtime_enabled(True)
        else:
            print("t,hr,hr_q,steps,energy,avg_hr,battery", flush=True)
            b.on_live_hrm(hrm_row)
            b.set_workout_mode(True)
        print("streaming, Ctrl-C to stop", file=sys.stderr)
        end = t0 + a.duration if a.duration else None
        while not lost.is_set() and (end is None or time.monotonic() < end):
            time.sleep(0.2)
        if lost.is_set():
            print("disconnected", file=sys.stderr)
    except KeyboardInterrupt:
        print("stopping", file=sys.stderr)
    except corsano.CorsanoError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        try:
            if b.is_connected:
                if not a.realtime:
                    b.set_workout_mode(False)
                elif not was_enabled:
                    b.set_realtime_enabled(False)
        except corsano.CorsanoError as e:
            print(f"warning: cleanup failed: {e}", file=sys.stderr)
        b.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
