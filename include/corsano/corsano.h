/* C API over Corsano::Bracelet, shaped for ctypes: opaque handle, plain
 * structs, caller-owned buffers (no library-side allocation to free), int
 * status codes. Callbacks run on the BLE backend thread. */
#ifndef CORSANO_H
#define CORSANO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MARK: - Status */

enum {
  CORSANO_OK = 0,
  CORSANO_ERR_TIMEOUT = -1,
  CORSANO_ERR_PAIRING = -2,  /* bracelet not bonded with this host */
  CORSANO_ERR_PROTOCOL = -3, /* bad CRC, status != OK, short payload */
  CORSANO_ERR_ARGUMENT = -4,
  CORSANO_ERR_BLE = -5, /* any other failure */
};

/* Message of the last failed call on the calling thread ("" if none) */
const char *corsano_last_error(void);

/* MARK: - Data types (mirror protocol.hpp) */

typedef struct {
  char name[64];
  char address[64]; /* MAC on Linux, CoreBluetooth UUID on macOS */
  int16_t rssi;
} corsano_device_t;

typedef struct {
  uint8_t percent;
  uint16_t millivolts;
  bool charging;
} corsano_battery_t;

typedef struct {
  int major;
  int minor;
} corsano_firmware_t;

typedef struct {
  uint32_t timestamp_s;
  uint8_t heart_rate, heart_rate_quality;
  float respiration_rate;
  uint8_t respiration_rate_quality;
  uint8_t spo2, spo2_quality;
  float core_body_temp_c;
  uint8_t movement_level;
  uint8_t battery_percent;
} corsano_realtime_t;

typedef struct {
  uint8_t heart_rate, heart_rate_quality;
  uint16_t steps;
  uint16_t energy;
  uint8_t max_heart_rate, speed;
  uint16_t duration;
  uint8_t battery_percent;
  uint8_t average_heart_rate, average_speed;
} corsano_live_hrm_t;

typedef struct {
  uint8_t battery_percent;
  uint16_t ppg_size, activity_size, hrv_size, workout_size, sleep_size,
      log_size;
} corsano_regular_ping_t;

typedef struct {
  uint32_t timestamp_s;
  uint8_t heart_rate, heart_rate_quality;
  uint16_t steps;
  uint8_t activity_type, activity_count;
  uint8_t spo2, spo2_quality;
  uint16_t energy;
  float respiration_rate;
  uint8_t respiration_rate_quality;
  uint8_t battery_percent;
  bool charging;
  uint8_t nibp_systolic, nibp_diastolic;
  float temp1_c, temp2_c;
  uint8_t wearing;
} corsano_activity_t;

/* File and vital ids: same values as Corsano::FileId / Corsano::VitalType */
enum { CORSANO_FILE_ACTIVITY = 2 };
enum { CORSANO_VITAL_PULSE_RATE = 2, CORSANO_VITAL_SPO2 = 4 };

/* MARK: - Callbacks (user = opaque pointer given at registration) */

typedef void (*corsano_realtime_cb)(const corsano_realtime_t *ping,
                                    void *user);
typedef void (*corsano_live_hrm_cb)(const corsano_live_hrm_t *hrm, void *user);
typedef void (*corsano_regular_ping_cb)(const corsano_regular_ping_t *ping,
                                        void *user);
typedef void (*corsano_battery_cb)(const corsano_battery_t *battery,
                                   void *user);
typedef void (*corsano_disconnect_cb)(void *user);

/* MARK: - Discovery */

/* Fills up to `capacity` devices; *count = number written */
int corsano_scan(int duration_ms, corsano_device_t *devices, size_t capacity,
                 size_t *count);

/* MARK: - Lifecycle */

typedef struct corsano corsano_t;

corsano_t *corsano_init(const char *address); /* NULL on failure */
void corsano_free(corsano_t *obj);            /* disconnects; NULL is ok */

/* MARK: - Connection */

int corsano_connect(corsano_t *obj, int scan_timeout_ms);
void corsano_disconnect(corsano_t *obj);
bool corsano_is_connected(const corsano_t *obj);

/* MARK: - Raw commands */

