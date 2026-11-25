#include "opsdk_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  int fd;
  int os_error;
} posix_port_t;

#define WRITE_STALL_MS 60000

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int port_read(void *context, uint8_t *buf, size_t n, uint32_t timeout_ms) {
  posix_port_t *port = context;
  size_t got = 0;
  int64_t deadline = now_ms() + timeout_ms;
  while (got < n) {
    int64_t left = deadline - now_ms();
    if (left < 0) left = 0;
    struct pollfd p = {.fd = port->fd, .events = POLLIN};
    int r = poll(&p, 1, (int)left);
    if (r < 0) {
      if (errno == EINTR) continue;
      port->os_error = errno;
      return OPSDK_E_IO;
    }
    if (r == 0) break;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      port->os_error = EIO;
      return OPSDK_E_IO;
    }
    ssize_t k = read(port->fd, buf + got, n - got);
    if (k < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      port->os_error = errno;
      return OPSDK_E_IO;
    }
    if (k == 0) {
      port->os_error = EIO;
      return OPSDK_E_IO;
    }
    got += (size_t)k;
  }
  return (int)got;
}

static int port_write(void *context, const uint8_t *buf, size_t n) {
  posix_port_t *port = context;
  size_t done = 0;
  while (done < n) {
    ssize_t k = write(port->fd, buf + done, n - done);
    if (k < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        struct pollfd p = {.fd = port->fd, .events = POLLOUT};
        if (poll(&p, 1, WRITE_STALL_MS) > 0) continue;
        port->os_error = errno ? errno : EAGAIN;
        return OPSDK_E_IO;
      }
      port->os_error = errno;
      return OPSDK_E_IO;
    }
    done += (size_t)k;
  }
  return OPSDK_OK;
}

static void port_close(void *context) {
  posix_port_t *port = context;
  if (port->fd >= 0) close(port->fd);
  free(port);
}

static const opsdk_transport_t POSIX_TRANSPORT = {port_read, port_write, port_close};

int opsdk_platform_wrap_fd(int fd, opsdk_transport_t *transport, void **context) {
  posix_port_t *port = calloc(1, sizeof(*port));
  if (!port) return OPSDK_E_NO_MEMORY;
  port->fd = fd;
  *transport = POSIX_TRANSPORT;
  *context = port;
  return OPSDK_OK;
}

void opsdk_platform_free_wrapper(void *context) {
  free(context);
}

int opsdk_platform_os_error(void *context) {
  return ((posix_port_t *)context)->os_error;
}

int opsdk_platform_open(const char *path, opsdk_transport_t *transport, void **context) {
  /* O_NONBLOCK or the open can hang waiting for carrier */
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return errno == ENOENT || errno == ENXIO || errno == ENODEV ? OPSDK_E_NO_DEVICE : OPSDK_E_IO;
  }
  struct termios tio;
  int err = 0;
  if (!isatty(fd)) {
    err = ENOTTY;
  } else if (tcgetattr(fd, &tio) != 0) {
    err = errno;
  } else {
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) err = errno;
  }
  if (err) {
    close(fd);
    errno = err;
    return OPSDK_E_IO;
  }
  tcflush(fd, TCIOFLUSH);
  int rc = opsdk_platform_wrap_fd(fd, transport, context);
  if (rc != OPSDK_OK) {
    close(fd);
    errno = ENOMEM;
  }
  return rc;
}

void opsdk_os_error_text(int os_error, char *buf, size_t size) {
  if (!buf || !size) return;
  snprintf(buf, size, "%s", strerror(os_error));
}

#if defined(__linux__)
static bool read_hex_file(const char *path, unsigned *value) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  int n = fscanf(f, "%x", value);
  fclose(f);
  return n == 1;
}

static bool linux_is_operator(const char *dev_path) {
  static const unsigned products[] = OPSDK_USB_PRODUCT_IDS;
  const char *name = strrchr(dev_path, '/');
  name = name ? name + 1 : dev_path;
  char path[256];
  unsigned vendor, product;
  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idVendor", name);
  // can't tell without ids, let the ping decide
  if (!read_hex_file(path, &vendor)) return true;
  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idProduct", name);
  if (!read_hex_file(path, &product)) return true;
  if (vendor != OPSDK_USB_VENDOR_ID) return false;
  for (size_t i = 0; i < sizeof(products) / sizeof(products[0]); i++) {
    if (product == products[i]) return true;
  }
  return false;
}
#endif

int opsdk_list_ports(char *buf, size_t size) {
#if defined(__APPLE__)
  static const char *pattern = "/dev/cu.usbmodem*";
#else
  static const char *pattern = "/dev/ttyACM*";
#endif
  size_t used = 0;
  int count = 0;
  if (!buf || size < 2) return 0;
  glob_t g;
  if (glob(pattern, 0, NULL, &g) == 0) {
    for (size_t k = 0; k < g.gl_pathc; k++) {
#if defined(__linux__)
      if (!linux_is_operator(g.gl_pathv[k])) continue;
#endif
      size_t len = strlen(g.gl_pathv[k]) + 1;
      if (used + len + 1 > size) break;
      memcpy(buf + used, g.gl_pathv[k], len);
      used += len;
      count++;
    }
    globfree(&g);
  }
  buf[used] = '\0';
  return count;
}
