// Corsano bracelet over BLE, macOS (CoreBluetooth) and Linux (BlueZ).
// The BLE backend (SimpleBLE) is hidden behind a pimpl so this header has no
// third-party includes; the C wrapper builds on this class only.
#pragma once

#include "corsano/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Corsano {

struct DeviceInfo {
  std::string name;
  // MAC on Linux; CoreBluetooth per-host UUID on macOS (MAC is not exposed)
  std::string address;
  int16_t rssi;
};

struct Timeout : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// One bracelet connection. Commands are serialized (one in flight, as the
// firmware requires); all methods are blocking and thread-safe. Callbacks run
// on the BLE backend thread: keep them short and do not call back into the
// Bracelet from them.
//
// Pairing (passkey 999999) must exist before commands work:
//   macOS: the OS shows a PIN dialog on first connect.
//   Linux: pair once with `bluetoothctl pair <MAC>`.
class Bracelet {
public:
  using NotificationCallback = std::function<void(const Notification &)>;
  using ProgressCallback =
      std::function<void(size_t received_bytes, size_t total_bytes)>;
  using DisconnectCallback = std::function<void()>;

  static constexpr std::chrono::milliseconds DefaultTimeout{15000};

  // MARK: - Discovery

  // Bracelets advertising the Corsano service, first BLE adapter
  static std::vector<DeviceInfo>
  scan(std::chrono::milliseconds duration = std::chrono::seconds(5));

  // MARK: - Lifecycle

  explicit Bracelet(std::string address);
  ~Bracelet(); // disconnects
  Bracelet(const Bracelet &) = delete;
  Bracelet &operator=(const Bracelet &) = delete;
  Bracelet(Bracelet &&) noexcept;
  Bracelet &operator=(Bracelet &&) noexcept;

  // MARK: - Connection

  // Scans until the address is seen (or scan_timeout), connects, subscribes
  // to DATA_INFO and TX. Throws Timeout / std::runtime_error.
  void connect(std::chrono::milliseconds scan_timeout = std::chrono::seconds(10));
  void disconnect();
  bool is_connected() const;
  const std::string &address() const;

  // MARK: - Raw commands

  // Returns the response payload (id and CRC stripped).
  // Throws Timeout, PairingRequired, ProtocolError (bad CRC).
  std::vector<uint8_t> command(uint8_t id,
                               std::span<const uint8_t> payload = {},
                               std::chrono::milliseconds timeout = DefaultTimeout);

  // MARK: - Device info

  BatteryLevel battery();
  FirmwareVersion firmware_version();
  std::string serial_number();
  void set_time(); // host clock and local UTC offset

  // MARK: - Plan
  // Selects what the bracelet measures and how often (see Plan). Persisted
  // on the bracelet; the Corsano app may change it on its next sync.

  PlanSettings plan();
  void set_plan(const PlanSettings &settings); // throws ProtocolError if refused

  // MARK: - User profile and calibration

  // Profile, birth date, height and weight (four commands)
  UserProfile user_profile();
  // Sets profile, and height/weight when non-zero
  void set_user_profile(const UserProfile &profile);
  // Wearing Optimization (special mode 4): tunes LED power to the wearer;
  // SpO2 depends on it. Needs plan and profile set; wearer seated or lying,
  // still, for 2-4 min. Poll the status every ~5 s until finished.
  void start_wearing_optimization();
  WearingOptimizationStatus wearing_optimization_status();
  void stop_wearing_optimization();

  // MARK: - Live vitals (HR, SpO2)
  // Setters throw ProtocolError if status != StatusOk.

  // RealtimePing notifications every `period` seconds while enabled
  void set_realtime_enabled(bool enabled);
  bool realtime_enabled();
  void set_realtime_period(std::chrono::seconds period); // 1..255 s
  std::chrono::seconds realtime_period();

  // How often the bracelet measures a vital (0 = off). SDK presets:
  // 0, 30, 60, 300, 1800 s. Drives both RealtimePing and Activity records.
  void set_vital_sampling_rate(VitalType type, std::chrono::seconds interval);
  std::chrono::seconds vital_sampling_rate(VitalType type);

  // Workout mode; LiveHrm notifications are expected while it is on
  // (unverified on hardware)
  void set_workout_mode(bool enabled);

  // MARK: - Files (history)

  // Bytes currently stored on the bracelet for that file
  uint32_t file_size(FileId file);
  // Downloads up to max_bytes (rounded down to whole records) as raw bytes.
  // Does not erase; decode with the decode_<name>() functions in files.hpp.
  std::vector<uint8_t> download_file(FileId file, uint32_t max_bytes,
                                     ProgressCallback progress = {});
  // Frees the first `bytes` of the file (use Decoded::consumed_bytes)
  void erase_file(FileId file, uint32_t bytes);

  // MARK: - Callbacks

  // Replaces any previous callback; pass {} to clear
  void set_notification_callback(NotificationCallback callback);
  void set_disconnect_callback(DisconnectCallback callback);

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace Corsano
