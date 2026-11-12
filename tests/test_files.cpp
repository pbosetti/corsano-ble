// Offline tests for the recorded-file decoders (files.hpp), built from the
// byte layouts in docs/PROTOCOL.md, not from the implementation.
#include "corsano/protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

using namespace Corsano;
using Catch::Approx;
using Bytes = std::vector<uint8_t>;

// MARK: - Helpers

// Independent CRC-8 (poly 0x07, init 0xFF, no reflection, no xorout)
static uint8_t ref_crc8(const uint8_t *p, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                         : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

// Independent CRC-32/IEEE (reflected 0xEDB88320, init/xorout 0xFFFFFFFF)
static uint32_t ref_crc32(const uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
  }
  return ~crc;
}

static void put_u16(Bytes &b, unsigned v) {
  b.push_back(v & 0xFF);
  b.push_back((v >> 8) & 0xFF);
}

static void put_u32(Bytes &b, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b.push_back((v >> (8 * i)) & 0xFF);
}

static Bytes concat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (auto &p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}

struct RecordSpec {
  uint32_t timestamp_s = 1000;
  uint8_t hr = 70, hr_q = 4;
  uint16_t steps = 123;
  uint8_t activity_type = 2, activity_count = 9;
  uint8_t spo2 = 97, spo2_q = 3;
  uint16_t energy = 321;
  uint8_t resp_raw = 60, resp_q = 2; // 60 / 4 = 15 breaths/min
  int8_t battery = 80;
  uint8_t nibp_sys = 120, nibp_dia = 80;
  int16_t temp1 = 3650, temp2 = 3120; // /100 -> degC
  uint8_t wearing = 1;
};

static Bytes make_record(const RecordSpec &s = {}) {
  Bytes r;
  put_u32(r, s.timestamp_s);                    // 0
  r.push_back(s.hr);                            // 4
  r.push_back(s.hr_q);                          // 5
  put_u16(r, s.steps);                          // 6
  r.push_back(s.activity_type);                 // 8
  r.push_back(s.activity_count);                // 9
  r.push_back(s.spo2);                          // 10
  put_u16(r, s.energy);                         // 11
  r.push_back(s.resp_raw);                      // 13
  r.push_back(s.resp_q);                        // 14
  r.push_back(static_cast<uint8_t>(s.battery)); // 15
  r.push_back(s.nibp_sys);                      // 16
  r.push_back(s.nibp_dia);                      // 17
  r.push_back(s.spo2_q);                        // 18
  r.resize(31, 0);                              // 19..30 reserved
  put_u16(r, static_cast<uint16_t>(s.temp1));   // 31
  put_u16(r, static_cast<uint16_t>(s.temp2));   // 33
  r.push_back(0);                               // 35 reserved
  r.push_back(s.wearing);                       // 36
  r.push_back(ref_crc8(r.data(), r.size()));    // 37
  return r;
}

// Record whose timestamp identifies it (1000 + index)
static Bytes make_indexed_record(uint32_t index) {
  RecordSpec s;
  s.timestamp_s = 1000 + index;
  return make_record(s);
}

// MARK: - CRC

// MARK: - Activity records

TEST_CASE("decode_activity field decoding", "[files][activity]") {
  RecordSpec s;
  s.timestamp_s = 0xAABBCCDD;
  auto d = decode_activity(make_record(s));
  REQUIRE(d.records.size() == 1);
  CHECK(d.consumed_bytes == 38);
  const auto &r = d.records[0];
  CHECK(r.timestamp_s == 0xAABBCCDDu);
  CHECK(r.heart_rate == 70);
  CHECK(r.heart_rate_quality == 4);
  CHECK(r.steps == 123);
  CHECK(r.activity_type == 2);
  CHECK(r.activity_count == 9);
  CHECK(r.spo2 == 97);
  CHECK(r.spo2_quality == 3);
  CHECK(r.energy == 321);
  CHECK(r.respiration_rate == Approx(15.0f));
  CHECK(r.respiration_rate_quality == 2);
  CHECK(r.battery_percent == 80);
  CHECK_FALSE(r.charging);
  CHECK(r.nibp_systolic == 120);
  CHECK(r.nibp_diastolic == 80);
  CHECK(r.temp1_c == Approx(36.50f));
  CHECK(r.temp2_c == Approx(31.20f));
  CHECK(r.wearing == 1);
}

