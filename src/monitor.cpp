#include "corsano/monitor.hpp"

#include "corsano/bracelet.hpp"
#include "corsano/files.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

namespace Corsano {

using Clock = std::chrono::steady_clock;

static double unix_now() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static bool iequals(const std::string &a, const std::string &b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower((unsigned char)x) ==
                  std::tolower((unsigned char)y);
         });
}

// MARK: - Impl

struct Monitor::Impl {
  std::string device;
  MonitorConfig config;
  std::optional<Bracelet> bracelet; // replaced only by the worker
  // Held while the worker swaps `bracelet` and while other threads use it
  // (sync_clock); the worker's own use needs no lock
  std::mutex bracelet_mutex;
  std::thread worker;
  std::atomic<bool> stop_flag{false};
  std::atomic<bool> disconnected{false};
  std::atomic<uint32_t> records{0};
  mutable std::mutex mutex; // guards queue, dropped, reported, last_error

  void reset_bracelet() {
    std::lock_guard lock(bracelet_mutex);
    bracelet.reset();
  }
  std::condition_variable cv;
  std::deque<Sample> queue;
  uint64_t dropped = 0, reported = 0;
  std::string error;
  std::map<FileId, double> newest; // --keep style dedupe (worker only)

  uint32_t next_record() { return records.fetch_add(1); }

  void set_error(const std::string &e) {
    std::lock_guard lock(mutex);
    error = e;
  }

  void push_locked(const Sample &s) {
    size_t cap = std::max<size_t>(config.max_queue, 1);
    if (queue.size() >= cap) {
      queue.pop_front();
      ++dropped;
    }
    queue.push_back(s);
  }

  void push(const std::vector<Sample> &v) {
    if (v.empty())
      return;
    {
      std::lock_guard lock(mutex);
      for (auto &s : v)
        push_locked(s);
    }
    cv.notify_all();
  }

  void push_one(double t, const char *field, double value, uint32_t rec) {
    push({Sample{t, field, value, rec}});
  }

  void push_status() {
    uint64_t d;
    {
      std::lock_guard lock(mutex);
      if (dropped == reported)
        return;
      reported = d = dropped;
    }
    push_one(unix_now(), "status.dropped", double(d), next_record());
  }

