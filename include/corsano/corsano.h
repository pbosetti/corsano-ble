/**
 * @file corsano.h
 * @brief C API over Corsano::Bracelet, Corsano::Monitor and the file decoders.
 *
 * Shaped for ctypes: opaque handle, plain structs, caller-owned buffers (no
 * library-side allocation to free), int status codes. Callbacks run on the BLE
 * backend thread.
 *
 * Every function returning `int` returns @ref CORSANO_OK (0) or a negative
 * `CORSANO_ERR_*` code; the message of the last failure on the calling thread
 * is available from corsano_last_error(). Handles are thread-safe like their
 * C++ counterparts (Corsano::Bracelet, Corsano::Monitor), whose documentation
 * describes the behaviour of each call in more detail. Wire format:
 * docs/PROTOCOL.md; HR and SpO2 fields: docs/HR_SPO2.md.
 */
#ifndef CORSANO_H
#define CORSANO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup corsano_c C API
 * @brief Plain-C interface for non-C++ callers and FFI.
 * @{
 */

/* MARK: - Status */

/**
 * @brief Status codes returned by every `int` function.
 *
 * The C++ exception mapping is: Corsano::Timeout, Corsano::PairingRequired,
 * Corsano::ProtocolError, std::invalid_argument, anything else.
 */
enum {
  CORSANO_OK = 0,             /**< Success. */
  CORSANO_ERR_TIMEOUT = -1,   /**< Corsano::Timeout. */
  CORSANO_ERR_PAIRING = -2,  /* bracelet not bonded with this host */
  CORSANO_ERR_PROTOCOL = -3, /* bad CRC, status != OK, short payload */
  CORSANO_ERR_ARGUMENT = -4, /**< Invalid argument (NULL, out of range, buffer too small). */
  CORSANO_ERR_BLE = -5, /* any other failure */
};

/**
 * @brief Message of the last failed call on the calling thread.
 * @return Message, "" if none. The pointer is owned by the library and valid
 *         until the next call on the same thread.
 */
const char *corsano_last_error(void);

/* MARK: - Data types (mirror protocol.hpp) */

/** @brief A bracelet found by corsano_scan(); mirrors Corsano::DeviceInfo. */
typedef struct {
  char name[64];    /**< Advertised name, NUL-terminated. */
  char address[64]; /**< MAC on Linux, CoreBluetooth UUID on macOS */
  int16_t rssi;     /**< Signal strength in dBm. */
} corsano_device_t;

/** @brief Battery state; mirrors Corsano::BatteryLevel. */
typedef struct {
  uint8_t percent;     /**< Charge in %. */
  uint16_t millivolts; /**< Voltage in mV. */
  bool charging;       /**< Currently charging. */
} corsano_battery_t;

/** @brief Firmware version; mirrors Corsano::FirmwareVersion. */
typedef struct {
  int major; /**< Major version. */
  int minor; /**< Minor version. */
} corsano_firmware_t;

/** @brief Realtime vitals; mirrors Corsano::RealtimePing. Quality 0 = bad ... 4 = very good. */
typedef struct {
  uint32_t timestamp_s; /**< Bracelet clock, epoch seconds. */
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; /**< Heart rate quality (0..4) */
  float respiration_rate; /**< Breaths/min. */
  uint8_t respiration_rate_quality; /**< Respiration quality. */
  uint8_t spo2, /**< SpO2 in % */spo2_quality; /**< SpO2 quality (0..4), or an alert value */
  float core_body_temp_c; /**< Core body temperature in degrees C. */
  uint8_t movement_level; /**< Movement level. */
  uint8_t battery_percent; /**< Battery charge in %. */
} corsano_realtime_t;

/** @brief Live workout heart rate; mirrors Corsano::LiveHrm. */
typedef struct {
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; /**< Heart rate quality (0..4) */
  uint16_t steps; /**< Steps. */
  uint16_t energy; /**< Energy. */
  uint8_t max_heart_rate, /**< Maximum heart rate */speed; /**< Current speed */
  uint16_t duration; /**< Workout duration. */
  uint8_t battery_percent; /**< Battery charge in %. */
  uint8_t average_heart_rate, /**< Average heart rate */average_speed; /**< Average speed */
} corsano_live_hrm_t;

/** @brief Periodic ping; mirrors Corsano::RegularPing. */
typedef struct {
  uint8_t battery_percent; /**< Battery charge in %. */
  uint16_t ppg_size, /**< PPG file */activity_size, /**< Activity file */hrv_size, /**< HRV file */workout_size, /**< Workout file */sleep_size, /**< Sleep file */
      log_size; /**< Logs file; all sizes are bytes pending on the device. */
} corsano_regular_ping_t;

/** @brief One Activity record; mirrors Corsano::ActivityRecord. */
typedef struct {
  uint32_t timestamp_s; /**< Record time, epoch seconds. */
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; /**< Heart rate quality (0..4) */
  uint16_t steps; /**< Steps. */
  uint8_t activity_type, /**< Activity type */activity_count; /**< Activity count */
  uint8_t spo2, /**< SpO2 in % */spo2_quality; /**< SpO2 quality (0..4), or an alert value */
  uint16_t energy; /**< Energy. */
  float respiration_rate; /**< Breaths/min. */
  uint8_t respiration_rate_quality; /**< Respiration quality. */
  uint8_t battery_percent; /**< Battery charge in %. */
  bool charging; /**< Charging at record time. */
  uint8_t nibp_systolic, /**< Systolic blood pressure */nibp_diastolic; /**< Diastolic blood pressure */
  float temp1_c, /**< Temperature 1 in degrees C */temp2_c; /**< Temperature 2 in degrees C */
  uint8_t wearing; /**< Wearing state. */
} corsano_activity_t;

/** @brief File id of the Activity file (Corsano::FileId::Activity). */
enum { CORSANO_FILE_ACTIVITY = 2 };
/** @brief Vital ids (Corsano::VitalType). */
enum { CORSANO_VITAL_PULSE_RATE = 2, /**< Corsano::VitalType::PulseRate. */CORSANO_VITAL_SPO2 = 4 /**< Corsano::VitalType::Spo2. */};
/* File and vital ids: same values as Corsano::FileId / Corsano::VitalType */

/* MARK: - Callbacks (user = opaque pointer given at registration) */

/**
 * @name Callback types
 * Called on the BLE backend thread: keep them short and do not call back into
 * the same handle. The pointed-to struct is only valid during the call. @p user
 * is the pointer given at registration.
 * @{
 */
/** @brief Realtime ping callback. @param ping Ping data. @param user User pointer. */
typedef void (*corsano_realtime_cb)(const corsano_realtime_t *ping,
                                    void *user);
/** @brief Live HRM callback. @param hrm Data. @param user User pointer. */
typedef void (*corsano_live_hrm_cb)(const corsano_live_hrm_t *hrm, void *user);
/** @brief Regular ping callback. @param ping Ping data. @param user User pointer. */
typedef void (*corsano_regular_ping_cb)(const corsano_regular_ping_t *ping,
                                        void *user);
/** @brief Battery/charging event callback. @param battery State. @param user User pointer. */
typedef void (*corsano_battery_cb)(const corsano_battery_t *battery,
                                   void *user);
/** @brief Disconnect callback. @param user User pointer. */
typedef void (*corsano_disconnect_cb)(void *user);
/** @} */

/* MARK: - Discovery */

/**
 * @brief Scan for bracelets; see Corsano::Bracelet::scan().
 * @param duration_ms Scan duration in ms.
 * @param devices Caller-owned array receiving up to @p capacity devices.
 * @param capacity Number of slots in @p devices.
 * @param[out] count Number of devices written.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_scan(int duration_ms, corsano_device_t *devices, size_t capacity,
                 size_t *count);

/* MARK: - Lifecycle */

/** @brief Opaque bracelet handle (wraps Corsano::Bracelet). */
typedef struct corsano corsano_t;

/**
 * @brief Create a disconnected handle; see Corsano::Bracelet::Bracelet().
 * @param address MAC on Linux, CoreBluetooth UUID on macOS.
 * @return New handle, or NULL on failure (see corsano_last_error()). Release
 *         with corsano_free().
 */
corsano_t *corsano_init(const char *address); /* NULL on failure */
/**
 * @brief Disconnect and destroy the handle.
 * @param obj Handle; NULL is ok.
 */
void corsano_free(corsano_t *obj);            /* disconnects; NULL is ok */

/* MARK: - Connection */

/**
 * @brief Connect; see Corsano::Bracelet::connect().
 * @param obj Handle.
 * @param scan_timeout_ms Maximum scan time in ms.
 * @return CORSANO_OK, CORSANO_ERR_TIMEOUT if the bracelet is not seen, or another CORSANO_ERR_* code.
 */
int corsano_connect(corsano_t *obj, int scan_timeout_ms);
/**
 * @brief Disconnect; see Corsano::Bracelet::disconnect().
 * @param obj Handle.
 */
void corsano_disconnect(corsano_t *obj);
/**
 * @brief Whether connected; see Corsano::Bracelet::is_connected().
 * @param obj Handle.
 * @return True if connected.
 */
bool corsano_is_connected(const corsano_t *obj);

/* MARK: - Raw commands */

/**
 * @brief Send a raw command; see Corsano::Bracelet::command().
 *
 * The response payload (id and CRC stripped) is copied into @p response (up
 * to @p capacity bytes); @p size receives the number of bytes.
 * @param obj Handle.
 * @param id Command id.
 * @param payload Command payload, may be NULL if @p payload_size is 0.
 * @param payload_size Payload length in bytes.
 * @param response Caller-owned buffer for the response payload.
 * @param capacity Size of @p response in bytes.
 * @param[out] size Bytes written to @p response.
 * @param timeout_ms Response timeout in ms.
 * @return CORSANO_OK, CORSANO_ERR_TIMEOUT, CORSANO_ERR_PAIRING or CORSANO_ERR_PROTOCOL, or another CORSANO_ERR_* code.
 */
int corsano_command(corsano_t *obj, uint8_t id, const uint8_t *payload,
                    size_t payload_size, uint8_t *response, size_t capacity,
                    size_t *size, int timeout_ms);

/* MARK: - Device info */

/**
 * @brief Read the battery state; see Corsano::Bracelet::battery().
 * @param obj Handle.
 * @param[out] out Receives the state.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_battery(corsano_t *obj, corsano_battery_t *out);
/**
 * @brief Read the firmware version; see Corsano::Bracelet::firmware_version().
 * @param obj Handle.
 * @param[out] out Receives the version.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_firmware_version(corsano_t *obj, corsano_firmware_t *out);
/**
 * @brief Read the serial number; see Corsano::Bracelet::serial_number().
 *
 * The result is NUL-terminated, truncated to capacity.
 * @param obj Handle.
 * @param out Caller-owned buffer.
 * @param capacity Size of @p out in bytes.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_serial_number(corsano_t *obj, char *out, size_t capacity);
/**
 * @brief Set the bracelet clock to the host clock; see Corsano::Bracelet::set_time().
 * @param obj Handle.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_set_time(corsano_t *obj);

/* MARK: - Plan (values as Corsano::Plan / Corsano::Ppg2Rate) */

/**
 * @brief Read the measurement plan; see Corsano::Bracelet::plan().
 * @param obj Handle.
 * @param[out] plan Receives a Corsano::Plan value.
 * @param[out] ppg2_rate Receives a Corsano::Ppg2Rate value.
 * @param[out] ppg1_rate Receives the PPG1 rate.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_plan(corsano_t *obj, uint8_t *plan, uint8_t *ppg2_rate,
                 uint8_t *ppg1_rate);
/**
 * @brief Set the measurement plan; see Corsano::Bracelet::set_plan().
 * @param obj Handle.
 * @param plan A Corsano::Plan value.
 * @param ppg2_rate A Corsano::Ppg2Rate value.
 * @param ppg1_rate PPG1 rate (the SDK uses 60).
 * @return CORSANO_OK, CORSANO_ERR_PROTOCOL if refused, or another CORSANO_ERR_* code.
 */
int corsano_set_plan(corsano_t *obj, uint8_t plan, uint8_t ppg2_rate,
                     uint8_t ppg1_rate);

/* MARK: - Command catalog (see Corsano::command_catalog()) */

/** @brief One known command; mirrors Corsano::CommandInfo. */
typedef struct {
  uint8_t id;              /**< Wire id */
  const char *name;        /**< Kebab-case name, e.g. "get-plan"; static */
  const char *description; /**< One line; static */
  bool is_set;             /**< Reply is a status byte (100 = OK) */
} corsano_command_info_t;

/**
 * @brief All known commands, sorted by id.
 * @param count Receives the number of entries (may be NULL).
 * @return Static table; never freed by the caller.
 * @see Corsano::command_catalog()
 */
const corsano_command_info_t *corsano_command_catalog(size_t *count);
/**
 * @brief Look up a command by id.
 * @param id Wire id.
 * @return Static entry, or NULL if unknown.
 */
const corsano_command_info_t *corsano_find_command_by_id(uint8_t id);
/**
 * @brief Look up a command by name (case-insensitive; `_` matches `-`).
 * @param name Command name; NULL returns NULL.
 * @return Static entry, or NULL if unknown.
 */
const corsano_command_info_t *corsano_find_command_by_name(const char *name);
/**
 * @brief Decode a command reply into readable `key=value` pairs.
 * @param id Command id the reply belongs to.
 * @param payload Reply payload (id and CRC stripped); may be NULL if size 0.
 * @param size Payload size in bytes.
 * @param out Buffer for the NUL-terminated text (truncated to capacity); may
 *        be NULL to query the length.
 * @param capacity Size of @p out in bytes.
 * @return Length of the full description, excluding the NUL; 0 for unknown
 *         ids or payloads that do not decode.
 * @see Corsano::describe_reply()
 */
size_t corsano_describe_reply(uint8_t id, const uint8_t *payload, size_t size,
                              char *out, size_t capacity);

/* MARK: - Authentication (FW >= 8.41) */

/**
 * @brief Set the host id used to authenticate on connect (FW >= 8.41).
 * @param obj Bracelet handle; call before corsano_connect().
 * @param host_id Any stable per-host string; NULL or "" = this host's name.
 * @return CORSANO_OK, or CORSANO_ERR_ARGUMENT for a NULL handle.
 * @see Corsano::Bracelet::set_host_id()
 */
int corsano_set_host_id(corsano_t *obj, const char *host_id);
/**
 * @brief Whether the connection is authenticated (true on FW < 8.41).
 * @param obj Bracelet handle.
 * @param out Receives the result.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 * @see Corsano::Bracelet::authenticated()
 */
int corsano_authenticated(corsano_t *obj, bool *out);

/* MARK: - User profile and calibration (values as Corsano::Gender, Wrist,
 * SkinType; see protocol.hpp) */

/** @brief User profile; mirrors Corsano::UserProfile. */
typedef struct {
  int birth_year, /**< Birth year */birth_month, /**< Birth month */birth_day; /**< Birth day */
  uint8_t gender, /**< Corsano::Gender value */wrist, /**< Corsano::Wrist value */skin; /**< Corsano::SkinType value */
  uint8_t height_cm, /**< Height in cm */weight_kg; /**< Weight in kg; 0 = leave unchanged when setting */
} corsano_user_profile_t;

/** @brief Wearing Optimization progress; mirrors Corsano::WearingOptimizationStatus. */
typedef struct {
  bool finished; /**< The run has ended. */
  uint8_t progress_percent, /**< Progress in % */movement_level, /**< Movement level */quality; /**< Quality 0..100; >= 50 passes */
  bool success; /**< finished and quality >= 50. */
} corsano_wearing_optimization_t;

/**
 * @brief spo2_quality values beyond the 0..4 scale.
 * @see Corsano::Spo2LowConfidence, docs/HR_SPO2.md
 */
enum {
  CORSANO_SPO2_LOW_CONFIDENCE = 103, /**< Fit, movement: re-seat. */
  CORSANO_SPO2_MOTION = 104, /**< Keep still. */
  CORSANO_SPO2_VENOUS_POOLING = 105, /**< Data discarded. */
  CORSANO_SPO2_SKIN_CONDITION = 106, /**< Other arm, recalibrate. */
};

/**
 * @brief Read the user profile; see Corsano::Bracelet::user_profile().
 * @param obj Handle.
 * @param[out] out Receives the profile.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_user_profile(corsano_t *obj, corsano_user_profile_t *out);
/**
 * @brief Set the user profile; see Corsano::Bracelet::set_user_profile().
 * @param obj Handle.
 * @param profile Profile to set.
 * @return CORSANO_OK, CORSANO_ERR_ARGUMENT for an invalid birth date, gender, wrist or skin type, or another CORSANO_ERR_* code.
 */
int corsano_set_user_profile(corsano_t *obj,
                             const corsano_user_profile_t *profile);
/**
 * @brief Start Wearing Optimization; see Corsano::Bracelet::start_wearing_optimization().
 * @param obj Handle.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_start_wearing_optimization(corsano_t *obj);
/**
 * @brief Read the Wearing Optimization status; see Corsano::Bracelet::wearing_optimization_status().
 * @param obj Handle.
 * @param[out] out Receives the status.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_wearing_optimization_status(corsano_t *obj,
                                        corsano_wearing_optimization_t *out);
/**
 * @brief Stop Wearing Optimization; see Corsano::Bracelet::stop_wearing_optimization().
 * @param obj Handle.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_stop_wearing_optimization(corsano_t *obj);

/* MARK: - Live vitals */

/**
 * @brief Enable or disable RealtimePing; see Corsano::Bracelet::set_realtime_enabled().
 * @param obj Handle.
 * @param enabled True to enable.
 * @return CORSANO_OK, CORSANO_ERR_PROTOCOL if refused, or another CORSANO_ERR_* code.
 */
int corsano_set_realtime_enabled(corsano_t *obj, bool enabled);
/**
 * @brief Read the realtime-enable flag; see Corsano::Bracelet::realtime_enabled().
 * @param obj Handle.
 * @param[out] out Receives the flag.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_realtime_enabled(corsano_t *obj, bool *out);
/**
 * @brief Set the RealtimePing period; see Corsano::Bracelet::set_realtime_period().
 * @param obj Handle.
 * @param seconds Period, 1..255 s.
 * @return CORSANO_OK, CORSANO_ERR_ARGUMENT if out of range, or another CORSANO_ERR_* code.
 */
int corsano_set_realtime_period(corsano_t *obj, int seconds);
/**
 * @brief Read the RealtimePing period; see Corsano::Bracelet::realtime_period().
 * @param obj Handle.
 * @param[out] seconds Receives the period in seconds.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_realtime_period(corsano_t *obj, int *seconds);
/**
 * @brief Set a vital's measurement interval; see Corsano::Bracelet::set_vital_sampling_rate().
 * @param obj Handle.
 * @param vital A Corsano::VitalType value, e.g. CORSANO_VITAL_SPO2.
 * @param seconds Interval in seconds (0 = off, up to 65535).
 * @return CORSANO_OK, CORSANO_ERR_ARGUMENT if out of range, or another CORSANO_ERR_* code.
 */
int corsano_set_vital_sampling_rate(corsano_t *obj, uint8_t vital,
                                    int seconds);
/**
 * @brief Read a vital's measurement interval; see Corsano::Bracelet::vital_sampling_rate().
 * @param obj Handle.
 * @param vital A Corsano::VitalType value, e.g. CORSANO_VITAL_PULSE_RATE.
 * @param[out] seconds Receives the interval in seconds (0 = off).
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_vital_sampling_rate(corsano_t *obj, uint8_t vital, int *seconds);
/**
 * @brief Enable or disable workout mode; see Corsano::Bracelet::set_workout_mode().
 * @param obj Handle.
 * @param enabled True to enable.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_set_workout_mode(corsano_t *obj, bool enabled);

/* MARK: - Files (download) */

/**
 * @brief Bytes stored for a file; see Corsano::Bracelet::file_size().
 * @param obj Handle.
 * @param file A Corsano::FileId value.
 * @param[out] size Receives the size in bytes.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_file_size(corsano_t *obj, uint8_t file, uint32_t *size);
/**
 * @brief Download a file; see Corsano::Bracelet::download_file().
 *
 * Downloads at most @p capacity bytes into @p data; @p size receives the
 * bytes received. Does not erase.
 * @param obj Handle.
 * @param file A Corsano::FileId value.
 * @param data Caller-owned buffer.
 * @param capacity Size of @p data in bytes.
 * @param[out] size Bytes received.
 * @return CORSANO_OK, CORSANO_ERR_TIMEOUT if the download stalls, or another CORSANO_ERR_* code.
 */
int corsano_download_file(corsano_t *obj, uint8_t file, uint8_t *data,
                          size_t capacity, size_t *size);
/**
 * @brief Free the first bytes of a file; see Corsano::Bracelet::erase_file().
 * @param obj Handle.
 * @param file A Corsano::FileId value.
 * @param bytes Bytes to free (use the `consumed` value of a decoder).
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_erase_file(corsano_t *obj, uint8_t file, uint32_t bytes);

/* MARK: - File decoders
 * Pattern: corsano_decode_<name>(data, size, rows, capacity, &count,
 * &consumed), pure (no handle), one flat row per record or sample. If
 * capacity is too small: fills what fits, returns CORSANO_ERR_ARGUMENT,
 * *consumed still covers the whole input. */

/**
 * @brief Decode Activity file data; see Corsano::decode_activity().
 *
 * `records` needs size/38 slots. All buffers are caller-owned.
 * @param data Raw file bytes (see corsano_download_file()).
 * @param size Length of @p data.
 * @param records Output rows.
 * @param capacity Number of slots in @p records.
 * @param[out] count Rows written.
 * @param[out] consumed Input bytes consumed; safe to pass to corsano_erase_file().
 * @return CORSANO_OK, or CORSANO_ERR_ARGUMENT if @p capacity is too small.
 */
int corsano_decode_activity(const uint8_t *data, size_t size,
                            corsano_activity_t *records, size_t capacity,
                            size_t *count, size_t *consumed);

/** @brief One Stress record; mirrors Corsano::StressRecord. */
typedef struct {
  uint32_t timestamp_s; /**< Record time, epoch seconds. */
  uint8_t cognitive_zone, /**< Cognitive zone */cognitive_zone_quality; /**< Cognitive zone quality */
  uint8_t predicted_cognitive_zone; /**< Predicted cognitive zone. */
  uint16_t predicted_transition_time; /**< Predicted time of the zone transition. */
  uint8_t cognitive_zone_hour; /**< Cognitive zone hour. */
  uint32_t cortisol_contribution; /**< Cortisol contribution. */
  uint16_t skin_conductance_stress; /**< Stress level from skin conductance. */
  uint8_t skin_conductance_stress_quality; /**< Quality of skin_conductance_stress. */
} corsano_stress_t;

/**
 * @brief Decode Stress file data; see Corsano::decode_stress().
 *
 * `records` needs size/18 slots. All buffers are caller-owned.
 * @param data Raw file bytes.
 * @param size Length of @p data.
 * @param records Output rows.
 * @param capacity Number of slots in @p records.
 * @param[out] count Rows written.
 * @param[out] consumed Input bytes consumed.
 * @return CORSANO_OK, or CORSANO_ERR_ARGUMENT if @p capacity is too small.
 */
int corsano_decode_stress(const uint8_t *data, size_t size,
                          corsano_stress_t *records, size_t capacity,
                          size_t *count, size_t *consumed);

/** @brief One row per PPG sample (25 Hz); flattened Corsano::PpgChunk / Corsano::PpgSample. */
typedef struct {
  uint64_t timestamp_ms;   /**< This sample. */
  uint32_t chunk_index; /**< Chunk counter. */
  uint16_t ppg; /**< Raw optical value. */
  uint16_t acc; /**< Motion magnitude. */
  int16_t acc_x, /**< Acceleration x */acc_y, /**< Acceleration y */acc_z; /**< Acceleration z (0 for protocol version 2) */
  uint8_t heart_rate, /**< Heart rate in bpm */heart_rate_quality; /**< On-device estimate for this second (quality) */
  uint8_t protocol_version; /**< PPG protocol version (2 or 3). */
} corsano_ppg_sample_t;

/**
 * @brief Decode PPG file data; see Corsano::decode_ppg().
 *
 * `samples` needs size/3 slots at most. All buffers are caller-owned.
 * @param data Raw file bytes.
 * @param size Length of @p data.
 * @param samples Output rows.
 * @param capacity Number of slots in @p samples.
 * @param[out] count Rows written.
 * @param[out] consumed Input bytes consumed.
 * @return CORSANO_OK, or CORSANO_ERR_ARGUMENT if @p capacity is too small.
 */
int corsano_decode_ppg(const uint8_t *data, size_t size,
                       corsano_ppg_sample_t *samples, size_t capacity,
                       size_t *count, size_t *consumed);

/* MARK: - Callbacks registration (NULL clears) */

/**
 * @name Callback registration
 * Replace any previous callback; pass a NULL @p cb to clear. @p user is
 * passed back unchanged to the callback, which runs on the BLE backend
 * thread. See Corsano::Bracelet::set_notification_callback().
 * @{
 */
/** @brief Register the RealtimePing callback. @param obj Handle. @param cb Callback or NULL. @param user User pointer. */
void corsano_set_realtime_callback(corsano_t *obj, corsano_realtime_cb cb,
                                   void *user);
/** @brief Register the LiveHrm callback. @param obj Handle. @param cb Callback or NULL. @param user User pointer. */
void corsano_set_live_hrm_callback(corsano_t *obj, corsano_live_hrm_cb cb,
                                   void *user);
/** @brief Register the RegularPing callback. @param obj Handle. @param cb Callback or NULL. @param user User pointer. */
void corsano_set_regular_ping_callback(corsano_t *obj,
                                       corsano_regular_ping_cb cb, void *user);
/** @brief Register the battery/charging callback. @param obj Handle. @param cb Callback or NULL. @param user User pointer. */
void corsano_set_battery_callback(corsano_t *obj, corsano_battery_cb cb,
                                  void *user);
/** @brief Register the disconnect callback. @param obj Handle. @param cb Callback or NULL. @param user User pointer. */
void corsano_set_disconnect_callback(corsano_t *obj, corsano_disconnect_cb cb,
                                     void *user);
/** @} */

/* MARK: - Monitor (see monitor.hpp)
 * Background thread: keeps the bracelet connected, polls/erases files,
 * queues tidy samples. Drain with corsano_monitor_poll (non-blocking) or
 * corsano_monitor_wait (blocking). */

/**
 * @name Monitor
 * C wrapper of Corsano::Monitor (see monitor.hpp): a background thread keeps
 * the bracelet connected, polls and erases files and queues tidy samples.
 * Drain with corsano_monitor_poll() (non-blocking) or corsano_monitor_wait()
 * (blocking).
 * @{
 */

/** @brief One queued measurement; mirrors Corsano::Sample. */
typedef struct {
  double time;       /**< Unix epoch seconds. */
  const char *field; /**< static string, e.g. "activity.spo2"; never free it */
  double value; /**< Measured value. */
  uint32_t record;   /**< samples from the same record share it */
} corsano_sample_t;

/** @brief Monitor options; mirrors Corsano::MonitorConfig. Initialise with corsano_monitor_config_default(). */
typedef struct {
  const uint8_t *files; /**< FileId values; NULL = default activity,stress,bioz */
  size_t n_files; /**< Number of entries in @p files. */
  bool live_hr;  /**< workout-mode HR every 2 s; stops Activity logging */
  bool erase;    /**< erase files after decoding */
  bool set_time; /**< sync bracelet clock on (re)connect */
  int plan;      /**< -1 = leave as is, else Corsano::Plan applied on connect */
  uint8_t ppg2_rate, /**< Corsano::Ppg2Rate value */ppg1_rate; /**< PPG1 rate; used when plan >= 0 */
  const corsano_user_profile_t *profile; /**< NULL = leave as is; copied at init */
  const char *host_id; /**< FW >= 8.41 authentication; NULL/"" = host name; copied at init */
  int interval_ms; /**< Minimum time per poll pass in ms; 0 = as soon as possible. */
  size_t max_queue; /**< Queue limit; when full the oldest samples are dropped (reported as status.dropped). */
} corsano_monitor_config_t;

/** @brief Opaque monitor handle (wraps Corsano::Monitor). */
typedef struct corsano_monitor corsano_monitor_t;

/**
 * @brief Fill a config with the defaults (files = NULL means the default set).
 * @param[out] cfg Config to initialise.
 */
void corsano_monitor_config_default(corsano_monitor_config_t *cfg);
/**
 * @brief Create a stopped monitor; see Corsano::Monitor::Monitor().
 * @param device Address or advertised name.
 * @param cfg Options; NULL = defaults. Copied, including the profile.
 * @return New handle, or NULL on failure (see corsano_last_error()). Release
 *         with corsano_monitor_free().
 */
corsano_monitor_t *corsano_monitor_init(const char *device,
                                        const corsano_monitor_config_t *cfg);
/**
 * @brief Stop and destroy the monitor.
 * @param mon Handle; NULL is ok.
 */
void corsano_monitor_free(corsano_monitor_t *mon); /* stops; NULL is ok */
/**
 * @brief Start the background thread; see Corsano::Monitor::start().
 * @param mon Handle.
 * @return CORSANO_OK or a CORSANO_ERR_* code (e.g. already running).
 */
int corsano_monitor_start(corsano_monitor_t *mon);
/**
 * @brief Stop the background thread; see Corsano::Monitor::stop().
 * @param mon Handle.
 */
void corsano_monitor_stop(corsano_monitor_t *mon);
/**
 * @brief Whether the monitor thread is running; see Corsano::Monitor::running().
 * @param mon Handle.
 * @return True while running.
 */
bool corsano_monitor_running(const corsano_monitor_t *mon);
/**
 * @brief Set the bracelet clock to the host clock; see Corsano::Monitor::sync_clock().
 * @param mon Handle.
 * @return CORSANO_OK, CORSANO_ERR_BLE if not connected, or another CORSANO_ERR_* code.
 */
int corsano_monitor_sync_clock(corsano_monitor_t *mon);
/**
 * @brief Last error seen by the monitor thread; see Corsano::Monitor::last_error().
 *
 * The result is NUL-terminated, truncated to capacity.
 * @param mon Handle.
 * @param out Caller-owned buffer.
 * @param capacity Size of @p out in bytes.
 * @return CORSANO_OK or CORSANO_ERR_ARGUMENT.
 */
int corsano_monitor_last_error(const corsano_monitor_t *mon, char *out,
                               size_t capacity);
/**
 * @brief Non-blocking drain; see Corsano::Monitor::poll().
 *
 * Writes up to @p capacity samples; the rest stay queued. The `field` strings
 * are static and must not be freed.
 * @param mon Handle.
 * @param samples Caller-owned output array.
 * @param capacity Number of slots in @p samples.
 * @param[out] count Samples written.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_monitor_poll(corsano_monitor_t *mon, corsano_sample_t *samples,
                         size_t capacity, size_t *count);
/**
 * @brief Blocking drain; see Corsano::Monitor::wait().
 *
 * As corsano_monitor_poll(), but waits up to @p timeout_ms for at least one
 * sample (or until the monitor is stopped).
 * @param mon Handle.
 * @param samples Caller-owned output array.
 * @param capacity Number of slots in @p samples.
 * @param[out] count Samples written.
 * @param timeout_ms Maximum wait in ms.
 * @return CORSANO_OK or a CORSANO_ERR_* code.
 */
int corsano_monitor_wait(corsano_monitor_t *mon, corsano_sample_t *samples,
                         size_t capacity, size_t *count, int timeout_ms);
/** @} */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CORSANO_H */
