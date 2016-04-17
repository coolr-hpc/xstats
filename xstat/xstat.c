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

#define STRBUFLEN    8
struct xstat_node {
    int id;
    spinlock_t lock;

    struct task_struct *task;

    const struct cpumask *mask;

    char stat_name[STRBUFLEN];
    char last_name[STRBUFLEN];
    struct class_attribute stat_attr;
    struct class_attribute last_attr;
};

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

    spin_lock_bh(&ctrl_lock);
    if (ctrl_on) {
        ctrl_on = false;

        for (i = 0; i < MAX_NUMNODES; i++) {
            if (xstat_nodes[i]) {
                node = xstat_nodes[i];
                spin_lock_bh(&node->lock);
                node->task = NULL;
                spin_unlock_bh(&node->lock);
            }
        }
    }
    spin_unlock_bh(&ctrl_lock);
}

static int init_counters(struct xstat_node *node) {
    return 0;
}

static int roll_buffer(struct xstat_node *node) {
    return 0;
}

static int kthread_function(void *data) {
    struct xstat_node *node = (struct xstat_node *) data;

    init_counters(node);

    while (ctrl_on && node->task == current) {
        roll_buffer(node);
        msleep(ctrl_period);
    }

    do_exit(0);
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
    if (ret == 0 && tmp > 100 && tmp < 10000) {
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
    return 0;
}

static ssize_t show_stat_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return 0;
}

static ssize_t show_last_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return 0;
}

static struct class_attribute xstat_class_attr[] = {
    __ATTR(ctrl, 0777, show_ctrl_attr, store_ctrl_attr),
    __ATTR(period, 0777, show_period_attr, store_period_attr),
    __ATTR(reset, 0444, NULL, store_reset_attr),
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

        err = class_create_file(&xstat_class, &node->stat_attr);
        err = class_create_file(&xstat_class, &node->last_attr);
    }

    return err;
}

static void unregister_xstat_node(int nid) {
    struct xstat_node *node = xstat_nodes[nid];
    if (node) {
        class_remove_file(&xstat_class, &node->stat_attr);
        class_remove_file(&xstat_class, &node->last_attr);
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
