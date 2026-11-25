#include "opsdk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portable.h"

static const char *port;

static void die(const char *what, int rc) {
  fprintf(stderr, "%s: %s (%d)\n", what, opsdk_strerror(rc), rc);
  exit(1);
}

static opsdk_t *connect_dev(void) {
  opsdk_t *dev;
  int rc = opsdk_open(&dev, port);
  if (rc) die("open", rc);
  return dev;
}

static const char *platform_name(opsdk_platform_t p) {
  switch (p) {
  case OPSDK_PLATFORM_EMPTY: return "empty";
  case OPSDK_PLATFORM_GB: return "GB";
  case OPSDK_PLATFORM_GBA: return "GBA";
  case OPSDK_PLATFORM_SNES: return "SNES";
  case OPSDK_PLATFORM_N64: return "N64";
  default: return "unknown";
  }
}

static void print_cart(const opsdk_cart_t *c) {
  printf("%s  type 0x%02X  ROM %u B  save %u B (chip 0x%02X)  logo %s  checksum %s%s%s\n",
         platform_name(c->platform), c->type_byte, c->rom_size, c->save_size,
         c->save_chip_type, c->logo_valid ? "ok" : "bad", c->checksum_valid ? "ok" : "bad",
         c->flashcart ? "  flashcart" : "", c->counterfeit ? "  counterfeit" : "");
}

static int write_file(const char *path, const void *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  bool wrote = fwrite(data, 1, n, f) == n;
  return fclose(f) || !wrote ? -1 : 0;
}

static uint8_t *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = size < 0 ? NULL : malloc((size_t)size);
  if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (buf) *n = (size_t)size;
  return buf;
}

static bool progress(uint32_t done, uint32_t total, void *user) {
  (void)user;
  fprintf(stderr, "\r  %u / %u KiB", done / 1024, total / 1024);
  if (done == total) fputc('\n', stderr);
  return true;
}

static int checks, fails;

