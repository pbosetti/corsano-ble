// Offline codec tests. Expected values are derived from docs/PROTOCOL.md.
#include "corsano/protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <string_view>

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

static void put_u16(Bytes &b, unsigned v) {
  b.push_back(v & 0xFF);
  b.push_back((v >> 8) & 0xFF);
}

static void put_u32(Bytes &b, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b.push_back((v >> (8 * i)) & 0xFF);
}

// id + payload + crc8 over everything before it
static Bytes make_frame(uint8_t id, const Bytes &payload) {
  Bytes f{id};
  f.insert(f.end(), payload.begin(), payload.end());
  f.push_back(ref_crc8(f.data(), f.size()));
  return f;
}

// MARK: - CRC

TEST_CASE("crc8 matches the spec check value", "[protocol][crc]") {
  constexpr std::string_view s = "123456789";
  std::span<const uint8_t> data(reinterpret_cast<const uint8_t *>(s.data()),
                                s.size());
  CHECK(crc8(data) == 0xFB);
  CHECK(crc8({}) == 0xFF);
}

TEST_CASE("check_crc", "[protocol][crc]") {
  Bytes f = make_frame(50, {1, 2, 3, 4});
  CHECK(check_crc(f));

  SECTION("corrupted payload") {
    f[2] ^= 0x01;
    CHECK_FALSE(check_crc(f));
  }
  SECTION("corrupted crc") {
    f.back() ^= 0x80;
    CHECK_FALSE(check_crc(f));
  }
  SECTION("frames shorter than 2 bytes are accepted") {
    CHECK(check_crc(Bytes{}));
    CHECK(check_crc(Bytes{0x42}));
  }
}

// MARK: - Requests

TEST_CASE("encode_request is id then payload, no CRC", "[protocol][encode]") {
  CHECK(encode_request(50) == Bytes{50});
  Bytes payload{2, 0x10, 0x20};
  CHECK(encode_request(46, payload) == Bytes{46, 2, 0x10, 0x20});
}

TEST_CASE("encode_set_time", "[protocol][encode]") {
  using namespace std::chrono;
  using namespace std::chrono_literals;
  auto tp = time_point_cast<system_clock::duration>(
      sys_days{year{2026} / 11 / 8} + 21h + 5min + 9s);

  CHECK(encode_set_time(tp, 0min) == Bytes{26, 11, 8, 21, 5, 9, 0x00, 0x00});
  CHECK(encode_set_time(tp, 60min) == Bytes{26, 11, 8, 21, 5, 9, 0x3C, 0x00});
  // -300 = 0xFED4 as int16, little endian
  CHECK(encode_set_time(tp, -300min) == Bytes{26, 11, 8, 21, 5, 9, 0xD4, 0xFE});
}

// MARK: - Payload decoders

TEST_CASE("decode_battery", "[protocol][decode]") {
  // 3892 mV = 0x0F34
  auto b = decode_battery(Bytes{87, 0x34, 0x0F, 0});
  CHECK(b.percent == 87);
  CHECK(b.millivolts == 3892);
  CHECK_FALSE(b.charging);

  for (uint8_t state : {1, 3, 11})
    CHECK(decode_battery(Bytes{50, 0, 0, state}).charging);
  for (uint8_t state : {0, 2})
    CHECK_FALSE(decode_battery(Bytes{50, 0, 0, state}).charging);

  CHECK_THROWS_AS(decode_battery(Bytes{87, 0x34, 0x0F}), ProtocolError);
  CHECK_THROWS_AS(decode_battery(Bytes{}), ProtocolError);
}

TEST_CASE("decode_firmware_version", "[protocol][decode]") {
  auto v = decode_firmware_version(Bytes{0, 0, 2, 7, 0});
  CHECK(v.major == 2);
  CHECK(v.minor == 7);

  SECTION("minor >= 100 is divided by 10") {
    CHECK(decode_firmware_version(Bytes{0, 0, 1, 105, 0}).minor == 10);
    CHECK(decode_firmware_version(Bytes{0, 0, 1, 100, 0}).minor == 10);
    CHECK(decode_firmware_version(Bytes{0, 0, 1, 99, 0}).minor == 99);
  }
  // 4-byte payload as returned by real hardware (FW 7.x)
  CHECK(decode_firmware_version(Bytes{0, 0, 7, 97}).minor == 97);
  CHECK_THROWS_AS(decode_firmware_version(Bytes{0, 0, 7}), ProtocolError);
  CHECK_THROWS_AS(decode_firmware_version(Bytes{0, 0}), ProtocolError);
  CHECK_THROWS_AS(decode_firmware_version(Bytes{}), ProtocolError);
}

