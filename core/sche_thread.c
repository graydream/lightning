#include <ucontext.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <stdint.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

typedef struct {
        struct list_head hook;

        task_t task;
        func_va_t exec;
        va_list ap;
        int retval;
} entry_t;

typedef struct {
        sem_t sem;
        ltg_spinlock_t lock;
        struct list_head trans_list;
        struct list_head nontrans_list;

        sche_thread_type_t type;
        int hash;
} sche_thread_t;

typedef struct {
        ltg_spinlock_t lock;
        int count;
        sche_thread_t *threads[__SCHE_THREAD_MAX__];
} sche_threadpool_t;

struct sche_thread_ops *sche_ops[SCHE_THREAD_MAX];
static sche_threadpool_t __sche_threadpool__;

static void __sche_thread_exec(sche_thread_t *st, struct list_head *list, int trans)
{
        struct list_head *pos, *n;
        entry_t *ctx;

        if (trans)
                sche_thread_ops_get(st->type)->begin_trans(st->hash);

        list_for_each_safe(pos, n, list) {
                ctx = (entry_t *)pos;

                ctx->retval = ctx->exec(ctx->ap);
        }

        if (trans)
                sche_thread_ops_get(st->type)->commit_trans(st->hash);

        list_for_each_safe(pos, n, list) {
                ctx = (entry_t *)pos;

                // task有scheid，所以外部线程可以唤醒task
                sche_task_post(&ctx->task, ctx->retval, NULL);

                list_del(pos);
                ltg_free((void **)&ctx);
        }
}

