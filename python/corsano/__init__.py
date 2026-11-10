"""ctypes wrapper for libcorsano_c (Corsano bracelet over BLE). Stdlib only."""
from __future__ import annotations

import ctypes as C
import datetime
import enum
import os
import re
import sys
import traceback
from dataclasses import dataclass, fields
from typing import Callable, NamedTuple, Optional, Union

# MARK: - Library loading

_NAMES = ("libcorsano_c.dylib", "libcorsano_c.so")


def _load() -> C.CDLL:
    here = os.path.dirname(os.path.abspath(__file__))
    tried = []
    env = os.environ.get("CORSANO_LIB")
    if env:
        tried.append(env)
    for d in (here, os.path.normpath(os.path.join(here, "..", "..", "build"))):
        tried.extend(os.path.join(d, n) for n in _NAMES)
    for p in tried:
        if os.path.isfile(p):
            return C.CDLL(p)
    raise OSError("libcorsano_c not found; tried: " + ", ".join(tried))


_lib = _load()

# MARK: - Constants

FILE_ACTIVITY = 2
VITAL_PULSE_RATE = 2
VITAL_SPO2 = 4


class FileId(enum.IntEnum):
    PPG = 1
    ACTIVITY = 2
    HRV = 3
    WORKOUT = 4
    SLEEP = 5
    LOGS = 6
    PPG2 = 9
    ECG = 13
    BIOZ = 14
    GREENTEG = 18
    STRESS = 21
    ACC = 22


class VitalType(enum.IntEnum):
    ACTIVITY = 1
    PULSE_RATE = 2
    RESPIRATION_RATE = 3
    SPO2 = 4
    TEMPERATURE = 5
    NIBP = 9


class Plan(enum.IntEnum):
    STAND_BY = 0
    MAX_BATTERY = 1
    TYPICAL = 2
    HOSPITAL = 3
    HZ25 = 4  # deprecated
    HOSPITAL_RAW = 5
    DISABLED = 6
    TYPICAL_MULTICOLOR = 7
    HOSPITAL_MULTICOLOR = 8
    MULTICOLOR_INTERMITTENT_RAW = 9
    MULTICOLOR_INTERMITTENT = 10
    CONTINUOUS_5MIN = 11


class Ppg2Rate(enum.IntEnum):
    HZ32 = 1
    HZ64 = 2
    HZ128 = 3
    HZ256 = 4
    HZ512 = 5


class Gender(enum.IntEnum):
    MALE = 0
    FEMALE = 1
    UNSPECIFIED = 2


class Wrist(enum.IntEnum):
    LEFT = 0
    RIGHT = 1


class SkinType(enum.IntEnum):
    VERY_FAIR = 1
    FAIR = 2
    OLIVE = 3
    LIGHT_BROWN = 4
    BROWN = 5
    BLACK_BROWN = 6


# spo2_quality values beyond the 0..4 scale (alerts)
SPO2_LOW_CONFIDENCE = 103  # fit or movement: re-seat the bracelet
SPO2_MOTION = 104  # keep still
SPO2_VENOUS_POOLING = 105  # data discarded
SPO2_SKIN_CONDITION = 106  # other arm, recalibrate

# MARK: - C structs

u8, u16, i16, u32 = C.c_uint8, C.c_uint16, C.c_int16, C.c_uint32


class CDevice(C.Structure):
    _fields_ = [("name", C.c_char * 64), ("address", C.c_char * 64),
                ("rssi", i16)]


class CBattery(C.Structure):
    _fields_ = [("percent", u8), ("millivolts", u16), ("charging", C.c_bool)]


class CFirmware(C.Structure):
    _fields_ = [("major", C.c_int), ("minor", C.c_int)]


class CRealtime(C.Structure):
    _fields_ = [("timestamp_s", u32), ("heart_rate", u8),
                ("heart_rate_quality", u8), ("respiration_rate", C.c_float),
                ("respiration_rate_quality", u8), ("spo2", u8),
                ("spo2_quality", u8), ("core_body_temp_c", C.c_float),
                ("movement_level", u8), ("battery_percent", u8)]


class CLiveHrm(C.Structure):
    _fields_ = [("heart_rate", u8), ("heart_rate_quality", u8),
                ("steps", u16), ("energy", u16), ("max_heart_rate", u8),
                ("speed", u8), ("duration", u16), ("battery_percent", u8),
                ("average_heart_rate", u8), ("average_speed", u8)]


