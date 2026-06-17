#include "../stats.h"
#include <assert.h>

int main(void) {
    uint64_t total, idle;
    /* user nice system idle iowait irq softirq steal guest guest_nice */
    const char *st = "cpu  100 0 100 800 0 0 0 0 0 0\n"
                     "cpu0 50 0 50 400 0 0 0 0 0 0\n";
    assert(stats_parse_cpu(st, &total, &idle) == 0);
    assert(total == 1000);       /* sum of all fields */
    assert(idle == 800);         /* idle + iowait */
    assert(stats_parse_cpu("garbage\n", &total, &idle) == -1);

    int used, tot;
    const char *mi = "MemTotal:        1024000 kB\n"
                     "MemFree:           10000 kB\n"
                     "MemAvailable:     512000 kB\n";
    assert(stats_parse_mem(mi, &used, &tot) == 0);
    assert(tot == 1000);         /* 1024000 kB / 1024 = 1000 MiB */
    assert(used == 500);         /* (1024000-512000)/1024 = 500 MiB */
    assert(stats_parse_mem("MemTotal: 1 kB\n", &used, &tot) == -1); /* no MemAvailable */

    int c;
    assert(stats_parse_temp("48123\n", &c) == 0);
    assert(c == 48);
    assert(stats_parse_temp("oops", &c) == -1);
    return 0;
}
