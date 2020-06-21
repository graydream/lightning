#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

#define DBG_SYLOCK

extern int srv_running;
extern int rdma_running;

typedef enum {
        LOCK_CO = 10,
        LOCK_NO,
} lock_type_t; 

#define RET_MAGIC 0x866aa9f0

typedef struct {
        struct list_head hook;
        char lock_type;
        char sche_type;
        task_t task;
        sem_t sem;
        int magic;
        int retval;
} lock_wait_t;

int ltg_rwlock_init(ltg_rwlock_t *rwlock, const char *name)
{
        int ret;

        (void) name;
        
        ret = pthread_rwlock_init(&rwlock->lock, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_spin_init(&rwlock->spin);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        INIT_LIST_HEAD(&rwlock->queue);

        rwlock->priority = 0;
#if LOCK_DEBUG
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
err_ret:
        return ret;
}

int ltg_rwlock_destroy(ltg_rwlock_t *rwlock)
{
        int ret;
        lock_wait_t *lock_wait = NULL;
        struct list_head list, *pos, *n;

        DWARN("destroy %p\n", rwlock);

        INIT_LIST_HEAD(&list);
        ret = ltg_spin_lock(&rwlock->spin);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = pthread_rwlock_destroy(&rwlock->lock);
        if (unlikely(ret))
                GOTO(err_lock, ret);

        list_splice_init(&rwlock->queue, &list);

        ltg_spin_unlock(&rwlock->spin);

        list_for_each_safe(pos, n, &list) {
                lock_wait = (void *)pos;
                list_del(pos);

                lock_wait->retval = EBUSY;
                lock_wait->magic = RET_MAGIC;
                if (lock_wait->sche_type == LOCK_CO) {
                        sche_task_post(&lock_wait->task, 0, NULL);
                } else {
                        sem_post(&lock_wait->sem);
                }
        }

        return 0;
err_lock:
        ltg_spin_unlock(&rwlock->spin);
err_ret:
        return ret;
}

STATIC int __ltg_rwlock_trylock__(ltg_rwlock_t *rwlock, char type, task_t *task)
{
        int ret;

        //LTG_ASSERT((int)rwlock->lock.__data.__readers >= 0);
        if (type == 'r')
                ret = pthread_rwlock_tryrdlock(&rwlock->lock);
        else {
                LTG_ASSERT(type == 'w');
                ret = pthread_rwlock_trywrlock(&rwlock->lock);
                if (ret == 0) {
                        (void) task;
#if LOCK_DEBUG
                        if (task) {
                                rwlock->writer = *task;
                        } else {
                                sche_task_given(&rwlock->writer);
                        }
#endif
                }
        }

        //LTG_ASSERT((int)rwlock->lock.__data.__readers >= 0);
        
        return ret;
}

STATIC int __ltg_rwlock_trylock0(ltg_rwlock_t *rwlock, char type)
{
        int ret;

        if (list_empty(&rwlock->queue)) {
                ret =  __ltg_rwlock_trylock__(rwlock, type, NULL);
                if (unlikely(ret))
                        goto err_ret;
        } else {
                ret = EBUSY;
                goto err_ret;
        }

        return 0;
err_ret:
        return ret;
}

STATIC int __ltg_rwlock_trylock(ltg_rwlock_t *rwlock, char type)
{
        int ret;

        if (list_empty(&rwlock->queue)) {
                ret =  __ltg_rwlock_trylock__(rwlock, type, NULL);
                if (unlikely(ret))
                        goto err_ret;

#if 1
                if (rwlock->priority && type == 'w') {
                        DBUG("priority %u, cleanup\n", rwlock->priority);
                        rwlock->priority = 0;
                }
#endif
        } else {
#if 0
                ret =  __ltg_rwlock_trylock__(rwlock, type, NULL);
                if (unlikely(ret))
                        goto err_ret;
#else
                if (rwlock->priority < 128 && type == 'r') {
                        ret =  __ltg_rwlock_trylock__(rwlock, type, NULL);
                        if (unlikely(ret))
                                goto err_ret;

                        rwlock->priority++;
                        DBUG("priority %u, increased\n", rwlock->priority);
                } else {
                        ret = EBUSY;
                        goto err_ret;
                }
#endif
        }

        return 0;
err_ret:
        return ret;
}

STATIC void __ltg_rwlock_register(ltg_rwlock_t *rwlock, char type, lock_wait_t *lock_wait)
{
        int ret;

        if (sche_running()) {
                lock_wait->task = sche_task_get();
                lock_wait->sche_type = LOCK_CO;
        } else {
                lock_wait->task.taskid = __gettid();
                lock_wait->task.scheid = getpid();
                lock_wait->task.fingerprint = TASK_THREAD;
                lock_wait->sche_type = LOCK_NO;

                ret = sem_init(&lock_wait->sem, 0, 0);
                LTG_ASSERT(ret == 0);
        }

        lock_wait->magic = 0;
        lock_wait->retval = 0;
        lock_wait->lock_type = type;
        list_add_tail(&lock_wait->hook, &rwlock->queue);
}

STATIC int __ltg_rwlock_wait(ltg_rwlock_t *rwlock, lock_wait_t *lock_wait, char type, int tmo)
{
        int ret;

        DBUG("lock_wait %c\n", type);

        (void) rwlock;

        if (lock_wait->sche_type == LOCK_CO) {
                ANALYSIS_BEGIN(0);
                ret = sche_yield(type == 'r' ? "rdlock" : "rwlock", NULL, lock_wait);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }

                ANALYSIS_END(0, IO_WARN, NULL);
        } else {
                ret = _sem_timedwait1(&lock_wait->sem, tmo != -1 ? tmo : ltgconf_global.rpc_timeout * 6);
                LTG_ASSERT(ret == 0);
        }

        ret = lock_wait->retval;
        if (unlikely(ret))
                GOTO(err_ret, ret);

#if LOCK_DEBUG
        DINFO("locked %p %c count %d, writer %d\n", rwlock, lock_wait->lock_type, rwlock->count, rwlock->lock.__data.__cur_writer);
#endif
        
        return 0;
err_ret:
        return ret;
}

