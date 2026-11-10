/// @file monitor.hpp
/// @brief Continuous monitoring on a background thread.
///
/// One background thread keeps a Bracelet connected (reconnecting with
/// backoff), polls and erases recorded files, optionally streams workout-mode
/// LiveHrm, and queues every value as a tidy Sample. The caller drains the
/// queue either at fixed timesteps (Monitor::poll()) or as soon as data arrives
/// (Monitor::wait()). Field names match `corsano-cli follow`.
///
/// Decoded fields come from files.hpp; file semantics: docs/PROTOCOL.md,
/// HR and SpO2: docs/HR_SPO2.md. The C counterpart is the "Monitor" section of
/// corsano.h.
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

/// @defgroup corsano_monitor Continuous monitoring
/// @brief Background monitor that turns bracelet data into a sample queue.
/// @{

/// @brief One tidy measurement queued by the Monitor.
///
/// The value of field `activity.spo2` is ActivityRecord::spo2, and so on for
/// the other decoded record types.
/// @see MonitorConfig for the list of fields.
struct Sample {
  double time;       ///< Unix epoch seconds (ms resolution)
  const char *field; ///< Static string, e.g. "activity.spo2"; never freed
  double value;      ///< Measured value.
  uint32_t record;   ///< Samples decoded from the same record share it
};

/// @brief Monitor options.
///
/// Fields (see cli/README.md for the full list):
/// - `activity.*` (ActivityRecord), `stress.*` (StressRecord), `bioz.z`
///   (BiozChunk), `ppg.*` (PpgSample), `acc.x/y/z` (AccChunk),
///   `hrm.*` (LiveHrm, live_hr only)
/// - `status.connected` (0/1, on every change)
/// - `status.dropped` (samples discarded because the queue was full)
struct MonitorConfig {
  /// Files to poll, decode and queue (FileId).
  std::vector<FileId> files{FileId::Activity, FileId::Stress, FileId::Bioz};
  /// Workout-mode LiveHrm (HR every 2 s). Stops Activity logging while on.
  bool live_hr = false;
  bool erase = true;    ///< Erase each file after decoding (else dedupe by time)
  bool set_time = true; ///< Sync bracelet clock on every (re)connect
  /// Applied on every (re)connect when set, e.g. Plan::Hospital for SpO2 1/10 s
  std::optional<PlanSettings> plan;
  /// Applied on every (re)connect when set (before the plan)
  std::optional<UserProfile> profile;
  std::chrono::milliseconds interval{0}; ///< Min time per poll pass; 0 = ASAP
  size_t max_queue = 100'000;            ///< Queue limit; full: drop oldest, report dropped
  /// Called (on the monitor thread) with every downloaded buffer, before
  /// erasing; lets callers keep raw bytes, e.g. of files without a decoder,
  /// which are then polled and erased like the others.
  std::function<void(FileId, std::span<const uint8_t>)> raw_sink;
};

/// @brief Background monitor: keeps a bracelet connected and queues Sample values.
///
/// Not copyable. Destruction calls stop().
/// @see Bracelet for the underlying connection.
class Monitor {
public:
  /// @brief Create a stopped monitor.
  /// @param device Address (MAC on Linux, UUID on macOS) or advertised name.
  /// @param config Monitor options.
  explicit Monitor(std::string device, MonitorConfig config = {});
  ~Monitor(); ///< Calls stop().
  Monitor(const Monitor &) = delete;
  Monitor &operator=(const Monitor &) = delete;

  // MARK: - Lifecycle
  /// @name Lifecycle
  /// @{

  /// @brief Start the background thread.
  ///
  /// Returns at once; connection happens on the thread (reported as
  /// status.connected).
  /// @throws std::logic_error if already running.
  void start();
  /// @brief Stop the thread, leave workout mode, disconnect; wakes wait(). Idempotent.
  void stop();
  /// @brief Whether the background thread is running.
  /// @return True while running.
  bool running() const;
  /// @brief Last error seen by the background thread; it keeps retrying.
  /// @return The error message, "" if none.
  std::string last_error() const;
  /// @}

  // MARK: - Device control
  /// @name Device control
  /// @{

  /// @brief Set the bracelet clock to the host clock now.
  ///
  /// Also done on every (re)connect when MonitorConfig::set_time. Blocks while
  /// a file download is in progress.
  /// @throws std::runtime_error if not connected.
  /// @throws ProtocolError if the bracelet refuses.
  void sync_clock();
  /// @}

  // MARK: - Reading
  /// @name Reading
  /// @{

  /// @brief Non-blocking: move up to @p max queued samples to the end of @p out.
  /// @param out Vector the samples are appended to.
  /// @param max Maximum number of samples to move.
  /// @return How many were added (0 if none).
  size_t poll(std::vector<Sample> &out, size_t max = SIZE_MAX);
  /// @brief Blocking: like poll(), but first waits until at least one sample is
  ///        queued, @p timeout elapses or stop() is called.
  /// @param out Vector the samples are appended to.
  /// @param timeout Maximum time to wait for a sample.
  /// @param max Maximum number of samples to move.
  /// @return How many were added (0 if none).
  size_t wait(std::vector<Sample> &out, std::chrono::milliseconds timeout,
              size_t max = SIZE_MAX);
  /// @}

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

/// @}

} // namespace Corsano
