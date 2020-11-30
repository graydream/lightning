#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_net.h"
#include "ltg_core.h"
#include "ltg_utils.h"

#define DBG_SYLOCK

extern int srv_running;

#define RET_MAGIC 0x866aa9f0

#if 0
#define PLOCK_DMSG
#endif

typedef struct {
        struct list_head hook;
        char lock_type;
        task_t task;
        int magic;
        int retval;
        int prio;
        time_t lock_time;
} lock_wait_t;

#if PLOCK_CHECK
STATIC void __plock_check(plock_t *rwlock)
{
        if (unlikely(rwlock->thread == -1)) {
                rwlock->thread = sche_getid();
                LTG_ASSERT(rwlock->thread >= gloconf.main_loop_threads);
        } else {
                LTG_ASSERT(rwlock->thread == sche_getid());
        }
}
#endif

int plock_init(plock_t *rwlock, const char *name)
{
        INIT_LIST_HEAD(&rwlock->queue);

        static_assert(TASK_MAX < INT16_MAX, "static");
        
        (void) name;
        rwlock->writer = -1;
        rwlock->readers = 0;
#if PLOCK_CHECK
        rwlock->thread = -1;
#endif
#if PLOCK_DEBUG
        rwlock->last_unlock = 0;
        rwlock->count = 0;
        if (name) {
                if (strlen(name) > MAX_LOCK_NAME - 1)
                        LTG_ASSERT(0);

                strcpy(rwlock->name, name);
        } else {
                rwlock->name[0] = '\0';
        }
#endif

        return 0;
//err_ret:
//        return ret;
}

int plock_destroy(plock_t *rwlock)
{
        lock_wait_t *lock_wait = NULL;
        struct list_head list, *pos, *n;

        DBUG("destroy %p\n", rwlock);

        INIT_LIST_HEAD(&list);

        LTG_ASSERT(rwlock->readers == 0 && rwlock->writer == -1);
        list_splice_init(&rwlock->queue, &list);

        list_for_each_safe(pos, n, &list) {
                lock_wait = (void *)pos;
                list_del(pos);

                lock_wait->retval = EBUSY;
                lock_wait->magic = RET_MAGIC;
                sche_task_post(&lock_wait->task, 0, NULL);
        }

        return 0;
}

STATIC int S_LTG __plock_trylock(plock_t *rwlock, char type, char force, task_t *task)
{
        int ret;

#if PLOCK_CHECK
        __plock_check(rwlock);
#endif

        if (type == 'r') {
                if (rwlock->writer == -1 && (list_empty(&rwlock->queue) || force)) {
                        LTG_ASSERT(rwlock->readers - 1 < UINT16_MAX);
                        rwlock->readers++;
#ifdef PLOCK_DMSG
                        DINFO("read lock reader %u\n", rwlock->readers);
#endif
                } else {
#ifdef PLOCK_DMSG
                        DWARN("lock fail, readers %u writer task[%u]\n", rwlock->readers, rwlock->writer);
#endif
                        ret = EBUSY;
                        goto err_ret;
                }
        } else {
                if (rwlock->readers == 0 && rwlock->writer == -1) {
                        if (task)
                                rwlock->writer = task->taskid;
                        else
                                rwlock->writer = sche_taskid();
                        LTG_ASSERT(rwlock->writer != -1);
#ifdef PLOCK_DMSG
                        DINFO("write lock, task[%u]\n", rwlock->writer);
#endif
                } else {
#ifdef PLOCK_DMSG
                        DWARN("lock fail, readers %u writer task[%u]\n", rwlock->readers, rwlock->writer);
#endif
                        ret = EBUSY;
                        goto err_ret;
                }
        }

        return 0;
err_ret:
        return ret;
}

STATIC int S_LTG __plock_unlock(plock_t *rwlock)
{
        if (rwlock->writer != -1) {
                LTG_ASSERT(rwlock->readers == 0);
#ifdef PLOCK_DMSG
                DINFO("unlock, writer task[%u]\n", rwlock->writer);
#endif

                rwlock->writer =  -1;
        } else {
                LTG_ASSERT(rwlock->readers > 0);
                rwlock->readers--;
#ifdef PLOCK_DMSG
                DINFO("unlock, readers %u\n", rwlock->readers);
#endif
        }

        return 0;
}

