// Offline tests of the command catalog; replies are real FW 8.12 captures.
#include "corsano/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

using namespace Corsano;
using Bytes = std::vector<uint8_t>;

// MARK: - Catalog

TEST_CASE("catalog is sorted, unique and complete", "[catalog]") {
  auto cat = command_catalog();
  REQUIRE(cat.size() > 40);
  std::set<std::string> names;
  for (size_t i = 0; i < cat.size(); ++i) {
    if (i)
      CHECK(cat[i - 1].id < cat[i].id);
    CHECK(names.insert(cat[i].name).second);
  }
  using C = CommandId;
  for (auto id : {C::SetWorkoutMode,
                  C::EraseAllFiles,
                  C::UpdateTime,
                  C::ResetWatch,
                  C::SetHeight,
                  C::SetWeight,
                  C::SetProfile,
                  C::EraseFromFile,
                  C::GetFileSize,
                  C::StartStreamingFileWithSize,
                  C::GetActiveMode,
                  C::GetBatteryLevel,
                  C::GetCurrentTime,
                  C::GetHeight,
                  C::GetWeight,
                  C::GetBirthdate,
                  C::GetProfile,
                  C::GetFwVersion,
                  C::GetHwId,
                  C::GetSerialNumber,
                  C::GetDeviceName,
                  C::GetBondingState,
                  C::Shutdown,
                  C::RegularPing,
                  C::SetPlan,
                  C::GetPlan,
                  C::WorkoutStop,
                  C::GetBraceletStatus,
                  C::LiveHrmPing,
                  C::StartSpecialMode,
                  C::StopSpecialMode,
                  C::GetSpecialMode,
                  C::Authenticate,
                  C::GetAuthStatus,
                  C::SetRealtimeEnable,
                  C::GetRealtimeEnable,
                  C::RealtimePing,
                  C::SetRealtimePeriod,
                  C::GetRealtimePeriod,
                  C::SetVitalsSamplingRate,
                  C::GetVitalsSamplingRate})
    CHECK(find_command(uint8_t(id)) != nullptr);
}

TEST_CASE("find_command", "[catalog]") {
  auto *p = find_command("get-plan");
  REQUIRE(p);
  CHECK(p->id == 150);
  CHECK(find_command("GET_Plan") == p);
  CHECK(find_command(uint8_t(150)) == p);
  CHECK(find_command("get-pla") == nullptr);
  CHECK(find_command("get-plans") == nullptr);
  CHECK(find_command("") == nullptr);
  CHECK(find_command(uint8_t(3)) == nullptr);
  CHECK(find_command("set-plan")->is_set);
  CHECK_FALSE(p->is_set);
}

// MARK: - describe_reply

TEST_CASE("describe_reply on real replies", "[catalog]") {
  struct Case {
    uint8_t id;
    Bytes payload;
    const char *text;
  };
  const Case cases[] = {
      {150,
       {0x07, 0x01, 0x3c, 0x00},
       "plan=typical-multicolor ppg2=32Hz ppg1=60"},
      {235, {0x04, 0x3c, 0x00, 0x00}, "vital=spo2 interval=60s"},
      {223, {0x01, 0, 0, 0}, "status=refused(1)"},
      {149, {0x64, 0, 0, 0}, "status=ok"},
      {51, {0x6c, 0x28, 0xf3, 0x6a}, "time=2026-11-10T14:21:32Z"},
      {50, {0x51, 0x7d, 0x0f, 0x03}, "battery=81% voltage=3965mV charging=yes"},
      {63, {0, 0, 0x08, 0x0c}, "firmware=8.12"},
      {46, {0x02, 0x52, 0xb7, 0, 0}, "file=activity size=46930B"},
      {46, {0x52, 0xb7, 0, 0}, "size=46930B"},
      {57, {0x37, 0, 0x01, 0x03}, "age=55 gender=male wrist=right skin=olive"},
      {56, {0xb3, 0x07, 0x02, 0x1c}, "birth=1971-02-28"},
      {54, {0xb9, 0, 0, 0}, "height=185cm"},
      {52, {0x14, 0x1e, 0, 0}, "time=20:30Z"},
      {156,
       {4, 0, 0x42, 0, 4, 0, 0x63},
       "wearing=4 mode=0 battery=66% charging=no idle=4 wearing-quality=99"},
      {152,
       {0, 0, 0, 0, 0, 0, 0, 0, 2},
       "mode=0 session=0 start=none duration=0s status=finished"},
      {152, // FW 8.12, 2026-11-10: start is not an epoch time
       {0x31, 0x33, 0xed, 0x0a, 0, 0, 0x71, 0x57, 0},
       "mode=49 session=51 start=2797s duration=22385s status=not-started"},
      {246,
       {4, 0, 1, 0, 100, 50},
       "finished=yes progress=100% movement=0 quality=50 result=pass"},
      {193, {1, 3, 3, 0}, "stress=continuous bioz=continuous"},
      {193, {2, 1, 0, 0}, "acc=off"},
      {224, {0x19, 0x0b, 0, 0}, "period=25s"},
      {70, {'B', '2', '1', 0, 'x'}, "serial=B21"},
  };
  for (auto &c : cases)
    CHECK(describe_reply(c.id, c.payload) == c.text);
}

TEST_CASE("describe_reply never throws", "[catalog]") {
  CHECK(describe_reply(3, Bytes{1, 2, 3, 4}).empty());
  for (auto &c : command_catalog())
    for (size_t n = 0; n < 3; ++n)
      CHECK_NOTHROW(describe_reply(c.id, Bytes(n, 0xff)));
  CHECK(describe_reply(150, {}).empty());
  CHECK(describe_reply(51, Bytes{1, 2}).empty());
  CHECK(describe_reply(246, Bytes{4, 0, 1}).empty());
}
