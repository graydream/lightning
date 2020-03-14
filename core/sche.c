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

#define __TASK_MAX__ (TASK_MAX * SCHEDULE_MAX)

/**
//gdb loop

define loop_sche
set $count= sche->size
set $i = 0
while ($i < $count)
if (sche->tasks[$i].state != TASK_STAT_FREE)
print  sche->tasks[$i].name
print  sche->tasks[$i].wait_name
print $i
end
set $i = $i + 1
end
end
**/


/**
 * polling core execute flow graph by Gabe:
 *
 *                        swapcontext    swapcontext
 *                  |user space|sche space|user space|
 * .--.             |          `----.     .---`          |
 * |  V             `---------.     v     v     .--------`
 * | poll()                   +-----+     +-----+          +-----+
 * |  |     .--reply_local--> |taks0| --> |taks1| --.      |taks2|
 * |  |     |                 +-----+     +-----+   |      +-----+
 * |  |     |                 .---------------------`      ^
 * |  |     |                 |     +-----+                |           +------------+
 * |  |     |                 `---> |taks2|                |           |another core|
 * |  |     |                       +-----+ ---> yield  ---`           +------------+
 * |  |     |                   .---------------`      ^                     |
 * |  |     |                   |                      |                     |
 * |  |     |                   |   +-----+            |                     |
 * |  |     |                   `-> |taks3| --resume---`                     |
 * |  |     |                       +-----+ ---> yield  <-----resume---------`
 * |  |     |  .--------------------------------`       `--.
 * |  v     |  |                                           V
 * | run()--`  |              +-----+     +-----+          +-----+
 * |  .-----.  reply_remote-> |taks4| --> |taks5| --.      |taks3|
 * |  |     |                 +-----+     +-----+   |      +-----+
 * |  |     |  .------------------------------------`
 * |  |     |  |
 * |  |     |  |              +-----+     +-----+          +-----+
 * |  |     |  runable------> |taks6| --> |taks7| --.      |taks8|
 * |  |     |                 +-----+     +-----+   |      +-----+
 * |  |     |  .------------------------------------`      ^
 * |  |     |  |                                           |
 * |  |     |  |              +-----+ -----------new-------`
 * |  |     |  request------> |req1 | --.
 * |  |     |                 +-----+   |
 * |  |     `---------------------------`
 * |  v
 * | scan()
 * `--`
 */

typedef struct {
        func_t func;
        void *arg;
} sche_task_ctx_t;

//globe data
sche_t **__sche_array__ = NULL;
static ltg_spinlock_t __sche_array_lock__;

extern int swapcontext1(struct cpu_ctx *cur_ctx, struct cpu_ctx *new_ctx);
static int __sche_isfree(taskctx_t *taskctx);
static void __sche_backtrace__(const char *name, int id, int idx, uint32_t seq);
static void __sche_backtrace_set(taskctx_t *taskctx);

//used by sche_stack_assert
static char zerobuf[KEEP_STACK_SIZE] = {0};

static __thread sche_t *__sche__;

inline IO_FUNC sche_t *sche_self()
{
        return __sche__;
}

static inline IO_FUNC sche_t *__sche_self(sche_t *sche)
{
        if (likely(sche)) {
#if ENABLE_SCHEDULE_SELF_DEBUG
                LTG_ASSERT(sche == sche_self());
#endif
                return sche;
        } else {
                return sche_self();
        }
}

static int __sche_runable(const sche_t *sche)
{
        int count = 0, i;

        for (i = 0; i < SCHE_GROUP_MAX; i++) {
                count += sche->runable[i].count;
        }

        return count;
}

#if SCHEDULE_CHECK_RUNTIME
static inline void __sche_check_running_used(sche_t *sche, taskctx_t *taskctx, uint64_t used)
{
        if (unlikely(used > 100 * 1000)) {
                __sche_backtrace__(sche->name, sche->id, taskctx->id, sche->scan_seq);
                DWARN("task[%u] %s running used %fs\n", taskctx->id, taskctx->name, (double)used / (1000 * 1000));
        } else if (used > 10 * 1000) {
                DWARN("task[%u] %s running used %fs\n", taskctx->id, taskctx->name, (double)used / (1000 * 1000));
        }
}
#endif

