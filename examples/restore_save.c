#include <opsdk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portable.h"

static uint8_t *read_file(const char *path, long *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint8_t *buf = NULL;
  if (fseek(f, 0, SEEK_END) == 0 && (*size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
    buf = malloc((size_t)*size);
    if (buf && fread(buf, 1, (size_t)*size, f) != (size_t)*size) {
      free(buf);
      buf = NULL;
    }
  }
  fclose(f);
  return buf;
}

static bool write_new_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f = portable_create_new(path);
  if (!f) return false;
  bool ok = fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

static bool show_progress(uint32_t done, uint32_t total, void *user) {
  (void)user;
  fprintf(stderr, "\r%u/%u B", done, total);
  return true;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: restore_save INPUT.sav BACKUP.sav [PORT]\n");
    return 2;
  }
  long size = 0;
  uint8_t *data = read_file(argv[1], &size), *current = NULL;
  if (!data) {
    perror(argv[1]);
    return 1;
  }
  opsdk_t *dev = NULL;
  int rc = opsdk_open(&dev, argc > 3 ? argv[3] : NULL);
  if (rc != OPSDK_OK) {
    fprintf(stderr, "open: %s\n", opsdk_strerror(rc));
    free(data);
    return 1;
  }
  bool session = false;
  const char *why = NULL;
  uint8_t *check = NULL;
  uint32_t crc = 0;

  opsdk_info_t info;
  if ((rc = opsdk_get_info(dev, &info)) != OPSDK_OK) goto fail;
  if (!opsdk_has(&info, OPSDK_OP_SAVE_WRITE)) {
    why = "this firmware cannot write saves";
    goto stop;
  }
  opsdk_cart_t cart, held, now;
  if ((rc = opsdk_cart_status(dev, &cart)) != OPSDK_OK) goto fail;
  if (!cart.logo_valid) {
    why = "no readable cartridge, insert one or reseat it";
    goto stop;
  }
  if (cart.flashcart || cart.counterfeit) {
    why = "writes are refused on this cartridge";
    goto stop;
  }
  if ((uint32_t)size != cart.save_size) {
    why = "the file size does not match this cartridge's save";
    goto stop;
  }

  current = malloc(cart.save_size);
  check = malloc(cart.save_size);
  if (!current || !check) {
    rc = OPSDK_E_NO_MEMORY;
    goto fail;
  }
  if ((rc = opsdk_save_read(dev, &cart, 0, cart.save_size, current)) != OPSDK_OK) goto fail;
  if ((rc = opsdk_save_crc32(dev, &cart, 0, cart.save_size, &crc)) != OPSDK_OK) goto fail;
  if ((rc = opsdk_cart_status(dev, &now)) != OPSDK_OK) goto fail;
  if (crc != opsdk_crc32(0, current, cart.save_size) || !now.logo_valid || !opsdk_same_cartridge(&cart, &now)) {
    why = "backup read did not verify, nothing written";
    goto stop;
  }
  if (!write_new_file(argv[2], current, cart.save_size)) {
    why = "cannot write the backup file, nothing written";
    goto stop;
  }
  printf("Backed up the current save to %s.\n", argv[2]);

  printf("Replace the save on the cartridge with %s? Type \"yes\": ", argv[1]);
  fflush(stdout);
  char answer[16] = {0};
  if (!fgets(answer, sizeof(answer), stdin) || strcmp(answer, "yes\n") != 0) {
    why = "not confirmed, nothing written";
    goto stop;
  }

  if ((rc = opsdk_session_begin(dev, &held)) != OPSDK_OK) goto fail;
  session = true;
  if (!held.logo_valid || !opsdk_same_cartridge(&cart, &held)) goto changed;
  if ((rc = opsdk_save_read(dev, &held, 0, held.save_size, check)) != OPSDK_OK) goto fail;
  if (memcmp(check, current, held.save_size) != 0) {
    why = "the save no longer matches the backup, nothing written";
    goto stop;
  }
  if ((rc = opsdk_cart_status(dev, &now)) != OPSDK_OK) goto fail;
  if (!now.logo_valid || !opsdk_same_cartridge(&cart, &now)) goto changed;

  rc = opsdk_save_write(dev, &held, data, (uint32_t)size, show_progress, NULL);
  fputc('\n', stderr);
  if (rc != OPSDK_OK) {
    fprintf(stderr, "The write failed (%s). The save may be partly written.\n", opsdk_strerror(rc));
    fprintf(stderr, "Recover by restoring %s once the cartridge is reseated.\n", argv[2]);
    goto done;
  }
  rc = opsdk_save_read(dev, &held, 0, held.save_size, check);
  if (rc == OPSDK_OK) rc = opsdk_cart_status(dev, &now);
  if (rc != OPSDK_OK) fprintf(stderr, "Operator error: %s (%d)\n", opsdk_strerror(rc), rc);
  if (rc != OPSDK_OK || !now.logo_valid || !opsdk_same_cartridge(&cart, &now) ||
      memcmp(check, data, (size_t)size) != 0) {
    fprintf(stderr, "Verification failed. Restore %s.\n", argv[2]);
    if (rc == OPSDK_OK) rc = OPSDK_E_PROTOCOL;
    goto done;
  }
  printf("Save written and verified.\n");
  goto done;

changed:
  why = "cartridge changed, nothing written";
stop:
  fprintf(stderr, "%s\n", why);
  rc = OPSDK_E_ARGUMENT;
  goto done;
fail:
  fprintf(stderr, "Operator error: %s (%d)\n", opsdk_strerror(rc), rc);
done:
  if (session) {
    int end_rc = opsdk_session_end(dev);
    if (rc == OPSDK_OK && end_rc != OPSDK_OK) {
      fprintf(stderr, "The session did not end cleanly: %s (%d)\n", opsdk_strerror(end_rc), end_rc);
      rc = end_rc;
    }
  }
  opsdk_close(dev);
  free(check);
  free(current);
  free(data);
  return rc == OPSDK_OK ? 0 : 1;
}
