#ifndef OPSDK_PORTABLE_H
#define OPSDK_PORTABLE_H

#include <stdio.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

static inline void portable_sleep_ms(unsigned ms) {
  Sleep(ms);
}

static inline double portable_now_s(void) {
  return (double)GetTickCount64() / 1000.0;
}

static inline FILE *portable_create_new(const char *path) {
  int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
  return fd >= 0 ? _fdopen(fd, "wb") : NULL;
}
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static inline void portable_sleep_ms(unsigned ms) {
  struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static inline double portable_now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline FILE *portable_create_new(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  return fd >= 0 ? fdopen(fd, "wb") : NULL;
}
#endif

#endif