/* Response payload copied into `response` (up to capacity), *size = bytes */
int corsano_command(corsano_t *obj, uint8_t id, const uint8_t *payload,
                    size_t payload_size, uint8_t *response, size_t capacity,
                    size_t *size, int timeout_ms);

/* MARK: - Device info */

int corsano_battery(corsano_t *obj, corsano_battery_t *out);
int corsano_firmware_version(corsano_t *obj, corsano_firmware_t *out);
/* NUL-terminated, truncated to capacity */
int corsano_serial_number(corsano_t *obj, char *out, size_t capacity);
int corsano_set_time(corsano_t *obj);

/* MARK: - Plan (values as Corsano::Plan / Corsano::Ppg2Rate) */

int corsano_plan(corsano_t *obj, uint8_t *plan, uint8_t *ppg2_rate,
                 uint8_t *ppg1_rate);
int corsano_set_plan(corsano_t *obj, uint8_t plan, uint8_t ppg2_rate,
                     uint8_t ppg1_rate);

/* MARK: - User profile and calibration (values as Corsano::Gender, Wrist,
 * SkinType; see protocol.hpp) */

typedef struct {
  int birth_year, birth_month, birth_day;
  uint8_t gender, wrist, skin;
  uint8_t height_cm, weight_kg; /* 0 = leave unchanged when setting */
} corsano_user_profile_t;

typedef struct {
  bool finished;
  uint8_t progress_percent, movement_level, quality; /* quality >= 50 passes */
  bool success;
} corsano_wearing_optimization_t;

/* spo2_quality values beyond the 0..4 scale */
enum {
  CORSANO_SPO2_LOW_CONFIDENCE = 103,
  CORSANO_SPO2_MOTION = 104,
  CORSANO_SPO2_VENOUS_POOLING = 105,
  CORSANO_SPO2_SKIN_CONDITION = 106,
};

int corsano_user_profile(corsano_t *obj, corsano_user_profile_t *out);
int corsano_set_user_profile(corsano_t *obj,
                             const corsano_user_profile_t *profile);
int corsano_start_wearing_optimization(corsano_t *obj);
int corsano_wearing_optimization_status(corsano_t *obj,
                                        corsano_wearing_optimization_t *out);
int corsano_stop_wearing_optimization(corsano_t *obj);

/* MARK: - Live vitals */

int corsano_set_realtime_enabled(corsano_t *obj, bool enabled);
int corsano_realtime_enabled(corsano_t *obj, bool *out);
int corsano_set_realtime_period(corsano_t *obj, int seconds);
int corsano_realtime_period(corsano_t *obj, int *seconds);
int corsano_set_vital_sampling_rate(corsano_t *obj, uint8_t vital,
                                    int seconds);
int corsano_vital_sampling_rate(corsano_t *obj, uint8_t vital, int *seconds);
int corsano_set_workout_mode(corsano_t *obj, bool enabled);

/* MARK: - Files (download) */

int corsano_file_size(corsano_t *obj, uint8_t file, uint32_t *size);
/* Downloads at most `capacity` bytes into `data`; *size = bytes received */
int corsano_download_file(corsano_t *obj, uint8_t file, uint8_t *data,
                          size_t capacity, size_t *size);
int corsano_erase_file(corsano_t *obj, uint8_t file, uint32_t bytes);

/* MARK: - File decoders
 * Pattern: corsano_decode_<name>(data, size, rows, capacity, &count,
 * &consumed), pure (no handle), one flat row per record or sample. If
 * capacity is too small: fills what fits, returns CORSANO_ERR_ARGUMENT,
 * *consumed still covers the whole input. */

/* `records` needs size/38 slots. */
int corsano_decode_activity(const uint8_t *data, size_t size,
                            corsano_activity_t *records, size_t capacity,
                            size_t *count, size_t *consumed);

typedef struct {
  uint32_t timestamp_s;
  uint8_t cognitive_zone, cognitive_zone_quality;
  uint8_t predicted_cognitive_zone;
  uint16_t predicted_transition_time;
  uint8_t cognitive_zone_hour;
  uint32_t cortisol_contribution;
  uint16_t skin_conductance_stress;
  uint8_t skin_conductance_stress_quality;
} corsano_stress_t;

