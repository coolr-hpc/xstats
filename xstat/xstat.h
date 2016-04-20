#ifndef _XSTAT_H_
#define _XSTAT_H_

#include <linux/types.h>

#define XSTAT_CNT_LEN   8
struct xstat_counter {
    char name[XSTAT_CNT_LEN];
    int (*init) (const struct cpumask *mask, void *data, void **ctx);
    void (*exit) (void **ctx);
    uint64_t (*restart) (void **ctx, uint64_t last);
    void (*reset) (void **ctx);
    int (*scnprintf) (char *buf, int limit, uint64_t data, void **ctx);
    void *data;
};

#define __XSTAT_CNT(aname, ainit, aexit, arestart, areset, ascnprintf) { \
    .name = #aname, \
    .init = ainit, \
    .exit = aexit, \
    .restart = arestart, \
    .reset = areset, \
    .scnprintf = ascnprintf, \
    .data = NULL \
}

#endif