TEST_CASE("decode_activity signed fields", "[files][activity]") {
  RecordSpec s;
  s.battery = -42;
  s.temp1 = -150;  // -1.50 degC
  s.temp2 = -3000; // -30.00 degC
  s.resp_raw = 6;  // 1.5 breaths/min
  auto d = decode_activity(make_record(s));
  REQUIRE(d.records.size() == 1);
  const auto &r = d.records[0];
  CHECK(r.battery_percent == 42);
  CHECK(r.charging);
  CHECK(r.temp1_c == Approx(-1.5f));
  CHECK(r.temp2_c == Approx(-30.0f));
  CHECK(r.respiration_rate == Approx(1.5f));
}

TEST_CASE("decode_activity splitting", "[files][activity]") {
  Bytes r0 = make_indexed_record(0), r1 = make_indexed_record(1),
        r2 = make_indexed_record(2);

  SECTION("empty input") {
    auto d = decode_activity({});
    CHECK(d.records.empty());
    CHECK(d.consumed_bytes == 0);
  }
  SECTION("three clean records") {
    auto d = decode_activity(concat({r0, r1, r2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.consumed_bytes == 114);
    for (uint32_t i = 0; i < 3; ++i)
      CHECK(d.records[i].timestamp_s == 1000 + i);
  }
  SECTION("5 garbage bytes before three records") {
    Bytes garbage{0xA5, 0x5A, 0x11, 0x22, 0x33};
    auto d = decode_activity(concat({garbage, r0, r1, r2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.consumed_bytes == 119);
    CHECK(d.records[0].timestamp_s == 1000);
  }
  SECTION("trailing partial record is neither decoded nor consumed") {
    Bytes partial(r2.begin(), r2.begin() + 20);
    auto d = decode_activity(concat({r0, r1, r2, partial}));
    REQUIRE(d.records.size() == 3);
    // loop stops with 20 bytes remaining (< 38)
    CHECK(d.consumed_bytes == 114);
  }
  SECTION("corrupted record is skipped and the walk resyncs") {
    // Six records, #3 corrupted. Records 0..2 lock on (3 good in a row) and
    // decode; #3 fails and advances byte by byte until #4 and #5 verify.
    Bytes bad = make_indexed_record(3);
    bad[4] ^= 0xFF;
    auto d = decode_activity(concat(
        {r0, r1, r2, bad, make_indexed_record(4), make_indexed_record(5)}));
    REQUIRE(d.records.size() == 5);
    CHECK(d.records[0].timestamp_s == 1000);
    CHECK(d.records[1].timestamp_s == 1001);
    CHECK(d.records[2].timestamp_s == 1002);
    CHECK(d.records[3].timestamp_s == 1004);
    CHECK(d.records[4].timestamp_s == 1005);
    CHECK(d.consumed_bytes == 228);
  }
  SECTION("lock-on lookahead drops good records before a bad one") {
    // r0 and r1 sit in the 3-record lookahead window of the bad r2, so they
    // are not locked and are skipped. Only the final record is decoded.
    Bytes bad = r2;
    bad[4] ^= 0xFF;
    auto d = decode_activity(concat({r0, r1, bad, make_indexed_record(3)}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.records[0].timestamp_s == 1003);
    CHECK(d.consumed_bytes == 152);
  }
}

// MARK: - Stress records

struct StressSpec {
  uint32_t timestamp_s = 2000;
  uint8_t zone = 2, predicted_zone = 3;
  uint16_t transition = 0x0102;
  uint8_t hour = 14;
  uint32_t cortisol = 0x11223344;
  uint8_t zone_q = 5;
  uint16_t sc_stress = 0xA1B2;
  uint8_t sc_q = 7;
};

static Bytes make_stress(const StressSpec &s = {}) {
  Bytes r;
  put_u32(r, s.timestamp_s);                 // 0
  r.push_back(s.zone);                       // 4
  r.push_back(s.predicted_zone);             // 5
  put_u16(r, s.transition);                  // 6
  r.push_back(s.hour);                       // 8
  put_u32(r, s.cortisol);                    // 9
  r.push_back(s.zone_q);                     // 13
  put_u16(r, s.sc_stress);                   // 14
  r.push_back(s.sc_q);                       // 16
  r.push_back(ref_crc8(r.data(), r.size())); // 17
  return r;
}

static Bytes make_indexed_stress(uint32_t index) {
  StressSpec s;
  s.timestamp_s = 2000 + index;
  return make_stress(s);
}

TEST_CASE("decode_stress field decoding", "[files][stress]") {
  StressSpec s;
  s.timestamp_s = 0xAABBCCDD;
  auto d = decode_stress(make_stress(s));
  REQUIRE(d.records.size() == 1);
  CHECK(d.consumed_bytes == 18);
  const auto &r = d.records[0];
  CHECK(r.timestamp_s == 0xAABBCCDDu);
  CHECK(r.cognitive_zone == 2);
  CHECK(r.predicted_cognitive_zone == 3);
  CHECK(r.predicted_transition_time == 0x0102);
  CHECK(r.cognitive_zone_hour == 14);
  CHECK(r.cortisol_contribution == 0x11223344u);
  CHECK(r.cognitive_zone_quality == 5);
  CHECK(r.skin_conductance_stress == 0xA1B2);
  CHECK(r.skin_conductance_stress_quality == 7);
}

TEST_CASE("decode_stress splitting", "[files][stress]") {
  Bytes r0 = make_indexed_stress(0), r1 = make_indexed_stress(1),
        r2 = make_indexed_stress(2);

  SECTION("three clean records") {
    auto d = decode_stress(concat({r0, r1, r2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.consumed_bytes == 54);
    for (uint32_t i = 0; i < 3; ++i)
      CHECK(d.records[i].timestamp_s == 2000 + i);
  }
  SECTION("5 garbage bytes before three records") {
    Bytes garbage{0xA5, 0x5A, 0x11, 0x22, 0x33};
    auto d = decode_stress(concat({garbage, r0, r1, r2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.consumed_bytes == 59);
  }
  SECTION("corrupted record is skipped and the walk resyncs") {
    Bytes bad = make_indexed_stress(3);
    bad[4] ^= 0xFF;
    auto d = decode_stress(concat(
        {r0, r1, r2, bad, make_indexed_stress(4), make_indexed_stress(5)}));
    REQUIRE(d.records.size() == 5);
    CHECK(d.records[3].timestamp_s == 2004);
    CHECK(d.records[4].timestamp_s == 2005);
    CHECK(d.consumed_bytes == 108);
  }
}

// MARK: - PPG

static const uint8_t Marker[] = {0x00, 0xFF, 0xAA, 0x4D, 0x4D, 0x54};

struct PpgSpec {
  uint8_t version = 2;
  uint16_t n_samples = 75;
  uint32_t index = 7;
  uint32_t ts_s = 1700000000;
  uint16_t ts_ms = 345;
  std::array<uint8_t, 6> hr = {61, 4, 62, 3, 63, 2}; // hr0, q0, hr1, q1, ...
  // sample i: ppg = 1000 + i, acc byte = acc(i), xyz = (i - 40, 5 - i, -128)
  uint8_t (*acc)(size_t) = [](size_t i) { return uint8_t(i); };
};

static Bytes make_ppg(const PpgSpec &s = {}) {
  size_t size = s.version == 2 ? 256 : 481;
  size_t sample_size = s.version == 2 ? 3 : 6;
  Bytes c(Marker, Marker + 6);
  c.push_back(s.version); // 6
  c.push_back(0);         // 7..8 reserved
  c.push_back(0);
  put_u16(c, s.n_samples);                       // 9
  put_u32(c, s.index);                           // 11
  put_u32(c, s.ts_s);                            // 15
  put_u16(c, s.ts_ms);                           // 19
  c.insert(c.end(), s.hr.begin(), s.hr.end());   // 21
  for (size_t i = 0; c.size() < size - 4; ++i) { // 27: samples, zero padded
    if (i < 75) {
      put_u16(c, 1000 + i);
      c.push_back(s.acc(i));
      if (sample_size == 6) {
        c.push_back(uint8_t(int8_t(int(i) - 40)));
        c.push_back(uint8_t(int8_t(5 - int(i))));
        c.push_back(uint8_t(int8_t(-128)));
      }
    } else {
      c.push_back(0);
    }
  }
  put_u32(c, ref_crc32(c.data(), c.size()));
  return c;
}

static Bytes make_ppg_idx(uint32_t index, uint8_t version = 2) {
  PpgSpec s;
  s.index = index;
  s.version = version;
  return make_ppg(s);
}

TEST_CASE("decode_ppg crc32 check value", "[files][ppg]") {
  // crc32 is internal: check the reference, then that the decoder accepts a
  // chunk sealed with it
  const char *s = "123456789";
  CHECK(ref_crc32(reinterpret_cast<const uint8_t *>(s), 9) == 0xCBF43926u);
  // The implementation agrees with the reference on a real chunk
  auto d = decode_ppg(make_ppg());
  CHECK(d.records.size() == 1);
}

TEST_CASE("decode_ppg v2 header and samples", "[files][ppg]") {
  auto d = decode_ppg(make_ppg());
  REQUIRE(d.records.size() == 1);
  CHECK(d.consumed_bytes == 256);
  const auto &c = d.records[0];
  CHECK(c.protocol_version == 2);
  CHECK(c.index == 7);
  CHECK(c.timestamp_ms == 1700000000345ull);
  CHECK(c.heart_rate == std::array<uint8_t, 3>{61, 62, 63});
  CHECK(c.heart_rate_quality == std::array<uint8_t, 3>{4, 3, 2});
  REQUIRE(c.samples.size() == 75);
  for (size_t i = 0; i < 75; ++i) {
    CHECK(c.samples[i].ppg == 1000 + i);
    CHECK(c.samples[i].acc == i); // below 100 the expansion is identity
    CHECK(c.samples[i].acc_x == 0);
    CHECK(c.samples[i].acc_y == 0);
    CHECK(c.samples[i].acc_z == 0);
  }
}

TEST_CASE("decode_ppg v3 header and signed xyz", "[files][ppg]") {
  auto d = decode_ppg(make_ppg_idx(9, 3));
  REQUIRE(d.records.size() == 1);
  CHECK(d.consumed_bytes == 481);
  const auto &c = d.records[0];
  CHECK(c.protocol_version == 3);
  CHECK(c.index == 9);
  REQUIRE(c.samples.size() == 75);
  for (int i = 0; i < 75; ++i) {
    CHECK(c.samples[i].ppg == 1000 + i);
    CHECK(c.samples[i].acc_x == (i - 40) * 4);
    CHECK(c.samples[i].acc_y == (5 - i) * 4);
    CHECK(c.samples[i].acc_z == -512);
  }
}

TEST_CASE("decode_ppg acc expansion", "[files][ppg]") {
  // Sample i carries the boundary value table[i]
  static const uint8_t in[] = {0, 99, 100, 101, 149, 150, 151, 254, 255};
  static const uint16_t out[] = {0, 99, 100, 102, 198, 200, 204, 616, 617};
  PpgSpec s;
  s.acc = [](size_t i) { return i < 9 ? in[i] : uint8_t(0); };
  auto d = decode_ppg(make_ppg(s));
  REQUIRE(d.records.size() == 1);
  for (size_t i = 0; i < 9; ++i)
    CHECK(d.records[0].samples[i].acc == out[i]);
}

TEST_CASE("decode_ppg n_samples clamps to what fits", "[files][ppg]") {
  PpgSpec s;
  s.n_samples = 1000;
  auto d = decode_ppg(make_ppg(s));
  REQUIRE(d.records.size() == 1);
  CHECK(d.records[0].samples.size() == 75); // (256 - 31) / 3
  s.n_samples = 10;
  d = decode_ppg(make_ppg(s));
  CHECK(d.records[0].samples.size() == 10);
  s.version = 3;
  s.n_samples = 1000;
  d = decode_ppg(make_ppg(s));
  CHECK(d.records[0].samples.size() == 75); // (481 - 31) / 6
}

TEST_CASE("decode_ppg splitting", "[files][ppg]") {
  Bytes c0 = make_ppg_idx(0), c1 = make_ppg_idx(1, 3), c2 = make_ppg_idx(2);

  SECTION("empty input") {
    auto d = decode_ppg({});
    CHECK(d.records.empty());
    CHECK(d.consumed_bytes == 0);
  }
  SECTION("two chunks of different versions") {
    auto d = decode_ppg(concat({c0, c1}));
    REQUIRE(d.records.size() == 2);
    CHECK(d.consumed_bytes == 256 + 481);
    CHECK(d.records[0].index == 0);
    CHECK(d.records[1].index == 1);
  }
  SECTION("garbage prefix is consumed") {
    Bytes garbage{0x00, 0xFF, 0xAA, 0x01, 0x02, 0x03, 0x04};
    auto d = decode_ppg(concat({garbage, c0}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.consumed_bytes == garbage.size() + 256);
  }
  SECTION("corrupted chunk is skipped, next one decoded") {
    Bytes bad = c1;
    bad[100] ^= 0xFF;
    auto d = decode_ppg(concat({bad, c2}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.records[0].index == 2);
    CHECK(d.consumed_bytes == 481 + 256);
  }
  SECTION("truncated trailing chunk is not consumed") {
    Bytes partial(c2.begin(), c2.begin() + 200);
    auto d = decode_ppg(concat({c0, c1, partial}));
    REQUIRE(d.records.size() == 2);
    CHECK(d.consumed_bytes == 256 + 481);
  }
  SECTION("marker with unknown version is skipped") {
    Bytes fake(Marker, Marker + 6);
    fake.push_back(9);
    fake.resize(300, 0);
    auto d = decode_ppg(concat({fake, c0}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.consumed_bytes == 300 + 256);
  }
}

// MARK: - OHR (BioZ, Acc)

namespace {

// "OHR" record: len counts id + payload
Bytes ohr(uint8_t id, const Bytes &payload) {
  Bytes r{'O', 'H', 'R'};
  put_u16(r, unsigned(payload.size() + 1));
  r.push_back(id);
  r.insert(r.end(), payload.begin(), payload.end());
  return r;
}

Bytes ohr_header(uint32_t start_s) {
  Bytes p(16, 0);
  for (int i = 0; i < 4; ++i)
    p[12 + i] = uint8_t(start_s >> (8 * i));
  return ohr(0x0A, p);
}

Bytes ohr_time(uint32_t ts) {
  Bytes p{0x1E, 0x06, 0x00, 0x00, 0x04};
  for (int i = 0; i < 4; ++i)
    p.push_back(uint8_t(ts >> (8 * i)));
  return ohr(0x0F, p);
}

Bytes ohr_chunk(uint8_t channel, uint8_t format, uint8_t index,
                const Bytes &samples) {
  Bytes p{channel};
  put_u16(p, unsigned(samples.size() + 4));
  p.insert(p.end(), {index, 4, 2, format});
  p.insert(p.end(), samples.begin(), samples.end());
  return ohr(0x0F, p);
}

Bytes bioz_samples(uint32_t first, size_t n = 25) {
  Bytes s;
  for (size_t i = 0; i < n; ++i)
    for (int b = 0; b < 3; ++b)
      s.push_back(uint8_t((first + i) >> (8 * b)));
  return s;
}

Bytes load(const char *name) {
  std::ifstream f(std::string(CORSANO_TEST_DATA) + "/" + name,
                  std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

} // namespace

TEST_CASE("decode_bioz synthetic", "[files][bioz]") {
  Bytes h = ohr_header(1000), t = ohr_time(2000);
  Bytes c0 = ohr_chunk(0x3D, 1, 0, bioz_samples(100));
  Bytes c1 = ohr_chunk(0x3D, 1, 1, bioz_samples(200));
  Bytes c2 = ohr_chunk(0x3D, 1, 2, bioz_samples(300));

  SECTION("the header starts a clock that advances 1 s per chunk") {
    auto d = decode_bioz(concat({h, c0, c1, c2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.consumed_bytes == h.size() + c0.size() + c1.size() + c2.size());
    for (size_t i = 0; i < 3; ++i) {
      CHECK(d.records[i].timestamp_ms == 1000000 + 1000 * i);
      CHECK(d.records[i].index == i);
      CHECK(d.records[i].quality == 4);
      CHECK(d.records[i].body_position == 2);
      CHECK(d.records[i].samples.size() == 25);
      CHECK(d.records[i].adc.empty());
    }
    CHECK(d.records[1].samples[0] == 200);
    CHECK(d.records[2].samples[24] == 324);
  }
  SECTION("a time record within 6 s does not move the clock") {
    auto d = decode_bioz(concat({h, c0, ohr_time(1006), c1}));
    REQUIRE(d.records.size() == 2);
    CHECK(d.records[1].timestamp_ms == 1001000);
  }
  SECTION("a time record beyond 6 s resyncs the clock") {
    auto d = decode_bioz(concat({h, c0, t, c1, c2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.records[0].timestamp_ms == 1000000);
    CHECK(d.records[1].timestamp_ms == 2000000);
    CHECK(d.records[2].timestamp_ms == 2001000);
  }
  SECTION("chunks before any time info are stamped back from the first time record") {
    // FW 8.12 order: a time record follows the chunk it times
    auto d = decode_bioz(concat({c0, c1, t, c2}));
    REQUIRE(d.records.size() == 3);
    CHECK(d.records[0].timestamp_ms == 1999000);
    CHECK(d.records[1].timestamp_ms == 2000000);
    CHECK(d.records[2].timestamp_ms == 2001000);
  }
  SECTION("a header does not time chunks before it") {
    auto d = decode_bioz(concat({c0, h, c1}));
    REQUIRE(d.records.size() == 2);
    CHECK(d.records[0].timestamp_ms == 0);
    CHECK(d.records[1].timestamp_ms == 1000000);
  }
  SECTION("ADC variant: 6-byte samples, raw u24 then signed ADC i24") {
    Bytes s;
    for (size_t i = 0; i < 25; ++i)
      s.insert(s.end(), {uint8_t(i), 0, 0, 0xFE, 0xFF, 0xFF}); // adc = -2
    auto d = decode_bioz(concat({h, ohr_chunk(0x3D, 1, 0, s), c1}));
    REQUIRE(d.records.size() == 2);
    REQUIRE(d.records[0].samples.size() == 25);
    REQUIRE(d.records[0].adc.size() == 25);
    CHECK(d.records[0].samples[24] == 24);
    CHECK(d.records[0].adc[0] == -2);
    CHECK(d.records[1].timestamp_ms == 1001000);
  }
  SECTION("normalized scaling") {
    CHECK(bioz_normalized(0) == Approx(0.5));
    CHECK(bioz_normalized(0x40000) == Approx(0.75));
    CHECK(bioz_normalized(0x80000) == Approx(0.5));
    CHECK(bioz_normalized(0xC0000) == Approx(0.25));
  }
  SECTION("no time info gives timestamp 0") {
    auto d = decode_bioz(c0);
    REQUIRE(d.records.size() == 1);
    CHECK(d.records[0].timestamp_ms == 0);
  }
  SECTION("24-bit values are unsigned") {
    auto d = decode_bioz(ohr_chunk(0x3D, 1, 0, {0xFF, 0xFF, 0xFF, 1, 0, 0}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.records[0].samples == std::vector<uint32_t>{0xFFFFFF, 1});
  }
  SECTION("channel 0x3E accepted, other channels ignored") {
    Bytes other = ohr_chunk(0x2B, 0x6E, 0, Bytes(6, 0));
    auto d = decode_bioz(concat({ohr_chunk(0x3E, 1, 7, bioz_samples(1)), other}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.records[0].index == 7);
    CHECK(decode_bioz(other).consumed_bytes == other.size());
  }
  SECTION("incomplete trailing record is not consumed") {
    Bytes partial(c2.begin(), c2.begin() + 20);
    auto d = decode_bioz(concat({t, c0, partial}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.consumed_bytes == t.size() + c0.size());
    Bytes tiny{'O', 'H'};
    CHECK(decode_bioz(tiny).consumed_bytes == 0);
  }
  SECTION("garbage is resynchronised") {
    Bytes junk{1, 2, 'O', 'H', 'R', 0, 0, 9, 'O'};
    auto d = decode_bioz(concat({junk, c0}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.consumed_bytes == junk.size() + c0.size());
  }
  SECTION("chunk longer than its record is skipped") {
    Bytes bad = ohr(0x0F, {0x3D, 0xFF, 0x00, 0, 0, 0, 1});
    auto d = decode_bioz(concat({bad, c0}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.consumed_bytes == bad.size() + c0.size());
  }
}

TEST_CASE("decode_acc synthetic", "[files][acc]") {
  Bytes s;
  for (int16_t v : {int16_t(12), int16_t(-8), int16_t(-514), int16_t(-32768),
                    int16_t(32767), int16_t(-1)}) {
    s.push_back(uint8_t(v & 0xFF));
    s.push_back(uint8_t((v >> 8) & 0xFF));
  }
  auto d = decode_acc(concat({ohr_chunk(0x2B, 0x6E, 3, s), ohr_time(5000),
                              ohr_chunk(0x3D, 1, 0, bioz_samples(1))}));
  REQUIRE(d.records.size() == 1);
  CHECK(d.records[0].timestamp_ms == 5000000);
  REQUIRE(d.records[0].samples.size() == 2);
  CHECK(d.records[0].samples[0] == std::array<int16_t, 3>{12, -8, -514});
  CHECK(d.records[0].samples[1] == std::array<int16_t, 3>{-32768, 32767, -1});
}

TEST_CASE("decode_acc resyncs beyond 1 s", "[files][acc]") {
  Bytes a = ohr_chunk(0x2B, 0x6E, 0, Bytes(6, 0));
  // clock at 5002 s when the time record arrives
  auto near = decode_acc(concat({ohr_header(5000), a, a, ohr_time(5003), a}));
  REQUIRE(near.records.size() == 3);
  CHECK(near.records[2].timestamp_ms == 5002000);
  auto far = decode_acc(concat({ohr_header(5000), a, a, ohr_time(5004), a}));
  REQUIRE(far.records.size() == 3);
  CHECK(far.records[2].timestamp_ms == 5004000);
}

namespace {

// metric, index, gains, then `rate` u16 samples at 128 Hz (format 0x62)
Bytes ppg2_chunk(uint8_t metric, uint8_t pd, std::array<uint8_t, 4> gain,
                 const std::vector<uint16_t> &v) {
  Bytes p{metric};
  put_u16(p, unsigned(15 + 2 * v.size()));
  p.insert(p.end(), {7, 4, 1, 0x62, pd, 0, 0, 10, 11, 12, 13});
  p.insert(p.end(), gain.begin(), gain.end());
  for (uint16_t x : v)
    put_u16(p, x);
  return p;
}

Bytes body(std::initializer_list<Bytes> metrics) {
  return ohr(0x0F, concat(metrics));
}

} // namespace

TEST_CASE("decode_ppg2 synthetic", "[files][ppg2]") {
  std::vector<uint16_t> flat(128, 20000);
  SECTION("several channels per record, one clock per (metric, led_pd_pos)") {
    Bytes rec = body({ppg2_chunk(126, 0x06, {0, 0, 0, 0}, flat),
                      ppg2_chunk(124, 0xB6, {0, 0, 0, 0}, flat)});
    auto d = decode_ppg2(concat({ohr_header(100), rec, rec}));
    REQUIRE(d.records.size() == 4);
    CHECK(d.records[0].metric_id == 126);
    CHECK(d.records[1].led_pd_pos == 0xB6);
    CHECK(d.records[0].timestamp_ms == 100000);
    CHECK(d.records[1].timestamp_ms == 100000);
    CHECK(d.records[2].timestamp_ms == 101000);
    CHECK(d.records[3].timestamp_ms == 101000);
    CHECK(d.records[0].sample_rate_hz == 128);
    CHECK(d.records[0].index == 7);
    CHECK(d.records[0].led == std::array<uint8_t, 4>{10, 11, 12, 13});
    REQUIRE(d.records[0].samples.size() == 128);
    CHECK(d.records[0].samples[0] == 20000 - 16384);
  }
  SECTION("gain applies per quarter second") {
    // steps of 2: a step of exactly 1 looks like a rollover and is folded back
    auto d = decode_ppg2(body({ppg2_chunk(123, 0x16, {0, 2, 4, 6}, flat)}));
    REQUIRE(d.records.size() == 1);
    auto &s = d.records[0].samples;
    CHECK(s[31] == 20000 - 16384);
    CHECK(s[32] == 20000 + 2 * 32768 - 16384);
    CHECK(s[64] == 20000 + 4 * 32768 - 16384);
    CHECK(s[127] == 20000 + 6 * 32768 - 16384);
  }
  SECTION("a 32768 jump is undone within a channel") {
    std::vector<uint16_t> v(128, 20000);
    v[1] = uint16_t(20000 + 100); // continues normally
    auto d = decode_ppg2(body({ppg2_chunk(123, 0x16, {0, 1, 1, 1}, v)}));
    auto &s = d.records[0].samples;
    CHECK(s[1] == 20100 - 16384);
    CHECK(s[32] == 20000 - 16384); // gain step of exactly 32768 folded back
  }
  SECTION("unknown format or short chunk is skipped") {
    Bytes bad = ppg2_chunk(123, 0x16, {0, 0, 0, 0}, flat);
    bad[6] = 0x70; // format byte
    Bytes ok = ppg2_chunk(126, 0x06, {0, 0, 0, 0}, flat);
    auto d = decode_ppg2(body({bad, ok}));
    REQUIRE(d.records.size() == 1);
    CHECK(d.records[0].metric_id == 126);
  }
}

TEST_CASE("decode_ecg synthetic", "[files][ecg]") {
  Bytes p{0x64};
  put_u16(p, 11 + 3 * 3);
  p.insert(p.end(), {5, 4, 1, 0x01, 3});
  for (int i = 0; i < 4; ++i)
    p.push_back(uint8_t(1800000000u >> (8 * i)));
  put_u16(p, 250);
  p.insert(p.end(), {0x10, 0, 0, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01});
  auto d = decode_ecg(concat({ohr_header(100), body({p})}));
  REQUIRE(d.records.size() == 1);
  auto &c = d.records[0];
  CHECK(c.timestamp_ms == 1800000000250ull); // own time, not the header clock
  CHECK(c.index == 5);
  CHECK(c.samples == std::vector<uint32_t>{0x10, 0x10, 0x10000}); // > 0x3FFFFF repeats
  CHECK(ecg_microvolts(0) == 0);
  CHECK(ecg_microvolts(0x10000) == -33333); // -0x10000 * 1e6 / 0x20000 / 15
  CHECK(ecg_microvolts(0x30000) == 33332);  // 0x3FFFF - 0x30000 = 0xFFFF
}

TEST_CASE("OHR real recordings", "[files][bioz][acc][ppg2][data]") {
  constexpr uint64_t Y2026 = 1767225600000ull, Y2027 = 1798761600000ull;
  SECTION("bioz") {
    Bytes data = load("bioz.bin");
    REQUIRE(!data.empty());
    auto d = decode_bioz(data);
    CHECK(d.consumed_bytes == data.size());
    REQUIRE(d.records.size() > 0);
    uint64_t prev = 0;
    for (auto &c : d.records) {
      CHECK(c.samples.size() == 25);
      CHECK(c.timestamp_ms >= prev);
      CHECK(c.timestamp_ms >= Y2026);
      CHECK(c.timestamp_ms < Y2027);
      prev = c.timestamp_ms;
    }
  }
  SECTION("acc") {
    Bytes data = load("acc.bin");
    REQUIRE(!data.empty());
    auto d = decode_acc(data);
    CHECK(d.consumed_bytes == data.size());
    REQUIRE(d.records.size() > 0);
    uint64_t prev = 0;
    for (auto &c : d.records) {
      CHECK(c.samples.size() == 32);
      CHECK(c.timestamp_ms >= prev);
      CHECK(c.timestamp_ms >= Y2026);
      CHECK(c.timestamp_ms < Y2027);
      prev = c.timestamp_ms;
    }
    // At rest: ~1 g (about 512 counts) on z
    for (size_t i = 0; i < 3 && i < d.records.size(); ++i)
      for (auto &s : d.records[i].samples)
        CHECK(std::abs(s[2]) > 400);
  }
  SECTION("ppg2") {
    Bytes data = load("ppg2.bin");
    REQUIRE(!data.empty());
    auto d = decode_ppg2(data);
    CHECK(d.consumed_bytes == data.size());
    REQUIRE(d.records.size() > 0);
    std::map<std::pair<int, int>, uint64_t> prev;
    for (auto &c : d.records) {
      CHECK(c.sample_rate_hz == 128);
      CHECK(c.samples.size() == 128);
      CHECK(c.timestamp_ms >= Y2026);
      CHECK(c.timestamp_ms < Y2027);
      auto &p = prev[{c.metric_id, c.led_pd_pos}];
      CHECK(c.timestamp_ms > p); // each channel strictly increasing
      p = c.timestamp_ms;
    }
    CHECK(prev.size() == 3);
  }
}

// Monitor-style decoding: the file grows by `step` bytes per poll, each poll
// decodes what is on the device with a state and erases what was consumed
template <class Decode>
static auto decode_live(const Bytes &data, size_t step, Decode decode) {
  OhrState st;
  decltype(decode(std::span<const uint8_t>(data), &st).records) all;
  size_t erased = 0;
  for (size_t end = 0; end < data.size() + step; end += step) {
    end = std::min(end, data.size());
    auto d = decode(std::span(data).subspan(erased, end - erased), &st);
    all.insert(all.end(), d.records.begin(), d.records.end());
    erased += d.consumed_bytes;
    if (end == data.size())
      break;
  }
  return all;
}

// Bytes from the first data record on: what a download after an erase sees
static Bytes from_record(const Bytes &data, size_t skip) {
  size_t pos = 0;
  for (size_t k = 0; k < skip; ++k)
    pos += 5 + (data[pos + 3] | (data[pos + 4] << 8));
  return Bytes(data.begin() + long(pos), data.end());
}

TEST_CASE("OHR slice-by-slice decoding matches whole-file decoding",
          "[files][ppg2][bioz][data]") {
  auto same = [](const auto &live, const auto &whole) {
    REQUIRE(live.size() <= whole.size()); // trailing untimed chunks held back
    REQUIRE(live.size() + 10 > whole.size());
    for (size_t i = 0; i < live.size(); ++i) {
      CHECK(live[i].timestamp_ms == whole[i].timestamp_ms);
      CHECK(live[i].samples == whole[i].samples);
    }
  };
  SECTION("ppg2 with header") {
    Bytes data = load("ppg2.bin");
    same(decode_live(data, 700, decode_ppg2), decode_ppg2(data).records);
  }
  SECTION("ppg2 after an erase: no header, untimed start") {
    Bytes data = from_record(load("ppg2.bin"), 4); // 0A 0B 0C and the first time
    auto whole = decode_ppg2(data).records;
    REQUIRE(whole.front().timestamp_ms > 0); // stamped back from a later time
    auto live = decode_live(data, 700, decode_ppg2);
    REQUIRE(!live.empty());
    for (auto &c : live)
      CHECK(c.timestamp_ms > 0);
    same(live, whole);
  }
  SECTION("bioz after an erase") {
    Bytes data = from_record(load("bioz.bin"), 3);
    same(decode_live(data, 100, decode_bioz), decode_bioz(data).records);
  }
}

