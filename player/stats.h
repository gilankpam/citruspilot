#ifndef CITRUSPILOT_STATS_H
#define CITRUSPILOT_STATS_H
#include <stdint.h>

/* Carries the previous CPU jiffie counters so load is a delta across samples. */
typedef struct {
    uint64_t prev_total, prev_idle;
    int have_prev;
} stats_t;

typedef struct {
    int cpu_pct;       /* 0..100 aggregate load, or -1 if unknown */
    int mem_used_mb;   /* MiB used, or -1 */
    int mem_total_mb;  /* MiB total */
    int temp_c;        /* hottest thermal zone in degrees C, or -1 */
} stats_sample_t;

/* Pure parsers (no I/O) — unit tested. Return 0 on success, -1 on failure. */
int stats_parse_cpu(const char *proc_stat, uint64_t *total, uint64_t *idle);
int stats_parse_mem(const char *proc_meminfo, int *used_mb, int *total_mb);
int stats_parse_temp(const char *temp_millideg, int *deg_c);

/* Reset the CPU delta state. */
void stats_init(stats_t *st);
/* Read /proc + /sys and fill `out`. Uses `st` to compute CPU load vs the prior
 * call (first call leaves cpu_pct == -1, since there is no delta yet). */
void stats_sample(stats_t *st, stats_sample_t *out);
#endif