  // Interruptible sleep; true if stop was requested
  bool sleep(std::chrono::milliseconds d) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, d, [&] { return stop_flag.load(); });
  }

  // MARK: Connection

  bool connect() {
    std::string address;
    try {
      for (auto &d : Bracelet::scan(std::chrono::seconds(5)))
        if (iequals(d.address, device) || d.name == device) {
          address = d.address;
          break;
        }
    } catch (const std::exception &e) {
      set_error(e.what());
      return false;
    }
    if (address.empty()) {
      set_error("not found: " + device);
      return false;
    }
    try {
      reset_bracelet();
      disconnected = false;
      {
        std::lock_guard lock(bracelet_mutex);
        bracelet.emplace(address);
      }
      if (!config.host_id.empty())
        bracelet->set_host_id(config.host_id);
      bracelet->connect();
    } catch (const std::exception &e) {
      set_error(e.what());
      reset_bracelet();
      return false;
    }
    bracelet->set_disconnect_callback([this] {
      disconnected = true;
      cv.notify_all();
    });
    bracelet->set_notification_callback([this](const Notification &n) {
      if (auto *p = std::get_if<LiveHrm>(&n)) {
        double t = unix_now();
        uint32_t r = next_record();
        push({{t, "hrm.hr", double(p->heart_rate), r},
              {t, "hrm.hr_q", double(p->heart_rate_quality), r},
              {t, "hrm.steps", double(p->steps), r},
              {t, "hrm.energy", double(p->energy), r},
              {t, "hrm.avg_hr", double(p->average_heart_rate), r},
              {t, "hrm.battery", double(p->battery_percent), r}});
      }
    });
    auto step = [&](auto &&fn) {
      try {
        fn();
      } catch (const std::exception &e) {
        set_error(e.what());
      }
    };
    if (config.set_time)
      step([&] { bracelet->set_time(); });
    if (config.profile)
      step([&] { bracelet->set_user_profile(*config.profile); });
    if (config.plan)
      step([&] { bracelet->set_plan(*config.plan); });
    if (config.live_hr)
      step([&] { bracelet->set_workout_mode(true); });
    push_one(unix_now(), "status.connected", 1, next_record());
    return true;
  }

  // MARK: Decoding

  using Out = std::vector<Sample>;

  size_t decode(FileId f, const std::vector<uint8_t> &data, Out &out) {
    bool keep = !config.erase;
    double &after = newest.try_emplace(f, -1.0).first->second;
    double top = after;
    auto add = [&](double t, const char *field, double v, uint32_t r) {
      out.push_back({t, field, v, r});
    };
    auto fresh = [&](double t) {
      top = std::max(top, t);
      return !keep || t > after;
    };
    size_t consumed = data.size();
    switch (f) {
    case FileId::Activity: {
      auto d = decode_activity(data);
      consumed = d.consumed_bytes;
      for (auto &r : d.records) {
        double t = r.timestamp_s;
        if (!fresh(t))
          continue;
        uint32_t id = next_record();
        add(t, "activity.hr", r.heart_rate, id);
        add(t, "activity.hr_q", r.heart_rate_quality, id);
        add(t, "activity.spo2", r.spo2, id);
        add(t, "activity.spo2_q", r.spo2_quality, id);
        add(t, "activity.resp", r.respiration_rate, id);
        add(t, "activity.resp_q", r.respiration_rate_quality, id);
        add(t, "activity.steps", r.steps, id);
        add(t, "activity.temp1", r.temp1_c, id);
        add(t, "activity.temp2", r.temp2_c, id);
        add(t, "activity.battery", r.battery_percent, id);
        add(t, "activity.charging", r.charging, id);
        add(t, "activity.wearing", r.wearing, id);
      }
      break;
    }
    case FileId::Stress: {
      auto d = decode_stress(data);
      consumed = d.consumed_bytes;
      for (auto &r : d.records) {
        double t = r.timestamp_s;
        if (!fresh(t))
          continue;
        uint32_t id = next_record();
        add(t, "stress.cognitive_zone", r.cognitive_zone, id);
        add(t, "stress.cognitive_zone_q", r.cognitive_zone_quality, id);
        add(t, "stress.predicted_zone", r.predicted_cognitive_zone, id);
        add(t, "stress.predicted_transition", r.predicted_transition_time, id);
        add(t, "stress.zone_hour", r.cognitive_zone_hour, id);
        add(t, "stress.cortisol", r.cortisol_contribution, id);
        add(t, "stress.sc_stress", r.skin_conductance_stress, id);
        add(t, "stress.sc_stress_q", r.skin_conductance_stress_quality, id);
      }
      break;
    }
    case FileId::Ppg: {
      auto d = decode_ppg(data);
      consumed = d.consumed_bytes;
      for (auto &c : d.records) {
        if (keep && c.timestamp_ms / 1000.0 <= after)
          continue;
        for (size_t i = 0; i < c.samples.size(); ++i) {
          auto &s = c.samples[i];
          double t = (c.timestamp_ms + PpgSamplePeriod.count() * i) / 1000.0;
          top = std::max(top, t);
          uint32_t id = next_record();
          add(t, "ppg.ppg", s.ppg, id);
          add(t, "ppg.acc", s.acc, id);
          if (c.protocol_version == 3) {
            add(t, "ppg.acc_x", s.acc_x, id);
            add(t, "ppg.acc_y", s.acc_y, id);
            add(t, "ppg.acc_z", s.acc_z, id);
          }
          if (i % PpgSamplesPerHeartRate == 0) {
            size_t k = std::min<size_t>(i / PpgSamplesPerHeartRate, 2);
            add(t, "ppg.hr", c.heart_rate[k], id);
            add(t, "ppg.hr_q", c.heart_rate_quality[k], id);
          }
        }
      }
      break;
    }
    case FileId::Bioz: {
      auto d = decode_bioz(data);
      consumed = d.consumed_bytes;
      for (auto &c : d.records) {
        if (keep && c.timestamp_ms / 1000.0 <= after)
          continue;
        for (size_t i = 0; i < c.samples.size(); ++i) {
          double t = c.timestamp_ms / 1000.0 + i / BiozSampleRateHz;
          top = std::max(top, t);
          add(t, "bioz.z", c.samples[i], next_record());
        }
      }
      break;
    }
    case FileId::Acc: {
      auto d = decode_acc(data);
      consumed = d.consumed_bytes;
      for (auto &c : d.records) {
        if (keep && c.timestamp_ms / 1000.0 <= after)
          continue;
        for (size_t i = 0; i < c.samples.size(); ++i) {
          double t = c.timestamp_ms / 1000.0 + i / AccSampleRateHz;
          top = std::max(top, t);
          uint32_t id = next_record();
          add(t, "acc.x", c.samples[i][0], id);
          add(t, "acc.y", c.samples[i][1], id);
          add(t, "acc.z", c.samples[i][2], id);
        }
      }
      break;
    }
    default:
      break;
    }
    after = top;
    return consumed;
  }

  // MARK: Polling

  // Returns true if anything was downloaded
  bool pass() {
    bool downloaded = false;
    for (FileId f : config.files) {
      if (stop_flag || disconnected)
        break;
      try {
        uint32_t n = bracelet->file_size(f);
        if (n == 0)
          continue;
        auto data = bracelet->download_file(f, n);
        downloaded = true;
        if (config.raw_sink)
          config.raw_sink(f, data);
        Out out;
        size_t consumed = decode(f, data, out);
        push(out);
        // Never erase undecoded data unless a sink has kept it
        bool decoded = f == FileId::Activity || f == FileId::Stress ||
                       f == FileId::Ppg || f == FileId::Bioz ||
                       f == FileId::Acc;
        if (!decoded && !config.raw_sink)
          consumed = 0;
        if (config.erase && consumed > 0)
          bracelet->erase_file(f, uint32_t(consumed));
      } catch (const std::exception &e) {
        set_error(e.what());
      }
    }
    push_status();
    return downloaded;
  }

  void run() {
    static const int backoff_s[] = {1, 2, 5, 10, 30};
    size_t failures = 0;
    while (!stop_flag) {
      if (!bracelet || disconnected || !bracelet->is_connected()) {
        if (bracelet && disconnected) // was connected: report the loss
          push_one(unix_now(), "status.connected", 0, next_record());
        reset_bracelet();
        if (!connect()) {
          if (sleep(std::chrono::seconds(
                  backoff_s[std::min(failures++, std::size(backoff_s) - 1)])))
            break;
          continue;
        }
        failures = 0;
      }
      auto start = Clock::now();
      bool downloaded = pass();
      auto elapsed = Clock::now() - start;
      auto pause = std::max<Clock::duration>(
          config.interval -
              std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
          downloaded ? std::chrono::milliseconds(0)
                     : std::chrono::milliseconds(1000));
      if (sleep(std::chrono::duration_cast<std::chrono::milliseconds>(pause)))
        break;
    }
    if (bracelet) {
      try {
        if (bracelet->is_connected() && config.live_hr)
          bracelet->set_workout_mode(false);
      } catch (...) {
      }
      try {
        bracelet->disconnect();
      } catch (...) {
      }
      reset_bracelet();
    }
  }
};

