// C API (include/corsano/corsano.h) over Corsano::Bracelet.
#include "corsano/corsano.h"

#include "corsano/bracelet.hpp"
#include "corsano/files.hpp"
#include "corsano/monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Corsano;

struct corsano {
  explicit corsano(std::string address) : bracelet(std::move(address)) {}

  std::mutex cb_mutex;
  corsano_realtime_cb realtime_fn = nullptr;
  void *realtime_user = nullptr;
  corsano_live_hrm_cb live_hrm_fn = nullptr;
  void *live_hrm_user = nullptr;
  corsano_regular_ping_cb regular_ping_fn = nullptr;
  void *regular_ping_user = nullptr;
  corsano_battery_cb battery_fn = nullptr;
  void *battery_user = nullptr;
  corsano_disconnect_cb disconnect_fn = nullptr;
  void *disconnect_user = nullptr;
  // Last: destroyed first, while cb_mutex and the fields above are alive
  Bracelet bracelet;
};

struct corsano_monitor {
  explicit corsano_monitor(std::string device, MonitorConfig cfg)
      : monitor(std::move(device), std::move(cfg)) {}
  Monitor monitor;
  std::vector<Sample> buf;
};

namespace {

// MARK: - Status

thread_local std::string LastError;

int fail(int code, const char *what) {
  LastError = what;
  return code;
}

// Runs f, mapping every exception to a status code; no exception escapes
template <class F> int guard(F &&f) {
  try {
    f();
    LastError.clear();
    return CORSANO_OK;
  } catch (const Timeout &e) {
    return fail(CORSANO_ERR_TIMEOUT, e.what());
  } catch (const PairingRequired &e) { // before ProtocolError (base class)
    return fail(CORSANO_ERR_PAIRING, e.what());
  } catch (const ProtocolError &e) {
    return fail(CORSANO_ERR_PROTOCOL, e.what());
  } catch (const std::invalid_argument &e) {
    return fail(CORSANO_ERR_ARGUMENT, e.what());
  } catch (const std::exception &e) {
    return fail(CORSANO_ERR_BLE, e.what());
  } catch (...) {
    return fail(CORSANO_ERR_BLE, "unknown error");
  }
}

int bad_argument(const char *what) { return fail(CORSANO_ERR_ARGUMENT, what); }

// MARK: - Conversions

void copy_string(char *dst, size_t size, const std::string &src) {
  std::strncpy(dst, src.c_str(), size - 1);
  dst[size - 1] = '\0';
}

// Throws std::invalid_argument (-> CORSANO_ERR_ARGUMENT) on out-of-range input
UserProfile from_c(const corsano_user_profile_t &p) {
  std::chrono::year_month_day birth{
      std::chrono::year{p.birth_year},
      std::chrono::month{static_cast<unsigned>(p.birth_month)},
      std::chrono::day{static_cast<unsigned>(p.birth_day)}};
  if (p.birth_month < 1 || p.birth_day < 1 || !birth.ok())
    throw std::invalid_argument("invalid birth date");
  if (p.gender > 2)
    throw std::invalid_argument("invalid gender");
  if (p.wrist > 1)
    throw std::invalid_argument("invalid wrist");
  if (p.skin < 1 || p.skin > 6)
    throw std::invalid_argument("invalid skin type");
  return {birth,
          static_cast<Gender>(p.gender),
          static_cast<Wrist>(p.wrist),
          static_cast<SkinType>(p.skin),
          p.height_cm,
          p.weight_kg};
}

corsano_battery_t to_c(const BatteryLevel &b) {
  return {b.percent, b.millivolts, b.charging};
}

corsano_realtime_t to_c(const RealtimePing &p) {
  return {p.timestamp_s,
          p.heart_rate,
          p.heart_rate_quality,
          p.respiration_rate,
          p.respiration_rate_quality,
          p.spo2,
          p.spo2_quality,
          p.core_body_temp_c,
          p.movement_level,
          p.battery_percent};
}

corsano_live_hrm_t to_c(const LiveHrm &h) {
  return {h.heart_rate,   h.heart_rate_quality, h.steps,
          h.energy,       h.max_heart_rate,     h.speed,
          h.duration,     h.battery_percent,    h.average_heart_rate,
          h.average_speed};
}

corsano_regular_ping_t to_c(const RegularPing &p) {
  return {p.battery_percent, p.ppg_size,   p.activity_size, p.hrv_size,
          p.workout_size,    p.sleep_size, p.log_size};
}

corsano_stress_t to_c(const StressRecord &r) {
  return {r.timestamp_s,
          r.cognitive_zone,
          r.cognitive_zone_quality,
          r.predicted_cognitive_zone,
          r.predicted_transition_time,
          r.cognitive_zone_hour,
          r.cortisol_contribution,
          r.skin_conductance_stress,
          r.skin_conductance_stress_quality};
}

corsano_activity_t to_c(const ActivityRecord &r) {
  return {r.timestamp_s,
          r.heart_rate,
          r.heart_rate_quality,
          r.steps,
          r.activity_type,
          r.activity_count,
          r.spo2,
          r.spo2_quality,
          r.energy,
          r.respiration_rate,
          r.respiration_rate_quality,
          r.battery_percent,
          r.charging,
          r.nibp_systolic,
          r.nibp_diastolic,
          r.temp1_c,
          r.temp2_c,
          r.wearing};
}

// Copies the {fn,user} pair under the lock, then calls outside it
template <class Fn, class Arg>
void dispatch(corsano_t *obj, Fn corsano::*fn, void *corsano::*user,
              const Arg &arg) {
  Fn f;
  void *u;
  {
    std::lock_guard<std::mutex> lock(obj->cb_mutex);
    f = obj->*fn;
    u = obj->*user;
  }
  if (f)
    f(&arg, u);
}

template <class Fn>
void set_callback(corsano_t *obj, Fn corsano::*fn, void *corsano::*user, Fn cb,
                  void *u) {
  if (!obj)
    return;
  std::lock_guard<std::mutex> lock(obj->cb_mutex);
  obj->*fn = cb;
  obj->*user = u;
}

// Shared by the corsano_decode_* functions: `decode()` returns
// {rows, consumed_bytes}; copies what fits, flags a too-small buffer.
template <class Row, class Decode>
int decode_rows(const uint8_t *data, size_t size, Row *out, size_t capacity,
                size_t *count, size_t *consumed, Decode decode) {
  if (!count || !consumed || (!data && size) || (!out && capacity))
    return bad_argument("NULL argument");
  *count = 0;
  *consumed = 0;
  bool truncated = false;
  int rc = guard([&] {
    auto [rows, used] = decode();
    size_t n = std::min(rows.size(), capacity);
    std::copy_n(rows.begin(), n, out);
    *count = n;
    *consumed = used;
    truncated = rows.size() > capacity;
  });
  return rc == CORSANO_OK && truncated ? bad_argument("buffer too small") : rc;
}

// MARK: - Monitor helpers

// Drains into mon->buf (via `fill`) and copies to the caller's array
template <class F>
int drain(corsano_monitor_t *mon, corsano_sample_t *samples, size_t capacity,
          size_t *count, F &&fill) {
  if (!count)
    return bad_argument("NULL argument");
  *count = 0;
  if (!mon || (!samples && capacity))
    return bad_argument("NULL argument");
  return guard([&] {
    mon->buf.clear();
    if (capacity)
      fill(mon->buf);
    for (const auto &s : mon->buf)
      samples[(*count)++] = {s.time, s.field, s.value, s.record};
  });
}

} // namespace

