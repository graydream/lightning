#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"
#include "core/sche.h"
#include "utils/fnotify.h"

int __d_info__ = __D_INFO;
int __d_goto__ = 1;
int __d_bug__ = 0;
int __d_level__ = __D_FATAL | __D_ERROR | __D_WARNING | __D_INFO;
uint32_t utils_dbg = 0;
uint32_t utils_sub = 0;

void dbg_sub_init()
{
        utils_dbg = ~0;
        utils_sub = 0;
}

void dbg_info(int i)
{
        __d_info__ = i ? __D_INFO : 0;
}

void dbg_goto(int i)
{
        __d_goto__ = i;
}

void dbg_bug(int i)
{
        __d_bug__ = i ? __D_BUG : 0;

}

void dbg_level(int i)
{
        switch(i) {
                case 1:
                        __d_level__ = __D_FATAL | __D_ERROR | __D_WARNING | __D_INFO | __D_BUG;
                        break;
                case 2:
                        __d_level__ = __D_FATAL | __D_ERROR | __D_WARNING | __D_INFO;
                        break;
                case 3:
                        __d_level__ = __D_FATAL | __D_ERROR | __D_WARNING;
                        break;
                case 4:
                        __d_level__ = __D_FATAL | __D_ERROR;
                        break;
                case 5:
                        __d_level__ = __D_FATAL;
                        break;
                default:
                        break;
        }
}

#if 1
void closecoredump()
{
       int ret = 0;
       struct rlimit rlim_new;

       rlim_new.rlim_cur = rlim_new.rlim_max = 0;
       ret = setrlimit(RLIMIT_CORE, &rlim_new);
       if (ret) {
                abort();
       }
}

int __dmsg_init_sub(const char *name, const char *value,
                  int (*callback)(const char *buf, uint32_t flag),
                  uint32_t flag)
{
        int ret;
        char path[MAX_PATH_LEN];

        snprintf(path, MAX_PATH_LEN, "/dev/shm/%s/%s", ltgconf_global.system_name, name);
        
        ret = fnotify_create(path, value, callback, flag);
        if (ret) {
                if (ret == ENOENT) {
                        //pass
                } else
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __dmsg_goto(const char *buf, uint32_t extra)
{
        int on = atoi(buf);

        (void) extra;

        DINFO("set goto %d\n", on);

        if (on) {
                dbg_goto(1);
        } else {
                dbg_goto(0);
        }

        return 0;
}

#endif

inline static int __dmsg_level(const char *buf, uint32_t extra)
{
        int level;

        (void) extra;
        DINFO("set level %s\n", buf);

        level = atoi(buf);
        if (level >= 1 && level <= 5) {
                dbg_level(level);
        } else {
                DERROR("set level %d fail, must in 1~5\n", level);
        }

        return 0;
}

inline static int __dmsg_sub(const char *buf, uint32_t extra)
{       
        int ret, on;

        on = atoi(buf);

        if (on == 0) {
                utils_sub &= ~extra;
        } else if (on == 1) {
                utils_sub |= extra;
        } else {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        DINFO("fnotify file modified: mask (0x%08x) %s\n", extra, on ? "on" : "off");

        return 0;
err_ret:
        return ret;
}       

int dmsg_init()
{
        int ret;

        DINFO("dmsg init %d\n", ltgconf_global.backtrace);

        if (ltgconf_global.backtrace) {
                ret = __dmsg_init_sub(DGOTO_PATH, "1", __dmsg_goto, 0);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                //dbg_goto(1);
        } else {
                ret = __dmsg_init_sub(DGOTO_PATH, "0", __dmsg_goto, 0);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                //dbg_goto(0);
        }

#if 1
        ret = __dmsg_init_sub(DLEVEL_PATH, "1", __dmsg_level, 1);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __dmsg_init_sub(DBUG_UTILS_PATH, "0", __dmsg_sub, S_LTG_UTILS);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __dmsg_init_sub(DBUG_CORE_PATH, "0", __dmsg_sub, S_LTG_CORE);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = __dmsg_init_sub(DBUG_NET_PATH, "0", __dmsg_sub, S_LTG_NET);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __dmsg_init_sub(DBUG_RPC_PATH, "0", __dmsg_sub, S_LTG_RPC);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __dmsg_init_sub(DBUG_MEM_PATH, "0", __dmsg_sub, S_LTG_MEM);
        if (unlikely(ret))
                GOTO(err_ret, ret);
#endif
        
        return 0;
err_ret:
        return ret;
}

int dmsg_init_sub(const char *name, uint32_t flag)
{
        int ret;
        char path[MAX_PATH_LEN];

        snprintf(path, MAX_PATH_LEN, "/msgctl/sub/%s", name);
        ret = __dmsg_init_sub(path, "0", __dmsg_sub, flag);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

#if 0
void sche_id(int *sid, int *taskid)
{
        *sid = -1;
        *taskid = -1;
}

#else
void sche_id(int *sid, int *taskid)
{
        core_t *core = core_self();
        if (core) {
                *sid = core->hash;
                *taskid = core->sche->running_task;
        } else {
                *sid = -1;
                *taskid = -1;
        }
}
#endif

void  __attribute__((noinline)) dbg_log_write(const int logtype, const int size, const int mask,
                const char *filename, const int line, const char *function,
                const char *format, ...)
{
        va_list arg;
        time_t __t = gettime();
        char *__d_msg_buf, *__d_msg_info, *__d_msg_time;
        struct tm __tm;
        int _s_id_, _taskid_;

        (void) mask;
        
        /**
         * | __d_msg_buf(size) | __d_msg_info(size) | __d_msg_time(32) |
         */
        __d_msg_buf = malloc(size * 2 + 32);
        __d_msg_info = (void *)__d_msg_buf + size;
        __d_msg_time = (void *)__d_msg_buf + size * 2;

        va_start(arg, format);
        vsnprintf(__d_msg_info, size, format, arg);
        va_end(arg);

        strftime(__d_msg_time, 32, "%F %T", localtime_safe(&__t, &__tm));
        sche_id(&_s_id_, &_taskid_);

        snprintf(__d_msg_buf, size,
                 "%s/%lu %lu/%lu %d/%d "
                 "%s:%d %s %s",
                 __d_msg_time, (unsigned long)__t,
                 (unsigned long)getpid( ),
                 (unsigned long)syscall(SYS_gettid),
                 _s_id_, _taskid_,
                 //_r_, _w_, _c_,
                 filename, line, function,
                 __d_msg_info);

        (void) log_write(logtype, __d_msg_buf);

        free(__d_msg_buf);
}
