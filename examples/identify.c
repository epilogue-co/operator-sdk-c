#include <opsdk.h>

#include <stdio.h>

static const char NO_SDK[] = "This Operator's firmware has no SDK support. Update it.\n";

static const char *platform_name(opsdk_platform_t p) {
  switch (p) {
  case OPSDK_PLATFORM_GB: return "Game Boy / Color";
  case OPSDK_PLATFORM_GBA: return "Game Boy Advance";
  case OPSDK_PLATFORM_SNES: return "Super Nintendo";
  case OPSDK_PLATFORM_N64: return "Nintendo 64";
  default: return "Unknown platform";
  }
}

int main(int argc, char **argv) {
  opsdk_t *dev = NULL;
  int rc = opsdk_open(&dev, argc > 1 ? argv[1] : NULL);
  if (rc == OPSDK_UNKNOWN_COMMAND) {
    fputs(NO_SDK, stderr);
    return 1;
  }
  if (rc == OPSDK_E_NO_DEVICE) {
    fprintf(stderr, "No Operator found. Is it plugged in? Pass the port path to skip auto-detection.\n");
    return 1;
  }
  if (rc != OPSDK_OK) {
    fprintf(stderr, "open: %s\n", opsdk_strerror(rc));
    return 1;
  }

  opsdk_info_t info;
  rc = opsdk_get_info(dev, &info);
  if (rc == OPSDK_UNKNOWN_COMMAND) {
    fputs(NO_SDK, stderr);
    goto done;
  }
  if (rc != OPSDK_OK) goto fail;
  printf("Operator %s, firmware %u.%u.%u, SDK protocol %u\n", info.serial, info.app_version[0],
         info.app_version[1], info.app_version[2], info.protocol_version);
  printf("  save writes %s, clock %s, camera %s\n", opsdk_has(&info, OPSDK_OP_SAVE_WRITE) ? "yes" : "no",
         opsdk_has(&info, OPSDK_OP_RTC_READ) ? "yes" : "no",
         opsdk_has(&info, OPSDK_OP_CAMERA_START) ? "yes" : "no");

  opsdk_cart_t cart;
  rc = opsdk_cart_status(dev, &cart);
  if (rc != OPSDK_OK) goto fail;
  if (!cart.logo_valid) {
    printf("No readable cartridge. Insert one or reseat it.\n");
  } else {
    printf("%s, game %u B, save %u B\n", platform_name(cart.platform), cart.rom_size, cart.save_size);
    if (cart.counterfeit) printf("  Writes are refused on this cartridge (counterfeit).\n");
  }
  rc = OPSDK_OK;
  goto done;

fail:
  fprintf(stderr, "Operator error: %s (%d)\n", opsdk_strerror(rc), rc);
done:
  opsdk_close(dev);
  return rc == OPSDK_OK ? 0 : 1;
}
