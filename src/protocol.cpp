#include "corsano/protocol.hpp"

#include "bytes.hpp"

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

} // namespace Corsano
