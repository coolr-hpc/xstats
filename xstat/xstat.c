#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaicheng Zhang");
MODULE_DESCRIPTION("X Stat for Intel Processors");

#define CHECK_RET(ret) do { \
    if (ret < 0) { \
        return ret; \
    } \
} while (0)

#include "xstat.h"
#include "base_cnt.c"
#include "hpc_cnt.c"
#include "msr_cnt.c"

static struct xstat_counter *node_counters[] = {
    &ts_counter,
    &intv_counter,
    &cyc_counter,
    &inst_counter,
    &llcref_counter,
    &llcmiss_counter,
    &br_counter,
    &brmiss_counter,
    &l2lin_counter,
    &temp_counter,
    &energy_counter,
    &eunit_counter,
};

#define STRBUFLEN    8
struct xstat_node {
    int id;
    spinlock_t lock;

    struct task_struct *task;

    const struct cpumask *mask;

    char stat_name[STRBUFLEN];
    char last_name[STRBUFLEN];
    char reset_name[STRBUFLEN];
    struct class_attribute stat_attr;
    struct class_attribute last_attr;
    struct class_attribute reset_attr;

    void **ctxs;
    uint64_t *buffer;
    uint64_t *working_buf;
    int buffer_base;
    int buffer_next;
    int buffer_size;
};

#define XSTAT_NCNT (sizeof(node_counters) / sizeof(node_counters[0]))
#define XSTAT_NBUF 256

struct xstat_node *xstat_nodes[MAX_NUMNODES];

static spinlock_t ctrl_lock;
static unsigned int ctrl_period;
static bool ctrl_on;
static int kthread_function(void *data);

static int start_stat(void) {
    int i;
    struct xstat_node *node;

    spin_lock(&ctrl_lock);
    if (!ctrl_on) {
        ctrl_on = true;
        
        for (i = 0; i < MAX_NUMNODES; i++) {
            if (xstat_nodes[i]) {
                node = xstat_nodes[i];
                
                node->task = kthread_create_on_node(kthread_function, node,
                        i, "xstat_node%d", i);
                if (!IS_ERR(node->task)) {
                    kthread_bind(node->task, cpumask_first(node->mask));
                    wake_up_process(node->task);
                } else {
                    node->task = NULL;
                }
            }
        }
    }
    spin_unlock(&ctrl_lock);
    return 0;
}

static void stop_stat(void) {
    int i;
    struct xstat_node *node;

    spin_lock(&ctrl_lock);
    if (ctrl_on) {
        ctrl_on = false;

        for (i = 0; i < MAX_NUMNODES; i++) {
            if (xstat_nodes[i]) {
                node = xstat_nodes[i];
                kthread_stop(node->task);
                node->task = NULL;
            }
        }
    }
    spin_unlock(&ctrl_lock);
}

static int init_counters(struct xstat_node *node) {
    int i;
    for (i = 0; i < XSTAT_NCNT; i++) {
        if (node_counters[i]->init) {
            node_counters[i]->init(node->mask, node_counters[i]->data, &node->ctxs[i]);
        }
    }
    return 0;
}

static void exit_counters(struct xstat_node *node) {
    int i;
    for (i = 0; i < XSTAT_NCNT; i++) {
        if (node_counters[i]->exit) {
            node_counters[i]->exit(&node->ctxs[i]);
        }
    }
}

static int roll_buffer(struct xstat_node *node) {
    int i;
    for (i = 0; i < XSTAT_NCNT; i++) {
        node->working_buf[i] = node_counters[i]->restart(&node->ctxs[i],
                node->working_buf[i]);
    }
    spin_lock_bh(&node->lock);
    memcpy(&node->buffer[node->buffer_next * XSTAT_NCNT], node->working_buf, sizeof(uint64_t) * XSTAT_NCNT);
    node->buffer_next = (node->buffer_next + 1) % XSTAT_NBUF;
    if (node->buffer_size < XSTAT_NBUF) {
        node->buffer_size++;
    } else {
        node->buffer_base = (node->buffer_base + 1) % XSTAT_NBUF;
    }
    spin_unlock_bh(&node->lock);
    return 0;
}

static int kthread_function(void *data) {
    struct xstat_node *node = (struct xstat_node *) data;
    int tosleep;
    uint64_t after_roll;

    init_counters(node);

    while (true) {
        roll_buffer(node);
        after_roll = get_time();
        tosleep = (after_roll - (uint64_t) node->ctxs[1]) / 1000000;
        tosleep = ctrl_period - tosleep - 1;
        if (tosleep <= 0) tosleep = 0;
        if (kthread_should_stop()) goto out;
        if (tosleep) {
            msleep(tosleep);
        } else {
            schedule();
        }
        if (kthread_should_stop()) goto out;
    }

out:
    exit_counters(node);

    return 0;
}

static ssize_t show_ctrl_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    if (ctrl_on) {
        return sprintf(buf, "on\n");
    } else {
        return sprintf(buf, "off\n");
    }
}

static ssize_t store_ctrl_attr(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    if (count >= 2 && strncmp(buf, "on", 2) == 0) {
        start_stat();
    }
    if (count >= 3 && strncmp(buf, "off", 3) == 0) {
        stop_stat();
    }
    return count;
}

static ssize_t show_period_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return sprintf(buf, "%u\n", ctrl_period);
}

static ssize_t store_period_attr(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    unsigned long tmp;
    int ret;
    ret = kstrtoul(buf, 0, &tmp);
    if (ret == 0 && tmp > 0 && tmp < 10000) {
        spin_lock_bh(&ctrl_lock);
        ctrl_period = tmp;
        spin_unlock_bh(&ctrl_lock);
    }
    return count;

    return 0;
}