class CRegularPing(C.Structure):
    _fields_ = [("battery_percent", u8), ("ppg_size", u16),
                ("activity_size", u16), ("hrv_size", u16),
                ("workout_size", u16), ("sleep_size", u16),
                ("log_size", u16)]


class CActivity(C.Structure):
    _fields_ = [("timestamp_s", u32), ("heart_rate", u8),
                ("heart_rate_quality", u8), ("steps", u16),
                ("activity_type", u8), ("activity_count", u8), ("spo2", u8),
                ("spo2_quality", u8), ("energy", u16),
                ("respiration_rate", C.c_float),
                ("respiration_rate_quality", u8), ("battery_percent", u8),
                ("charging", C.c_bool), ("nibp_systolic", u8),
                ("nibp_diastolic", u8), ("temp1_c", C.c_float),
                ("temp2_c", C.c_float), ("wearing", u8)]


class CStress(C.Structure):
    _fields_ = [("timestamp_s", u32), ("cognitive_zone", u8),
                ("cognitive_zone_quality", u8),
                ("predicted_cognitive_zone", u8),
                ("predicted_transition_time", u16),
                ("cognitive_zone_hour", u8),
                ("cortisol_contribution", u32),
                ("skin_conductance_stress", u16),
                ("skin_conductance_stress_quality", u8)]


class CPpgSample(C.Structure):
    _fields_ = [("timestamp_ms", C.c_uint64), ("chunk_index", u32),
                ("ppg", u16), ("acc", u16), ("acc_x", C.c_int16),
                ("acc_y", C.c_int16), ("acc_z", C.c_int16),
                ("heart_rate", u8), ("heart_rate_quality", u8),
                ("protocol_version", u8)]


class CSample(C.Structure):
    _fields_ = [("time", C.c_double), ("field", C.c_char_p),
                ("value", C.c_double), ("record", u32)]


class CUserProfile(C.Structure):
    _fields_ = [("birth_year", C.c_int), ("birth_month", C.c_int),
                ("birth_day", C.c_int), ("gender", u8), ("wrist", u8),
                ("skin", u8), ("height_cm", u8), ("weight_kg", u8)]


class CWearingOptimization(C.Structure):
    _fields_ = [("finished", C.c_bool), ("progress_percent", u8),
                ("movement_level", u8), ("quality", u8),
                ("success", C.c_bool)]


class CMonitorConfig(C.Structure):
    _fields_ = [("files", C.POINTER(u8)), ("n_files", C.c_size_t),
                ("live_hr", C.c_bool), ("erase", C.c_bool),
                ("set_time", C.c_bool), ("plan", C.c_int),
                ("ppg2_rate", u8), ("ppg1_rate", u8),
                ("profile", C.POINTER(CUserProfile)),
                ("host_id", C.c_char_p),
                ("interval_ms", C.c_int), ("max_queue", C.c_size_t)]


# MARK: - Dataclasses


@dataclass(frozen=True)
class PlanSettings:
    plan: Plan
    ppg2_rate: Ppg2Rate = Ppg2Rate.HZ32
    ppg1_rate: int = 60


@dataclass(frozen=True)
class UserProfile:
    birth: datetime.date
    gender: Gender
    wrist: Wrist
    skin: SkinType
    height_cm: int = 0  # 0 = leave unchanged when setting
    weight_kg: int = 0


def _profile_to_c(p: UserProfile) -> CUserProfile:
    return CUserProfile(p.birth.year, p.birth.month, p.birth.day,
                        int(p.gender), int(p.wrist), int(p.skin),
                        p.height_cm, p.weight_kg)


@dataclass(frozen=True)
class WearingOptimizationStatus:
    finished: bool
    progress_percent: int
    movement_level: int
    quality: int  # 0..100; >= 50 passes
    success: bool


class Sample(NamedTuple):
    time: float  # Unix epoch seconds
    field: str  # e.g. "activity.spo2"
    value: float
    record: int  # samples from the same record share it


@dataclass(frozen=True)
class Device:
    name: str
    address: str
    rssi: int


@dataclass(frozen=True)
class Battery:
    percent: int
    millivolts: int
    charging: bool


@dataclass(frozen=True)
class Firmware:
    major: int
    minor: int


