// corsano-cli: CSV on stdout (header + rows only), everything else on stderr,
// so the output can be piped straight into complot.
#include "corsano/bracelet.hpp"
#include "corsano/files.hpp"
#include "corsano/monitor.hpp"

// -x arguments contain commas (set-sampling:spo2,60): do not split them
#define CXXOPTS_VECTOR_DELIMITER '\x1f'
#include <cxxopts.hpp>

#include <algorithm>
#include <bit>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Corsano::Bracelet;

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_out_mutex;

// MARK: - Output helpers

template <typename... Args> std::string fmt(const char *f, Args... args) {
  char buf[512];
  std::snprintf(buf, sizeof buf, f, args...);
  return buf;
}

void write_out(const std::string &s) {
  std::lock_guard lock(g_out_mutex);
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

// Garbage readings (quality 0) become NA so plots skip them
std::string val_or_na(unsigned v, unsigned q) {
  return q == 0 ? "NA" : std::to_string(v);
}

void on_signal(int) { g_stop = true; }

// Sleeps until a signal arrives or `seconds` elapse (0 = forever)
void wait_for_stop(double seconds) {
  auto end = std::chrono::steady_clock::now() +
             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                 std::chrono::duration<double>(seconds));
  while (!g_stop && (seconds <= 0 || std::chrono::steady_clock::now() < end))
    std::this_thread::sleep_for(100ms);
}

// 6 hex pairs (MAC) or 36-char dashed UUID
bool looks_like_address(const std::string &s) {
  auto all_hex = [&](char sep, size_t len) {
    if (s.size() != len)
      return false;
    for (size_t i = 0; i < s.size(); ++i)
      if (i % 3 == 2 ? s[i] != sep : !std::isxdigit((unsigned char)s[i]))
        return false;
    return true;
  };
  if (all_hex(':', 17))
    return true;
  if (s.size() != 36)
    return false;
  for (size_t i = 0; i < s.size(); ++i)
    if (i == 8 || i == 13 || i == 18 || i == 23
            ? s[i] != '-'
            : !std::isxdigit((unsigned char)s[i]))
      return false;
  return true;
}

// -a, then -n, then $CORSANO_ADDR (address or name); empty if none
std::string device_arg(const cxxopts::ParseResult &args) {
  if (args.count("address"))
    return args["address"].as<std::string>();
  if (args.count("name"))
    return args["name"].as<std::string>();
  const char *env = std::getenv("CORSANO_ADDR");
  return env ? env : "";
}

// An address is used as is; a name is resolved to an address with a scan
std::string resolve_address(const cxxopts::ParseResult &args) {
  auto name = device_arg(args);
  if (name.empty())
    throw std::runtime_error(
        "missing -a/--address, -n/--name or $CORSANO_ADDR (see `scan`)");
  if (args.count("address") ||
      (!args.count("name") && looks_like_address(name)))
    return name;
  std::cerr << "looking for \"" << name << "\"...\n";
  std::vector<std::string> found;
  for (auto &d : Bracelet::scan(std::chrono::seconds(5)))
    if (d.name == name)
      found.push_back(d.address);
  if (found.empty())
    throw std::runtime_error(
        "no bracelet named \"" + name +
        "\" (is it worn, awake, not connected elsewhere?)");
  if (found.size() > 1) {
    std::string list;
    for (auto &a : found)
      list += " " + a;
    throw std::runtime_error("several bracelets named \"" + name +
                             "\", use -a with one of:" + list);
  }
  return found.front();
}

Bracelet connect(const cxxopts::ParseResult &args) {
  Bracelet b(resolve_address(args));
  if (args.count("host-id"))
    b.set_host_id(args["host-id"].as<std::string>());
  std::cerr << "connecting...\n";
  b.connect();
  std::cerr << "connected\n";
  return b;
}

// MARK: - Commands

// MARK: - Plans

struct PlanName {
  const char *name;
  Corsano::Plan plan;
};
constexpr PlanName PlanNames[] = {
    {"stand-by", Corsano::Plan::StandBy},
    {"max-battery", Corsano::Plan::MaxBattery},
    {"typical", Corsano::Plan::Typical},
    {"hospital", Corsano::Plan::Hospital},
    {"hz25", Corsano::Plan::Hz25},
    {"hospital-raw", Corsano::Plan::HospitalRaw},
    {"disabled", Corsano::Plan::Disabled},
    {"typical-multicolor", Corsano::Plan::TypicalMulticolor},
    {"hospital-multicolor", Corsano::Plan::HospitalMulticolor},
    {"multicolor-intermittent-raw", Corsano::Plan::MulticolorIntermittentRaw},
    {"multicolor-intermittent", Corsano::Plan::MulticolorIntermittent},
    {"continuous-5min", Corsano::Plan::Continuous5Min},
};

Corsano::Plan parse_plan(const std::string &name) {
  for (auto &p : PlanNames)
    if (name == p.name)
      return p.plan;
  std::string list;
  for (auto &p : PlanNames)
    list += std::string(list.empty() ? "" : ", ") + p.name;
  throw std::runtime_error("unknown plan: " + name + " (one of: " + list + ")");
}

const char *plan_name(Corsano::Plan plan) {
  for (auto &p : PlanNames)
    if (p.plan == plan)
      return p.name;
  return "unknown";
}

int ppg2_hz(Corsano::Ppg2Rate r) { return 32 << (int(r) - 1); }

// MARK: - Profile file

