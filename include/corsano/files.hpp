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
#include <map>
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
  uint8_t cognitive_zone, /**< Cognitive zone: 0 undefined, 1 under-stimulated, 2 balanced, 3 over-stimulated */cognitive_zone_quality; ///< Quality of cognitive_zone, 0..4
  uint8_t predicted_cognitive_zone; ///< Predicted cognitive zone, 0..3 as cognitive_zone.
  uint16_t predicted_transition_time; ///< Minutes until the predicted zone transition.
  uint8_t cognitive_zone_hour; ///< Cognitive zone expected one hour ahead, 0..3.
  uint32_t cortisol_contribution; ///< Cortisol contribution (undocumented unit; grows steadily over the day).
  uint16_t skin_conductance_stress; ///< Stress level from skin conductance, 0..1000
  uint8_t skin_conductance_stress_quality; ///< Quality of skin_conductance_stress, 0..4.
};

/// @brief Decode FileId::Stress data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @return Decoded records and consumed byte count. Never throws.
Decoded<StressRecord> decode_stress(std::span<const uint8_t> data);
/// @}

// MARK: - OHR files (FileId::Bioz, FileId::Acc, FileId::Ppg2, FileId::Ecg)
/// @name OHR files (FileId::Bioz, FileId::Acc, FileId::Ppg2, FileId::Ecg)
/// Records "OHR" + len:u16 + id; body records (id 0x0F) hold one or more
/// metric chunks `[metric:u8][len:u16][index][quality][body_position][format]...`.
/// Timing follows Corsano's reference parser: each channel runs its own clock
/// at the nominal chunk duration, resynced by the file header (0x0A) and the
/// time metric (0x1E) only when they drift past a tolerance (1 s for Acc, 6 s
/// otherwise). Chunks seen before any time info are stamped back from the
/// first 0x1E record; with no time info at all they get 0. ECG chunks carry
/// their own time. Byte layouts: docs/PROTOCOL.md.
/// @{

/// @brief Decoder state carried between calls on consecutive slices of one
///        OHR file (download, erase Decoded::consumed_bytes, download more).
///
/// With a state, channel clocks and the PPG2 rollover continue across calls,
/// and chunks seen before the first time information are held back (not
/// consumed, not returned) until a later slice brings it. Use one state per
/// file; treat the fields as private.
struct OhrState {
  std::map<uint32_t, uint64_t> clocks;  ///< Next timestamp (ms) per channel.
  uint64_t sync_ms = 0;                 ///< Last header/time value (ms), 0 = none yet.
  std::map<uint32_t, int32_t> ppg2_last; ///< Last PPG2 value per channel.
};

/// @brief Bioimpedance sample rate in Hz.
inline constexpr double BiozSampleRateHz = 25.0;
/// @brief Accelerometer sample rate in Hz.
inline constexpr double AccSampleRateHz = 32.0;
/// @brief Accelerometer counts per g (observed at rest, unverified).
inline constexpr double AccCountsPerG = 512.0; // documented unit 1/512 g; matches rest readings
/// @brief ECG sample rate in Hz (documented, unverified against the data).
inline constexpr double EcgSampleRateHz = 256.0;

/// @brief One bioimpedance chunk (nominally 25 samples = 1 s).
/// @see decode_bioz()
struct BiozChunk {
  uint64_t timestamp_ms; ///< First sample; sample i at + i * 1000 / 25 ms.
  uint8_t index, /**< Chunk index */quality, /**< Quality */body_position; ///< Body position
  std::vector<uint32_t> samples; ///< Raw 24-bit, nominally 25 (1 s).
  /// Signed 24-bit ADC value per sample, only in the "ADC" variant (chunks of
  /// 154 bytes or more); empty otherwise.
  std::vector<int32_t> adc;
};

