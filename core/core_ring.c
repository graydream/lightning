#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define OP_REQUEST 1
#define OP_REPLY 2

#define RING_SIZE (1<<12)
#define RING_ARRAY_SIZE 128

int core_ring_init(core_t *core)
{
        int ret;
        core_ring_t *ring;

        ret = slab_static_alloc1((void **)&ring, sizeof(*ring));
        if (ret)
                GOTO(err_ret, ret);

        ret = slab_static_alloc1((void **)&ring->ringbuf,
                                 sizeof(struct ringbuf **) * CORE_MAX);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        INIT_LIST_HEAD(&ring->list);
        
        for (int i = 0; i < CORE_MAX; i++) {
                ring->ringbuf[i] = NULL;
        }

        core->ring = ring;
        
        return 0;
err_ret:
        return ret;
}

static inline void __core_ring_poller_run(void **array, int count)
{
        ring_ctx_t *ring_ctx = NULL;
        
        for (int i = 0; i < count; i++) {
                ring_ctx = array[i];
                        
                if (ring_ctx->type == OP_REPLY) {
                        ring_ctx->reply_func(ring_ctx->reply_ctx);
                } else if (ring_ctx->type == OP_REQUEST) {
                        sche_task_new("ring", ring_ctx->task_run,
                                      (void *)ring_ctx, ring_ctx->group);

                } else {
                        DWARN("%p\n", ring_ctx);
                        UNIMPLEMENTED(__DUMP__);
                }
        }
}

static inline void __core_ring_poller__(struct ringbuf *ringbuf)
{
        void *array[128];
        int count;
        
        while (1) {
                count = libringbuf_dequeue_burst(ringbuf, array,
                                                 RING_ARRAY_SIZE);
                if (count == 0)
                        break;

                DBUG("count %u\n", count);

                __core_ring_poller_run(array, count);
        }
}

typedef struct {
        struct list_head hook;
        struct ringbuf *ringbuf;
} ringlist_t;

inline void core_ring_poller(void *_core, void *var, void *arg)
{
        core_t *core = _core;
        core_ring_t *ring = core->ring;
        ringlist_t *ringlist;
        struct list_head *pos;

        (void)var;
        (void)arg;
        
        list_for_each(pos, &ring->list) {
                ringlist = (void *)pos;
                __core_ring_poller__(ringlist->ringbuf);
        }
}

int core_ring_count(core_t *core)
{
        int count = 0;
        core_ring_t *ring = core->ring;

        for (int i = 0; i < CORE_MAX; i++) {
                if (ring->ringbuf[i]) {
                        count += libringbuf_count(ring->ringbuf[i]);
                }
        }

        return count;
}

static void __core_ring_new(core_ring_t *ring, int idx)
{
        int ret;
        ringlist_t *ringlist;

        if (ring->ringbuf[idx]) {
                return;
        }
        
        ret = slab_static_alloc1((void **)&ringlist, sizeof(*ringlist));
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        ring->ringbuf[idx] = libringbuf_create((1<<12),
                                               RING_F_SP_ENQ
                                               | RING_F_SC_DEQ);

        ringlist->ringbuf = ring->ringbuf[idx];
        list_add_tail(&ringlist->hook, &ring->list);
}

static int __core_ring_connect__(va_list ap)
{
        int coreid = va_arg(ap, int);
        core_t *core = core_self();
        
        if (core->ring->ringbuf[coreid] == NULL) {
                __core_ring_new(core->ring, coreid);
        }

        return 0;
}

void __core_ring_connect(core_t *rcore, core_t *lcore,
                         struct ringbuf **request,
                         struct ringbuf **reply)
{
        if (unlikely(rcore->ring->ringbuf[lcore->hash] == NULL)) {
                int ret = core_request(rcore->hash, -1, "ring_connect",
                                    __core_ring_connect__, lcore->hash);
                LTG_ASSERT(ret == 0);
        }


        if (unlikely(lcore->ring->ringbuf[rcore->hash] == NULL)) {
                DINFO("%s[%d] connect to %s[%d]\n", lcore->name, lcore->hash,
                      rcore->name, rcore->hash);
                __core_ring_new(lcore->ring, rcore->hash);
        }

        *request = rcore->ring->ringbuf[lcore->hash];
        *reply = lcore->ring->ringbuf[rcore->hash];
}

inline static void __core_request_run__(void *_ctx)
{
        ring_ctx_t *ring_ctx = _ctx;
        int reply_coreid = ring_ctx->reply_coreid;

        ring_ctx->request_func(ring_ctx->request_ctx);
        ring_ctx->type = OP_REPLY;

        libringbuf_sp_enqueue(ring_ctx->reply, (void *)ring_ctx);

        if (unlikely(ltgconf_global.polling_timeout)) {
                core_t *rcore = core_get(reply_coreid);
                sche_post(rcore->sche);
        }
}

inline static void __core_ring_queue(int coreid, ring_ctx_t *ctx,
                              func_t request, void *requestctx,
                              func_t reply, void *replyctx)
{
        core_t *rcore;
        core_t *core = core_self();

        rcore = core_get(coreid);
        LTG_ASSERT(rcore);

        __core_ring_connect(rcore, core, &ctx->request, &ctx->reply);
        LTG_ASSERT(ctx->request && ctx->reply);

        ctx->task_run = __core_request_run__;
        ctx->type = OP_REQUEST;
        ctx->reply_coreid = core->hash;
        ctx->request_func = request;
        ctx->reply_func = reply;
        ctx->request_ctx = requestctx;
        ctx->reply_ctx = replyctx;

        return ;
}

inline void core_ring_queue(int coreid, ring_ctx_t *ctx,
                              func_t request, void *requestctx,
                              func_t reply, void *replyctx)
{
        __core_ring_queue(coreid, ctx, request, requestctx, reply, replyctx);
        LTG_ASSERT(ctx->request && ctx->reply);

        ctx->group = -1;
        
        libringbuf_sp_enqueue(ctx->request, (void *)ctx);

        if (unlikely(ltgconf_global.polling_timeout)) {
                core_t *rcore = core_get(coreid);
                sche_post(rcore->sche);
        }
        
        return ;
}

typedef struct {
        va_list ap;
        func_va_t exec;
        int retval;
} request_ctx_t;

inline static void __core_ring_reply(void *arg)
{
        sche_task_post((task_t *)arg, 0, NULL);
}

inline static void __core_ring_request(void *arg)
{
        request_ctx_t *request_ctx = arg;
        request_ctx->retval = request_ctx->exec(request_ctx->ap);
}

inline int core_ring_wait(int coreid, int group, const char *name,
                          func_va_t exec, ...)
{
        int ret;
        request_ctx_t request_ctx;
        ring_ctx_t ring_ctx, *ctx;
        task_t task;

        DBUG("core ring request\n");
        
        (void) group;
        
        va_start(request_ctx.ap, exec);
        request_ctx.exec = exec;

        ctx = &ring_ctx;
        __core_ring_queue(coreid, ctx,
                          __core_ring_request, &request_ctx,
                          __core_ring_reply, NULL);

        ret = sche_task_get1(sche_self(), &task);
        LTG_ASSERT(ret == 0);

        ctx->reply_ctx = (void *)&task;
        ctx->group = group;
        
        libringbuf_sp_enqueue(ctx->request, (void *)ctx);

        if (unlikely(ltgconf_global.polling_timeout)) {
                core_t *rcore = core_get(coreid);
                sche_post(rcore->sche);
        }
        
        ret = sche_yield1(name, NULL, NULL, NULL, -1);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = request_ctx.retval;
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}
