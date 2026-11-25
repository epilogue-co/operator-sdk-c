#include "fake_device.h"

#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

uint8_t fake_rom[FAKE_ROM_SIZE] = {0};
uint8_t fake_save[FAKE_SAVE_SIZE] = {0};

void fake_init_data(void) {
  for (size_t i = 0; i < sizeof(fake_rom); i++) fake_rom[i] = (uint8_t)(i * 7 + (i >> 9));
  for (size_t i = 0; i < sizeof(fake_save); i++) fake_save[i] = (uint8_t)(i ^ 0xA5);
  /* rom contents that start like control packets must still arrive as data */
  static const uint8_t unstable_like[8] = {0x55, 0xDE, 0xC0, 0xDE, 0xDB, 0xA1, 0x1A, 0xD5};
  static const uint8_t done_like[5] = {0xC0, 0xDE, 0x01, 0xA8, 0x57};
  memcpy(fake_rom + 1024, unstable_like, sizeof(unstable_like));
  memcpy(fake_rom + 2048, done_like, sizeof(done_like));
}

struct fake_device {
  fake_options_t o;
  uint8_t save[FAKE_SAVE_SIZE];
  bool session;
  int commands;
  int counts[256];
  uint8_t in[64];
  size_t in_len;
  bool uploading;
  uint8_t upload_op, upload_id;
  uint32_t upload_size, upload_got;
  uint8_t *out;
  size_t out_len, out_cap, out_pos;
};

static uint32_t mpeg2(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint32_t)d[i] << 24;
    for (int b = 0; b < 8; b++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
  }
  return c;
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void emit(fake_device_t *f, const uint8_t *b, size_t n) {
  if (f->out_len + n > f->out_cap) {
    size_t cap = f->out_cap ? f->out_cap : 4096;
    while (cap < f->out_len + n) cap *= 2;
    uint8_t *grown = realloc(f->out, cap);
    if (!grown) abort();
    f->out = grown;
    f->out_cap = cap;
  }
  memcpy(f->out + f->out_len, b, n);
  f->out_len += n;
}

static void send_rd(fake_device_t *f, uint8_t type, uint8_t op, uint8_t status, uint8_t id) {
  uint8_t p[512] = {0xC0, 0xDE, type, op, (uint8_t)~op, status, id};
  emit(f, p, sizeof(p));
}

static bool is_abort(const uint8_t *p) {
  static const char magic[8] = {'E', 'P', 'I', 'A', 'B', 'O', 'R', 'T'};
  for (int i = 0; i < 8; i++) {
    const uint8_t inverse = (uint8_t)~(uint8_t)magic[i];
    if (p[i] != (uint8_t)magic[i] || p[8 + i] != inverse) return false;
  }
  return true;
}

static void upload_packet(fake_device_t *f, const uint8_t *chunk) {
  uint8_t status = 0;
  if (is_abort(chunk)) {
    status = 0x06;
  } else {
    uint32_t p = f->upload_got, left = f->upload_size - p;
    memcpy(f->save + p, chunk, left < 64 ? left : 64);
    f->upload_got += 64;
    if (f->upload_got < f->upload_size) return;
    if (f->save[0] == 0xEE) {
      uint8_t e[64] = {0x55, 0xAA, 0x48};
      emit(f, e, sizeof(e));
    }
  }
  f->uploading = false;
  send_rd(f, 0x01, f->upload_op, status, f->upload_id);
}