/// @brief Corsano's BioZ scaling of a raw sample to 0..1 (unit undocumented).
inline double bioz_normalized(uint32_t raw) {
  double v = raw >= 0x80000 ? 0x80000 - double(raw) : double(raw);
  return (v / 0x80000 + 1.0) / 2.0;
}

/// @brief One accelerometer chunk (nominally 32 samples = 1 s).
/// @see decode_acc()
struct AccChunk {
  uint64_t timestamp_ms; ///< First sample; sample i at + i * 1000 / 32 ms.
  uint8_t index, /**< Chunk index */quality, /**< Quality */body_position; ///< Body position
  std::vector<std::array<int16_t, 3>> samples; ///< Raw x, y, z; nominally 32.
};

/// @brief One PPG2 chunk: 1 s of one channel.
///
/// A channel is the pair (metric_id, led_pd_pos); metric_id 121..127 selects
/// the colour. Each quarter second q uses led[q] and gain[q].
/// @see decode_ppg2()
struct Ppg2Chunk {
  uint64_t timestamp_ms; ///< First sample; sample i at + i * 1000 / sample_rate_hz ms.
  uint8_t metric_id;     ///< 121..127.
  uint8_t index, /**< Chunk index */quality, /**< Quality */body_position; ///< Body position
  uint8_t led_pd_pos, /**< LED/photodiode position */offset, /**< Offset */exp; ///< Exposure
  std::array<uint8_t, 4> led, /**< LED current per quarter second */gain; ///< Gain per quarter second
  uint16_t sample_rate_hz; ///< 32, 64, 128, 256 or 512.
  /// 19-bit values, `raw + gain * 32768 - 16384`, with the 32768 rollover
  /// corrected per channel within one decode call.
  std::vector<int32_t> samples;
};

/// @brief One ECG chunk.
/// @see decode_ecg()
struct EcgChunk {
  uint64_t timestamp_ms; ///< First sample, from the chunk itself; sample i at + i * 1000 / EcgSampleRateHz ms.
  uint8_t index, /**< Chunk index */quality, /**< Quality */body_position; ///< Body position
  /// Raw 24-bit values; values above 0x3FFFFF are replaced by the previous one.
  std::vector<uint32_t> samples;
};

/// @brief Corsano's conversion of a raw ECG sample to microvolts.
inline int32_t ecg_microvolts(uint32_t raw) {
  int64_t v = raw;
  if (v > 0x20000)
    v = -(0x3FFFF - v);
  v = -v;
  if (v > 0x20000)
    v = -(0x3FFFF - v);
  return int32_t(double(v) * 1000 / 0x20000 / 15 * 1000);
}

/// @brief Decode FileId::Bioz data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @param state Optional, for decoding a file slice by slice (see OhrState).
/// @return Decoded chunks and consumed byte count. Never throws.
Decoded<BiozChunk> decode_bioz(std::span<const uint8_t> data,
                               OhrState *state = nullptr);
/// @brief Decode FileId::Acc data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @param state Optional, for decoding a file slice by slice (see OhrState).
/// @return Decoded chunks and consumed byte count. Never throws.
Decoded<AccChunk> decode_acc(std::span<const uint8_t> data,
                             OhrState *state = nullptr);
/// @brief Decode FileId::Ppg2 data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @param state Optional, for decoding a file slice by slice (see OhrState).
/// @return Decoded chunks (all channels, in file order) and consumed byte count. Never throws.
Decoded<Ppg2Chunk> decode_ppg2(std::span<const uint8_t> data,
                               OhrState *state = nullptr);
/// @brief Decode FileId::Ecg data.
/// @param data Raw file bytes as returned by Bracelet::download_file().
/// @param state Optional, for decoding a file slice by slice (see OhrState).
/// @return Decoded chunks and consumed byte count. Never throws.
Decoded<EcgChunk> decode_ecg(std::span<const uint8_t> data,
                             OhrState *state = nullptr);
/// @}

/// @}

} // namespace Corsano