@dataclass(frozen=True)
class RealtimePing:
    timestamp_s: int
    heart_rate: int
    heart_rate_quality: int
    respiration_rate: float
    respiration_rate_quality: int
    spo2: int
    spo2_quality: int
    core_body_temp_c: float
    movement_level: int
    battery_percent: int


@dataclass(frozen=True)
class LiveHrm:
    heart_rate: int
    heart_rate_quality: int
    steps: int
    energy: int
    max_heart_rate: int
    speed: int
    duration: int
    battery_percent: int
    average_heart_rate: int
    average_speed: int


@dataclass(frozen=True)
class RegularPing:
    battery_percent: int
    ppg_size: int
    activity_size: int
    hrv_size: int
    workout_size: int
    sleep_size: int
    log_size: int


@dataclass(frozen=True)
class ActivityRecord:
    timestamp_s: int
    heart_rate: int
    heart_rate_quality: int
    steps: int
    activity_type: int
    activity_count: int
    spo2: int
    spo2_quality: int
    energy: int
    respiration_rate: float
    respiration_rate_quality: int
    battery_percent: int
    charging: bool
    nibp_systolic: int
    nibp_diastolic: int
    temp1_c: float
    temp2_c: float
    wearing: int


@dataclass(frozen=True)
class StressRecord:
    timestamp_s: int
    cognitive_zone: int
    cognitive_zone_quality: int
    predicted_cognitive_zone: int
    predicted_transition_time: int
    cognitive_zone_hour: int
    cortisol_contribution: int
    skin_conductance_stress: int
    skin_conductance_stress_quality: int


@dataclass(frozen=True)
class PpgSample:
    timestamp_ms: int
    chunk_index: int
    ppg: int
    acc: int
    acc_x: int
    acc_y: int
    acc_z: int
    heart_rate: int
    heart_rate_quality: int
    protocol_version: int


def _conv(cls, s):
    vals = {}
    for f in fields(cls):
        v = getattr(s, f.name)
        vals[f.name] = v.decode("utf-8", "replace") if isinstance(v, bytes) else v
    return cls(**vals)


# MARK: - Errors


class CorsanoError(Exception):
    def __init__(self, code: int, message: str = ""):
        super().__init__(message or f"corsano error {code}")
        self.code = code


class Timeout(CorsanoError):
    pass


class PairingRequired(CorsanoError):
    pass


class ProtocolError(CorsanoError):
    pass


class ArgumentError(CorsanoError, ValueError):
    pass


_ERRORS = {-1: Timeout, -2: PairingRequired, -3: ProtocolError,
           -4: ArgumentError}


def _last_error() -> str:
    return (_lib.corsano_last_error() or b"").decode("utf-8", "replace")


def _check(rc: int) -> int:
    if rc != 0:
        raise _ERRORS.get(rc, CorsanoError)(rc, _last_error())
    return rc


# MARK: - Function prototypes

_P = C.POINTER
_h = C.c_void_p  # corsano_t*
_CB_RT = C.CFUNCTYPE(None, _P(CRealtime), C.c_void_p)
_CB_HRM = C.CFUNCTYPE(None, _P(CLiveHrm), C.c_void_p)
_CB_PING = C.CFUNCTYPE(None, _P(CRegularPing), C.c_void_p)
_CB_BAT = C.CFUNCTYPE(None, _P(CBattery), C.c_void_p)
_CB_DISC = C.CFUNCTYPE(None, C.c_void_p)


def _proto(name, res, *args):
    f = getattr(_lib, name)
    f.restype, f.argtypes = res, list(args)


_i, _sz = C.c_int, C.c_size_t
_proto("corsano_last_error", C.c_char_p)
_proto("corsano_scan", _i, _i, _P(CDevice), _sz, _P(_sz))
_proto("corsano_init", _h, C.c_char_p)
_proto("corsano_free", None, _h)
_proto("corsano_connect", _i, _h, _i)
_proto("corsano_disconnect", None, _h)
_proto("corsano_is_connected", C.c_bool, _h)
_proto("corsano_command", _i, _h, u8, C.c_char_p, _sz, C.c_void_p, _sz,
       _P(_sz), _i)