static inline void __sche_check_yield_used(sche_t *sche, taskctx_t *taskctx, uint64_t used)
{
        if (unlikely(used < (uint64_t)1000 * 1000 * (ltgconf_global.lease_timeout / 2 + 1))) {
                return;
        }

        if (unlikely(used > (uint64_t)1000 * 1000 * (ltgconf_global.rpc_timeout * 4))) {
                __sche_backtrace__(sche->name, sche->id, taskctx->id, sche->scan_seq);

                if (unlikely(used > (uint64_t)1000 * 1000 * (ltgconf_global.rpc_timeout * 4))) {
                        DWARN("%s[%u][%u] %s.%s wait %fs, total %lu, retval %u\n",
                              sche->name, sche->id, taskctx->id,
                              taskctx->name, taskctx->wait_name, (double)used / (1000 * 1000),
                              gettime() - taskctx->ctime.tv_sec, taskctx->retval);
                } else if (unlikely(used > (uint64_t)1000 * 1000 * (ltgconf_global.rpc_timeout * 2))) {
                        DINFO("%s[%u][%u] %s.%s wait %fs, total %lu, retval %u\n",
                              sche->name, sche->id, taskctx->id,
                              taskctx->name, taskctx->wait_name, (double)used / (1000 * 1000),
                              gettime() - taskctx->ctime.tv_sec, taskctx->retval);
                } else {
                        DINFO("%s[%u][%u] %s.%s wait %fs, total %lu, retval %u\n",
                              sche->name, sche->id, taskctx->id,
                              taskctx->name, taskctx->wait_name, (double)used / (1000 * 1000),
                              gettime() - taskctx->ctime.tv_sec, taskctx->retval);
                }
        }
}

static inline void __sche_check_scan_used(sche_t *sche, taskctx_t *taskctx, uint64_t time_used)
{
        /* notice: time_used is seconds */
        if (unlikely(taskctx->sleeping
                     || time_used < (uint64_t)(ltgconf_global.lease_timeout / 2 + 1))) {
                return;
        }

        if (unlikely((taskctx->wait_tmo != -1 && time_used > (uint64_t)taskctx->wait_tmo))
            || time_used > (uint64_t)ltgconf_global.rpc_timeout * 4) {
                DBUG("%s[%u][%u] %s status %u time_used %lus\n", sche->name,
                     sche->id, taskctx->id, taskctx->name, taskctx->state, time_used);

                if (!taskctx->sleeping)
                        __sche_backtrace_set(taskctx);
        } else if (unlikely(time_used > (uint64_t)ltgconf_global.rpc_timeout * 2)) {
                DINFO("%s[%u][%u] %s status %u time_used %lus\n", sche->name,
                      sche->id, taskctx->id, taskctx->name, taskctx->state, time_used);
        } else if (unlikely(time_used > 1)) {
                DBUG("%s[%u][%u] %s status %u time_used %lus\n", sche->name, sche->id, taskctx->id,
                     taskctx->name, taskctx->state, time_used);
        }
}

int sche_suspend()
{
        sche_t *sche = sche_self();
        if (sche == NULL)
                return 0;

        if (sche->running_task == -1 && !sche->suspendable)
                return 1;
        else
                return 0;
}

static void IO_FUNC __sche_queue__(sche_t *sche, taskctx_t *taskctx, int retval, ltgbuf_t *buf)
{
        LTG_ASSERT(retval <= INT32_MAX);
        taskctx->retval = retval;
        taskctx->state = TASK_STAT_RUNNABLE;

        ltgbuf_init(&taskctx->buf, 0);
        if (buf && buf->len) {
                ltgbuf_merge(&taskctx->buf, buf);
        }

        count_list_add_tail(&taskctx->hook, &sche->runable[taskctx->group]);
}

