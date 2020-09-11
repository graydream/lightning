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
        char name[MAX_NAME_LEN];
        int group;
        func_t func;
        void *arg;
} wait_task_t;

extern sche_t **__sche_array__;

#ifdef NEW_SCHED
int IO_FUNC swapcontext1(struct cpu_ctx *cur_ctx, struct cpu_ctx *new_ctx);
__asm__ (
"       .text                                                           \n"
"       .p2align 4,,15                                                  \n"
"       .globl swapcontext1                                             \n"
"       .globl swapcontext1                                             \n"
"swapcontext1:                                                          \n"
"swapcontext1:                                                          \n"
"       movq %rsp, 0(%rdi)      # save stack_pointer                    \n"
"       movq %rbp, 8(%rdi)      # save frame_pointer                    \n"
"       movq (%rsp), %rax       # save insn_pointer                     \n"
"       movq %rax, 16(%rdi)                                             \n"
"       movq %rbx, 24(%rdi)     # save rbx,r12-r15                      \n"
"       movq 24(%rsi), %rbx                                             \n"
"       movq %r15, 56(%rdi)                                             \n"
"       movq %r14, 48(%rdi)                                             \n"
"       movq %rdi, 64(%rdi)     #was a lack, to backup rdi              \n"
"       movq 48(%rsi), %r14                                             \n"
"       movq 56(%rsi), %r15                                             \n"
"       movq %r13, 40(%rdi)                                             \n"
"       movq %r12, 32(%rdi)                                             \n"
"       movq 32(%rsi), %r12                                             \n"
"       movq 40(%rsi), %r13     # restore rbx,r12-r15                   \n"
"       movq 0(%rsi), %rsp      # restore stack_pointer                 \n"
"       movq 16(%rsi), %rax     # restore insn_pointer                  \n"
"       movq 8(%rsi), %rbp      # restore frame_pointer                 \n"
"       movq 64(%rsi), %rdi     # save taskctx to rdi tramp function    \n"
"       movq %rax, (%rsp)                                               \n"
"       ret                                                             \n"
);
#endif

/** called before yield
 *
 * @return
 */

inline task_t sche_task_get()
{
        task_t taskid;
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        LTG_ASSERT(sche);
        LTG_ASSERT(sche->running_task != -1);
        taskctx = &sche->tasks[sche->running_task];
        LTG_ASSERT(taskctx->state == TASK_STAT_RUNNING);
        LTG_ASSERT(taskctx->pre_yield == 0);
        taskctx->pre_yield = 1;

        sche_fingerprint_new(sche, taskctx);

        taskid.scheid = sche->id;
        taskid.taskid = sche->running_task;
        taskid.fingerprint = taskctx->fingerprint;
        LTG_ASSERT(taskid.fingerprint);
        return taskid;
}

inline int sche_task_get1(sche_t *sche, task_t *taskid)
{
        int ret;
        taskctx_t *taskctx;

        LTG_ASSERT(sche);
        LTG_ASSERT(sche->running_task != -1);
        taskctx = &sche->tasks[sche->running_task];
        LTG_ASSERT(taskctx->state == TASK_STAT_RUNNING);

        if (unlikely(taskctx->pre_yield)) {
                ret = EBUSY;
                GOTO(err_ret, ret);
        }

        taskctx->pre_yield = 1;
        sche_fingerprint_new(sche, taskctx);

        taskid->scheid = sche->id;
        taskid->taskid = sche->running_task;
        taskid->fingerprint = taskctx->fingerprint;
        LTG_ASSERT(taskid->fingerprint);

        return 0;
err_ret:
        return ret;
}

static void __sche_task_sleep(task_t *task)
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        LTG_ASSERT(sche);
        taskctx = &sche->tasks[task->taskid];
        LTG_ASSERT(taskctx->sleeping == 0);
        taskctx->sleeping = 1;
        taskctx->sleep = 1;
}

static void __sche_task_wakeup(task_t *task)
{
        sche_t *sche = __sche_array__[task->scheid];
        taskctx_t *taskctx;

        LTG_ASSERT(sche);
        taskctx = &sche->tasks[task->taskid];
        LTG_ASSERT(taskctx->sleeping == 1);
        taskctx->sleeping = 0;
}

void sche_task_reset()
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        LTG_ASSERT(sche);
        LTG_ASSERT(sche->running_task != -1);
        taskctx = &sche->tasks[sche->running_task];
        LTG_ASSERT(taskctx->state == TASK_STAT_RUNNING);
        //LTG_ASSERT(taskctx->pre_yield == 1);
        taskctx->pre_yield = 0;
}

int sche_taskid()
{
        sche_t *sche = sche_self();

        LTG_ASSERT(sche);
        return sche->running_task;
}

