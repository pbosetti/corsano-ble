/* Pure C17 smoke test of the C API: no device needed. */
#include "corsano/corsano.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static void make_record(uint8_t *r, uint32_t ts, uint8_t hr, uint8_t spo2,
                        int8_t battery, int16_t temp1) {
  memset(r, 0, 38);
  r[0] = ts & 0xFF;
  r[1] = (ts >> 8) & 0xFF;
  r[2] = (ts >> 16) & 0xFF;
  r[3] = (ts >> 24) & 0xFF;
  r[4] = hr;
  r[10] = spo2;
  r[15] = (uint8_t)battery;
  r[31] = (uint8_t)(temp1 & 0xFF);
  r[32] = (uint8_t)((temp1 >> 8) & 0xFF);
  r[37] = crc8(r, 37);
}

/* Stress: ts, zone=2, predicted=3, transition=0x0102, hour=14,
 * cortisol=0x11223344, zone_q=5, sc=0xA1B2, sc_q=7 */
static void make_stress(uint8_t *r, uint32_t ts) {
  memset(r, 0, 18);
  for (int i = 0; i < 4; ++i)
    r[i] = (uint8_t)(ts >> (8 * i));
  r[4] = 2;
  r[5] = 3;
  r[6] = 0x02;
  r[7] = 0x01;
  r[8] = 14;
  r[9] = 0x44;
  r[10] = 0x33;
  r[11] = 0x22;
  r[12] = 0x11;
  r[13] = 5;
  r[14] = 0xB2;
  r[15] = 0xA1;
  r[16] = 7;
  r[17] = crc8(r, 17);
}

static uint32_t crc32(const uint8_t *d, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
  }
  return ~crc;
}

static void put_le(uint8_t *p, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i)
    p[i] = (uint8_t)(v >> (8 * i));
}

/* v2 chunk: 75 samples, ppg = 1000 + i, acc = 160 (expands to 240) */
static void make_ppg_chunk_v2(uint8_t *c, uint32_t index, uint32_t ts_s) {
  static const uint8_t marker[6] = {0x00, 0xFF, 0xAA, 0x4D, 0x4D, 0x54};
  memset(c, 0, 256);
  memcpy(c, marker, 6);
  c[6] = 2;
  put_le(c + 9, 75, 2);
  put_le(c + 11, index, 4);
  put_le(c + 15, ts_s, 4);
  put_le(c + 19, 250, 2);
  static const uint8_t hr[6] = {61, 1, 62, 2, 63, 3};
  memcpy(c + 21, hr, 6);
  for (int i = 0; i < 75; ++i) {
    put_le(c + 27 + 3 * i, 1000 + i, 2);
    c[29 + 3 * i] = 160;
  }
  put_le(c + 252, crc32(c, 252), 4);
}