extern "C" {

// MARK: - Status

const char *corsano_last_error(void) { return LastError.c_str(); }

// MARK: - Discovery

int corsano_scan(int duration_ms, corsano_device_t *devices, size_t capacity,
                 size_t *count) {
  if (!count || (!devices && capacity))
    return bad_argument("NULL argument");
  *count = 0;
  return guard([&] {
    auto found = Bracelet::scan(std::chrono::milliseconds(duration_ms));
    for (const auto &d : found) {
      if (*count == capacity)
        break;
      corsano_device_t &out = devices[(*count)++];
      copy_string(out.name, sizeof out.name, d.name);
      copy_string(out.address, sizeof out.address, d.address);
      out.rssi = d.rssi;
    }
  });
}

// MARK: - Lifecycle

corsano_t *corsano_init(const char *address) {
  corsano_t *obj = nullptr;
  if (!address) {
    bad_argument("NULL address");
    return nullptr;
  }
  int rc = guard([&] {
    obj = new corsano(address);
    obj->bracelet.set_notification_callback([obj](const Notification &n) {
      std::visit(
          [obj](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, RealtimePing>)
              dispatch(obj, &corsano::realtime_fn, &corsano::realtime_user,
                       to_c(v));
            else if constexpr (std::is_same_v<T, LiveHrm>)
              dispatch(obj, &corsano::live_hrm_fn, &corsano::live_hrm_user,
                       to_c(v));
            else if constexpr (std::is_same_v<T, RegularPing>)
              dispatch(obj, &corsano::regular_ping_fn,
                       &corsano::regular_ping_user, to_c(v));
            else
              dispatch(obj, &corsano::battery_fn, &corsano::battery_user,
                       to_c(v));
          },
          n);
    });
    obj->bracelet.set_disconnect_callback([obj] {
      corsano_disconnect_cb f;
      void *u;
      {
        std::lock_guard<std::mutex> lock(obj->cb_mutex);
        f = obj->disconnect_fn;
        u = obj->disconnect_user;
      }
      if (f)
        f(u);
    });
  });
  if (rc != CORSANO_OK) {
    delete obj;
    return nullptr;
  }
  return obj;
}

