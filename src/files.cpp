#include "corsano/files.hpp"

#include "bytes.hpp"
#include "corsano/protocol.hpp" // check_crc

#include <algorithm>

namespace Corsano {

namespace {

// MARK: - Framing
// One splitter per framing kind; every decoder is a thin parse() on top.

// Fixed-size records, each ending in its own CRC-8. Locks on after up to three
// consecutive good records, then trusts each record until one fails.
template <class Record, class Parse>
Decoded<Record> split_crc8_records(std::span<const uint8_t> data,
                                   size_t record_size, Parse parse) {
  Decoded<Record> out;
  auto crc_ok = [&](size_t at) {
    return check_crc(data.subspan(at, record_size));
  };
  size_t pos = 0;
  bool locked = false;
  while (data.size() - pos >= record_size) {
    size_t remaining = data.size() - pos;
    bool ok = crc_ok(pos);
    if (ok && !locked) {
      if (remaining >= 2 * record_size)
        ok = ok && crc_ok(pos + record_size);
      if (remaining >= 3 * record_size)
        ok = ok && crc_ok(pos + 2 * record_size);
    }
    locked = ok;
    if (ok) {
      out.records.push_back(parse(data.data() + pos));
      pos += record_size;
    } else {
      pos += 1;
    }
  }
  out.consumed_bytes = pos;
  return out;
}

// "OHR" records: [OHR][len:u16][id:u8][payload len-1], no CRC. Body records
// (id 0x0F) carry one channel chunk; channel 0x1E sets the epoch time that the
// following chunks of the requested channels count up from (1 s each).
// parse(timestamp_ms, p, n) gets p -> [index][quality][body_position][format]
// [samples...] with n = sample bytes / sample_size. Stops before an incomplete
// record so consumed_bytes stays safe to erase.
template <class Chunk, class Parse>
Decoded<Chunk> split_ohr_chunks(std::span<const uint8_t> data,
                                std::span<const uint8_t> channel_ids,
                                size_t sample_size, Parse parse) {
  constexpr uint8_t TimeChannel = 0x1E;
  Decoded<Chunk> out;
  // A 0x1E record carries the time of the data chunk(s) just BEFORE it (seen
  // on FW 8.12: chunk, time, chunk, time...). Chunks wait in `pending` until
  // timed; earlier ones in a group are 1 s apart.
  uint64_t last_s = 0;
  size_t pending = 0;
  auto stamp_back = [&](uint64_t t) {
    for (size_t k = 0; k < pending; ++k)
      out.records[out.records.size() - 1 - k].timestamp_ms = (t - k) * 1000;
    pending = 0;
  };
  size_t pos = 0;
  while (data.size() - pos >= 5) {
    const uint8_t *r = data.data() + pos;
    size_t len = rd16(r + 3);
    if (r[0] != 'O' || r[1] != 'H' || r[2] != 'R' || len == 0 || len > 4096) {
      pos += 1;
      continue;
    }
    if (data.size() - pos < 5 + len)
      break;
    const uint8_t *p = r + 6; // payload, len - 1 bytes
    size_t plen = len - 1;
    if (r[5] == 0x0A && plen >= 16) {
      last_s = rd32(p + 12); // file start time: fallback for untimed tails
    } else if (r[5] == 0x0F && plen >= 3 && 3 + size_t(rd16(p + 1)) <= plen) {
      size_t clen = rd16(p + 1);
      if (p[0] == TimeChannel && clen >= 6) {
        last_s = rd32(p + 5);
        stamp_back(last_s);
      } else if (std::find(channel_ids.begin(), channel_ids.end(), p[0]) !=
                     channel_ids.end() &&
                 clen >= 4) {
        out.records.push_back(parse(0, p + 3, (clen - 4) / sample_size));
        ++pending;
      }
    }
    pos += 5 + len;
  }
  // Trailing chunks with no time record yet: extrapolate 1 s apart
  if (last_s)
    for (size_t k = 0; k < pending; ++k)
      out.records[out.records.size() - pending + k].timestamp_ms =
          (last_s + 1 + k) * 1000;
  out.consumed_bytes = pos;
  return out;
}

// IEEE CRC-32 (reflected 0xEDB88320, init and xorout 0xFFFFFFFF), bitwise
uint32_t crc32(std::span<const uint8_t> data) {
  uint32_t crc = 0xFFFFFFFF;
  for (uint8_t b : data) {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
  }
  return ~crc;
}

// Chunks that start with a sync marker followed by a version byte, which
// fixes the chunk size (chunk_size_for returns 0 for unknown versions). The
// last 4 bytes are a little-endian CRC-32 of the rest. Stops before an
// incomplete chunk so consumed_bytes stays safe to erase.
template <class Chunk, class SizeFor, class Parse>
Decoded<Chunk> split_marker_chunks(std::span<const uint8_t> data,
                                   std::span<const uint8_t> marker,
                                   SizeFor chunk_size_for, Parse parse) {
  Decoded<Chunk> out;
  size_t pos = 0;
  while (data.size() - pos >= marker.size() + 1) {
    auto at = data.subspan(pos);
    size_t size = 0;
    if (std::equal(marker.begin(), marker.end(), at.begin()))
      size = chunk_size_for(at[marker.size()]);
    if (size == 0) {
      pos += 1;
      continue;
    }
    if (at.size() < size)
      break;
    if (crc32(at.first(size - 4)) != rd32(&at[size - 4])) {
      pos += 1;
      continue;
    }
    out.records.push_back(parse(at.first(size)));
    pos += size;
  }
  out.consumed_bytes = pos;
  return out;
}

// MARK: - Activity

constexpr size_t ActivityRecordSize = 38;

ActivityRecord parse_activity(const uint8_t *p) {
  ActivityRecord r{};
  r.timestamp_s = rd32(p);
  r.heart_rate = p[4];
  r.heart_rate_quality = p[5];
  r.steps = rd16(p + 6);
  r.activity_type = p[8];
  r.activity_count = p[9];
  r.spo2 = p[10];
  r.energy = rd16(p + 11);
  r.respiration_rate = p[13] / 4.0f;
  r.respiration_rate_quality = p[14];
  int battery = int8_t(p[15]);
  r.charging = battery < 0;
  r.battery_percent = uint8_t(battery < 0 ? -battery : battery);
  r.nibp_systolic = p[16];
  r.nibp_diastolic = p[17];
  r.spo2_quality = p[18];
  r.temp1_c = int16_t(rd16(p + 31)) / 100.0f;
  r.temp2_c = int16_t(rd16(p + 33)) / 100.0f;
  r.wearing = p[36];
  return r;
}

// MARK: - Stress

constexpr size_t StressRecordSize = 18;

StressRecord parse_stress(const uint8_t *p) {
  StressRecord r{};
  r.timestamp_s = rd32(p);
  r.cognitive_zone = p[4];
  r.predicted_cognitive_zone = p[5];
  r.predicted_transition_time = rd16(p + 6);
  r.cognitive_zone_hour = p[8];
  r.cortisol_contribution = rd32(p + 9);
  r.cognitive_zone_quality = p[13];
  r.skin_conductance_stress = rd16(p + 14);
  r.skin_conductance_stress_quality = p[16];
  return r;
}

// MARK: - PPG

constexpr uint8_t PpgMarker[] = {0x00, 0xFF, 0xAA, 0x4D, 0x4D, 0x54};
constexpr size_t PpgHeaderSize = 27;
constexpr size_t PpgSizeOverhead = 31; // header + crc32

size_t ppg_chunk_size(uint8_t version) {
  return version == 2 ? 256 : version == 3 ? 481 : 0;
}

// 8-bit compressed motion magnitude back to its nominal range
uint16_t expand_acc(uint8_t v) {
  if (v >= 255)
    return 617;
  if (v >= 150)
    return uint16_t((v - 150) * 4 + 200);
  if (v >= 100)
    return uint16_t((v - 100) * 2 + 100);
  return v;
}

PpgChunk parse_ppg(std::span<const uint8_t> c) {
  PpgChunk r{};
  const uint8_t *p = c.data();
  r.protocol_version = p[6];
  size_t sample_size = r.protocol_version == 3 ? 6 : 3;
  size_t n =
      std::min<size_t>(rd16(p + 9), (c.size() - PpgSizeOverhead) / sample_size);
  r.index = rd32(p + 11);
  r.timestamp_ms = uint64_t(rd32(p + 15)) * 1000 + rd16(p + 19);
  for (size_t i = 0; i < 3; ++i) {
    r.heart_rate[i] = p[21 + 2 * i];
    r.heart_rate_quality[i] = p[22 + 2 * i];
  }
  r.samples.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *s = p + PpgHeaderSize + i * sample_size;
    PpgSample out{rd16(s), expand_acc(s[2]), 0, 0, 0};
    if (sample_size == 6) {
      out.acc_x = int16_t(int8_t(s[3]) * 4);
      out.acc_y = int16_t(int8_t(s[4]) * 4);
      out.acc_z = int16_t(int8_t(s[5]) * 4);
    }
    r.samples.push_back(out);
  }
  return r;
}

// MARK: - OHR (BioZ, Acc)

constexpr uint8_t BiozChannels[] = {0x3D, 0x3E}; // Corsano docs say 0x3E
constexpr uint8_t AccChannels[] = {0x2B};

BiozChunk parse_bioz(uint64_t ts, const uint8_t *p, size_t n) {
  BiozChunk r{ts, p[0], p[1], p[2], {}};
  r.samples.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *s = p + 4 + 3 * i;
    r.samples.push_back(uint32_t(s[0]) | (uint32_t(s[1]) << 8) |
                        (uint32_t(s[2]) << 16));
  }
  return r;
}

