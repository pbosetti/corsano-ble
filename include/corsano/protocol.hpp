/// @file protocol.hpp
/// @brief Corsano bracelet BLE wire protocol: constants, ids, payload types and
///        the pure encode/decode codec.
///
/// Reverse engineered from com.corsano.sdk:ble:1.1.29 (Maven Central). Pure
/// data/codec layer: no BLE, no threads, so it can be unit-tested and reused by
/// the C/Python wrappers. Byte-level details: docs/PROTOCOL.md; HR and SpO2
/// specifics: docs/HR_SPO2.md.
///
/// Transport (Nordic UART-like service, all multi-byte fields little endian):
/// @code
///   RX  (write with response): request  = [cmd id][payload...]   (no CRC)
///   DATA_INFO (notify):        response = [cmd id][payload...][crc8]
///                              also unsolicited pings, same framing
///   TX  (notify):              raw file stream after "start streaming"
/// @endcode
///
/// @see Corsano::Bracelet for the BLE connection that uses this codec.
/// @see files.hpp for the decoders of recorded files.
#pragma once

#include "corsano/files.hpp" // recorded-file decoders

#include <chrono>
#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Corsano {

/// @defgroup corsano_protocol Wire protocol
/// @brief Command ids, GATT uuids, payload types and the codec.
/// @{

// MARK: - GATT
/// @name GATT
/// @{

/// @brief UUID of the Nordic-UART-like GATT service advertised by bracelets.
inline constexpr const char *ServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca3e";
/// @brief RX characteristic (write with response): requests to the bracelet.
inline constexpr const char *RxCharUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca3e";
/// @brief TX characteristic (notify): raw file stream after "start streaming".
inline constexpr const char *TxCharUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca3e";
/// @brief DATA_INFO characteristic (notify): responses and unsolicited pings.
inline constexpr const char *DataInfoCharUuid = "6e400004-b5a3-f393-e0a9-e50e24dcca3e";
/// @brief Default pairing passkey of the bracelet.
inline constexpr const char *DefaultPasskey = "999999";
/// @}

// MARK: - Identifiers
/// @name Identifiers
/// @{

/// @brief Result byte of "set" commands: success.
inline constexpr uint8_t StatusOk = 100;
/// @brief Result byte of "set" commands: failure.
inline constexpr uint8_t StatusFail = 101;

/// @brief Command ids (raw wire bytes).
///
/// Subset of the SDK's CommandId enum (~180 entries). Any other id can still be
/// sent through Bracelet::command().
enum class CommandId : uint8_t {
  SetWorkoutMode = 16,                ///< Enable/disable workout mode (LiveHrm).
  EraseAllFiles = 21,                 ///< Erase every recorded file.
  UpdateTime = 22,                    ///< Set the bracelet clock, see encode_set_time().
  ResetWatch = 24,                    ///< Reset the bracelet.
  StartMeasurement = 34,              ///< Start an on-demand measurement; payload 2 = ECG.
  StopMeasurement = 35,               ///< Stop an on-demand measurement; same payload.
  SetHeight = 42,                     ///< Set the user height.
  SetWeight = 43,                     ///< Set the user weight.
  SetProfile = 44,                    ///< Set the user profile, see encode_user_profile().
  EraseFromFile = 45,                 ///< Free the first bytes of a file.
  GetFileSize = 46,                   ///< Bytes stored in a file, see decode_file_size().
  StartStreamingFileWithSize = 48,    ///< Start the raw file stream on TX.
  GetActiveMode = 49,                 ///< Query the active mode.
  GetBatteryLevel = 50, ///< Battery level, see decode_battery(); also arrives unsolicited as charging event
  GetCurrentTime = 51,                ///< Read the bracelet clock.
  GetHeight = 54,                     ///< Read the user height.
  GetWeight = 55,                     ///< Read the user weight.
  GetBirthdate = 56,                  ///< Read the user birth date.
  GetProfile = 57,                    ///< Read the user profile.
  GetFwVersion = 63,                  ///< Firmware version, see decode_firmware_version().
  GetHwId = 65,                       ///< Hardware id.
  GetSerialNumber = 70,               ///< Serial number.
  GetDeviceName = 72,                 ///< Device name.
  GetBondingState = 110,              ///< Whether the bracelet is bonded with this host.
  Shutdown = 111,                     ///< Power the bracelet off.
  RegularPing = 125, ///< Unsolicited, see RegularPing
  SetPlan = 149,                      ///< Set the measurement plan.
  GetPlan = 150,                      ///< Read the measurement plan, see decode_plan().
  WorkoutStop = 155,                  ///< Stop the workout.
  GetBraceletStatus = 156,            ///< Bracelet status.
  LiveHrmPing = 160, ///< Unsolicited, see LiveHrm
  StartSpecialMode = 216, ///< Payload 4 = Wearing Optimization
  StopSpecialMode = 217,              ///< Stop a special mode.
  GetSpecialMode = 246, ///< Payload u32 mode, see decode_wearing_optimization()
  Authenticate = 250,   ///< FW >= 8.41: `A7 16` + 8-byte host id, see encode_authenticate()
  GetAuthStatus = 251,  ///< FW >= 8.41: payload `0`; 100 = connection authenticated
  SetRealtimeEnable = 220,            ///< Enable/disable RealtimePing notifications.
  GetRealtimeEnable = 221,            ///< Read the realtime-enable flag.
  RealtimePing = 222, ///< Unsolicited, see RealtimePing
  SetRealtimePeriod = 223,            ///< Set the RealtimePing period.
  GetRealtimePeriod = 224,            ///< Read the RealtimePing period.
  SetVitalsSamplingRate = 234,        ///< Set a vital's measurement interval, see VitalType.
  GetVitalsSamplingRate = 235,        ///< Read a vital's measurement interval.
};

