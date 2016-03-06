#include <linux/delay.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/cpu.h>
#include <linux/perf_event.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <asm/msr.h>

#include "stat.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaicheng Zhang");
MODULE_DESCRIPTION("Haswell Stat Stat");

#define NR_CPUS_MIC 32
#define CHECK_RET(ret) do { \
    if (ret < 0) { \
        return ret; \
    } \
} while (0)
#define USE_KTHREAD

static bool on;
static unsigned int period;

// noop
// static uint64_t noop_restart(struct micstat_counter *cnt) {return 0;}
static void noop_reset(struct micstat_counter *cnt) {}

struct uint32_helper {
    uint32_t value;
};

// sample tracer: ts, intv, and oh.
static uint64_t st_prev;
static uint64_t st_current;
static uint64_t st_oh;
static uint64_t get_time(void) {
    static struct timespec ts;
    do_posix_clock_monotonic_gettime(&ts);
    return timespec_to_ns(&ts);
}
static uint64_t ts_restart(struct micstat_counter *cnt) {
    st_prev = st_current;
    st_current = get_time();
    return st_current;
}
static uint64_t intv_restart(struct micstat_counter *cnt) {
    return st_current - st_prev;
}
static uint64_t oh_restart(struct micstat_counter *cnt) {
    st_oh = get_time();
    return st_oh - st_current;
}
static void ts_reset(struct micstat_counter *cnt) {
    st_prev = st_current = st_oh = get_time();
}

// jif
#ifdef MICSTAT_JIF
static uint64_t jif;
static uint64_t jif_restart(struct micstat_counter *cnt) {
    uint64_t prev = jif;
    jif = jiffies_64;
    return jif - prev;
}
static void jif_reset(struct micstat_counter *cnt) {
    jif = jiffies_64;
}
#endif

// temperature
static uint64_t temp_restart(struct micstat_counter *cnt) {
    uint64_t tmsr;
    rdmsrl(MSR_IA32_THERM_STATUS, tmsr);
    return (tmsr >> 16) & 0x7f;
}

// cycles
#ifdef MICSTAT_CYCLES
static uint64_t cycles_data;
static uint64_t cycles_restart(struct micstat_counter *cnt) {
    uint64_t prev = cycles_data;
    cycles_data = local_clock();
    return cycles_data - prev;
}

static void cycles_reset(struct micstat_counter *cnt) {
    cycles_data = local_clock();
}
#endif

// perf
struct perf_event_value {
    uint64_t total;
    uint64_t enabled;
    uint64_t running;
};

struct perf_event_config {
    __u32       type;
    __u32       config;
    struct perf_event *events[NR_CPUS_MIC];
    struct perf_event_value values[NR_CPUS_MIC];
};

static void overflow_handler(struct perf_event *event,
                             struct perf_sample_data *data, struct pt_regs *regs) {
    printk(KERN_INFO "micstat: overflow_handler triggerd.\n");
}

static int perf_init(struct micstat_counter *cnt) {
    int i;
    struct perf_event_attr pe_attr;
    struct perf_event *event;
    struct perf_event_config *cfg = (struct perf_event_config *) cnt->data;
    struct perf_event **events = cfg->events;
    struct perf_event_value *values = cfg->values;

    memset(&pe_attr, 0, sizeof(pe_attr));
    pe_attr.type = cfg->type;
    pe_attr.size = sizeof(pe_attr);
    pe_attr.config = cfg->config;
    pe_attr.sample_period = 0;

    for (i = 0; i < NR_CPUS_MIC; i++) {
        events[i] = NULL;
        values[i].total = 0;
        values[i].running = 0;
        values[i].enabled = 0;
    }
    for (i = 0; i < NR_CPUS_MIC; i++) {
        event = perf_event_create_kernel_counter(&pe_attr, i, NULL, overflow_handler, NULL);
        if (IS_ERR(event) || event == NULL) {
            printk("micstat: error [code:%lld] on creating perf_event[%d:%x] at cpu %d\n",
                    (int64_t) event, cfg->type, cfg->config, i);
            return -1;
        }
        events[i] = event;
    }
    return 0;
}

static void perf_exit(struct micstat_counter *cnt) {
    struct perf_event_config *cfg = (struct perf_event_config *) cnt->data;
    struct perf_event **events = cfg->events;
    int i;
    for (i = 0; i < NR_CPUS_MIC; i++) {
        if (events[i]) {
            perf_event_release_kernel(events[i]);
            events[i] = NULL;
        }
    }
}

