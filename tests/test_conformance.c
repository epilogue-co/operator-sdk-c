#include "opsdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char word[16];
  char *value;
} item_t;

typedef struct {
  item_t *items;
  size_t count, cursor;
  uint8_t *queue;
  size_t queue_len, queue_pos, queue_cap;
  char mismatch[160];
} replay_t;

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void queue_hex(replay_t *r, const char *hex) {
  size_t n = strlen(hex) / 2;
  if (r->queue_len + n > r->queue_cap) {
    size_t cap = r->queue_cap ? r->queue_cap : 4096;
    while (cap < r->queue_len + n) cap *= 2;
    uint8_t *q = realloc(r->queue, cap);
    if (!q) abort();
    r->queue = q;
    r->queue_cap = cap;
  }
  for (size_t i = 0; i < n; i++) {
    r->queue[r->queue_len++] = (uint8_t)(hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]));
  }
}

static int t_read(void *context, uint8_t *buf, size_t n, uint32_t timeout_ms) {
  (void)timeout_ms;
  replay_t *r = context;
  size_t avail = r->queue_len - r->queue_pos;
  if (n > avail) n = avail;
  if (n == 0) return 0;
  memcpy(buf, r->queue + r->queue_pos, n);
  r->queue_pos += n;
  if (r->queue_pos == r->queue_len) r->queue_pos = r->queue_len = 0;
  return (int)n;
}

static int t_write(void *context, const uint8_t *buf, size_t n) {
  replay_t *r = context;
  item_t *it = r->cursor < r->count ? &r->items[r->cursor] : NULL;
  bool same = it && strcmp(it->word, ">") == 0 && strlen(it->value) == 2 * n;
  for (size_t i = 0; same && i < n; i++) {
    same = hexval(it->value[2 * i]) == (buf[i] >> 4) && hexval(it->value[2 * i + 1]) == (buf[i] & 15);
  }
  if (!same) {
    if (!r->mismatch[0]) {
      snprintf(r->mismatch, sizeof(r->mismatch), "expected %s %.24s..., host sent %02x%02x%02x%02x...",
               it ? it->word : "nothing", it ? it->value : "", buf[0], n > 1 ? buf[1] : 0,
               n > 2 ? buf[2] : 0, n > 3 ? buf[3] : 0);
    }
    return OPSDK_E_IO;
  }
  r->cursor++;
  while (r->cursor < r->count && strcmp(r->items[r->cursor].word, "<") == 0) {
    queue_hex(r, r->items[r->cursor++].value);
  }
  return OPSDK_OK;
}

static const char *status_name(int rc) {
  switch (rc) {
  case OPSDK_UNKNOWN_COMMAND: return "status UNKNOWN_COMMAND";
  case OPSDK_UNSUPPORTED: return "status UNSUPPORTED";
  case OPSDK_BAD_ARGUMENT: return "status BAD_ARGUMENT";
  case OPSDK_NO_CARTRIDGE: return "status NO_CARTRIDGE";
  case OPSDK_CART_UNSTABLE: return "status CART_UNSTABLE";
  case OPSDK_ABORTED: return "status ABORTED";
  case OPSDK_TIMEOUT: return "status TIMEOUT";
  case OPSDK_NOT_PERMITTED: return "status NOT_PERMITTED";
  case OPSDK_NO_SESSION: return "status NO_SESSION";
  case OPSDK_CART_ERROR: return "status CART_ERROR";
  case OPSDK_E_IO: return "error IO";
  case OPSDK_E_TIMEOUT: return "error TIMEOUT";
  case OPSDK_E_PROTOCOL: return "error PROTOCOL";
  case OPSDK_E_ARGUMENT: return "error ARGUMENT";
  case OPSDK_E_DEVICE: return "error DEVICE";
  case OPSDK_E_NO_DEVICE: return "error NO_DEVICE";
  case OPSDK_E_NO_MEMORY: return "error NO_MEMORY";
  default: return "status ?";
  }
}

static void cart_fields(char *out, size_t size, const opsdk_cart_t *c) {
  snprintf(out, size, "ok platform=0x%x rom_size=%u save_size=%u", (unsigned)c->platform,
           (unsigned)c->rom_size, (unsigned)c->save_size);
}

