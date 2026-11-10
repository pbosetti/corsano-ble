#include "corsano/protocol.hpp"

#include "bytes.hpp"

#include <algorithm>

namespace Corsano {

namespace {

void require(std::span<const uint8_t> data, size_t n, const char *what) {
  if (data.size() < n)
    throw ProtocolError(std::string(what) + ": need " + std::to_string(n) +
                        " bytes, got " + std::to_string(data.size()));
}

} // namespace

// MARK: - Identifiers

uint32_t record_size(FileId file) {
  switch (file) {
  case FileId::Ppg: // transfer unit, as in SDK 1.x/2.x; v3 chunks (481 B) may
    return 256;     // straddle downloads, decode_ppg leaves them unconsumed
  case FileId::Activity:
    return 38;
  case FileId::Hrv:
    return 7;
  case FileId::Workout:
    return 40;
  case FileId::Greenteg:
    return 15;
  case FileId::Stress:
    return 18;
  default:
    return 0;
  }
}

// MARK: - Codec

uint8_t crc8(std::span<const uint8_t> data) {
  uint8_t crc = 0xFF;
  for (uint8_t b : data) {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
      crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
  }
  return crc;
}

bool check_crc(std::span<const uint8_t> frame) {
  if (frame.size() < 2)
    return true;
  return crc8(frame.first(frame.size() - 1)) == frame.back();
}

std::vector<uint8_t> encode_request(uint8_t id,
                                    std::span<const uint8_t> payload) {
  std::vector<uint8_t> out;
  out.reserve(payload.size() + 1);
  out.push_back(id);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> encode_set_time(std::chrono::system_clock::time_point now,
                                     std::chrono::minutes utc_offset) {
  using namespace std::chrono;
  auto day = floor<days>(now);
  year_month_day ymd{day};
  hh_mm_ss<seconds> hms{floor<seconds>(now - day)};
  int16_t off = int16_t(utc_offset.count());
  return {uint8_t(int(ymd.year()) - 2000), uint8_t(unsigned(ymd.month())),
          uint8_t(unsigned(ymd.day())),    uint8_t(hms.hours().count()),
          uint8_t(hms.minutes().count()),  uint8_t(hms.seconds().count()),
          uint8_t(uint16_t(off) & 0xFF),   uint8_t(uint16_t(off) >> 8)};
}

BatteryLevel decode_battery(std::span<const uint8_t> p) {
  require(p, 4, "battery");
  uint8_t state = p[3];
  return {p[0], rd16(&p[1]), state == 1 || state == 3 || state == 11};
}

FirmwareVersion decode_firmware_version(std::span<const uint8_t> p) {
  // SDK reads 5 bytes, but the 5th is the frame CRC (seen on hardware)
  require(p, 4, "firmware version");
  int minor = p[3];
  if (minor >= 100)
    minor /= 10;
  return {p[2], minor};
}

std::vector<uint8_t> encode_user_profile(const UserProfile &profile,
                                         std::chrono::year_month_day today) {
  using namespace std::chrono;
  const auto &b = profile.birth;
  if (!b.ok() || !today.ok() || sys_days(b) > sys_days(today))
    throw std::invalid_argument("user profile: invalid birth date");
  int age = int(today.year()) - int(b.year());
  if (today.month() < b.month() ||
      (today.month() == b.month() && today.day() < b.day()))
    --age;
  int year = int(b.year());
  return {uint8_t(age),
          uint8_t(year >> 8),
          uint8_t(year & 0xFF),
          uint8_t(unsigned(b.month())),
          uint8_t(unsigned(b.day())),
          uint8_t(profile.gender),
          uint8_t(profile.wrist),
          uint8_t(profile.skin)};
}

WearingOptimizationStatus
decode_wearing_optimization(std::span<const uint8_t> p) {
  require(p, 6, "wearing optimization");
  if (p[0] != 4)
    throw ProtocolError("wearing optimization: response for mode " +
                        std::to_string(p[0]));
  return {p[2] == 1, p[4], p[3], p[5]};
}

PlanSettings decode_plan(std::span<const uint8_t> p) {
  require(p, 1, "plan");
  PlanSettings s{Plan(p[0])};
  if (p.size() > 1)
    s.ppg2_rate = Ppg2Rate(p[1]);
  if (p.size() > 2)
    s.ppg1_rate = p[2];
  return s;
}

uint32_t decode_file_size(std::span<const uint8_t> p, FileId file) {
  if (p.size() < 5)
    return decode_u32(p);
  if (p[0] != uint8_t(file))
    throw ProtocolError("file size: response for file " + std::to_string(p[0]));
  return decode_u32(p.subspan(1));
}

uint32_t decode_u32(std::span<const uint8_t> p) {
  require(p, 4, "u32");
  return rd32(p.data());
}

std::optional<Notification>
decode_notification(std::span<const uint8_t> frame) {
  require(frame, 2, "notification");
  if (!check_crc(frame))
    throw ProtocolError("notification: bad CRC");
  auto p = frame.subspan(1, frame.size() - 2);
  switch (frame[0]) {
  case uint8_t(CommandId::RegularPing): {
    require(p, 14, "regular ping");
    return RegularPing{p[1],        rd16(&p[2]),  rd16(&p[4]), rd16(&p[6]),
                       rd16(&p[8]), rd16(&p[10]), rd16(&p[12])};
  }
  case uint8_t(CommandId::RealtimePing): {
    require(p, 14, "realtime ping");
    return RealtimePing{rd32(&p[0]), p[4], p[5], p[6] / 4.0f,
                        p[7],        p[8], p[9], rd16(&p[10]) / 100.0f,
                        p[12],       p[13]};
  }
  case uint8_t(CommandId::LiveHrmPing): {
    require(p, 14, "live hrm");
    return LiveHrm{p[0], p[1],        rd16(&p[2]), rd16(&p[4]), p[6],
                   p[7], rd16(&p[9]), p[11],       p[12],       p[13]};
  }
  case uint8_t(CommandId::GetBatteryLevel):
    return decode_battery(p);
  default:
    return std::nullopt;
  }
}

// MARK: - Authentication

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
  static constexpr uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  auto block = [&](const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 |
             uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    std::copy(h, h + 8, v);
    for (int i = 0; i < 64; ++i) {
      uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      uint32_t t1 = v[7] + s1 + ch + K[i] + w[i];
      uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      uint32_t t2 = s0 + maj;
      std::copy_backward(v, v + 7, v + 8);
      v[4] += t1;
      v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i)
      h[i] += v[i];
  };
  size_t full = data.size() / 64;
  for (size_t i = 0; i < full; ++i)
    block(data.data() + 64 * i);
  // Final block(s): remainder, 0x80, zero padding, 64-bit bit length
  uint8_t tail[128] = {};
  size_t rem = data.size() - full * 64;
  std::copy(data.end() - rem, data.end(), tail);
  tail[rem] = 0x80;
  size_t tail_len = rem < 56 ? 64 : 128;
  uint64_t bits = uint64_t(data.size()) * 8;
  for (int i = 0; i < 8; ++i)
    tail[tail_len - 1 - i] = uint8_t(bits >> (8 * i));
  block(tail);
  if (tail_len == 128)
    block(tail + 64);
  std::array<uint8_t, 32> out;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j)
      out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
  return out;
}

std::array<uint8_t, 8> host_id_bytes(std::string_view host_id) {
  std::array<uint8_t, 8> out;
  auto nibble = [](char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  bool hex = host_id.size() == 16;
  for (size_t i = 0; hex && i < 16; ++i)
    hex = nibble(host_id[i]) >= 0;
  if (hex) {
    for (size_t i = 0; i < 8; ++i)
      out[i] =
          uint8_t(nibble(host_id[2 * i]) << 4 | nibble(host_id[2 * i + 1]));
  } else {
    auto d = sha256(
        {reinterpret_cast<const uint8_t *>(host_id.data()), host_id.size()});
    std::copy_n(d.begin(), 8, out.begin());
  }
  return out;
}

std::vector<uint8_t> encode_authenticate(std::string_view host_id) {
  std::vector<uint8_t> out{0xA7, 0x16};
  auto b = host_id_bytes(host_id);
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

} // namespace Corsano