static uint64_t const perf_overflow_threshold = 2000000000000ULL; // 2T

static uint64_t perf_restart(struct micstat_counter *cnt) {
    struct perf_event_config *cfg = (struct perf_event_config *) cnt->data;
    struct perf_event **events = cfg->events;
    struct perf_event_value *values = cfg->values;
    u64 enabled, running;
    uint64_t total = 0;
    int i;
    uint64_t ret;
    uint64_t tmp;
    for (i = 0; i < NR_CPUS_MIC; i++) {
        if (events[i]) {
            ret = perf_event_read_value(events[i], &enabled, &running);
            if (ret < values[i].total || enabled < values[i].enabled || running <= values[i].running) {
#ifdef MICSTAT_DEBUG_PERF_OVERFLOW
                printk(KERN_INFO "micstat [%s] overflow at cpu %d.\n"
                       "micstat total %llu -> %llu, enabled %llu -> %llu, running %llu -> %llu.\n",
                       cnt->name, i, values[i].total, ret, values[i].enabled, enabled, values[i].running, running);
#endif
            } else {
                tmp = (uint64_t) ((double) (ret - values[i].total) * (enabled - values[i].enabled) / (running - values[i].running) + 0.5);
                if (tmp > perf_overflow_threshold) {
#ifdef MICSTAT_DEBUG_PERF_OVERFLOW
                    printk("micstat [%s] reading %llu at cpu %i may overflows. ignored.\n"
                           "micstat total %llu -> %llu, enabled %llu -> %llu, running %llu -> %llu.\n",
                           cnt->name, tmp, i, values[i].total, ret, values[i].enabled, enabled, values[i].running, running);
#endif
                } else {
                    total += tmp;
                }
            }
            values[i].total = ret;
            values[i].enabled = enabled;
            values[i].running = running;
        }
    }

#ifdef MICSTAT_DEBUG_PERF_OVERFLOW
    if (total > perf_overflow_threshold) {
        printk("micstat [%s] total(%llu) may overflows.\n", cnt->name, total);
    }
#endif

    return total;
}

static void perf_reset(struct micstat_counter *cnt) {
    // Do not support perf_reset for now.
}

#define PERF_RAW_CONFIG(name, code) static struct perf_event_config perf_##name##_data = { .type = PERF_TYPE_RAW, .config = code }
PERF_RAW_CONFIG(cyc, 0x003c);
PERF_RAW_CONFIG(inst, 0x00c0);
PERF_RAW_CONFIG(llcref, 0x4f2e);
PERF_RAW_CONFIG(llcmiss, 0x412e);
PERF_RAW_CONFIG(br, 0x00c4);
PERF_RAW_CONFIG(brmiss, 0x00c5);
PERF_RAW_CONFIG(l2lin, 0x07f4);

// counters
// counters[0] must be ts(timestamp), counters[1] must be intv(interval), and last element must be oh(overhead)
struct micstat_counter counters[] = {
    __MIC_COUNTER(ts, NULL, ts_restart, ts_reset),
    __MIC_COUNTER(intv, NULL, intv_restart, noop_reset),
#ifdef MICSTAT_JIF
    __MIC_COUNTER(jif, NULL, jif_restart, jif_reset),
#endif
#ifdef MICSTAT_CYCLES
    __MIC_COUNTER(cycles, NULL, cycles_restart, cycles_reset),
#endif
    __MIC_COUNTER(temp, NULL, temp_restart, noop_reset),
    __PERF_COUNTER(cyc),
    __PERF_COUNTER(inst),
    __PERF_COUNTER(llcref),
    __PERF_COUNTER(llcmiss),
    __PERF_COUNTER(br),
    __PERF_COUNTER(brmiss),
    __PERF_COUNTER(l2lin),
    __MIC_COUNTER(oh, NULL, oh_restart, noop_reset),
};

#define MICSTAT_NCNT (sizeof(counters) / sizeof(counters[0]))
#define MICSTAT_NBUF 128

static uint64_t buffer[MICSTAT_NBUF][MICSTAT_NCNT];
static int buffer_base;
static int buffer_next;
static int buffer_size;
static spinlock_t buffer_lock;
static uint64_t working_buf[MICSTAT_NCNT];