TEST_CASE("decode_u32 is little endian", "[protocol][decode]") {
  CHECK(decode_u32(Bytes{0x78, 0x56, 0x34, 0x12}) == 0x12345678u);
  CHECK_THROWS_AS(decode_u32(Bytes{1, 2, 3}), ProtocolError);
  CHECK_THROWS_AS(decode_u32(Bytes{}), ProtocolError);
}

// MARK: - Notifications

TEST_CASE("decode_notification RegularPing (125)", "[protocol][notification]") {
  Bytes p{0xEE, 77};
  for (unsigned v : {0x0102u, 0x0304u, 0x0506u, 0x0708u, 0x090Au, 0x0B0Cu})
    put_u16(p, v);
  auto n = decode_notification(make_frame(125, p));
  REQUIRE(n.has_value());
  auto *r = std::get_if<RegularPing>(&*n);
  REQUIRE(r != nullptr);
  CHECK(r->battery_percent == 77);
  CHECK(r->ppg_size == 0x0102);
  CHECK(r->activity_size == 0x0304);
  CHECK(r->hrv_size == 0x0506);
  CHECK(r->workout_size == 0x0708);
  CHECK(r->sleep_size == 0x090A);
  CHECK(r->log_size == 0x0B0C);
}

TEST_CASE("decode_notification RealtimePing (222)",
          "[protocol][notification]") {
  Bytes p;
  put_u32(p, 0x65432100);
  p.insert(p.end(),
           {72, 4, 58, 3, 98, 2}); // hr, hr_q, resp, resp_q, spo2, spo2_q
  put_u16(p, 3705);                // core temp x100
  p.insert(p.end(), {5, 64});      // movement, battery
  auto n = decode_notification(make_frame(222, p));
  REQUIRE(n.has_value());
  auto *r = std::get_if<RealtimePing>(&*n);
  REQUIRE(r != nullptr);
  CHECK(r->timestamp_s == 0x65432100u);
  CHECK(r->heart_rate == 72);
  CHECK(r->heart_rate_quality == 4);
  CHECK(r->respiration_rate == Approx(14.5f));
  CHECK(r->respiration_rate_quality == 3);
  CHECK(r->spo2 == 98);
  CHECK(r->spo2_quality == 2);
  CHECK(r->core_body_temp_c == Approx(37.05f));
  CHECK(r->movement_level == 5);
  CHECK(r->battery_percent == 64);
}

TEST_CASE("decode_notification LiveHrm (160)", "[protocol][notification]") {
  Bytes p{140, 3};
  put_u16(p, 1234);                  // steps
  put_u16(p, 567);                   // energy
  p.insert(p.end(), {180, 9, 0xEE}); // max_hr, speed, skip
  put_u16(p, 3600);                  // duration
  p.insert(p.end(), {55, 120, 7});   // battery, avg_hr, avg_speed
  auto n = decode_notification(make_frame(160, p));
  REQUIRE(n.has_value());
  auto *r = std::get_if<LiveHrm>(&*n);
  REQUIRE(r != nullptr);
  CHECK(r->heart_rate == 140);
  CHECK(r->heart_rate_quality == 3);
  CHECK(r->steps == 1234);
  CHECK(r->energy == 567);
  CHECK(r->max_heart_rate == 180);
  CHECK(r->speed == 9);
  CHECK(r->duration == 3600);
  CHECK(r->battery_percent == 55);
  CHECK(r->average_heart_rate == 120);
  CHECK(r->average_speed == 7);
}

TEST_CASE("decode_notification charging event (50)",
          "[protocol][notification]") {
  auto n = decode_notification(make_frame(50, Bytes{91, 0x34, 0x0F, 3}));
  REQUIRE(n.has_value());
  auto *b = std::get_if<BatteryLevel>(&*n);
  REQUIRE(b != nullptr);
  CHECK(b->percent == 91);
  CHECK(b->millivolts == 3892);
  CHECK(b->charging);
}

TEST_CASE("decode_notification edge cases", "[protocol][notification]") {
  SECTION("unknown id gives nullopt") {
    CHECK_FALSE(decode_notification(make_frame(200, {1, 2, 3})).has_value());
  }
  SECTION("truncated known frames throw ProtocolError") {
    for (uint8_t id : {125, 222, 160, 50})
      CHECK_THROWS_AS(decode_notification(make_frame(id, {1, 2})),
                      ProtocolError);
  }
}

// MARK: - record_size