static void run_call(opsdk_t *dev, opsdk_cart_t *cart, const char *line, char *out, size_t size) {
  char name[32] = "";
  unsigned long a[3] = {0, 0, 0};
  int nargs = 0;
  char buf[96];
  snprintf(buf, sizeof(buf), "%s", line);
  char *tok = strtok(buf, " ");
  if (tok) snprintf(name, sizeof(name), "%s", tok);
  while ((tok = strtok(NULL, " ")) != NULL && nargs < 3) a[nargs++] = strtoul(tok, NULL, 0);

  int rc;
  if (strcmp(name, "ping") == 0) {
    rc = opsdk_ping(dev, (uint8_t)a[0]);
    if (rc == OPSDK_OK) snprintf(out, size, "ok");
  } else if (strcmp(name, "info") == 0) {
    opsdk_info_t info;
    rc = opsdk_get_info(dev, &info);
    if (rc == OPSDK_OK) {
      snprintf(out, size, "ok serial=%s protocol=%u device_id=%u", info.serial,
               (unsigned)info.protocol_version, (unsigned)info.device_id);
    }
  } else if (strcmp(name, "cart_status") == 0 || strcmp(name, "session_begin") == 0) {
    rc = name[0] == 'c' ? opsdk_cart_status(dev, cart) : opsdk_session_begin(dev, cart);
    if (rc == OPSDK_OK) cart_fields(out, size, cart);
  } else if (strcmp(name, "session_end") == 0) {
    rc = opsdk_session_end(dev);
    if (rc == OPSDK_OK) snprintf(out, size, "ok");
  } else if (strcmp(name, "rom_read") == 0 || strcmp(name, "save_read") == 0) {
    uint8_t *data = malloc(a[1] ? a[1] : 1);
    if (!data) abort();
    rc = name[0] == 'r' ? opsdk_rom_read(dev, cart, (uint32_t)a[0], (uint32_t)a[1], data, NULL, NULL)
                        : opsdk_save_read(dev, cart, (uint32_t)a[0], (uint32_t)a[1], data);
    if (rc == OPSDK_OK) {
      snprintf(out, size, "ok length=%lu crc32=%08x", a[1], (unsigned)opsdk_crc32(0, data, a[1]));
    }
    free(data);
  } else if (strcmp(name, "rom_crc32") == 0 || strcmp(name, "save_crc32") == 0) {
    uint32_t crc = 0;
    rc = name[0] == 'r' ? opsdk_rom_crc32(dev, cart, (uint32_t)a[0], (uint32_t)a[1], &crc)
                        : opsdk_save_crc32(dev, cart, (uint32_t)a[0], (uint32_t)a[1], &crc);
    if (rc == OPSDK_OK) snprintf(out, size, "ok crc=%08x", (unsigned)crc);
  } else if (strcmp(name, "save_write") == 0) {
    uint8_t *data = malloc(a[0]);
    if (!data) abort();
    for (unsigned long i = 0; i < a[0]; i++) data[i] = (uint8_t)(i * a[1]);
    if (nargs > 2) data[0] = (uint8_t)a[2];
    rc = opsdk_save_write(dev, cart, data, (uint32_t)a[0], NULL, NULL);
    if (rc == OPSDK_OK) snprintf(out, size, "ok");
    free(data);
  } else if (strcmp(name, "led_set") == 0) {
    opsdk_led_t led = {{(uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]},
                       {(uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]},
                       OPSDK_EFFECT_CONSTANT, 0, OPSDK_EFFECT_CONSTANT, 100, 0, 1000};
    rc = opsdk_led_set(dev, &led);
    if (rc == OPSDK_OK) snprintf(out, size, "ok");
  } else if (strcmp(name, "led_release") == 0) {
    rc = opsdk_led_release(dev);
    if (rc == OPSDK_OK) snprintf(out, size, "ok");
  } else {
    snprintf(out, size, "unknown call %s", name);
    return;
  }
  if (rc == OPSDK_E_DEVICE) {
    snprintf(out, size, "error DEVICE device_error=0x%02x", opsdk_last_error(dev));
  } else if (rc != OPSDK_OK) {
    snprintf(out, size, "%s", status_name(rc));
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_conformance CONFORMANCE_DIR\n");
    return 2;
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/transcripts.txt", argv[1]);
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) {
    fclose(f);
    return 1;
  }
  char *text = malloc((size_t)size + 1);
  if (!text) abort();
  if (fread(text, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    return 1;
  }
  text[size] = '\0';
  fclose(f);

  item_t *items = calloc((size_t)size / 4 + 16, sizeof(item_t));
  if (!items) abort();
  size_t count = 0;
  int scenarios = 0, failures = 0;
  char scenario[64] = "";
  char *next = text;
  while (next && *next) {
    char *line = next;
    next = strchr(line, '\n');
    if (next) *next++ = '\0';
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
    if (!len || line[0] == '#') continue;
    char *space = strchr(line, ' ');
    char *value = space ? space + 1 : line + len;
    if (space) *space = '\0';
    if (strcmp(line, "scenario") == 0) {
      snprintf(scenario, sizeof(scenario), "%s", value);
      count = 0;
      continue;
    }
    if (strcmp(line, "end") != 0) {
      snprintf(items[count].word, sizeof(items[count].word), "%s", line);
      items[count++].value = value;
      continue;
    }
    scenarios++;
    replay_t r;
    memset(&r, 0, sizeof(r));
    r.items = items;
    r.count = count;
    static const opsdk_transport_t replay = {t_read, t_write, NULL};
    opsdk_t *dev;
    if (opsdk_open_transport(&dev, &replay, &r) != OPSDK_OK) return 1;
    opsdk_set_timeout(dev, 500);
    opsdk_cart_t cart;
    memset(&cart, 0, sizeof(cart));
    bool ok = true;
    while (ok && r.cursor < r.count) {
      item_t *call = &r.items[r.cursor++];
      if (strcmp(call->word, "call") != 0) {
        fprintf(stderr, "FAIL %s: expected a call, found %s\n", scenario, call->word);
        ok = false;
        break;
      }
      char result[160] = "";
      run_call(dev, &cart, call->value, result, sizeof(result));
      item_t *expect = r.cursor < r.count ? &r.items[r.cursor++] : NULL;
      if (r.mismatch[0]) {
        fprintf(stderr, "FAIL %s: %s: %s\n", scenario, call->value, r.mismatch);
        ok = false;
      } else if (!expect || strcmp(expect->word, "expect") != 0) {
        fprintf(stderr, "FAIL %s: %s: the host stopped before the recorded bytes\n", scenario, call->value);
        ok = false;
      } else if (strcmp(expect->value, result) != 0) {
        fprintf(stderr, "FAIL %s: %s: got \"%s\", recorded \"%s\"\n", scenario, call->value, result,
                expect->value);
        ok = false;
      }
    }
    opsdk_close(dev);
    free(r.queue);
    if (!ok) failures++;
    else printf("ok   %s\n", scenario);
  }
  free(items);
  free(text);
  if (!scenarios) {
    fprintf(stderr, "no scenarios in %s\n", path);
    return 1;
  }
  printf("%d scenarios, %d failed\n", scenarios, failures);
  return failures ? 1 : 0;
}
