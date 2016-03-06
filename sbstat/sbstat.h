#ifndef _MICSTAT_H_
#define _MICSTAT_H_

#include <linux/types.h>

#define MICSTAT_LEN 16

#define __MIC_COUNTER(aname, ascnprintf, arestart, areset) { .init = NULL, .data = NULL, .name = #aname, .scnprintf = ascnprintf, .restart = arestart, .reset = areset, .exit = NULL}
#define __PERF_COUNTER(aname) { .init = perf_init, .data = &perf_##aname##_data, .name = #aname, .reset = perf_reset, .restart = perf_restart, .exit = perf_exit}
#define __NULL_COUNTER { .init = NULL, .data = NULL, .name = "", .scnprintf = NULL, .restart = NULL, .reset = NULL, .exit = NULL}
#define GET_BITS(l,r,v) (((v) >> (r)) & ((1U << ((l) - (r) +1)) -1))

struct micstat_counter;

typedef uint64_t (*func_restart) (struct micstat_counter *cnt);
typedef void (*func_reset) (struct micstat_counter *cnt);
typedef int (*func_scnprintf) (char *buf, int limit, uint64_t data);
typedef int (*func_init) (struct micstat_counter *cnt);
typedef void (*func_exit) (struct micstat_counter *cnt);

struct micstat_buffer {
    int nbuf;
    int ncnt;
    int base;
    int next;
    int size;
    spintlock_t lock;
    uint64_t data[0];
};

#define WORKING_BUF(buf, i) (buf).data[i]
#define BUFFER(buf, i, j) (buf).data[((i)+1)*((buf).ncnt) + (j)]

struct micstat_counter {
    char name[MICSTAT_LEN];
    func_scnprintf  scnprintf;
    func_restart    restart;
    func_reset      reset;
    func_init       init;
    func_exit       exit;
    void            *data;
};

struct micstat_context {
    char tag[MICSTAT_LEN];
    int cpu;
    int ncnt;
    micstat_counter *counters;
    micstat_buffer *buf;
};

#endif