typedef struct {
        task_t task;
        int free;
} slp_task_t;

static void  __sche_task_sleep__(void *arg)
{
        slp_task_t *slp = arg;
        __sche_task_wakeup(&slp->task);
        sche_task_post(&slp->task, 0, NULL);

        slab_stream_free(slp);
}

int sche_task_sleep(const char *name, suseconds_t usec)
{
        int ret;
        slp_task_t *slp;
        sche_t *sche = sche_self();

        if (likely(sche_running())) {
                LTG_ASSERT(usec < 180 * 1000 * 1000);

                if (sche->eventfd == -1) {
                        slp = slab_stream_alloc(sizeof(*slp));
                } else {
                        slp = slab_stream_alloc_glob(sizeof(*slp));
                }

                slp->task = sche_task_get();

                __sche_task_sleep(&slp->task);
                timer_insert(name, slp, __sche_task_sleep__, usec);

                ret = sche_yield(name, NULL, &slp->task);
                if (unlikely(ret)) {
                        if (ret == ESTALE) {
                                GOTO(err_ret, ret);
                        } else {
                                UNIMPLEMENTED(__DUMP__);
                        }
                }
        } else {
                usleep(usec);
        }

        return 0;
err_ret:
        return ret;
}

static void __sche_task_post_remote(sche_t *sche, const task_t *task,
                                    int retval, ltgbuf_t *buf)
{
        int ret;
        reply_remote_t *reply;// = slab_stream_alloc_glob(PAGE_SIZE);

        ret = ltg_malloc((void **)&reply, sizeof(*reply));
        LTG_ASSERT(ret == 0);

        LTG_ASSERT(task->scheid >= 0 && task->scheid <= SCHEDULE_MAX);
        LTG_ASSERT(task->taskid >= 0 && task->taskid < TASK_MAX);
        LTG_ASSERT(task->fingerprint);

        (void) sche;
        reply->task = *task;
        reply->retval = retval;
        ltgbuf_init(&reply->buf, 0);
        if (buf && buf->len) {
                ltgbuf_merge(&reply->buf, buf);
        }

        ret = ltg_spin_lock(&sche->reply_remote_lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        list_add_tail(&reply->hook, &sche->reply_remote_list);

        ltg_spin_unlock(&sche->reply_remote_lock);

        return;
}

static void IO_FUNC __sche_task_post(sche_t *sche, reply_queue_t *reply_queue,
                              const task_t *task, int retval, ltgbuf_t *buf)
{
        int ret;
        ltgbuf_t *mem = slab_stream_alloc(PAGE_SIZE);
        reply_t *reply;
        
        LTG_ASSERT(task->scheid >= 0 && task->scheid <= SCHEDULE_MAX);
        LTG_ASSERT(task->taskid >= 0 && task->taskid < TASK_MAX);
        LTG_ASSERT(task->fingerprint);

        (void) sche;

        if (unlikely(reply_queue->count == reply_queue->max)) {
                if (unlikely(reply_queue->count + REPLY_QUEUE_STEP > REPLY_QUEUE_MAX)) {
                        ret = ENOSPC;
                        GOTO(err_ret, ret);
                }

                DBUG("new reply_queue array %u\n", reply_queue->max + REPLY_QUEUE_STEP);

                // @note 会导致buffer.list失效, 所以reply.buffer需要是指针类型
                ret = ltg_realloc((void **)&reply_queue->replys,
                               sizeof(*reply) * reply_queue->max,
                               sizeof(*reply) * (reply_queue->max + REPLY_QUEUE_STEP));
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                reply_queue->max += REPLY_QUEUE_STEP;
        }

        reply = &reply_queue->replys[reply_queue->count];
        reply->task = *task;
        reply->retval = retval;
        reply->buf = mem;
        ltgbuf_init(reply->buf, 0);
        if (buf && buf->len) {
                ltgbuf_merge(reply->buf, buf);
        }

        reply_queue->count++;

        return;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return;
}

void IO_FUNC sche_task_post(const task_t *task, int retval, ltgbuf_t *buf)
{
        sche_t *sche = __sche_array__[task->scheid];

        if (likely(sche == sche_self())) {
                __sche_task_post(sche, &sche->reply_local, task, retval, buf);

                //sche_post(sche);
        } else {
                __sche_task_post_remote(sche, task, retval, buf);
                sche_post(sche);
        }
}

inline void sche_task_setname(const char *name)
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        LTG_ASSERT(sche->running_task != -1);
        taskctx = &sche->tasks[sche->running_task];
        LTG_ASSERT(taskctx->state == TASK_STAT_RUNNING);
        strcpy(taskctx->name, name);
}

