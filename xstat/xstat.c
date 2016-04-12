#include <linux/device.h>
#include <linux/module.h>
#include <linux/sysfs.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaicheng Zhang");
MODULE_DESCRIPTION("X Stat for Intel Processors");

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

static int __init xstat_init(void) {
    int ret;

    ret = class_register(&xstat_class); 
    return ret;
}

static void __exit xstat_exit(void) {
    class_unregister(&xstat_class);
}

module_init(xstat_init);
module_exit(xstat_exit);
