#include <linux/device.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaicheng Zhang");
MODULE_DESCRIPTION("X Stat for Intel Processors");

struct xstat_node {
    struct device dev;
};
#define to_node(device) container_of(device, struct xstat_node, dev)

struct xstat_node *xstat_nodes[MAX_NUMNODES];

static ssize_t show_ctrl_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return 0;
}

static ssize_t store_ctrl_attr(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    return 0;
}

static ssize_t show_period_attr(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return 0;
}

static ssize_t store_period_attr(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
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
        struct device *dev,
        struct device_attribute *attr,
        char *buf) {
    return 0;
}

static ssize_t show_last_attr(
        struct device *dev,
        struct device_attribute *attr,
        char *buf) {
    return 0;
}

// static struct class_attribute ctrl_attr = __ATTR(ctrl, 0666, show_ctrl_attr, store_ctrl_attr);
// static struct class_attribute period_attr = __ATTR(period, 0666, show_period_attr, store_period_attr);
// static struct device_attribute reset_attr = __ATTR(reset, 0222, NULL, store_reset_attr);
static struct device_attribute stat_attr = __ATTR(stat, 0444, show_stat_attr, NULL);
static struct device_attribute last_attr = __ATTR(last, 0444, show_last_attr, NULL);

static struct class_attribute xstat_class_attr[] = {
    __ATTR(ctrl, 0777, show_ctrl_attr, store_ctrl_attr),
    __ATTR(period, 0777, show_period_attr, store_period_attr),
    __ATTR(reset, 0444, NULL, store_reset_attr),
    __ATTR_NULL,
};

static struct attribute *xstat_node_attrs[] = {
    &stat_attr,
    &last_attr,
    NULL,
};

static struct attribute_group xstat_node_attr_group = {
    .attrs = xstat_node_attrs,
};

static struct attribute_group *xstat_node_attr_groups[] = {
    &xstat_node_attr_group,
    NULL,
};

static struct class xstat_class = {
    .name = "xstat",
    .owner = THIS_MODULE,

    .class_attrs = xstat_class_attr,
    .dev_groups = xstat_node_attr_groups,
};

static void xstat_node_device_release(struct device *dev) {
    struct xstat_node *node = to_node(dev);
    kfree(node);
}

static int register_xstat_node(int nid) {
    int err = 0;
    struct xstat_node *node;

    if (node_online(nid)) {
        node = kzalloc(sizeof(struct xstat_node),  GFP_KERNEL);
        if (!node)
            return -ENOMEM;

        xstat_nodes[nid] = node;

        node->dev.id = nid;
        node->dev.release = xstat_node_device_release;
        node->dev.class = &xstat_class;

        err = device_register(&node->dev);
    }

    return err;
}

static void unregister_xstat_node(int nid) {
    device_unregister(&xstat_nodes[nid]->dev);
    xstat_nodes[nid] = NULL;
}

static int __init xstat_init(void) {
    int ret, i;
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
    for (i = 0; i < MAX_NUMNODES; i++) {
        if (xstat_nodes[i])
            unregister_xstat_node(i);
    }
    class_unregister(&xstat_class);
}

module_init(xstat_init);
module_exit(xstat_exit);