struct SkinName {
  const char *name;
  Corsano::SkinType skin;
};
constexpr SkinName SkinNames[] = {
    {"very-fair", Corsano::SkinType::VeryFair},
    {"fair", Corsano::SkinType::Fair},
    {"olive", Corsano::SkinType::Olive},
    {"light-brown", Corsano::SkinType::LightBrown},
    {"brown", Corsano::SkinType::Brown},
    {"black-brown", Corsano::SkinType::BlackBrown},
};

const char *gender_name(Corsano::Gender g) {
  return g == Corsano::Gender::Male     ? "male"
         : g == Corsano::Gender::Female ? "female"
                                        : "unspecified";
}

const char *skin_name(Corsano::SkinType s) {
  for (auto &n : SkinNames)
    if (n.skin == s)
      return n.name;
  return "unknown";
}

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// Lower case, spaces and underscores become '-': "Light_Brown" -> "light-brown"
std::string normalize(std::string s) {
  for (auto &c : s)
    c = c == ' ' || c == '_' ? '-' : char(std::tolower((unsigned char)c));
  return s;
}

std::string birth_string(const Corsano::UserProfile &p) {
  return fmt("%04d-%02u-%02u", int(p.birth.year()), unsigned(p.birth.month()),
             unsigned(p.birth.day()));
}

// `key<sep>value` lines; ": " is the profile file format
std::string profile_text(const Corsano::UserProfile &p, const char *sep) {
  return std::string("birth") + sep + birth_string(p) + "\ngender" + sep +
         gender_name(p.gender) + "\nwrist" + sep +
         (p.wrist == Corsano::Wrist::Left ? "left" : "right") + "\nskin" + sep +
         skin_name(p.skin) + "\nheight_cm" + sep + std::to_string(p.height_cm) +
         "\nweight_kg" + sep + std::to_string(p.weight_kg) + "\n";
}

uint8_t parse_u8(const std::string &v) {
  size_t used = 0;
  int n = -1;
  try {
    n = std::stoi(v, &used);
  } catch (const std::exception &) {
  }
  if (used != v.size() || n < 0 || n > 255)
    throw std::runtime_error("expected 0..255");
  return uint8_t(n);
}

Corsano::UserProfile parse_profile(std::istream &in, const std::string &path) {
  Corsano::UserProfile p;
  bool have_birth = false, have_gender = false, have_wrist = false,
       have_skin = false;
  std::string line;
  for (int no = 1; std::getline(in, line); ++no) {
    auto hash = line.find('#');
    if (hash != std::string::npos)
      line.erase(hash);
    line = trim(line);
    if (line.empty())
      continue;
    auto fail = [&](const std::string &why) {
      return std::runtime_error(path + ":" + std::to_string(no) + ": " + why);
    };
    auto colon = line.find(':');
    if (colon == std::string::npos)
      throw fail("expected `key: value`");
    auto key = normalize(trim(line.substr(0, colon)));
    auto val = trim(line.substr(colon + 1));
    if (val.empty()) // unfilled template line: treated as absent
      continue;
    auto nval = normalize(val);
    try {
      if (key == "birth") {
        int y, m, d, n = 0;
        if (std::sscanf(val.c_str(), "%d-%d-%d%n", &y, &m, &d, &n) != 3 ||
            n != int(val.size()))
          throw std::runtime_error("expected YYYY-MM-DD");
        p.birth = std::chrono::year_month_day{std::chrono::year{y},
                                              std::chrono::month(unsigned(m)),
                                              std::chrono::day(unsigned(d))};
        if (m < 1 || d < 1 || !p.birth.ok())
          throw std::runtime_error("not a valid date");
        have_birth = true;
      } else if (key == "gender") {
        if (nval == "male")
          p.gender = Corsano::Gender::Male;
        else if (nval == "female")
          p.gender = Corsano::Gender::Female;
        else if (nval == "unspecified")
          p.gender = Corsano::Gender::Unspecified;
        else
          throw std::runtime_error("expected male, female or unspecified");
        have_gender = true;
      } else if (key == "wrist") {
        if (nval != "left" && nval != "right")
          throw std::runtime_error("expected left or right");
        p.wrist = nval == "left" ? Corsano::Wrist::Left : Corsano::Wrist::Right;
        have_wrist = true;
      } else if (key == "skin") {
        have_skin = false;
        for (auto &n : SkinNames)
          if (nval == n.name)
            p.skin = n.skin, have_skin = true;
        if (!have_skin)
          throw std::runtime_error("expected very-fair, fair, olive, "
                                   "light-brown, brown or black-brown");
      } else if (key == "height-cm" || key == "height") {
        p.height_cm = parse_u8(val);
      } else if (key == "weight-kg" || key == "weight") {
        p.weight_kg = parse_u8(val);
      } else {
        throw std::runtime_error("unknown key `" + trim(line.substr(0, colon)) +
                                 "`");
      }
    } catch (const std::runtime_error &e) {
      throw fail(e.what());
    }
  }
  for (auto [ok, key] : {std::pair{have_birth, "birth"},
                         {have_gender, "gender"},
                         {have_wrist, "wrist"},
                         {have_skin, "skin"}})
    if (!ok)
      throw std::runtime_error(path + ": missing required key `" + key + "`");
  return p;
}

Corsano::UserProfile load_profile(const std::string &path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("cannot read profile file " + path);
  return parse_profile(f, path);
}

const char *DefaultProfilePath = "corsano-profile.txt";

std::string profile_path(const cxxopts::ParseResult &args) {
  return args.count("profile") ? args["profile"].as<std::string>()
                               : DefaultProfilePath;
}