/// @brief Files stored on the bracelet, downloadable via Bracelet::download_file().
///
/// Decoders for the recorded formats live in files.hpp.
enum class FileId : uint8_t {
  Ppg = 1,       ///< Raw PPG, see decode_ppg().
  Activity = 2, ///< Periodic vitals incl. HR and SpO2, see files.hpp and decode_activity()
  Hrv = 3,       ///< Heart-rate variability (no decoder).
  Workout = 4,   ///< Workout data (no decoder).
  Sleep = 5,     ///< Sleep data (no decoder).
  Logs = 6,      ///< Device logs (no decoder).
  Ppg2 = 9,      ///< Multicolour raw PPG, see decode_ppg2().
  Ecg = 13,      ///< ECG, see decode_ecg().
  Bioz = 14,     ///< Bioimpedance, see decode_bioz().
  Greenteg = 18, ///< Greenteg sensor data (no decoder).
  Stress = 21,   ///< Stress, see decode_stress().
  Acc = 22,      ///< Accelerometer, see decode_acc().
};

/// @brief Fixed record size of a file.
/// @param file File to query.
/// @return Record size in bytes; 0 means variable-length (no rounding applied).
uint32_t record_size(FileId file);

/// @brief Vitals whose on-device measurement interval can be set.
/// @see Bracelet::set_vital_sampling_rate()
enum class VitalType : uint8_t {
  Activity = 1,        ///< Activity (steps, energy).
  PulseRate = 2,       ///< Heart rate.
  RespirationRate = 3, ///< Respiration rate.
  Spo2 = 4,            ///< Blood oxygen saturation.
  Temperature = 5,     ///< Temperature.
  Nibp = 9,            ///< Non-invasive blood pressure.
};

/// @brief Measurement plans.
///
/// See developer.corsano.com ios/bracelet/bracelet_plans. Autonomy in days;
/// "B2" = multicolor bracelets only.
/// @see Bracelet::set_plan()
enum class Plan : uint8_t {
  StandBy = 0,          ///< Stand-by, no measurements.
  MaxBattery = 1,       ///< 15 d: HR, steps, temp 1/min
  Typical = 2,          ///< 7.5 d: + resp 1/min, SpO2 1/30 min
  Hospital = 3,         ///< 6 d: HR, resp, SpO2 1/10 s
  Hz25 = 4,             ///< Deprecated.
  HospitalRaw = 5,      ///< 3.5 d @32 Hz: Hospital + continuous raw PPG2/acc
  Disabled = 6,         ///< Measurements disabled.
  TypicalMulticolor = 7, ///< B2, 5.5 d: + raw PPG2/acc 2 min every 30 min
  HospitalMulticolor = 8, ///< B2, 4 d
  MulticolorIntermittentRaw = 9, ///< B2, 3 d: + continuous raw green PPG2
  MulticolorIntermittent = 10,   ///< B2, 5 d
  Continuous5Min = 11,           ///< B2, 6 d
};

/// @brief Raw PPG2 sampling rate set together with the plan.
enum class Ppg2Rate : uint8_t { Hz32 = 1, /**< 32 Hz */Hz64 = 2, /**< 64 Hz */Hz128 = 3, /**< 128 Hz */Hz256 = 4, /**< 256 Hz */Hz512 = 5 /**< 512 Hz */};

