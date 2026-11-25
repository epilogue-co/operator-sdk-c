#include "opsdk_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>

#ifndef MAX_DEVICE_ID_LEN
#define MAX_DEVICE_ID_LEN 200
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_SLICE_MS 50
#define WRITE_TIMEOUT_MS 2000

typedef struct {
  HANDLE handle;
  DWORD os_error;
} win_port_t;

static int port_read(void *context, uint8_t *buf, size_t n, uint32_t timeout_ms) {
  win_port_t *port = context;
  size_t got = 0;
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (got < n && GetTickCount64() < deadline) {
    DWORD want = n - got > 0x10000 ? 0x10000 : (DWORD)(n - got);
    DWORD k = 0;
    if (!ReadFile(port->handle, buf + got, want, &k, NULL)) {
      port->os_error = GetLastError();
      return OPSDK_E_IO;
    }
    got += k;
  }
  return (int)got;
}

static int port_write(void *context, const uint8_t *buf, size_t n) {
  win_port_t *port = context;
  size_t done = 0;
  while (done < n) {
    DWORD want = n - done > 0x10000 ? 0x10000 : (DWORD)(n - done);
    DWORD k = 0;
    if (!WriteFile(port->handle, buf + done, want, &k, NULL)) {
      port->os_error = GetLastError();
      return OPSDK_E_IO;
    }
    if (k == 0) {
      port->os_error = ERROR_TIMEOUT;
      return OPSDK_E_IO;
    }
    done += k;
  }
  return OPSDK_OK;
}

static void port_close(void *context) {
  win_port_t *port = context;
  if (port->handle != INVALID_HANDLE_VALUE) CloseHandle(port->handle);
  free(port);
}

static const opsdk_transport_t WIN32_TRANSPORT = {port_read, port_write, port_close};

int opsdk_platform_wrap_fd(int fd, opsdk_transport_t *transport, void **context) {
  (void)fd;
  (void)transport;
  (void)context;
  return OPSDK_E_ARGUMENT;
}

void opsdk_platform_free_wrapper(void *context) {
  free(context);
}

int opsdk_platform_os_error(void *context) {
  return (int)((win_port_t *)context)->os_error;
}

static bool configure(HANDLE h) {
  DCB dcb;
  memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(h, &dcb)) return false;
  dcb.BaudRate = 115200;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fErrorChar = FALSE;
  dcb.fNull = FALSE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fAbortOnError = FALSE;
  if (!SetCommState(h, &dcb)) return false;
  SetupComm(h, 1 << 16, 1 << 16);
  COMMTIMEOUTS t;
  /* MAXDWORD/MAXDWORD/constant: return with whatever is there, or wait up to the
     constant for the first byte. See the COMMTIMEOUTS docs. */
  t.ReadIntervalTimeout = MAXDWORD;
  t.ReadTotalTimeoutMultiplier = MAXDWORD;
  t.ReadTotalTimeoutConstant = READ_SLICE_MS;
  t.WriteTotalTimeoutMultiplier = 0;
  t.WriteTotalTimeoutConstant = WRITE_TIMEOUT_MS;
  if (!SetCommTimeouts(h, &t)) return false;
  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
  return true;
}

/* TODO test against real hardware */
int opsdk_platform_open(const char *path, opsdk_transport_t *transport, void **context) {
  char full[MAX_PATH];
  /* COM10 and up only open with the device prefix */
  if (strncmp(path, "\\\\.\\", 4) == 0) snprintf(full, sizeof(full), "%s", path);
  else snprintf(full, sizeof(full), "\\\\.\\%s", path);

  HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? OPSDK_E_NO_DEVICE
                                                                      : OPSDK_E_IO;
  }
  if (!configure(h)) {
    DWORD err = GetLastError();
    CloseHandle(h);
    SetLastError(err);
    return OPSDK_E_IO;
  }
  win_port_t *port = calloc(1, sizeof(*port));
  if (!port) {
    CloseHandle(h);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return OPSDK_E_NO_MEMORY;
  }
  port->handle = h;
  *transport = WIN32_TRANSPORT;
  *context = port;
  return OPSDK_OK;
}

void opsdk_os_error_text(int os_error, char *buf, size_t size) {
  if (!buf || !size) return;
  DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                           (DWORD)os_error, 0, buf, (DWORD)size, NULL);
  if (n == 0) {
    snprintf(buf, size, "error %d", os_error);
    return;
  }
  while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
}

static const GUID COMPORT_INTERFACE = {0x86E0D1E0, 0x8089, 0x11D0,
                                       {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};

static bool is_operator(const char *instance_id) {
  static const unsigned products[] = OPSDK_USB_PRODUCT_IDS;
  char upper[MAX_DEVICE_ID_LEN];
  size_t i = 0;
  for (; instance_id[i] && i < sizeof(upper) - 1; i++) {
    char c = instance_id[i];
    upper[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
  }
  upper[i] = '\0';
  const char *v = strstr(upper, "VID_");
  const char *p = strstr(upper, "PID_");
  if (!v || !p) return false;
  unsigned vendor = (unsigned)strtoul(v + 4, NULL, 16);
  unsigned product = (unsigned)strtoul(p + 4, NULL, 16);
  if (vendor != OPSDK_USB_VENDOR_ID) return false;
  for (size_t k = 0; k < sizeof(products) / sizeof(products[0]); k++) {
    if (product == products[k]) return true;
  }
  return false;
}

int opsdk_list_ports(char *buf, size_t size) {
  if (!buf || size < 2) return 0;
  size_t used = 0;
  int count = 0;
  HDEVINFO set = SetupDiGetClassDevsA(&COMPORT_INTERFACE, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set != INVALID_HANDLE_VALUE) {
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++) {
      char id[MAX_DEVICE_ID_LEN];
      if (!SetupDiGetDeviceInstanceIdA(set, &info, id, sizeof(id), NULL)) continue;
      if (!is_operator(id)) continue;
      HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
      if (key == INVALID_HANDLE_VALUE) continue;
      char name[64];
      DWORD type = 0, len = sizeof(name) - 1;
      LONG r = RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)name, &len);
      RegCloseKey(key);
      if (r != ERROR_SUCCESS || type != REG_SZ) continue;
      name[len < sizeof(name) ? len : sizeof(name) - 1] = '\0';
      size_t n = strlen(name) + 1;
      if (used + n + 1 > size) break;
      memcpy(buf + used, name, n);
      used += n;
      count++;
    }
    SetupDiDestroyDeviceInfoList(set);
  }
  buf[used] = '\0';
  return count;
}
