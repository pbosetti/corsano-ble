#include "corsano/bracelet.hpp"

// SimpleBLE's Config.h trips -Wunused-function
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <simpleble/SimpleBLE.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <optional>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace Corsano {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

SimpleBLE::Adapter first_adapter() {
  auto adapters = SimpleBLE::Adapter::get_adapters();
  if (adapters.empty())
    throw std::runtime_error("no Bluetooth adapter found");
  return adapters.front();
}

// Local UTC offset, computed from broken-down local vs UTC time so it does
// not depend on the (platform-dependent) chrono time zone database.
std::chrono::minutes
local_utc_offset(std::chrono::system_clock::time_point now) {
  using namespace std::chrono;
  std::time_t t = system_clock::to_time_t(now);
  std::tm local{}, utc{};
#ifdef _WIN32
  localtime_s(&local, &t);
  gmtime_s(&utc, &t);
#else
  localtime_r(&t, &local);
  gmtime_r(&t, &utc);
#endif
  auto secs = [](const std::tm &tm) {
    return sys_days{year{tm.tm_year + 1900} / (tm.tm_mon + 1) / tm.tm_mday}
               .time_since_epoch() +
           hours{tm.tm_hour} + minutes{tm.tm_min} + seconds{tm.tm_sec};
  };
  return duration_cast<minutes>(secs(local) - secs(utc));
}

uint8_t first_byte(const std::vector<uint8_t> &p, const char *what) {
  if (p.empty())
    throw ProtocolError(std::string(what) + ": empty response");
  return p[0];
}

void expect_ok(const std::vector<uint8_t> &p, const char *what) {
  uint8_t status = first_byte(p, what);
  if (status != StatusOk)
    throw ProtocolError(std::string(what) + " status " +
                        std::to_string(status));
}

// CORSANO_DEBUG=1 prints every received frame as hex to stderr
void debug_frame(const char *channel, const std::vector<uint8_t> &bytes) {
  static const bool enabled = std::getenv("CORSANO_DEBUG") != nullptr;
  if (!enabled)
    return;
  std::string hex;
  for (uint8_t b : bytes) {
    char buf[3];
    std::snprintf(buf, sizeof buf, "%02x", b);
    hex += buf;
  }
  std::fprintf(stderr, "[corsano] %s %s\n", channel, hex.c_str());
}

std::string default_host_id() {
  std::string name;
#ifdef _WIN32
  if (const char *c = std::getenv("COMPUTERNAME"))
    name = c;
#else
  char buf[256] = {};
  if (gethostname(buf, sizeof buf - 1) == 0)
    name = buf;
#endif
  return name.empty() ? "corsano-ble" : name;
}

bool needs_auth(FirmwareVersion v) {
  return v.major > AuthMinFirmware.major ||
         (v.major == AuthMinFirmware.major && v.minor >= AuthMinFirmware.minor);
}

} // namespace

// MARK: - Impl

struct Bracelet::Impl {
  using Clock = std::chrono::steady_clock;

  explicit Impl(std::string addr)
      : address(std::move(addr)), host_id(default_host_id()) {}

  void on_data_info(const std::vector<uint8_t> &frame);
  void on_tx(const std::vector<uint8_t> &bytes);
  void connect(std::chrono::milliseconds scan_timeout);
  void disconnect();
  bool connected() { return peripheral && peripheral->is_connected(); }
  std::vector<uint8_t> command(uint8_t id, std::span<const uint8_t> payload,
                               std::chrono::milliseconds timeout);
  // Caller must hold command_mutex
  std::vector<uint8_t> command_locked(uint8_t id,
                                      std::span<const uint8_t> payload,
                                      std::chrono::milliseconds timeout);
  void write_rx(const std::vector<uint8_t> &data) {
    peripheral->write_request(ServiceUuid, RxCharUuid,
                              SimpleBLE::ByteArray(data));
  }
  std::vector<uint8_t> download(FileId file, uint32_t max_bytes,
                                ProgressCallback progress);

  const std::string address;
  std::string host_id;            // set before connect()
  FirmwareVersion firmware{0, 0}; // read at connect()
  std::mutex command_mutex;       // serializes commands and downloads
  std::mutex mutex;               // guards everything below
  std::condition_variable cv;
  std::optional<uint8_t> pending_id;
  std::optional<std::vector<uint8_t>> response;
  bool pairing_required = false;
  bool disconnected = true;
  bool download_active = false;
  bool download_cancelled = false; // sticky until the next connect()
  std::vector<uint8_t> download_buffer;
  Clock::time_point last_tx;
  NotificationCallback notification_cb;
  DisconnectCallback disconnect_cb;
  // Declared last: destroyed first, so no callback outlives the state above.
  std::optional<SimpleBLE::Adapter> adapter;
  std::optional<SimpleBLE::Peripheral> peripheral;
};

