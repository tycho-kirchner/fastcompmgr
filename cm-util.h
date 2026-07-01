#pragma once

#include <time.h>
#include <string.h>

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#define READ_ONCE(x) \
({ typeof(x) ___x = ACCESS_ONCE(x); ___x; })

#define WRITE_ONCE(x, val) \
do { ACCESS_ONCE(x) = (val); } while (0)

static inline int
get_time_in_milliseconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// normalize double to range 0-1
static inline double normalize_d(double d) {
  if (d > 1.0)
    return 1.0;
  if (d < 0.0)
    return 0.0;

  return d;
}