/// @brief Plan together with the raw sampling rates it is applied with.
/// @see Bracelet::plan(), Bracelet::set_plan(), decode_plan()
struct PlanSettings {
  Plan plan; ///< Selected measurement plan.
  Ppg2Rate ppg2_rate = Ppg2Rate::Hz32; ///< Raw PPG2 sampling rate.
  uint8_t ppg1_rate = 60; ///< SDK "sampleRateV1"; meaning unknown, SDK uses 60
};
/// @}

// MARK: - User profile and calibration
/// @name User profile and calibration
/// The PPG algorithms (SpO2 above all) use the profile; run Wearing
/// Optimization after setting it (docs: developer.corsano.com, Quality and
/// Wearing Optimization pages).
/// @{

/// @brief User gender as stored in the profile.
enum class Gender : uint8_t { Male = 0, /**< Male */Female = 1, /**< Female */Unspecified = 2 /**< Not specified */};
/// @brief Wrist the bracelet is worn on.
enum class Wrist : uint8_t { Left = 0, /**< Left wrist */Right = 1 /**< Right wrist */};
/// @brief Fitzpatrick-style skin type used by the PPG algorithms.
enum class SkinType : uint8_t {
  VeryFair = 1,   ///< Very fair.
  Fair = 2,       ///< Fair.
  Olive = 3,      ///< Olive.
  LightBrown = 4, ///< Light brown.
  Brown = 5,      ///< Brown.
  BlackBrown = 6, ///< Black-brown.
};

/// @brief User profile used by the bracelet's algorithms.
/// @see Bracelet::user_profile(), Bracelet::set_user_profile(), encode_user_profile()
struct UserProfile {
  std::chrono::year_month_day birth{}; ///< Birth date.
  Gender gender = Gender::Unspecified; ///< Gender.
  Wrist wrist = Wrist::Left; ///< Wrist worn on.
  SkinType skin = SkinType::Fair; ///< Skin type.
  uint8_t height_cm = 0; ///< Height in cm; 0 = leave unchanged when setting
  uint8_t weight_kg = 0; ///< Weight in kg; 0 = leave unchanged when setting
};

/// @brief Progress of Wearing Optimization (special mode 4).
/// @see Bracelet::start_wearing_optimization(), decode_wearing_optimization()
struct WearingOptimizationStatus {
  bool finished; ///< The run has ended.
  uint8_t progress_percent; ///< Progress in %; restarts when movement is detected
  uint8_t movement_level; ///< Movement level seen during the run.
  uint8_t quality; ///< 0..100; >= 50 passes
  /// @brief True if the run finished with a passing quality (>= 50).
  /// @return `finished && quality >= 50`.
  bool success() const { return finished && quality >= 50; }
};

/// @name SpO2 alert values
/// spo2_quality / spo2_q values beyond the 0..4 quality scale (alerts).
/// See docs/HR_SPO2.md.
/** @{ */
/// @brief Low confidence: fit or movement, re-seat the bracelet.
inline constexpr uint8_t Spo2LowConfidence = 103; // fit, movement: re-seat
/// @brief Motion: keep still.
inline constexpr uint8_t Spo2Motion = 104;        // keep still
/// @brief Venous pooling: data discarded.
inline constexpr uint8_t Spo2VenousPooling = 105; // data discarded
/// @brief Skin condition: use the other arm or recalibrate.
inline constexpr uint8_t Spo2SkinCondition = 106; // other arm, recalibrate
/// @}
/// @}

// MARK: - Decoded payloads
/// @name Decoded payloads
/// @{

/// @brief Battery state (response of GetBatteryLevel, or charging event).
/// @see decode_battery()
struct BatteryLevel {
  uint8_t percent; ///< Charge in %.
  uint16_t millivolts; ///< Battery voltage in mV.
  bool charging; ///< Currently charging.
};

/// @brief Firmware version (e.g. 8.12).
/// @see decode_firmware_version()
struct FirmwareVersion {
  int major; ///< Major version.
  int minor; ///< Minor version.
};

/// @brief Unsolicited periodic ping (CommandId::RegularPing).
struct RegularPing {
  uint8_t battery_percent; ///< Battery charge in %.
  /// Bytes pending in each on-device file (FileId::Ppg, Activity, Hrv,
  /// Workout, Sleep, Logs).
  uint16_t ppg_size, /**< PPG file */activity_size, /**< Activity file */hrv_size, /**< HRV file */workout_size, /**< Workout file */sleep_size, /**< Sleep file */
      log_size; ///< Logs file.
};

