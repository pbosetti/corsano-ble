#include "corsano/files.hpp"

#include "bytes.hpp"
#include "corsano/protocol.hpp" // check_crc

#include <algorithm>
#include <map>
#include <optional>

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
// (id 0x0F) hold metric chunks [metric:u8][clen:u16][clen bytes]. parse(metric,
// p, clen) gets p -> [index][quality]... and returns the chunk plus its clock
// slot, or nothing to skip it. Stops before an incomplete record so
// consumed_bytes stays safe to erase.
//
// Timing as in Corsano's reference parser: one clock per slot key, advanced by
// each chunk's duration; the header (0x0A) and the time metric (0x1E) resync a
// clock only when it is off by more than a tolerance, so jittery time records
// do not produce uneven or duplicate timestamps. Extension: chunks seen before
// any time info wait in `pending` and are stamped back from the first 0x1E
// (on FW 8.12 a 0x1E follows the chunk it times; a download after an erase
// starts with untimed chunks).
struct OhrSlot {
  static constexpr uint32_t NoClock = ~0u; // chunk carries its own time
  uint32_t key;
  uint64_t duration_ms;
};

template <class Chunk, class Parse>
Decoded<Chunk> split_ohr_chunks(std::span<const uint8_t> data,
                                uint64_t time_tolerance_ms, OhrState *state,
                                Parse parse) {
  constexpr uint8_t TimeMetric = 0x1E;
  constexpr uint64_t HeaderToleranceMs = 6000;
  struct Clock {
    uint64_t next_ms = 0; // 0: not running yet
    std::vector<std::pair<size_t, uint64_t>> pending; // record, duration
  };
  Decoded<Chunk> out;
  std::map<uint32_t, Clock> clocks;
  uint64_t sync_ms = 0; // last header/time value, starts new clocks
  if (state) {
    for (auto [key, next] : state->clocks)
      clocks[key].next_ms = next;
    sync_ms = state->sync_ms;
  }
  // First record holding a still-untimed chunk: where a state'd call stops
  std::optional<std::pair<size_t, size_t>> hold; // byte pos, records before
  auto sync = [&](uint64_t t_ms, uint64_t tol_ms, bool time_record) {
    sync_ms = t_ms;
    hold.reset();
    for (auto &[key, c] : clocks) {
      if (c.next_ms == 0) {
        if (!time_record || c.pending.empty()) {
          c.pending.clear(); // a header starts a new session: earlier chunks stay 0
          c.next_ms = t_ms;
          continue;
        }
        uint64_t ts = t_ms; // the last pending chunk is the one being timed
        for (size_t k = c.pending.size(); k-- > 0;) {
          out.records[c.pending[k].first].timestamp_ms = ts;
          if (k > 0)
            ts -= std::min(ts, c.pending[k - 1].second);
        }
        c.next_ms = t_ms + c.pending.back().second;
        c.pending.clear();
      } else if (c.next_ms > t_ms + tol_ms || c.next_ms + tol_ms < t_ms) {
        c.next_ms = t_ms;
      }
    }
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
    size_t records_before = out.records.size();
    if (r[5] == 0x0A && plen >= 16) {
      sync(uint64_t(rd32(p + 12)) * 1000, HeaderToleranceMs, false);
    } else if (r[5] == 0x0F) {
      for (size_t j = 0; j + 3 <= plen;) {
        uint8_t metric = p[j];
        size_t clen = rd16(p + j + 1);
        if (j + 3 + clen > plen)
          break; // overruns its record: drop the rest of the record
        const uint8_t *c = p + j + 3;
        j += 3 + clen;
        if (metric == TimeMetric) {
          if (clen >= 6)
            sync(uint64_t(rd32(c + 2)) * 1000, time_tolerance_ms, true);
          continue;
        }
        auto parsed = parse(metric, c, clen);
        if (!parsed)
          continue;
        auto &[chunk, slot] = *parsed;
        if (slot.key != OhrSlot::NoClock) {
          auto [it, fresh] = clocks.try_emplace(slot.key);
          Clock &clk = it->second;
          if (fresh)
            clk.next_ms = sync_ms;
          chunk.timestamp_ms = clk.next_ms;
          if (clk.next_ms)
            clk.next_ms += slot.duration_ms;
          else {
            clk.pending.push_back({out.records.size(), slot.duration_ms});
            if (!hold)
              hold = {{pos, records_before}};
          }
        }
        out.records.push_back(std::move(chunk));
      }
    }
    pos += 5 + len;
  }
  out.consumed_bytes = pos;
  if (state) {
    if (hold) { // untimed: leave them for a slice that brings the time
      out.records.resize(hold->second);
      out.consumed_bytes = hold->first;
    }
    state->clocks.clear();
    for (auto &[key, c] : clocks)
      if (c.next_ms)
        state->clocks[key] = c.next_ms;
    state->sync_ms = sync_ms;
  }
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

// MARK: - OHR (BioZ, Acc, PPG2, ECG)

template <class Chunk> using OhrParsed = std::optional<std::pair<Chunk, OhrSlot>>;

uint32_t rd24(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}

int32_t rd24s(const uint8_t *p) {
  uint32_t v = rd24(p);
  return int32_t(v & 0x800000 ? v | 0xFF000000u : v);
}

// [index][quality][body_position][format][samples]
OhrParsed<BiozChunk> parse_bioz(uint8_t metric, const uint8_t *p, size_t clen) {
  constexpr size_t AdcVariantSize = 154; // 25 samples x 6 bytes + 4
  if ((metric != 0x3D && metric != 0x3E) || clen < 4) // Corsano docs say 0x3E
    return {};
  bool adc = clen >= AdcVariantSize;
  size_t n = (clen - 4) / (adc ? 6 : 3);
  BiozChunk r{0, p[0], p[1], p[2], {}, {}};
  r.samples.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *s = p + 4 + (adc ? 6 : 3) * i;
    r.samples.push_back(rd24(s));
    if (adc)
      r.adc.push_back(rd24s(s + 3));
  }
  return {{std::move(r), {0, uint64_t(n * 1000 / BiozSampleRateHz)}}};
}