void Bracelet::Impl::on_data_info(const std::vector<uint8_t> &frame) {
  debug_frame("data_info", frame);
  if (frame.empty())
    return;
  NotificationCallback cb;
  {
    std::lock_guard lock(mutex);
    if (pending_id && frame[0] == *pending_id) {
      response = frame;
      cv.notify_all();
      return;
    }
    // "Not bonded" while a command waits means it will never be answered.
    if (pending_id && frame[0] == uint8_t(CommandId::GetBondingState) &&
        frame.size() >= 3 && frame[1] == StatusFail) {
      pairing_required = true;
      cv.notify_all();
      return;
    }
    cb = notification_cb;
  }
  if (!cb)
    return;
  try {
    if (auto n = decode_notification(frame))
      cb(*n);
  } catch (const ProtocolError &) {
    // malformed unsolicited frame: drop
  }
}

void Bracelet::Impl::on_tx(const std::vector<uint8_t> &bytes) {
  debug_frame("tx", bytes);
  std::lock_guard lock(mutex);
  if (!download_active)
    return;
  download_buffer.insert(download_buffer.end(), bytes.begin(), bytes.end());
  last_tx = Clock::now();
  cv.notify_all();
}

void Bracelet::Impl::connect(std::chrono::milliseconds scan_timeout) {
  if (connected())
    return;
  struct ScanState {
    std::mutex m;
    std::condition_variable cv;
    std::optional<SimpleBLE::Peripheral> found;
  };
  auto state = std::make_shared<ScanState>();
  const std::string wanted = lower(address);

  adapter = first_adapter();
  adapter->set_callback_on_scan_found([state, wanted](SimpleBLE::Peripheral p) {
    if (lower(p.address()) != wanted)
      return;
    std::lock_guard lock(state->m);
    if (!state->found)
      state->found = p;
    state->cv.notify_all();
  });
  adapter->scan_start();
  std::optional<SimpleBLE::Peripheral> found;
  {
    std::unique_lock lock(state->m);
    state->cv.wait_for(lock, scan_timeout,
                       [&] { return state->found.has_value(); });
    found = state->found;
  }
  adapter->scan_stop();
  if (!found)
    throw Timeout("bracelet " + address + " not found");

  peripheral = *found;
  {
    std::lock_guard lock(mutex);
    disconnected = false;
    download_cancelled = false;
  }
  peripheral->set_callback_on_disconnected([this] {
    DisconnectCallback cb;
    {
      std::lock_guard lock(mutex);
      disconnected = true;
      cb = disconnect_cb;
      cv.notify_all();
    }
    if (cb)
      cb();
  });
  peripheral->connect();
  peripheral->notify(ServiceUuid, TxCharUuid, [this](SimpleBLE::ByteArray b) {
    on_tx(std::vector<uint8_t>(b.begin(), b.end()));
  });
  peripheral->notify(ServiceUuid, DataInfoCharUuid,
                     [this](SimpleBLE::ByteArray b) {
                       on_data_info(std::vector<uint8_t>(b.begin(), b.end()));
                     });
}

void Bracelet::Impl::disconnect() {
  if (!peripheral)
    return;
  try {
    if (peripheral->is_connected()) {
      try {
        peripheral->unsubscribe(ServiceUuid, TxCharUuid);
        peripheral->unsubscribe(ServiceUuid, DataInfoCharUuid);
      } catch (...) {
      }
      peripheral->disconnect();
    }
  } catch (...) {
  }
  std::lock_guard lock(mutex);
  disconnected = true;
  cv.notify_all();
}

std::vector<uint8_t>
Bracelet::Impl::command(uint8_t id, std::span<const uint8_t> payload,
                        std::chrono::milliseconds timeout) {
  std::lock_guard cmd_lock(command_mutex);
  return command_locked(id, payload, timeout);
}

