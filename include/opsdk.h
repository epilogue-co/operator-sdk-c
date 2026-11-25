#ifndef OPSDK_H
#define OPSDK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(OPSDK_BUILDING_SHARED)
#define OPSDK_API __declspec(dllexport)
#elif defined(OPSDK_SHARED)
#define OPSDK_API __declspec(dllimport)
#else
#define OPSDK_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define OPSDK_API __attribute__((visibility("default")))
#else
#define OPSDK_API
#endif

#define OPSDK_VERSION_MAJOR 0
#define OPSDK_VERSION_MINOR 3
#define OPSDK_VERSION_PATCH 0
#define OPSDK_VERSION "0.3.0"
#define OPSDK_PROTOCOL_VERSION 1

OPSDK_API const char *opsdk_version(void);

enum {
  OPSDK_OK = 0x00,
  OPSDK_UNKNOWN_COMMAND = 0x01,
  OPSDK_UNSUPPORTED = 0x02,
  OPSDK_BAD_ARGUMENT = 0x03,
  OPSDK_NO_CARTRIDGE = 0x04,
  OPSDK_CART_UNSTABLE = 0x05,
  OPSDK_ABORTED = 0x06,
  OPSDK_TIMEOUT = 0x07,
  OPSDK_NOT_PERMITTED = 0x09,
  OPSDK_NO_SESSION = 0x0B,
  OPSDK_CART_ERROR = 0x0C,
};

/* Negative codes come from the library, the device never sends them. */
enum {
  OPSDK_E_IO = -1,
  OPSDK_E_TIMEOUT = -2,
  OPSDK_E_PROTOCOL = -3,
  OPSDK_E_ARGUMENT = -4,
  OPSDK_E_NO_DEVICE = -5,
  OPSDK_E_DEVICE = -6,
  OPSDK_E_NO_MEMORY = -7,
};

enum {
  OPSDK_OP_PING = 0xA0,
  OPSDK_OP_GET_INFO = 0xA1,
  OPSDK_OP_CART_STATUS = 0xA2,
  OPSDK_OP_SESSION_BEGIN = 0xA4,
  OPSDK_OP_SESSION_END = 0xA5,
  OPSDK_OP_ROM_READ = 0xA8,
  OPSDK_OP_ROM_CRC32 = 0xA9,
  OPSDK_OP_SAVE_READ = 0xB1,
  OPSDK_OP_SAVE_WRITE = 0xB2,
  OPSDK_OP_SAVE_CRC32 = 0xB3,
  OPSDK_OP_RTC_READ = 0xB4,
  OPSDK_OP_RTC_WRITE = 0xB5,
  OPSDK_OP_LED_SET = 0xB8,
  OPSDK_OP_LED_RELEASE = 0xB9,
  OPSDK_OP_CAMERA_START = 0xBA,
  OPSDK_OP_CAMERA_CAPTURE = 0xBB,
  OPSDK_OP_CAMERA_STOP = 0xBC,
  OPSDK_OP_SENSOR_READ = 0xBD,
  OPSDK_OP_RUMBLE_SET = 0xBE,
};

enum {
  OPSDK_DEVICE_GB_OPERATOR = 0,
  OPSDK_DEVICE_SN_OPERATOR = 1,
  OPSDK_DEVICE_N64_OPERATOR = 2,
};

/* Offsets and lengths are multiples of this. The one exception is a save read
   or CRC that runs to the end of a save smaller than that. */
#define OPSDK_ALIGN 512
#define OPSDK_RTC_SIZE 64
#define OPSDK_SIGNATURE_SIZE 512
#define OPSDK_CAMERA_FRAME_SIZE 3584
#define OPSDK_USB_VENDOR_ID 0x16D0

typedef enum {
  OPSDK_PLATFORM_EMPTY = 0x00,
  OPSDK_PLATFORM_GB = 0x20,
  OPSDK_PLATFORM_GBA = 0x30,
  OPSDK_PLATFORM_SNES = 0x50,
  OPSDK_PLATFORM_N64 = 0x60,
  OPSDK_PLATFORM_UNKNOWN = 0xE0,
} opsdk_platform_t;