#if defined(__GNUC__)
static void result(bool ok, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#endif
static void result(bool ok, const char *name, const char *fmt, ...) {
  checks++;
  if (!ok) fails++;
  printf("%s %-28s ", ok ? "  ok  " : "  FAIL", name);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}

static int check(opsdk_t *dev, bool allow_write) {
  opsdk_info_t info = {0};
  int rc = opsdk_get_info(dev, &info);
  result(rc == OPSDK_OK, "GET_INFO", "app %u.%u.%u, serial %s, caps 0x%08X", info.app_version[0],
         info.app_version[1], info.app_version[2], info.serial, info.capabilities);
  if (rc) return 1;
  result(opsdk_ping(dev, 0x3C) == OPSDK_OK, "PING", "%s", "");

  opsdk_cart_t cart = {0};
  rc = opsdk_cart_status(dev, &cart);
  result(rc == OPSDK_OK, "CART_STATUS", "%s", "");
  if (rc) return 1;
  print_cart(&cart);
  if (!cart.logo_valid) {
    printf("no readable cartridge, insert one or reseat it and run again\n");
    return 1;
  }

  uint32_t rom_size = cart.rom_size;
  uint8_t *rom = malloc(rom_size);
  if (!rom) die("check", OPSDK_E_NO_MEMORY);
  double t0 = portable_now_s();
  rc = opsdk_rom_read(dev, &cart, 0, rom_size, rom, progress, NULL);
  double dt = portable_now_s() - t0;
  uint32_t rom_crc = opsdk_crc32(0, rom, rom_size);
  result(rc == OPSDK_OK, "ROM_READ whole", "%u B in %.1f s (%.0f KB/s), crc32 %08X", rom_size, dt,
         rom_size / 1024.0 / dt, rom_crc);
  uint32_t crc = 0;
  t0 = portable_now_s();
  rc = opsdk_rom_crc32(dev, &cart, 0, rom_size, &crc);
  result(rc == OPSDK_OK && crc == rom_crc, "ROM_CRC32 whole", "%08X in %.1f s", crc, portable_now_s() - t0);
  uint32_t mid = (rom_size / 2) & ~511u, len = rom_size >= 65536 ? 16384 : 1024;
  uint8_t *part = malloc(len);
  if (!part) die("check", OPSDK_E_NO_MEMORY);
  rc = opsdk_rom_read(dev, &cart, mid, len, part, NULL, NULL);
  result(rc == OPSDK_OK && memcmp(part, rom + mid, len) == 0, "ROM_READ range", "0x%X +%u", mid, len);
  rc = opsdk_rom_read(dev, &cart, rom_size - 512, 512, part, NULL, NULL);
  result(rc == OPSDK_OK && memcmp(part, rom + rom_size - 512, 512) == 0, "ROM_READ last packet", "%s", "");
  rc = opsdk_rom_crc32(dev, &cart, mid, len, &crc);
  result(rc == OPSDK_OK && crc == opsdk_crc32(0, rom + mid, len), "ROM_CRC32 range", "%08X", crc);
  rc = opsdk_rom_read(dev, &cart, rom_size, 512, part, NULL, NULL);
  result(rc == OPSDK_BAD_ARGUMENT, "ROM_READ past the end", "%s", opsdk_strerror(rc));
  free(part);

  rc = opsdk_session_begin(dev, &cart);
  result(rc == OPSDK_OK, "SESSION_BEGIN", "%s", opsdk_strerror(rc));
  bool same = true;
  for (int i = 0; i < 20 && same; i++) {
    opsdk_cart_t c2;
    same = opsdk_cart_status(dev, &c2) == OPSDK_OK && c2.type_byte == cart.type_byte;
  }
  rc = opsdk_rom_crc32(dev, &cart, 0, rom_size, &crc);
  result(same && rc == OPSDK_OK && crc == rom_crc, "session polls, ROM CRC", "%08X", crc);

  if (cart.save_size > 0) {
    uint8_t *sv = malloc(cart.save_size);
    if (!sv) die("check", OPSDK_E_NO_MEMORY);
    rc = opsdk_save_read(dev, &cart, 0, cart.save_size, sv) == OPSDK_OK ? 0 : 1;
    uint32_t save_crc = opsdk_crc32(0, sv, cart.save_size);
    result(rc == 0, "SAVE_READ", "%u B, crc32 %08X", cart.save_size, save_crc);
    rc = opsdk_save_crc32(dev, &cart, 0, cart.save_size, &crc);
    result(rc == OPSDK_OK && crc == save_crc, "SAVE_CRC32", "%08X", crc);
    if (allow_write) {
      uint8_t *pattern = malloc(cart.save_size);
      if (!pattern) die("check", OPSDK_E_NO_MEMORY);
      for (uint32_t i = 0; i < cart.save_size; i++) pattern[i] = (uint8_t)(i * 13 + 7);
      rc = opsdk_save_write(dev, &cart, pattern, cart.save_size, NULL, NULL);
      int rc2 = opsdk_save_crc32(dev, &cart, 0, cart.save_size, &crc);
      result(rc == OPSDK_OK && rc2 == OPSDK_OK && crc == opsdk_crc32(0, pattern, cart.save_size),
             "SAVE_WRITE pattern", "%s", opsdk_strerror(rc));
      rc = opsdk_save_write(dev, &cart, sv, cart.save_size, NULL, NULL);
      rc2 = opsdk_save_crc32(dev, &cart, 0, cart.save_size, &crc);
      result(rc == OPSDK_OK && rc2 == OPSDK_OK && crc == save_crc, "SAVE_WRITE restore", "%08X", crc);
      free(pattern);
    }
    free(sv);
  } else {
    printf("  --   %-28s none\n", "save");
  }

  if (opsdk_has(&info, OPSDK_OP_RTC_READ)) {
    uint8_t raw[OPSDK_RTC_SIZE];
    rc = opsdk_rtc_read(dev, &cart, raw);
    result(rc == OPSDK_OK, "RTC_READ", "%02X %02X %02X %02X %02X %02X %02X %02X", raw[0], raw[1],
           raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
    opsdk_rtc4513_t c;
    if (rc == OPSDK_OK && cart.platform == OPSDK_PLATFORM_SNES && opsdk_rtc4513_decode(raw, &c) == OPSDK_OK)
      result(c.valid && c.calendar, "RTC-4513", "%02u-%02u-%02u %02u:%02u:%02u%s%s%s%s", c.year, c.month,
             c.day, c.hour, c.minute, c.second, c.hour12 ? " 12-hour" : "", c.stopped ? " stopped" : "",
             c.calendar ? "" : " calendar off", c.lost_power ? " lost power" : "");
  }

  if (opsdk_has(&info, OPSDK_OP_SENSOR_READ)) {
    opsdk_sensor_t s;
    rc = opsdk_sensor_read(dev, &s);
    if (rc == OPSDK_UNSUPPORTED) printf("  --   %-28s no sensor on this cart\n", "SENSOR_READ");
    else result(rc == OPSDK_OK, "SENSOR_READ", "kind %u x %04X y %04X z %04X", s.kind, s.x, s.y, s.z);
  }
  if (opsdk_has(&info, OPSDK_OP_RUMBLE_SET)) {
    rc = opsdk_rumble_set(dev, true);
    if (rc == OPSDK_UNSUPPORTED) {
      printf("  --   %-28s no motor on this cart\n", "RUMBLE_SET");
    } else {
      portable_sleep_ms(500);
      int rc2 = opsdk_rumble_set(dev, false);
      result(rc == OPSDK_OK && rc2 == OPSDK_OK, "RUMBLE_SET on/off", "0.5 s");
    }
  }

  opsdk_led_t led = {{0, 128, 255}, {255, 0, 128}, OPSDK_EFFECT_SINE, 50, OPSDK_EFFECT_CONSTANT,
                     100, 0, 1000};
  rc = opsdk_led_set(dev, &led);
  portable_sleep_ms(300);
  int rc2 = opsdk_led_release(dev);
  result(rc == OPSDK_OK && rc2 == OPSDK_OK, "LED_SET / RELEASE", "%s", "");

  rc = opsdk_session_end(dev);
  result(rc == OPSDK_OK, "SESSION_END", "%s", "");
  rc = opsdk_session_end(dev);
  result(rc == OPSDK_NO_SESSION, "SESSION_END twice", "%s", opsdk_strerror(rc));
  if (cart.save_size > 0 && allow_write) {
    uint8_t one[512] = {0};
    rc = opsdk_save_write(dev, &cart, one, 512, NULL, NULL);
    result(rc == OPSDK_NO_SESSION, "SAVE_WRITE without session", "%s", opsdk_strerror(rc));
  }
  free(rom);
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

static uint32_t num(const char *s) {
  return (uint32_t)strtoul(s, NULL, 0);
}

int main(int argc, char **argv) {
  int i = 1;
  if (i + 1 < argc && strcmp(argv[i], "-p") == 0) {
    port = argv[i + 1];
    i += 2;
  }
  if (i >= argc) {
    fprintf(stderr, "usage: opsdk [-p PORT] info|status|rom|rom-crc|save|save-crc|save-write|"
                    "rtc|led|led-release|rumble|sensor|camera|check\n");
    return 2;
  }
  const char *cmd = argv[i++];
  opsdk_t *dev = connect_dev();
  int rc = 0;
  opsdk_cart_t cart;

  if (strcmp(cmd, "info") == 0) {
    opsdk_info_t info;
    if ((rc = opsdk_get_info(dev, &info))) die("info", rc);
    printf("SDK v%u  device %u  board rev %u  app %u.%u.%u  sys %u.%u.%u  serial %s\n"
           "capabilities 0x%08X  session idle timeout %u s\n",
           info.protocol_version, info.device_id, info.board_revision, info.app_version[0],
           info.app_version[1], info.app_version[2], info.sys_version[0], info.sys_version[1],
           info.sys_version[2], info.serial, info.capabilities, info.session_idle_timeout_s);
  } else if (strcmp(cmd, "status") == 0) {
    if ((rc = opsdk_cart_status(dev, &cart))) die("status", rc);
    print_cart(&cart);
  } else if (strcmp(cmd, "check") == 0) {
    rc = check(dev, i < argc && strcmp(argv[i], "--write") == 0);
  } else {
    if ((rc = opsdk_cart_status(dev, &cart))) die("status", rc);
    if (strcmp(cmd, "rom") == 0 && i < argc) {
      uint32_t off = i + 2 < argc ? num(argv[i + 1]) : 0;
      uint32_t len = i + 2 < argc ? num(argv[i + 2]) : cart.rom_size;
      if (!len) die("rom", OPSDK_E_ARGUMENT);
      uint8_t *buf = malloc(len);
      if (!buf) die("rom", OPSDK_E_NO_MEMORY);
      if ((rc = opsdk_rom_read(dev, &cart, off, len, buf, progress, NULL))) die("rom", rc);
      if (write_file(argv[i], buf, len)) die("write file", OPSDK_E_IO);
      printf("%s: %u B, crc32 %08X\n", argv[i], len, opsdk_crc32(0, buf, len));
    } else if (strcmp(cmd, "rom-crc") == 0) {
      uint32_t off = i + 1 < argc ? num(argv[i]) : 0, len = i + 1 < argc ? num(argv[i + 1]) : cart.rom_size;
      uint32_t crc;
      if ((rc = opsdk_rom_crc32(dev, &cart, off, len, &crc))) die("rom-crc", rc);
      printf("%08X\n", crc);
    } else if (strcmp(cmd, "save") == 0 && i < argc) {
      if (!cart.save_size) die("save", OPSDK_UNSUPPORTED);
      uint8_t *buf = malloc(cart.save_size);
      if (!buf) die("save", OPSDK_E_NO_MEMORY);
      if ((rc = opsdk_save_read(dev, &cart, 0, cart.save_size, buf))) die("save", rc);
      if (write_file(argv[i], buf, cart.save_size)) die("write file", OPSDK_E_IO);
      printf("%s: %u B, crc32 %08X\n", argv[i], cart.save_size, opsdk_crc32(0, buf, cart.save_size));
    } else if (strcmp(cmd, "save-crc") == 0) {
      uint32_t crc;
      if ((rc = opsdk_save_crc32(dev, &cart, 0, cart.save_size, &crc))) die("save-crc", rc);
      printf("%08X\n", crc);
    } else if (strcmp(cmd, "save-write") == 0 && i < argc) {
      size_t n;
      uint8_t *buf = read_file(argv[i], &n);
      if (!buf) die("read file", OPSDK_E_ARGUMENT);
      if ((rc = opsdk_session_begin(dev, &cart))) die("session", rc);
      if ((rc = opsdk_save_write(dev, &cart, buf, (uint32_t)n, progress, NULL))) die("save-write", rc);
      opsdk_session_end(dev);
      printf("wrote %zu B\n", n);
    } else if (strcmp(cmd, "rtc") == 0) {
      uint8_t raw[OPSDK_RTC_SIZE];
      if ((rc = opsdk_rtc_read(dev, &cart, raw))) die("rtc", rc);
      for (int k = 0; k < 48; k++) printf("%02X%c", raw[k], k % 16 == 15 ? '\n' : ' ');
      opsdk_rtc4513_t c;
      if (cart.platform == OPSDK_PLATFORM_SNES && opsdk_rtc4513_decode(raw, &c) == OPSDK_OK)
        printf("RTC-4513 %02u-%02u-%02u %02u:%02u:%02u%s%s%s%s%s\n", c.year, c.month, c.day, c.hour,
               c.minute, c.second, c.hour12 ? " 12-hour" : "", c.stopped ? " stopped" : "",
               c.calendar ? "" : " calendar off", c.lost_power ? " lost power" : "",
               c.valid ? "" : " not valid");
    } else if (strcmp(cmd, "led") == 0 && i + 2 < argc) {
      opsdk_led_t led = {{(uint8_t)num(argv[i]), (uint8_t)num(argv[i + 1]), (uint8_t)num(argv[i + 2])},
                         {0, 0, 0}, OPSDK_EFFECT_CONSTANT, 0, OPSDK_EFFECT_CONSTANT, 100, 0, 1000};
      memcpy(led.secondary, led.primary, 3);
      if ((rc = opsdk_led_set(dev, &led))) die("led", rc);
    } else if (strcmp(cmd, "led-release") == 0) {
      if ((rc = opsdk_led_release(dev))) die("led-release", rc);
    } else if (strcmp(cmd, "rumble") == 0 && i < argc) {
      if ((rc = opsdk_session_begin(dev, &cart))) die("session", rc);
      if ((rc = opsdk_rumble_set(dev, strcmp(argv[i], "on") == 0))) die("rumble", rc);
      if (strcmp(argv[i], "on") == 0) {
        portable_sleep_ms(1000);
        opsdk_rumble_set(dev, false);
      }
      opsdk_session_end(dev);
    } else if (strcmp(cmd, "sensor") == 0) {
      opsdk_sensor_t s;
      for (int k = 0; k < 20; k++) {
        if ((rc = opsdk_sensor_read(dev, &s))) die("sensor", rc);
        printf("kind %u  x %5u  y %5u  z %5u\n", s.kind, s.x, s.y, s.z);
        portable_sleep_ms(100);
      }
    } else if (strcmp(cmd, "camera") == 0 && i < argc) {
      static uint8_t frame[OPSDK_CAMERA_FRAME_SIZE];
      opsdk_camera_t settings;
      opsdk_camera_defaults(&settings);
      if ((rc = opsdk_session_begin(dev, &cart))) die("session", rc);
      if ((rc = opsdk_camera_start(dev))) die("camera start", rc);
      for (int k = 0; k < 2; k++)
        if ((rc = opsdk_camera_capture(dev, &settings, frame))) die("camera capture", rc);
      opsdk_camera_stop(dev);
      opsdk_session_end(dev);
      static uint8_t img[112][128];
      for (int t = 0; t < 16 * 14; t++)
        for (int y = 0; y < 8; y++) {
          uint8_t lo = frame[t * 16 + y * 2], hi = frame[t * 16 + y * 2 + 1];
          for (int x = 0; x < 8; x++) {
            int v = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1);
            img[(t / 16) * 8 + y][(t % 16) * 8 + x] = (uint8_t)(255 - v * 85);
          }
        }
      FILE *f = fopen(argv[i], "wb");
      if (!f) die("write file", OPSDK_E_IO);
      fprintf(f, "P5 128 112 255\n");
      bool wrote = fwrite(img, 1, sizeof(img), f) == sizeof(img);
      if (fclose(f) || !wrote) die("write file", OPSDK_E_IO);
      printf("%s: 128x112\n", argv[i]);
    } else {
      fprintf(stderr, "unknown command or missing argument: %s\n", cmd);
      rc = 2;
    }
  }
  opsdk_close(dev);
  return rc;
}