static int __sche_thread_worker__(sche_thread_t *st)
{
        int ret;
        struct list_head transactionList;
        struct list_head nonTransactionList;

        INIT_LIST_HEAD(&transactionList);
        INIT_LIST_HEAD(&nonTransactionList);

        ret = ltg_spin_lock(&st->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        // 提取到本地队列
        list_splice_init(&st->trans_list, &transactionList);
        list_splice_init(&st->nontrans_list, &nonTransactionList);

        ltg_spin_unlock(&st->lock);

        if (!list_empty(&transactionList))
                __sche_thread_exec(st, &transactionList, TRUE);
        if (!list_empty(&nonTransactionList))
                __sche_thread_exec(st, &nonTransactionList, FALSE);

        return 0;
err_ret:
        return ret;
}

void * __sche_thread_worker(void *_arg)
{
        int ret;
        sche_thread_t *st = _arg;

        (void) _arg;

        while (1) {
                ret = _sem_wait(&st->sem);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                ret = __sche_thread_worker__(st);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return NULL;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

int sche_thread_ops_register(struct sche_thread_ops *hook, int type, int size)
{
        int ret, i;
        sche_threadpool_t *tp = &__sche_threadpool__;
        sche_thread_t *st;
        char *ptr;

        ret = ltg_malloc((void **)&ptr, sizeof(sche_thread_t) * size);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_spin_lock(&tp->lock);
        if (unlikely(ret))
                GOTO(err_free, ret);

        if (tp->count + size > __SCHE_THREAD_MAX__) {
                ret = ENOSPC;
                GOTO(err_lock, ret);
        }

        LTG_ASSERT(type >= 0 && type < SCHE_THREAD_MAX);
        LTG_ASSERT(sche_ops[type] == NULL);
        sche_ops[type] = hook;
        sche_ops[type]->off = tp->count;
        sche_ops[type]->size = size;

        for (i = 0; i < size; i++) {
                st = (sche_thread_t  *)(ptr + i * sizeof(sche_thread_t));

                st->type = type;
                st->hash = i;

                INIT_LIST_HEAD(&st->trans_list);
                INIT_LIST_HEAD(&st->nontrans_list);

                ret = sem_init(&st->sem, 0, 0);
                if (unlikely(ret))
                        GOTO(err_lock, ret);

                ret = ltg_spin_init(&st->lock);
                if (unlikely(ret))
                        GOTO(err_lock, ret);

                ret = ltg_thread_create(__sche_thread_worker, st, "sche_thread");
                if (unlikely(ret))
                        GOTO(err_lock, ret);

                tp->threads[tp->count++] = st;
        }

        ltg_spin_unlock(&tp->lock);

        return 0;
err_lock:
        ltg_spin_unlock(&tp->lock);
err_free:
        ltg_free((void **)&ptr);
err_ret:
        return ret;
}

inline struct sche_thread_ops *__attribute__((always_inline)) sche_thread_ops_get(int type)
{
        return sche_ops[type];
}

struct sche_thread_ops misc_ops = {
        .type           = SCHE_THREAD_MISC,
        .begin_trans    = NULL,
        .commit_trans   = NULL,
};

int sche_thread_init()
{
        int ret;
        sche_threadpool_t *tp = &__sche_threadpool__;

        ret = ltg_spin_init(&tp->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        tp->count = 0;
        memset(tp->threads, 0x0, sizeof(tp->threads));

        memset(sche_ops, 0x0, sizeof(*sche_ops) * SCHE_THREAD_MAX);
        ret = sche_thread_ops_register(&misc_ops, misc_ops.type, 7);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static sche_thread_t *__sche_thread_get(int type, int hash)
{
        sche_threadpool_t *tp = &__sche_threadpool__;
        struct sche_thread_ops *sche_ops = sche_thread_ops_get(type);
        return tp->threads[sche_ops->off + hash % sche_ops->size];
}

static int __sche_thread_post(sche_thread_type_t type,
                                  const int hash, int trans, entry_t *ctx)
{
        int ret;
        sche_thread_t *st = __sche_thread_get(type, hash);

        ret = ltg_spin_lock(&st->lock);
        if (unlikely(ret)) {
                // 本过程，或处理线程会free ent，外层不能再次free
                ltg_free((void **)&ctx);
                GOTO(err_ret, ret);
        }

        if (trans)
                list_add_tail(&ctx->hook, &st->trans_list);
        else
                list_add_tail(&ctx->hook, &st->nontrans_list);

        ltg_spin_unlock(&st->lock);

        sem_post(&st->sem);

        return 0;
err_ret:
        return ret;
}

int sche_newthread(sche_thread_type_t type, const int hash, int trans,
                       const char *name, int timeout, func_va_t exec, ...)
{
        int ret;
        entry_t *ctx;

        (void) timeout;
        LTG_ASSERT(sche_running());

        ret = ltg_malloc((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ctx->exec = exec;
        va_start(ctx->ap, exec);
        ctx->task = sche_task_get();

        ANALYSIS_BEGIN(0);

        ret = __sche_thread_post(type, hash, trans, ctx);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = sche_yield1(name, NULL, NULL, NULL, -1);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        return 0;
err_ret:
        ANALYSIS_END(0, IO_WARN, NULL);
        return ret;
}

static void * __solo_exec_and_post(void *arg)
{
        entry_t *ctx = arg;

        ctx->retval = ctx->exec(ctx->ap);
        sche_task_post(&ctx->task, ctx->retval, NULL);

        return NULL;
}

int sche_thread_solo(sche_thread_type_t type, const int hash, int trans,
                       const char *name, int timeout, func_va_t exec, ...)
{
        int ret;
        entry_t *ctx;

        (void) type;
        (void) hash;
        (void) trans;
        (void) timeout;

        ret = ltg_malloc((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ctx->exec = exec;
        va_start(ctx->ap, exec);
        ctx->task = sche_task_get();

        ANALYSIS_BEGIN(0);

        ret = ltg_thread_create(__solo_exec_and_post, (void *)ctx, "sche_thread");
        if (unlikely(ret))
                GOTO(err_free, ret);

        ret = sche_yield1(name, NULL, NULL, NULL, -1);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        ltg_free((void **)&ctx);
        return 0;
err_free:
        ltg_free((void **)&ctx);
err_ret:
        ANALYSIS_END(0, IO_WARN, NULL);
        return ret;
}
