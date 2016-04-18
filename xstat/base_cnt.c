// To be included in xstat.c

#include <linux/time.h>

static uint64_t get_time(void) {
    static struct timespec ts;
    do_posix_clock_monotonic_gettime(&ts);
    return timespec_to_ns(&ts);
}

static uint64_t ts_restart(void **ctx, uint64_t last) {
    return get_time();
}

static struct xstat_counter ts_counter = __XSTAT_CNT(ts, NULL, NULL, ts_restart, NULL, NULL);