void corsano_free(corsano_t *obj) {
  if (!obj)
    return;
  try {
    // No user callback may run once the caller considers obj gone
    obj->bracelet.set_notification_callback({});
    obj->bracelet.set_disconnect_callback({});
    obj->bracelet.disconnect();
    delete obj;
  } catch (...) {
  }
}

// MARK: - Connection

int corsano_connect(corsano_t *obj, int scan_timeout_ms) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] {
    obj->bracelet.connect(std::chrono::milliseconds(scan_timeout_ms));
  });
}

void corsano_disconnect(corsano_t *obj) {
  if (!obj)
    return;
  guard([&] { obj->bracelet.disconnect(); });
}

bool corsano_is_connected(const corsano_t *obj) {
  if (!obj)
    return false;
  try {
    return obj->bracelet.is_connected();
  } catch (...) {
    return false;
  }
}

// MARK: - Raw commands

int corsano_command(corsano_t *obj, uint8_t id, const uint8_t *payload,
                    size_t payload_size, uint8_t *response, size_t capacity,
                    size_t *size, int timeout_ms) {
  if (!obj || !size || (!payload && payload_size) || (!response && capacity))
    return bad_argument("NULL argument");
  int rc = CORSANO_OK;
  int g = guard([&] {
    auto timeout = timeout_ms <= 0 ? Bracelet::DefaultTimeout
                                   : std::chrono::milliseconds(timeout_ms);
    auto out = obj->bracelet.command(id, {payload, payload_size}, timeout);
    size_t n = std::min(out.size(), capacity);
    if (n)
      std::memcpy(response, out.data(), n);
    *size = out.size();
    if (out.size() > capacity)
      rc = CORSANO_ERR_ARGUMENT;
  });
  return g != CORSANO_OK    ? g
         : rc == CORSANO_OK ? g
                            : bad_argument("buffer too small");
}

// MARK: - Device info

int corsano_battery(corsano_t *obj, corsano_battery_t *out) {
  if (!obj || !out)
    return bad_argument("NULL argument");
  return guard([&] { *out = to_c(obj->bracelet.battery()); });
}

int corsano_firmware_version(corsano_t *obj, corsano_firmware_t *out) {
  if (!obj || !out)
    return bad_argument("NULL argument");
  return guard([&] {
    auto v = obj->bracelet.firmware_version();
    *out = {v.major, v.minor};
  });
}

int corsano_serial_number(corsano_t *obj, char *out, size_t capacity) {
  if (!obj || !out || !capacity)
    return bad_argument("NULL argument");
  return guard(
      [&] { copy_string(out, capacity, obj->bracelet.serial_number()); });
}

