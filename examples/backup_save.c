#include <opsdk.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "portable.h"

static volatile sig_atomic_t cancelled;

static void on_sigint(int sig) {
  (void)sig;
  cancelled = 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: backup_save OUTPUT.sav [PORT]\n");
    return 2;
  }
  opsdk_t *dev = NULL;
  int rc = opsdk_open(&dev, argc > 2 ? argv[2] : NULL);
  if (rc != OPSDK_OK) {
    fprintf(stderr, "open: %s\n", opsdk_strerror(rc));
    return 1;
  }
  signal(SIGINT, on_sigint);

  uint8_t *save = NULL;
  bool session = false;
  opsdk_cart_t cart;
  rc = opsdk_session_begin(dev, &cart);
  if (rc != OPSDK_OK) goto fail;
  session = true;
  if (cancelled) goto cancel;
  if (cart.save_size == 0) {
    fprintf(stderr, "This cartridge has no save to back up.\n");
    rc = OPSDK_E_ARGUMENT;
    goto done;
  }

  save = malloc(cart.save_size);
  if (!save) {
    rc = OPSDK_E_NO_MEMORY;
    goto fail;
  }
  rc = opsdk_save_read(dev, &cart, 0, cart.save_size, save);
  if (rc != OPSDK_OK) goto fail;
  if (cancelled) goto cancel;
  uint32_t device_crc = 0;
  rc = opsdk_save_crc32(dev, &cart, 0, cart.save_size, &device_crc);
  if (rc != OPSDK_OK) goto fail;
  if (cancelled) goto cancel;
  if (opsdk_crc32(0, save, cart.save_size) != device_crc) {
    fprintf(stderr, "The save changed or was corrupted during the read. Try again.\n");
    rc = OPSDK_E_PROTOCOL;
    goto done;
  }

  FILE *f = portable_create_new(argv[1]);
  if (!f) {
    perror(argv[1]);
    rc = OPSDK_E_IO;
    goto done;
  }
  bool written = fwrite(save, 1, cart.save_size, f) == cart.save_size;
  if (fclose(f) != 0) written = false;
  if (!written) {
    perror(argv[1]);
    remove(argv[1]);
    rc = OPSDK_E_IO;
    goto done;
  }
  printf("%s: %u B, CRC-32 %08X\n", argv[1], cart.save_size, device_crc);
  goto done;

cancel:
  rc = OPSDK_ABORTED;
fail:
  fprintf(stderr, "%s\n", rc == OPSDK_ABORTED ? "Cancelled." : opsdk_strerror(rc));
done:
  if (session) opsdk_session_end(dev);
  opsdk_close(dev);
  free(save);
  return rc == OPSDK_OK ? 0 : 1;
}
