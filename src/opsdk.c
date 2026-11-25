#include "opsdk.h"
#include "opsdk_platform.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMD_SIZE 64
#define PKT_SIZE 512

#define OP_PING 0xA0
#define OP_GET_INFO 0xA1
#define OP_CART_STATUS 0xA2
#define CART_STATUS_REQUIRE_SESSION 0x01
#define OP_SESSION_BEGIN 0xA4
#define OP_SESSION_END 0xA5
#define OP_ROM_READ 0xA8
#define OP_ROM_CRC32 0xA9
#define OP_SAVE_READ 0xB1
#define OP_SAVE_WRITE 0xB2
#define OP_SAVE_CRC32 0xB3
#define OP_RTC_READ 0xB4
#define OP_RTC_WRITE 0xB5
#define OP_LED_SET 0xB8
#define OP_LED_RELEASE 0xB9
#define OP_CAMERA_START 0xBA
#define OP_CAMERA_CAPTURE 0xBB
#define OP_CAMERA_STOP 0xBC
#define OP_SENSOR_READ 0xBD
#define OP_RUMBLE_SET 0xBE

#define PKT_READY 0x00
#define PKT_DONE 0x01
#define PKT_PROCEED 0x02

#define CMD_SAVE_CHIP 0x01
#define CMD_ROM_SIZE 0x02
#define CMD_SAVE_SIZE 0x06
#define DATA_OFFSET 0x0A
#define REQUEST_ID_OFFSET 0x3B
#define CMD_CRC 0x3C

#define REPLY_MAGIC_1 0xC0
#define REPLY_MAGIC_2 0xDE
#define REPLY_TYPE 0x02
#define REPLY_OP 0x03
#define REPLY_OP_INVERSE 0x04
#define REPLY_STATUS 0x05
#define REPLY_REQUEST_ID 0x06

#define ERROR_MAGIC_1 0x55
#define ERROR_MAGIC_2 0xAA
#define ERROR_CODE 0x02

#define SIG_TYPE 0x02
#define SIG_LOGO 0x03
#define SIG_CHECKSUM 0x04
#define SIG_ROM_SHIFT 0x05
#define SIG_SAVE_SHIFT 0x06
#define SIG_SAVE_CHIP 0x07
#define SIG_REVISION 0x08
#define SIG_REGION 0x09
#define SIG_ROM_CODED 0x0A
#define SIG_HEADER 0x0D
#define SAVE_CHIP_SRAM_96K 0x24
#define SAVE_SIZE_SRAM_96K 98304
#define PLATFORM_MASK 0xF0
#define FLAGS_MASK 0x0F
#define FLAG_FLASHCART 0x0B
#define FLAG_COUNTERFEIT 0x04

#define INFO_PROTOCOL 0x04
#define INFO_DEVICE_ID 0x06
#define INFO_BOARD 0x07
#define INFO_APP_VERSION 0x08
#define INFO_SYS_VERSION 0x0B
#define INFO_CAPABILITIES 0x0E
#define INFO_SERIAL 0x12
#define INFO_IDLE_TIMEOUT 0x22
#define INFO_SIZE 0x24

#define SENSOR_KIND 0
#define SENSOR_X 2
#define SENSOR_Y 4
#define SENSOR_Z 6
#define SENSOR_SIZE 8

#define LED_COLORS 0x01
#define LED_COLOR_EFFECT 0x19
#define LED_COLOR_DUTY 0x1A
#define LED_LIGHT_EFFECT 0x1B
#define LED_LIGHT_DUTY 0x1C
#define LED_OFFSET 0x1D
#define LED_PERIOD 0x1E

#define CAM_EXPOSURE 1
#define CAM_GAIN 3
#define CAM_VERTICAL_EDGE 4
#define CAM_EDGE_OPERATION 5
#define CAM_EDGE_RATIO 6
#define CAM_INVERT 7
#define CAM_VREF 8
#define CAM_ZERO_CALIBRATION 9
#define CAM_OUTPUT_REF 10
#define CAM_MATRIX 11
#define CAMERA_MATRIX_SLOPE 0x12
#define CAMERA_MATRIX_OFFSET 0x82

#define SN_RTC_TYPE_RTC4513 2
#define RTC4513_REGISTERS 16
#define RTC4513_SECOND 0
#define RTC4513_MINUTE 2
#define RTC4513_HOUR 4
#define RTC4513_DAY 6
#define RTC4513_MONTH 8
#define RTC4513_YEAR 10
#define RTC4513_WEEKDAY 12
#define RTC4513_CONTROL_D 13
#define RTC4513_CONTROL_E 14
#define RTC4513_CONTROL_F 15
#define RTC4513_LOST_POWER 0x08
#define RTC4513_PM 0x04
#define RTC4513_CALENDAR 0x02
#define RTC4513_INTERRUPT_MASKED 0x0F
#define RTC4513_24_HOUR 0x04
#define RTC4513_STOP 0x02

#define DEFAULT_TIMEOUT_MS 5000
#define PROBE_TIMEOUT_MS 500
#define READY_TIMEOUT_MS 3000
#define DONE_WRITE_TIMEOUT_MS 120000
#define CRC_BASE_MS 5000
#define CRC_MS_PER_KIB 10
#define RTC_WRITE_TIMEOUT_MS 10000
#define STALE_QUIET_MS 300

struct opsdk {
  opsdk_transport_t transport;
  void *context;
  bool builtin;
  int os_error;
  opsdk_trace_fn trace;
  void *trace_user;
  uint8_t op;
  uint32_t timeout_ms;
  uint8_t request_id;
  uint8_t last_error;
  bool stale;
};

static const uint8_t UNSTABLE_MAGIC[8] = {0x55, 0xDE, 0xC0, 0xDE, 0xDB, 0xA1, 0x1A, 0xD5};
static const uint8_t ABORT_MAGIC[8] = {'E', 'P', 'I', 'A', 'B', 'O', 'R', 'T'};