int cmd_scan(const cxxopts::ParseResult &args) {
  auto secs = args.count("duration") ? args["duration"].as<double>() : 0.0;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::cerr << "scanning, Ctrl-C to stop\n";
  write_out("name,address,rssi\n");
  auto end = std::chrono::steady_clock::now() +
             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                 std::chrono::duration<double>(secs));
  Bracelet::scan(
      [](const Corsano::DeviceInfo &d) {
        write_out(
            fmt("%s,%s,%d\n", d.name.c_str(), d.address.c_str(), d.rssi));
      },
      [&] {
        return g_stop || (secs > 0 && std::chrono::steady_clock::now() >= end);
      });
  return 0;
}

int cmd_info(const cxxopts::ParseResult &args) {
  auto b = connect(args);
  auto bat = b.battery();
  auto fw = b.firmware_version();
  std::string out = "key,value\n";
  out += "serial," + b.serial_number() + "\n";
  out += fmt("firmware,%d.%d\n", fw.major, fw.minor);
  out += "host_id," + b.host_id() + "\n";
  out += fmt("authenticated,%d\n", b.authenticated());
  out += fmt("battery_percent,%d\n", bat.percent);
  out += fmt("battery_mv,%d\n", bat.millivolts);
  out += fmt("charging,%d\n", bat.charging);
  out += fmt("realtime_enabled,%d\n", b.realtime_enabled());
  out += fmt("realtime_period_s,%d\n", int(b.realtime_period().count()));
  out += fmt("pulse_rate_sampling_s,%d\n",
             int(b.vital_sampling_rate(Corsano::VitalType::PulseRate).count()));
  out += fmt("spo2_sampling_s,%d\n",
             int(b.vital_sampling_rate(Corsano::VitalType::Spo2).count()));
  out += std::string("plan,") + plan_name(b.plan().plan) + "\n";
  out += profile_text(b.user_profile(), ",");
  write_out(out);
  return 0;
}

int cmd_live(const cxxopts::ParseResult &args) {
  // FW 8.12 pushes LiveHrm every 2 s in workout mode; realtime pings only once
  bool workout = !args.count("realtime");
  auto t0 = std::chrono::steady_clock::now();
  auto now_s = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
  };

  auto b = connect(args);
  if (args.count("set-time"))
    b.set_time();
  if (args.count("sampling")) {
    auto s = std::chrono::seconds(int(args["sampling"].as<double>()));
    b.set_vital_sampling_rate(Corsano::VitalType::PulseRate, s);
    b.set_vital_sampling_rate(Corsano::VitalType::Spo2, s);
  }

  write_out(workout ? "t,hr,hr_q,steps,energy,avg_hr,battery\n"
                    : "t,device_t,hr,hr_q,spo2,spo2_q,resp,resp_q,temp,"
                      "movement,battery\n");
  b.set_notification_callback([&](const Corsano::Notification &n) {
    if (!workout) {
      if (auto *p = std::get_if<Corsano::RealtimePing>(&n))
        write_out(fmt(
            "%.3f,%u,%s,%u,%s,%u,%.2f,%u,%.2f,%u,%u\n", now_s(), p->timestamp_s,
            val_or_na(p->heart_rate, p->heart_rate_quality).c_str(),
            p->heart_rate_quality, val_or_na(p->spo2, p->spo2_quality).c_str(),
            p->spo2_quality, p->respiration_rate, p->respiration_rate_quality,
            p->core_body_temp_c, p->movement_level, p->battery_percent));
    } else if (auto *p = std::get_if<Corsano::LiveHrm>(&n)) {
      write_out(fmt("%.3f,%u,%u,%u,%u,%u,%u\n", now_s(), p->heart_rate,
                    p->heart_rate_quality, p->steps, p->energy,
                    p->average_heart_rate, p->battery_percent));
    }
  });

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  b.set_disconnect_callback([] {
    std::cerr << "bracelet disconnected\n";
    g_stop = true;
  });
  auto period = std::chrono::seconds(int(args["period"].as<double>()));
  bool was_enabled = false; // restored on exit: leave the bracelet as found
  if (workout) {
    b.set_workout_mode(true);
  } else {
    // FW 8.12 rejects period changes; stream at the bracelet's own period
    try {
      b.set_realtime_period(period);
    } catch (const Corsano::ProtocolError &e) {
      std::cerr << "warning: " << e.what() << "; using bracelet period "
                << b.realtime_period().count() << " s\n";
    }
    was_enabled = b.realtime_enabled();
    b.set_realtime_enabled(true); // sent even if enabled: may (re)start pings
  }
  std::cerr << "streaming, Ctrl-C to stop\n";
  wait_for_stop(args.count("duration") ? args["duration"].as<double>() : 0);

  std::cerr << "stopping...\n";
  try {
    if (workout)
      b.set_workout_mode(false);
    else if (!was_enabled)
      b.set_realtime_enabled(false);
  } catch (const std::exception &e) {
    std::cerr << "warning: cleanup failed: " << e.what() << "\n";
  }
  b.set_notification_callback({});
  b.disconnect();
  return 0;
}

