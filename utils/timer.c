

#include <stdint.h>
#include <sys/timerfd.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"
#include "ltg_core.h"

#define TIMER_IDLE 1024 * 1024
#define TIMER_TYPE_MISC 0
#define TIMER_TYPE_SCHE 1

#define ytime_t uint64_t /*time in microsecond*/

typedef struct {
        ytime_t time;
        func_t func;
        void *obj;
} entry_t;

typedef struct {
        struct skiplist *list;
        ltg_spinlock_t lock;
        int count;
        int seq;
        sem_t sem;
} group_t;

typedef struct {
        ytime_t max;
        ytime_t min;
        int thread;
        int maxlevel;
        int chunksize;
        int private;
        group_t group;
} ytimer_t;

static ytimer_t *__timer__ = NULL;

ytime_t ytime_gettime()
{
        int ret;
        struct timeval tv;

        ret = _gettimeofday(&tv, NULL);
        if (ret)
                LTG_ASSERT(0);

        return tv.tv_sec * 1000000 + tv.tv_usec;
}

int ytime_getntime(struct timespec *ntime)
{
        int ret;
        struct timeval tv;

        ret = _gettimeofday(&tv, NULL);
        if (ret)
                GOTO(err_ret, ret);

        ntime->tv_sec = tv.tv_sec;
        ntime->tv_nsec = tv.tv_usec * 1000;

        return 0;
err_ret:
        return ret;
}

void ytime_2ntime(ytime_t ytime, struct timespec *ntime)
{
        ntime->tv_sec = ytime / 1000000;
        ntime->tv_nsec = (ytime % 1000000) * 1000;
}

int __timer_cmp(const void *key, const void *data)
{
        int ret;
        ytime_t *keyid, *dataid;

        keyid = (ytime_t *)key;
        dataid = (ytime_t *)data;

        if (*keyid < *dataid)
                ret = -1;
        else if (*keyid > *dataid)
                ret = 1;
        else
                ret = 0;

        return ret;
}

static void __timer_expire__(group_t *group)
{
        int ret;
        ytime_t now;
        void *first;
        entry_t *ent;

        now = ytime_gettime();
        while (1) {
                ret = skiplist_get1st(group->list, (void **)&first);
                if (unlikely(ret)) {
                        break;
                }

                ent = first;

                if (now >= ent->time) {
                        group->count--;
                        (void) skiplist_del(group->list, first, (void **)&first);

                        DBUG("func %p\n", ent->obj);

                        ANALYSIS_BEGIN(0);
                        ent->func(ent->obj);
                        ANALYSIS_END(0, 1000 * 100, NULL);

                        huge_mem_free(ent);
                } else {
                        break;
                }
        }
}


static void *__timer_expire(void *_args)
{
        int ret;
        group_t *group;
        struct timespec ts;
        ytime_t now;
        void *first;
        entry_t *ent;

        group = _args;

        ret = ytime_getntime(&ts);
        if (unlikely(ret)) {
                LTG_ASSERT(0);
        }

        ts.tv_sec += TIMER_IDLE;

        while (srv_running) {
                ret = _sem_timedwait(&group->sem, &ts);
                if (unlikely(ret)) {
                        if (ret != ETIMEDOUT)
                                LTG_ASSERT(0);
                }

                while (srv_running) {
                        ret = ltg_spin_lock(&group->lock);
                        if (unlikely(ret))
                                LTG_ASSERT(0);

                        ret = skiplist_get1st(group->list, (void **)&first);
                        if (unlikely(ret)) {
                                if (ret == ENOENT) {
                                        ret = ltg_spin_unlock(&group->lock);
                                        if (unlikely(ret))
                                                LTG_ASSERT(0);

                                        ts.tv_sec += TIMER_IDLE;

                                        break;
                                } else
                                        LTG_ASSERT(0);
                        }

                        ret = ltg_spin_unlock(&group->lock);
                        if (unlikely(ret))
                                LTG_ASSERT(0);

                        now = ytime_gettime();

                        ent = first;

                        if (now >= ent->time) {
                                ret = ltg_spin_lock(&group->lock);
                                if (unlikely(ret))
                                        LTG_ASSERT(0);

                                group->count--;
                                (void) skiplist_del(group->list, first, (void **)&first);

                                ret = ltg_spin_unlock(&group->lock);
                                if (unlikely(ret))
                                        LTG_ASSERT(0);

                                DBUG("func %p\n", ent->obj);

                                ANALYSIS_BEGIN(0);
                                ent->func(ent->obj);
                                ANALYSIS_END(0, IO_WARN, NULL);

                                huge_mem_free(ent);
                        } else {
                                ytime_2ntime(ent->time, &ts);
                                break;
                        }
                }
        }

	return NULL;
}