/// @brief Unsolicited realtime vitals (CommandId::RealtimePing).
///
/// Quality fields: 0 = bad ... 4 = very good (per SDK docs for HR); SpO2
/// quality can also hold the alert values Spo2LowConfidence etc.
/// @see Bracelet::set_realtime_enabled()
struct RealtimePing {
  uint32_t timestamp_s; ///< Bracelet clock, epoch seconds
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; ///< Heart rate quality (0..4)
  float respiration_rate; ///< Breaths/min
  uint8_t respiration_rate_quality; ///< Respiration quality (0..4).
  uint8_t spo2, /**< SpO2 in % */spo2_quality; ///< SpO2 quality (0..4), or an alert value
  float core_body_temp_c; ///< Core body temperature in degrees C.
  uint8_t movement_level; ///< Movement level.
  uint8_t battery_percent; ///< Battery charge in %.
};

/// @brief Unsolicited live heart-rate message of workout mode (CommandId::LiveHrmPing).
/// @see Bracelet::set_workout_mode()
struct LiveHrm {
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; ///< Heart rate quality (0..4)
  uint16_t steps; ///< Steps.
  uint16_t energy; ///< Energy.
  uint8_t max_heart_rate, /**< Maximum heart rate */speed; ///< Current speed
  uint16_t duration; ///< Workout duration.
  uint8_t battery_percent; ///< Battery charge in %.
  uint8_t average_heart_rate, /**< Average heart rate */average_speed; ///< Average speed
};

/// @brief Unsolicited messages on DATA_INFO. BatteryLevel = charging state change.
using Notification =
    std::variant<RegularPing, RealtimePing, LiveHrm, BatteryLevel>;
/// @}

// MARK: - Errors
/// @name Errors
/// @{

/// @brief Malformed, short or refused protocol message.
struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// @brief The bracelet reported it is not bonded with this host.
struct PairingRequired : ProtocolError {
  using ProtocolError::ProtocolError;
};
/// @}

// MARK: - Codec
/// @name Codec
/// @{

/// @brief CRC-8, poly 0x07, init 0xFF, no reflection, no final xor.
/// @param data Bytes to checksum.
/// @return The CRC-8 of @p data.
uint8_t crc8(std::span<const uint8_t> data);

/// @brief Check the CRC of a DATA_INFO frame.
/// @param frame Frame whose last byte is the CRC of the preceding bytes.
/// @return True if the CRC matches. Frames shorter than 2 bytes are accepted,
///         as the SDK does.
bool check_crc(std::span<const uint8_t> frame);

/// @brief Build a request frame `[id][payload...]` (no CRC).
/// @param id Command id (a CommandId value or any raw byte).
/// @param payload Command payload, may be empty.
/// @return The bytes to write to the RX characteristic.
std::vector<uint8_t> encode_request(uint8_t id,
                                    std::span<const uint8_t> payload = {});

/// @brief Build the UpdateTime payload.
///
/// Layout: UTC Y-2000, M, D, h, m, s, then local offset (int16 min).
/// @param now Time to set.
/// @param utc_offset Local offset from UTC.
/// @return The payload for CommandId::UpdateTime.
std::vector<uint8_t> encode_set_time(std::chrono::system_clock::time_point now,
                                     std::chrono::minutes utc_offset);

// Payload decoders take the response with id and CRC already stripped.

/// @brief Decode a GetBatteryLevel response payload.
/// @param payload Response with id and CRC stripped.
/// @return The battery state.
/// @throws ProtocolError on short input.
BatteryLevel decode_battery(std::span<const uint8_t> payload);
/// @brief Decode a GetFwVersion response payload.
/// @param payload Response with id and CRC stripped.
/// @return The firmware version.
/// @throws ProtocolError on short input.
FirmwareVersion decode_firmware_version(std::span<const uint8_t> payload);
/// @brief Decode a little-endian u32 response payload.
/// @param payload Response with id and CRC stripped.
/// @return The value.
/// @throws ProtocolError on short input.
uint32_t decode_u32(std::span<const uint8_t> payload);
/// @brief Build the SetProfile payload.
///
/// Layout: age at `today`, year (hi, lo), month, day, gender, wrist, skin.
/// @param profile Profile to encode.
/// @param today Date used to compute the age.
/// @return The payload for CommandId::SetProfile.
std::vector<uint8_t> encode_user_profile(const UserProfile &profile,
                                         std::chrono::year_month_day today);