int main(void) {
  assert(strcmp(corsano_last_error(), "") == 0);

  uint8_t buf[114];
  make_record(buf, 1000, 60, 97, 80, 3650);
  make_record(buf + 38, 1060, 72, 98, -50, -125);
  make_record(buf + 76, 1120, 90, 99, 40, 3700);

  corsano_activity_t rec[3];
  size_t count = 99, consumed = 99;
  int rc = corsano_decode_activity(buf, sizeof buf, rec, 3, &count, &consumed);
  assert(rc == CORSANO_OK);
  assert(count == 3 && consumed == 114);
  assert(rec[0].timestamp_s == 1000 && rec[0].heart_rate == 60);
  assert(rec[0].spo2 == 97 && rec[0].battery_percent == 80 && !rec[0].charging);
  assert(rec[0].temp1_c > 36.49f && rec[0].temp1_c < 36.51f);
  assert(rec[1].heart_rate == 72 && rec[1].charging);
  assert(rec[1].battery_percent == 50);
  assert(rec[1].temp1_c < -1.24f && rec[1].temp1_c > -1.26f);
  assert(rec[2].heart_rate == 90 && rec[2].spo2 == 99);

  rc = corsano_decode_activity(buf, sizeof buf, rec, 1, &count, &consumed);
  assert(rc == CORSANO_ERR_ARGUMENT);
  assert(count == 1 && consumed == 114);

  uint8_t sbuf[36];
  make_stress(sbuf, 3000);
  make_stress(sbuf + 18, 3001);
  corsano_stress_t st[2];
  rc = corsano_decode_stress(sbuf, sizeof sbuf, st, 2, &count, &consumed);
  assert(rc == CORSANO_OK);
  assert(count == 2 && consumed == 36);
  assert(st[0].timestamp_s == 3000 && st[1].timestamp_s == 3001);
  assert(st[0].cognitive_zone == 2 && st[0].predicted_cognitive_zone == 3);
  assert(st[0].predicted_transition_time == 0x0102);
  assert(st[0].cognitive_zone_hour == 14);
  assert(st[0].cortisol_contribution == 0x11223344u);
  assert(st[0].cognitive_zone_quality == 5);
  assert(st[0].skin_conductance_stress == 0xA1B2);
  assert(st[0].skin_conductance_stress_quality == 7);

  rc = corsano_decode_stress(sbuf, sizeof sbuf, st, 1, &count, &consumed);
  assert(rc == CORSANO_ERR_ARGUMENT);
  assert(count == 1 && consumed == 36);

  uint8_t chunk[256];
  make_ppg_chunk_v2(chunk, 7, 2000);
  corsano_ppg_sample_t ppg[75];
  rc = corsano_decode_ppg(chunk, sizeof chunk, ppg, 75, &count, &consumed);
  assert(rc == CORSANO_OK);
  assert(count == 75 && consumed == 256);
  assert(ppg[0].timestamp_ms == 2000250 &&
         ppg[74].timestamp_ms == 2000250 + 74 * 40);
  assert(ppg[0].chunk_index == 7 && ppg[0].protocol_version == 2);
  assert(ppg[0].ppg == 1000 && ppg[1].ppg == 1001);
  assert(ppg[0].acc == 240 && ppg[0].acc_x == 0);
  assert(ppg[0].heart_rate == 61 && ppg[0].heart_rate_quality == 1);
  assert(ppg[30].heart_rate == 62 && ppg[30].heart_rate_quality == 2);
  assert(ppg[74].heart_rate == 63);

  rc = corsano_decode_ppg(chunk, sizeof chunk, ppg, 10, &count, &consumed);
  assert(rc == CORSANO_ERR_ARGUMENT);
  assert(count == 10 && consumed == 256);

  assert(corsano_battery(NULL, NULL) == CORSANO_ERR_ARGUMENT);
  assert(corsano_user_profile(NULL, NULL) == CORSANO_ERR_ARGUMENT);
  assert(corsano_set_user_profile(NULL, NULL) == CORSANO_ERR_ARGUMENT);
  assert(corsano_start_wearing_optimization(NULL) == CORSANO_ERR_ARGUMENT);
  assert(corsano_wearing_optimization_status(NULL, NULL) ==
         CORSANO_ERR_ARGUMENT);
  assert(corsano_stop_wearing_optimization(NULL) == CORSANO_ERR_ARGUMENT);
  assert(corsano_set_host_id(NULL, "x") == CORSANO_ERR_ARGUMENT);
  assert(corsano_authenticated(NULL, NULL) == CORSANO_ERR_ARGUMENT);
  assert(CORSANO_SPO2_MOTION == 104);
  assert(strlen(corsano_last_error()) > 0);

  /* Monitor: no device needed until start */
  corsano_monitor_config_t mc;
  corsano_monitor_config_default(&mc);
  assert(mc.profile == NULL && mc.host_id == NULL);
  assert(mc.files == NULL && mc.n_files == 0 && !mc.live_hr && mc.erase &&
         mc.set_time && mc.plan == -1 && mc.ppg2_rate == 1 &&
         mc.ppg1_rate == 60 && mc.interval_ms == 0 && mc.max_queue == 100000);
  corsano_monitor_t *mon = corsano_monitor_init("fake-device", &mc);
  assert(mon != NULL && !corsano_monitor_running(mon));
  corsano_sample_t smp[4];
  count = 99;
  assert(corsano_monitor_poll(mon, smp, 4, &count) == CORSANO_OK && count == 0);
  count = 99;
  assert(corsano_monitor_wait(mon, smp, 4, &count, 50) == CORSANO_OK &&
         count == 0);
  assert(corsano_monitor_sync_clock(mon) ==
         CORSANO_ERR_BLE); /* not connected */
  assert(corsano_monitor_sync_clock(NULL) == CORSANO_ERR_ARGUMENT);
  corsano_monitor_free(mon);
  corsano_monitor_free(NULL);

  corsano_free(NULL);
  puts("ok");
  return 0;
}
