// Online tests: need a paired bracelet. Set CORSANO_ADDRESS or CORSANO_ADDR
// (address or advertised name) to run.
// Never erases files on the device.
#include "corsano/bracelet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

using namespace Corsano;
using namespace std::chrono_literals;

// MARK: - Helpers

// CORSANO_ADDRESS or CORSANO_ADDR (as the CLI); a name is resolved once to
// its address by scanning, since Bracelet takes addresses only
const char *address_env() {
  static const std::string address = [] {
    const char *v = std::getenv("CORSANO_ADDRESS");
    if (!v || !*v)
      v = std::getenv("CORSANO_ADDR");
    if (!v || !*v)
      return std::string();
    for (auto &d : Bracelet::scan(10s))
      if (d.name == v)
        return d.address;
    return std::string(v);
  }();
  return address.empty() ? nullptr : address.c_str();
}

#define REQUIRE_DEVICE()                                                       \
  if (!address_env())                                                          \
  SKIP("set CORSANO_ADDRESS or CORSANO_ADDR")

// One connection shared by all cases, reconnected lazily
static Bracelet &bracelet_instance() {
  static Bracelet bracelet(address_env());
  return bracelet;
}

static Bracelet &shared_bracelet() {
  auto &bracelet = bracelet_instance();
  if (!bracelet.is_connected())
    bracelet.connect();
  return bracelet;
}

// A connected bracelet stops advertising, so Monitor (own connection) could
// not find it while this process holds the shared one
void release_shared_bracelet() { bracelet_instance().disconnect(); }

// MARK: - Discovery

TEST_CASE("scan finds the bracelet", "[online]") {
  REQUIRE_DEVICE();
  auto devices = Bracelet::scan(10s);
  bool found = false;
  for (const auto &d : devices)
    found = found || d.address == address_env();
  CHECK(found);
}

// MARK: - Device info

TEST_CASE("battery is plausible", "[online]") {
  REQUIRE_DEVICE();
  auto b = shared_bracelet().battery();
  CHECK(b.percent <= 100);
  CHECK(b.millivolts >= 3000);
  CHECK(b.millivolts <= 4500);
}

TEST_CASE("firmware version", "[online]") {
  REQUIRE_DEVICE();
  CHECK(shared_bracelet().firmware_version().major > 0);
}

TEST_CASE("serial number is not empty", "[online]") {
  REQUIRE_DEVICE();
  CHECK_FALSE(shared_bracelet().serial_number().empty());
}

TEST_CASE("set_time succeeds", "[online]") {
  REQUIRE_DEVICE();
  CHECK_NOTHROW(shared_bracelet().set_time());
}

// MARK: - Realtime

TEST_CASE("realtime pings carry plausible vitals", "[online]") {
  REQUIRE_DEVICE();
  auto &bracelet = shared_bracelet();

  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<RealtimePing> pings;
  };
  auto state = std::make_shared<State>();

  bracelet.set_notification_callback([state](const Notification &n) {
    if (auto *p = std::get_if<RealtimePing>(&n)) {
      std::lock_guard lock(state->mutex);
      state->pings.push_back(*p);
      state->cv.notify_all();
    }
  });
  // Leave the bracelet's config as found (FW 8.12 also rejects period sets)
  const bool was_enabled = bracelet.realtime_enabled();
  const auto wait = bracelet.realtime_period() * 2 + 5s;
  if (!was_enabled)
    bracelet.set_realtime_enabled(true);

  {
    std::unique_lock lock(state->mutex);
    state->cv.wait_for(lock, wait, [&] { return !state->pings.empty(); });
  }
  if (!was_enabled)
    bracelet.set_realtime_enabled(false);
  bracelet.set_notification_callback({});

  std::lock_guard lock(state->mutex);
  REQUIRE_FALSE(state->pings.empty());
  for (const auto &p : state->pings) {
    CHECK(((p.heart_rate >= 30 && p.heart_rate <= 220) ||
           p.heart_rate_quality == 0));
    // spo2 0 = not measured; spo2_quality then carries a status (FW 8.12: 103,
    // 106)
    CHECK((p.spo2 == 0 || (p.spo2 >= 70 && p.spo2 <= 100)));
  }
}

// MARK: - Authentication

TEST_CASE("authentication", "[online]") {
  REQUIRE_DEVICE();
  auto &bracelet = shared_bracelet();
  CHECK_FALSE(bracelet.host_id().empty());
  CHECK(bracelet.authenticated()); // FW < 8.41: true, nothing sent
}

// MARK: - Files

TEST_CASE("activity file size and download", "[online]") {
  REQUIRE_DEVICE();
  auto &bracelet = shared_bracelet();
  CHECK_NOTHROW(bracelet.file_size(FileId::Activity));

  auto data = bracelet.download_file(FileId::Activity, 38 * 10);
  CHECK(data.size() % 38 == 0);
  CHECK_NOTHROW(decode_activity(data));
}

TEST_CASE("ppg file download and decode", "[online]") {
  REQUIRE_DEVICE();
  auto &bracelet = shared_bracelet();
  auto data = bracelet.download_file(FileId::Ppg, 481 * 4);
  Decoded<PpgChunk> d;
  CHECK_NOTHROW(d = decode_ppg(data));
  CHECK(d.consumed_bytes <= data.size());
}

// MARK: - Profile

TEST_CASE("user profile round trip", "[online]") {
  REQUIRE_DEVICE();
  auto &bracelet = shared_bracelet();
  auto before = bracelet.user_profile();
  bracelet.set_user_profile(before);
  auto after = bracelet.user_profile();
  CHECK(after.birth == before.birth);
  CHECK(after.gender == before.gender);
  CHECK(after.wrist == before.wrist);
  CHECK(after.skin == before.skin);
  CHECK(after.height_cm == before.height_cm);
  CHECK(after.weight_kg == before.weight_kg);
}

TEST_CASE("wearing optimization status", "[online]") {
  REQUIRE_DEVICE();
  CHECK_NOTHROW(shared_bracelet().wearing_optimization_status());
}
