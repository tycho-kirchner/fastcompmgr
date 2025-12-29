#pragma once

#include <sys/time.h>
#include <string.h>

extern time_t _program_start_secs;

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#define READ_ONCE(x) \
({ typeof(x) ___x = ACCESS_ONCE(x); ___x; })

#define WRITE_ONCE(x, val) \
do { ACCESS_ONCE(x) = (val); } while (0)

static inline int
get_time_in_milliseconds() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec-_program_start_secs) * 1000 + tv.tv_usec / 1000;
}

// normalize double to range 0-1
static inline double normalize_d(double d) {
  if (d > 1.0)
    return 1.0;
  if (d < 0.0)
    return 0.0;

  return d;
}