int corsano_set_time(corsano_t *obj) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] { obj->bracelet.set_time(); });
}

// MARK: - Authentication

int corsano_set_host_id(corsano_t *obj, const char *host_id) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] {
    if (host_id && *host_id) // NULL/"" keeps the host-name default
      obj->bracelet.set_host_id(host_id);
  });
}

int corsano_authenticated(corsano_t *obj, bool *out) {
  if (!obj || !out)
    return bad_argument("NULL argument");
  return guard([&] { *out = obj->bracelet.authenticated(); });
}

// MARK: - Plan

int corsano_plan(corsano_t *obj, uint8_t *plan, uint8_t *ppg2_rate,
                 uint8_t *ppg1_rate) {
  if (!obj || !plan || !ppg2_rate || !ppg1_rate)
    return bad_argument("NULL argument");
  return guard([&] {
    auto p = obj->bracelet.plan();
    *plan = static_cast<uint8_t>(p.plan);
    *ppg2_rate = static_cast<uint8_t>(p.ppg2_rate);
    *ppg1_rate = p.ppg1_rate;
  });
}

int corsano_set_plan(corsano_t *obj, uint8_t plan, uint8_t ppg2_rate,
                     uint8_t ppg1_rate) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] {
    obj->bracelet.set_plan(
        {static_cast<Plan>(plan), static_cast<Ppg2Rate>(ppg2_rate), ppg1_rate});
  });
}

// MARK: - User profile and calibration

int corsano_user_profile(corsano_t *obj, corsano_user_profile_t *out) {
  if (!obj || !out)
    return bad_argument("NULL argument");
  return guard([&] {
    auto p = obj->bracelet.user_profile();
    out->birth_year = static_cast<int>(p.birth.year());
    out->birth_month = static_cast<int>(static_cast<unsigned>(p.birth.month()));
    out->birth_day = static_cast<int>(static_cast<unsigned>(p.birth.day()));
    out->gender = static_cast<uint8_t>(p.gender);
    out->wrist = static_cast<uint8_t>(p.wrist);
    out->skin = static_cast<uint8_t>(p.skin);
    out->height_cm = p.height_cm;
    out->weight_kg = p.weight_kg;
  });
}

int corsano_set_user_profile(corsano_t *obj,
                             const corsano_user_profile_t *profile) {
  if (!obj || !profile)
    return bad_argument("NULL argument");
  return guard([&] { obj->bracelet.set_user_profile(from_c(*profile)); });
}

int corsano_start_wearing_optimization(corsano_t *obj) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] { obj->bracelet.start_wearing_optimization(); });
}

int corsano_wearing_optimization_status(corsano_t *obj,
                                        corsano_wearing_optimization_t *out) {
  if (!obj || !out)
    return bad_argument("NULL argument");
  return guard([&] {
    auto s = obj->bracelet.wearing_optimization_status();
    *out = {s.finished, s.progress_percent, s.movement_level, s.quality,
            s.success()};
  });
}

int corsano_stop_wearing_optimization(corsano_t *obj) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] { obj->bracelet.stop_wearing_optimization(); });
}

// MARK: - Live vitals

int corsano_set_realtime_enabled(corsano_t *obj, bool enabled) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] { obj->bracelet.set_realtime_enabled(enabled); });
}

int corsano_realtime_enabled(corsano_t *obj, bool *out) {
  if (!obj || !out)
    return bad_argument("NULL argument");
  return guard([&] { *out = obj->bracelet.realtime_enabled(); });
}

int corsano_set_realtime_period(corsano_t *obj, int seconds) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] {
    obj->bracelet.set_realtime_period(std::chrono::seconds(seconds));
  });
}

int corsano_realtime_period(corsano_t *obj, int *seconds) {
  if (!obj || !seconds)
    return bad_argument("NULL argument");
  return guard([&] {
    *seconds = static_cast<int>(obj->bracelet.realtime_period().count());
  });
}

int corsano_set_vital_sampling_rate(corsano_t *obj, uint8_t vital,
                                    int seconds) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] {
    obj->bracelet.set_vital_sampling_rate(static_cast<VitalType>(vital),
                                          std::chrono::seconds(seconds));
  });
}

