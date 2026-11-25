#include "opsdk.h"

#include "fake_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      failures++;                                                            \
    }                                                                        \
  } while (0)

static void hex(const char *s, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(s + 2 * i, "%2x", &v);
    out[i] = (uint8_t)v;
  }
}

static void test_vectors(void) {
  uint8_t want[64], got[64];
  hex("a8020000800000800000000200000010000000000000000000000000000000000000000000000000"
      "000000000000000000000000000000000000000072c569f5",
      want, 64);
  const uint8_t args[8] = {0, 2, 0, 0, 0, 0x10, 0, 0};
  opsdk_build_command(got, 0xA8, 0x02, 0x00800000, 0x8000, args, 8, 0);
  CHECK(memcmp(got, want, 64) == 0);

  uint8_t led_want[32], led_got[32];
  hex("b80000013238000000002a18010000003b0c000000017d05013032221e07dc05", led_want, 32);
  opsdk_led_t led = {{255, 128, 0}, {0, 64, 255}, OPSDK_EFFECT_SINE, 50,
                     OPSDK_EFFECT_TRIANGLE, 30, 7, 1500};
  opsdk_build_led(led_got, 0xB8, &led);
  CHECK(memcmp(led_got, led_want, 32) == 0);

  CHECK(opsdk_crc32(0, "123456789", 9) == 0xCBF43926u);
  uint8_t pattern[2048];
  for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)i;
  CHECK(opsdk_crc32(0, pattern, sizeof(pattern)) == 0x9F5EDD58u);
  CHECK(opsdk_crc32(opsdk_crc32(0, pattern, 1000), pattern + 1000, 1048) == 0x9F5EDD58u);
}

static void test_signature(void) {
  uint8_t raw[512] = {0};
  raw[0x02] = 0x31;
  raw[0x03] = 1;
  raw[0x04] = 1;
  raw[0x05] = 23;
  raw[0x06] = 17;
  raw[0x07] = 0x31;
  opsdk_cart_t c;
  opsdk_parse_signature(raw, &c);
  CHECK(c.platform == OPSDK_PLATFORM_GBA);
  CHECK(c.flashcart && !c.counterfeit);
  CHECK(c.rom_size == 8u << 20 && c.save_size == 128u << 10);
  CHECK(c.save_chip == 0x03);

  raw[0x02] = 0x20;
  raw[0x05] = 21;
  raw[0x0A] = (uint8_t)(20 << 2 | 1);
  raw[0x07] = 0x40;
  opsdk_parse_signature(raw, &c);
  CHECK(c.platform == OPSDK_PLATFORM_GB && !c.flashcart);
  CHECK(c.rom_size == 2u << 20);
  CHECK(c.save_chip == 0x40);
  raw[0x05] = 22;
  opsdk_parse_signature(raw, &c);
  CHECK(c.rom_size == 3u << 20);
  raw[0x16] = 0x10;
  opsdk_parse_signature(raw, &c);
  CHECK(c.rom_size == 3u << 20);
  raw[0x06] = 16;
  raw[0x07] = 0x24;
  opsdk_parse_signature(raw, &c);
  CHECK(c.save_size == 98304);
}

static void test_camera_defaults(void) {
  opsdk_camera_t s;
  opsdk_camera_defaults(&s);
  CHECK(s.exposure == 0x1000 && s.vref == 3 && s.zero_calibration && s.edge_ratio == 0x40);
  CHECK(s.matrix[0] == 0x82);
  bool in_range = true;
  for (size_t i = 0; i < sizeof(s.matrix); i++)
    in_range = in_range && s.matrix[i] >= 0x82 && s.matrix[i] < 0x82 + 3 * 0x12;
  CHECK(in_range);
}

