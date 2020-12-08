#ifndef __LTG_SCHE_H__
#define __LTG_SCHE_H__

/** @file scher
 *
 * 协程是一种非连续执行的机制，每个core thread一个调度器
 *
 * 目前调度器的调度策略：公平调度，不支持优先级调度
 * 对并发运行的任务数有一定限制：1024
 *
 * 一些约束：
 * - 每个task有64K的stack，所以不能声明太大的stack上数据，特别是数量多的数组，或大对象。
 * - 一个任务的总执行时间不能超过180s，否则会timeout，导致进程退出
 * - IDLE状态的代码，不能加锁，会形成deadlock (@see __rpc_table_check)
 */

#include <ucontext.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>

#include "3part/libringbuf.h"
#include "ltg_utils.h"
#include "sche_thread.h"

#define ENABLE_SCHEDULE_SELF_DEBUG 1
#define ENABLE_SCHEDULE_STACK_ASSERT 1

#define SCHEDULE_CHECK_IOPS 0

#define TASK_MAX (16384)

#define REQUEST_QUEUE_STEP 128
#define REQUEST_QUEUE_MAX (TASK_MAX * 1)

#define REPLY_QUEUE_STEP 128
#define REPLY_QUEUE_MAX TASK_MAX
#define SCHE_NAME_LEN 32

#define KEEP_STACK_SIZE (1024)
#define DEFAULT_STACK_SIZE (1024 * 128)

#if 1
#define NEW_SCHED
#endif

#ifndef NEW_SCHED
#define swapcontext1 swapcontext
#endif

#define SCHEDULE_STATUS_NONE 0
#define SCHEDULE_STATUS_IDLE 1
#define SCHEDULE_STATUS_RUNNING 2

typedef struct reply_remote {
        struct list_head hook;
        ltgbuf_t buf;
        task_t task;
        int retval;
} reply_remote_t;

typedef enum {
        TASK_STAT_FREE = 10, //0
        TASK_STAT_RUNNABLE,  //1
        TASK_STAT_RUNNING,   //2        //previous was conflicating with spdk.
        TASK_STAT_BACKTRACE, //3
        TASK_STAT_SUSPEND,   //4
        //TIMEOUT, //5
} taskstate_t;

struct cpu_ctx {
        void *rsp;
        void *rbp;
        void *rip;
        void *rbx;
        void *r12;
        void *r13;
        void *r14;
        void *r15;
        void *rdi;
}  __attribute__((__aligned__(CACHE_LINE_SIZE)));

typedef enum {
        TASK_VALUE_LEASE = 0,
        TASK_VALUE_REPLICA = 1,
        TASK_VALUE_MAX,
} taskvalue_t;

typedef struct __task {
        // must be first member
        struct list_head hook;
        struct list_head running_hook;

#ifdef NEW_SCHED
        struct cpu_ctx main;
        struct cpu_ctx ctx;
#else
        //ltg_spinlock_t lock;
        ucontext_t ctx;          // coroutine context
        ucontext_t main;         // scher context(core thread)
#endif

        void *sche;
        void *stack;
        // for sche->running_task_list;

        char name[MAX_NAME_LEN];
        int id;
        ltg_time_t ctime; /*live time*/
        ltg_time_t rtime; /*running time*/

#if ENABLE_SCHEDULE_LOCK_CHECK
        int lock_count;
        int ref_count;
#endif

        taskstate_t state;
        char pre_yield;
        char sleeping;
        int8_t step;
        int8_t group;
        int8_t wait_tmo;

        func_t func;
        void *arg;

        //uint32_t fingerprint_prev;
        uint32_t fingerprint;

        time_t wait_begin;

        uint32_t value[TASK_VALUE_MAX];

        const char *wait_name;
        const void *wait_opaque;
        int32_t retval;
        int sleep;
        // last member?
} taskctx_t;

typedef struct {
        void (*exec)(void *arg);
        void *arg;
        int8_t group;
        char name[SCHE_NAME_LEN];
} request_t;

typedef struct {
        ltg_spinlock_t lock;
        int count;
        int max;
        request_t *requests;
} request_queue_t;

typedef struct {
        task_t task;
        int retval;
} reply_t;

