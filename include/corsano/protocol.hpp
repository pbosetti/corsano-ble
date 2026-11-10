// Corsano bracelet BLE wire protocol, reverse engineered from
// com.corsano.sdk:ble:1.1.29 (Maven Central). Pure data/codec layer: no BLE,
// no threads, so it can be unit-tested and reused by the C/Python wrappers.
//
// Transport (Nordic UART-like service, all multi-byte fields little endian):
//   RX  (write with response): request  = [cmd id][payload...]   (no CRC)
//   DATA_INFO (notify):        response = [cmd id][payload...][crc8]
//                              also unsolicited pings, same framing
//   TX  (notify):              raw file stream after "start streaming"
#pragma once

#include "corsano/files.hpp" // recorded-file decoders

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace Corsano {

// MARK: - GATT

inline constexpr const char *ServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca3e";
inline constexpr const char *RxCharUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca3e";
inline constexpr const char *TxCharUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca3e";
inline constexpr const char *DataInfoCharUuid = "6e400004-b5a3-f393-e0a9-e50e24dcca3e";
inline constexpr const char *DefaultPasskey = "999999";

// MARK: - Identifiers

// Result byte of "set" commands
inline constexpr uint8_t StatusOk = 100;
inline constexpr uint8_t StatusFail = 101;

// Subset of the SDK's CommandId enum (~180 entries). Any other id can still be
// sent through Bracelet::command(); values are the raw wire bytes.
enum class CommandId : uint8_t {
  SetWorkoutMode = 16,
  EraseAllFiles = 21,
  UpdateTime = 22,
  ResetWatch = 24,
  SetHeight = 42,
  SetWeight = 43,
  SetProfile = 44,
  EraseFromFile = 45,
  GetFileSize = 46,
  StartStreamingFileWithSize = 48,
  GetActiveMode = 49,
  GetBatteryLevel = 50, // also arrives unsolicited as charging event
  GetCurrentTime = 51,
  GetHeight = 54,
  GetWeight = 55,
  GetBirthdate = 56,
  GetProfile = 57,
  GetFwVersion = 63,
  GetHwId = 65,
  GetSerialNumber = 70,
  GetDeviceName = 72,
  GetBondingState = 110,
  Shutdown = 111,
  RegularPing = 125, // unsolicited
  SetPlan = 149,
  GetPlan = 150,
  WorkoutStop = 155,
  GetBraceletStatus = 156,
  LiveHrmPing = 160, // unsolicited
  StartSpecialMode = 216, // payload 4 = Wearing Optimization
  StopSpecialMode = 217,
  GetSpecialMode = 246, // payload u32 mode
  SetRealtimeEnable = 220,
  GetRealtimeEnable = 221,
  RealtimePing = 222, // unsolicited
  SetRealtimePeriod = 223,
  GetRealtimePeriod = 224,
  SetVitalsSamplingRate = 234,
  GetVitalsSamplingRate = 235,
};

// Files stored on the bracelet, downloadable via Bracelet::download_file()
enum class FileId : uint8_t {
  Ppg = 1,
  Activity = 2, // periodic vitals incl. HR and SpO2, see files.hpp
  Hrv = 3,
  Workout = 4,
  Sleep = 5,
  Logs = 6,
  Ppg2 = 9,
  Ecg = 13,
  Bioz = 14,
  Greenteg = 18,
  Stress = 21,
  Acc = 22,
};

// Fixed record size in bytes; 0 means variable-length (no rounding applied)
uint32_t record_size(FileId file);

// Vitals whose on-device measurement interval can be set
enum class VitalType : uint8_t {
  Activity = 1,
  PulseRate = 2,
  RespirationRate = 3,
  Spo2 = 4,
  Temperature = 5,
  Nibp = 9,
};

// Measurement plans (developer.corsano.com ios/bracelet/bracelet_plans).
// Autonomy in days; "B2" = multicolor bracelets only.
enum class Plan : uint8_t {
  StandBy = 0,
  MaxBattery = 1,       // 15 d: HR, steps, temp 1/min
  Typical = 2,          // 7.5 d: + resp 1/min, SpO2 1/30 min
  Hospital = 3,         // 6 d: HR, resp, SpO2 1/10 s
  Hz25 = 4,             // deprecated
  HospitalRaw = 5,      // 3.5 d @32 Hz: Hospital + continuous raw PPG2/acc
  Disabled = 6,
  TypicalMulticolor = 7, // B2, 5.5 d: + raw PPG2/acc 2 min every 30 min
  HospitalMulticolor = 8, // B2, 4 d
  MulticolorIntermittentRaw = 9, // B2, 3 d: + continuous raw green PPG2
  MulticolorIntermittent = 10,   // B2, 5 d
  Continuous5Min = 11,           // B2, 6 d
};

// Raw PPG2 sampling rate set together with the plan
enum class Ppg2Rate : uint8_t { Hz32 = 1, Hz64 = 2, Hz128 = 3, Hz256 = 4, Hz512 = 5 };

struct PlanSettings {
  Plan plan;
  Ppg2Rate ppg2_rate = Ppg2Rate::Hz32;
  uint8_t ppg1_rate = 60; // SDK "sampleRateV1"; meaning unknown, SDK uses 60
};

// MARK: - User profile and calibration
// The PPG algorithms (SpO2 above all) use the profile; run Wearing
// Optimization after setting it (docs: developer.corsano.com, Quality and
// Wearing Optimization pages).

enum class Gender : uint8_t { Male = 0, Female = 1, Unspecified = 2 };
enum class Wrist : uint8_t { Left = 0, Right = 1 };
enum class SkinType : uint8_t {
  VeryFair = 1,
  Fair = 2,
  Olive = 3,
  LightBrown = 4,
  Brown = 5,
  BlackBrown = 6,
};

struct UserProfile {
  std::chrono::year_month_day birth{};
  Gender gender = Gender::Unspecified;
  Wrist wrist = Wrist::Left;
  SkinType skin = SkinType::Fair;
  uint8_t height_cm = 0; // 0 = leave unchanged when setting
  uint8_t weight_kg = 0; // 0 = leave unchanged when setting
};

struct WearingOptimizationStatus {
  bool finished;
  uint8_t progress_percent; // restarts when movement is detected
  uint8_t movement_level;
  uint8_t quality; // 0..100; >= 50 passes
  bool success() const { return finished && quality >= 50; }
};

// spo2_quality / spo2_q values beyond the 0..4 quality scale (alerts)
inline constexpr uint8_t Spo2LowConfidence = 103; // fit, movement: re-seat
inline constexpr uint8_t Spo2Motion = 104;        // keep still
inline constexpr uint8_t Spo2VenousPooling = 105; // data discarded
inline constexpr uint8_t Spo2SkinCondition = 106; // other arm, recalibrate

// MARK: - Decoded payloads

struct BatteryLevel {
  uint8_t percent;
  uint16_t millivolts;
  bool charging;
};

struct FirmwareVersion {
  int major;
  int minor;
};

struct RegularPing {
  uint8_t battery_percent;
  // bytes pending in each on-device file
  uint16_t ppg_size, activity_size, hrv_size, workout_size, sleep_size,
      log_size;
};

// Quality fields: 0 = bad ... 4 = very good (per SDK docs for HR)
struct RealtimePing {
  uint32_t timestamp_s; // bracelet clock, epoch seconds
  uint8_t heart_rate, heart_rate_quality;
  float respiration_rate; // breaths/min
  uint8_t respiration_rate_quality;
  uint8_t spo2, spo2_quality; // spo2 in %
  float core_body_temp_c;
  uint8_t movement_level;
  uint8_t battery_percent;
};

struct LiveHrm {
  uint8_t heart_rate, heart_rate_quality;
  uint16_t steps;
  uint16_t energy;
  uint8_t max_heart_rate, speed;
  uint16_t duration;
  uint8_t battery_percent;
  uint8_t average_heart_rate, average_speed;
};

// Unsolicited messages on DATA_INFO. BatteryLevel = charging state change.
using Notification =
    std::variant<RegularPing, RealtimePing, LiveHrm, BatteryLevel>;

// MARK: - Errors

struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The bracelet reported it is not bonded with this host
struct PairingRequired : ProtocolError {
  using ProtocolError::ProtocolError;
};

// MARK: - Codec

// CRC-8, poly 0x07, init 0xFF, no reflection, no final xor
uint8_t crc8(std::span<const uint8_t> data);

// True if the frame's last byte is the CRC of the preceding bytes.
// Frames shorter than 2 bytes are accepted, as the SDK does.
bool check_crc(std::span<const uint8_t> frame);

std::vector<uint8_t> encode_request(uint8_t id,
                                    std::span<const uint8_t> payload = {});

// UpdateTime payload: UTC Y-2000, M, D, h, m, s, then local offset (int16 min)
std::vector<uint8_t> encode_set_time(std::chrono::system_clock::time_point now,
                                     std::chrono::minutes utc_offset);

// Payload decoders take the response with id and CRC already stripped;
// they throw ProtocolError on short input.
BatteryLevel decode_battery(std::span<const uint8_t> payload);
FirmwareVersion decode_firmware_version(std::span<const uint8_t> payload);
uint32_t decode_u32(std::span<const uint8_t> payload);
// SetProfile payload: age at `today`, year (hi, lo), month, day, gender,
// wrist, skin
std::vector<uint8_t> encode_user_profile(const UserProfile &profile,
                                         std::chrono::year_month_day today);
// GetSpecialMode(4) response: `[4][?][finished][movement][progress][quality]`
WearingOptimizationStatus
decode_wearing_optimization(std::span<const uint8_t> payload);
// GetPlan: `[plan][ppg2_rate][ppg1_rate]` (shorter responses keep defaults)
PlanSettings decode_plan(std::span<const uint8_t> payload);
// GetFileSize: FW 7.x `[size:u32]`, FW 8.x `[file:u8][size:u32]`
uint32_t decode_file_size(std::span<const uint8_t> payload, FileId file);

// Full DATA_INFO frame (with id and CRC). nullopt for unknown ids.
std::optional<Notification> decode_notification(std::span<const uint8_t> frame);

} // namespace Corsano