int timer_init(int private)
{
        int ret, len;
        void *ptr;
        ytimer_t *_timer;
        group_t *group;
        pthread_t th;
        pthread_attr_t ta;

        /* group [ sche , misc ] */
        len = sizeof(ytimer_t);

        ret = ltg_malloc(&ptr, len);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        _timer = ptr;
        _timer->min = 0;
        _timer->max = (unsigned long long)-1;
        _timer->maxlevel = SKIPLIST_MAX_LEVEL;
        _timer->chunksize = SKIPLIST_CHKSIZE_DEF;
        _timer->private = private;

        group = &_timer->group;
        ret = skiplist_create(__timer_cmp, _timer->maxlevel, _timer->chunksize,
                              (void *)&_timer->min, (void *)&_timer->max,
                              &group->list);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        group->count = 0;

        if (private) {
                _timer->thread = sche_getid();
                core_tls_set(VARIABLE_TIMER, _timer);
        } else {
                LTG_ASSERT(__timer__ == NULL);
                _timer->thread = -1;
                __timer__ = _timer;

                ret = ltg_spin_init(&group->lock);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                ret = sem_init(&group->sem, 0, 0);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                (void) pthread_attr_init(&ta);
                (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
                //pthread_attr_setstacksize(&ta, 1<<19);
                
                ret = pthread_create(&th, &ta, __timer_expire, (void *)group);
                if (unlikely(ret))
                        GOTO(err_ret, ret); 
        }

        return 0;
err_ret:
        return ret;
}

static int __timer_insert(group_t *group, suseconds_t usec, void *obj, func_t func)
{
        int ret, retry = 0;
        entry_t *ent;
        uint64_t tmo;

        tmo = ytime_gettime();
        tmo += usec;

        ent = huge_mem_alloc(64);
        ent->time = tmo;
        ent->obj = obj;
        ent->func = func;

retry:
        ret = skiplist_put(group->list, (void *)&ent->time, (void *)ent);
        if (unlikely(ret)) {
                if (ret == EEXIST) {
                        if (retry > 1024) {
                                LTG_ASSERT(0);
                        }

                        if (retry > 256)
                                DINFO("retry %u, count %u\n", retry, group->count);

                        ent->time = ent->time + (group->seq ++) % 1024;
                        retry ++;
                        goto retry;
                }

                GOTO(err_ret, ret);
        }

        group->count++;

        return 0;
err_ret:
        huge_mem_free(ent);
        return ret;
}

int timer_insert(const char *name, void *ctx, func_t func, suseconds_t usec)
{
        int ret;
        group_t *group;
        ytimer_t *timer;

        timer = core_tls_get(VARIABLE_TIMER);
        if (likely(timer)) {
                DBUG("timer insert %s %ju\n", name, usec);
                LTG_ASSERT(timer->thread == sche_getid());
        } else {
                DBUG("timer insert %s %ju\n", name, usec);
                timer = __timer__;
        }

        group = &timer->group;
        if (unlikely(!timer->private)) {
                ret = ltg_spin_lock(&group->lock);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }
        
        ret = __timer_insert(group, usec, ctx, func);
        if (unlikely(ret))
                GOTO(err_lock, ret);

        if (unlikely(!timer->private)) {
                ltg_spin_unlock(&group->lock);
        }

        if (unlikely(!timer->private)) {
                sem_post(&group->sem);
        }
                
        return 0;
err_lock:
        if (unlikely(!timer->private)) {
                ltg_spin_unlock(&group->lock);
        }
err_ret:
        return ret;
}

#if 0
static void __timer_expire_task(void *arg)
{
        return __timer_expire__(arg);
}
#endif

void IO_FUNC timer_expire(void *ctx)
{
        ytimer_t *timer;
        timer = core_tls_getfrom1(ctx, VARIABLE_TIMER);

        if (unlikely(timer == NULL))
                return;

#if 0
        sche_task_new("timer expire", __timer_expire_task, &timer->group, -1);
#else
        __timer_expire__(&timer->group);
#endif
}
