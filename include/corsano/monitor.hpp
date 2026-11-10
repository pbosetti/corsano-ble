// Continuous monitoring: one background thread keeps a Bracelet connected
// (reconnecting with backoff), polls and erases recorded files, optionally
// streams workout-mode LiveHrm, and queues every value as a tidy Sample.
// The caller drains the queue either at fixed timesteps (poll) or as soon as
// data arrives (wait). Field names match `corsano-cli follow`.
#pragma once

#include "corsano/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Corsano {

struct Sample {
  double time;       // Unix epoch seconds (ms resolution)
  const char *field; // static string, e.g. "activity.spo2"; never freed
  double value;
  uint32_t record;   // samples decoded from the same record share it
};

// Fields (see cli/README.md for the full list):
//   activity.* stress.* bioz.z ppg.* acc.x/y/z hrm.* (live_hr only)
//   status.connected (0/1, on every change)
//   status.dropped   (samples discarded because the queue was full)
struct MonitorConfig {
  std::vector<FileId> files{FileId::Activity, FileId::Stress, FileId::Bioz};
  // Workout-mode LiveHrm (HR every 2 s). Stops Activity logging while on.
  bool live_hr = false;
  bool erase = true;    // erase each file after decoding (else dedupe by time)
  bool set_time = true; // sync bracelet clock on every (re)connect
  // Applied on every (re)connect when set, e.g. Plan::Hospital for SpO2 1/10 s
  std::optional<PlanSettings> plan;
  // Applied on every (re)connect when set (before the plan)
  std::optional<UserProfile> profile;
  std::chrono::milliseconds interval{0}; // min time per poll pass; 0 = ASAP
  size_t max_queue = 100'000;            // full: drop oldest, report dropped
  // Called (on the monitor thread) with every downloaded buffer, before
  // erasing; lets callers keep raw bytes, e.g. of files without a decoder,
  // which are then polled and erased like the others.
  std::function<void(FileId, std::span<const uint8_t>)> raw_sink;
};

class Monitor {
public:
  // `device`: address (MAC on Linux, UUID on macOS) or advertised name
  explicit Monitor(std::string device, MonitorConfig config = {});
  ~Monitor(); // stop()
  Monitor(const Monitor &) = delete;
  Monitor &operator=(const Monitor &) = delete;

  // MARK: - Lifecycle

  // Starts the background thread and returns at once; connection happens
  // there (reported as status.connected). Throws if already running.
  void start();
  // Stops the thread, leaves workout mode, disconnects; wakes wait(). Idempotent.
  void stop();
  bool running() const;
  // Last error seen by the background thread ("" if none); it keeps retrying
  std::string last_error() const;

  // MARK: - Device control

  // Sets the bracelet clock to the host clock now (also done on every
  // (re)connect when MonitorConfig::set_time). Blocks while a file download
  // is in progress. Throws std::runtime_error if not connected, ProtocolError
  // if the bracelet refuses.
  void sync_clock();

  // MARK: - Reading

  // Non-blocking: moves up to max queued samples to the end of `out`.
  // Returns how many were added (0 if none).
  size_t poll(std::vector<Sample> &out, size_t max = SIZE_MAX);
  // Blocking: like poll(), but first waits until at least one sample is
  // queued, `timeout` elapses or stop() is called.
  size_t wait(std::vector<Sample> &out, std::chrono::milliseconds timeout,
              size_t max = SIZE_MAX);

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace Corsano
