#include "opsdk.h"

#include "fake_device.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      failures++;                                                            \
    }                                                                        \
  } while (0)

int main(void) {
  fake_init_data();
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) || unlockpt(master)) {
    perror("posix_openpt");
    return 1;
  }
  char path[256];
  snprintf(path, sizeof(path), "%s", ptsname(master));

  fake_options_t o = {.sdk = true, .platform = 0x30};
  pid_t pid = fake_spawn(master, -1, &o);
  close(master);

  opsdk_t *dev = NULL;
  int rc = opsdk_open(&dev, path);
  CHECK(rc == OPSDK_OK);
  if (rc != OPSDK_OK) {
    kill(pid, SIGTERM);
    return 1;
  }
  opsdk_set_timeout(dev, 3000);

  CHECK(opsdk_ping(dev, 0x77) == OPSDK_OK);
  opsdk_info_t info;
  CHECK(opsdk_get_info(dev, &info) == OPSDK_OK);
  CHECK(strcmp(info.serial, "SIMULATED0001") == 0);
  opsdk_cart_t cart;
  CHECK(opsdk_cart_status(dev, &cart) == OPSDK_OK);
  CHECK(cart.platform == OPSDK_PLATFORM_GBA && cart.rom_size == FAKE_ROM_SIZE);

  static uint8_t rom[FAKE_ROM_SIZE];
  CHECK(opsdk_rom_read(dev, &cart, 0, cart.rom_size, rom, NULL, NULL) == OPSDK_OK);
  CHECK(memcmp(rom, fake_rom, FAKE_ROM_SIZE) == 0);
  uint32_t crc = 0;
  CHECK(opsdk_rom_crc32(dev, &cart, 0, cart.rom_size, &crc) == OPSDK_OK);
  CHECK(crc == opsdk_crc32(0, rom, sizeof(rom)));

  CHECK(opsdk_session_begin(dev, &cart) == OPSDK_OK);
  uint8_t data[FAKE_SAVE_SIZE];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 5);
  CHECK(opsdk_save_write(dev, &cart, data, sizeof(data), NULL, NULL) == OPSDK_OK);
  CHECK(opsdk_save_crc32(dev, &cart, 0, sizeof(data), &crc) == OPSDK_OK);
  CHECK(crc == opsdk_crc32(0, data, sizeof(data)));
  CHECK(opsdk_session_end(dev) == OPSDK_OK);

  opsdk_close(dev);
  int st;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("opsdk serial (pty): all tests passed");
  return 0;
}
