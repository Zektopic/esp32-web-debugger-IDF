#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* Compile-time "is this SOC capability macro defined?" check.
 * Stringifying an undefined macro yields its own name, a defined one yields
 * its value, so the two strings differ only when the macro exists.
 * NOTE: pass the macro name directly. Wrapping CAP_DEFINED in another macro
 * expands the argument first and always yields false. */
#define CAPS_STR_(x) #x
#define CAPS_STR(x)  CAPS_STR_(x)
#define CAP_DEFINED(m) (__builtin_strcmp(#m, CAPS_STR(m)) != 0)

/* "ESP32-S3", "ESP32-C6", ... */
const char *caps_chip_name(void);

/* Everything the UI needs to adapt itself to the chip: identity, features,
 * memory sizes, firmware versions and default pin assignments. Caller owns the
 * returned object (cJSON_Delete). */
cJSON *caps_info_json(void);
