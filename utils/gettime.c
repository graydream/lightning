#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"
#include "ltg_core.h"

typedef struct {
        time_t time;
        struct timeval tv;
        uint32_t cycle;
} gettime_t;

#define GETTIME_CYCLE 100
#define GETTIME_CYCLE_PRINT 100000000

inline time_t INLINE gettime()
{
        struct timeval tv;
        
        _gettimeofday(&tv, NULL);

        return tv.tv_sec;
}

inline int INLINE _gettimeofday(struct timeval *tv, struct timezone *tz)
{
        (void) tz;
        
        gettime_t *gettime = core_tls_get(NULL, VARIABLE_GETTIME);

        if (likely(gettime)) {
                *tv = gettime->tv;
        } else {
                gettimeofday(tv, NULL);
        }

        return 0;
}

inline static void S_LTG INLINE __gettime(void *_core, void *var, void *_gettime)
{
        gettime_t *gettime = _gettime;

        (void) _core;
        (void) var;

        gettime->cycle++;
        if ((gettime->cycle % GETTIME_CYCLE == 0)
                     || ltgconf_global.performance_analysis) {
                gettimeofday(&gettime->tv, NULL);
        }

#if 0
        if (unlikely(gettime->cycle % GETTIME_CYCLE_PRINT == 0)) {
                DBUG("gettime cycle\n");
        }
#endif
}

static int __gettime_init()
{
        int ret;
        gettime_t *gettime;

        ret = ltg_malloc((void **)&gettime, sizeof(*gettime));
        if (ret)
                GOTO(err_ret, ret);

        memset(gettime, 0x0, sizeof(*gettime));
        gettimeofday(&gettime->tv, NULL);

        core_tls_set(VARIABLE_GETTIME, gettime);

        ret = core_register_routine("gettime", __gettime, gettime);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int gettime_init()
{
        int ret;

        ret = core_init_modules("epoch", __gettime_init, NULL);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
