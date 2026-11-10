// Command catalog and human-readable reply decoding
#include "corsano/protocol.hpp"

#include "bytes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iterator>

namespace Corsano {

namespace {

// MARK: - Table

constexpr CommandInfo Catalog[] = {
    {16, "workout-start", "Start workout mode (LiveHrm every 2 s)", true},
    {21, "erase-all", "Erase every recorded file", true},
    {22, "set-time", "Set the bracelet clock (8 bytes)", true},
    {24, "reset", "Reset the bracelet", true},
    {40, "set-bedtime", "Set bedtime, hh mm GMT", true},
    {41, "set-risetime", "Set risetime, hh mm GMT", true},
    {42, "set-height", "Set the user height in cm", true},
    {43, "set-weight", "Set the user weight in kg", true},
    {44, "set-profile", "Set the user profile", true},
    {45, "erase-file", "Free the first bytes of a file", true},
    {46, "get-file-size", "Bytes stored in a file", false},
    {48, "start-file-stream", "Start the raw file stream on TX", false},
    {49, "get-active-mode", "Query the active mode", false},
    {50, "battery", "Battery percent, voltage and charging state", false},
    {51, "get-time", "Read the bracelet clock", false},
    {52, "get-bedtime", "Read bedtime, GMT", false},
    {53, "get-risetime", "Read risetime, GMT", false},
    {54, "get-height", "Read the user height", false},
    {55, "get-weight", "Read the user weight", false},
    {56, "get-birthdate", "Read the user birth date", false},
    {57, "get-profile", "Read the user profile", false},
    {63, "firmware", "Firmware version", false},
    {65, "hardware-id", "Hardware id", false},
    {70, "serial", "Serial number", false},
    {72, "device-name", "Device name", false},
    {110, "bonding-state", "Whether the bracelet is bonded with this host",
     false},
    {111, "shutdown", "Power the bracelet off", true},
    {125, "regular-ping", "Unsolicited once-a-minute notification", false},
    {149, "set-plan", "Set plan, PPG2 rate and PPG1 rate", true},
    {150, "get-plan", "Read plan and PPG2 rate", false},
    {152, "session-status", "Workout session status", false},
    {155, "workout-stop", "Stop the workout", true},
    {156, "bracelet-status", "Wearing, mode, battery and quality", false},
    {160, "live-hrm", "Unsolicited workout-mode heart-rate notification",
     false},
    {192, "set-recording", "Set a raw recording mode (stress/BioZ, acc, temp)",
     true},
    {193, "get-recording", "Read a raw recording mode", false},
    {216, "start-special-mode",
     "Start a special mode (4 = wearing optimization)", true},
    {217, "stop-special-mode", "Stop a special mode", true},
    {220, "set-realtime", "Enable or disable realtime pings", true},
    {221, "get-realtime", "Read the realtime-ping enable flag", false},
    {222, "realtime-ping", "Unsolicited realtime vitals notification", false},
    {223, "set-realtime-period", "Set the realtime-ping period", true},
    {224, "get-realtime-period", "Read the realtime-ping period", false},
    {234, "set-sampling", "Set a vital's measurement interval", true},
    {235, "get-sampling", "Read a vital's measurement interval", false},
    {246, "get-special-mode", "Special-mode status (wearing optimization)",
     false},
    {250, "authenticate", "Authenticate this host (FW 8.41+)", true},
    {251, "get-auth-status", "Whether the connection is authenticated", false},
};

static_assert(std::is_sorted(std::begin(Catalog), std::end(Catalog),
                             [](auto &a, auto &b) { return a.id < b.id; }));

// MARK: - Names

template <typename T> struct Named {
  uint8_t value;
  const char *name;
};

constexpr Named<Plan> PlanNames[] = {
    {0, "stand-by"},
    {1, "max-battery"},
    {2, "typical"},
    {3, "hospital"},
    {4, "hz25"},
    {5, "hospital-raw"},
    {6, "disabled"},
    {7, "typical-multicolor"},
    {8, "hospital-multicolor"},
    {9, "multicolor-intermittent-raw"},
    {10, "multicolor-intermittent"},
    {11, "continuous-5min"},
};
constexpr Named<VitalType> VitalNames[] = {
    {1, "activity"}, {2, "pulse-rate"},  {3, "respiration"},
    {4, "spo2"},     {5, "temperature"}, {9, "nibp"},
};
constexpr Named<Gender> GenderNames[] = {
    {0, "male"}, {1, "female"}, {2, "unspecified"}};
constexpr Named<Wrist> WristNames[] = {{0, "left"}, {1, "right"}};
constexpr Named<SkinType> SkinNames[] = {
    {1, "very-fair"},   {2, "fair"},  {3, "olive"},
    {4, "light-brown"}, {5, "brown"}, {6, "black-brown"},
};
constexpr Named<int> FileNames[] = {
    {1, "ppg"},   {2, "activity"},  {3, "hrv"},     {4, "workout"},
    {5, "sleep"}, {6, "logs"},      {9, "ppg2"},    {13, "ecg"},
    {14, "bioz"}, {18, "greenteg"}, {21, "stress"}, {22, "acc"},
};
constexpr Named<int> RecordingNames[] = {
    {1, "off"}, {2, "bedtime-risetime"}, {3, "continuous"}};

template <typename T, size_t N>
std::string name_of(const Named<T> (&table)[N], unsigned v) {
  for (auto &n : table)
    if (n.value == v)
      return n.name;
  return std::to_string(v);
}

// MARK: - Helpers

std::string fmt(const char *f, auto... args) {
  char buf[256];
  std::snprintf(buf, sizeof buf, f, args...);
  return buf;
}

const char *yes_no(bool v) { return v ? "yes" : "no"; }

std::string iso_time(uint32_t epoch) {
  using namespace std::chrono;
  sys_seconds t{seconds(epoch)};
  auto day = floor<days>(t);
  year_month_day ymd{day};
  hh_mm_ss<seconds> hms{t - day};
  return fmt("%04d-%02u-%02uT%02d:%02d:%02dZ", int(ymd.year()),
             unsigned(ymd.month()), unsigned(ymd.day()),
             int(hms.hours().count()), int(hms.minutes().count()),
             int(hms.seconds().count()));
}

std::string ascii(std::span<const uint8_t> p) {
  std::string s;
  for (uint8_t c : p) {
    if (c == 0)
      break;
    s += char(c);
  }
  return s;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }

std::string decode(uint8_t id, std::span<const uint8_t> p, bool is_set) {
  if (p.empty())
    return "";
  if (is_set)
    return p[0] == StatusOk ? "status=ok"
                            : fmt("status=refused(%u)", unsigned(p[0]));
  auto need = [&](size_t n) { return p.size() >= n; };
  switch (id) {
  case uint8_t(CommandId::GetBatteryLevel): {
    auto b = decode_battery(p);
    return fmt("battery=%u%% voltage=%umV charging=%s", unsigned(b.percent),
               unsigned(b.millivolts), yes_no(b.charging));
  }
  case uint8_t(CommandId::GetFwVersion): {
    auto v = decode_firmware_version(p);
    return fmt("firmware=%d.%d", v.major, v.minor);
  }
  case uint8_t(CommandId::GetSerialNumber):
  case uint8_t(CommandId::GetDeviceName): {
    auto s = ascii(p);
    return s.empty() ? "" : "serial=" + s;
  }
  case uint8_t(CommandId::GetCurrentTime):
    return need(4) ? "time=" + iso_time(rd32(p.data())) : "";
  case 52:
  case 53:
    return need(2) ? fmt("time=%02u:%02uZ", unsigned(p[0]), unsigned(p[1]))
                   : "";
  case uint8_t(CommandId::GetHeight):
    return fmt("height=%ucm", unsigned(p[0]));
  case uint8_t(CommandId::GetWeight):
    return fmt("weight=%ukg", unsigned(p[0]));
  case uint8_t(CommandId::GetBirthdate):
    return need(4) ? fmt("birth=%04u-%02u-%02u", unsigned(rd16(p.data())),
                         unsigned(p[2]), unsigned(p[3]))
                   : "";
  case uint8_t(CommandId::GetProfile):
    return need(4) ? fmt("age=%u gender=%s wrist=%s skin=%s", unsigned(p[0]),
                         name_of(GenderNames, p[1]).c_str(),
                         name_of(WristNames, p[2]).c_str(),
                         name_of(SkinNames, p[3]).c_str())
                   : "";
  case uint8_t(CommandId::GetPlan): {
    auto s = decode_plan(p);
    std::string out = "plan=" + name_of(PlanNames, unsigned(s.plan));
    if (need(2))
      out += fmt(" ppg2=%uHz", unsigned(s.ppg2_rate) - 1 < 5
                                   ? 32u << (unsigned(s.ppg2_rate) - 1)
                                   : 0u);
    if (need(3))
      out += fmt(" ppg1=%u", unsigned(s.ppg1_rate));
    return out;
  }
  case uint8_t(CommandId::GetVitalsSamplingRate):
    return need(3)
               ? fmt("vital=%s interval=%us", name_of(VitalNames, p[0]).c_str(),
                     unsigned(rd16(&p[1])))
               : "";
  case 193: {
    if (!need(2))
      return "";
    auto v = [&](size_t i) { return name_of(RecordingNames, p[i]); };
    if (p[0] == 1)
      return need(3) ? "stress=" + v(1) + " bioz=" + v(2) : "";
    if (p[0] == 2)
      return "acc=" + v(1);
    if (p[0] == 4)
      return "temperature-raw=" + v(1);
    return "";
  }
  case uint8_t(CommandId::GetFileSize): {
    if (p.size() >= 5)
      return fmt("file=%s size=%uB", name_of(FileNames, p[0]).c_str(),
                 unsigned(rd32(&p[1])));
    return need(4) ? fmt("size=%uB", unsigned(rd32(p.data()))) : "";
  }
  case uint8_t(CommandId::GetRealtimeEnable):
    return fmt("enabled=%s", yes_no(p[0] == 1));
  case uint8_t(CommandId::GetRealtimePeriod):
    return fmt("period=%us", unsigned(p[0]));
  case uint8_t(CommandId::GetBraceletStatus):
    return need(7) ? fmt("wearing=%u mode=%u battery=%u%% charging=%s idle=%u "
                         "wearing-quality=%u",
                         unsigned(p[0]), unsigned(p[1]), unsigned(p[2]),
                         yes_no(p[3] != 0), unsigned(p[4]), unsigned(p[6]))
                   : "";
  case 152: {
    if (!need(9))
      return "";
    const char *st = p[8] == 0   ? "not-started"
                     : p[8] == 1 ? "ongoing"
                     : p[8] == 2 ? "finished"
                                 : nullptr;
    uint32_t start = rd32(&p[2]);
    // 0 = no session; small values are not epoch times (seen on FW 8.12)
    std::string when = start == 0           ? "none"
                       : start < 1000000000 ? std::to_string(start) + "s"
                                            : iso_time(start);
    std::string s =
        fmt("mode=%u session=%u start=%s duration=%us status=", unsigned(p[0]),
            unsigned(p[1]), when.c_str(), unsigned(rd16(&p[6])));
    return s + (st ? st : std::to_string(p[8]));
  }
  case uint8_t(CommandId::GetSpecialMode): {
    auto w = decode_wearing_optimization(p);
    return fmt("finished=%s progress=%u%% movement=%u quality=%u result=%s",
               yes_no(w.finished), unsigned(w.progress_percent),
               unsigned(w.movement_level), unsigned(w.quality),
               !w.finished   ? "running"
               : w.success() ? "pass"
                             : "fail");
  }
  case uint8_t(CommandId::GetAuthStatus):
    return fmt("authenticated=%s", yes_no(p[0] == StatusOk));
  case uint8_t(CommandId::GetBondingState):
    return fmt("bonded=%s", yes_no(p[0] == StatusOk));
  case uint8_t(CommandId::GetHwId): // SDK reads one byte (GetHwIdRequest)
    return fmt("hwid=%u", unsigned(p[0]));
  default:
    return "";
  }
}

} // namespace

// MARK: - Public

std::span<const CommandInfo> command_catalog() { return Catalog; }

const CommandInfo *find_command(uint8_t id) {
  auto it = std::lower_bound(
      std::begin(Catalog), std::end(Catalog), id,
      [](const CommandInfo &c, uint8_t v) { return c.id < v; });
  return it != std::end(Catalog) && it->id == id ? it : nullptr;
}

const CommandInfo *find_command(std::string_view name) {
  auto same = [&](const char *n) {
    size_t i = 0;
    for (; n[i] && i < name.size(); ++i) {
      char c = name[i] == '_' ? '-' : lower(name[i]);
      if (c != n[i])
        return false;
    }
    return !n[i] && i == name.size();
  };
  for (auto &c : Catalog)
    if (same(c.name))
      return &c;
  return nullptr;
}

std::string describe_reply(uint8_t id, std::span<const uint8_t> payload) {
  try {
    auto *info = find_command(id);
    return info ? decode(id, payload, info->is_set) : "";
  } catch (...) {
    return "";
  }
}

} // namespace Corsano