static int IO_FUNC __sche_task_hasfree(sche_t *sche)
{
        if (unlikely((TASK_MAX - sche->free_task.count) >= TASK_MAX / 2))
                return 0;
        else
                return 1;
}

static void __sche_wait_task_resume(sche_t *sche)
{
        wait_task_t *wait_task;

        if (likely(list_empty(&sche->wait_task.list))) {
                return;
        }

        if (!__sche_task_hasfree(sche)) {
                return;
        }

        wait_task = (void *)sche->wait_task.list.next;
        count_list_del(&wait_task->hook, &sche->wait_task);

        DBUG("resume wait task %s\n", wait_task->name);

        sche_task_new(wait_task->name, wait_task->func, wait_task->arg,
                      wait_task->group);
        ltg_free((void **)&wait_task);
}

static void IO_FUNC __sche_trampoline(taskctx_t *taskctx)
{
        sche_t *sche = taskctx->sche;

        //ANALYSIS_BEGIN(0);

#if ENABLE_SCHEDULE_DEBUG
        DINFO("start task[%u] %s\n", taskctx->id, taskctx->name);
#else
        DBUG("start task[%u] %s\n", taskctx->id, taskctx->name);
#endif

        LTG_ASSERT(sche->running_task != -1);

        taskctx->func(taskctx->arg);

#if ENABLE_SCHEDULE_DEBUG
        DINFO("finish task[%u] %s\n", taskctx->id, taskctx->name);
#else
        DBUG("finish task[%u] %s\n", taskctx->id, taskctx->name);
#endif

#if SCHEDULE_TASKCTX_RUNTIME
	if (likely(taskctx->sleep == 0))
                sche->c_runtime += _microsec_time_used_from_now(&taskctx->ctime);
#else
        sche->task_count--;
#endif

        __sche_wait_task_resume(sche);

        DBUG("free task %s, id [%u][%u]\n", taskctx->name, sche->id, taskctx->id);

#if ENABLE_SCHEDULE_LOCK_CHECK
        LTG_ASSERT(taskctx->lock_count == 0);
        LTG_ASSERT(taskctx->ref_count == 0);
#endif

        //taskctx->fingerprint_prev = taskctx->fingerprint;
        taskctx->fingerprint = 0;
        taskctx->func = NULL;
        taskctx->arg = NULL;
        taskctx->state = TASK_STAT_FREE;
        LTG_ASSERT(sche->task_count >= 0);
        list_del(&taskctx->running_hook);
        /* list_add better than list_add_tail here, otherwise statck will malloc for each task */
        count_list_add(&taskctx->running_hook, &sche->free_task);

#ifdef NEW_SCHED
        swapcontext1(&taskctx->ctx, &taskctx->main);
#endif
}

#ifdef NEW_SCHED
static void IO_FUNC __sche_makecontext(sche_t *sche, taskctx_t *taskctx)
{
        (void) sche;
        char *stack_top = (char *)taskctx->stack +  DEFAULT_STACK_SIZE;
        void **stack = NULL;
        stack = (void **)stack_top;

        stack[-3] = NULL;
        taskctx->ctx.rdi = (void *)taskctx;
        taskctx->ctx.rsp = (void *) (stack_top - (4 * sizeof(void *)));
        taskctx->ctx.rbp = (void *) (stack_top - (3 * sizeof(void *)));
        taskctx->ctx.rip = (void *)__sche_trampoline;
}
#else
static void IO_FUNC __sche_makecontext(sche_t *sche, taskctx_t *taskctx)
{
        (void) sche;
        getcontext(&(taskctx->ctx));

        taskctx->ctx.uc_stack.ss_sp = taskctx->stack;
        taskctx->ctx.uc_stack.ss_size = DEFAULT_STACK_SIZE;
        taskctx->ctx.uc_stack.ss_flags = 0;
        taskctx->ctx.uc_link = &(taskctx->main);
        makecontext(&(taskctx->ctx), (void (*)(void))(__sche_trampoline), 1, taskctx);
}
#endif

static int __sche_wait_task(const char *name, func_t func, void *arg, int group)
{
        int ret;
        wait_task_t *wait_task;
        sche_t *sche = sche_self();

        DINFO("wait task %s, count %u\n", name, sche->wait_task.count);

        ret = ltg_malloc((void **)&wait_task, sizeof(*wait_task));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        wait_task->arg = arg;
        wait_task->func = func;
        wait_task->group = group;
        strcpy(wait_task->name, name);

        count_list_add_tail(&wait_task->hook, &sche->wait_task);

        return 0;
err_ret:
        return ret;
}

