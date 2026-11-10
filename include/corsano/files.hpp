// Decoders for files recorded on the bracelet (FileId), downloaded raw with
// Bracelet::download_file(). Every decoder follows one pattern:
//   struct <Name>Chunk { ... };                              one chunk/record
//   Decoded<<Name>Chunk> decode_<name>(std::span<const uint8_t> data);
// Decoders never throw on corrupt or truncated input: they skip bytes until
// the next valid chunk and stop before an incomplete one, so consumed_bytes
// is always safe to pass to Bracelet::erase_file().
// Byte layouts and framing: docs/PROTOCOL.md. Roadmap: docs/RAW_SIGNALS.md.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Corsano {

template <class Chunk> struct Decoded {
  std::vector<Chunk> records;
  size_t consumed_bytes = 0;
};

// MARK: - Activity (FileId::Activity)
// Fixed 38-byte records, trailing CRC-8

struct ActivityRecord {
  uint32_t timestamp_s;
  uint8_t heart_rate, heart_rate_quality;
  uint16_t steps;
  uint8_t activity_type, activity_count;
  uint8_t spo2, spo2_quality;
  uint16_t energy;
  float respiration_rate;
  uint8_t respiration_rate_quality;
  uint8_t battery_percent;
  bool charging;
  uint8_t nibp_systolic, nibp_diastolic;
  float temp1_c, temp2_c;
  uint8_t wearing;
};

Decoded<ActivityRecord> decode_activity(std::span<const uint8_t> data);

// MARK: - PPG (FileId::Ppg)
// Sync-marker chunks (256 B v2, 481 B v3), trailing CRC-32

inline constexpr std::chrono::milliseconds PpgSamplePeriod{40}; // 25 Hz
inline constexpr size_t PpgSamplesPerHeartRate = 25; // one estimate per 1 s

struct PpgSample {
  uint16_t ppg;                // raw optical value
  uint16_t acc;                // motion magnitude, SDK-expanded from 8 bit
  int16_t acc_x, acc_y, acc_z; // v3 only (0 in v2), raw int8 x 4
};

struct PpgChunk {
  uint8_t protocol_version; // 2 or 3
  uint32_t index;           // chunk counter
  uint64_t timestamp_ms;    // first sample; sample i at + i * PpgSamplePeriod
  // On-device estimates for samples [0,25), [25,50), [50,75)
  std::array<uint8_t, 3> heart_rate, heart_rate_quality;
  std::vector<PpgSample> samples; // nominally 75
};

Decoded<PpgChunk> decode_ppg(std::span<const uint8_t> data);

// MARK: - Stress (FileId::Stress)
// Fixed 18-byte records, trailing CRC-8 ("emography" in the SDK). Values are
// raw firmware units; the SDK does not document their scale.

struct StressRecord {
  uint32_t timestamp_s;
  uint8_t cognitive_zone, cognitive_zone_quality;
  uint8_t predicted_cognitive_zone;
  uint16_t predicted_transition_time;
  uint8_t cognitive_zone_hour;
  uint32_t cortisol_contribution;
  uint16_t skin_conductance_stress; // stress level from skin conductance
  uint8_t skin_conductance_stress_quality;
};

Decoded<StressRecord> decode_stress(std::span<const uint8_t> data);

// MARK: - OHR files (FileId::Bioz, FileId::Acc; PPG2 later)
// Records "OHR" + len:u16 + id; body records (id 0x0F) hold one channel chunk
// [channel:u8][len:u16][index][quality][body_position][format][samples...].
// Channel 0x1E chunks carry the u32 epoch time of the data chunk(s) just
// BEFORE them (1 s each; trailing untimed chunks are extrapolated). Chunks of
// 0xFF are erased flash and skipped. Documented by Corsano: developer.corsano.com
// android/data_models/raw_files; deviations noted in docs/PROTOCOL.md.

inline constexpr double BiozSampleRateHz = 25.0;
inline constexpr double AccSampleRateHz = 32.0;
inline constexpr double AccCountsPerG = 512.0; // observed at rest, unverified

struct BiozChunk {
  uint64_t timestamp_ms; // first sample; sample i at + i * 1000 / 25 ms
  uint8_t index, quality, body_position;
  std::vector<uint32_t> samples; // raw 24-bit, nominally 25 (1 s)
};

struct AccChunk {
  uint64_t timestamp_ms; // first sample; sample i at + i * 1000 / 32 ms
  uint8_t index, quality, body_position;
  std::vector<std::array<int16_t, 3>> samples; // raw x, y, z; nominally 32
};

Decoded<BiozChunk> decode_bioz(std::span<const uint8_t> data);
Decoded<AccChunk> decode_acc(std::span<const uint8_t> data);

} // namespace Corsano