TEST_CASE("record_size table", "[protocol][files]") {
  CHECK(record_size(FileId::Ppg) == 256);
  CHECK(record_size(FileId::Activity) == 38);
  CHECK(record_size(FileId::Hrv) == 7);
  CHECK(record_size(FileId::Workout) == 40);
  CHECK(record_size(FileId::Sleep) == 0);
  CHECK(record_size(FileId::Logs) == 0);
  CHECK(record_size(FileId::Ppg2) == 0);
  CHECK(record_size(FileId::Ecg) == 0);
  CHECK(record_size(FileId::Bioz) == 0);
  CHECK(record_size(FileId::Greenteg) == 15);
  CHECK(record_size(FileId::Stress) == 18);
  CHECK(record_size(FileId::Acc) == 0);
}

TEST_CASE("decode_file_size firmware variants", "[protocol][decode]") {
  // FW 7.x: bare u32
  CHECK(decode_file_size(Bytes{0x52, 0xb7, 0x00, 0x00}, FileId::Activity) ==
        46930);
  // FW 8.12: file id echo first (captured from hardware)
  CHECK(decode_file_size(Bytes{0x02, 0x52, 0xb7, 0x00, 0x00},
                         FileId::Activity) == 46930);
  CHECK_THROWS_AS(
      decode_file_size(Bytes{0x01, 0x52, 0xb7, 0x00, 0x00}, FileId::Activity),
      ProtocolError);
  CHECK_THROWS_AS(decode_file_size(Bytes{0x52, 0xb7}, FileId::Activity),
                  ProtocolError);
}

TEST_CASE("decode_plan", "[protocol][decode]") {
  // Hardware response (captured)
  auto s = decode_plan(Bytes{0x07, 0x01, 0x3c, 0x00});
  CHECK(s.plan == Plan::TypicalMulticolor);
  CHECK(s.ppg2_rate == Ppg2Rate::Hz32);
  CHECK(s.ppg1_rate == 60);
  // Short responses keep defaults
  auto t = decode_plan(Bytes{0x05});
  CHECK(t.plan == Plan::HospitalRaw);
  CHECK(t.ppg2_rate == Ppg2Rate::Hz32);
  CHECK(t.ppg1_rate == 60);
  auto u = decode_plan(Bytes{0x09, 0x03});
  CHECK(u.ppg2_rate == Ppg2Rate::Hz128);
  CHECK(u.ppg1_rate == 60);
  CHECK_THROWS_AS(decode_plan(Bytes{}), ProtocolError);
}

// MARK: - User profile and Wearing Optimization

TEST_CASE("encode_user_profile", "[protocol][encode]") {
  using namespace std::chrono;
  UserProfile u;
  u.birth = year(1971) / February / 28;
  u.gender = Gender::Male;
  u.wrist = Wrist::Right;
  u.skin = SkinType::Olive;
  // Hardware example
  CHECK(encode_user_profile(u, year(2026) / November / 10) ==
        Bytes{0x37, 0x07, 0xB3, 0x02, 0x1C, 0x00, 0x01, 0x03});
  // Birthday not reached yet / today / already passed
  CHECK(encode_user_profile(u, year(2026) / February / 27)[0] == 54);
  CHECK(encode_user_profile(u, year(2026) / February / 28)[0] == 55);
  // Leap-day birth
  u.birth = year(2000) / February / 29;
  CHECK(encode_user_profile(u, year(2026) / February / 28)[0] == 25);
  CHECK(encode_user_profile(u, year(2026) / March / 1)[0] == 26);
  // Invalid
  u.birth = year(2001) / February / 29;
  CHECK_THROWS_AS(encode_user_profile(u, year(2026) / March / 1),
                  std::invalid_argument);
  u.birth = year(2027) / January / 1;
  CHECK_THROWS_AS(encode_user_profile(u, year(2026) / March / 1),
                  std::invalid_argument);
}

TEST_CASE("decode_wearing_optimization", "[protocol][decode]") {
  auto r = decode_wearing_optimization(Bytes{4, 1, 0, 1, 0x53, 0x32});
  CHECK_FALSE(r.finished);
  CHECK(r.movement_level == 1);
  CHECK(r.progress_percent == 83);
  CHECK(r.quality == 50);
  CHECK_FALSE(r.success());
  auto f = decode_wearing_optimization(Bytes{4, 0, 1, 0, 0x64, 0x32});
  CHECK(f.finished);
  CHECK(f.progress_percent == 100);
  CHECK(f.success());
  CHECK_THROWS_AS(decode_wearing_optimization(Bytes{4, 0, 1, 0, 0x64}),
                  ProtocolError);
  CHECK_THROWS_AS(decode_wearing_optimization(Bytes{3, 0, 1, 0, 0x64, 0x32}),
                  ProtocolError);
}

TEST_CASE("SpO2 alert constants", "[protocol]") {
  CHECK(Spo2LowConfidence == 103);
  CHECK(Spo2Motion == 104);
  CHECK(Spo2VenousPooling == 105);
  CHECK(Spo2SkinCondition == 106);
}
