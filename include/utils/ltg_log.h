#ifndef __YLOG_H__
#define __YLOG_H__

#include <stdint.h>
#include <semaphore.h>

#include "lock.h"
#include "ltg_list.h"

typedef enum {
        YLOG_STDERR,
        YLOG_FILE,
        YLOG_SYSLOG,
} logmode_t;

typedef struct {
        int logfd;
        int count;
        time_t time;
        ltg_rwlock_t lock;
        logmode_t logmode;
} log_t;

typedef enum {
        YLOG_TYPE_STD, /*standard log optput*/
        YLOG_TYPE_PERF, /*performance log optput*/
        YLOG_TYPE_BALANCE, /*balance log type*/
        YLOG_TYPE_RAMDISK, /*ramdisk log type, record each io crc */
        YLOG_TYPE_MAX,
} logtype_t;

extern int log_init(logmode_t, const char *file);
extern int log_destroy(void);
extern int log_write(logtype_t type, const char *msg);
extern int log_getfd();

#endif