int corsano_vital_sampling_rate(corsano_t *obj, uint8_t vital, int *seconds) {
  if (!obj || !seconds)
    return bad_argument("NULL argument");
  return guard([&] {
    *seconds = static_cast<int>(
        obj->bracelet.vital_sampling_rate(static_cast<VitalType>(vital))
            .count());
  });
}

int corsano_set_workout_mode(corsano_t *obj, bool enabled) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard([&] { obj->bracelet.set_workout_mode(enabled); });
}

// MARK: - Files

int corsano_file_size(corsano_t *obj, uint8_t file, uint32_t *size) {
  if (!obj || !size)
    return bad_argument("NULL argument");
  return guard(
      [&] { *size = obj->bracelet.file_size(static_cast<FileId>(file)); });
}

int corsano_download_file(corsano_t *obj, uint8_t file, uint8_t *data,
                          size_t capacity, size_t *size) {
  if (!obj || !size || (!data && capacity))
    return bad_argument("NULL argument");
  return guard([&] {
    auto max_bytes = static_cast<uint32_t>(
        std::min<size_t>(capacity, std::numeric_limits<uint32_t>::max()));
    auto out =
        obj->bracelet.download_file(static_cast<FileId>(file), max_bytes);
    size_t n = std::min(out.size(), capacity); // backend honors max_bytes
    if (n)
      std::memcpy(data, out.data(), n);
    *size = n;
  });
}

int corsano_erase_file(corsano_t *obj, uint8_t file, uint32_t bytes) {
  if (!obj)
    return bad_argument("NULL handle");
  return guard(
      [&] { obj->bracelet.erase_file(static_cast<FileId>(file), bytes); });
}

int corsano_decode_activity(const uint8_t *data, size_t size,
                            corsano_activity_t *records, size_t capacity,
                            size_t *count, size_t *consumed) {
  return decode_rows(data, size, records, capacity, count, consumed, [&] {
    auto r = decode_activity({data, size});
    std::vector<corsano_activity_t> rows;
    for (const auto &rec : r.records)
      rows.push_back(to_c(rec));
    return std::pair{std::move(rows), r.consumed_bytes};
  });
}

int corsano_decode_stress(const uint8_t *data, size_t size,
                          corsano_stress_t *records, size_t capacity,
                          size_t *count, size_t *consumed) {
  return decode_rows(data, size, records, capacity, count, consumed, [&] {
    auto r = decode_stress({data, size});
    std::vector<corsano_stress_t> rows;
    for (const auto &rec : r.records)
      rows.push_back(to_c(rec));
    return std::pair{std::move(rows), r.consumed_bytes};
  });
}

int corsano_decode_ppg(const uint8_t *data, size_t size,
                       corsano_ppg_sample_t *samples, size_t capacity,
                       size_t *count, size_t *consumed) {
  return decode_rows(data, size, samples, capacity, count, consumed, [&] {
    auto r = decode_ppg({data, size});
    std::vector<corsano_ppg_sample_t> rows;
    for (const auto &c : r.records)
      for (size_t i = 0; i < c.samples.size(); ++i) {
        const auto &s = c.samples[i];
        size_t h =
            std::min(i / PpgSamplesPerHeartRate, c.heart_rate.size() - 1);
        rows.push_back({c.timestamp_ms + i * PpgSamplePeriod.count(), c.index,
                        s.ppg, s.acc, s.acc_x, s.acc_y, s.acc_z,
                        c.heart_rate[h], c.heart_rate_quality[h],
                        c.protocol_version});
      }
    return std::pair{std::move(rows), r.consumed_bytes};
  });
}

// MARK: - Monitor

void corsano_monitor_config_default(corsano_monitor_config_t *cfg) {
  if (!cfg)
    return;
  *cfg = {};
  cfg->erase = true;
  cfg->set_time = true;
  cfg->plan = -1;
  cfg->ppg2_rate = 1;
  cfg->ppg1_rate = 60;
  cfg->max_queue = 100000;
}