#if 0
STATIC void __plock_register(plock_t *rwlock, char type, lock_wait_t *lock_wait, int prio)
{
        (void) prio;

        LTG_ASSERT(sche_running());
        lock_wait->task = sche_task_get();
        lock_wait->magic = 0;
        lock_wait->retval = 0;
        lock_wait->prio = 0;
        lock_wait->lock_type = type;
        lock_wait->lock_time = gettime();

        list_add_tail(&lock_wait->hook, &rwlock->queue);
}

#else

STATIC void __plock_register(plock_t *rwlock, char type, lock_wait_t *lock_wait, int prio)
{
        int found = 0, count = 0;
        lock_wait_t *tmp;
        struct list_head *pos;
        
        LTG_ASSERT(sche_running());
        lock_wait->task = sche_task_get();
        lock_wait->magic = 0;
        lock_wait->retval = 0;
        lock_wait->prio = prio;
        lock_wait->lock_type = type;
        lock_wait->lock_time = gettime();

        if (unlikely(prio)) {
                list_for_each(pos, &rwlock->queue) {
                        tmp = (void *)pos;
                        if (tmp->prio == 0) {
                                found = 1;
                                list_add_tail(&lock_wait->hook, pos);
                                break;
                        }

                        count++;
                }

                if (count > 0) {
                        DWARN("seek count %u\n", count);
                }
                
                if (found == 0) {
                        list_add_tail(&lock_wait->hook, &rwlock->queue);
                }
        } else {
                list_add_tail(&lock_wait->hook, &rwlock->queue);
        }
}

#endif

STATIC void __plock_timeout_check(void *args)
{
        int ret, count = 0, lock_timeout = 0;
        plock_t *rwlock = args;
        lock_wait_t *lock_wait;
        struct list_head list, *pos, *n;

        if (rwlock->writer != -1) {
#if PLOCK_DEBUG
                DWARN("lock %p, writer %d, readers %u count %u,"
                      " write locked, last %u\n", rwlock,
                      rwlock->writer, rwlock->readers,
                      rwlock->count, rwlock->last_unlock);
                sche_backtrace();
#endif
                return;
        } else {
#if PLOCK_DEBUG
                DWARN("lock %p, writer %d, readers %u count %u,"
                      " read locked, last %u\n", rwlock,
                      rwlock->writer, rwlock->readers,
                      rwlock->count, rwlock->last_unlock);

                sche_backtrace();
#endif
        }

        INIT_LIST_HEAD(&list);

        list_for_each_safe(pos, n, &rwlock->queue) {
                lock_wait = (void *)pos;

                if (lock_wait->lock_type == 'r') {
                        ret = __plock_trylock(rwlock, lock_wait->lock_type,
                                              1, &lock_wait->task);
                        if (unlikely(ret))
                                break;

                        list_del(pos);
                        list_add_tail(&lock_wait->hook, &list);
                        count++;

#if PLOCK_DEBUG
                        DWARN("force lock %p, writer %d, readers %u count %u,"
                              " last %u %d\n", rwlock,
                              rwlock->writer, rwlock->readers, rwlock->count,
                              rwlock->last_unlock, count);
#endif
                        sche_backtrace();                        
                }
        }

        list_for_each_safe(pos, n, &list) {
                lock_wait = (void *)pos;
                list_del(pos);

                lock_wait->retval = 0;
                lock_wait->magic = RET_MAGIC;
                sche_task_post(&lock_wait->task, 0, NULL);
        }

#if 1
        list_for_each_safe(pos, n, &rwlock->queue) {
                lock_wait = (void *)pos;

                if (lock_wait->lock_time + ltgconf_global.rpc_timeout  <= gettime()) {
                        list_del(pos);
                        lock_wait->retval = ETIMEDOUT;
                        lock_wait->magic = RET_MAGIC;
                        sche_task_post(&lock_wait->task, ETIMEDOUT, NULL);
                        lock_timeout++;
#if PLOCK_DEBUG
                        DWARN("lock timeout %p, writer %d, readers %u count %u,"
                              " last %u scheudle[%u] task[%u] type %d %d\n", rwlock,
                              rwlock->writer, rwlock->readers, rwlock->count,
                              rwlock->last_unlock,
                              lock_wait->lock_type, lock_wait->task.scheid,
                              lock_wait->task.taskid,
                              lock_timeout);
#endif
                }
        }
#endif
}

