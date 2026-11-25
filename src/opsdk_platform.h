#ifndef OPSDK_PLATFORM_H
#define OPSDK_PLATFORM_H

#include "opsdk.h"

#define OPSDK_USB_PRODUCT_IDS {0x123D, 0x123E, 0x134D}

int opsdk_platform_open(const char *path, opsdk_transport_t *transport, void **context);

int opsdk_platform_wrap_fd(int fd, opsdk_transport_t *transport, void **context);
void opsdk_platform_free_wrapper(void *context);

int opsdk_platform_os_error(void *context);

#endif