// Downloadable files by CLI name. `decoded` files print CSV; the others can
// only be dumped with --raw until their format is understood.
struct HistoryFile {
  const char *name;
  Corsano::FileId id;
  bool decoded;
};
constexpr HistoryFile HistoryFiles[] = {
    {"activity", Corsano::FileId::Activity, true},
    {"ppg", Corsano::FileId::Ppg, true},
    {"hrv", Corsano::FileId::Hrv, false},
    {"workout", Corsano::FileId::Workout, false},
    {"sleep", Corsano::FileId::Sleep, false},
    {"logs", Corsano::FileId::Logs, false},
    {"ppg2", Corsano::FileId::Ppg2, false},
    {"ecg", Corsano::FileId::Ecg, false},
    {"bioz", Corsano::FileId::Bioz, false},
    {"greenteg", Corsano::FileId::Greenteg, false},
    {"stress", Corsano::FileId::Stress, true},
    {"acc", Corsano::FileId::Acc, false},
};

std::string activity_csv(const Corsano::Decoded<Corsano::ActivityRecord> &d) {
  std::string out = "time,hr,hr_q,spo2,spo2_q,resp,resp_q,steps,temp1,temp2,"
                    "battery,charging,wearing\n";
  for (auto &r : d.records)
    out += fmt("%u,%u,%u,%u,%u,%.2f,%u,%u,%.2f,%.2f,%u,%d,%u\n", r.timestamp_s,
               r.heart_rate, r.heart_rate_quality, r.spo2, r.spo2_quality,
               r.respiration_rate, r.respiration_rate_quality, r.steps,
               r.temp1_c, r.temp2_c, r.battery_percent, r.charging, r.wearing);
  return out;
}

std::string stress_csv(const Corsano::Decoded<Corsano::StressRecord> &d) {
  std::string out = "time,cognitive_zone,cognitive_zone_q,predicted_zone,"
                    "predicted_transition,zone_hour,cortisol,sc_stress,"
                    "sc_stress_q\n";
  for (auto &r : d.records)
    out += fmt("%u,%u,%u,%u,%u,%u,%u,%u,%u\n", r.timestamp_s, r.cognitive_zone,
               r.cognitive_zone_quality, r.predicted_cognitive_zone,
               r.predicted_transition_time, r.cognitive_zone_hour,
               r.cortisol_contribution, r.skin_conductance_stress,
               r.skin_conductance_stress_quality);
  return out;
}

std::string ppg_csv(const Corsano::Decoded<Corsano::PpgChunk> &d) {
  std::string out = "t_ms,chunk,ppg,acc,acc_x,acc_y,acc_z,hr,hr_q\n";
  for (auto &c : d.records)
    for (size_t i = 0; i < c.samples.size(); ++i) {
      auto &s = c.samples[i];
      size_t k = std::min<size_t>(i / Corsano::PpgSamplesPerHeartRate, 2);
      out += fmt("%llu,%u,%u,%u,%d,%d,%d,%u,%u\n",
                 (unsigned long long)(c.timestamp_ms +
                                      Corsano::PpgSamplePeriod.count() * i),
                 c.index, s.ppg, s.acc, s.acc_x, s.acc_y, s.acc_z,
                 c.heart_rate[k], c.heart_rate_quality[k]);
    }
  return out;
}

int cmd_history(const cxxopts::ParseResult &args) {
  auto name = args["file"].as<std::string>();
  const HistoryFile *file = nullptr;
  for (auto &f : HistoryFiles)
    if (name == f.name)
      file = &f;
  if (!file)
    throw std::runtime_error("unknown file: " + name);
  bool raw = args.count("raw");
  if (!file->decoded && !raw)
    throw std::runtime_error("no decoder for " + name + "; use --raw FILE");
  if (!file->decoded && args.count("erase"))
    throw std::runtime_error("--erase refused for " + name +
                             ": format not understood yet");

  auto b = connect(args);
  auto max_bytes = args["max-bytes"].as<uint32_t>();
  std::cerr << "on device: " << b.file_size(file->id) << " bytes\n";
  auto data =
      b.download_file(file->id, max_bytes, [](size_t got, size_t total) {
        std::cerr << "\rdownloaded " << got << "/" << total << " bytes"
                  << std::flush;
      });
  std::cerr << "\n";

  if (raw) {
    auto path = args["raw"].as<std::string>();
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), data.size());
    if (!f)
      throw std::runtime_error("cannot write " + path);
    std::cerr << "wrote " << data.size() << " bytes to " << path << "\n";
  }
  if (file->decoded) {
    size_t consumed, count;
    if (file->id == Corsano::FileId::Ppg) {
      auto d = Corsano::decode_ppg(data);
      write_out(ppg_csv(d));
      consumed = d.consumed_bytes, count = d.records.size();
    } else if (file->id == Corsano::FileId::Stress) {
      auto d = Corsano::decode_stress(data);
      write_out(stress_csv(d));
      consumed = d.consumed_bytes, count = d.records.size();
    } else {
      auto d = Corsano::decode_activity(data);
      write_out(activity_csv(d));
      consumed = d.consumed_bytes, count = d.records.size();
    }
    std::cerr << count << " records\n";
    if (args.count("erase")) {
      b.erase_file(file->id, uint32_t(consumed));
      std::cerr << "erased " << consumed << " bytes\n";
    }
  }
  b.disconnect();
  return 0;
}

int cmd_plan(const cxxopts::ParseResult &args) {
  std::optional<Corsano::Plan> set;
  if (args.count("set"))
    set = parse_plan(args["set"].as<std::string>());
  Corsano::Ppg2Rate rate = Corsano::Ppg2Rate::Hz32;
  if (args.count("ppg2-rate")) {
    int hz = args["ppg2-rate"].as<int>();
    if (hz < 32 || hz > 512 || (hz & (hz - 1)))
      throw std::runtime_error("--ppg2-rate must be 32, 64, 128, 256 or 512");
    rate = Corsano::Ppg2Rate(1 + std::countr_zero(unsigned(hz / 32)));
  }
  auto b = connect(args);
  if (set)
    b.set_plan({*set, rate});
  auto p = b.plan();
  write_out(fmt("key,value\nplan,%s\nplan_id,%d\nppg2_rate_hz,%d\n"
                "ppg1_rate,%d\n",
                plan_name(p.plan), int(p.plan), ppg2_hz(p.ppg2_rate),
                p.ppg1_rate));
  b.disconnect();
  return 0;
}

