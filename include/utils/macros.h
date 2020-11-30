#ifndef __NEWDEF_H__
#define __NEWDEF_H__

#include <time.h>
#include <stdint.h>
#include <stdarg.h>

#include "ltg_def.h"

#define IO_WARN (1000 * 1000)
#define IO_INFO (1000 * 500)

#if 1
#define S_LTG  __attribute__((section(".xltg")))
#else
#define S_LTG
#endif

#ifndef likely
#define likely(x)       __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x)     __builtin_expect(!!(x), 0)
#endif

#if 0
#define DIO1(__id__, __op__, __size__, __offset__, __buf__)              \
        do {                                                            \
                DINFO("DIO "CHKID_FORMAT" %s size %u off %ju\n",       \
                      CHKID_ARG(__id__), __op__, __size__, __offset__); \
        } while (0)

#define DIO2(__io__, __op__, __buf__)                                   \
        do {                                                            \
                DINFO("DIO "CHKID_FORMAT" %s size %u off %ju clock %jd\n", \
                      CHKID_ARG(&(__io__)->id), __op__, (__io__)->size, \
                      (__io__)->offset,                                 \
                      (__io__)->vclock.clock);                          \
        } while (0)

#else

#define DIO1(__id__, __op__, __size__, __offset__, __buf__)
#define DIO2(__io__, __op__, __buf__)

#endif

#define ARRAY_POP(__head__, __count__, __total__)                         \
        do {                                                            \
                memmove((__head__), (__head__) + (__count__),           \
                        sizeof(__head__[0]) * ((__total__) - (__count__))); \
                memset((__head__) + (__total__) - (__count__), 0x0,    \
                       sizeof(__head__[0]) * (__count__));                 \
        } while (0);

#define ARRAY_PUSH(__head__, __count__, __len__)                        \
        do {                                                            \
                memmove(&(__head__) + __count__, &(__head__),           \
                        sizeof(__head__) * (__len__));                  \
                memset(&(__head__), 0x0,                                \
                       sizeof(__head__) * (__count__));                 \
        } while (0);

#define ARRAY_SORT(__head__, __count__, __cmp__)                        \
        do {                                                            \
                qsort(__head__, __count__, sizeof(__head__[0]), __cmp__); \
        } while (0);


#define ARRAY_COPY(__src__, __dist__, __len__)                         \
        do {                                                            \
                memcpy(&(__src__), &(__dist__),                         \
                       sizeof(__src__) * (__len__));                    \
        } while (0);


