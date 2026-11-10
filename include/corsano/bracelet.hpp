/// @file bracelet.hpp
/// @brief Corsano bracelet over BLE, macOS (CoreBluetooth) and Linux (BlueZ).
///
/// The BLE backend (SimpleBLE) is hidden behind a pimpl so this header has no
/// third-party includes; the C wrapper (corsano.h) builds on this class only.
/// Wire format: protocol.hpp and docs/PROTOCOL.md.
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

/// @defgroup corsano_bracelet Bracelet connection
/// @brief BLE discovery, connection and blocking commands.
/// @{

/// @brief A bracelet found by Bracelet::scan().
struct DeviceInfo {
  std::string name; ///< Advertised name.
  /// MAC on Linux; CoreBluetooth per-host UUID on macOS (MAC is not exposed)
  std::string address;
  int16_t rssi; ///< Signal strength in dBm.
};

/// @brief A scan, command or download did not complete in time.
struct Timeout : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// @brief One bracelet connection.
///
/// Commands are serialized (one in flight, as the firmware requires); all
/// methods are blocking and thread-safe. Callbacks run on the BLE backend
/// thread: keep them short and do not call back into the Bracelet from them.
///
/// Pairing (passkey 999999, see DefaultPasskey) must exist before commands
/// work:
/// - macOS: the OS shows a PIN dialog on first connect.
/// - Linux: pair once with `bluetoothctl pair <MAC>`.
///
/// Methods that send a command may throw Timeout, PairingRequired and
/// ProtocolError (see command()), and std::runtime_error if not connected.
class Bracelet {
public:
  /// @brief Receives unsolicited messages (on the BLE backend thread).
  using NotificationCallback = std::function<void(const Notification &)>;
  /// @brief Download progress: bytes received so far and total expected.
  using ProgressCallback =
      std::function<void(size_t received_bytes, size_t total_bytes)>;
  /// @brief Called when the connection is lost (on the BLE backend thread).
  using DisconnectCallback = std::function<void()>;

  /// @brief Default timeout of command().
  static constexpr std::chrono::milliseconds DefaultTimeout{15000};

  // MARK: - Discovery
  /// @name Discovery
  /// @{

  /// @brief Find bracelets advertising the Corsano service on the first BLE adapter.
  /// @param duration How long to scan.
  /// @return The bracelets seen.
  /// @throws std::runtime_error if no Bluetooth adapter is found.
  static std::vector<DeviceInfo>
  scan(std::chrono::milliseconds duration = std::chrono::seconds(5));
  /// @}

  // MARK: - Lifecycle
  /// @name Lifecycle
  /// @{

  /// @brief Create a disconnected Bracelet.
  /// @param address MAC on Linux, CoreBluetooth UUID on macOS (see DeviceInfo::address).
  explicit Bracelet(std::string address);
  ~Bracelet(); ///< Disconnects.
  Bracelet(const Bracelet &) = delete;
  Bracelet &operator=(const Bracelet &) = delete;
  /// @brief Move constructor.
  Bracelet(Bracelet &&) noexcept;
  /// @brief Move assignment.
  Bracelet &operator=(Bracelet &&) noexcept;
  /// @}

  // MARK: - Connection
  /// @name Connection
  /// @{

  /// @brief Scan until the address is seen (or @p scan_timeout), connect, subscribe
  ///        to DATA_INFO and TX.
  /// @param scan_timeout Maximum scan time.
  /// @throws Timeout if the bracelet is not seen.
  /// @throws std::runtime_error on other failures.
  void connect(std::chrono::milliseconds scan_timeout = std::chrono::seconds(10));
  /// @brief Disconnect.
  void disconnect();
  /// @brief Whether a connection is established.
  /// @return True if connected.
  bool is_connected() const;
  /// @brief Address given at construction.
  /// @return The address.
  const std::string &address() const;
  /// @}

  // MARK: - Raw commands
  /// @name Raw commands
  /// @{

  /// @brief Send a command and wait for its response.
  /// @param id Command id (see CommandId).
  /// @param payload Command payload, may be empty.
  /// @param timeout Maximum time to wait for the response.
  /// @return The response payload (id and CRC stripped).
  /// @throws Timeout if no response arrives in time.
  /// @throws PairingRequired if the bracelet is not bonded with this host.
  /// @throws ProtocolError on a bad CRC.
  std::vector<uint8_t> command(uint8_t id,
                               std::span<const uint8_t> payload = {},
                               std::chrono::milliseconds timeout = DefaultTimeout);
  /// @}

  // MARK: - Device info
  /// @name Device info
  /// @{

  /// @brief Read the battery state.
  /// @return The battery state.
  BatteryLevel battery();
  /// @brief Read the firmware version.
  /// @return The firmware version.
  FirmwareVersion firmware_version();
  /// @brief Read the serial number.
  /// @return The serial number.
  std::string serial_number();
  /// @brief Set the bracelet clock to the host clock and local UTC offset.
  void set_time(); // host clock and local UTC offset
  /// @}