std::vector<uint8_t>
Bracelet::Impl::command_locked(uint8_t id, std::span<const uint8_t> payload,
                               std::chrono::milliseconds timeout) {
  if (!connected())
    throw std::runtime_error("not connected");
  {
    std::lock_guard lock(mutex);
    pending_id = id;
    response.reset();
    pairing_required = false;
  }
  struct Clear {
    Impl &i;
    ~Clear() {
      std::lock_guard lock(i.mutex);
      i.pending_id.reset();
    }
  } clear{*this};

  write_rx(encode_request(id, payload));

  std::unique_lock lock(mutex);
  bool done = cv.wait_for(lock, timeout, [&] {
    return response || pairing_required || disconnected;
  });
  if (response) {
    auto frame = std::move(*response);
    lock.unlock();
    if (!check_crc(frame))
      throw ProtocolError("bad CRC in response to command " +
                          std::to_string(id));
    if (frame.size() < 2)
      return {};
    return {frame.begin() + 1, frame.end() - 1};
  }
  if (pairing_required)
    throw PairingRequired("bracelet is not bonded; pair with passkey " +
                          std::string(DefaultPasskey));
  if (!done)
    throw Timeout("timeout waiting for response to command " +
                  std::to_string(id));
  throw std::runtime_error("disconnected");
}

std::vector<uint8_t> Bracelet::Impl::download(FileId file, uint32_t max_bytes,
                                              ProgressCallback progress) {
  std::lock_guard cmd_lock(command_mutex);
  const uint8_t fid = uint8_t(file);
  const uint8_t size_req[] = {fid};
  uint32_t available = decode_file_size(
      command_locked(uint8_t(CommandId::GetFileSize), size_req, DefaultTimeout),
      file);
  uint32_t n = std::min(available, max_bytes);
  if (uint32_t rs = record_size(file))
    n -= n % rs;
  if (n == 0)
    return {};

  const uint8_t start_req[] = {fid, uint8_t(n), uint8_t(n >> 8),
                               uint8_t(n >> 16), uint8_t(n >> 24)};
  {
    std::lock_guard lock(mutex);
    download_buffer.clear();
    download_buffer.reserve(n);
    download_active = true;
    last_tx = Clock::now();
  }
  struct Stop {
    Impl &i;
    ~Stop() {
      std::lock_guard lock(i.mutex);
      i.download_active = false;
      i.download_buffer.clear();
    }
  } stop{*this};

  write_rx(encode_request(uint8_t(CommandId::StartStreamingFileWithSize),
                          start_req));

  size_t reported = 0;
  std::unique_lock lock(mutex);
  while (download_buffer.size() < n) {
    if (disconnected)
      throw std::runtime_error("disconnected");
    if (download_cancelled)
      throw std::runtime_error("download cancelled");
    if (Clock::now() - last_tx >= DefaultTimeout)
      throw Timeout("file download stalled after " +
                    std::to_string(download_buffer.size()) + " of " +
                    std::to_string(n) + " bytes");
    cv.wait_until(lock, last_tx + DefaultTimeout);
    size_t got = std::min<size_t>(download_buffer.size(), n);
    if (progress && got != reported) {
      reported = got;
      lock.unlock();
      progress(got, n);
      lock.lock();
    }
  }
  download_buffer.resize(n);
  return std::move(download_buffer);
}

// MARK: - Discovery

void Bracelet::scan(const std::function<void(const DeviceInfo &)> &on_found,
                    const std::function<bool()> &stop) {
  // shared: the backend may still hold the callback after we return
  struct ScanState {
    std::mutex m;
    bool active = true;
    std::vector<std::string> seen;
    std::function<void(const DeviceInfo &)> on_found;
  };
  auto state = std::make_shared<ScanState>();
  state->on_found = on_found;

  auto adapter = first_adapter();
  // The service UUID may only come with a later advertisement or scan
  // response, so check updates too (seen dedupes)
  auto check = [state](SimpleBLE::Peripheral p) {
    auto services = p.services();
    if (std::none_of(services.begin(), services.end(), [](auto &s) {
          return lower(s.uuid()) == ServiceUuid;
        }))
      return;
    std::lock_guard lock(state->m);
    auto addr = p.address();
    if (!state->active ||
        std::find(state->seen.begin(), state->seen.end(), addr) !=
            state->seen.end())
      return;
    state->seen.push_back(addr);
    state->on_found({p.identifier(), addr, p.rssi()});
  };
  adapter.set_callback_on_scan_found(check);
  adapter.set_callback_on_scan_updated(check);
  adapter.scan_start();
  while (!stop())
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  adapter.scan_stop();
  std::lock_guard lock(state->m);
  state->active = false;
}

std::vector<DeviceInfo> Bracelet::scan(std::chrono::milliseconds duration) {
  std::vector<DeviceInfo> out; // no lock needed: on_found calls are serialized
  auto end = std::chrono::steady_clock::now() + duration;
  scan([&](const DeviceInfo &d) { out.push_back(d); },
       [&] { return std::chrono::steady_clock::now() >= end; });
  return out;
}