// Empty values on purpose: an unedited template must not upload a fake
// profile (`--set` then fails with "missing required key")
constexpr const char *ProfileTemplate =
    R"(# Corsano user profile for `corsano-cli profile --set` and `calibrate`.
# Format: `key: value`; everything after `#` is a comment.
# Personal health data: keep it private (corsano-profile.txt is git-ignored).
# The SpO2 algorithm uses it: re-run `corsano-cli calibrate` after changes.
# birth, gender, wrist and skin are required; height and weight optional.

birth:      # date of birth, YYYY-MM-DD, e.g. 1971-02-28
gender:     # male | female | unspecified
wrist:      # wrist the bracelet is worn on: left | right
skin:       # very-fair | fair | olive | light-brown | brown | black-brown
height_cm:  # body height in cm (1-255), e.g. 175; empty = leave unchanged
weight_kg:  # body weight in kg (1-255), e.g. 70; empty = leave unchanged
)";

int cmd_profile(const cxxopts::ParseResult &args) {
  auto path = profile_path(args);
  bool set = args.count("upload") > 0, save = args.count("save") > 0;
  if (int(set) + int(save) + int(args.count("create") > 0) > 1)
    throw std::runtime_error(
        "--set, --save and --create are mutually exclusive");
  if (args.count("create")) { // no bracelet needed
    if (std::filesystem::exists(path) && !args.count("force"))
      throw std::runtime_error(path + " exists (use --force to overwrite)");
    std::ofstream f(path);
    f << ProfileTemplate;
    if (!f)
      throw std::runtime_error("cannot write " + path);
    std::cerr << "wrote template " << path << ": fill in the values\n";
    return 0;
  }
  // Validate the file before connecting
  std::optional<Corsano::UserProfile> upload;
  if (set)
    upload = load_profile(path);
  if (save && std::filesystem::exists(path) && !args.count("force"))
    throw std::runtime_error(path + " exists (use --force to overwrite)");

  auto b = connect(args);
  if (upload)
    b.set_user_profile(*upload);
  auto text = profile_text(b.user_profile(), ": ");
  if (save) {
    std::ofstream f(path);
    f << text;
    if (!f)
      throw std::runtime_error("cannot write " + path);
    std::cerr << "wrote " << path << "\n";
  } else {
    write_out(text);
  }
  b.disconnect();
  return 0;
}

int cmd_calibrate(const cxxopts::ParseResult &args) {
  double timeout = args["timeout"].as<double>();
  if (timeout <= 0)
    throw std::runtime_error("--timeout must be > 0");
  std::optional<Corsano::UserProfile> upload;
  if (!args.count("no-profile")) {
    auto path = profile_path(args);
    if (args.count("profile") || std::filesystem::exists(path))
      upload = load_profile(path);
  }

  auto b = connect(args);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  if (upload)
    b.set_user_profile(*upload);
  b.start_wearing_optimization();
  std::cerr << "sit or lie still, bracelet snug a finger above the wrist bone "
               "(2-4 min)\n";
  write_out("t,progress,movement,quality,finished\n");
  auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
  };
  while (!g_stop && elapsed() < timeout) {
    auto st = b.wearing_optimization_status();
    write_out(fmt("%.0f,%u,%u,%u,%d\n", elapsed(), st.progress_percent,
                  st.movement_level, st.quality, st.finished));
    if (st.finished) {
      std::cerr << (st.success() ? "calibration succeeded"
                                 : "calibration FAILED")
                << " (quality " << unsigned(st.quality) << ", passes at 50)\n";
      b.disconnect();
      return st.success() ? 0 : 2;
    }
    for (int i = 0; i < 50 && !g_stop; ++i)
      std::this_thread::sleep_for(100ms);
  }
  std::cerr << (g_stop ? "interrupted" : "timeout") << ", stopping...\n";
  try {
    b.stop_wearing_optimization();
  } catch (const std::exception &e) {
    std::cerr << "warning: " << e.what() << "\n";
  }
  b.disconnect();
  return 1;
}

// MARK: - follow

// Integers without decimals, floats with 2
std::string compact(double v) {
  return v == std::floor(v) && std::fabs(v) < 1e15 ? fmt("%.0f", v)
                                                   : fmt("%.2f", v);
}