static void test_rtc4513(void) {
  uint8_t raw[OPSDK_RTC_SIZE] = {2, 7, 3, 0, 0, 8, 1, 9, 1, 1, 1, 6, 2, 4, 3, 15, 4};
  opsdk_rtc4513_t c;
  CHECK(opsdk_rtc4513_decode(raw, &c) == OPSDK_OK);
  CHECK(c.year == 26 && c.month == 11 && c.day == 19 && c.weekday == 4);
  CHECK(c.hour == 18 && c.minute == 0 && c.second == 37);
  CHECK(!c.hour12 && !c.stopped && c.calendar && !c.lost_power && c.valid);

  uint8_t never_set[OPSDK_RTC_SIZE] = {2, 2, 2, 15, 7, 0, 0, 3, 0, 1, 12, 15, 14, 0, 1, 15, 6};
  CHECK(opsdk_rtc4513_decode(never_set, &c) == OPSDK_OK);
  CHECK(!c.valid && c.stopped && !c.calendar);

  uint8_t eleven_pm[OPSDK_RTC_SIZE] = {2, 5, 5, 9, 5, 1, 5, 9, 1, 1, 1, 6, 2, 4, 3, 15, 0};
  CHECK(opsdk_rtc4513_decode(eleven_pm, &c) == OPSDK_OK);
  CHECK(c.hour12 && c.hour == 23 && c.valid);

  uint8_t lost[OPSDK_RTC_SIZE] = {2, 0, 8, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 3, 15, 4};
  CHECK(opsdk_rtc4513_decode(lost, &c) == OPSDK_OK);
  CHECK(c.lost_power && c.second == 0);

  opsdk_rtc4513_t t = {.year = 26, .month = 11, .day = 19, .weekday = 4, .hour = 18, .second = 37};
  uint8_t want[16] = {7, 3, 0, 0, 8, 1, 9, 1, 1, 1, 6, 2, 4, 2, 15, 4};
  uint8_t out[OPSDK_RTC_SIZE];
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_OK);
  CHECK(memcmp(out, want, sizeof(want)) == 0 && out[16] == 0);

  t.hour12 = true;
  t.hour = 0;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_OK && out[4] == 2 && out[5] == 1 && out[15] == 0);
  t.hour = 12;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_OK && out[4] == 2 && out[5] == 5);
  t.hour = 13;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_OK && out[4] == 1 && out[5] == 4);
  t.hour12 = false;
  t.stopped = true;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_OK && out[15] == 6);
  t.stopped = false;

  for (int mode = 0; mode < 2; mode++) {
    for (uint8_t hour = 0; hour < 24; hour++) {
      t.hour12 = mode;
      t.hour = hour;
      uint8_t back[OPSDK_RTC_SIZE] = {2};
      CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_OK);
      memcpy(back + 1, out, 16);
      CHECK(opsdk_rtc4513_decode(back, &c) == OPSDK_OK);
      CHECK(c.hour == hour && c.hour12 == (bool)mode && c.valid);
    }
  }

  t.hour12 = false;
  t.hour = 24;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_E_ARGUMENT);
  t.hour = 18;
  t.month = 13;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_E_ARGUMENT);
  t.month = 11;
  t.day = 0;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_E_ARGUMENT);
  t.day = 19;
  t.year = 100;
  CHECK(opsdk_rtc4513_encode(&t, out) == OPSDK_E_ARGUMENT);
  t.year = 26;
  CHECK(opsdk_rtc4513_encode(NULL, out) == OPSDK_E_ARGUMENT);
  CHECK(opsdk_rtc4513_encode(&t, NULL) == OPSDK_E_ARGUMENT);

  uint8_t srtc[OPSDK_RTC_SIZE] = {1};
  CHECK(opsdk_rtc4513_decode(srtc, &c) == OPSDK_E_ARGUMENT);
  CHECK(opsdk_rtc4513_decode(NULL, &c) == OPSDK_E_ARGUMENT);
  CHECK(opsdk_rtc4513_decode(raw, NULL) == OPSDK_E_ARGUMENT);
}

static fake_device_t *spawn(opsdk_t **dev, const fake_options_t *o) {
  fake_device_t *fake = fake_create(o);
  CHECK(opsdk_open_transport(dev, fake_transport(), fake) == OPSDK_OK);
  opsdk_set_timeout(*dev, 2000);
  return fake;
}

static void finish(opsdk_t *dev, fake_device_t *fake) {
  opsdk_close(dev);
  fake_destroy(fake);
}

