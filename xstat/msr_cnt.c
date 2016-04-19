#include <asm/msr.h>

// #define TEMP_DETAIL

static uint64_t temp_restart(void **ctx, uint64_t last) {
    uint64_t status, target;
    int tjmax, max, tpkg;

    rdmsrl(MSR_IA32_PACKAGE_THERM_STATUS, status);
    rdmsrl(MSR_IA32_TEMPERATURE_TARGET, target);

    tjmax = (target >> 16) & 0xff;
    max = (target >> 8) & 0xff;
    tpkg = (status >> 16) & 0x7f;
    max = tjmax - max;
    tpkg = tjmax - tpkg;
    return (tjmax << 24) | (max << 16) | (tpkg);
}

static int temp_scnprintf(char *buf, int limit, uint64_t data, void **ctx) {
    int tpkg;
#ifdef TEMP_DETAIL
    int tjmax, max;
    tjmax = (data >> 24) & 0xff;
    max = (data >> 16) & 0xff;
#endif
    tpkg = data & 0xff;

#ifdef TEMP_DETAIL
    return scnprintf(buf, limit, "\"tjmax\":%d,\"tmax\":%d,\"tpkg\":%d",
            tjmax, max, tpkg);
#else
    return scnprintf(buf, limit, "\"tpkg\":%d", tpkg);
#endif
}

#ifdef TEMP_DETAIL
#undef TEMP_DETAIL
#endif

static struct xstat_counter temp_counter = __XSTAT_CNT(temp, NULL, NULL, temp_restart, NULL, temp_scnprintf);