typedef struct opsdk opsdk_t;

typedef struct {
  uint16_t protocol_version;
  uint8_t device_id;
  uint8_t board_revision;
  uint8_t app_version[3];
  uint8_t sys_version[3];
  uint32_t capabilities;
  char serial[17];
  uint16_t session_idle_timeout_s;
} opsdk_info_t;

typedef struct {
  uint8_t raw[OPSDK_SIGNATURE_SIZE];
  opsdk_platform_t platform;
  uint8_t type_byte;
  bool flashcart;
  bool counterfeit;
  bool logo_valid;
  bool checksum_valid;
  uint32_t rom_size;
  uint32_t save_size;
  uint8_t save_chip_type;
  uint8_t save_chip;
  uint8_t revision;
  uint8_t region;
  uint8_t header_bytes[7];
} opsdk_cart_t;

typedef enum {
  OPSDK_SENSOR_MBC7_ACCEL = 0x01,
  OPSDK_SENSOR_GBA_TILT = 0x02,
  OPSDK_SENSOR_GBA_GYRO = 0x03,
  OPSDK_SENSOR_GBA_SOLAR = 0x04,
} opsdk_sensor_kind_t;

typedef struct {
  uint8_t kind;
  uint16_t x, y, z;
} opsdk_sensor_t;

enum {
  OPSDK_LIGHT_OFF = 0x00,
  OPSDK_EFFECT_CONSTANT = 0x10,
  OPSDK_EFFECT_SQUARE = 0x20,
  OPSDK_EFFECT_SAWTOOTH = 0x21,
  OPSDK_EFFECT_TRIANGLE = 0x22,
  OPSDK_EFFECT_SINE = 0x30,
  OPSDK_LIGHT_ALWAYS_ON = 0xFF,
};

typedef struct {
  uint8_t primary[3];
  uint8_t secondary[3];
  uint8_t color_effect;
  uint8_t color_duty;
  uint8_t light_effect;
  uint8_t light_duty;
  uint8_t offset;
  uint16_t period_ms;
} opsdk_led_t;

typedef struct {
  uint16_t exposure;
  uint8_t gain;
  bool vertical_edge;
  uint8_t edge_operation;
  uint8_t edge_ratio;
  bool invert;
  uint8_t vref;
  bool zero_calibration;
  uint8_t output_ref_voltage;
  uint8_t matrix[48];
} opsdk_camera_t;

typedef struct {
  uint8_t year;
  uint8_t month;
  uint8_t day;
  uint8_t weekday;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  bool hour12;
  bool stopped;
  bool calendar;
  bool lost_power;
  bool valid;
} opsdk_rtc4513_t;

typedef bool (*opsdk_progress_fn)(uint32_t done, uint32_t total, void *user);

OPSDK_API int opsdk_open(opsdk_t **dev, const char *path);
OPSDK_API int opsdk_open_fd(opsdk_t **dev, int fd);

/* read: bytes read, 0 on timeout, <0 on error.
   write: OPSDK_OK or <0.
   close may be NULL. */
typedef struct {
  int (*read)(void *context, uint8_t *buf, size_t length, uint32_t timeout_ms);
  int (*write)(void *context, const uint8_t *buf, size_t length);
  void (*close)(void *context);
} opsdk_transport_t;

OPSDK_API int opsdk_open_transport(opsdk_t **dev, const opsdk_transport_t *transport, void *context);
OPSDK_API void opsdk_close(opsdk_t *dev);
OPSDK_API int opsdk_list_ports(char *buf, size_t size);
OPSDK_API void opsdk_set_timeout(opsdk_t *dev, uint32_t ms);
OPSDK_API uint8_t opsdk_last_error(const opsdk_t *dev);
OPSDK_API const char *opsdk_strerror(int code);

OPSDK_API int opsdk_last_os_error(const opsdk_t *dev);
OPSDK_API void opsdk_os_error_text(int os_error, char *buf, size_t size);

typedef enum {
  OPSDK_TRACE_SEND = 1,
  OPSDK_TRACE_RECEIVE = 2,
  OPSDK_TRACE_EVENT = 3,
} opsdk_trace_kind_t;

