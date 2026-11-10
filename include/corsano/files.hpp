/// @file files.hpp
/// @brief Decoders for files recorded on the bracelet (FileId), downloaded raw
///        with Bracelet::download_file().
///
/// Every decoder follows one pattern:
/// @code
///   struct <Name>Chunk { ... };                              one chunk/record
///   Decoded<<Name>Chunk> decode_<name>(std::span<const uint8_t> data);
/// @endcode
/// Decoders never throw on corrupt or truncated input: they skip bytes until
/// the next valid chunk and stop before an incomplete one, so
/// Decoded::consumed_bytes is always safe to pass to Bracelet::erase_file().
///
/// Byte layouts and framing: docs/PROTOCOL.md. Roadmap: docs/RAW_SIGNALS.md.
/// HR and SpO2 fields: docs/HR_SPO2.md.
///
/// @see Corsano::FileId for the file ids the decoders correspond to.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Corsano {

/// @defgroup corsano_files Recorded-file decoders
/// @brief Offline decoders for raw files downloaded from the bracelet.
/// @{

/// @brief Result of a file decoder.
/// @tparam Chunk Record or chunk type (e.g. ActivityRecord).
template <class Chunk> struct Decoded {
  std::vector<Chunk> records; ///< Records decoded, in file order.
  size_t consumed_bytes = 0;  ///< Input bytes consumed; safe for Bracelet::erase_file().
};

// MARK: - Activity (FileId::Activity)
/// @name Activity (FileId::Activity)
/// Fixed 38-byte records, trailing CRC-8.
/// @{

/// @brief One periodic-vitals record of FileId::Activity.
/// @see decode_activity()
struct ActivityRecord {
  uint32_t timestamp_s; ///< Record time, epoch seconds.
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; ///< Heart rate quality (0..4)
  uint16_t steps; ///< Steps.
  uint8_t activity_type, /**< Activity type */activity_count; ///< Activity count
  uint8_t spo2, /**< SpO2 in % */spo2_quality; ///< SpO2 quality (0..4), or an alert value
  uint16_t energy; ///< Energy.
  float respiration_rate; ///< Breaths/min.
  uint8_t respiration_rate_quality; ///< Respiration quality (0..4).
  uint8_t battery_percent; ///< Battery charge in %.
  bool charging; ///< Charging at record time.
  uint8_t nibp_systolic, /**< Systolic blood pressure */nibp_diastolic; ///< Diastolic blood pressure
  float temp1_c, /**< Temperature 1 in degrees C */temp2_c; ///< Temperature 2 in degrees C
  uint8_t wearing; ///< Wearing state.
};

/// @brief Decode FileId::Activity data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @return Decoded records and consumed byte count. Never throws.
Decoded<ActivityRecord> decode_activity(std::span<const uint8_t> data);
/// @}

// MARK: - PPG (FileId::Ppg)
/// @name PPG (FileId::Ppg)
/// Sync-marker chunks (256 B v2, 481 B v3), trailing CRC-32.
/// @{

/// @brief Time between PPG samples (25 Hz).
inline constexpr std::chrono::milliseconds PpgSamplePeriod{40}; // 25 Hz
/// @brief PPG samples per on-device heart-rate estimate (one per 1 s).
inline constexpr size_t PpgSamplesPerHeartRate = 25; // one estimate per 1 s

/// @brief One raw PPG sample.
struct PpgSample {
  uint16_t ppg;                ///< Raw optical value.
  uint16_t acc;                ///< Motion magnitude, SDK-expanded from 8 bit.
  int16_t acc_x, /**< Acceleration x */acc_y, /**< Acceleration y */acc_z; ///< Acceleration z; v3 only (0 in v2), raw int8 x 4
};

/// @brief One PPG chunk (nominally 75 samples = 3 s).
/// @see decode_ppg()
struct PpgChunk {
  uint8_t protocol_version; ///< 2 or 3.
  uint32_t index;           ///< Chunk counter.
  uint64_t timestamp_ms;    ///< First sample; sample i at + i * PpgSamplePeriod.
  /// On-device estimates for samples [0,25), [25,50), [50,75).
  std::array<uint8_t, 3> heart_rate, /**< Heart rate estimates */heart_rate_quality; ///< Quality of the estimates.
  std::vector<PpgSample> samples; ///< Nominally 75.
};