/* `records` needs size/18 slots. */
int corsano_decode_stress(const uint8_t *data, size_t size,
                          corsano_stress_t *records, size_t capacity,
                          size_t *count, size_t *consumed);

/* One row per PPG sample (25 Hz) */
typedef struct {
  uint64_t timestamp_ms;   /* this sample */
  uint32_t chunk_index;
  uint16_t ppg;
  uint16_t acc;
  int16_t acc_x, acc_y, acc_z; /* 0 for protocol version 2 */
  uint8_t heart_rate, heart_rate_quality; /* on-device estimate, this second */
  uint8_t protocol_version;
} corsano_ppg_sample_t;

/* `samples` needs size/3 slots at most. */
int corsano_decode_ppg(const uint8_t *data, size_t size,
                       corsano_ppg_sample_t *samples, size_t capacity,
                       size_t *count, size_t *consumed);

/* MARK: - Callbacks registration (NULL clears) */

void corsano_set_realtime_callback(corsano_t *obj, corsano_realtime_cb cb,
                                   void *user);
void corsano_set_live_hrm_callback(corsano_t *obj, corsano_live_hrm_cb cb,
                                   void *user);
void corsano_set_regular_ping_callback(corsano_t *obj,
                                       corsano_regular_ping_cb cb, void *user);
void corsano_set_battery_callback(corsano_t *obj, corsano_battery_cb cb,
                                  void *user);
void corsano_set_disconnect_callback(corsano_t *obj, corsano_disconnect_cb cb,
                                     void *user);

/* MARK: - Monitor (see monitor.hpp)
 * Background thread: keeps the bracelet connected, polls/erases files,
 * queues tidy samples. Drain with corsano_monitor_poll (non-blocking) or
 * corsano_monitor_wait (blocking). */

typedef struct {
  double time;       /* Unix epoch seconds */
  const char *field; /* static string, e.g. "activity.spo2"; never free it */
  double value;
  uint32_t record;   /* samples from the same record share it */
} corsano_sample_t;

typedef struct {
  const uint8_t *files; /* FileId values; NULL = default activity,stress,bioz */
  size_t n_files;
  bool live_hr;  /* workout-mode HR every 2 s; stops Activity logging */
  bool erase;    /* erase files after decoding */
  bool set_time; /* sync bracelet clock on (re)connect */
  int plan;      /* -1 = leave as is, else Corsano::Plan applied on connect */
  uint8_t ppg2_rate, ppg1_rate; /* used when plan >= 0 */
  const corsano_user_profile_t *profile; /* NULL = leave as is; copied at init */
  int interval_ms;
  size_t max_queue;
} corsano_monitor_config_t;

typedef struct corsano_monitor corsano_monitor_t;

/* Fills *cfg with the defaults (files = NULL means the default set) */
void corsano_monitor_config_default(corsano_monitor_config_t *cfg);
/* device: address or advertised name; cfg NULL = defaults. NULL on failure */
corsano_monitor_t *corsano_monitor_init(const char *device,
                                        const corsano_monitor_config_t *cfg);
void corsano_monitor_free(corsano_monitor_t *mon); /* stops; NULL is ok */
int corsano_monitor_start(corsano_monitor_t *mon);
void corsano_monitor_stop(corsano_monitor_t *mon);
bool corsano_monitor_running(const corsano_monitor_t *mon);
/* Bracelet clock := host clock; CORSANO_ERR_BLE if not connected */
int corsano_monitor_sync_clock(corsano_monitor_t *mon);
/* NUL-terminated, truncated to capacity */
int corsano_monitor_last_error(const corsano_monitor_t *mon, char *out,
                               size_t capacity);
/* Up to capacity samples; the rest stay queued. *count = written */
int corsano_monitor_poll(corsano_monitor_t *mon, corsano_sample_t *samples,
                         size_t capacity, size_t *count);
/* As poll, but waits up to timeout_ms for at least one sample */
int corsano_monitor_wait(corsano_monitor_t *mon, corsano_sample_t *samples,
                         size_t capacity, size_t *count, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CORSANO_H */
