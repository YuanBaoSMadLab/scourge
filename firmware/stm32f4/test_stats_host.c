/* Host-side proof of the stats ring-buffer traversal bug.
 * Build: gcc -O1 -o test_stats test_stats.c stats.c && ./test_stats
 */
#include <stdio.h>
#include "stats.h"

int main(void)
{
    int i, mn, mx, ok;
    long var;
    /* Push 256 (== STATS_CAP) values 1000..1255, then 7 more 300..306.
     * After wrap, the live window is [300..306] + [1006..1255] (255 items),
     * i.e. min should be 300, max 1255.  A linear 0..count-1 scan instead
     * reads stale ring[] slots and mixes in dead data. */
    stats_reset();
    for (i = 0; i < 256; i++) stats_push(1000 + i);
    for (i = 0; i < 7; i++)   stats_push(300 + i);

    mn = stats_min();
    mx = stats_max();
    var = stats_variance();

    printf("count=%d (expect %d)\n", stats_count(), STATS_CAP);
    printf("min=%d (expect 300)\n", mn);
    printf("max=%d (expect 1255)\n", mx);

    ok = (mn == 300 && mx == 1255 && stats_count() == STATS_CAP);
    printf("mean=%d (expect ~1194)\n", stats_mean());
    printf("var=%ld\n", var);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL <-- ring traversal bug");
    return ok ? 0 : 1;
}
