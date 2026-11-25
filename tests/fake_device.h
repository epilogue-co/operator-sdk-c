#ifndef FAKE_DEVICE_H
#define FAKE_DEVICE_H

#include "opsdk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <sys/types.h>
#endif

#define FAKE_ROM_SIZE (64 * 1024)
#define FAKE_SAVE_SIZE 8192

typedef struct {
  bool sdk;
  uint8_t platform;
  int stale_before[4];
  uint32_t save_size;
  uint8_t rtc_status;
  bool pulled_after_begin;
  bool expires_after_begin;
  bool status_fails;
} fake_options_t;

typedef struct fake_device fake_device_t;

extern uint8_t fake_rom[FAKE_ROM_SIZE];
extern uint8_t fake_save[FAKE_SAVE_SIZE];
void fake_init_data(void);

fake_device_t *fake_create(const fake_options_t *options);
void fake_destroy(fake_device_t *fake);
const opsdk_transport_t *fake_transport(void);
int fake_count(const fake_device_t *fake, uint8_t op);
void fake_set_platform(fake_device_t *fake, uint8_t platform);

#if !defined(_WIN32)
pid_t fake_spawn(int fd, int host_fd, const fake_options_t *options);
#endif

#endif