// MARK: - Lifecycle

Bracelet::Bracelet(std::string address)
    : _impl(std::make_unique<Impl>(std::move(address))) {}

Bracelet::~Bracelet() {
  if (_impl)
    _impl->disconnect();
}

Bracelet::Bracelet(Bracelet &&) noexcept = default;
Bracelet &Bracelet::operator=(Bracelet &&) noexcept = default;

// MARK: - Connection

void Bracelet::connect(std::chrono::milliseconds scan_timeout) {
  if (_impl->connected())
    return;
  _impl->connect(scan_timeout);
  try {
    _impl->firmware = firmware_version(); // allowed before authentication
    if (needs_auth(_impl->firmware))
      authenticate();
  } catch (...) {
    _impl->disconnect();
    throw;
  }
}

// MARK: - Authentication

void Bracelet::set_host_id(std::string host_id) {
  _impl->host_id = std::move(host_id);
}
const std::string &Bracelet::host_id() const { return _impl->host_id; }

void Bracelet::authenticate() {
  auto payload = encode_authenticate(_impl->host_id);
  expect_ok(command(uint8_t(CommandId::Authenticate), payload), "authenticate");
}

bool Bracelet::authenticated() {
  if (!needs_auth(_impl->firmware))
    return true;
  const uint8_t zero[] = {0};
  return first_byte(command(uint8_t(CommandId::GetAuthStatus), zero),
                    "auth status") == StatusOk;
}
void Bracelet::disconnect() { _impl->disconnect(); }
bool Bracelet::is_connected() const { return _impl->connected(); }
const std::string &Bracelet::address() const { return _impl->address; }

// MARK: - Raw commands

std::vector<uint8_t> Bracelet::command(uint8_t id,
                                       std::span<const uint8_t> payload,
                                       std::chrono::milliseconds timeout) {
  return _impl->command(id, payload, timeout);
}

// MARK: - Device info

BatteryLevel Bracelet::battery() {
  return decode_battery(command(uint8_t(CommandId::GetBatteryLevel)));
}

FirmwareVersion Bracelet::firmware_version() {
  return decode_firmware_version(command(uint8_t(CommandId::GetFwVersion)));
}

std::string Bracelet::serial_number() {
  auto p = command(uint8_t(CommandId::GetSerialNumber));
  auto end = std::find(p.begin(), p.end(), uint8_t(0));
  return {p.begin(), end};
}

void Bracelet::set_time() {
  auto now = std::chrono::system_clock::now();
  auto payload = encode_set_time(now, local_utc_offset(now));
  expect_ok(command(uint8_t(CommandId::UpdateTime), payload), "set_time");
}

// MARK: - Live vitals

void Bracelet::set_realtime_enabled(bool enabled) {
  const uint8_t p[] = {uint8_t(enabled)};
  expect_ok(command(uint8_t(CommandId::SetRealtimeEnable), p),
            "set_realtime_enabled");
}

bool Bracelet::realtime_enabled() {
  return first_byte(command(uint8_t(CommandId::GetRealtimeEnable)),
                    "realtime_enabled") == 1;
}

void Bracelet::set_realtime_period(std::chrono::seconds period) {
  if (period.count() < 1 || period.count() > 255)
    throw std::invalid_argument("realtime period must be 1..255 s");
  const uint8_t p[] = {uint8_t(period.count())};
  expect_ok(command(uint8_t(CommandId::SetRealtimePeriod), p),
            "set_realtime_period");
}

std::chrono::seconds Bracelet::realtime_period() {
  return std::chrono::seconds(first_byte(
      command(uint8_t(CommandId::GetRealtimePeriod)), "realtime_period"));
}

PlanSettings Bracelet::plan() {
  return decode_plan(command(uint8_t(CommandId::GetPlan)));
}

void Bracelet::set_plan(const PlanSettings &s) {
  const uint8_t p[] = {uint8_t(s.plan), uint8_t(s.ppg2_rate), s.ppg1_rate};
  expect_ok(command(uint8_t(CommandId::SetPlan), p), "set_plan");
}