static void __sche_exec__(sche_t *sche, taskctx_t *taskctx)
{
#if SCHEDULE_TASKCTX_RUNTIME
        struct timeval now;
        uint64_t used;
#endif

        LTG_ASSERT(taskctx->state != TASK_STAT_FREE && taskctx->state != TASK_STAT_RUNNING);
        LTG_ASSERT(sche->running_task == -1);
        sche->running_task = taskctx->id;
        taskctx->state = TASK_STAT_RUNNING;

        DBUG("swap in task[%u] %s\n", taskctx->id, taskctx->name);

#if SCHEDULE_TASKCTX_RUNTIME
        gettimeofday(&taskctx->rtime, NULL);
#endif

        swapcontext1(&(taskctx->main), &(taskctx->ctx));
        
#if SCHEDULE_TASKCTX_RUNTIME
        gettimeofday(&now, NULL);
        used = _time_used(&taskctx->rtime, &now);
        sche->run_time += used;
        //__sche_check_running_used(sche, taskctx, used);
#endif

        sche->running_task = -1;
        sche->counter++;
}

void sche_stack_assert(sche_t *_sche)
{
        sche_t *sche = __sche_self(_sche);
        taskctx_t *taskctx;

        if (unlikely(!(sche && sche->running_task != -1))) {
                return;
        }

        taskctx = &sche->tasks[sche->running_task];
        LTG_ASSERT(taskctx->state == TASK_STAT_RUNNING);

        DBUG("size %u\n", (int)((uint64_t)&taskctx - (uint64_t)taskctx->stack));
        LTG_ASSERT((int)((uint64_t)&taskctx - (uint64_t)taskctx->stack)
                   > DEFAULT_STACK_SIZE / 4);

        LTG_ASSERT(!memcmp(taskctx->stack, zerobuf, KEEP_STACK_SIZE));
}

static inline void __sche_backtrace__(const char *name, int id, int idx, uint32_t seq)
{
        char info[MAX_INFO_LEN];

        snprintf(info, MAX_INFO_LEN, "%s[%u][%u] seq[%u]", name, id, idx, seq);
        _backtrace(info);
}


static void __sche_backtrace_exec(sche_t *sche, taskctx_t *taskctx,
                                      const char *name, void *opaque, func_t func)
{
        int used = gettime() - taskctx->wait_begin;

        __sche_backtrace__(sche->name, sche->id, taskctx->id, sche->scan_seq);

        if (func && taskctx->wait_tmo != -1 && used > taskctx->wait_tmo) {
                func(opaque);
        }

        LTG_ASSERT(gettime() -  taskctx->wait_begin < ltgconf_global.rpc_timeout * 6 * 1000 * 1000);

        DINFO("%s[%u][%u] %s.%s wait %ds, total %lu s\n",
              sche->name, sche->id, taskctx->id,
              taskctx->name, name, used,
              gettime() - taskctx->ctime.tv_sec);

#if ENABLE_SCHEDULE_LOCK_CHECK
        if ((taskctx->lock_count || taskctx->ref_count)
            && used > ltgconf_global.rpc_timeout * 1 * 1000 * 1000) {
                DERROR("%s[%u][%u] %s.%s wait %ds, total %lu s, "
                       "retval %u, lock %u, ref %u\n",
                       sche->name, sche->id, taskctx->id,
                       taskctx->name, name, used,
                       gettime() - taskctx->ctime.tv_sec, taskctx->retval,
                       taskctx->lock_count, taskctx->ref_count);
        }
#endif
}


/**
 * 记录了任务执行时间.
 *
 * @param name
 * @param buf
 * @param opaque
 * @param func
 * @param _tmo
 * @return
 */
int IO_FUNC sche_yield1(const char *name, ltgbuf_t *buf, void *opaque, func_t func, int _tmo)
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;
#if SCHEDULE_CHECK_RUNTIME
        struct timeval now;
#endif
        struct timeval t1, t2;
        uint64_t used;

        LTG_ASSERT(_tmo < 1000);

#if ENABLE_SCHEDULE_STACK_ASSERT
        sche_stack_assert(sche);
#endif

        LTG_ASSERT(sche->running_task != -1);
        _gettimeofday(&t1, NULL);

        taskctx = &sche->tasks[sche->running_task];
        LTG_ASSERT(taskctx->state == TASK_STAT_RUNNING);
        taskctx->pre_yield = 0;
        taskctx->state = TASK_STAT_SUSPEND;
        taskctx->wait_begin = gettime();
        taskctx->wait_tmo = _tmo;
        taskctx->wait_name = name;
        taskctx->wait_opaque = opaque;
        taskctx->step++;