static int roll_buffer(void) {
    int i;
    for (i = 0; i < MICSTAT_NCNT; i++) {
        working_buf[i] = counters[i].restart(&counters[i]);
    }
    spin_lock_bh(&buffer_lock);
    for (i = 0; i < MICSTAT_NCNT; ++i) {
        buffer[buffer_next][i] = working_buf[i];
    }
    buffer_next = (buffer_next + 1) % MICSTAT_NBUF;
    if (buffer_size < MICSTAT_NBUF ){
        buffer_size ++;
    } else {
        buffer_base = (buffer_base + 1) % MICSTAT_NBUF;
    }
    spin_unlock_bh(&buffer_lock);
    return 0;
}

static int print_buffer(char *charbuf, int limit, uint64_t *buf) {
    char *ptr = charbuf;
    int ret;
    int i;

    ret = scnprintf(ptr, limit, "{");
    CHECK_RET(ret);
    ptr += ret;
    limit -= ret;

    for (i = 0; i < MICSTAT_NCNT; ++i) {
        if (counters[i].scnprintf != NULL) {
            ret = counters[i].scnprintf(ptr, limit, buf[i]);
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        } else {
            ret = scnprintf(ptr, limit, "\"%s\":%llu,", counters[i].name, buf[i]);
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        }
    }

    ret = scnprintf(ptr, limit, "}\n");
    CHECK_RET(ret);
    ptr += ret;
    limit -= ret;

    return  ptr - charbuf;
}