_proto("corsano_battery", _i, _h, _P(CBattery))
_proto("corsano_firmware_version", _i, _h, _P(CFirmware))
_proto("corsano_serial_number", _i, _h, C.c_char_p, _sz)
_proto("corsano_set_time", _i, _h)
_proto("corsano_set_host_id", _i, _h, C.c_char_p)
_proto("corsano_authenticated", _i, _h, _P(C.c_bool))
_proto("corsano_plan", _i, _h, _P(u8), _P(u8), _P(u8))
_proto("corsano_set_plan", _i, _h, u8, u8, u8)
_proto("corsano_user_profile", _i, _h, _P(CUserProfile))
_proto("corsano_set_user_profile", _i, _h, _P(CUserProfile))
_proto("corsano_start_wearing_optimization", _i, _h)
_proto("corsano_wearing_optimization_status", _i, _h,
       _P(CWearingOptimization))
_proto("corsano_stop_wearing_optimization", _i, _h)
_proto("corsano_set_realtime_enabled", _i, _h, C.c_bool)
_proto("corsano_realtime_enabled", _i, _h, _P(C.c_bool))
_proto("corsano_set_realtime_period", _i, _h, _i)
_proto("corsano_realtime_period", _i, _h, _P(_i))
_proto("corsano_set_vital_sampling_rate", _i, _h, u8, _i)
_proto("corsano_vital_sampling_rate", _i, _h, u8, _P(_i))
_proto("corsano_set_workout_mode", _i, _h, C.c_bool)
_proto("corsano_file_size", _i, _h, u8, _P(u32))
_proto("corsano_download_file", _i, _h, u8, C.c_void_p, _sz, _P(_sz))
_proto("corsano_erase_file", _i, _h, u8, u32)
_proto("corsano_decode_activity", _i, C.c_char_p, _sz, _P(CActivity), _sz,
       _P(_sz), _P(_sz))
_proto("corsano_decode_stress", _i, C.c_char_p, _sz, _P(CStress), _sz,
       _P(_sz), _P(_sz))
_proto("corsano_decode_ppg", _i, C.c_char_p, _sz, _P(CPpgSample), _sz,
       _P(_sz), _P(_sz))
_mon = C.c_void_p  # corsano_monitor_t*
_proto("corsano_monitor_config_default", None, _P(CMonitorConfig))
_proto("corsano_monitor_init", _mon, C.c_char_p, _P(CMonitorConfig))
_proto("corsano_monitor_free", None, _mon)
_proto("corsano_monitor_start", _i, _mon)
_proto("corsano_monitor_sync_clock", _i, _mon)
_proto("corsano_monitor_stop", None, _mon)
_proto("corsano_monitor_running", C.c_bool, _mon)
_proto("corsano_monitor_last_error", _i, _mon, C.c_char_p, _sz)
_proto("corsano_monitor_poll", _i, _mon, _P(CSample), _sz, _P(_sz))
_proto("corsano_monitor_wait", _i, _mon, _P(CSample), _sz, _P(_sz), _i)
_proto("corsano_set_realtime_callback", None, _h, _CB_RT, C.c_void_p)
_proto("corsano_set_live_hrm_callback", None, _h, _CB_HRM, C.c_void_p)
_proto("corsano_set_regular_ping_callback", None, _h, _CB_PING, C.c_void_p)
_proto("corsano_set_battery_callback", None, _h, _CB_BAT, C.c_void_p)
_proto("corsano_set_disconnect_callback", None, _h, _CB_DISC, C.c_void_p)

# MARK: - Discovery & decoding


def scan(duration: float = 5.0) -> list[Device]:
    """Scan for bracelets for `duration` seconds (up to 64 devices)."""
    devs = (CDevice * 64)()
    n = _sz(0)
    _check(_lib.corsano_scan(int(duration * 1000), devs, 64, C.byref(n)))
    return [_conv(Device, devs[i]) for i in range(n.value)]


def _decode(cfunc, cstruct, cls, data, capacity):
    data = bytes(data)
    rows = (cstruct * capacity)()
    count, used = _sz(0), _sz(0)
    _check(cfunc(data, len(data), rows, capacity, C.byref(count),
                 C.byref(used)))
    return [_conv(cls, rows[i]) for i in range(count.value)], used.value


