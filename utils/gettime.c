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

#define TIME_ZONE 8

typedef struct {
        time_t time;
        struct timeval tv;
        uint32_t cycle;
} gettime_t;

#define GETTIME_CYCLE 100
#define GETTIME_CYCLE_PRINT 100000000

void IO_FUNC gettime_refresh(void *ctx)
{
        gettime_t *gettime = core_tls_get(ctx, VARIABLE_GETTIME);

        LTG_ASSERT(gettime);
        gettime->cycle++;
        if (unlikely((gettime->cycle % GETTIME_CYCLE == 0) || ltgconf_global.performance_analysis)) {
                gettimeofday(&gettime->tv, NULL);
        }

        if (unlikely(gettime->cycle % GETTIME_CYCLE_PRINT == 0)) {
                DBUG("gettime cycle\n");
        }
}

time_t IO_FUNC gettime()
{
        struct timeval tv;
        
        _gettimeofday(&tv, NULL);

        return tv.tv_sec;
}

int IO_FUNC _gettimeofday(struct timeval *tv, struct timezone *tz)
{
        (void) tz;
        
        gettime_t *gettime = core_tls_get(NULL, VARIABLE_GETTIME);

        if (likely(gettime && (ltgconf_global.polling_timeout == 0 || ltgconf_global.rdma))) {
                *tv = gettime->tv;
        } else {
                gettimeofday(tv, NULL);
        }

        return 0;
}

int gettime_private_init()
{
        int ret;
        gettime_t *gettime;

        ret = ltg_malloc((void **)&gettime, sizeof(*gettime));
        if (ret)
                GOTO(err_ret, ret);

        memset(gettime, 0x0, sizeof(*gettime));
        gettimeofday(&gettime->tv, NULL);

        core_tls_set(VARIABLE_GETTIME, gettime);

        return 0;
err_ret:
        return ret;
}

struct tm *localtime_safe(time_t *_time, struct tm *tm_time)
{
        time_t time = *_time;
        long timezone = TIME_ZONE;
        const char Days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        uint32_t n32_Pass4year;
        uint32_t n32_hpery;

        time=time + (timezone * 60 * 60);

        if(time < 0)
        {
                time = 0;
        }
        tm_time->tm_sec=(int)(time % 60);
        time /= 60;
        tm_time->tm_min=(int)(time % 60);
        time /= 60;
        n32_Pass4year=((unsigned int)time / (1461L * 24L));
        tm_time->tm_year=(n32_Pass4year << 2)+70;
        time %= 1461L * 24L;
        for (;;)
        {
                n32_hpery = 365 * 24;
                if ((tm_time->tm_year & 3) == 0)
                {
                        n32_hpery += 24;
                }
                if (time < n32_hpery)
                {
                        break;
                }
                tm_time->tm_year++;
                time -= n32_hpery;
        }
        tm_time->tm_hour=(int)(time % 24);
        time /= 24;
        time++;
        if ((tm_time->tm_year & 3) == 0)
        {
                if (time > 60)
                {
                        time--;
                }
                else
                {
                        if (time == 60)
                        {
                                tm_time->tm_mon = 1;
                                tm_time->tm_mday = 29;
                                return tm_time;
                        }
                }
        }
        for (tm_time->tm_mon = 0; Days[tm_time->tm_mon] < time;tm_time->tm_mon++)
        {
                time -= Days[tm_time->tm_mon];
        }

        tm_time->tm_mday = (int)(time);
        return tm_time;
}