// MARK: - Lifecycle

Monitor::Monitor(std::string device, MonitorConfig config)
    : _impl(std::make_unique<Impl>()) {
  _impl->device = std::move(device);
  _impl->config = std::move(config);
}

Monitor::~Monitor() {
  try {
    stop();
  } catch (...) {
  }
}

void Monitor::start() {
  if (running())
    throw std::logic_error("Monitor already running");
  if (_impl->worker.joinable()) // finished but not joined
    _impl->worker.join();
  _impl->stop_flag = false;
  _impl->worker = std::thread([this] { _impl->run(); });
}

void Monitor::stop() {
  {
    std::lock_guard lock(_impl->mutex);
    _impl->stop_flag = true;
  }
  _impl->cv.notify_all();
  if (_impl->worker.joinable())
    _impl->worker.join();
}

bool Monitor::running() const {
  return _impl->worker.joinable() && !_impl->stop_flag;
}

void Monitor::sync_clock() {
  std::lock_guard lock(_impl->bracelet_mutex);
  auto &b = _impl->bracelet;
  if (!b || !b->is_connected())
    throw std::runtime_error("sync_clock: bracelet not connected");
  b->set_time();
}

std::string Monitor::last_error() const {
  std::lock_guard lock(_impl->mutex);
  return _impl->error;
}

// MARK: - Reading

size_t Monitor::poll(std::vector<Sample> &out, size_t max) {
  std::lock_guard lock(_impl->mutex);
  size_t n = std::min(max, _impl->queue.size());
  auto end = _impl->queue.begin() + n;
  out.insert(out.end(), std::make_move_iterator(_impl->queue.begin()),
             std::make_move_iterator(end));
  _impl->queue.erase(_impl->queue.begin(), end);
  return n;
}

size_t Monitor::wait(std::vector<Sample> &out,
                     std::chrono::milliseconds timeout, size_t max) {
  {
    std::unique_lock lock(_impl->mutex);
    _impl->cv.wait_for(lock, timeout, [&] {
      return !_impl->queue.empty() || _impl->stop_flag.load();
    });
  }
  return poll(out, max);
}

} // namespace Corsano