corsano_monitor_t *corsano_monitor_init(const char *device,
                                        const corsano_monitor_config_t *cfg) {
  if (!device) {
    bad_argument("NULL device");
    return nullptr;
  }
  corsano_monitor_t *mon = nullptr;
  int rc = guard([&] {
    MonitorConfig c;
    if (cfg) {
      if (cfg->files) {
        c.files.clear();
        for (size_t i = 0; i < cfg->n_files; ++i)
          c.files.push_back(static_cast<FileId>(cfg->files[i]));
      }
      c.live_hr = cfg->live_hr;
      c.erase = cfg->erase;
      c.set_time = cfg->set_time;
      if (cfg->plan >= 0)
        c.plan =
            PlanSettings{static_cast<Plan>(cfg->plan),
                         static_cast<Ppg2Rate>(cfg->ppg2_rate), cfg->ppg1_rate};
      if (cfg->profile)
        c.profile = from_c(*cfg->profile);
      if (cfg->host_id)
        c.host_id = cfg->host_id;
      c.interval = std::chrono::milliseconds(cfg->interval_ms);
      c.max_queue = cfg->max_queue;
    }
    mon = new corsano_monitor(device, std::move(c));
  });
  return rc == CORSANO_OK ? mon : nullptr;
}

void corsano_monitor_free(corsano_monitor_t *mon) {
  if (!mon)
    return;
  try {
    mon->monitor.stop();
    delete mon;
  } catch (...) {
  }
}

int corsano_monitor_start(corsano_monitor_t *mon) {
  if (!mon)
    return bad_argument("NULL handle");
  return guard([&] { mon->monitor.start(); });
}

int corsano_monitor_sync_clock(corsano_monitor_t *mon) {
  if (!mon)
    return bad_argument("NULL handle");
  return guard([&] { mon->monitor.sync_clock(); });
}

void corsano_monitor_stop(corsano_monitor_t *mon) {
  if (!mon)
    return;
  guard([&] { mon->monitor.stop(); });
}

bool corsano_monitor_running(const corsano_monitor_t *mon) {
  if (!mon)
    return false;
  try {
    return mon->monitor.running();
  } catch (...) {
    return false;
  }
}

int corsano_monitor_last_error(const corsano_monitor_t *mon, char *out,
                               size_t capacity) {
  if (!mon || !out || !capacity)
    return bad_argument("NULL argument");
  return guard([&] { copy_string(out, capacity, mon->monitor.last_error()); });
}

int corsano_monitor_poll(corsano_monitor_t *mon, corsano_sample_t *samples,
                         size_t capacity, size_t *count) {
  return drain(mon, samples, capacity, count,
               [&](std::vector<Sample> &v) { mon->monitor.poll(v, capacity); });
}

int corsano_monitor_wait(corsano_monitor_t *mon, corsano_sample_t *samples,
                         size_t capacity, size_t *count, int timeout_ms) {
  return drain(mon, samples, capacity, count, [&](std::vector<Sample> &v) {
    mon->monitor.wait(v, std::chrono::milliseconds(timeout_ms), capacity);
  });
}

// MARK: - Callbacks registration

void corsano_set_realtime_callback(corsano_t *obj, corsano_realtime_cb cb,
                                   void *user) {
  set_callback(obj, &corsano::realtime_fn, &corsano::realtime_user, cb, user);
}

void corsano_set_live_hrm_callback(corsano_t *obj, corsano_live_hrm_cb cb,
                                   void *user) {
  set_callback(obj, &corsano::live_hrm_fn, &corsano::live_hrm_user, cb, user);
}

void corsano_set_regular_ping_callback(corsano_t *obj,
                                       corsano_regular_ping_cb cb, void *user) {
  set_callback(obj, &corsano::regular_ping_fn, &corsano::regular_ping_user, cb,
               user);
}

void corsano_set_battery_callback(corsano_t *obj, corsano_battery_cb cb,
                                  void *user) {
  set_callback(obj, &corsano::battery_fn, &corsano::battery_user, cb, user);
}

void corsano_set_disconnect_callback(corsano_t *obj, corsano_disconnect_cb cb,
                                     void *user) {
  set_callback(obj, &corsano::disconnect_fn, &corsano::disconnect_user, cb,
               user);
}

} // extern "C"