UserProfile Bracelet::user_profile() {
  auto need = [](const std::vector<uint8_t> &r, size_t n, const char *what) {
    if (r.size() < n)
      throw ProtocolError(std::string(what) + ": short response");
  };
  auto prof = command(uint8_t(CommandId::GetProfile));
  need(prof, 4, "user_profile");
  auto birth = command(uint8_t(CommandId::GetBirthdate));
  need(birth, 4, "user_profile birth date");
  UserProfile u;
  u.gender = prof[1] <= 2 ? Gender(prof[1]) : Gender::Unspecified;
  u.wrist = prof[2] <= 1 ? Wrist(prof[2]) : Wrist::Left;
  u.skin = (prof[3] >= 1 && prof[3] <= 6) ? SkinType(prof[3]) : SkinType::Fair;
  u.birth = std::chrono::year(birth[0] | (birth[1] << 8)) /
            std::chrono::month(birth[2]) / std::chrono::day(birth[3]);
  u.height_cm = first_byte(command(uint8_t(CommandId::GetHeight)), "height");
  u.weight_kg = first_byte(command(uint8_t(CommandId::GetWeight)), "weight");
  return u;
}

void Bracelet::set_user_profile(const UserProfile &profile) {
  auto today = std::chrono::year_month_day(
      std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
  auto p = encode_user_profile(profile, today);
  expect_ok(command(uint8_t(CommandId::SetProfile), p), "set_user_profile");
  if (profile.height_cm) {
    const uint8_t h[] = {profile.height_cm};
    expect_ok(command(uint8_t(CommandId::SetHeight), h), "set_height");
  }
  if (profile.weight_kg) {
    const uint8_t w[] = {profile.weight_kg};
    expect_ok(command(uint8_t(CommandId::SetWeight), w), "set_weight");
  }
}

void Bracelet::start_wearing_optimization() {
  const uint8_t p[] = {4};
  expect_ok(command(uint8_t(CommandId::StartSpecialMode), p),
            "start_wearing_optimization");
}

void Bracelet::stop_wearing_optimization() {
  const uint8_t p[] = {4};
  expect_ok(command(uint8_t(CommandId::StopSpecialMode), p),
            "stop_wearing_optimization");
}

WearingOptimizationStatus Bracelet::wearing_optimization_status() {
  const uint8_t p[] = {4, 0, 0, 0};
  return decode_wearing_optimization(
      command(uint8_t(CommandId::GetSpecialMode), p));
}

void Bracelet::set_vital_sampling_rate(VitalType type,
                                       std::chrono::seconds interval) {
  if (interval.count() < 0 || interval.count() > 0xFFFF)
    throw std::invalid_argument("sampling interval must be 0..65535 s");
  uint16_t s = uint16_t(interval.count());
  const uint8_t p[] = {uint8_t(type), uint8_t(s), uint8_t(s >> 8)};
  expect_ok(command(uint8_t(CommandId::SetVitalsSamplingRate), p),
            "set_vital_sampling_rate");
}

std::chrono::seconds Bracelet::vital_sampling_rate(VitalType type) {
  const uint8_t p[] = {uint8_t(type)};
  auto r = command(uint8_t(CommandId::GetVitalsSamplingRate), p);
  // [type echo][seconds:u16]
  if (r.size() < 3 || r[0] != uint8_t(type))
    throw ProtocolError("vital_sampling_rate: unexpected response");
  return std::chrono::seconds(r[1] | (r[2] << 8));
}

void Bracelet::set_workout_mode(bool enabled) {
  if (enabled)
    expect_ok(command(uint8_t(CommandId::SetWorkoutMode)), "set_workout_mode");
  else
    command(uint8_t(CommandId::WorkoutStop)); // stats ignored
}

// MARK: - Files (history)

uint32_t Bracelet::file_size(FileId file) {
  const uint8_t p[] = {uint8_t(file)};
  return decode_file_size(command(uint8_t(CommandId::GetFileSize), p), file);
}

std::vector<uint8_t> Bracelet::download_file(FileId file, uint32_t max_bytes,
                                             ProgressCallback progress) {
  return _impl->download(file, max_bytes, std::move(progress));
}

void Bracelet::cancel_download() {
  std::lock_guard lock(_impl->mutex);
  _impl->download_cancelled = true;
  _impl->cv.notify_all();
}

void Bracelet::erase_file(FileId file, uint32_t bytes) {
  const uint8_t p[] = {uint8_t(file), uint8_t(bytes), uint8_t(bytes >> 8),
                       uint8_t(bytes >> 16), uint8_t(bytes >> 24)};
  expect_ok(command(uint8_t(CommandId::EraseFromFile), p), "erase_file");
}

// MARK: - Callbacks

void Bracelet::set_notification_callback(NotificationCallback callback) {
  std::lock_guard lock(_impl->mutex);
  _impl->notification_cb = std::move(callback);
}

void Bracelet::set_disconnect_callback(DisconnectCallback callback) {
  std::lock_guard lock(_impl->mutex);
  _impl->disconnect_cb = std::move(callback);
}

} // namespace Corsano