int cmd_follow(const cxxopts::ParseResult &args) {
  // MARK: validate everything before connecting
  Corsano::MonitorConfig cfg;
  cfg.files.clear();
  cfg.live_hr = args.count("hrm") > 0;
  cfg.erase = !args.count("keep");
  cfg.set_time = !args.count("no-set-time");
  if (args.count("host-id"))
    cfg.host_id = args["host-id"].as<std::string>();
  double interval = args["interval"].as<double>();
  if (interval < 0)
    throw std::runtime_error("--interval must be >= 0");
  cfg.interval = std::chrono::milliseconds(int(interval * 1000));
  if (args.count("plan"))
    cfg.plan =
        Corsano::PlanSettings{parse_plan(args["plan"].as<std::string>())};
  if (args.count("profile"))
    cfg.profile = load_profile(args["profile"].as<std::string>());
  std::string raw_dir =
      args.count("raw-dir") ? args["raw-dir"].as<std::string>() : "";
  std::string list = args["files"].as<std::string>();
  for (size_t pos = 0; pos <= list.size();) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos)
      end = list.size();
    auto name = list.substr(pos, end - pos);
    pos = end + 1;
    if (name.empty())
      continue;
    const HistoryFile *hf = nullptr;
    for (auto &f : HistoryFiles)
      if (name == f.name)
        hf = &f;
    if (!hf)
      throw std::runtime_error("unknown file: " + name);
    bool monitor_decodes = hf->decoded || hf->id == Corsano::FileId::Bioz ||
                           hf->id == Corsano::FileId::Acc;
    if (!monitor_decodes) {
      if (raw_dir.empty())
        throw std::runtime_error(name +
                                 " has no decoder: add --raw-dir DIR "
                                 "to append its bytes to DIR/" +
                                 name + ".bin");
      if (!cfg.erase)
        throw std::runtime_error("--keep not supported for raw files");
    }
    cfg.files.push_back(hf->id);
  }
  if (!cfg.live_hr && cfg.files.empty())
    throw std::runtime_error("nothing to follow (no --hrm and no --files)");
  if (!raw_dir.empty()) {
    std::filesystem::create_directories(raw_dir);
    cfg.raw_sink = [&](Corsano::FileId id, std::span<const uint8_t> data) {
      for (auto &f : HistoryFiles)
        if (f.id == id) {
          std::ofstream o(raw_dir + "/" + f.name + ".bin",
                          std::ios::binary | std::ios::app);
          o.write(reinterpret_cast<const char *>(data.data()), data.size());
          if (!o)
            std::cerr << "warning: cannot append to " << raw_dir << "/"
                      << f.name << ".bin\n";
        }
    };
  }

  std::string device = device_arg(args);
  if (device.empty())
    throw std::runtime_error(
        "missing -a/--address, -n/--name or $CORSANO_ADDR (see `scan`)");

  Corsano::Monitor monitor(device, std::move(cfg));
  write_out("time,field,value\n");
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  auto deadline = std::chrono::steady_clock::time_point::max();
  if (args.count("duration"))
    deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(args["duration"].as<double>()));

  monitor.start();
  std::cerr << "following, Ctrl-C to stop\n";
  std::vector<Corsano::Sample> samples;
  std::string last_error;
  auto print = [&] {
    std::string out;
    for (auto &s : samples)
      out += fmt("%.3f,%s,", s.time, s.field) + compact(s.value) + "\n";
    samples.clear();
    if (!out.empty())
      write_out(out);
  };
  while (!g_stop && std::chrono::steady_clock::now() < deadline) {
    monitor.wait(samples, 250ms);
    print();
    auto err = monitor.last_error();
    if (err != last_error) {
      last_error = err;
      if (!err.empty())
        std::cerr << "warning: " << err << "\n";
    }
  }
  std::cerr << "stopping...\n";
  monitor.stop();
  monitor.poll(samples);
  print();
  return 0;
}

// MARK: - Raw

// CSV field, quoted if it holds a comma or quote
std::string csv_field(const std::string &s) {
  if (s.find_first_of(",\"\n") == std::string::npos)
    return s;
  std::string out = "\"";
  for (char c : s)
    out += c == '"' ? std::string("\"\"") : std::string(1, c);
  return out + "\"";
}

std::string hex_string(std::span<const uint8_t> v) {
  std::string out;
  for (uint8_t x : v)
    out += fmt("%02x", x);
  return out;
}

std::vector<uint8_t> parse_hex(const std::string &hex) {
  bool ok = hex.size() % 2 == 0;
  for (char c : hex)
    ok = ok && std::isxdigit((unsigned char)c);
  if (!ok)
    throw std::runtime_error("bad hex payload \"" + hex + "\"");
  std::vector<uint8_t> out;
  for (size_t i = 0; i < hex.size(); i += 2)
    out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return out;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string part; std::getline(ss, part, sep);)
    out.push_back(part);
  return out;
}

struct Vital {
  const char *name;
  uint8_t id;
};
constexpr Vital Vitals[] = {{"activity", 1},    {"pulse-rate", 2},
                            {"respiration", 3}, {"spo2", 4},
                            {"temperature", 5}, {"nibp", 9}};

uint8_t parse_vital(const std::string &name) {
  for (auto &v : Vitals)
    if (name == v.name)
      return v.id;
  throw std::runtime_error("unknown vital: " + name +
                           " (activity, pulse-rate, respiration, spo2, "
                           "temperature, nibp)");
}

uint8_t parse_recording_value(const std::string &v) {
  if (v == "off")
    return 1;
  if (v == "bedtime-risetime")
    return 2;
  if (v == "continuous")
    return 3;
  throw std::runtime_error("unknown recording value: " + v +
                           " (off, bedtime-risetime, continuous)");
}

uint8_t parse_recording_type(const std::string &t) {
  if (t == "stress-bioz")
    return 1;
  if (t == "acc")
    return 2;
  if (t == "temperature-raw")
    return 4;
  throw std::runtime_error("unknown recording: " + t +
                           " (stress-bioz, acc, temperature-raw)");
}

