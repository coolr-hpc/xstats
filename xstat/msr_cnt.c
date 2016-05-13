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

static uint64_t energy_restart(void **ctx, uint64_t last) {
    uint64_t laste = (uint64_t) *ctx;
    uint64_t eread;
    rdmsrl(MSR_PKG_ENERGY_STATUS, eread);
    *ctx = (void *) eread;
    eread &= 0xffffffff;
    if (eread < laste) {
        eread += 0x100000000LLU;
    }
    return eread - laste;
}

static struct xstat_counter energy_counter = __XSTAT_CNT(energy, NULL, NULL, energy_restart, NULL, NULL);

static uint64_t eunit_restart(void **ctx, uint64_t last) {
    uint64_t units;
    rdmsrl(MSR_RAPL_POWER_UNIT, units);
    return (units >> 8) & 0x1f;
}

static struct xstat_counter eunit_counter = __XSTAT_CNT(eunit, NULL, NULL, eunit_restart, NULL, NULL);

#define MSR_CORE_PERF_LIMIT_REASONS_RST_MASK 0xffffffff0000ffffULL
static uint64_t perflmt_restart(void **ctx, uint64_t last) {
    uint64_t perf_limit;
    rdmsrl(MSR_CORE_PERF_LIMIT_REASONS, perf_limit);
    wrmsrl(MSR_CORE_PERF_LIMIT_REASONS, perf_limit & MSR_CORE_PERF_LIMIT_REASONS_RST_MASK);
    return perf_limit;
}

static struct xstat_counter perflmt_counter = __XSTAT_CNT(perflmt, NULL, NULL, perflmt_restart, NULL, NULL);