#if ENABLE_ANALYSIS
#define ANALYSIS_BEGIN(mark)                    \
        ltg_time_t t1##mark;                    \
        int used##mark;                         \
        static time_t __warn__##mark;           \
        (void ) __warn__##mark;                 \
                                                \
        if (unlikely(ltgconf_global.performance_analysis)) {\
                _microsec_update_now(&t1##mark);         \
        }

#define ANALYSIS_START(mark, __str)             \
        ltg_time_t t1##mark, t2##mark;          \
        uint64_t used##mark;                    \
                                                \
        if (ltgconf_global.performance_analysis) {\
                DWARN_PERF("analysis %s start\n", (__str) ? (__str) : ""); \
                _microsec_update_now(&t1##mark) \
        }                                       \

#define ANALYSIS_RESET(mark)                    \
        if (ltgconf_global.performance_analysis) {\
                _microsec_update_now(&t1##mark);\
        }

#define ANALYSIS_TIMED_END(mark, __str)                                                 \
        if (ltgconf_global.performance_analysis) {                                      \
                used##mark = _microsec_time_used_from_now(&t1##mark)                    \
                DERROR(""#mark" time %ju us %s\n", used##mark, (__str) ? (__str) : ""); \
        }

#define ANALYSIS_END(mark, __usec, __str)                               \
        if (unlikely(ltgconf_global.performance_analysis)) {            \
                used##mark = _microsec_time_used_from_now(&t1##mark);   \
                if (used##mark > (__usec)) {                            \
                        if (used##mark > IO_WARN * 2) {                     \
                                DERROR("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        } else if (used##mark > IO_WARN) { \
                                DWARN("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        } else {                                        \
                                DINFO("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        }                                               \
                } \
        }

#define ANALYSIS_ASSERT(mark, __usec, __str)                            \
        if (ltgconf_global.performance_analysis) {                      \
                used##mark = _microsec_time_used_from_now(&t1##mark);   \
                LTG_ASSERT(used##mark < (__usec));                      \
        }                                                               \
        
#define ANALYSIS_QUEUE(mark, __usec, __str)                             \
        if (unlikely(ltgconf_global.performance_analysis)) {            \
                used##mark = _microsec_time_used_from_now(&t1##mark);   \
                if (used##mark) {                                       \
                        analysis_private_queue(__str ? __str : __FUNCTION__, NULL, used##mark); \
                }                                                       \
                if (used##mark > (__usec)) {                            \
                        if (used##mark > IO_WARN * 2) {                     \
                                DERROR("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        } else if (used##mark > IO_WARN) {              \
                                DWARN("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        } else {                                        \
                                DINFO_PERF("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        }                                               \
                }                                                       \
                _microsec_update_now(&t1##mark);                        \
                                                                        \
        }

#define ANALYSIS_UPDATE(mark, __usec, __str)                            \
        if (ltgconf_global.performance_analysis) {                      \
                used##mark = _microsec_time_used_from_now(&t1##mark)    \
                core_latency_update(used##mark);                        \
                if (used##mark > (__usec)) {                            \
                        if (used##mark > IO_WARN) {                     \
                                DWARN_PERF("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        } else {                                        \
                                DINFO_PERF("analysis used %fs %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                        }                                               \
                }                                                       \
        }
#else
#define ANALYSIS_BEGIN(mark)  {}
#define ANALYSIS_RESET(mark)   {}
#define ANALYSIS_QUEUE(mark, __usec, __str)     \
        do { \
        (void) __str; \
        } while (0);

#define ANALYSIS_END(mark, __usec, __str) \
        do { \
        (void) __str; \
        } while (0);

#define ANALYSIS_ASSERT(mark, __usec, __str)

#endif

#if ENABLE_SCHEDULE_LOCK_CHECK

#define USLEEP_RETRY(__err_ret__, __ret__, __labal__, __retry__, __max__, __sleep__) \
        if ((__retry__)  < __max__) {                                   \
                if (__retry__ % 10 == 0) {                              \
                        DINFO("retry %u/%u\n", (__retry__), __max__);   \
                }                                                       \
                                                \
                sche_assert_retry();                                \
                sche_task_sleep("none", __sleep__);                      \
                __retry__++;                                            \
                goto __labal__;                                 \
        } else                                                  \
                GOTO(__err_ret__, __ret__);

#else
#define USLEEP_RETRY(__err_ret__, __ret__, __labal__, __retry__, __max__, __sleep__) \
        if ((__retry__)  < __max__) {                                   \
                if (__retry__ % 5 == 0 && __retry__ > 3) {             \
                        DINFO("retry %u/%u\n", (__retry__), __max__);   \
                }                                                       \
                sche_task_sleep("none", __sleep__);                      \
                __retry__++;                                            \
                goto __labal__;                                 \
        } else                                                  \
                GOTO(__err_ret__, __ret__);
#endif


#define _ceil(size, align) ((size) % (align) == 0 ? (size) / (align) : (size) / (align) + 1)
#define _min(x, y) ((x) < (y) ? (x) : (y))
#define _max(x, y) ((x) > (y) ? (x) : (y))

/* align only for 2 ** x */
#define _align_down(a, size)    ((a) & (~((size) - 1)) )
#define _align_up(a, size)      (((a) + (size) - 1) & (~((size) - 1)))

/* align for any type */
#define round_up(x, y) (((x) % (y) == 0)? (x) : ((x) + (y)) / (y) * (y))
#define round_down(x, y) (((x) / (y)) * (y))

#ifndef offsetof
#define offsetof(t, m) ((size_t) &((t *)0)->m)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({ \
		typeof(((const type *)0)->member)(*__mptr) = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); })
#endif


typedef void (*func_t)(void *arg);
typedef int (*func1_t)(void *, void *);
typedef void (*func2_t)(void *, void *, void *);
typedef void (*func3_t)(void *, void *, void *, void *);

typedef int (*func_va_t)(va_list ap);

typedef void * (*thread_proc)(void *);
typedef void *(*thread_func)(void *);

#endif