STATIC int __ltg_rwlock_lock(ltg_rwlock_t *rwlock, char type, int tmo)
{
        int ret, retry = 0;
        lock_wait_t lock_wait;
        //char *name = type == 'r' ? "rdlock" : "rwlock";

        if (ltgconf_global.daemon) {
                LTG_ASSERT(sche_status() != SCHEDULE_STATUS_IDLE);
        }
        
        ret = __ltg_rwlock_trylock0(rwlock, type);
        if (likely(ret == 0)) {
                goto success;
        }
        
retry:
        ret = ltg_spin_lock(&rwlock->spin);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __ltg_rwlock_trylock(rwlock, type);
        if (unlikely(ret)) {
                if (ret == EBUSY || ret == EWOULDBLOCK) {
#if LOCK_DEBUG
                        DINFO("wait %p %c count %d, retry %u\n", rwlock, type, rwlock->count, retry);
                        LTG_ASSERT(rwlock->count > 0);
                        rwlock->count++;
#endif

                        __ltg_rwlock_register(rwlock, type, &lock_wait);

                        ltg_spin_unlock(&rwlock->spin);

                        ret = __ltg_rwlock_wait(rwlock, &lock_wait, type, tmo);

                        ret = ltg_spin_lock(&rwlock->spin);
                        LTG_ASSERT(ret == 0);
                        
                        LTG_ASSERT(!list_exists(&lock_wait.hook, &rwlock->queue));

                        ltg_spin_unlock(&rwlock->spin);
                        
                        if (unlikely(ret)) {
                                if (ret == ESTALE) {//dangerous here
                                        LTG_ASSERT(lock_wait.retval == 0);
                                        LTG_ASSERT(sche_running());
                                        ltg_rwlock_unlock(rwlock);
                                }

                                GOTO(err_ret, ret);
                        }

                        LTG_ASSERT(lock_wait.magic == (int)RET_MAGIC);

                        retry++;
                        goto retry;
                } else
                        GOTO(err_lock, ret);
        } else {
#if LOCK_DEBUG
                LTG_ASSERT(rwlock->count >= 0);
                rwlock->count++;
                DINFO("locked %p %c count %d, writer %d\n", rwlock, type,
                      rwlock->count, rwlock->lock.__data.__cur_writer);
#endif
                ltg_spin_unlock(&rwlock->spin);
        }

success:
        return 0;
err_lock:
        ltg_spin_unlock(&rwlock->spin);
err_ret:
        return ret;
}