def decode_activity(data: bytes) -> tuple[list[ActivityRecord], int]:
    """Decode 38-byte Activity records; returns (records, bytes consumed)."""
    return _decode(_lib.corsano_decode_activity, CActivity, ActivityRecord,
                   data, len(data) // 38 + 1)


def decode_stress(data: bytes) -> tuple[list[StressRecord], int]:
    """Decode 18-byte Stress records (raw firmware units); returns (records,
    bytes consumed)."""
    return _decode(_lib.corsano_decode_stress, CStress, StressRecord, data,
                   len(data) // 18 + 1)


def decode_ppg(data: bytes) -> tuple[list[PpgSample], int]:
    """Decode a PPG file into one row per 25 Hz sample; returns (samples,
    bytes consumed). Pass `consumed` to erase_file, not len(data)."""
    return _decode(_lib.corsano_decode_ppg, CPpgSample, PpgSample, data,
                   len(data) // 3 + 1)


# MARK: - Bracelet


def _safe(fn: Callable, cls, ptr) -> None:
    """Run user callback; never let an exception reach C."""
    try:
        fn(_conv(cls, ptr.contents))
    except BaseException:
        traceback.print_exc(file=sys.stderr)


class Bracelet:
    """One bracelet. Methods on a closed instance raise ArgumentError.

    Callbacks (on_*) are invoked from the BLE backend thread, not the caller's.
    """

    def __init__(self, address: str, host_id: Optional[str] = None):
        """host_id: identity sent to FW >= 8.41 on connect (default: this
        host's name). Use the same one across runs."""
        self.address = address
        self._h: Optional[int] = _lib.corsano_init(address.encode())
        if not self._h:
            raise CorsanoError(-5, _last_error() or "corsano_init failed")
        if host_id:
            _check(_lib.corsano_set_host_id(self._h, host_id.encode()))
        self._cbs: dict[str, object] = {}

    def __enter__(self) -> "Bracelet":
        self.connect()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def close(self) -> None:
        """Disconnect and free the handle; idempotent."""
        h, self._h = getattr(self, "_h", None), None
        if h:
            _lib.corsano_free(h)
        getattr(self, "_cbs", {}).clear()

    @property
    def _handle(self) -> int:
        if not self._h:
            raise ArgumentError(-4, "Bracelet is closed")
        return self._h

    # MARK: Connection

    def connect(self, scan_timeout: float = 10.0) -> None:
        _check(_lib.corsano_connect(self._handle, int(scan_timeout * 1000)))

    def disconnect(self) -> None:
        _lib.corsano_disconnect(self._handle)

    @property
    def is_connected(self) -> bool:
        return bool(self._h) and _lib.corsano_is_connected(self._h)

    def command(self, id: int, payload: bytes = b"",
                timeout: float = 15.0) -> bytes:
        cap = 512
        for attempt in range(2):
            buf = C.create_string_buffer(cap)
            n = _sz(0)
            try:
                _check(_lib.corsano_command(
                    self._handle, id, bytes(payload), len(payload), buf, cap,
                    C.byref(n), int(timeout * 1000)))
                return buf.raw[:n.value]
            except ArgumentError as e:
                if attempt or "buffer too small" not in str(e).lower():
                    raise
                m = re.findall(r"\d+", str(e))
                cap = max([n.value] + [int(x) for x in m] + [cap + 1])

    # MARK: Device info

    def battery(self) -> Battery:
        o = CBattery()
        _check(_lib.corsano_battery(self._handle, C.byref(o)))
        return _conv(Battery, o)

    def firmware_version(self) -> Firmware:
        o = CFirmware()
        _check(_lib.corsano_firmware_version(self._handle, C.byref(o)))
        return _conv(Firmware, o)

    def serial_number(self) -> str:
        buf = C.create_string_buffer(64)
        _check(_lib.corsano_serial_number(self._handle, buf, 64))
        return buf.value.decode("utf-8", "replace")

    @property
    def authenticated(self) -> bool:
        """True if this connection is authenticated (always True on FW < 8.41)."""
        o = C.c_bool()
        _check(_lib.corsano_authenticated(self._handle, C.byref(o)))
        return o.value

    def set_time(self) -> None:
        _check(_lib.corsano_set_time(self._handle))

    # MARK: Plan

    @property
    def plan(self) -> PlanSettings:
        p, r2, r1 = u8(), u8(), u8()
        _check(_lib.corsano_plan(self._handle, C.byref(p), C.byref(r2),
                                 C.byref(r1)))
        return PlanSettings(Plan(p.value), Ppg2Rate(r2.value), r1.value)

    def set_plan(self, plan: Plan, ppg2_rate: Ppg2Rate = Ppg2Rate.HZ32,
                 ppg1_rate: int = 60) -> None:
        _check(_lib.corsano_set_plan(self._handle, int(plan), int(ppg2_rate),
                                     ppg1_rate))

    # MARK: User profile and calibration

    @property
    def user_profile(self) -> UserProfile:
        c = CUserProfile()
        _check(_lib.corsano_user_profile(self._handle, C.byref(c)))
        return UserProfile(
            datetime.date(c.birth_year, c.birth_month, c.birth_day),
            Gender(c.gender), Wrist(c.wrist), SkinType(c.skin),
            c.height_cm, c.weight_kg)

    def set_user_profile(self, profile: UserProfile) -> None:
        c = _profile_to_c(profile)
        _check(_lib.corsano_set_user_profile(self._handle, C.byref(c)))

    def start_wearing_optimization(self) -> None:
        _check(_lib.corsano_start_wearing_optimization(self._handle))

    def wearing_optimization_status(self) -> WearingOptimizationStatus:
        c = CWearingOptimization()
        _check(_lib.corsano_wearing_optimization_status(self._handle,
                                                        C.byref(c)))
        return WearingOptimizationStatus(
            c.finished, c.progress_percent, c.movement_level, c.quality,
            c.success)

    def stop_wearing_optimization(self) -> None:
        _check(_lib.corsano_stop_wearing_optimization(self._handle))

    # MARK: Live vitals

    @property
    def realtime_enabled(self) -> bool:
        o = C.c_bool()
        _check(_lib.corsano_realtime_enabled(self._handle, C.byref(o)))
        return o.value

    def set_realtime_enabled(self, enabled: bool) -> None:
        _check(_lib.corsano_set_realtime_enabled(self._handle, bool(enabled)))

    @property
    def realtime_period(self) -> int:
        o = C.c_int()
        _check(_lib.corsano_realtime_period(self._handle, C.byref(o)))
        return o.value

    def set_realtime_period(self, seconds: int) -> None:
        _check(_lib.corsano_set_realtime_period(self._handle, int(seconds)))

    def vital_sampling_rate(self, vital: int) -> int:
        o = C.c_int()
        _check(_lib.corsano_vital_sampling_rate(self._handle, int(vital),
                                                C.byref(o)))
        return o.value

    def set_vital_sampling_rate(self, vital: int, seconds: int) -> None:
        _check(_lib.corsano_set_vital_sampling_rate(self._handle, int(vital),
                                                    int(seconds)))

    def set_workout_mode(self, enabled: bool) -> None:
        _check(_lib.corsano_set_workout_mode(self._handle, bool(enabled)))

    # MARK: Files

    def file_size(self, file: int) -> int:
        o = u32()
        _check(_lib.corsano_file_size(self._handle, int(file), C.byref(o)))
        return o.value

    def download_file(self, file: int, max_bytes: int) -> bytes:
        buf = C.create_string_buffer(max(max_bytes, 1))
        n = _sz(0)
        _check(_lib.corsano_download_file(self._handle, int(file), buf,
                                          max_bytes, C.byref(n)))
        return buf.raw[:n.value]

    def erase_file(self, file: int, nbytes: int) -> None:
        _check(_lib.corsano_erase_file(self._handle, int(file), int(nbytes)))

    # MARK: Callbacks

    def _set_cb(self, key: str, setter, ctype, wrap, fn) -> None:
        h = self._handle
        if fn is None:
            setter(h, ctype(), None)  # NULL clears
            self._cbs.pop(key, None)
            return
        cb = ctype(wrap(fn))
        self._cbs[key] = cb  # keep alive: GC would crash the BLE thread
        setter(h, cb, None)

    def on_realtime(self, fn: Optional[Callable[[RealtimePing], None]]) -> None:
        self._set_cb("rt", _lib.corsano_set_realtime_callback, _CB_RT,
                     lambda f: lambda p, u: _safe(f, RealtimePing, p), fn)

    def on_live_hrm(self, fn: Optional[Callable[[LiveHrm], None]]) -> None:
        self._set_cb("hrm", _lib.corsano_set_live_hrm_callback, _CB_HRM,
                     lambda f: lambda p, u: _safe(f, LiveHrm, p), fn)

    def on_regular_ping(self,
                        fn: Optional[Callable[[RegularPing], None]]) -> None:
        self._set_cb("ping", _lib.corsano_set_regular_ping_callback, _CB_PING,
                     lambda f: lambda p, u: _safe(f, RegularPing, p), fn)

    def on_battery(self, fn: Optional[Callable[[Battery], None]]) -> None:
        self._set_cb("bat", _lib.corsano_set_battery_callback, _CB_BAT,
                     lambda f: lambda p, u: _safe(f, Battery, p), fn)

    def on_disconnect(self, fn: Optional[Callable[[], None]]) -> None:
        def wrap(f):
            def cb(u):
                try:
                    f()
                except BaseException:
                    traceback.print_exc(file=sys.stderr)
            return cb
        self._set_cb("disc", _lib.corsano_set_disconnect_callback, _CB_DISC,
                     wrap, fn)


# MARK: - Monitor


class Monitor:
    """Background thread that keeps a bracelet connected, polls its files and
    queues tidy Samples. Use as a context manager (starts on enter), then
    iterate (blocking) or call poll()/wait()."""

    def __init__(self, device: str, files: Optional[list[int]] = None,
                 live_hr: bool = False, erase: bool = True,
                 set_time: bool = True,
                 plan: Union[Plan, PlanSettings, None] = None,
                 interval: float = 0.0, max_queue: int = 100000,
                 profile: Optional[UserProfile] = None,
                 host_id: Optional[str] = None):
        cfg = CMonitorConfig()
        _lib.corsano_monitor_config_default(C.byref(cfg))
        self._files = None
        if files is not None:
            self._files = (u8 * len(files))(*[int(f) for f in files])
            cfg.files = C.cast(self._files, C.POINTER(u8))
            cfg.n_files = len(files)
        cfg.live_hr, cfg.erase, cfg.set_time = live_hr, erase, set_time
        if plan is not None:
            ps = plan if isinstance(plan, PlanSettings) else PlanSettings(plan)
            cfg.plan = int(ps.plan)
            cfg.ppg2_rate, cfg.ppg1_rate = int(ps.ppg2_rate), ps.ppg1_rate
        self._profile = None  # kept alive for the config pointer
        if profile is not None:
            self._profile = _profile_to_c(profile)
            cfg.profile = C.pointer(self._profile)
        self._host_id = host_id.encode() if host_id else None  # kept alive
        cfg.host_id = self._host_id
        cfg.interval_ms = int(interval * 1000)
        cfg.max_queue = max_queue
        self._m: Optional[int] = _lib.corsano_monitor_init(
            device.encode(), C.byref(cfg))
        if not self._m:
            raise CorsanoError(-5, _last_error() or "monitor init failed")

    def __enter__(self) -> "Monitor":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def close(self) -> None:
        """Stop and free; idempotent."""
        m, self._m = getattr(self, "_m", None), None
        if m:
            _lib.corsano_monitor_free(m)

    @property
    def _handle(self) -> int:
        if not self._m:
            raise ArgumentError(-4, "Monitor is closed")
        return self._m

    def start(self) -> None:
        _check(_lib.corsano_monitor_start(self._handle))

    def stop(self) -> None:
        _lib.corsano_monitor_stop(self._handle)

    def sync_clock(self) -> None:
        """Set the bracelet clock to the host clock; raises if not connected."""
        _check(_lib.corsano_monitor_sync_clock(self._handle))

    @property
    def running(self) -> bool:
        return bool(self._m) and bool(_lib.corsano_monitor_running(self._m))

    @property
    def last_error(self) -> str:
        buf = C.create_string_buffer(256)
        _check(_lib.corsano_monitor_last_error(self._handle, buf, 256))
        return buf.value.decode("utf-8", "replace")

    @staticmethod
    def _convert(arr, n: int) -> list[Sample]:
        return [Sample(arr[i].time, (arr[i].field or b"").decode(),
                       arr[i].value, arr[i].record) for i in range(n)]

    def poll(self, max: int = 4096) -> list[Sample]:
        """Non-blocking: up to `max` queued samples."""
        arr, n = (CSample * max)(), C.c_size_t()
        _check(_lib.corsano_monitor_poll(self._handle, arr, max, C.byref(n)))
        return self._convert(arr, n.value)

    def wait(self, timeout: float = 1.0, max: int = 4096) -> list[Sample]:
        """Block until a sample arrives, `timeout` s pass or stop()."""
        arr, n = (CSample * max)(), C.c_size_t()
        _check(_lib.corsano_monitor_wait(self._handle, arr, max, C.byref(n),
                                         int(timeout * 1000)))
        return self._convert(arr, n.value)

    def __iter__(self):
        while self._m and self.running:
            yield from self.wait()
