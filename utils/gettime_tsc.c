#include <time.h>
#include <stdlib.h>

#include "ltg_utils.h"
/**
    *  * Macro to align a value to the multiple of given value. The resultant
    *   *   * value will be of the same type as the first parameter and will be no lower
    *    *    * than the first parameter.
    *     *     */
#define RTE_ALIGN_MUL_CEIL(v, mul) \
                (((v + (typeof(v))(mul) - 1) / ((typeof(v))(mul))) * (typeof(v))(mul))

/**
 *  *  * Macro to align a value to the multiple of given value. The resultant
 *   *   * value will be of the same type as the first parameter and will be no higher
 *    *    * than the first parameter.
 *     *     */
#define RTE_ALIGN_MUL_FLOOR(v, mul) \
                ((v / ((typeof(v))(mul))) * (typeof(v))(mul))

#define RTE_ALIGN_MUL_NEAR(v, mul)                              \
        ({                                                      \
         typeof(v) ceil = RTE_ALIGN_MUL_CEIL(v, mul);    \
         typeof(v) floor = RTE_ALIGN_MUL_FLOOR(v, mul);  \
         (ceil - v) > (v - floor) ? floor : ceil;        \
         })

inline uint64_t get_rdtsc(void)
{
        uint32_t lo, hi;
        __asm__ __volatile__ (
                        "rdtscp" : "=a"(lo), "=d"(hi)
                        );

        return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

static uint64_t __get_cpu_freq(void)
{
#define NS_PER_SEC 1E9
#define CYC_PER_10MHZ 1E7

        struct timespec sleeptime = {.tv_nsec = NS_PER_SEC / 10 }; /* 1/10 second */

        struct timespec t_start, t_end;
        uint64_t tsc_hz;

        if (clock_gettime(CLOCK_MONOTONIC_RAW, &t_start) == 0) {
                uint64_t ns, end, start = get_rdtsc();
                nanosleep(&sleeptime,NULL);
                clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);
                end = get_rdtsc();
                ns = ((t_end.tv_sec - t_start.tv_sec) * NS_PER_SEC);
                ns += (t_end.tv_nsec - t_start.tv_nsec);

                double secs = (double)ns/NS_PER_SEC;
                tsc_hz = (uint64_t)((end - start)/secs);
                return RTE_ALIGN_MUL_NEAR(tsc_hz, CYC_PER_10MHZ);
        }

        return 0;

}

inline uint64_t _microsec_used(uint64_t old, uint64_t new, uint64_t hz)
{
        return (new - old) * 1000 * 1000/ hz;
}

uint64_t cpu_freq_init()
{
        return __get_cpu_freq();
}