// Local HH:MM -> GMT [hh, mm] with the host's UTC offset right now
std::vector<uint8_t> local_to_gmt(const std::string &arg) {
  int h = -1, m = -1;
  char tail = 0;
  if (std::sscanf(arg.c_str(), "%d:%d%c", &h, &m, &tail) != 2 || h < 0 ||
      h > 23 || m < 0 || m > 59)
    throw std::runtime_error("bad time \"" + arg + "\" (want HH:MM, local)");
  std::time_t now = std::time(nullptr);
  std::tm l = *std::localtime(&now), g = *std::gmtime(&now);
  int day = l.tm_yday - g.tm_yday;
  if (day > 1)
    day = -1; // year wrap
  if (day < -1)
    day = 1;
  int offset = day * 1440 + (l.tm_hour - g.tm_hour) * 60 + l.tm_min - g.tm_min;
  int gmt = ((h * 60 + m - offset) % 1440 + 1440) % 1440;
  return {uint8_t(gmt / 60), uint8_t(gmt % 60)};
}

// Readable argument of a few commands, else plain hex. CLI-only sugar.
std::vector<uint8_t> parse_raw_args(uint8_t id, const std::string &arg) {
  if (arg.empty())
    return {};
  auto parts = split(arg, ',');
  switch (id) {
  case 149: // set-plan
    for (auto &p : PlanNames)
      if (arg == p.name)
        return {uint8_t(p.plan), uint8_t(Corsano::Ppg2Rate::Hz32), 60};
    if (std::isalpha((unsigned char)arg[0]) && arg.size() > 2 &&
        !std::isxdigit((unsigned char)arg[2]))
      parse_plan(arg); // throws with the list of names
    break;
  case 234: // set-sampling
    if (parts.size() == 2) {
      int sec = std::stoi(parts[1]);
      if (sec < 0 || sec > 65535)
        throw std::runtime_error("bad interval: " + parts[1]);
      return {parse_vital(parts[0]), uint8_t(sec & 0xFF), uint8_t(sec >> 8)};
    }
    break;
  case 235: // get-sampling
    if (!std::isxdigit((unsigned char)arg[0]) || arg.size() > 2)
      return {parse_vital(arg)};
    break;
  case 40:
  case 41: // set-bedtime, set-risetime
    if (arg.find(':') != std::string::npos)
      return local_to_gmt(arg);
    break;
  case 192: // set-recording
    if (parts.size() == 2) {
      uint8_t type = parse_recording_type(parts[0]);
      uint8_t v = parse_recording_value(parts[1]);
      if (type == 1)
        return {type, v, v};
      return {type, v};
    }
    break;
  case 193: // get-recording
    if (std::isalpha((unsigned char)arg[0]) && arg.size() > 2)
      return {parse_recording_type(arg)};
    break;
  case 220: // set-realtime
    if (arg == "on" || arg == "off")
      return {uint8_t(arg == "on")};
    break;
  case 46: // get-file-size
    for (auto &f : HistoryFiles)
      if (arg == f.name)
        return {uint8_t(f.id)};
    break;
  default:
    break;
  }
  return parse_hex(arg);
}

// NAME or number, optionally followed by :ARGS
std::pair<uint8_t, std::vector<uint8_t>> parse_raw_spec(const std::string &s) {
  auto colon = s.find(':');
  std::string head = s.substr(0, colon);
  std::string arg = colon == std::string::npos ? "" : s.substr(colon + 1);
  int id = -1;
  if (!head.empty() && std::all_of(head.begin(), head.end(), [](char c) {
        return std::isdigit((unsigned char)c);
      })) {
    id = std::stoi(head);
  } else if (auto *c = Corsano::find_command(head)) {
    id = c->id;
  } else {
    std::string close;
    for (auto &c : Corsano::command_catalog())
      if (!head.empty() &&
          (std::string_view(c.name).find(head) != std::string_view::npos ||
           head.find(c.name) != std::string::npos))
        close += std::string(close.empty() ? "" : ", ") + c.name;
    throw std::runtime_error(
        "unknown command \"" + head + "\"" +
        (close.empty() ? "" : " (did you mean: " + close + "?)") +
        "; see `raw --list`");
  }
  if (id > 255)
    throw std::runtime_error("bad command id " + head + " (0..255)");
  try {
    return {uint8_t(id), parse_raw_args(uint8_t(id), arg)};
  } catch (const std::logic_error &e) { // stoi
    throw std::runtime_error("bad argument in -x " + s);
  }
}

// Debug/reverse-engineering aid: each -x NAME|ID[:ARGS] is sent in order and
// its response payload printed as hex and decoded
int cmd_raw(const cxxopts::ParseResult &args) {
  if (args.count("list")) {
    write_out("id,name,set,description\n");
    for (auto &c : Corsano::command_catalog())
      write_out(fmt("%u,%s,%s,%s\n", c.id, c.name, c.is_set ? "yes" : "no",
                    csv_field(c.description).c_str()));
    return 0;
  }
  if (!args.count("send"))
    throw std::runtime_error("raw needs at least one -x NAME|ID[:ARGS] "
                             "(or --list)");
  // Parse everything before connecting, so typos fail fast
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> requests;
  for (auto &s : args["send"].as<std::vector<std::string>>())
    requests.push_back(parse_raw_spec(s));
  auto b = connect(args);
  write_out("id,name,request,response,decoded\n");
  for (auto &[id, payload] : requests) {
    auto response = b.command(id, payload);
    auto *info = Corsano::find_command(id);
    write_out(fmt("%u,%s,%s,%s,%s\n", id, info ? info->name : "",
                  hex_string(payload).c_str(), hex_string(response).c_str(),
                  csv_field(Corsano::describe_reply(id, response)).c_str()));
  }
  b.disconnect();
  return 0;
}

} // namespace

// MARK: - Main