/// @brief Decode a GetSpecialMode(4) response.
///
/// Layout: `[4][?][finished][movement][progress][quality]`.
/// @param payload Response with id and CRC stripped.
/// @return The Wearing Optimization status.
/// @throws ProtocolError on short input.
WearingOptimizationStatus
decode_wearing_optimization(std::span<const uint8_t> payload);

// MARK: - Command catalog
/// @name Command catalog
/// Names and human-readable reply decoding for every command corsano-ble
/// knows, for tools and logs (e.g. `corsano-cli raw`). Raw ids not in the
/// catalog still work everywhere; they just have no name or description.
/// @{

/// @brief One known command.
struct CommandInfo {
  uint8_t id;              ///< Wire id (see CommandId)
  const char *name;        ///< Kebab-case name, e.g. "get-plan"; unique
  const char *description; ///< One line, e.g. "Read plan and PPG2 rate"
  bool is_set;             ///< Reply is a status byte (100 = OK)
};

/// @brief All known commands, sorted by id.
/// @return Static table; never empty.
std::span<const CommandInfo> command_catalog();

/// @brief Look up a command by id.
/// @param id Wire id.
/// @return The entry, or nullptr if unknown.
const CommandInfo *find_command(uint8_t id);

/// @brief Look up a command by name (case-insensitive; `_` matches `-`).
/// @param name Name as in CommandInfo::name.
/// @return The entry, or nullptr if unknown.
const CommandInfo *find_command(std::string_view name);

/// @brief Decode a command reply into readable `key=value` pairs.
///
/// Set commands give `status=ok` or `status=refused(N)`; gets give their
/// fields with units and names, e.g. `plan=typical-multicolor ppg2=32Hz
/// ppg1=60`, `vital=spo2 interval=60s`, `time=2026-11-10T14:21:32Z`.
/// @param id Command id the reply belongs to.
/// @param payload Reply payload (id and CRC stripped, as Bracelet::command()
///        returns it; FW padding is tolerated).
/// @return The description; empty for unknown ids or payloads that do not
///         decode. Never throws.
std::string describe_reply(uint8_t id, std::span<const uint8_t> payload);
/// @}

// MARK: - Authentication (FW >= 8.41)
/// @name Authentication
/// Firmware 8.41+ acts on commands only from an authenticated connection,
/// and authentication does not survive a disconnection (developer.corsano.com,
/// Android "Bracelet Initialization"). It is an identification handshake: no
/// licence, key or signature is involved.
/// @{

/// @brief First firmware that requires authentication.
inline constexpr FirmwareVersion AuthMinFirmware{8, 41};

/// @brief Fold a host id into the firmware's 8-byte field, as the SDK does.
/// @param host_id Any stable per-host string (the SDK uses Android's
///        ANDROID_ID).
/// @return Its 8 bytes if @p host_id is exactly 16 hex digits, else the first
///         8 bytes of SHA-256(@p host_id).
std::array<uint8_t, 8> host_id_bytes(std::string_view host_id);

/// @brief Encode the Authenticate (250) payload: `A7 16` + host_id_bytes().
/// @param host_id Host id, see host_id_bytes().
/// @return The 10-byte payload.
std::vector<uint8_t> encode_authenticate(std::string_view host_id);

/// @brief SHA-256 digest (used by host_id_bytes()).
/// @param data Input bytes.
/// @return The 32-byte digest.
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
/// @}
/// @brief Decode a GetPlan response.
///
/// Layout: `[plan][ppg2_rate][ppg1_rate]` (shorter responses keep defaults).
/// @param payload Response with id and CRC stripped.
/// @return The plan settings.
/// @throws ProtocolError on short input.
PlanSettings decode_plan(std::span<const uint8_t> payload);
/// @brief Decode a GetFileSize response.
///
/// FW 7.x `[size:u32]`, FW 8.x `[file:u8][size:u32]`.
/// @param payload Response with id and CRC stripped.
/// @param file File the size was requested for.
/// @return Bytes stored in the file.
/// @throws ProtocolError on short input.
uint32_t decode_file_size(std::span<const uint8_t> payload, FileId file);

/// @brief Decode an unsolicited DATA_INFO frame.
/// @param frame Full frame (with id and CRC).
/// @return The notification, or nullopt for unknown ids.
std::optional<Notification> decode_notification(std::span<const uint8_t> frame);
/// @}

/// @}

} // namespace Corsano