static void test_transactions(void) {
  fake_options_t o = {.sdk = true, .platform = 0x20};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);

  CHECK(opsdk_ping(dev, 0x12) == OPSDK_OK);
  opsdk_info_t info;
  CHECK(opsdk_get_info(dev, &info) == OPSDK_OK);
  CHECK(info.protocol_version == 1 && info.device_id == 2);
  CHECK(strcmp(info.serial, "SIMULATED0001") == 0);
  CHECK(opsdk_has(&info, OPSDK_OP_ROM_READ) && !opsdk_has(&info, OPSDK_OP_LED_SET));
  CHECK(!opsdk_has(&info, 0x10));

  opsdk_cart_t cart;
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK);
  CHECK(cart.platform == OPSDK_PLATFORM_GB && cart.rom_size == 65536 && cart.save_size == 8192);

  static uint8_t buf[FAKE_ROM_SIZE];
  CHECK(opsdk_rom_read(dev, &cart, 4096, 8192, buf, NULL, NULL) == OPSDK_OK);
  CHECK(memcmp(buf, fake_rom + 4096, 8192) == 0);
  uint32_t crc = 0;
  CHECK(opsdk_rom_crc32(dev, &cart, 0, FAKE_ROM_SIZE, &crc) == OPSDK_OK);
  CHECK(crc == opsdk_crc32(0, fake_rom, FAKE_ROM_SIZE));
  CHECK(opsdk_rom_read(dev, &cart, 100, 512, buf, NULL, NULL) == OPSDK_E_ARGUMENT);
  CHECK(opsdk_rom_read(dev, &cart, 65536, 512, buf, NULL, NULL) == OPSDK_BAD_ARGUMENT);

  CHECK(opsdk_rom_read(dev, &cart, 0, 4096, buf, NULL, NULL) == OPSDK_OK);
  CHECK(memcmp(buf, fake_rom, 4096) == 0);
  CHECK(opsdk_rom_read(dev, &cart, 61440, 4096, buf, NULL, NULL) == OPSDK_OK);
  CHECK(memcmp(buf, fake_rom + 61440, 4096) == 0);
  uint8_t rtc[OPSDK_RTC_SIZE];
  CHECK(opsdk_rtc_read(dev, &cart, rtc) == OPSDK_OK);

  CHECK(opsdk_save_read(dev, &cart, 0, 8192, buf) == OPSDK_OK);
  CHECK(memcmp(buf, fake_save, 8192) == 0);

  uint8_t data[8192];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 3);
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_NO_SESSION);
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_OK);
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_OK);
  CHECK(opsdk_save_crc32(dev, &cart, 0, 8192, &crc) == OPSDK_OK);
  CHECK(crc == opsdk_crc32(0, data, sizeof(data)));
  data[0] = 0xEE;
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_E_DEVICE);
  CHECK(opsdk_last_error(dev) == 0x48);
  CHECK(opsdk_session_end(dev) == OPSDK_OK);
  CHECK(opsdk_session_end(dev) == OPSDK_NO_SESSION);
  opsdk_led_t led = {{1, 2, 3}, {1, 2, 3}, 0x10, 0, 0x10, 100, 0, 1000};
  CHECK(opsdk_led_set(dev, &led) == OPSDK_UNKNOWN_COMMAND);
  CHECK(opsdk_led_release(dev) == OPSDK_OK);
  finish(dev, fake);

  o.sdk = false;
  fake = spawn(&dev, &o);
  CHECK(opsdk_ping(dev, 0x12) == OPSDK_UNKNOWN_COMMAND);
  CHECK(opsdk_get_info(dev, &info) == OPSDK_UNKNOWN_COMMAND);
  finish(dev, fake);
}

static void test_small_save(void) {
  fake_options_t o = {.sdk = true, .platform = 0x20, .save_size = 32};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  opsdk_cart_t cart;
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK);
  CHECK(cart.save_size == 32);
  uint8_t save[32] = {0};
  uint32_t crc = 0;
  CHECK(opsdk_save_read(dev, &cart, 0, 32, save) == OPSDK_OK);
  CHECK(memcmp(save, fake_save, 32) == 0);
  CHECK(opsdk_save_crc32(dev, &cart, 0, 32, &crc) == OPSDK_OK);
  CHECK(crc == opsdk_crc32(0, fake_save, 32));
  CHECK(opsdk_save_read(dev, &cart, 0, 16, save) == OPSDK_E_ARGUMENT);
  finish(dev, fake);
}

static void test_cart_error(void) {
  fake_options_t o = {.sdk = true, .platform = 0x20, .rtc_status = OPSDK_CART_ERROR};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  uint8_t rtc[OPSDK_RTC_SIZE];
  CHECK(opsdk_rtc_read(dev, NULL, rtc) == OPSDK_CART_ERROR);
  CHECK(strstr(opsdk_strerror(OPSDK_CART_ERROR), "did not complete") != NULL);
  finish(dev, fake);
}

static void test_empty_slot(void) {
  fake_options_t o = {.sdk = true, .platform = 0};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  opsdk_cart_t cart;
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK);
  CHECK(cart.platform == OPSDK_PLATFORM_EMPTY && cart.rom_size == 0);
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_NO_CARTRIDGE);
  finish(dev, fake);
}