  // MARK: - Plan
  /// @name Plan
  /// Selects what the bracelet measures and how often (see Plan). Persisted
  /// on the bracelet; the Corsano app may change it on its next sync.
  /// @{

  /// @brief Read the measurement plan.
  /// @return The current plan settings.
  PlanSettings plan();
  /// @brief Set the measurement plan.
  /// @param settings Plan and sampling rates to apply.
  /// @throws ProtocolError if refused.
  void set_plan(const PlanSettings &settings); // throws ProtocolError if refused
  /// @}

  // MARK: - User profile and calibration
  /// @name User profile and calibration
  /// @{

  /// @brief Read the user profile, birth date, height and weight (four commands).
  /// @return The profile.
  UserProfile user_profile();
  /// @brief Set the profile, and height/weight when non-zero.
  /// @param profile Profile to set.
  void set_user_profile(const UserProfile &profile);
  /// @brief Start Wearing Optimization (special mode 4).
  ///
  /// Tunes LED power to the wearer; SpO2 depends on it. Needs plan and profile
  /// set; wearer seated or lying, still, for 2-4 min. Poll
  /// wearing_optimization_status() every ~5 s until finished.
  void start_wearing_optimization();
  /// @brief Read the Wearing Optimization progress.
  /// @return The status; check WearingOptimizationStatus::success().
  WearingOptimizationStatus wearing_optimization_status();
  /// @brief Stop Wearing Optimization.
  void stop_wearing_optimization();
  /// @}

  // MARK: - Live vitals (HR, SpO2)
  /// @name Live vitals (HR, SpO2)
  /// Setters throw ProtocolError if status != StatusOk. See docs/HR_SPO2.md.
  /// @{

  /// @brief Enable or disable RealtimePing notifications.
  ///
  /// RealtimePing notifications arrive every realtime_period() seconds while
  /// enabled.
  /// @param enabled True to enable.
  /// @throws ProtocolError if status != StatusOk.
  void set_realtime_enabled(bool enabled);
  /// @brief Whether RealtimePing notifications are enabled.
  /// @return True if enabled.
  bool realtime_enabled();
  /// @brief Set the RealtimePing period.
  /// @param period Period, 1..255 s.
  /// @throws std::invalid_argument if out of range.
  /// @throws ProtocolError if status != StatusOk.
  void set_realtime_period(std::chrono::seconds period); // 1..255 s
  /// @brief Read the RealtimePing period.
  /// @return The period.
  std::chrono::seconds realtime_period();

  /// @brief Set how often the bracelet measures a vital (0 = off).
  ///
  /// SDK presets: 0, 30, 60, 300, 1800 s. Drives both RealtimePing and
  /// Activity records.
  /// @param type Vital to configure.
  /// @param interval Measurement interval, 0..65535 s.
  /// @throws std::invalid_argument if out of range.
  /// @throws ProtocolError if status != StatusOk.
  void set_vital_sampling_rate(VitalType type, std::chrono::seconds interval);
  /// @brief Read how often the bracelet measures a vital (0 = off).
  /// @param type Vital to query.
  /// @return The measurement interval.
  std::chrono::seconds vital_sampling_rate(VitalType type);

  /// @brief Enable or disable workout mode.
  ///
  /// LiveHrm notifications are expected while it is on (unverified on
  /// hardware).
  /// @param enabled True to enable.
  /// @throws ProtocolError if status != StatusOk.
  void set_workout_mode(bool enabled);
  /// @}

  // MARK: - Files (history)
  /// @name Files (history)
  /// @{

  /// @brief Bytes currently stored on the bracelet for that file.
  /// @param file File to query.
  /// @return Size in bytes.
  uint32_t file_size(FileId file);
  /// @brief Download up to @p max_bytes (rounded down to whole records) as raw bytes.
  ///
  /// Does not erase; decode with the decode_<name>() functions in files.hpp.
  /// @param file File to download.
  /// @param max_bytes Maximum number of bytes.
  /// @param progress Optional progress callback.
  /// @return The raw bytes.
  /// @throws Timeout if the download stalls.
  /// @throws std::runtime_error if disconnected during the download.
  std::vector<uint8_t> download_file(FileId file, uint32_t max_bytes,
                                     ProgressCallback progress = {});
  /// @brief Free the first @p bytes of the file.
  /// @param file File to erase from.
  /// @param bytes Bytes to free (use Decoded::consumed_bytes).
  void erase_file(FileId file, uint32_t bytes);
  /// @}

  // MARK: - Callbacks
  /// @name Callbacks
  /// Replace any previous callback; pass {} to clear. They run on the BLE
  /// backend thread.
  /// @{

  /// @brief Set the callback for unsolicited notifications.
  /// @param callback Receiver, or {} to clear.
  void set_notification_callback(NotificationCallback callback);
  /// @brief Set the callback for connection loss.
  /// @param callback Receiver, or {} to clear.
  void set_disconnect_callback(DisconnectCallback callback);
  /// @}

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

/// @}

} // namespace Corsano