/// @brief Decode FileId::Ppg data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @return Decoded chunks and consumed byte count. Never throws.
Decoded<PpgChunk> decode_ppg(std::span<const uint8_t> data);
/// @}

// MARK: - Stress (FileId::Stress)
/// @name Stress (FileId::Stress)
/// Fixed 18-byte records, trailing CRC-8 ("emography" in the SDK). Values are
/// raw firmware units; the SDK does not document their scale.
/// @{

/// @brief One record of FileId::Stress.
/// @see decode_stress()
struct StressRecord {
  uint32_t timestamp_s; ///< Record time, epoch seconds.
  uint8_t cognitive_zone, /**< Cognitive zone */cognitive_zone_quality; ///< Cognitive zone quality
  uint8_t predicted_cognitive_zone; ///< Predicted cognitive zone.
  uint16_t predicted_transition_time; ///< Predicted time of the zone transition.
  uint8_t cognitive_zone_hour; ///< Cognitive zone hour.
  uint32_t cortisol_contribution; ///< Cortisol contribution.
  uint16_t skin_conductance_stress; ///< Stress level from skin conductance
  uint8_t skin_conductance_stress_quality; ///< Quality of skin_conductance_stress.
};

/// @brief Decode FileId::Stress data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @return Decoded records and consumed byte count. Never throws.
Decoded<StressRecord> decode_stress(std::span<const uint8_t> data);
/// @}

// MARK: - OHR files (FileId::Bioz, FileId::Acc; PPG2 later)
/// @name OHR files (FileId::Bioz, FileId::Acc; PPG2 later)
/// Records "OHR" + len:u16 + id; body records (id 0x0F) hold one channel chunk
/// `[channel:u8][len:u16][index][quality][body_position][format][samples...]`.
/// Channel 0x1E chunks carry the u32 epoch time of the data chunk(s) just
/// BEFORE them (1 s each; trailing untimed chunks are extrapolated). Chunks of
/// 0xFF are erased flash and skipped. Documented by Corsano: developer.corsano.com
/// android/data_models/raw_files; deviations noted in docs/PROTOCOL.md.
/// @{

/// @brief Bioimpedance sample rate in Hz.
inline constexpr double BiozSampleRateHz = 25.0;
/// @brief Accelerometer sample rate in Hz.
inline constexpr double AccSampleRateHz = 32.0;
/// @brief Accelerometer counts per g (observed at rest, unverified).
inline constexpr double AccCountsPerG = 512.0; // observed at rest, unverified

/// @brief One bioimpedance chunk (nominally 25 samples = 1 s).
/// @see decode_bioz()
struct BiozChunk {
  uint64_t timestamp_ms; ///< First sample; sample i at + i * 1000 / 25 ms.
  uint8_t index, /**< Chunk index */quality, /**< Quality */body_position; ///< Body position
  std::vector<uint32_t> samples; ///< Raw 24-bit, nominally 25 (1 s).
};

/// @brief One accelerometer chunk (nominally 32 samples = 1 s).
/// @see decode_acc()
struct AccChunk {
  uint64_t timestamp_ms; ///< First sample; sample i at + i * 1000 / 32 ms.
  uint8_t index, /**< Chunk index */quality, /**< Quality */body_position; ///< Body position
  std::vector<std::array<int16_t, 3>> samples; ///< Raw x, y, z; nominally 32.
};

/// @brief Decode FileId::Bioz data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @return Decoded chunks and consumed byte count. Never throws.
Decoded<BiozChunk> decode_bioz(std::span<const uint8_t> data);
/// @brief Decode FileId::Acc data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @return Decoded chunks and consumed byte count. Never throws.
Decoded<AccChunk> decode_acc(std::span<const uint8_t> data);
/// @}

/// @}

} // namespace Corsano