int ltg_rwlock_rdlock(ltg_rwlock_t *rwlock)
{
        return __ltg_rwlock_lock(rwlock, 'r', -1);
}

inline int ltg_rwlock_wrlock(ltg_rwlock_t *rwlock)
{
        return __ltg_rwlock_lock(rwlock, 'w', -1);
}

int ltg_rwlock_timedwrlock(ltg_rwlock_t *rwlock, int sec)
{
        return __ltg_rwlock_lock(rwlock, 'w', sec);
}


STATIC int __ltg_rwlock_trylock1(ltg_rwlock_t *rwlock, char type)
{
        int ret;
        char *name = type == 'r' ? "rdlock" : "rwlock";

        ANALYSIS_BEGIN(0);
        ret = ltg_spin_lock(&rwlock->spin);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __ltg_rwlock_trylock(rwlock, type);
        if (unlikely(ret)) {
                if (ret == EBUSY || ret == EWOULDBLOCK)
                        goto err_lock;
                else
                        GOTO(err_lock, ret);
        }

#if LOCK_DEBUG
        LTG_ASSERT(rwlock->count >= 0);
        rwlock->count++;
        DINFO("trylock %p count %d\n", rwlock, rwlock->count);
#endif
        ltg_spin_unlock(&rwlock->spin);

        ANALYSIS_END(0, 1000 * 50, name);

        return 0;
err_lock:
        ltg_spin_unlock(&rwlock->spin);
err_ret:
        return ret;
}

inline int ltg_rwlock_tryrdlock(ltg_rwlock_t *rwlock)
{
        return __ltg_rwlock_trylock1(rwlock, 'r');
}


inline int ltg_rwlock_trywrlock(ltg_rwlock_t *rwlock)
{
        return __ltg_rwlock_trylock1(rwlock, 'w');
}

void ltg_rwlock_unlock(ltg_rwlock_t *rwlock)
{
        int ret, locked = 0;
        lock_wait_t *lock_wait = NULL;
        struct list_head list, *pos, *n;

        INIT_LIST_HEAD(&list);

        ret = ltg_spin_lock(&rwlock->spin);
        if (unlikely(ret))
                UNIMPLEMENTED(__WARN__);

        //LTG_ASSERT((int)rwlock->lock.__data.__readers >= 0);
        ret = pthread_rwlock_unlock(&rwlock->lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__WARN__);

        //LTG_ASSERT((int)rwlock->lock.__data.__readers >= 0);
#if LOCK_DEBUG
        int empty = list_empty(&rwlock->queue);
        
        memset(&rwlock->writer, 0x0, sizeof(rwlock->writer));

        rwlock->count--;
        DINFO("unlock %p count %d, locked %u, empty %d, writer %d\n", rwlock,
              rwlock->count, locked, empty, rwlock->lock.__data.__cur_writer);

        LTG_ASSERT(rwlock->count >= 0);

        rwlock->last_unlock = gettime();
#endif

#if 0
        list_for_each_safe(pos, n, &rwlock->queue) {
                lock_wait = (void *)pos;
                /* rwlock->__writer maybe not real writer, but this thread who try lock the rwlock */

#if 0
                ret = __ltg_rwlock_trylock__(rwlock, lock_wait->lock_type, &lock_wait->task);
                if (unlikely(ret)) {
                        break;
                }
#endif

#if LOCK_DEBUG
                DINFO("resume %p %c count %d \n", rwlock,
                      lock_wait->lock_type, rwlock->count);
#endif
                
                list_del(pos);
                list_add_tail(&lock_wait->hook, &list);
                locked++;
        }
#else
        (void) locked;
        (void) pos;
        (void) n;

        list_splice_init(&rwlock->queue, &list);
#endif

        ltg_spin_unlock(&rwlock->spin);

        list_for_each_safe(pos, n, &list) {
                lock_wait = (void *)pos;
                list_del(pos);

                lock_wait->retval = 0;
                lock_wait->magic = RET_MAGIC;
                if (lock_wait->sche_type == LOCK_CO) {
                        sche_task_post(&lock_wait->task, 0, NULL);
                } else {
                        sem_post(&lock_wait->sem);
                }
        }

        return;
}
