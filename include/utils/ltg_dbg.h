#ifndef __LLIB_DBG_H__
#define __LLIB_DBG_H__

#include <time.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <assert.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "ltg_log.h"
#include "dbg_proto.h"
#include "ltg_conf.h"

/* for gettime, localtime_safe etc */
#include "ltg_misc.h"
#include "macros.h"

extern int rdma_running;

// DEBUG
#define DEBUG_MODE       ENABLE_DEBUG_MODE


#define DPERF_PATH            "/msgctl/perf"
#define DGOTO_PATH            "/msgctl/backtrace"
#define DLEVEL_PATH           "/msgctl/level"

#define DBUG_PATH             "/msgctl/dbug"
#define DBUG_UTILS_PATH        "/msgctl/sub/utils"
#define DBUG_CORE_PATH        "/msgctl/sub/core"
#define DBUG_NET_PATH        "/msgctl/sub/net"
#define DBUG_RPC_PATH        "/msgctl/sub/rpc"
#define DBUG_MEM_PATH        "/msgctl/sub/mem"

extern int __d_info__;
extern int __d_goto__;
extern int __d_bug__;
extern int __d_level__;
extern void _backtrace(const char *name);

static inline pid_t __gettid(void)
{
        return syscall(SYS_gettid);
}

void dbg_sub_init();
void dbg_info(int on);
void dbg_goto(int on);
void dbg_bug(int on);
void dbg_level(int level);

#define ERRNO_KEEP_SYSTEM 256
#define ERRNO_KEEP_LTG 256

const char *ltg_strerror(int errno);
int ltg_errno_set(int idx, const char *_str);

#if defined(CMAKE_SOURCE_PATH_SIZE)

#define __FILENAME__ (__FILE__ + CMAKE_SOURCE_PATH_SIZE)

#else

#define __FILENAME__ (__FILE__)

#endif

#define __MSG__(mask) (((mask) & (__d_bug__ | __d_info__ | __D_WARNING | __D_ERROR | __D_FATAL)) \
                               || ((utils_dbg & (mask)) && (utils_sub & DBG_SUBSYS)))
#define __LEVEL__(mask) ((mask) & (__d_level__))

void  __attribute__((noinline)) dbg_log_write(const int logtype, const int size, const int mask,
                const char *filename, const int line, const char *function,
                const char *format, ...);
                
#define __D_MSG__(logtype, size, mask, format, a...)                    \
        if (unlikely(__MSG__(mask)) && unlikely(__LEVEL__(mask))) {     \
                dbg_log_write(logtype, size, mask, __FILENAME__, __LINE__, __FUNCTION__, format, ##a); \
        }


#define D_MSG(mask, format, a...)  __D_MSG__(YLOG_TYPE_STD, 4096 - 128, mask, format, ## a)
#define DINFO(format, a...)        D_MSG(__D_INFO, "INFO: "format, ## a)
#define DINFO1(size, format, a...) __D_MSG__(YLOG_TYPE_STD, size, __D_INFO, "INFO: "format, ## a)
#define DWARN(format, a...)        D_MSG(__D_WARNING, "WARNING: "format, ## a)
#define DWARN1(size, format, a...) __D_MSG__(YLOG_TYPE_STD, size, __D_WARNING, "WARNING: "format, ## a)
#define DERROR(format, a...)       D_MSG(__D_ERROR, "ERROR: "format, ## a)
#define DFATAL(format, a...)       D_MSG(__D_FATAL, "FATAL: "format, ## a)
#define DBUG(format, a...)         D_MSG(__D_BUG, "DBUG: "format, ## a)

#define DINFO_PERF(format,a...)    __D_MSG__(YLOG_TYPE_PERF, 4069 - 128, __D_INFO, "INFO:"format, ## a)
#define DWARN_PERF(format,a...)    __D_MSG__(YLOG_TYPE_PERF, 4069 - 128, __D_WARNING, "WARNING:"format, ## a)

#define D_MSG_RAW(logtype, mask, format, a...)                                   \
        do {                                                            \
                if (__MSG__(mask)) {                                    \
                        char __d_msg_buf[2 * 1024];                     \
                        snprintf(__d_msg_buf, 2 * 1024, format, ##a);   \
                                                                        \
                        (void) log_write(logtype, __d_msg_buf);        \
                }                                                       \
        } while (0);

#define DBUG_RAW(format, a...)         D_MSG_RAW(YLOG_TYPE_STD, __D_BUG, format, ## a)

#ifdef LTG_DEBUG
#define LTG_ASSERT(exp)                                                \
        do {                                                            \
                if (unlikely(!(exp))) {                                 \
                        DERROR("!!!!!!!!!!assert fail, coredump:%d\n", ltgconf_global.coredump); \
                        if (ltgconf_global.coredump) {                  \
                                abort();                                \
                        } else {                                        \
                                char info[MAX_INFO_LEN];                \
                                _backtrace(info);                       \
                                if (ltgconf_global.restart) {           \
                                        EXIT(EAGAIN);                   \
                                } else {                                \
                                        EXIT(100);                      \
                                }                                       \
                        }                                               \
                }                                                       \
        } while (0)

#else
#define LTG_ASSERT(exp) {}
#endif

#define GOTO(label, ret)                                               \
        do {                                                            \
                if (__d_goto__) {                                       \
                        DWARN("Process leaving via (%d)%s\n", ret, strerror(ret)); \
                }                                                       \
                goto label;                                             \
        } while (0)

#define __NULL__ 0
#define __WARN__ 1
#define __DUMP__ 2

#define UNIMPLEMENTED(__arg__)                                          \
        do {                                                            \
                if (__arg__ == __WARN__ || __arg__ == __DUMP__) {       \
                        DWARN("unimplemented yet\n");                   \
                        if (__arg__ == __DUMP__) {                      \
                                LTG_ASSERT(0);                             \
                        }                                               \
                }                                                       \
        } while (0)

extern uint32_t utils_loglevel;
extern uint32_t utils_dbg;
extern uint32_t utils_sub;

void closecoredump();
#define FATAL_RETVAL 255

#define EXIT(__ret__)                             \
        do {       					\
		rdma_running = 0;                               \
                closecoredump();                                \
                DWARN("exit worker (%u) %s\n", __ret__, strerror(__ret__)); \
                exit(__ret__);                                          \
        } while (0)

#if 1
#define STATIC
#else
#define STATIC static
#endif

#define DINFO_NOP(format, a...)
#define DWARN_NOP(format, a...)
#define DERROR_NOP(format, a...)
#define DFATAL_NOP(format, a...)

#if 1
        #define DDEV DINFO
#else
        #define DDEV DBUG
#endif

int dmsg_init();
int dmsg_init_sub(const char *name, uint32_t flag);
int dmsg_init_misc(const char *name, const char *value,
                   int (*callback)(const char *buf, uint32_t flag),
                   uint32_t flag);
#endif