int main(int argc, char **argv) {
  cxxopts::Options options("corsano-cli", "Corsano bracelet CSV streamer");
  options
      .positional_help(
          "scan|info|live|history|follow|plan|profile|calibrate|raw")
      .show_positional_help();
  // clang-format off
  options.add_options()
    ("command", "scan, info, live, history, follow, plan, profile, calibrate or raw", cxxopts::value<std::string>())
    ("a,address", "Device address (MAC on Linux, UUID on macOS); if neither -a nor -n is given, $CORSANO_ADDR (address or name) is used", cxxopts::value<std::string>())
    ("n,name", "Device name as shown by `scan` (alternative to -a)", cxxopts::value<std::string>())
    ("d,duration", "scan/live/follow: stop after N seconds (default: until Ctrl-C)", cxxopts::value<double>())
    ("p,period", "live: ping period in seconds", cxxopts::value<double>()->default_value("1"))
    ("sampling", "live: vital sampling interval in seconds (pulse rate, SpO2)", cxxopts::value<double>())
    ("set-time", "live: set the bracelet clock first")
    ("no-set-time", "follow: do not sync the bracelet clock on connect")
    ("realtime", "live: use realtime pings (HR, SpO2, resp, temp) instead of workout-mode LiveHrm (default; logs a workout on the bracelet)")
    ("max-bytes", "history: max bytes to download", cxxopts::value<uint32_t>()->default_value("38000"))
    ("file", "history: ppg, activity, hrv, workout, sleep, logs, ppg2, ecg, bioz, greenteg, stress or acc", cxxopts::value<std::string>()->default_value("activity"))
    ("raw", "history: also write the downloaded bytes to FILE (required for files without decoder)", cxxopts::value<std::string>())
    ("x,send", "raw: command NAME|ID[:ARGS], repeatable (e.g. -x get-plan -x 223:05)", cxxopts::value<std::vector<std::string>>())
    ("erase", "history: erase downloaded records from the bracelet (decoded files only)")
    ("files", "follow: comma-separated files to poll (decoded: activity, stress, ppg, bioz, acc; others need --raw-dir)", cxxopts::value<std::string>()->default_value("activity,stress,bioz"))
    ("interval", "follow: seconds per polling pass (default 0 = back-to-back)", cxxopts::value<double>()->default_value("0"))
    ("keep", "follow: do NOT erase downloaded data; emit only newer records")
    ("hrm", "follow: add workout-mode LiveHrm (HR every 2 s); stops Activity logging while on")
    ("plan", "follow: apply a measurement plan on every (re)connect (see `plan`)", cxxopts::value<std::string>())
    ("set", "plan: set this plan (e.g. hospital) before printing", cxxopts::value<std::string>())
    ("host-id", "FW >= 8.41 authentication id (default: host name); keep it the same across runs", cxxopts::value<std::string>())
    ("upload", "profile: upload the profile file first (spelled --set on the command line)")
    ("profile", "profile file (default ./corsano-profile.txt); profile, calibrate, follow", cxxopts::value<std::string>())
    ("save", "profile: write the bracelet's profile to the profile file")
    ("create", "profile: write a commented template to the profile file (no bracelet needed)")
    ("force", "profile: with --save/--create, overwrite an existing file")
    ("no-profile", "calibrate: do not upload the profile file first")
    ("timeout", "calibrate: give up after S seconds", cxxopts::value<double>()->default_value("360"))
    ("ppg2-rate", "plan: with --set, raw PPG2 rate in Hz (32, 64, 128, 256, 512)", cxxopts::value<int>())
    ("raw-dir", "follow: append undecoded files to DIR/<name>.bin", cxxopts::value<std::string>())
    ("list", "raw: print the known commands as CSV (no bracelet needed)")
    ("h,help", "Show help");
  // clang-format on
  options.parse_positional({"command"});

  // `profile --set` is a flag, `plan --set NAME` takes a value. Only the
  // command word counts (so a device or file named "profile" is harmless).
  // shortcut: the command must come first for this, as in the README examples
  std::vector<std::string> words(argv, argv + argc);
  if (words.size() > 1 && words[1] == "profile")
    for (auto &w : words)
      if (w == "--set")
        w = "--upload";
  std::vector<const char *> av;
  for (auto &w : words)
    av.push_back(w.c_str());

  try {
    auto args = options.parse(int(av.size()), av.data());
    if (args.count("help") || !args.count("command")) {
      std::cout << options.help() << "\n";
      return args.count("help") ? 0 : 1;
    }
    auto cmd = args["command"].as<std::string>();
    if (cmd == "scan")
      return cmd_scan(args);
    if (cmd == "info")
      return cmd_info(args);
    if (cmd == "live")
      return cmd_live(args);
    if (cmd == "history")
      return cmd_history(args);
    if (cmd == "follow")
      return cmd_follow(args);
    if (cmd == "plan")
      return cmd_plan(args);
    if (cmd == "profile")
      return cmd_profile(args);
    if (cmd == "calibrate")
      return cmd_calibrate(args);
    if (cmd == "raw")
      return cmd_raw(args);
    throw std::runtime_error("unknown command: " + cmd);
  } catch (const Corsano::PairingRequired &) {
    std::cerr << "error: not paired. Pair first: macOS accept the PIN dialog "
                 "(999999); Linux: bluetoothctl pair <MAC>\n";
  } catch (const Corsano::Timeout &e) {
    std::cerr << "error: timeout (" << e.what()
              << "). Is the bracelet awake, in range, and not connected to "
                 "another host?\n";
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
  }
  return 1;
}