static uint32_t crc32_mpeg2(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; i++) {
    crc ^= (uint32_t)data[i] << 24;
    for (int b = 0; b < 8; b++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

uint32_t opsdk_crc32(uint32_t crc, const void *data, size_t length) {
  const uint8_t *p = data;
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void seal(uint8_t packet[CMD_SIZE]) {
  put_u32(&packet[CMD_CRC], crc32_mpeg2(packet, CMD_CRC));
}

void opsdk_build_command(uint8_t packet[64], uint8_t operation, uint8_t save_chip,
                         uint32_t rom_size, uint32_t save_size, const uint8_t *data,
                         size_t data_len, uint8_t request_id) {
  memset(packet, 0, CMD_SIZE);
  packet[0] = operation;
  packet[CMD_SAVE_CHIP] = save_chip;
  put_u32(&packet[CMD_ROM_SIZE], rom_size);
  put_u32(&packet[CMD_SAVE_SIZE], save_size);
  if (data && data_len) {
    if (data_len > REQUEST_ID_OFFSET - DATA_OFFSET) data_len = REQUEST_ID_OFFSET - DATA_OFFSET;
    memcpy(&packet[DATA_OFFSET], data, data_len);
  }
  packet[REQUEST_ID_OFFSET] = request_id;
  seal(packet);
}

static bool is_ready_done(const uint8_t *p, uint8_t type, uint8_t op) {
  const uint8_t inverse = (uint8_t)~op;
  return p[0] == REPLY_MAGIC_1 && p[1] == REPLY_MAGIC_2 && p[REPLY_TYPE] == type &&
         p[REPLY_OP] == op && p[REPLY_OP_INVERSE] == inverse;
}

static bool zero_from(const uint8_t *p, size_t start) {
  for (size_t i = start; i < PKT_SIZE; i++)
    if (p[i]) return false;
  return true;
}

static bool is_done(const uint8_t *p, uint8_t op) {
  return is_ready_done(p, PKT_DONE, op) && zero_from(p, REPLY_REQUEST_ID + 1);
}

static bool is_unstable(const uint8_t *p) {
  return memcmp(p, UNSTABLE_MAGIC, sizeof(UNSTABLE_MAGIC)) == 0 &&
         zero_from(p, sizeof(UNSTABLE_MAGIC));
}

static const char *op_name(uint8_t op, char fallback[8]) {
  switch (op) {
  case OP_PING: return "PING";
  case OP_GET_INFO: return "GET_INFO";
  case OP_CART_STATUS: return "CART_STATUS";
  case OP_SESSION_BEGIN: return "SESSION_BEGIN";
  case OP_SESSION_END: return "SESSION_END";
  case OP_ROM_READ: return "ROM_READ";
  case OP_ROM_CRC32: return "ROM_CRC32";
  case OP_SAVE_READ: return "SAVE_READ";
  case OP_SAVE_WRITE: return "SAVE_WRITE";
  case OP_SAVE_CRC32: return "SAVE_CRC32";
  case OP_RTC_READ: return "RTC_READ";
  case OP_RTC_WRITE: return "RTC_WRITE";
  case OP_LED_SET: return "LED_SET";
  case OP_LED_RELEASE: return "LED_RELEASE";
  case OP_CAMERA_START: return "CAMERA_START";
  case OP_CAMERA_CAPTURE: return "CAMERA_CAPTURE";
  case OP_CAMERA_STOP: return "CAMERA_STOP";
  case OP_SENSOR_READ: return "SENSOR_READ";
  case OP_RUMBLE_SET: return "RUMBLE_SET";
  default:
    snprintf(fallback, 8, "0x%02x", op);
    return fallback;
  }
}

static void trace_bytes(opsdk_t *dev, opsdk_trace_kind_t kind, const uint8_t *data, size_t length) {
  if (!dev->trace) return;
  opsdk_trace_t e = {kind, dev->op, data, length, NULL};
  dev->trace(&e, dev->trace_user);
}

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void trace_event(opsdk_t *dev, const char *fmt, ...) {
  if (!dev->trace) return;
  char message[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);
  opsdk_trace_t e = {OPSDK_TRACE_EVENT, dev->op, NULL, 0, message};
  dev->trace(&e, dev->trace_user);
}

static int io_failed(opsdk_t *dev, int rc) {
  if (dev->builtin) dev->os_error = opsdk_platform_os_error(dev->context);
  return rc < 0 ? rc : OPSDK_E_IO;
}

static int read_exact(opsdk_t *dev, uint8_t *buf, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  while (got < n) {
    int r = dev->transport.read(dev->context, buf + got, n - got, timeout_ms);
    if (r < 0) return io_failed(dev, r);
    if (r == 0) break;
    got += (size_t)r;
  }
  if (got < n) {
    dev->stale = true;
    trace_event(dev, "timeout: %u of %u bytes", (unsigned)got, (unsigned)n);
    return OPSDK_E_TIMEOUT;
  }
  trace_bytes(dev, OPSDK_TRACE_RECEIVE, buf, n);
  return 0;
}

static int write_all(opsdk_t *dev, const uint8_t *buf, size_t n) {
  trace_bytes(dev, OPSDK_TRACE_SEND, buf, n);
  int rc = dev->transport.write(dev->context, buf, n);
  return rc == OPSDK_OK ? 0 : io_failed(dev, rc);
}

static int drain(opsdk_t *dev, uint32_t quiet_ms) {
  uint8_t junk[PKT_SIZE];
  unsigned long total = 0;
  for (;;) {
    int r = dev->transport.read(dev->context, junk, sizeof(junk), quiet_ms);
    if (r < 0) return io_failed(dev, r);
    if (r == 0) break;
    total += (unsigned long)r;
  }
  if (total) trace_event(dev, "drained %lu stale bytes", total);
  return 0;
}

int opsdk_abort(opsdk_t *dev) {
  if (!dev) return OPSDK_E_ARGUMENT;
  uint8_t pkt[CMD_SIZE] = {0};
  memcpy(pkt, ABORT_MAGIC, 8);
  for (int i = 0; i < 8; i++) pkt[8 + i] = (uint8_t)(ABORT_MAGIC[i] ^ 0xFF);
  trace_event(dev, "abort sent");
  return write_all(dev, pkt, sizeof(pkt));
}

/* safe to send a second time. anything else may already have run on the device */
static bool read_only(uint8_t op) {
  switch (op) {
  case OP_PING: case OP_GET_INFO: case OP_CART_STATUS: case OP_ROM_READ: case OP_ROM_CRC32:
  case OP_SAVE_READ: case OP_SAVE_CRC32: case OP_RTC_READ: case OP_SENSOR_READ:
    return true;
  default:
    return false;
  }
}

typedef struct {
  uint8_t op;
  uint8_t save_chip;
  uint32_t rom_size;
  uint32_t save_size;
  const uint8_t *args;
  size_t args_len;
  const uint8_t *raw;
  uint8_t *out;
  uint32_t expect;
  uint32_t *got;
  const uint8_t *upload;
  uint32_t upload_size;
  uint32_t done_timeout_ms;
  uint32_t skip_wait_ms;
  opsdk_progress_fn progress;
  void *user;
} xfer_t;

static int transact_once(opsdk_t *dev, const xfer_t *x, bool retried) {
  uint8_t cmd[CMD_SIZE];
  if (++dev->request_id == 0) dev->request_id = 1;
  uint8_t id = dev->request_id;
  if (x->raw) {
    memcpy(cmd, x->raw, 60);
    cmd[0] = x->op;
    cmd[REQUEST_ID_OFFSET] = id;
    seal(cmd);
  } else {
    opsdk_build_command(cmd, x->op, x->save_chip, x->rom_size, x->save_size, x->args,
                        x->args_len, id);
  }
  dev->last_error = 0;
  dev->op = x->op;
  char name[8];

  // stale bytes would be read as READY
  int rc = drain(dev, dev->stale ? STALE_QUIET_MS : 0);
  if (rc) return rc;
  dev->stale = false;

  rc = write_all(dev, cmd, sizeof(cmd));
  if (rc) return rc;

  uint8_t pkt[PKT_SIZE];
  rc = read_exact(dev, pkt, PKT_SIZE, READY_TIMEOUT_MS);
  if (rc) return rc;
  if (!is_ready_done(pkt, PKT_READY, x->op)) {
    trace_event(dev, "unexpected packet instead of READY: aborting the previous transfer");
    opsdk_abort(dev);
    rc = drain(dev, STALE_QUIET_MS);
    if (rc) return rc;
    if (!retried && read_only(x->op)) {
      trace_event(dev, "re-sending %s (read-only)", op_name(x->op, name));
      return transact_once(dev, x, true);
    }
    trace_event(dev, "not re-sending %s (it changes state)", op_name(x->op, name));
    dev->stale = true;
    return OPSDK_E_PROTOCOL;
  }

  bool done_seen = false;
  if (x->upload) {
    rc = read_exact(dev, pkt, PKT_SIZE, dev->timeout_ms);
    if (rc) return rc;
    if (is_ready_done(pkt, PKT_DONE, x->op)) {
      done_seen = true;
    } else {
      if (!is_ready_done(pkt, PKT_PROCEED, x->op)) {
        trace_event(dev, "unexpected packet instead of PROCEED");
        dev->stale = true;
        return OPSDK_E_PROTOCOL;
      }
      uint8_t chunk[CMD_SIZE];
      for (uint32_t sent = 0; sent < x->upload_size; sent += CMD_SIZE) {
        uint32_t n = x->upload_size - sent < CMD_SIZE ? x->upload_size - sent : CMD_SIZE;
        memset(chunk, 0xFF, sizeof(chunk));
        memcpy(chunk, x->upload + sent, n);
        rc = write_all(dev, chunk, sizeof(chunk));
        if (rc) return rc;
        if (x->progress && !x->progress(sent + n, x->upload_size, x->user)) {
          opsdk_abort(dev);
          break;
        }
      }
    }
  }

  uint32_t got = 0;
  bool aborted = false;
  uint32_t data_wait = x->done_timeout_ms && x->expect <= PKT_SIZE
                           ? x->done_timeout_ms
                           : dev->timeout_ms + x->skip_wait_ms;
  while (!done_seen && got < x->expect) {
    rc = read_exact(dev, pkt, PKT_SIZE, data_wait);
    if (rc) return rc;
    if (is_done(pkt, x->op)) {
      done_seen = true;
      break;
    }
    if (is_unstable(pkt)) continue;
    uint32_t n = x->expect - got < PKT_SIZE ? x->expect - got : PKT_SIZE;
    if (x->out) memcpy(x->out + got, pkt, n);
    got += n;
    if (x->progress && !aborted && !x->progress(got, x->expect, x->user)) {
      opsdk_abort(dev);
      aborted = true;
    }
  }
  if (x->got) *x->got = got;

  if (!done_seen) {
    uint32_t wait = x->done_timeout_ms ? x->done_timeout_ms : dev->timeout_ms;
    /* error packets are 64 bytes, DONE is 512 */
    for (;;) {
      rc = read_exact(dev, pkt, CMD_SIZE, wait);
      if (rc) return rc;
      if (pkt[0] == ERROR_MAGIC_1 && pkt[1] == ERROR_MAGIC_2) {
        dev->last_error = pkt[ERROR_CODE];
        trace_event(dev, "device error packet 0x%02X", pkt[ERROR_CODE]);
        continue;
      }
      rc = read_exact(dev, pkt + CMD_SIZE, PKT_SIZE - CMD_SIZE, dev->timeout_ms);
      if (rc) return rc;
      if (is_ready_done(pkt, PKT_DONE, x->op)) break;
    }
  }

  uint8_t status = pkt[REPLY_STATUS];
  if (pkt[REPLY_REQUEST_ID] != id) {
    // Old firmware acks every opcode with id 0. That is why we never send id 0.
    if (status == 0 && pkt[REPLY_REQUEST_ID] == 0) return OPSDK_UNKNOWN_COMMAND;
    if (status != OPSDK_UNKNOWN_COMMAND) {
      trace_event(dev, "request id mismatch");
      dev->stale = true;
      return OPSDK_E_PROTOCOL;
    }
  }
  trace_event(dev, "DONE status 0x%02X", status);
  if (dev->last_error && status == OPSDK_OK) return OPSDK_E_DEVICE;
  return status;
}

static int transact(opsdk_t *dev, const xfer_t *x) {
  if (!dev) return OPSDK_E_ARGUMENT;
  int rc = transact_once(dev, x, false);
  dev->op = 0;
  return rc;
}

const char *opsdk_version(void) {
  return OPSDK_VERSION;
}

static int wrap(opsdk_t **out, const opsdk_transport_t *transport, void *context, bool builtin) {
  opsdk_t *dev = calloc(1, sizeof(*dev));
  if (!dev) return OPSDK_E_NO_MEMORY;
  dev->transport = *transport;
  dev->context = context;
  dev->builtin = builtin;
  dev->timeout_ms = DEFAULT_TIMEOUT_MS;
  dev->stale = true;
  *out = dev;
  return OPSDK_OK;
}

int opsdk_open_transport(opsdk_t **out, const opsdk_transport_t *transport, void *context) {
  if (!out || !transport || !transport->read || !transport->write) return OPSDK_E_ARGUMENT;
  return wrap(out, transport, context, false);
}

int opsdk_open_fd(opsdk_t **out, int fd) {
  if (!out || fd < 0) return OPSDK_E_ARGUMENT;
  opsdk_transport_t transport;
  void *context;
  int rc = opsdk_platform_wrap_fd(fd, &transport, &context);
  if (rc != OPSDK_OK) return rc;
  rc = wrap(out, &transport, context, true);
  if (rc != OPSDK_OK) opsdk_platform_free_wrapper(context);
  return rc;
}

static int open_path(opsdk_t **out, const char *path) {
  opsdk_transport_t transport;
  void *context;
  int rc = opsdk_platform_open(path, &transport, &context);
  if (rc != OPSDK_OK) return rc;
  rc = wrap(out, &transport, context, true);
  if (rc != OPSDK_OK) transport.close(context);
  return rc;
}

int opsdk_open(opsdk_t **out, const char *path) {
  if (!out) return OPSDK_E_ARGUMENT;
  *out = NULL;
  if (path) return open_path(out, path);
  char ports[4096];
  int n = opsdk_list_ports(ports, sizeof(ports));
  bool without_sdk = false;
  for (const char *p = ports; n > 0 && *p; p += strlen(p) + 1) {
    opsdk_t *dev;
    if (open_path(&dev, p) != OPSDK_OK) continue;
    dev->timeout_ms = PROBE_TIMEOUT_MS;
    int rc = opsdk_ping(dev, 0x5A);
    if (rc == OPSDK_OK) {
      dev->timeout_ms = DEFAULT_TIMEOUT_MS;
      *out = dev;
      return OPSDK_OK;
    }
    if (rc == OPSDK_UNKNOWN_COMMAND) without_sdk = true;
    opsdk_close(dev);
  }
  return without_sdk ? OPSDK_UNKNOWN_COMMAND : OPSDK_E_NO_DEVICE;
}

void opsdk_close(opsdk_t *dev) {
  if (!dev) return;
  if (dev->transport.close) dev->transport.close(dev->context);
  free(dev);
}

int opsdk_last_os_error(const opsdk_t *dev) {
  return dev->os_error;
}

void opsdk_set_trace(opsdk_t *dev, opsdk_trace_fn fn, void *user) {
  dev->trace = fn;
  dev->trace_user = user;
}

void opsdk_set_timeout(opsdk_t *dev, uint32_t ms) {
  dev->timeout_ms = ms;
}

uint8_t opsdk_last_error(const opsdk_t *dev) {
  return dev->last_error;
}

const char *opsdk_strerror(int code) {
  switch (code) {
  case OPSDK_OK: return "ok";
  case OPSDK_UNKNOWN_COMMAND: return "unknown command (firmware without the SDK)";
  case OPSDK_UNSUPPORTED: return "not supported by this cartridge or device";
  case OPSDK_BAD_ARGUMENT: return "bad argument";
  case OPSDK_NO_CARTRIDGE: return "no cartridge";
  case OPSDK_CART_UNSTABLE: return "cartridge connection unstable";
  case OPSDK_ABORTED: return "aborted";
  case OPSDK_TIMEOUT: return "device timed out waiting for data";
  case OPSDK_NOT_PERMITTED: return "not permitted on this cartridge";
  case OPSDK_NO_SESSION: return "no session open";
  case OPSDK_CART_ERROR: return "the cartridge did not complete the operation";
  case OPSDK_E_IO: return "I/O error";
  case OPSDK_E_TIMEOUT: return "no answer from the device";
  case OPSDK_E_PROTOCOL: return "protocol error";
  case OPSDK_E_ARGUMENT: return "bad argument to the library";
  case OPSDK_E_NO_DEVICE: return "no Operator found";
  case OPSDK_E_DEVICE: return "device error packet";
  case OPSDK_E_NO_MEMORY: return "out of memory";
  default: return "unknown status";
  }
}

static uint8_t protocol_save_chip(uint8_t t) {
  switch (t) {
  case 0x40: return 0x40;
  case 0x41: return 0x02;
  case 0x42: return 0x01;
  case 0x43: return 0x03;
  default: break;
  }
  switch (t & 0xF0) {
  case 0x10: return 0x01;
  case 0x20: return 0x02;
  case 0x30: return 0x03;
  default: return 0x00;
  }
}

void opsdk_parse_signature(const uint8_t raw[OPSDK_SIGNATURE_SIZE], opsdk_cart_t *c) {
  memset(c, 0, sizeof(*c));
  memcpy(c->raw, raw, sizeof(c->raw));
  c->type_byte = raw[SIG_TYPE];
  uint8_t plat = raw[SIG_TYPE] & PLATFORM_MASK;
  switch (plat) {
  case 0x00: case 0x20: case 0x30: case 0x50: case 0x60:
    c->platform = (opsdk_platform_t)plat;
    break;
  default:
    c->platform = OPSDK_PLATFORM_UNKNOWN;
  }
  uint8_t flags = raw[SIG_TYPE] & FLAGS_MASK;
  c->flashcart = (flags & FLAG_FLASHCART) != 0;
  c->counterfeit = (flags & FLAG_COUNTERFEIT) != 0;
  c->logo_valid = raw[SIG_LOGO] != 0;
  c->checksum_valid = raw[SIG_CHECKSUM] != 0;

  uint32_t pow2 = raw[SIG_ROM_SHIFT] ? (raw[SIG_ROM_SHIFT] < 32 ? 1u << raw[SIG_ROM_SHIFT] : 0) : 0;
  /* the shift can only say powers of two. sizes in between (3 MiB, 6 MiB) come as
     odd << n in the coded byte, never above the shift size */
  uint32_t exact = 0;
  if (raw[SIG_ROM_CODED] && (raw[SIG_ROM_CODED] >> 2) < 29)
    exact = (uint32_t)(2 * (raw[SIG_ROM_CODED] & 3) + 1) << (raw[SIG_ROM_CODED] >> 2);
  if (exact && pow2 && exact > pow2) exact = pow2;
  c->rom_size = exact ? exact : pow2;

  c->save_size = raw[SIG_SAVE_SHIFT] && raw[SIG_SAVE_SHIFT] < 32 ? 1u << raw[SIG_SAVE_SHIFT] : 0;
  if (raw[SIG_SAVE_CHIP] == SAVE_CHIP_SRAM_96K) c->save_size = SAVE_SIZE_SRAM_96K;
  c->save_chip_type = raw[SIG_SAVE_CHIP];
  c->save_chip = protocol_save_chip(raw[SIG_SAVE_CHIP]);
  c->revision = raw[SIG_REVISION];
  c->region = raw[SIG_REGION];
  memcpy(c->header_bytes, &raw[SIG_HEADER], sizeof(c->header_bytes));
}

bool opsdk_same_cartridge(const opsdk_cart_t *a, const opsdk_cart_t *b) {
  return a->type_byte == b->type_byte && a->rom_size == b->rom_size &&
         a->save_size == b->save_size && a->save_chip_type == b->save_chip_type &&
         a->revision == b->revision && a->region == b->region &&
         memcmp(a->header_bytes, b->header_bytes, sizeof(a->header_bytes)) == 0;
}

int opsdk_ping(opsdk_t *dev, uint8_t challenge) {
  uint8_t reply[1];
  uint32_t got = 0;
  xfer_t x = {.op = OP_PING, .args = &challenge, .args_len = 1, .out = reply, .expect = 1,
              .got = &got};
  int rc = transact(dev, &x);
  if (rc != OPSDK_OK) return rc;
  if (got == 0) return OPSDK_UNKNOWN_COMMAND;
  const uint8_t inverse = (uint8_t)~challenge;
  return reply[0] == inverse ? OPSDK_OK : OPSDK_E_PROTOCOL;
}

int opsdk_get_info(opsdk_t *dev, opsdk_info_t *info) {
  if (!info) return OPSDK_E_ARGUMENT;
  uint8_t raw[PKT_SIZE];
  uint32_t got = 0;
  xfer_t x = {.op = OP_GET_INFO, .out = raw, .expect = PKT_SIZE, .got = &got};
  int rc = transact(dev, &x);
  if (rc != OPSDK_OK) return rc;
  if (got < INFO_SIZE || memcmp(raw, "OPSD", 4) != 0) return OPSDK_UNKNOWN_COMMAND;
  memset(info, 0, sizeof(*info));
  info->protocol_version = (uint16_t)(raw[INFO_PROTOCOL] | raw[INFO_PROTOCOL + 1] << 8);
  info->device_id = raw[INFO_DEVICE_ID];
  info->board_revision = raw[INFO_BOARD];
  memcpy(info->app_version, &raw[INFO_APP_VERSION], 3);
  memcpy(info->sys_version, &raw[INFO_SYS_VERSION], 3);
  info->capabilities = get_u32(&raw[INFO_CAPABILITIES]);
  memcpy(info->serial, &raw[INFO_SERIAL], sizeof(info->serial) - 1);
  info->session_idle_timeout_s = (uint16_t)(raw[INFO_IDLE_TIMEOUT] | raw[INFO_IDLE_TIMEOUT + 1] << 8);
  return OPSDK_OK;
}

bool opsdk_has(const opsdk_info_t *info, uint8_t opcode) {
  return opcode >= 0xA0 && opcode <= 0xBF && (info->capabilities >> (opcode - 0xA0)) & 1u;
}

static int cart_status(opsdk_t *dev, opsdk_cart_t *cart, bool in_session) {
  uint8_t raw[PKT_SIZE];
  uint8_t flags = in_session ? CART_STATUS_REQUIRE_SESSION : 0;
  uint32_t got = 0;
  xfer_t x = {.op = OP_CART_STATUS, .args = &flags, .args_len = 1, .out = raw, .expect = PKT_SIZE,
              .got = &got};
  int rc = transact(dev, &x);
  if (rc == OPSDK_OK && got != PKT_SIZE) rc = OPSDK_E_PROTOCOL;
  if (cart) {
    if (rc == OPSDK_OK) opsdk_parse_signature(raw, cart);
    else memset(cart, 0, sizeof(*cart));
  }
  return rc;
}

int opsdk_cart_status(opsdk_t *dev, opsdk_cart_t *cart) {
  return cart_status(dev, cart, false);
}

int opsdk_session_begin(opsdk_t *dev, opsdk_cart_t *cart) {
  if (cart) memset(cart, 0, sizeof(*cart));
  xfer_t begin = {.op = OP_SESSION_BEGIN};
  int rc = transact(dev, &begin);
  if (rc != OPSDK_OK) return rc;

  rc = cart_status(dev, cart, true);
  if (rc != OPSDK_OK) opsdk_session_end(dev);
  return rc;
}

int opsdk_session_end(opsdk_t *dev) {
  xfer_t x = {.op = OP_SESSION_END};
  return transact(dev, &x);
}

static bool range_ok(uint32_t offset, uint32_t length) {
  return length && offset % OPSDK_ALIGN == 0 && length % OPSDK_ALIGN == 0 &&
         offset + length >= offset;
}

static int stream(opsdk_t *dev, uint8_t op, const opsdk_cart_t *cart, uint32_t offset,
                  uint32_t length, void *buf, uint32_t *crc, opsdk_progress_fn progress,
                  void *user) {
  if (!cart || !range_ok(offset, length)) return OPSDK_E_ARGUMENT;
  uint8_t args[8];
  put_u32(&args[0], offset);
  put_u32(&args[4], length);
  bool is_crc = crc != NULL;
  /* the device is silent while it seeks to offset (and for a whole CRC),
     ~10 ms per KiB */
  uint8_t crc_raw[4] = {0};
  uint32_t got = 0;
  xfer_t x = {.op = op, .save_chip = cart->save_chip, .rom_size = cart->rom_size,
              .save_size = cart->save_size, .args = args, .args_len = sizeof(args),
              .out = is_crc ? crc_raw : buf, .expect = is_crc ? 4 : length, .got = &got,
              .done_timeout_ms =
                  is_crc ? CRC_BASE_MS + (uint32_t)(((uint64_t)offset + length + 1023) / 1024) * CRC_MS_PER_KIB : 0,
              .skip_wait_ms = (uint32_t)(((uint64_t)offset + 1023) / 1024) * CRC_MS_PER_KIB,
              .progress = progress, .user = user};
  int rc = transact(dev, &x);
  if (rc != OPSDK_OK) return rc;
  if (got < x.expect) return OPSDK_E_PROTOCOL;
  if (is_crc) *crc = get_u32(crc_raw);
  return OPSDK_OK;
}

int opsdk_rom_read(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                   void *buf, opsdk_progress_fn progress, void *user) {
  if (!buf) return OPSDK_E_ARGUMENT;
  return stream(dev, OP_ROM_READ, cart, offset, length, buf, NULL, progress, user);
}

int opsdk_rom_crc32(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                    uint32_t *crc) {
  if (!crc) return OPSDK_E_ARGUMENT;
  return stream(dev, OP_ROM_CRC32, cart, offset, length, NULL, crc, NULL, NULL);
}

/* saves smaller than one packet (MBC7, TAMA5) cannot be asked for in 512s: allow an
   unaligned length when it runs exactly to the end of the save */
static bool save_tail(const opsdk_cart_t *cart, uint32_t offset, uint32_t length) {
  return cart && length % OPSDK_ALIGN && offset % OPSDK_ALIGN == 0 && offset < cart->save_size &&
         length == cart->save_size - offset;
}

int opsdk_save_read(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                    void *buf) {
  if (!buf) return OPSDK_E_ARGUMENT;
  if (!save_tail(cart, offset, length))
    return stream(dev, OP_SAVE_READ, cart, offset, length, buf, NULL, NULL, NULL);
  uint32_t aligned = (length / OPSDK_ALIGN + 1) * OPSDK_ALIGN;
  uint8_t *whole = malloc(aligned);
  if (!whole) return OPSDK_E_NO_MEMORY;
  int rc = stream(dev, OP_SAVE_READ, cart, offset, aligned, whole, NULL, NULL, NULL);
  if (rc == OPSDK_OK) memcpy(buf, whole, length);
  free(whole);
  return rc;
}

int opsdk_save_crc32(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                     uint32_t *crc) {
  if (!crc) return OPSDK_E_ARGUMENT;
  if (!save_tail(cart, offset, length))
    return stream(dev, OP_SAVE_CRC32, cart, offset, length, NULL, crc, NULL, NULL);
  uint8_t *data = malloc(length);
  if (!data) return OPSDK_E_NO_MEMORY;
  int rc = opsdk_save_read(dev, cart, offset, length, data);
  if (rc == OPSDK_OK) *crc = opsdk_crc32(0, data, length);
  free(data);
  return rc;
}

int opsdk_save_write(opsdk_t *dev, const opsdk_cart_t *cart, const void *data, uint32_t size,
                     opsdk_progress_fn progress, void *user) {
  if (!cart || !data || !size) return OPSDK_E_ARGUMENT;
  xfer_t x = {.op = OP_SAVE_WRITE, .save_chip = cart->save_chip, .rom_size = cart->rom_size,
              .save_size = size, .upload = data, .upload_size = size,
              .done_timeout_ms = DONE_WRITE_TIMEOUT_MS, .progress = progress, .user = user};
  return transact(dev, &x);
}

int opsdk_rtc_read(opsdk_t *dev, const opsdk_cart_t *cart, uint8_t raw[OPSDK_RTC_SIZE]) {
  if (!raw) return OPSDK_E_ARGUMENT;
  uint8_t pkt[PKT_SIZE] = {0};
  uint32_t got = 0;
  xfer_t x = {.op = OP_RTC_READ, .save_chip = cart ? cart->save_chip : 0, .out = pkt,
              .expect = PKT_SIZE, .got = &got};
  int rc = transact(dev, &x);
  memcpy(raw, pkt, OPSDK_RTC_SIZE);
  if (rc == OPSDK_OK && got == 0) return OPSDK_UNSUPPORTED;
  return rc;
}

int opsdk_rtc_write(opsdk_t *dev, const opsdk_cart_t *cart, const uint8_t raw[OPSDK_RTC_SIZE]) {
  if (!raw) return OPSDK_E_ARGUMENT;
  xfer_t x = {.op = OP_RTC_WRITE, .save_chip = cart ? cart->save_chip : 0, .upload = raw,
              .upload_size = OPSDK_RTC_SIZE, .done_timeout_ms = RTC_WRITE_TIMEOUT_MS};
  return transact(dev, &x);
}

static uint8_t rtc4513_pair(const uint8_t *reg, int index, uint8_t tens_mask) {
  return (uint8_t)((reg[index] & 0x0F) + ((reg[index + 1] & 0x0F) & tens_mask) * 10);
}

static bool rtc4513_in_range(const opsdk_rtc4513_t *c) {
  return c->year <= 99 && c->month >= 1 && c->month <= 12 && c->day >= 1 && c->day <= 31 &&
         c->weekday <= 6 && c->hour <= 23 && c->minute <= 59 && c->second <= 59;
}

int opsdk_rtc4513_decode(const uint8_t raw[OPSDK_RTC_SIZE], opsdk_rtc4513_t *clock) {
  if (!raw || !clock || raw[0] != SN_RTC_TYPE_RTC4513) return OPSDK_E_ARGUMENT;
  const uint8_t *reg = raw + 1;
  uint8_t control_f = reg[RTC4513_CONTROL_F] & 0x0F;
  bool hour12 = !(control_f & RTC4513_24_HOUR);
  bool pm = reg[RTC4513_HOUR + 1] & RTC4513_PM;
  uint8_t counted = rtc4513_pair(reg, RTC4513_HOUR, hour12 ? 0x01 : 0x03);
  memset(clock, 0, sizeof(*clock));
  clock->year = rtc4513_pair(reg, RTC4513_YEAR, 0x0F);
  clock->month = rtc4513_pair(reg, RTC4513_MONTH, 0x01);
  clock->day = rtc4513_pair(reg, RTC4513_DAY, 0x03);
  clock->weekday = reg[RTC4513_WEEKDAY] & 0x07;
  clock->hour = hour12 ? (uint8_t)(counted % 12 + (pm ? 12 : 0)) : counted;
  clock->minute = rtc4513_pair(reg, RTC4513_MINUTE, 0x07);
  clock->second = rtc4513_pair(reg, RTC4513_SECOND, 0x07);
  clock->hour12 = hour12;
  clock->stopped = control_f & RTC4513_STOP;
  clock->calendar = reg[RTC4513_CONTROL_D] & RTC4513_CALENDAR;
  clock->lost_power = reg[RTC4513_SECOND + 1] & RTC4513_LOST_POWER;
  static const uint8_t ones[] = {RTC4513_SECOND, RTC4513_MINUTE, RTC4513_HOUR, RTC4513_DAY,
                                 RTC4513_MONTH,  RTC4513_YEAR,   RTC4513_YEAR + 1};
  bool valid = rtc4513_in_range(clock) && (!hour12 || (counted >= 1 && counted <= 12));
  for (size_t i = 0; i < sizeof(ones); i++) valid = valid && (reg[ones[i]] & 0x0F) <= 9;
  clock->valid = valid;
  return OPSDK_OK;
}

static void rtc4513_put(uint8_t *reg, int index, uint8_t value) {
  reg[index] = value % 10;
  reg[index + 1] = value / 10;
}

int opsdk_rtc4513_encode(const opsdk_rtc4513_t *clock, uint8_t raw[OPSDK_RTC_SIZE]) {
  if (!clock || !raw || !rtc4513_in_range(clock)) return OPSDK_E_ARGUMENT;
  memset(raw, 0, OPSDK_RTC_SIZE);
  rtc4513_put(raw, RTC4513_SECOND, clock->second);
  rtc4513_put(raw, RTC4513_MINUTE, clock->minute);
  uint8_t counted = clock->hour;
  if (clock->hour12) counted = clock->hour % 12 ? clock->hour % 12 : 12;
  rtc4513_put(raw, RTC4513_HOUR, counted);
  if (clock->hour12 && clock->hour >= 12) raw[RTC4513_HOUR + 1] |= RTC4513_PM;
  rtc4513_put(raw, RTC4513_DAY, clock->day);
  rtc4513_put(raw, RTC4513_MONTH, clock->month);
  rtc4513_put(raw, RTC4513_YEAR, clock->year);
  raw[RTC4513_WEEKDAY] = clock->weekday;
  raw[RTC4513_CONTROL_D] = RTC4513_CALENDAR;
  raw[RTC4513_CONTROL_E] = RTC4513_INTERRUPT_MASKED;
  raw[RTC4513_CONTROL_F] =
      (uint8_t)((clock->hour12 ? 0 : RTC4513_24_HOUR) | (clock->stopped ? RTC4513_STOP : 0));
  return OPSDK_OK;
}

void opsdk_build_led(uint8_t block[32], uint8_t id, const opsdk_led_t *led) {
  memset(block, 0, 32);
  block[0] = id;
  const uint8_t *colors[2] = {led->primary, led->secondary};
  for (int c = 0; c < 2; c++) {
    double lin[3], sum = 0;
    for (int i = 0; i < 3; i++) {
      lin[i] = pow(colors[c][i] / 255.0, 2.2);
      sum += lin[i];
    }
    uint8_t *f = &block[LED_COLORS + c * 12];
    for (int i = 0; i < 4; i++) {
      double v = i < 3 ? lin[i] : pow(sum, 1 / 2.2);
      uint32_t fx = (uint32_t)(v * 65536.0);
      f[i * 3 + 0] = (uint8_t)fx;
      f[i * 3 + 1] = (uint8_t)(fx >> 8);
      f[i * 3 + 2] = (uint8_t)(fx >> 16);
    }
  }
  block[LED_COLOR_EFFECT] = led->color_effect;
  block[LED_COLOR_DUTY] = led->color_duty;
  block[LED_LIGHT_EFFECT] = led->light_effect;
  block[LED_LIGHT_DUTY] = led->light_duty;
  block[LED_OFFSET] = led->offset;
  block[LED_PERIOD] = (uint8_t)led->period_ms;
  block[LED_PERIOD + 1] = (uint8_t)(led->period_ms >> 8);
}

int opsdk_led_set(opsdk_t *dev, const opsdk_led_t *led) {
  if (!led) return OPSDK_E_ARGUMENT;
  uint8_t block[32];
  opsdk_build_led(block, OP_LED_SET, led);
  xfer_t x = {.op = OP_LED_SET, .args = block, .args_len = sizeof(block)};
  return transact(dev, &x);
}

int opsdk_led_release(opsdk_t *dev) {
  xfer_t x = {.op = OP_LED_RELEASE};
  return transact(dev, &x);
}

void opsdk_camera_defaults(opsdk_camera_t *s) {
  memset(s, 0, sizeof(*s));
  s->exposure = 0x1000;
  s->edge_ratio = 0x40;
  s->vref = 0x03;
  s->zero_calibration = true;
  static const uint8_t layout[16] = {0, 30, 24, 6, 15, 45, 39, 21, 12, 42, 36, 18, 3, 33, 27, 9};
  uint8_t q[4];
  for (int i = 0; i < 4; i++) {
    int v = CAMERA_MATRIX_OFFSET + i * CAMERA_MATRIX_SLOPE;
    q[i] = (uint8_t)(v < 256 ? v : 255);
  }
  for (int comp = 0; comp < 3; comp++) {
    uint16_t acc = (uint16_t)(q[comp] << 4), inc = (uint16_t)(q[comp + 1] - q[comp]);
    for (int px = 0; px < 16; px++) {
      s->matrix[comp + layout[px]] = (uint8_t)(acc >> 4);
      acc = (uint16_t)(acc + inc);
    }
  }
}

int opsdk_camera_start(opsdk_t *dev) {
  xfer_t x = {.op = OP_CAMERA_START};
  return transact(dev, &x);
}

int opsdk_camera_capture(opsdk_t *dev, const opsdk_camera_t *s,
                         uint8_t frame[OPSDK_CAMERA_FRAME_SIZE]) {
  if (!s || !frame) return OPSDK_E_ARGUMENT;
  uint8_t body[60] = {0};
  body[CAM_EXPOSURE] = (uint8_t)s->exposure;
  body[CAM_EXPOSURE + 1] = (uint8_t)(s->exposure >> 8);
  body[CAM_GAIN] = s->gain;
  body[CAM_VERTICAL_EDGE] = s->vertical_edge;
  body[CAM_EDGE_OPERATION] = s->edge_operation;
  body[CAM_EDGE_RATIO] = s->edge_ratio;
  body[CAM_INVERT] = s->invert;
  body[CAM_VREF] = s->vref;
  body[CAM_ZERO_CALIBRATION] = s->zero_calibration;
  body[CAM_OUTPUT_REF] = s->output_ref_voltage;
  memcpy(&body[CAM_MATRIX], s->matrix, sizeof(s->matrix));
  uint32_t got = 0;
  xfer_t x = {.op = OP_CAMERA_CAPTURE, .raw = body, .out = frame,
              .expect = OPSDK_CAMERA_FRAME_SIZE, .got = &got};
  int rc = transact(dev, &x);
  if (rc == OPSDK_OK && got < OPSDK_CAMERA_FRAME_SIZE) return OPSDK_E_PROTOCOL;
  return rc;
}

int opsdk_camera_stop(opsdk_t *dev) {
  xfer_t x = {.op = OP_CAMERA_STOP};
  return transact(dev, &x);
}

int opsdk_sensor_read(opsdk_t *dev, opsdk_sensor_t *sensor) {
  if (!sensor) return OPSDK_E_ARGUMENT;
  uint8_t raw[PKT_SIZE];
  uint32_t got = 0;
  xfer_t x = {.op = OP_SENSOR_READ, .out = raw, .expect = PKT_SIZE, .got = &got};
  int rc = transact(dev, &x);
  if (rc != OPSDK_OK) return rc;
  if (got < SENSOR_SIZE) return OPSDK_E_PROTOCOL;
  sensor->kind = raw[SENSOR_KIND];
  sensor->x = (uint16_t)(raw[SENSOR_X] | raw[SENSOR_X + 1] << 8);
  sensor->y = (uint16_t)(raw[SENSOR_Y] | raw[SENSOR_Y + 1] << 8);
  sensor->z = (uint16_t)(raw[SENSOR_Z] | raw[SENSOR_Z + 1] << 8);
  return OPSDK_OK;
}

int opsdk_rumble_set(opsdk_t *dev, bool on) {
  uint8_t v = on ? 1 : 0;
  xfer_t x = {.op = OP_RUMBLE_SET, .args = &v, .args_len = 1};
  return transact(dev, &x);
}