OhrParsed<AccChunk> parse_acc(uint8_t metric, const uint8_t *p, size_t clen) {
  if (metric != 0x2B || clen < 4)
    return {};
  size_t n = (clen - 4) / 6;
  AccChunk r{0, p[0], p[1], p[2], {}};
  r.samples.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *s = p + 4 + 6 * i;
    r.samples.push_back(
        {int16_t(rd16(s)), int16_t(rd16(s + 2)), int16_t(rd16(s + 4))});
  }
  return {{std::move(r), {0, 1000}}};
}

// [index][quality][body_position][format][led_pd_pos][offset][exp][led x4]
// [gain x4][samples u16 x rate]; rate from the format code
OhrParsed<Ppg2Chunk> parse_ppg2(uint8_t metric, const uint8_t *p, size_t clen,
                                std::map<uint32_t, int32_t> &last) {
  constexpr size_t Header = 15;
  if (metric < 121 || metric > 127 || clen < Header || p[3] < 0x60 ||
      p[3] > 0x64)
    return {};
  uint16_t rate = uint16_t(32u << (p[3] - 0x60));
  if (Header + 2 * size_t(rate) > clen)
    return {};
  Ppg2Chunk r{0, metric, p[0], p[1], p[2], p[4], p[5], p[6], {}, {}, rate, {}};
  std::copy(p + 7, p + 11, r.led.begin());
  std::copy(p + 11, p + 15, r.gain.begin());
  uint32_t key = (uint32_t(metric) << 8) | r.led_pd_pos;
  int32_t &lv = last[key];
  r.samples.reserve(rate);
  for (size_t i = 0; i < rate; ++i) {
    int32_t v = int32_t(rd16(p + Header + 2 * i)) +
                int32_t(r.gain[i * 4 / rate]) * 32768 - 16384;
    if (lv > 0 && v > lv + 31768 && v < lv + 33768)
      v -= 32768;
    else if (lv > 0 && v < lv - 31768 && v > lv - 33768)
      v += 32768;
    lv = v;
    r.samples.push_back(v);
  }
  return {{std::move(r), {key, 1000}}};
}

// [index][quality][body_position][format][count:u8][time:u32 s][ms:u16]
// [samples u24 x count]
OhrParsed<EcgChunk> parse_ecg(uint8_t metric, const uint8_t *p, size_t clen) {
  constexpr size_t Header = 11;
  if (metric != 0x64 || clen < Header || Header + 3 * size_t(p[4]) > clen)
    return {};
  EcgChunk r{uint64_t(rd32(p + 5)) * 1000 + rd16(p + 9), p[0], p[1], p[2], {}};
  uint32_t prev = 0;
  for (size_t i = 0; i < p[4]; ++i) {
    uint32_t v = rd24(p + Header + 3 * i);
    if (v > 0x3FFFFF)
      v = prev;
    r.samples.push_back(prev = v);
  }
  return {{std::move(r), {OhrSlot::NoClock, 0}}};
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

// MARK: - OHR

Decoded<BiozChunk> decode_bioz(std::span<const uint8_t> data,
                               OhrState *state) {
  return split_ohr_chunks<BiozChunk>(data, 6000, state, parse_bioz);
}

Decoded<AccChunk> decode_acc(std::span<const uint8_t> data, OhrState *state) {
  return split_ohr_chunks<AccChunk>(data, 1000, state, parse_acc);
}

Decoded<Ppg2Chunk> decode_ppg2(std::span<const uint8_t> data,
                               OhrState *state) {
  std::map<uint32_t, int32_t> local; // rollover state per channel
  auto &last = state ? state->ppg2_last : local;
  auto before = last;
  auto d = split_ohr_chunks<Ppg2Chunk>(
      data, 6000, state, [&](uint8_t m, const uint8_t *p, size_t n) {
        return parse_ppg2(m, p, n, last);
      });
  // Still no time at all: every chunk was held back, so is its rollover state
  if (state && state->sync_ms == 0)
    last = before;
  return d;
}

Decoded<EcgChunk> decode_ecg(std::span<const uint8_t> data, OhrState *state) {
  return split_ohr_chunks<EcgChunk>(data, 6000, state, parse_ecg);
}

} // namespace Corsano