AccChunk parse_acc(uint64_t ts, const uint8_t *p, size_t n) {
  AccChunk r{ts, p[0], p[1], p[2], {}};
  r.samples.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *s = p + 4 + 6 * i;
    r.samples.push_back(
        {int16_t(rd16(s)), int16_t(rd16(s + 2)), int16_t(rd16(s + 4))});
  }
  return r;
}

} // namespace

// MARK: - Activity

Decoded<ActivityRecord> decode_activity(std::span<const uint8_t> data) {
  return split_crc8_records<ActivityRecord>(data, ActivityRecordSize,
                                            parse_activity);
}

// MARK: - Stress

Decoded<StressRecord> decode_stress(std::span<const uint8_t> data) {
  return split_crc8_records<StressRecord>(data, StressRecordSize, parse_stress);
}

// MARK: - PPG

Decoded<PpgChunk> decode_ppg(std::span<const uint8_t> data) {
  return split_marker_chunks<PpgChunk>(data, PpgMarker, ppg_chunk_size,
                                       parse_ppg);
}

// MARK: - BioZ

Decoded<BiozChunk> decode_bioz(std::span<const uint8_t> data) {
  return split_ohr_chunks<BiozChunk>(data, BiozChannels, 3, parse_bioz);
}

// MARK: - Acc

Decoded<AccChunk> decode_acc(std::span<const uint8_t> data) {
  return split_ohr_chunks<AccChunk>(data, AccChannels, 6, parse_acc);
}

} // namespace Corsano