typedef struct {
  opsdk_trace_kind_t kind;
  uint8_t op;
  const uint8_t *data;
  size_t length;
  const char *message;
} opsdk_trace_t;

typedef void (*opsdk_trace_fn)(const opsdk_trace_t *event, void *user);

OPSDK_API void opsdk_set_trace(opsdk_t *dev, opsdk_trace_fn fn, void *user);

OPSDK_API int opsdk_ping(opsdk_t *dev, uint8_t challenge);
OPSDK_API int opsdk_get_info(opsdk_t *dev, opsdk_info_t *info);
OPSDK_API bool opsdk_has(const opsdk_info_t *info, uint8_t opcode);

OPSDK_API int opsdk_cart_status(opsdk_t *dev, opsdk_cart_t *cart);
/* Writes, camera and rumble need a session. It times out after
   session_idle_timeout_s and ends when the cartridge is removed. */
OPSDK_API int opsdk_session_begin(opsdk_t *dev, opsdk_cart_t *cart);
OPSDK_API int opsdk_session_end(opsdk_t *dev);
OPSDK_API bool opsdk_same_cartridge(const opsdk_cart_t *a, const opsdk_cart_t *b);

OPSDK_API int opsdk_rom_read(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                             void *buf, opsdk_progress_fn progress, void *user);
OPSDK_API int opsdk_rom_crc32(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                              uint32_t *crc);
OPSDK_API int opsdk_save_read(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                              void *buf);
OPSDK_API int opsdk_save_crc32(opsdk_t *dev, const opsdk_cart_t *cart, uint32_t offset, uint32_t length,
                               uint32_t *crc);
OPSDK_API int opsdk_save_write(opsdk_t *dev, const opsdk_cart_t *cart, const void *data, uint32_t size,
                               opsdk_progress_fn progress, void *user);

OPSDK_API int opsdk_rtc_read(opsdk_t *dev, const opsdk_cart_t *cart, uint8_t raw[OPSDK_RTC_SIZE]);
OPSDK_API int opsdk_rtc_write(opsdk_t *dev, const opsdk_cart_t *cart, const uint8_t raw[OPSDK_RTC_SIZE]);
OPSDK_API int opsdk_rtc4513_decode(const uint8_t raw[OPSDK_RTC_SIZE], opsdk_rtc4513_t *clock);
OPSDK_API int opsdk_rtc4513_encode(const opsdk_rtc4513_t *clock, uint8_t raw[OPSDK_RTC_SIZE]);

OPSDK_API int opsdk_led_set(opsdk_t *dev, const opsdk_led_t *led);
OPSDK_API int opsdk_led_release(opsdk_t *dev);

OPSDK_API int opsdk_camera_start(opsdk_t *dev);
OPSDK_API int opsdk_camera_capture(opsdk_t *dev, const opsdk_camera_t *settings,
                                   uint8_t frame[OPSDK_CAMERA_FRAME_SIZE]);
OPSDK_API int opsdk_camera_stop(opsdk_t *dev);
OPSDK_API void opsdk_camera_defaults(opsdk_camera_t *settings);

OPSDK_API int opsdk_sensor_read(opsdk_t *dev, opsdk_sensor_t *sensor);
OPSDK_API int opsdk_rumble_set(opsdk_t *dev, bool on);

OPSDK_API int opsdk_abort(opsdk_t *dev);

OPSDK_API uint32_t opsdk_crc32(uint32_t crc, const void *data, size_t length);
OPSDK_API void opsdk_parse_signature(const uint8_t raw[OPSDK_SIGNATURE_SIZE], opsdk_cart_t *cart);
OPSDK_API void opsdk_build_command(uint8_t packet[64], uint8_t operation, uint8_t save_chip,
                                   uint32_t rom_size, uint32_t save_size, const uint8_t *data,
                                   size_t data_len, uint8_t request_id);
OPSDK_API void opsdk_build_led(uint8_t block[32], uint8_t id, const opsdk_led_t *led);

#ifdef __cplusplus
}
#endif

#endif
