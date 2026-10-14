/* Hand-written C baseline for bench_loop.sg (1e8 int adds).
 * Compiled by the same clang the AOT linker uses; -O2 like generated code. */
#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec ts0, ts1;
    timespec_get(&ts0, TIME_UTC);
    long long s = 0;
    long long i = 0;
    while (i < 100000000LL) {
        s = s + i;
        i = i + 1;
        /* in-loop read/write barrier: LLVM folds linear recurrences to
         * closed form, so the loop must see opaque operand updates */
        __asm__ volatile("" : "+r"(s), "+r"(i));
    }
    timespec_get(&ts1, TIME_UTC);
    /* opaque sink: stop -O2 from constant-folding the whole loop */
    __asm__ volatile("" : : "r"(s), "r"(i) : "memory");
    double el = (double)(ts1.tv_sec - ts0.tv_sec) +
                (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e9;
    printf("%lld\n", s);
    printf("elapsed_s=%.6f\n", el);
    return 0;
}
