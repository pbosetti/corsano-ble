"""Offline tests (no device). Run: python3 -I -m unittest discover -s python/tests"""
import ctypes
import datetime
import os
import struct
import sys
import time
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import corsano  # noqa: E402


def crc8(data: bytes) -> int:
    c = 0xFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def record(ts, hr, spo2, resp4, batt, temp1, temp2) -> bytes:
    r = bytearray(38)
    struct.pack_into("<IBBH", r, 0, ts, hr, 4, 100)
    r[8], r[9], r[10] = 1, 2, spo2
    struct.pack_into("<H", r, 11, 7)
    r[13], r[14] = resp4, 3
    struct.pack_into("<b", r, 15, batt)
    r[18] = 4
    struct.pack_into("<hh", r, 31, temp1, temp2)
    r[36] = 1
    r[37] = crc8(bytes(r[:37]))
    return bytes(r)


DATA = (record(1000, 60, 97, 64, 80, 3650, -120)
        + record(1001, 70, 98, 60, -50, 3700, 3710)
        + record(1002, 80, 99, 56, 10, 3720, 3730))


def stress_record(ts) -> bytes:
    r = struct.pack("<IBBHBIBHB", ts, 2, 3, 0x0102, 14, 0x11223344, 5,
                    0xA1B2, 7)
    return r + bytes([crc8(r)])


def ppg_chunk_v3(index=3, ts_s=1000, ms=500) -> bytes:
    c = bytearray(481)
    c[:6] = b"\x00\xff\xaaMMT"
    c[6] = 3
    struct.pack_into("<HIIH", c, 9, 75, index, ts_s, ms)
    c[21:27] = bytes([60, 1, 70, 2, 80, 3])
    for i in range(75):
        struct.pack_into("<HBbbb", c, 27 + 6 * i, 2000 + i, 50, -5, 6, 7)
    struct.pack_into("<I", c, 477, zlib.crc32(bytes(c[:477])))
    return bytes(c)