static void test_session_poll_empty(void) {
  fake_options_t o = {.sdk = true, .platform = 0x20};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  opsdk_cart_t cart, now;
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_OK);
  fake_set_platform(fake, 0);
  CHECK(opsdk_cart_status(dev, &now) == OPSDK_OK);
  CHECK(now.platform == OPSDK_PLATFORM_EMPTY);
  uint8_t data[FAKE_SAVE_SIZE] = {0};
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_NO_SESSION);
  finish(dev, fake);
}

static void test_session_begin_fail(void) {
  fake_options_t pulled = {.sdk = true, .platform = 0x20, .pulled_after_begin = true};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &pulled);
  opsdk_cart_t cart;
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_NO_CARTRIDGE);
  CHECK(opsdk_session_end(dev) == OPSDK_NO_SESSION);
  finish(dev, fake);

  fake_options_t expired = {.sdk = true, .platform = 0x20, .expires_after_begin = true};
  fake = spawn(&dev, &expired);
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_NO_SESSION);
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK && cart.platform == OPSDK_PLATFORM_GB);
  finish(dev, fake);

  fake_options_t failing = {.sdk = true, .platform = 0x20, .status_fails = true};
  fake = spawn(&dev, &failing);
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_ABORTED);
  CHECK(fake_count(fake, 0xA5) == 1);
  CHECK(opsdk_session_end(dev) == OPSDK_NO_SESSION);
  finish(dev, fake);
}

static bool cancel_after_1k(uint32_t done, uint32_t total, void *user) {
  (void)total;
  (*(int *)user)++;
  return done < 1024;
}

static void test_cancel(void) {
  fake_options_t o = {.sdk = true, .platform = 0x20};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  opsdk_cart_t cart;
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_OK);
  static uint8_t buf[FAKE_ROM_SIZE];
  int calls = 0;
  int rc = opsdk_rom_read(dev, &cart, 0, 8192, buf, cancel_after_1k, &calls);
  CHECK(rc == OPSDK_OK);
  CHECK(calls >= 2);

  uint8_t data[FAKE_SAVE_SIZE];
  memset(data, 0x77, sizeof(data));
  calls = 0;
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), cancel_after_1k, &calls) == OPSDK_ABORTED);
  uint8_t now[FAKE_SAVE_SIZE];
  CHECK(opsdk_save_read(dev, &cart, 0, sizeof(now), now) == OPSDK_OK);
  CHECK(now[0] == 0x77);
  CHECK(now[FAKE_SAVE_SIZE - 1] == fake_save[FAKE_SAVE_SIZE - 1]);
  CHECK(fake_count(fake, 0xB2) == 1);
  finish(dev, fake);
}

static void test_retry_policy(void) {
  // stale data before commands 1 and 5
  fake_options_t o = {.sdk = true, .platform = 0x20, .stale_before = {1, 5}};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  opsdk_cart_t cart;
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK);
  CHECK(cart.platform == OPSDK_PLATFORM_GB);
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_OK);
  uint8_t data[FAKE_SAVE_SIZE];
  memset(data, 0x11, sizeof(data));
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_E_PROTOCOL);
  uint32_t crc = 0;
  CHECK(opsdk_save_crc32(dev, &cart, 0, FAKE_SAVE_SIZE, &crc) == OPSDK_OK);
  CHECK(crc == opsdk_crc32(0, fake_save, FAKE_SAVE_SIZE));
  CHECK(fake_count(fake, 0xB2) == 1);
  CHECK(fake_count(fake, 0xA2) == 3);
  finish(dev, fake);
}

