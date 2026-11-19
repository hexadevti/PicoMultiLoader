// pico/stdlib.h for the apps built into the loader (PML_BUILTIN, the ESP32-P4 build): the few Pico
// SDK calls they make, on top of the platform's plat.h.
#pragma once

#include <stdint.h>
#include "plat.h"

typedef uint64_t absolute_time_t;

static inline uint32_t time_us_32(void)
{
    return (uint32_t)time_us_64();
}

static inline absolute_time_t get_absolute_time(void)
{
    return time_us_64();
}

static inline uint32_t to_ms_since_boot(absolute_time_t t)
{
    return (uint32_t)(t / 1000);
}