typedef struct {
        ltg_spinlock_t lock;
        int count;
        int max;
        reply_t *replys;
} reply_queue_t;

#if 0
#define SCHE_GROUP0 0
#define SCHE_GROUP1 1
#define SCHE_GROUP2 2
#define SCHE_GROUP3 3
//#define SCHE_GROUP4 4
#define SCHE_GROUP_MAX 4

#else

#define SCHE_GROUP0 0
#define SCHE_GROUP1 0
#define SCHE_GROUP2 0
#define SCHE_GROUP3 0
//#define SCHE_GROUP4 0
#define SCHE_GROUP_MAX 1

#endif

typedef struct sche_t {
        // scher
        char name[32];
        int id;

#if SCHEDULE_CHECK_IOPS
        struct timeval t1, t2;
        uint64_t nr_run1, nr_run2;
#endif
        // uint64_t hz;
        // scher status
        int eventfd;
        int running;
        int suspendable;

        // coroutine
        void *private_mem;
        taskctx_t *tasks;
        void *stack_addr;
        int size;

        // no free task count
        int task_count;
        int running_task;
        int cursor;

        // running task list
        struct list_head running_task_list;

        // free task list
        count_list_t free_task;

        // 若tasks满，缓存到wait_task
        count_list_t wait_task;

        // core_request的请求，先放入队列，而后才生成task
        request_queue_t request_queue;

        // 当前可调度的任务队列
        count_list_t runable[SCHE_GROUP_MAX];

        // resume相关, local是本调度器上的任务，remote是跨core任务(需要MT同步）
        reply_queue_t reply_local;
        
        ltg_spinlock_t reply_remote_lock;
        struct list_head reply_remote_list;

        void *task_hpage;
        int task_hpage_offset;
        
        // backtrace
        uint32_t sequence;
        uint32_t scan_seq;
        time_t last_scan;
        int backtrace;
        uint64_t counter;
        uint64_t run_time;
        uint64_t c_runtime;
} sche_t;

// API

// scher相关

typedef enum {
        TASK_SCHEDULE = 10,
        TASK_THREAD,
} task_type_t;

int sche_init();
int sche_create(int *eventfd, const char *name, int *idx, sche_t **_sche, void *private_mem);
void sche_run(sche_t *_sche);
sche_t *sche_self();
int sche_running();
int sche_suspend();
int sche_stat(int *sid, int *taskid, int *runable, int *wait_task,
              int *task_count, uint64_t *run_time, uint64_t *c_runtime);


int sche_request(sche_t *sche, int group, func_t exec, void *buf, const char *name);
int sche_task_new(const char *name, func_t func, void *arg, int group);
task_t sche_task_get();
void sche_task_given(task_t *task);
int sche_task_get1(sche_t *sche, task_t *task);

int sche_yield(const char *name, ltgbuf_t *buf, void *opaque);

/**
 *
 * @param name
 * @param buf
 * @param opaque
 * @param func
 * @param _tmo
 * @return
 */
int sche_yield1(const char *name, ltgbuf_t *buf, void *opaque, func_t func, int _tmo);

/**
 *
 * @param task
 * @param retval
 * @param buf
 */
void sche_task_post(const task_t *task, int retval, ltgbuf_t *buf);

/**
 *
 * @param name
 * @param usec
 * @return
 */
int sche_task_sleep(const char *name, suseconds_t usec);

// 当前任务相关函数
int sche_taskid();

void sche_task_setname(const char *name);

// internals

void sche_post(sche_t *sche);

void sche_scan(sche_t *sche);
void sche_backtrace();

/** task stack overflow，影响性能
 *
 */
void sche_stack_assert(sche_t *sche);
// task/coroutine

void sche_dump(sche_t *sche, int block);
void sche_task_reset();

#if ENABLE_SCHEDULE_LOCK_CHECK
void sche_lock_set(int lock, int ref);
#endif

int sche_assert_retry();
int sche_status();

void sche_value_set(int key, uint32_t value);
int sche_task_uptime();
void sche_value_get(int key, uint32_t *value);
void sche_fingerprint_new(sche_t *sche, taskctx_t *taskctx);

int sche_task_run(int group, func_va_t exec, ...);
int sche_getid();
void sche_unyielding(int unyielding);

#define SCHE_ANALYSIS 0

#endif