static void test_arguments(void) {
  opsdk_t *dev = NULL;
  CHECK(opsdk_open(NULL, NULL) == OPSDK_E_ARGUMENT);
  CHECK(opsdk_open_fd(&dev, -1) == OPSDK_E_ARGUMENT);
#if defined(_WIN32)
  CHECK(opsdk_open(&dev, "COM251") == OPSDK_E_NO_DEVICE && dev == NULL);
#else
  CHECK(opsdk_open(&dev, "/dev/opsdk-does-not-exist") == OPSDK_E_NO_DEVICE && dev == NULL);
  CHECK(opsdk_open(&dev, "/dev/null") == OPSDK_E_IO);
#endif
  CHECK(opsdk_open_transport(&dev, NULL, NULL) == OPSDK_E_ARGUMENT);
  opsdk_transport_t incomplete = {NULL, NULL, NULL};
  CHECK(opsdk_open_transport(&dev, &incomplete, NULL) == OPSDK_E_ARGUMENT);
  char text[128] = "";
  opsdk_os_error_text(2, text, sizeof(text));
  CHECK(text[0] != '\0');
  CHECK(opsdk_ping(NULL, 0) == OPSDK_E_ARGUMENT);
  CHECK(opsdk_abort(NULL) == OPSDK_E_ARGUMENT);
  opsdk_close(NULL);
  CHECK(strcmp(opsdk_version(), OPSDK_VERSION) == 0);
  static const int codes[] = {OPSDK_OK, OPSDK_UNKNOWN_COMMAND, OPSDK_UNSUPPORTED, OPSDK_BAD_ARGUMENT,
                              OPSDK_NO_CARTRIDGE, OPSDK_CART_UNSTABLE, OPSDK_ABORTED, OPSDK_TIMEOUT,
                              OPSDK_NOT_PERMITTED, OPSDK_NO_SESSION, OPSDK_CART_ERROR, OPSDK_E_IO,
                              OPSDK_E_TIMEOUT, OPSDK_E_PROTOCOL, OPSDK_E_ARGUMENT, OPSDK_E_NO_DEVICE,
                              OPSDK_E_DEVICE, OPSDK_E_NO_MEMORY};
  for (size_t k = 0; k < sizeof(codes) / sizeof(codes[0]); k++) {
    CHECK(strcmp(opsdk_strerror(codes[k]), "unknown status") != 0);
  }
  CHECK(strcmp(opsdk_strerror(0x7F), "unknown status") == 0);
  CHECK(strcmp(opsdk_strerror(OPSDK_E_NO_MEMORY), "out of memory") == 0);
  CHECK(opsdk_list_ports(NULL, 0) == 0);

  opsdk_cart_t a, b;
  uint8_t raw[512] = {0};
  raw[2] = 0x20;
  raw[5] = 15;
  opsdk_parse_signature(raw, &a);
  opsdk_parse_signature(raw, &b);
  CHECK(opsdk_same_cartridge(&a, &b));
  b.header_bytes[3] ^= 1;
  CHECK(!opsdk_same_cartridge(&a, &b));
}

typedef struct {
  int sends, receives, events;
  bool saw_drain, saw_resend, saw_status, saw_error_packet, saw_no_resend;
} trace_log_t;

static void on_trace(const opsdk_trace_t *e, void *user) {
  trace_log_t *log = user;
  if (e->kind == OPSDK_TRACE_SEND) log->sends++;
  if (e->kind == OPSDK_TRACE_RECEIVE) log->receives++;
  if (e->kind != OPSDK_TRACE_EVENT) return;
  log->events++;
  if (strstr(e->message, "drained ")) log->saw_drain = true;
  if (strcmp(e->message, "re-sending CART_STATUS (read-only)") == 0) log->saw_resend = true;
  if (strcmp(e->message, "not re-sending SAVE_WRITE (it changes state)") == 0) log->saw_no_resend = true;
  if (strcmp(e->message, "DONE status 0x00") == 0) log->saw_status = true;
  if (strcmp(e->message, "device error packet 0x48") == 0) log->saw_error_packet = true;
}

static void test_trace(void) {
  fake_options_t o = {.sdk = true, .platform = 0x20, .stale_before = {1, 5}};
  opsdk_t *dev;
  fake_device_t *fake = spawn(&dev, &o);
  trace_log_t log = {0};
  opsdk_set_trace(dev, on_trace, &log);
  opsdk_cart_t cart;
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK);
  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_OK);
  uint8_t data[FAKE_SAVE_SIZE];
  memset(data, 0xEE, sizeof(data));
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_E_PROTOCOL);
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_E_DEVICE);
  CHECK(log.sends > 0 && log.receives > 0 && log.events > 0);
  CHECK(log.saw_drain && log.saw_resend && log.saw_no_resend && log.saw_status && log.saw_error_packet);
  int events = log.events;
  opsdk_set_trace(dev, NULL, NULL);
  CHECK(opsdk_ping(dev, 1) == OPSDK_OK);
  CHECK(log.events == events);
  CHECK(opsdk_last_os_error(dev) == 0);
  finish(dev, fake);
}

int main(void) {
  fake_init_data();
  test_trace();
  test_vectors();
  test_signature();
  test_camera_defaults();
  test_rtc4513();
  test_arguments();
  test_transactions();
  test_small_save();
  test_cart_error();
  test_empty_slot();
  test_session_begin_fail();
  test_session_poll_empty();
  test_cancel();
  test_retry_policy();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("opsdk: all tests passed");
  return 0;
}