static ssize_t store_reset_attr(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    struct xstat_node *node = container_of(attr, struct xstat_node, reset_attr);
    if (count > 0) {
        spin_lock_bh(&node->lock);
        node->buffer_size = 0;
        node->buffer_next = node->buffer_base;
        spin_unlock_bh(&node->lock);
    }
    return count;
}

static int print_buffer(char *charbuf, int limit, struct xstat_node *node, uint64_t *buf) {
    char *ptr = charbuf;
    int ret;
    int i;

    ret = scnprintf(ptr, limit, "{");
    CHECK_RET(ret);
    ptr += ret;
    limit -= ret;

    for (i = 0; i < XSTAT_NCNT; i++) {
        if (node_counters[i]->scnprintf != NULL) {
            ret = node_counters[i]->scnprintf(ptr, limit, buf[i], &node->ctxs[i]);
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        } else {
            ret = scnprintf(ptr, limit, "\"%s\":%llu", node_counters[i]->name, buf[i]);
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        }
        if (i != XSTAT_NCNT - 1) {
            ret = scnprintf(ptr, limit, ",");
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        }
    }

    ret = scnprintf(ptr, limit, "}\n");
    CHECK_RET(ret);
    ptr += ret;
    limit -= ret;

    return ptr - charbuf;
}

static ssize_t show_stat_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    int limit = PAGE_SIZE;
    int ret;
    int count = 0;
    char *ptr = buf;
    struct xstat_node *node = container_of(attr, struct xstat_node, stat_attr);

    spin_lock_bh(&node->lock);
    while (limit > (PAGE_SIZE / 2) && node->buffer_size > 0) {
        ret = print_buffer(ptr, limit, node, &node->buffer[node->buffer_base * XSTAT_NCNT]);
        if (ret < 0) {
            break;
        }
        count += ret;
        ptr += ret;
        limit -= ret;
        node->buffer_base = (node->buffer_base + 1) % XSTAT_NBUF;
        node->buffer_size--;
    }
    spin_unlock_bh(&node->lock);
    return count;
}

static ssize_t show_last_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    struct xstat_node *node = container_of(attr, struct xstat_node, last_attr);
    int buffer_last;
    int ret = 0;
    spin_lock_bh(&node->lock);
    if (node->buffer_size > 0) {
        buffer_last = (node->buffer_base + node->buffer_size - 1) % XSTAT_NBUF;
        ret = print_buffer(buf, PAGE_SIZE, node, &node->buffer[buffer_last * XSTAT_NCNT]);
    }
    spin_unlock_bh(&node->lock);
    return ret;
}

static struct class_attribute xstat_class_attr[] = {
    __ATTR(ctrl, 0777, show_ctrl_attr, store_ctrl_attr),
    __ATTR(period, 0777, show_period_attr, store_period_attr),
    __ATTR_NULL,
};

static struct class xstat_class = {
    .name = "xstat",
    .owner = THIS_MODULE,

    .class_attrs = xstat_class_attr,
};

static int register_xstat_node(int nid) {
    int err = 0;
    struct xstat_node *node;

    if (node_online(nid)) {
        node = kzalloc(sizeof(struct xstat_node),  GFP_KERNEL);
        if (!node)
            return -ENOMEM;

        xstat_nodes[nid] = node;

        node->id = nid;
        node->mask = cpumask_of_node(nid);
        spin_lock_init(&node->lock);

        sprintf(node->stat_name, "stat%d", nid);
        node->stat_attr.attr.name = node->stat_name;
        node->stat_attr.attr.mode = 0444;
        node->stat_attr.show = show_stat_attr;
        sprintf(node->last_name, "last%d", nid);
        node->last_attr.attr.name = node->last_name;
        node->last_attr.attr.mode = 0444;
        node->last_attr.show = show_last_attr;
        sprintf(node->reset_name, "reset%d", nid);
        node->reset_attr.attr.name = node->reset_name;
        node->reset_attr.attr.mode = 0222;
        node->reset_attr.store = store_reset_attr;

        node->ctxs = kzalloc(sizeof(void *) * XSTAT_NCNT, GFP_KERNEL);
        node->buffer = kzalloc(sizeof(uint64_t) * XSTAT_NCNT * XSTAT_NBUF, GFP_KERNEL);
        node->working_buf = kzalloc(sizeof(uint64_t) * XSTAT_NCNT, GFP_KERNEL);

        err = class_create_file(&xstat_class, &node->stat_attr);
        err = class_create_file(&xstat_class, &node->last_attr);
        err = class_create_file(&xstat_class, &node->reset_attr);
    }

    return err;
}

static void unregister_xstat_node(int nid) {
    struct xstat_node *node = xstat_nodes[nid];
    if (node) {
        class_remove_file(&xstat_class, &node->reset_attr);
        class_remove_file(&xstat_class, &node->stat_attr);
        class_remove_file(&xstat_class, &node->last_attr);
        kfree(node->working_buf);
        kfree(node->buffer);
        kfree(node->ctxs);
        kfree(node);
        xstat_nodes[nid] = NULL;
    }
}

static int __init xstat_init(void) {
    int ret, i;

    spin_lock_init(&ctrl_lock);
    ctrl_on = false;
    ctrl_period = 1000;

    for (i = 0; i < MAX_NUMNODES; i++)
        xstat_nodes[i] = NULL;

    ret = class_register(&xstat_class); 

    for_each_online_node(i) {
        register_xstat_node(i);
    }
    return ret;
}

static void __exit xstat_exit(void) {
    int i;

    stop_stat();
    for (i = 0; i < MAX_NUMNODES; i++) {
        if (xstat_nodes[i])
            unregister_xstat_node(i);
    }
    class_unregister(&xstat_class);
}

module_init(xstat_init);
module_exit(xstat_exit);