STATIC int __plock_wait(lock_wait_t *lock_wait, char type, int tmo, plock_t *rwlock)
{
        int ret;

        (void) tmo;

        DBUG("lock_wait %c\n", type);

        ANALYSIS_BEGIN(0);
        ret = sche_yield1(type == 'r' ? "rdplock" : "wrplock", NULL,
                              rwlock, __plock_timeout_check, 180);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        ret = lock_wait->retval;
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

STATIC int S_LTG __plock_lock(plock_t *rwlock, char type, int tmo, int prio)
{
        int ret;
        lock_wait_t lock_wait;
        char *name = type == 'r' ? "rdlock" : "rwlock";

        if (likely(ltgconf_global.daemon)) {
                LTG_ASSERT(sche_status() != SCHEDULE_STATUS_IDLE);
        }

#if PLOCK_CHECK
        __plock_check(rwlock);
#endif

        ANALYSIS_BEGIN(0);

        ret = __plock_trylock(rwlock, type, 0, NULL);
        if (unlikely(ret)) {
                if (ret == EBUSY || ret == EWOULDBLOCK) {
                        __plock_register(rwlock, type, &lock_wait, prio);

#if PLOCK_DEBUG
                        rwlock->count++;
#endif
                        ret = __plock_wait(&lock_wait, type, tmo, rwlock);
                        if (unlikely(ret)) {
                                LTG_ASSERT(lock_wait.magic == (int)RET_MAGIC);
                                GOTO(err_ret, ret);
                        }

                        LTG_ASSERT(lock_wait.magic == (int)RET_MAGIC);
                        goto out;
                } else
                        GOTO(err_ret, ret);
        } else {
#if PLOCK_DEBUG
                rwlock->count++;
#endif
        }

        ANALYSIS_END(0, 1000 * 50, name);

out:
#if ENABLE_SCHEDULE_LOCK_CHECK
        sche_lock_set(1, 0);
#endif

        return 0;
err_ret:
        return ret;
}

int S_LTG plock_rdlock(plock_t *rwlock)
{
        return __plock_lock(rwlock, 'r', -1, 0);
}

inline int plock_wrlock(plock_t *rwlock)
{
        return __plock_lock(rwlock, 'w', -1, 0);
}

inline int plock_wrlock_prio(plock_t *rwlock, int prio)
{
        return __plock_lock(rwlock, 'w', -1, prio);
}

int plock_timedwrlock(plock_t *rwlock, int sec)
{
        return __plock_lock(rwlock, 'w', sec, 0);
}

STATIC int __plock_trylock1(plock_t *rwlock, char type)
{
        int ret;
        char *name = type == 'r' ? "rdlock" : "rwlock";

#if PLOCK_CHECK
        __plock_check(rwlock);
#endif

        ANALYSIS_BEGIN(0);
        ret = __plock_trylock(rwlock, type, 0, NULL);
        if (unlikely(ret)) {
                goto err_ret;
        }

#if PLOCK_DEBUG
        rwlock->count++;
#endif

        ANALYSIS_END(0, 1000 * 50, name);

#if ENABLE_SCHEDULE_LOCK_CHECK
        sche_lock_set(1, 0);
#endif
        
        return 0;
err_ret:
        return ret;
}

inline int plock_tryrdlock(plock_t *rwlock)
{
        return __plock_trylock1(rwlock, 'r');
}


inline int plock_trywrlock(plock_t *rwlock)
{
        return __plock_trylock1(rwlock, 'w');
}

void S_LTG plock_unlock(plock_t *rwlock)
{
        int ret;
        lock_wait_t *lock_wait = NULL;
        struct list_head list, *pos, *n;

#if PLOCK_CHECK
        __plock_check(rwlock);
#endif

        ret = __plock_unlock(rwlock);
        if (unlikely(ret))
                UNIMPLEMENTED(__WARN__);

        if (list_empty(&rwlock->queue)) {
                goto out;
        }

        INIT_LIST_HEAD(&list);

        list_for_each_safe(pos, n, &rwlock->queue) {
                lock_wait = (void *)pos;
                ret = __plock_trylock(rwlock, lock_wait->lock_type, 1, &lock_wait->task);
                if (unlikely(ret))
                        break;

                list_del(pos);
                list_add_tail(&lock_wait->hook, &list);
        }

        list_for_each_safe(pos, n, &list) {
                lock_wait = (void *)pos;
                list_del(pos);

                lock_wait->retval = 0;
                lock_wait->magic = RET_MAGIC;
                sche_task_post(&lock_wait->task, 0, NULL);
        }

out:
#if PLOCK_DEBUG
        rwlock->last_unlock = gettime();
        rwlock->count--;
        DBUG("lock count %d\n", rwlock->count);
        LTG_ASSERT(rwlock->count >= 0);
#endif

#if ENABLE_SCHEDULE_LOCK_CHECK
        sche_lock_set(-1, 0);
#endif
        
        return;
}
