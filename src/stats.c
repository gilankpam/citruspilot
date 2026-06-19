#define _GNU_SOURCE
#include "stats.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int stats_parse_cpu(const char *s, uint64_t *total, uint64_t *idle)
{
    const char *p = strstr(s, "cpu ");
    if (!p) return -1;
    p += 4;
    const char *nl = strchr(p, '\n');
    const char *end = nl ? nl : p + strlen(p);

    uint64_t v[10] = {0};
    int n = 0;
    const char *q = p;
    while (n < 10 && q < end) {
        char *e;
        unsigned long long x = strtoull(q, &e, 10);
        if (e == q) break;       /* no more digits on this line */
        v[n++] = x;
        q = e;
    }
    if (n < 4) return -1;        /* need at least user/nice/system/idle */

    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += v[i];
    *total = t;
    *idle  = v[3] + (n > 4 ? v[4] : 0);   /* idle + iowait */
    return 0;
}

static int find_kb(const char *s, const char *key, long *kb)
{
    const char *p = strstr(s, key);
    if (!p) return -1;
    *kb = strtol(p + strlen(key), NULL, 10);
    return 0;
}

int stats_parse_mem(const char *s, int *used_mb, int *total_mb)
{
    long tot, avail;
    if (find_kb(s, "MemTotal:", &tot)) return -1;
    if (find_kb(s, "MemAvailable:", &avail)) return -1;
    long used = tot - avail;
    if (used < 0) used = 0;
    *total_mb = (int)(tot / 1024);
    *used_mb  = (int)(used / 1024);
    return 0;
}

int stats_parse_temp(const char *s, int *deg_c)
{
    char *e;
    long milli = strtol(s, &e, 10);
    if (e == s) return -1;
    *deg_c = (int)(milli / 1000);
    return 0;
}

void stats_init(stats_t *st)
{
    st->prev_total = st->prev_idle = 0;
    st->have_prev = 0;
}

static char *slurp(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return NULL;
    buf[r] = '\0';
    return buf;
}

void stats_sample(stats_t *st, stats_sample_t *out)
{
    char buf[8192];
    out->cpu_pct = out->mem_used_mb = out->mem_total_mb = out->temp_c = -1;

    if (slurp("/proc/stat", buf, sizeof buf)) {
        uint64_t total, idle;
        if (stats_parse_cpu(buf, &total, &idle) == 0) {
            if (st->have_prev && total > st->prev_total) {
                uint64_t dt = total - st->prev_total;
                uint64_t di = idle  - st->prev_idle;
                out->cpu_pct = (int)((100 * (dt - di)) / dt);
            }
            st->prev_total = total;
            st->prev_idle  = idle;
            st->have_prev  = 1;
        }
    }

    if (slurp("/proc/meminfo", buf, sizeof buf))
        stats_parse_mem(buf, &out->mem_used_mb, &out->mem_total_mb);

    for (int z = 0; z < 16; z++) {
        char path[64], tb[32];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        if (slurp(path, tb, sizeof tb)) {
            int c;
            if (stats_parse_temp(tb, &c) == 0 && c > out->temp_c)
                out->temp_c = c;
        }
    }
}
