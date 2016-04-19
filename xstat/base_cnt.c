// To be included in xstat.c

#include <linux/time.h>

static uint64_t get_time(void) {
    static struct timespec ts;
    do_posix_clock_monotonic_gettime(&ts);
    return timespec_to_ns(&ts);
}

static uint64_t ts_restart(void **ctx, uint64_t last) {
    uint64_t now = get_time();
    *ctx = (void *) last;
    ctx++;
    *ctx = (void *) now;
    return now;
}

static uint64_t intv_restart(void **ctx, uint64_t last) {
    uint64_t now = (uint64_t) *ctx;
    uint64_t prev;
    ctx--;
    prev = (uint64_t) *ctx;
    return now - prev;
}

// These counters must be put together
static struct xstat_counter ts_counter = __XSTAT_CNT(ts, NULL, NULL, ts_restart, NULL, NULL);
static struct xstat_counter intv_counter = __XSTAT_CNT(intv, NULL, NULL, intv_restart, NULL, NULL);