static void *__sche_task_new(sche_t *sche)
{
        int ret;
        void *stack, *vaddr;

        if (core_self() && ltgconf_global.daemon
            && ENABLE_HUGEPAGE
            && ENABLE_TASK_HUGEPAGE) {
                if (sche->task_hpage == NULL) {
                        uint32_t size = HUGEPAGE_SIZE;
                        int ret = hugepage_getfree((void **)&vaddr, &size, __FUNCTION__);
                        LTG_ASSERT(ret == 0);
                        LTG_ASSERT(size == HUGEPAGE_SIZE);

                        sche->task_hpage = vaddr;
                        sche->task_hpage_offset = 0;

                        DINFO("new task hugepage %p offset %x, new\n",
                              sche->task_hpage, sche->task_hpage_offset);
                }

                stack =  sche->task_hpage + sche->task_hpage_offset;
                sche->task_hpage_offset += DEFAULT_STACK_SIZE;
                if (sche->task_hpage_offset >= 2 * 1024 * 1024){
                        DBUG("new task hugepage %p offset %x, removed\n",
                              sche->task_hpage, sche->task_hpage_offset);

                        sche->task_hpage = NULL;
                        sche->task_hpage_offset = 0;
                } else {
                        DBUG("new task hugepage %p offset %x, reuse\n",
                              sche->task_hpage, sche->task_hpage_offset);
                }
        } else {
                ret = ltg_malloc((void **)&stack, DEFAULT_STACK_SIZE);
                LTG_ASSERT(ret == 0);
        }

        memset(stack, 0x0, KEEP_STACK_SIZE);

        return stack;
}

int IO_FUNC sche_task_new(const char *name, func_t func, void *arg, int _group)
{
        int ret, group;
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

#if 1
        group = _group != -1 ? _group : (SCHE_GROUP_MAX - 1);
#else
        (void) _group;
        group = 0;
#endif

        DBUG("create task %s group %u\n", name, group);

        LTG_ASSERT(group >= SCHE_GROUP0 && group < SCHE_GROUP_MAX);
        LTG_ASSERT(sche);

        if (unlikely(!__sche_task_hasfree(sche))) {
                ret = __sche_wait_task(name, func, arg, group);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                return -1;
        }

	taskctx = list_entry(sche->free_task.list.next, taskctx_t, running_hook);
	count_list_del(&taskctx->running_hook, &sche->free_task);
	if (unlikely(taskctx->stack == NULL)) {
                taskctx->stack = __sche_task_new(sche);
	}

        DBUG("%s\n", name);
        strcpy(taskctx->name, name);
        taskctx->state = TASK_STAT_RUNNABLE;
        taskctx->func = func;
        taskctx->arg = arg;
        taskctx->step = 0;
        taskctx->pre_yield = 0;
        taskctx->sleeping = 0;
        taskctx->wait_begin = 0;
        taskctx->wait_tmo = 0;
        taskctx->sleep = 0;
        taskctx->sche = sche;
        taskctx->group = group;
        sche->task_count++;

        list_add_tail(&taskctx->running_hook, &sche->running_task_list);

#if ENABLE_SCHEDULE_LOCK_CHECK
        taskctx->lock_count = 0;
        taskctx->ref_count = 0;
#endif

        DBUG("new task[%d] %s count:%d\n", taskctx->id, name, sche->task_count);

        sche_fingerprint_new(sche, taskctx);

        _microsec_update_now(&taskctx->ctime);

        count_list_add_tail(&taskctx->hook, &sche->runable[taskctx->group]);

        __sche_makecontext(sche, taskctx);

        return taskctx->id;
}

#define REQUEST_SEM 1
#define REQUEST_TASK 2

typedef struct {
        task_t task;
        sem_t sem;
        func_va_t exec;
        va_list ap;
        int type;
        int retval;
} arg1_t;

static void IO_FUNC __sche_task_run(void *arg)
{
        arg1_t *ctx = arg;

        DBUG("sche_task_run\n");
        
        ctx->retval = ctx->exec(ctx->ap);

        if (ctx->type == REQUEST_SEM) {
                sem_post(&ctx->sem);
        } else {
                sche_task_post(&ctx->task, 0, NULL);
        }
}

int IO_FUNC sche_task_run(int group, func_va_t exec, ...)
{
        int ret;
        arg1_t ctx;

        ctx.exec = exec;
        va_start(ctx.ap, exec);

        sche_task_new("grp", __sche_task_run, &ctx, group);
        
        if (likely(sche_running())) {
                ctx.type = REQUEST_TASK;
                ctx.task = sche_task_get();
                
                ret = sche_yield1("grp", NULL, NULL, NULL, -1);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        } else {
                ctx.type = REQUEST_SEM;
                ret = sem_init(&ctx.sem, 0, 0);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
                
                ret = _sem_wait(&ctx.sem);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        return ctx.retval;
err_ret:
        return ret;
}