class Offline(unittest.TestCase):
    def test_profile_types(self):
        self.assertEqual(ctypes.sizeof(corsano.CUserProfile), 20)
        self.assertEqual(ctypes.sizeof(corsano.CWearingOptimization), 5)
        self.assertEqual(corsano.CMonitorConfig.profile.offset
                         % ctypes.alignment(ctypes.c_void_p), 0)
        self.assertEqual((corsano.Gender.UNSPECIFIED, corsano.Wrist.RIGHT,
                          corsano.SkinType.BLACK_BROWN), (2, 1, 6))
        self.assertEqual((corsano.SPO2_LOW_CONFIDENCE, corsano.SPO2_MOTION,
                          corsano.SPO2_VENOUS_POOLING,
                          corsano.SPO2_SKIN_CONDITION), (103, 104, 105, 106))
        c = corsano._profile_to_c(corsano.UserProfile(
            datetime.date(1990, 5, 17), corsano.Gender.FEMALE,
            corsano.Wrist.LEFT, corsano.SkinType.OLIVE, 170))
        self.assertEqual((c.birth_year, c.birth_month, c.birth_day, c.gender,
                          c.skin, c.height_cm, c.weight_kg),
                         (1990, 5, 17, 1, 3, 170, 0))

    def test_decode(self):
        recs, used = corsano.decode_activity(DATA)
        self.assertEqual((len(recs), used), (3, 114))
        a, b, c = recs
        self.assertEqual((a.timestamp_s, a.heart_rate, a.spo2), (1000, 60, 97))
        self.assertEqual(c.heart_rate, 80)
        self.assertAlmostEqual(a.respiration_rate, 16.0)
        self.assertAlmostEqual(a.temp1_c, 36.5, places=4)
        self.assertAlmostEqual(a.temp2_c, -1.2, places=4)
        self.assertEqual((a.battery_percent, a.charging), (80, False))
        self.assertEqual((b.battery_percent, b.charging), (50, True))

    def test_garbage_prefix(self):
        recs, used = corsano.decode_activity(b"\x00" * 5 + DATA)
        self.assertEqual((len(recs), used), (3, 119))

    def test_stress(self):
        data = b"\x00" * 5 + b"".join(stress_record(3000 + i) for i in range(3))
        recs, used = corsano.decode_stress(data)
        self.assertEqual((len(recs), used), (3, 59))
        self.assertEqual(recs[2].timestamp_s, 3002)
        self.assertEqual(recs[0], corsano.StressRecord(
            3000, 2, 5, 3, 0x0102, 14, 0x11223344, 0xA1B2, 7))
        self.assertEqual(corsano.decode_stress(b""), ([], 0))

    def test_ppg(self):
        s, used = corsano.decode_ppg(ppg_chunk_v3())
        self.assertEqual((len(s), used), (75, 481))
        self.assertEqual(s[0].timestamp_ms, 1000500)
        self.assertEqual(s[74].timestamp_ms, 1000500 + 74 * 40)
        self.assertEqual((s[0].ppg, s[0].acc, s[0].protocol_version),
                         (2000, 50, 3))
        self.assertEqual((s[0].acc_x, s[0].acc_y, s[0].acc_z), (-20, 24, 28))
        self.assertEqual((s[0].chunk_index, s[0].heart_rate), (3, 60))
        self.assertEqual((s[30].heart_rate, s[74].heart_rate_quality), (70, 3))

    def test_ppg_garbage_prefix(self):
        s, used = corsano.decode_ppg(b"\x00" * 5 + ppg_chunk_v3())
        self.assertEqual((len(s), used), (75, 486))
        self.assertEqual(corsano.decode_ppg(b""), ([], 0))

    def test_empty_and_short(self):
        self.assertEqual(corsano.decode_activity(b""), ([], 0))
        self.assertEqual(corsano.decode_activity(b"\x01" * 10)[0], [])

    def test_struct_sizes(self):
        for n in dir(corsano):
            o = getattr(corsano, n)
            if isinstance(o, type) and issubclass(o, ctypes.Structure) \
                    and o is not ctypes.Structure:
                self.assertGreater(ctypes.sizeof(o), 0, n)
        self.assertEqual(ctypes.sizeof(corsano.CDevice), 130)

    def test_enums(self):
        self.assertEqual(corsano.FileId.ACTIVITY, corsano.FILE_ACTIVITY)
        self.assertEqual(corsano.VitalType.SPO2, corsano.VITAL_SPO2)

    def test_closed_instance_raises(self):
        # Closed Bracelet methods raise ArgumentError (CorsanoError + ValueError).
        try:
            b = corsano.Bracelet("00000000-0000-0000-0000-000000000000")
        except corsano.CorsanoError:
            self.skipTest("corsano_init failed (no BLE adapter?)")
        b.close()
        b.close()  # idempotent
        self.assertFalse(b.is_connected)
        with self.assertRaises(corsano.ArgumentError):
            b.battery()
        with self.assertRaises(ValueError):
            b.command(1)
        with self.assertRaises(corsano.CorsanoError):
            b.on_realtime(lambda p: None)

    def test_plan_enums(self):
        self.assertEqual([int(p) for p in corsano.Plan], list(range(12)))
        self.assertEqual(corsano.Plan.CONTINUOUS_5MIN, 11)
        self.assertEqual([int(r) for r in corsano.Ppg2Rate], [1, 2, 3, 4, 5])
        ps = corsano.PlanSettings(corsano.Plan.HOSPITAL)
        self.assertEqual((ps.ppg2_rate, ps.ppg1_rate), (corsano.Ppg2Rate.HZ32, 60))

    def test_monitor_layouts(self):
        # x86-64/arm64 LP64; verified against C offsetof in the layout check
        self.assertEqual(ctypes.sizeof(corsano.CSample), 32)
        self.assertEqual(corsano.CSample.record.offset, 24)
        self.assertEqual(ctypes.sizeof(corsano.CMonitorConfig), 64)
        self.assertEqual(corsano.CMonitorConfig.profile.offset, 32)
        self.assertEqual(corsano.CMonitorConfig.host_id.offset, 40)
        self.assertEqual(corsano.CMonitorConfig.max_queue.offset, 56)

    def test_monitor_offline(self):
        try:
            m = corsano.Monitor("fake-device", files=[corsano.FileId.ACTIVITY],
                                plan=corsano.Plan.HOSPITAL)
        except corsano.CorsanoError:
            self.skipTest("monitor init failed")
        self.assertEqual(m.poll(), [])
        t = time.monotonic()
        self.assertEqual(m.wait(0.05), [])
        self.assertLess(time.monotonic() - t, 1.0)
        self.assertFalse(m.running)
        m.close()
        m.close()  # idempotent
        with self.assertRaises(corsano.ArgumentError):
            m.poll()

    def test_command_catalog(self):
        cat = corsano.command_catalog()
        self.assertGreater(len(cat), 40)
        self.assertEqual([c.id for c in cat], sorted(c.id for c in cat))
        plan = corsano.find_command("GET_plan")
        self.assertEqual(plan, corsano.find_command(150))
        self.assertEqual((plan.name, plan.is_set), ("get-plan", False))
        self.assertTrue(corsano.find_command("set-plan").is_set)
        self.assertIsNone(corsano.find_command("nonsense"))
        self.assertIsNone(corsano.find_command(3))

    def test_describe_reply(self):
        d = corsano.describe_reply
        self.assertEqual(d(150, bytes.fromhex("07013c00")),
                         "plan=typical-multicolor ppg2=32Hz ppg1=60")
        self.assertEqual(d(51, bytes.fromhex("6c28f36a")),
                         "time=2026-11-10T14:21:32Z")
        self.assertEqual(d(223, bytes.fromhex("01000000")),
                         "status=refused(1)")
        self.assertEqual(d(3, b"\x01"), "")


if __name__ == "__main__":
    unittest.main()