retry:
#if SCHEDULE_CHECK_RUNTIME
        _gettimeofday(&now, NULL);
        used = _time_used(&taskctx->rtime, &now);
        __sche_check_running_used(sche, taskctx, used);
#endif

#if ENABLE_SCHEDULE_DEBUG
        DINFO("swap out task[%u] %s yield @ %s\n", taskctx->id, taskctx->name, name);
#endif

        // 交错执行scher和task代码，现在切换回调度器
        swapcontext1(&(taskctx->ctx), &(taskctx->main));

#if ENABLE_SCHEDULE_DEBUG
        DINFO("swap in task[%u] %s yield @ %s\n", taskctx->id, taskctx->name, name);
#endif

#if SCHEDULE_CHECK_RUNTIME
        _gettimeofday(&taskctx->rtime, NULL);
#endif

#if 1
        if (unlikely(taskctx->state == TASK_STAT_BACKTRACE)) {
                __sche_backtrace_exec(sche, taskctx, name,  opaque, func);
                goto retry;
        }
#endif

        DBUG("task[%u] %s return @ %s\n", taskctx->id, taskctx->name, name);

        LTG_ASSERT(sche == sche_self());
        taskctx->wait_name = NULL;
        taskctx->wait_opaque = NULL;
        taskctx->wait_begin = 0;
        LTG_ASSERT(sche->running_task != -1);

        sche_fingerprint_new(sche, taskctx);

        if (taskctx->buf.len) {
                LTG_ASSERT(buf);
                ltgbuf_clone1(buf, &taskctx->buf, 0);
                ltgbuf_free(&taskctx->buf);
        }

        // 任务等待时间，过大说明调度器堵塞，或在此期间别的被调度任务堵塞
        // 如write等同步过程，不运行出现在调度器循环里

        _gettimeofday(&t2, NULL);
        used = _time_used(&t1, &t2);
        __sche_check_yield_used(sche, taskctx, used);

        return taskctx->retval;
}

int IO_FUNC sche_yield(const char *name, ltgbuf_t *buf, void *opaque)
{
        return sche_yield1(name, buf, opaque, NULL, -1);
}

static void  __sche_backtrace_set(taskctx_t *taskctx)
{
        sche_t *sche = taskctx->sche;

        DBUG("run task %s, id %u\n", taskctx->name, taskctx->id);

        LTG_ASSERT(sche->running_task == -1);

        if (taskctx->state == TASK_STAT_SUSPEND) {
                taskctx->state = TASK_STAT_BACKTRACE;

                swapcontext1(&(taskctx->main), &(taskctx->ctx));

                taskctx->state = TASK_STAT_SUSPEND;
        }
}

static int IO_FUNC __sche_queue(sche_t *sche, const task_t  *task, int retval,
                        ltgbuf_t *buf, int warn)
{
        int ret;
        taskctx_t *taskctx;

        LTG_ASSERT(task->taskid >= 0 && task->taskid < TASK_MAX);
        taskctx = &sche->tasks[task->taskid];

        DBUG("run task %s, id [%u][%u]\n", taskctx->name, sche->id, taskctx->id);

        // TODO core
        LTG_ASSERT(task->fingerprint);
        if (unlikely(task->fingerprint != taskctx->fingerprint)) {
                DERROR("run task[%u] %s already destroyed\n", taskctx->id, taskctx->name);
                ret = ESTALE;
                GOTO(err_ret, ret);
        }

        if (unlikely(warn)) {
                DBUG("run task[%u] %s wait %s resume remote\n", taskctx->id,
                      taskctx->name, taskctx->wait_name);
        }

        LTG_ASSERT(taskctx->state == TASK_STAT_SUSPEND);
        __sche_queue__(sche, taskctx, retval, buf);

        return 0;
err_ret:
        return ret;
}

