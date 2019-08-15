#ifndef __CORE_H__
#define __CORE_H__

#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>

#include "ltg_net.h"
#include "ltg_utils.h"
#include "sche.h"
#include "cpuset.h"

typedef enum {
        VARIABLE_CORE,
        VARIABLE_MAPING,
        VARIABLE_SLAB_STATIC,
        VARIABLE_SLAB_STREAM,
        VARIABLE_CORENET_TCP,
        VARIABLE_CORENET_RDMA,
        VARIABLE_CORENET_RING,
        VARIABLE_CORERPC,
        VARIABLE_SCHEDULE,
        VARIABLE_HUGEPAGE,
        VARIABLE_TIMER,
        VARIABLE_GETTIME,
        VARIABLE_ANALYSIS,
        VARIABLE_KEEP,
} variable_type_t;

typedef int (*core_exec)(void *ctx, void *buf, int *count);
typedef int (*core_exec1)(void *ctx, void *msg_buf);
typedef int (*core_reconnect)(int *fd, void *ctx);
typedef int (*core_func)();
typedef void (*core_exit)();

#define CORE_MAX 64

typedef struct {
        struct list_head hook;
	func_va_t func_va;
	func_t task_run;
	int type;
	int reply_coreid;
	int group;
	//int retval;
	//task_t task;

        func_t request_func;
        void *request_ctx;
        func_t reply_func;
        void *reply_ctx;
        
	struct ringbuf *request;
	//va_list ap;
	//for reply
	struct ringbuf *reply;
} ring_ctx_t __attribute__((__aligned__(CACHE_LINE_SIZE)));

typedef struct __routine {
        struct list_head hook;
        char name[64];
        func2_t func;
        void *ctx;
} routine_t;

#define ENABLE_RING_REQUEST_QUEUE 0

typedef struct {
        struct ringbuf **ringbuf;
        struct list_head list;
} core_ring_t;

//typedef core_t;

typedef struct __core {
        core_ring_t *ring;
        char name[MAX_NAME_LEN];
        int interrupt_eventfd;   // === sche->eventfd, 通知机制

        int sche_idx;
        int hash;
        int flag;
        coreinfo_t *main_core;
        void *maping;
        void *rpc_table;
        void *corenet;
        //void *private_mem;

        sche_t *sche;

        ltg_spinlock_t keepalive_lock; // for keepalive
        time_t keepalive;

        sem_t sem;
        struct list_head poller_list;
        struct list_head routine_list;
        struct list_head destroy_list;
        struct list_head nvme_poll_list;
        
        time_t last_scan;
        struct list_head scan_list;
        uint64_t stat_nr1;
        uint64_t stat_nr2;
        struct timeval  stat_t1;
        struct timeval  stat_t2;
        void *tls[LTG_TLS_MAX];
} core_t;

//typedef void (*poller_t)(core_t *core, void *ctx);
#define CORE_FLAG_POLLING 0x0020

int core_init(uint64_t mask, int flag);
int core_usedby(uint64_t mask, int idx);
int core_used(int idx);
uint64_t core_mask();
int core_attach(int hash, const sockid_t *sockid, const char *name, void *ctx,
                core_exec func, func_t reset, func_t check);
core_t *core_get(int hash);
core_t *core_self();

int core_request(int coreid, int group, const char *name, func_va_t exec, ...);
int core_ring_wait(int hash, int priority, const char *name, func_va_t exec, ...);
void __core_tls_set(int type, void *ptr);
void *__core_tls_get(void *core, int type);
void core_tls_set(int type, void *ptr);
void *core_tls_get(void *core, int type);

int core_islocal(const coreid_t *coreid);
int core_getid(coreid_t *coreid);
int core_init_modules(const char *name, func_va_t exec, ...);
int core_init_modules1(const char *name, uint64_t coremask, func_va_t exec, ...);
void core_occupy(const char *name, uint64_t coremask);
void core_iterator(func1_t func, const void *opaque);
void core_latency_update(uint64_t used);
int core_dump_memory(uint64_t *memory);
int core_latency_init();

int core_register_destroy(const char *name, func2_t func, void *ctx);
int core_register_poller(const char *name, func2_t func, void *ctx);
int core_register_routine(const char *name, func2_t func, void *ctx);
int core_register_scan(const char *name, func2_t func, void *ctx);
uint32_t get_io();
int perf_init(int target_begin, int target_end);

#if 1
void core_ring_queue(int coreid, ring_ctx_t *ctx,
                      func_t request, void *requestctx,
                      func_t reply, void *replyctx);
#endif
void  core_ring_poller(void *_core, void *var, void *arg);
void  tgt_core_ring_poller(void *_core, void *var, void *arg);
void core_worker_run(core_t *core);

#define CORE_ANALYSIS_BEGIN(mark)               \
        struct timeval t1##mark, t2##mark;      \
        int used##mark;                         \
                                                \
        _gettimeofday(&t1##mark, NULL);         \


#define CORE_ANALYSIS_UPDATE(mark, __usec, __str)                       \
        _gettimeofday(&t2##mark, NULL);                                 \
        used##mark = _time_used(&t1##mark, &t2##mark);                  \
        core_latency_update(used##mark);                                \
        if (used##mark > (__usec)) {                                    \
                if (used##mark > 1000 * 1000 * ltgconf.rpc_timeout) {   \
                        DWARN_PERF("analysis used %f s %s, timeout\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                } else {                                                \
                        DINFO_PERF("analysis used %f s %s\n", (double)(used##mark) / 1000 / 1000, (__str) ? (__str) : ""); \
                }                                                       \
        }                                                               \

uint64_t core_latency_get();

typedef struct {
        int count;
        int coreid[CORE_MAX];
} coremask_t;

void coremask_trans(coremask_t *coremask, uint64_t mask);
int coremask_hash(const coremask_t *coremask, uint64_t id);

#endif