static spinlock_t ctrl_lock;
#if defined(USE_KTHREAD)
static struct task_struct *kthread_task;
static int kthread_function(void *data) {
    unsigned int oh;
    unsigned int tosleep;
    spin_lock_bh(&ctrl_lock);
    while (on) {
        roll_buffer();
        oh = (st_oh - st_current) / 1000000;
        tosleep = period - oh;
        if (period < oh * 5) {
            // force overhead / period < 20%
            tosleep = oh * 4;
        }
        spin_unlock_bh(&ctrl_lock);
        msleep(tosleep);
        if (kthread_should_stop()) return 0;
        spin_lock_bh(&ctrl_lock);
    }
    spin_unlock_bh(&ctrl_lock);
    return 0;
}
static void timer_start(void) {
    kthread_task = kthread_create(kthread_function, (void*) NULL, "micstat");
#ifdef MICSTAT_BIND_KTHREAD
    kthread_bind(kthread_task, 0);
#endif
    if (!IS_ERR(kthread_task))
        wake_up_process(kthread_task);
}
static void timer_cleanup(void) {
    kthread_stop(kthread_task);
}
#elif defined(USE_HTIMER)
static struct hrtimer htimer;
static ktime_t kt_periode;
static enum hrtimer_restart htimer_function(struct hrtimer *unused) {
    bool lon ;
    spin_lock_bh(&ctrl_lock);
    lon = on;
    spin_unlock_bh(&ctrl_lock);
    if (lon) {
        roll_buffer();
        hrtimer_forward_now(&htimer, kt_periode);
        return HRTIMER_RESTART;
    } else {
        return HRTIMER_NORESTART;
    }
}
static void timer_start(void) {
    kt_periode = ktime_set(1, 104167);
    hrtimer_start(&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
    htimer.function = timer_function;
    hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
}
static void timer_cleanup(void) {
    hrtimer_cancel(&htimer);
}
#
#else   // USE_TIMER
static struct timer_list timer;
static void timer_function(unsigned long data) {
    if (on) {
        roll_buffer();
        mod_timer(&timer, jiffies + msecs_to_jiffies(1000));
    }
}
static void timer_start(void) {
    setup_timer(&timer, timer_function, 0);
    mod_timer(&timer, jiffies + msecs_to_jiffies(1000));
}
static void timer_cleanup(void) {
    del_timer(&timer);
}
#endif

// freq

static void set_on(void) {
    bool lon;
    spin_lock_bh(&ctrl_lock);
    lon = on;
    on = true;
    spin_unlock_bh(&ctrl_lock);
    if (!lon) {
        int i;
        for (i = 0; i < MICSTAT_NCNT; i++) {
            counters[i].reset(&counters[i]);
        }
        timer_start();
    }
}

static void set_off(void) {
    bool lon;
    spin_lock_bh(&ctrl_lock);
    lon = on;
    on = false;
    spin_unlock_bh(&ctrl_lock);
    if (lon) {
        timer_cleanup();
    }
}

static ssize_t micstat_show_period(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return sprintf(buf, "%u\n", period);
}

static ssize_t micstat_store_period(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    unsigned long tmp;
    int ret;
    ret = kstrtoul(buf, 0, &tmp);
    if (ret == 0 && tmp > 100 && tmp < 10000) {
        period = tmp;
    }
    return count;
}

static ssize_t micstat_show_clear(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return 0;
}

static ssize_t micstat_store_clear(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    if ((count >= 2 && strncmp(buf, "on", 2) == 0) || 
        (count >= 1 && strncmp(buf, "1", 1) == 0)) {
        spin_lock_bh(&buffer_lock);
        buffer_size = 0;
        buffer_next = buffer_base;
        spin_unlock_bh(&buffer_lock);
        return count;
    }
    return 0;
}

static ssize_t micstat_show_ctrl(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    if (on) {
        return sprintf(buf, "on\n");
    } else {
        return sprintf(buf, "off\n");
    }
}

static ssize_t micstat_store_ctrl(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    if (count >= 2 && strncmp(buf, "on", 2) == 0) {
        set_on();
        return count;
    }
    if (count >= 3 && strncmp(buf, "off", 3) == 0) {
        set_off();
        return count;
    }
    return count;
}

static ssize_t micstat_show_stat(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    // WARN: will fail when one data point is larger than 4Kb
    int limit = PAGE_SIZE;
    int ret;
    int count = 0;
    char *ptr = buf;

    // WARN: NOT thread safe. lock on buffer!
    spin_lock_bh(&buffer_lock);
    while (limit > (PAGE_SIZE / 2) && buffer_size > 0) {
        ret = print_buffer(ptr, limit, buffer[buffer_base]);
        if (ret < 0) {
            break;
        }
        count += ret;
        ptr += ret;
        limit -= ret;
        buffer_base = (buffer_base + 1) % MICSTAT_NBUF;
        buffer_size -= 1;
    }
    spin_unlock_bh(&buffer_lock);
    return count;
}

static ssize_t micstat_show_last(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    int ret = 0;
    int buffer_last;
    char *ptr = buf;
    spin_lock_bh(&buffer_lock);
    if (buffer_size > 0) {
        buffer_last = (buffer_base + buffer_size - 1) % MICSTAT_NBUF;
        ret = print_buffer(ptr, PAGE_SIZE, buffer[buffer_last]);
    }
    spin_unlock_bh(&buffer_lock);
    return ret;
}


static struct class_attribute micstat_attr[] = {
    __ATTR(ctrl,    0666, micstat_show_ctrl, micstat_store_ctrl),
    __ATTR(period,  0666, micstat_show_period, micstat_store_period),
    __ATTR(clear,   0666, micstat_show_clear, micstat_store_clear),
    __ATTR(stat,    0444, micstat_show_stat, 0),
    __ATTR(last,    0444, micstat_show_last, 0),
    __ATTR_NULL,
};

static struct class micstat_class = {
    .name = "micstat",
    .owner = THIS_MODULE,
    .class_attrs = micstat_attr,
};

static int __init micstat_init(void) {
    int err = 0;
    int i;

    printk(KERN_INFO "micstat_init with NR_CPUS_MIC=%d\n", NR_CPUS_MIC);

    spin_lock_init(&ctrl_lock);
    on = false;
    period = 1000;      // default sampling rate at 1Hz

    // init counters
    for (i = 0; i < MICSTAT_NCNT; i++) {
        if (counters[i].init) {
            err = counters[i].init(&counters[i]);
            if (err != 0) {
                goto fail_cnt_init;
            }
        }
    }

    buffer_base = 0;
    buffer_next = 0;
    buffer_size = 0;
    spin_lock_init(&buffer_lock);

    err = class_register(&micstat_class);
    if (err) {
        printk("micstat.init: cannot register class 'micstat', error %d\n", err);
        goto fail_class;
    }

    return 0;
fail_cnt_init:
    for (; i >= 0; i--) {
        if (counters[i].exit) {
            counters[i].exit(&counters[i]);
        }
    }
fail_class:

    return err;
}

static void __exit micstat_exit(void) {
    int i;
    printk(KERN_INFO "micstat_exit\n");

    printk(KERN_INFO "micstat: set off\n");
    set_off();

    printk(KERN_INFO "micstat: class unregister\n");
    class_unregister(&micstat_class);

    printk(KERN_INFO "micstat: exit counters\n");
    for (i = 0; i < MICSTAT_NCNT; i++) {
        if (counters[i].exit) {
            counters[i].exit(&counters[i]);
        }
    }
}

module_init(micstat_init);
module_exit(micstat_exit);