static int __sche_task_wait(sche_t *sche)
{
        int count = 0;

        count += sche->request_queue.count;
        count += sche->reply_local.count;

        struct list_head *pos;
        int ret = ltg_spin_lock(&sche->reply_remote_lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        list_for_each(pos, &sche->reply_remote_list) {
                count++;
        }

        ltg_spin_unlock(&sche->reply_remote_lock);

        return count;
}

inline int sche_stat(int *sid, int *taskid, int *runable, int *wait, int *count,
                     uint64_t *run_time, uint64_t *c_runtime)
{
        sche_t *sche = sche_self();

        if (likely(sche)) {
                *sid = sche->id;
                *runable = __sche_runable(sche);
                *wait = __sche_task_wait(sche);
                *taskid = sche->running_task;
                *count = sche->task_count;
                *run_time = sche->run_time;
                *c_runtime = sche->c_runtime;
#if SCHEDULE_TASKCTX_RUNTIME
                sche->run_time = 0;
                sche->c_runtime = 0;
#endif
        } else {
                *sid = -1;
                *taskid = -1;
                *runable = -1;
                *wait = -1;
                *count = -1;
        }

        return 0;
}


inline int sche_running()
{
        sche_t *sche = sche_self();

        if (unlikely(sche == NULL))
                return 0;

        if (unlikely(sche->running_task == -1))
                return 0;

        return 1;
}

inline int sche_status()
{
        sche_t *sche = sche_self();

        if (sche == NULL)
                return SCHEDULE_STATUS_NONE;

        if (sche->running_task == -1)
                return SCHEDULE_STATUS_IDLE;

        return SCHEDULE_STATUS_RUNNING;
}

/** append to sche->wait_task
 *
 * @param name
 * @param func
 * @param arg
 * @return
 */
inline void IO_FUNC sche_fingerprint_new(sche_t *sche, taskctx_t *taskctx)
{
	(void)sche;
	taskctx->fingerprint++;
}

static inline int __sche_isfree(taskctx_t *taskctx)
{
        if (taskctx->state == TASK_STAT_FREE) {
                return 1;
        } else
                return 0;
}

static int __sche_create__(sche_t **_sche, const char *name, int idx,
                           void *private_mem, int *_eventfd)
{
        int ret, fd, i;
        sche_t *sche;
        taskctx_t *taskctx;

        (void) private_mem;

        ret = ltg_malloc((void **)&sche, sizeof(*sche));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(sche, 0x0, sizeof(*sche));

        if (_eventfd) {
                fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                if (fd < 0) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }

                *_eventfd = fd;
        } else {
                fd = -1;
        }

        ret = ltg_spin_init(&sche->request_queue.lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_spin_init(&sche->reply_remote_lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        INIT_LIST_HEAD(&sche->reply_remote_list);
        sche->task_hpage = NULL;
        sche->task_hpage_offset = 0;
       
        sche->running_task = -1;
        sche->task_count = 0;
        sche->id = idx;
        sche->eventfd = fd;
        sche->suspendable = 0;
        strcpy(sche->name, name);

#if 1
        ret = ltg_malloc((void **)&taskctx, sizeof(*taskctx) * TASK_MAX);
        if (unlikely(ret))
                GOTO(err_ret, ret);
#endif

        LTG_ASSERT(taskctx);
        memset(taskctx, 0x0, sizeof(*taskctx) * TASK_MAX);

        INIT_LIST_HEAD(&sche->running_task_list);
        count_list_init(&sche->free_task);

        for (i = 0; i < TASK_MAX; ++i) {
                taskctx[i].id = i;
                taskctx[i].stack = NULL;
                taskctx[i].state = TASK_STAT_FREE;
                count_list_add_tail(&taskctx[i].running_hook, &sche->free_task);
        }

        count_list_init(&sche->wait_task);
        for (i = 0; i < SCHE_GROUP_MAX; i++) {
                count_list_init(&sche->runable[i]);
        }

        sche->tasks = taskctx;
        sche->running = 1;
        sche->size = TASK_MAX;

        __sche__ = sche;
        if (_sche)
                *_sche = sche;

        return 0;
err_ret:
        return ret;
}

static int __sche_create(int *eventfd, const char *name, int idx,
                         sche_t **_sche, void *private_mem)
{
        int ret;
        sche_t *sche = NULL;

        LTG_ASSERT(__sche_array__);

        ret = ltg_spin_lock(&__sche_array_lock__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (__sche_array__[idx]) {
                ret = EEXIST;
                goto err_lock;
        }

        DINFO("create sche[%d] name %s\n", idx, name);

        if (ltgconf_global.solomode) {
                (void)private_mem;
                ret = __sche_create__(&sche, name, idx, NULL, eventfd);
        } else {
                ret = __sche_create__(&sche, name, idx, private_mem, eventfd);
        }

        if (unlikely(ret))
                GOTO(err_lock, ret);

        __sche_array__[idx] = sche;
        if (_sche)
                *_sche = sche;

        ltg_spin_unlock(&__sche_array_lock__);

        return 0;
err_lock:
        ltg_spin_unlock(&__sche_array_lock__);
err_ret:
        return ret;
}

int sche_create(int *eventfd, const char *name, int *idx,
                    sche_t **_sche, void *private_mem)
{
        int ret, i;

        for (i = 0; i < SCHEDULE_MAX; i++) {
                ret = __sche_create(eventfd, name, i, _sche, private_mem);
                if (unlikely(ret)) {
                        if (ret == EEXIST)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                if (idx)
                        *idx = i;
                break;
        }

        return 0;
err_ret:
        return ret;
}

static taskctx_t * IO_FUNC __sche_task_pop(sche_t *sche, int group)
{
        count_list_t *list;
        taskctx_t *taskctx;

        list = &sche->runable[group];

        if (!list_empty(&list->list)) {
                taskctx = (void *)list->list.next;
                count_list_del(&taskctx->hook, list);

                DBUG("run task %s, group %u\n", taskctx->name);

                return taskctx;
        } else {
                return NULL;
        }
}


static int IO_FUNC __sche_group_run(sche_t *sche, int group)
{
        int count = 0;
        taskctx_t *taskctx;

        while (1) {
                taskctx = __sche_task_pop(sche, group);
                if (unlikely(taskctx == NULL)) {
                        break;
                }

#if SCHEDULE_CHECK_IOPS
                int64_t used;

                sche->nr_run2++;
                if (sche->nr_run2 % 10000 == 0) {
                        used = _time_used(&sche->t1, &sche->t2);
                        if (used >= 1000000) {
                                DDUG("sche %p name %s run1 %ju run2 %ju iops %ju\n",
                                     sche, taskctx->name,
                                     sche->nr_run1, sche->nr_run2,
                                     (sche->nr_run2 - sche->nr_run1) / used);
                                sche->t1 = sche->t2;
                                sche->nr_run1 = sche->nr_run2;
                        }
                }
#endif

                count++;
                LTG_ASSERT(taskctx->state == TASK_STAT_RUNNABLE);
                __sche_exec__(sche, taskctx);
        }

        return count;
}

static void __sche_request_queue_run(sche_t *sche)
{
        int ret, count, i;
        request_queue_t *request_queue = &sche->request_queue;
        request_t _request[REQUEST_QUEUE_MAX], *request;
        
        ret = ltg_spin_lock(&request_queue->lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        memcpy(_request, request_queue->requests, sizeof(request_t)
               * request_queue->count);
        count = request_queue->count;
        request_queue->count = 0;
        ltg_spin_unlock(&request_queue->lock);

        for (i = 0; i < count; ++i) {
                request = &_request[i];
                sche_task_new(request->name, request->exec,
                              request->arg, request->group);
        }
}

static void __sche_reply_remote_run(sche_t *sche)
{
        int ret;
        struct list_head list, *pos, *n;
        reply_remote_t *reply;

        INIT_LIST_HEAD(&list);

        ret = ltg_spin_lock(&sche->reply_remote_lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        list_splice_init(&sche->reply_remote_list, &list);

        ltg_spin_unlock(&sche->reply_remote_lock);

        list_for_each_safe(pos, n, &list) {
                list_del(pos);
                reply = (void *)pos;

                ret = __sche_queue(sche, &reply->task, reply->retval, &reply->buf, 0);
                if (unlikely(ret)) {
                        LTG_ASSERT(ret == ESTALE);
                }

                ltg_free((void **)&reply);
        }
}

static void IO_FUNC __sche_reply_local_run(sche_t *sche)
{
        int ret, count = 0, i;
        reply_queue_t *reply_local = &sche->reply_local;
        reply_t _reply[REPLY_QUEUE_MAX], *reply;

        LTG_ASSERT(reply_local->count <= REPLY_QUEUE_MAX);
        memcpy(_reply, reply_local->replys, sizeof(reply_t) * reply_local->count);
        count = reply_local->count;
        reply_local->count = 0;

        for (i = 0; i < count; ++i) {
                reply = &_reply[i];
                DBUG("**** rep %u\n", reply->task.taskid);
                ret = __sche_queue(sche, &reply->task, reply->retval, reply->buf, 0);
                if (unlikely(ret)) {
                        LTG_ASSERT(ret == ESTALE);
                }

                slab_stream_free(reply->buf);
        }
}

static int IO_FUNC __sche_run(sche_t *sche)
{
        int count = 0;

        if (sche->reply_local.count) {
                __sche_reply_local_run(sche);
        }

        for (int i = 0; i < SCHE_GROUP_MAX; i++) {
                count += __sche_group_run(sche, i);
        }

        return count;
}

void IO_FUNC sche_run(sche_t *_sche)
{
        int count;
        sche_t *sche = __sche_self(_sche);
        
        if (unlikely(!list_empty(&sche->reply_remote_list))) {
                __sche_reply_remote_run(sche);
        }

        if (unlikely(sche->request_queue.count)) {
                __sche_request_queue_run(sche);
        }
        
        do {
                count = __sche_run(sche);
        } while (count);
}

void IO_FUNC sche_post(sche_t *sche)
{
        int ret;
        uint64_t e = 1;

        DBUG("eventfd %d\n", sche->eventfd);
        if (unlikely(sche->eventfd != -1)) {
                ret = write(sche->eventfd, &e, sizeof(e));
                if (ret < 0) {
                        ret = errno;
                        DERROR("errno %u\n", ret);
                        LTG_ASSERT(0);
                }
        }
}

void sche_task_given(task_t *task)
{
        if (likely(sche_running())) {
                sche_t *sche = sche_self();

                task->scheid = sche->id;
                task->taskid = sche->running_task;
                task->fingerprint = TASK_SCHEDULE;
        } else {
                task->taskid = __gettid();
                task->scheid = getpid();
                task->fingerprint = TASK_THREAD;
        }
}

int sche_request(sche_t *sche, int group, func_t exec, void *arg, const char *name)
{
        int ret;
        request_queue_t *request_queue = &sche->request_queue;
        request_t *request;

        LTG_ASSERT(strlen(name) + 1 <= SCHE_NAME_LEN);

        ret = ltg_spin_lock(&request_queue->lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        if (request_queue->count == request_queue->max) {
                if (request_queue->count + REQUEST_QUEUE_STEP == REQUEST_QUEUE_MAX) {
                        ret = ENOSPC;
                        UNIMPLEMENTED(__DUMP__);
                        GOTO(err_lock, ret);
                }

                DINFO("new request_queue array %u\n", request_queue->max + REQUEST_QUEUE_STEP);

                ret = ltg_realloc((void **)&request_queue->requests,
                               sizeof(*request) * request_queue->max,
                               sizeof(*request) * (request_queue->max + REQUEST_QUEUE_STEP));
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                request_queue->max += REQUEST_QUEUE_STEP;
        }

        DBUG("count %u max %u\n", request_queue->count, request_queue->max);

        request = &request_queue->requests[request_queue->count];
        request->exec = exec;
        request->arg = arg;
        request->group = group;
        snprintf(request->name, SCHE_NAME_LEN, "%s", name);
        request_queue->count++;

        ltg_spin_unlock(&request_queue->lock);

        sche_post(sche);

        return 0;
err_lock:
        ltg_spin_unlock(&request_queue->lock);
//err_ret:
        return ret;
}

void sche_dump(sche_t *sche, int block)
{
        int i, count = 0;
        taskctx_t *taskctx;
        struct timeval t1;
        uint64_t used;
        taskctx_t *tasks = sche->tasks;

        _gettimeofday(&t1, NULL);

        list_for_each_entry_safe(taskctx, tasks, &sche->running_task_list, running_hook) {
                i = taskctx->id;
                if (taskctx->state != TASK_STAT_FREE) {
                        count ++;
                        used = _time_used(&taskctx->ctime, &t1);
                        DINFO("%s[%u] %s status %u used %fms\n", sche->name, i,
                              taskctx->name, taskctx->state, (double)used / 1000);
                }
        }

        DINFO("%s used %u\n", sche->name, count);
        sche->backtrace = 1;
        sche_post(sche);

        while (block) {
                usleep(1000);
                if (sche->backtrace == 0) {
                        break;
                }
        }
}

void IO_FUNC sche_scan(sche_t *_sche)
{
        int i, time_used, used = 0;
        sche_t *sche = __sche_self(_sche);
        taskctx_t *taskctx, *tasks = sche->tasks;

        LTG_ASSERT(sche);

        (void) i;
        list_for_each_entry_safe(taskctx, tasks, &sche->running_task_list, running_hook) {
                i = taskctx->id;
                if (likely(taskctx->state != TASK_STAT_FREE && taskctx->wait_begin)) {
                        time_used = gettime() - taskctx->wait_begin;
			used++;
                        DBUG("task %s\n", taskctx->name);
                        __sche_check_scan_used(sche, taskctx, time_used);
                }
        }

}

void sche_backtrace()
{
        int i;
        sche_t *sche = sche_self();
        taskctx_t *taskctx, *tasks = sche->tasks;

        if (!sche->backtrace) {
                return;
        }

        sche->backtrace = 0;

        LTG_ASSERT(sche);

        list_for_each_entry_safe(taskctx, tasks, &sche->running_task_list, running_hook) {
                i = taskctx->id;
                int time_used = gettime() - taskctx->wait_begin;
                if (taskctx->state != TASK_STAT_FREE) {
                        DINFO("%s[%u][%u] %s status %u time_used %us\n", sche->name,
                              sche->id, i, taskctx->name, taskctx->state, time_used);
                        __sche_backtrace_set(taskctx);
                }
        }
}

int sche_init()
{
        int ret;
        sche_t **sche;

        DINFO("sche malloc %lu\n", sizeof(**sche) * SCHEDULE_MAX);

        ret = ltg_malloc((void **)&sche, sizeof(**sche) * SCHEDULE_MAX);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(sche, 0x0, sizeof(**sche) * SCHEDULE_MAX);

        ret = ltg_spin_init(&__sche_array_lock__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        __sche_array__ = sche;

        ret = sche_thread_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

#if ENABLE_SCHEDULE_LOCK_CHECK

void sche_lock_set(int lock, int ref)
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        if (sche == NULL)
                return;

        LTG_ASSERT(sche);
        LTG_ASSERT(sche->running_task != -1);
        taskctx = &sche->tasks[sche->running_task];

        if (lock) {
                taskctx->lock_count += lock;
                LTG_ASSERT(taskctx->lock_count >= 0);
        }

        if (ref) {
                taskctx->ref_count += ref;
                LTG_ASSERT(taskctx->ref_count >= 0);
        }
}

int sche_assert_retry()
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        (void) taskctx;

        if (!(sche && sche->running_task != -1)) {
                return 0;
        }

        taskctx = &sche->tasks[sche->running_task];
        // TODO core
        LTG_ASSERT(taskctx->lock_count == 0);
        if (taskctx->ref_count) {
                DERROR("task %s ref count %d\n", taskctx->name, taskctx->ref_count);
        }

        return 0;
}
#endif

void sche_value_set(int key, uint32_t value)
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        LTG_ASSERT(key < TASK_VALUE_MAX);

        if (unlikely(!(sche && sche->running_task != -1))) {
        } else {
                taskctx = &sche->tasks[sche->running_task];
                taskctx->value[key] = value;
        }
}

void sche_value_get(int key, uint32_t *value)
{
        sche_t *sche = sche_self();
        taskctx_t *taskctx;

        LTG_ASSERT(key < TASK_VALUE_MAX);

        if (!(sche && sche->running_task != -1)) {
                *value = -1;
        } else {
                taskctx = &sche->tasks[sche->running_task];
                *value = taskctx->value[key];
        }
}

int sche_getid()
{
        sche_t *sche = sche_self();

        if (unlikely(sche == NULL))
                return -1;
        else
                return sche->id;
}

