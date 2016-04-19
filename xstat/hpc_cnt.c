#include <linux/perf_event.h>

struct perf_event_config {
    uint32_t type;
    uint32_t config;
};

struct perf_counter_per_cpu {
    uint64_t total;
    uint64_t enabled;
    uint64_t running;
    struct perf_event *event;
};

struct perf_counter_context {
    struct perf_event_config config;
    const struct cpumask *mask;
    struct perf_counter_per_cpu events[0];
};

static uint64_t const perf_overflow_threshold = 2000000000000ULL; // 2T
static void perf_overflow_handler(struct perf_event *event,
                                  struct perf_sample_data *data, struct pt_regs *regs) {
}

static int perf_init(const struct cpumask *mask, void *data, void **ctx) {
    struct perf_event_attr pe_attr;
    struct perf_event *event;
    struct perf_event_config *cfg = (struct perf_event_config *) data;
    int nevents = cpumask_weight(mask);
    struct perf_counter_context *perf_ctx = kzalloc(sizeof(struct perf_counter_context)
            + nevents * sizeof(struct perf_counter_per_cpu), GFP_KERNEL);
    int cpu, i;

    *ctx = perf_ctx;
    perf_ctx->mask = mask;
    perf_ctx->config = *cfg;

    memset(&pe_attr, 0, sizeof(pe_attr));
    pe_attr.type = cfg->type;
    pe_attr.size = sizeof(pe_attr);
    pe_attr.config = cfg->config;
    pe_attr.sample_period = 0;

    i = 0;
    for_each_cpu_mask(cpu, *mask) {
        event = perf_event_create_kernel_counter(&pe_attr, cpu, NULL, perf_overflow_handler, NULL);
        if (IS_ERR(event) || event == NULL) {
            printk("xstat: error creating perf_event config = %d, cpu = %d.\n",
                cfg->config, cpu);
            perf_ctx->events[i].event = NULL;
            i++;
            continue;
        }
        perf_ctx->events[i].event = event;
        i++;
    }
    return 0;
}

static void perf_exit(void **ctx) {
    struct perf_counter_context *perf_ctx = (struct perf_counter_context *) *ctx;
    int i;
    for (i = 0; i < cpumask_weight(perf_ctx->mask); i++) {
        if (perf_ctx->events[i].event) {
            perf_event_release_kernel(perf_ctx->events[i].event);
        }
    }
    kfree(perf_ctx);
}

static uint64_t perf_restart(void **ctx, uint64_t last) {
    struct perf_counter_context *perf_ctx = (struct perf_counter_context *) *ctx;
    struct perf_event *event;
    uint64_t enabled, running;
    uint64_t total = 0;
    int i;
    uint64_t ret, tmp;

    for (i = 0; i < cpumask_weight(perf_ctx->mask); i++) {
        event = perf_ctx->events[i].event;
        if (event) {
            ret = perf_event_read_value(event, &enabled, &running);
            if (ret < perf_ctx->events[i].total ||
                enabled < perf_ctx->events[i].enabled ||
                running <= perf_ctx->events[i].running) {
            } else {
                tmp = (uint64_t) ((double) (ret - perf_ctx->events[i].total)
                      * (enabled - perf_ctx->events[i].enabled)
                      / (running - perf_ctx->events[i].running) + 0.5);
                if (tmp > perf_overflow_threshold) {
                } else {
                    total += tmp;
                }
            }
            perf_ctx->events[i].total = ret;
            perf_ctx->events[i].enabled = enabled;
            perf_ctx->events[i].running = running;
        }
    }
    return total;
}

static void perf_reset(void **ctx) {}

#define PERF_RAW_CONFIG(name, code) static struct perf_event_config perf_##name##_data = { .type = PERF_TYPE_RAW, .config = code }
PERF_RAW_CONFIG(cyc, 0x003c);
PERF_RAW_CONFIG(inst, 0x00c0);
PERF_RAW_CONFIG(llcref, 0x4f2e);
PERF_RAW_CONFIG(llcmiss, 0x412e);
PERF_RAW_CONFIG(br, 0x00c4);
PERF_RAW_CONFIG(brmiss, 0x00c5);
PERF_RAW_CONFIG(l2lin, 0x07f4);

#define PERF_COUNTER(aname) static struct xstat_counter aname##_counter = { \
    .name = #aname, \
    .init = perf_init, \
    .exit = perf_exit, \
    .restart = perf_restart, \
    .reset = perf_reset, \
    .scnprintf = NULL, \
    .data = &perf_##aname##_data \
}
PERF_COUNTER(cyc);
PERF_COUNTER(inst);
PERF_COUNTER(llcref);
PERF_COUNTER(llcmiss);
PERF_COUNTER(br);
PERF_COUNTER(brmiss);
PERF_COUNTER(l2lin);
