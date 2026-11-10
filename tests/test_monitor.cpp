// Online Monitor test: needs CORSANO_ADDRESS or CORSANO_ADDR. Erases nothing it
// did not decode, but Monitor erases decoded files by default: opt out with
// erase=false.
#include "corsano/monitor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <stdexcept>

using namespace Corsano;
using namespace std::chrono_literals;

void release_shared_bracelet(); // test_online.cpp
const char *address_env();      // test_online.cpp

TEST_CASE("Monitor construct/stop without start is safe", "[monitor]") {
  Monitor m("nothing");
  REQUIRE_FALSE(m.running());
  m.stop();
  m.stop();
  std::vector<Sample> out;
  REQUIRE(m.poll(out) == 0);
  REQUIRE(m.wait(out, 10ms) == 0);
  REQUIRE_THROWS_AS(m.sync_clock(), std::runtime_error); // not connected
}

TEST_CASE("Monitor streams samples", "[online][monitor]") {
  const char *addr = address_env();
  if (!addr || !*addr)
    SKIP("set CORSANO_ADDRESS or CORSANO_ADDR");
  release_shared_bracelet();
  MonitorConfig cfg;
  cfg.erase = false; // tests never erase device data
  Monitor m(addr, cfg);
  m.start();
  REQUIRE_THROWS_AS(m.start(), std::logic_error);

  std::vector<Sample> all;
  bool connected = false, data = false;
  auto deadline = std::chrono::steady_clock::now() + 60s;
  while (std::chrono::steady_clock::now() < deadline && !(connected && data)) {
    size_t from = all.size();
    m.wait(all, 2s);
    for (size_t i = from; i < all.size(); ++i) {
      std::string f = all[i].field;
      connected |= f == "status.connected" && all[i].value == 1;
      data |= f == "bioz.z" || f.rfind("activity.", 0) == 0;
    }
  }
  if (connected)
    CHECK_NOTHROW(m.sync_clock()); // concurrent with the worker's polling
  auto t0 = std::chrono::steady_clock::now();
  m.stop();
  REQUIRE(std::chrono::steady_clock::now() - t0 < 10s);
  REQUIRE_FALSE(m.running());
  t0 = std::chrono::steady_clock::now();
  m.poll(all);
  REQUIRE(std::chrono::steady_clock::now() - t0 < 1s);
  INFO("last_error: " << m.last_error());
  REQUIRE(connected);
  REQUIRE(data);
}