static void command(fake_device_t *f, const uint8_t *cmd) {
  if (is_abort(cmd)) return;
  f->commands++;
  uint8_t op = le32(&cmd[0x3C]) == mpeg2(cmd, 0x3C) ? cmd[0] : 0xFE;
  uint8_t id = cmd[0x3B];
  f->counts[op]++;
  for (int i = 0; i < 4; i++) {
    if (f->o.stale_before[i] == f->commands) {
      uint8_t junk[512];
      memset(junk, 0x42, sizeof(junk));
      emit(f, junk, sizeof(junk));
    }
  }
  send_rd(f, 0x00, op, 0, 0);
  if (!f->o.sdk) {
    send_rd(f, 0x01, op, 0, 0);
    return;
  }
  uint8_t pkt[512] = {0}, status = 0;
  uint32_t off = le32(&cmd[0x0A]), len = le32(&cmd[0x0E]);
  switch (op) {
  case 0xA0:
    pkt[0] = (uint8_t)~cmd[0x0A];
    emit(f, pkt, 512);
    break;
  case 0xA1:
    memcpy(pkt, "OPSD", 4);
    pkt[4] = 1;
    pkt[6] = 2;
    pkt[7] = 1;
    pkt[8] = 10;
    pkt[0x0E] = 0x37;
    pkt[0x0F] = 0x03;
    pkt[0x10] = 0x1E;
    pkt[0x11] = 0x02;
    memcpy(&pkt[0x12], "SIMULATED0001", 13);
    pkt[0x22] = 30;
    emit(f, pkt, 512);
    break;
  case 0xA4:
    if (f->o.platform) f->session = true;
    else status = 0x04;
    if (f->o.pulled_after_begin) f->o.platform = 0;
    if (f->o.expires_after_begin) f->session = false;
    break;
  case 0xA2:
    if (!f->o.platform) f->session = false;
    if (f->o.status_fails) {
      status = 0x06;
      break;
    }
    if (cmd[0x0A] == 1 && !f->o.platform) {
      status = 0x04;
      break;
    }
    if (cmd[0x0A] == 1 && !f->session) {
      status = 0x0B;
      break;
    }
    pkt[2] = f->o.platform;
    pkt[3] = pkt[4] = f->o.platform ? 1 : 0;
    pkt[5] = f->o.platform ? 16 : 0;
    pkt[6] = 0;
    for (uint32_t size = f->o.platform ? f->o.save_size : 0; size > 1; size >>= 1) pkt[6]++;
    pkt[7] = 0x21;
    emit(f, pkt, 512);
    break;
  case 0xA5:
    status = f->session ? 0 : 0x0B;
    f->session = false;
    break;
  case 0xA8:
    if (off + len > sizeof(fake_rom) || len % 512 || off % 512) {
      status = 0x03;
      break;
    }
    for (uint32_t p = off; p < off + len; p += 512) {
      if (p == off + 512 && len > 1024) {
        static const uint8_t m[8] = {0x55, 0xDE, 0xC0, 0xDE, 0xDB, 0xA1, 0x1A, 0xD5};
        uint8_t u[512] = {0};
        memcpy(u, m, 8);
        emit(f, u, 512);
      }
      emit(f, fake_rom + p, 512);
    }
    break;
  case 0xA9:
  case 0xB3: {
    const uint8_t *src = op == 0xA9 ? fake_rom : f->save;
    uint32_t c = opsdk_crc32(0, src + off, len);
    pkt[0] = (uint8_t)c;
    pkt[1] = (uint8_t)(c >> 8);
    pkt[2] = (uint8_t)(c >> 16);
    pkt[3] = (uint8_t)(c >> 24);
    emit(f, pkt, 512);
    break;
  }
  case 0xB1:
    for (uint32_t p = off; p < off + len; p += 512) {
      uint8_t block[512] = {0};
      if (p < f->o.save_size)
        memcpy(block, f->save + p, f->o.save_size - p < 512 ? f->o.save_size - p : 512);
      emit(f, block, 512);
    }
    break;
  case 0xB4:
    emit(f, pkt, 512);
    status = f->o.rtc_status;
    break;
  case 0xB2:
    if (!f->session) {
      status = 0x0B;
      break;
    }
    send_rd(f, 0x02, op, 0, 0);
    f->uploading = true;
    f->upload_op = op;
    f->upload_id = id;
    f->upload_size = le32(&cmd[6]);
    f->upload_got = 0;
    return;
  case 0xB9:
    break;
  default:
    status = 0x01;
  }
  send_rd(f, 0x01, op, status, id);
}

static void input(fake_device_t *f, const uint8_t *b, size_t n) {
  while (n) {
    size_t k = 64 - f->in_len < n ? 64 - f->in_len : n;
    memcpy(f->in + f->in_len, b, k);
    f->in_len += k;
    b += k;
    n -= k;
    if (f->in_len == 64) {
      f->in_len = 0;
      if (f->uploading) upload_packet(f, f->in);
      else command(f, f->in);
    }
  }
}

static size_t output(fake_device_t *f, uint8_t *buf, size_t n) {
  size_t avail = f->out_len - f->out_pos;
  if (n > avail) n = avail;
  if (n == 0) return 0;
  memcpy(buf, f->out + f->out_pos, n);
  f->out_pos += n;
  if (f->out_pos == f->out_len) f->out_pos = f->out_len = 0;
  return n;
}

fake_device_t *fake_create(const fake_options_t *options) {
  fake_device_t *f = calloc(1, sizeof(*f));
  if (!f) abort();
  f->o = *options;
  if (!f->o.save_size) f->o.save_size = FAKE_SAVE_SIZE;
  memcpy(f->save, fake_save, sizeof(f->save));
  return f;
}

void fake_destroy(fake_device_t *f) {
  if (!f) return;
  free(f->out);
  free(f);
}

int fake_count(const fake_device_t *f, uint8_t op) {
  return f->counts[op];
}

void fake_set_platform(fake_device_t *f, uint8_t platform) {
  f->o.platform = platform;
}

static int t_read(void *context, uint8_t *buf, size_t n, uint32_t timeout_ms) {
  (void)timeout_ms;
  return (int)output(context, buf, n);
}

static int t_write(void *context, const uint8_t *buf, size_t n) {
  input(context, buf, n);
  return OPSDK_OK;
}

static const opsdk_transport_t FAKE_TRANSPORT = {t_read, t_write, NULL};

const opsdk_transport_t *fake_transport(void) {
  return &FAKE_TRANSPORT;
}

#if !defined(_WIN32)
pid_t fake_spawn(int fd, int host_fd, const fake_options_t *options) {
  pid_t pid = fork();
  if (pid < 0) abort();
  if (pid > 0) return pid;
  if (host_fd >= 0) close(host_fd);
  fake_device_t *f = fake_create(options);
  uint8_t buf[4096];
  for (;;) {
    ssize_t k = read(fd, buf, sizeof(buf));
    if (k <= 0) break;
    input(f, buf, (size_t)k);
    while (f->out_len > f->out_pos) {
      ssize_t w = write(fd, f->out + f->out_pos, f->out_len - f->out_pos);
      if (w <= 0) _exit(3);
      f->out_pos += (size_t)w;
    }
    f->out_pos = f->out_len = 0;
  }
  fake_destroy(f);
  _exit(0);
}
#endif
