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

#define QUEUE_BULK 1

#if QUEUE_BULK
static __thread struct list_head __queue__;

typedef struct {
        struct list_head hook;
	struct ringbuf *ring;
        struct list_head list;
} ring_bulk_t;

static void S_LTG __core_ring_queue__(struct ringbuf *ring, ring_ctx_t *ctx)
{
        int found = 0;
        ring_bulk_t *ring_bulk;
        struct list_head *pos, *queue = &__queue__;

        DBUG("corenet_fwd %p %p\n", ring, ctx);
        
        list_for_each(pos, queue) {
                ring_bulk = (void *)pos;

                if (ring == ring_bulk->ring) {
                        list_add_tail(&ctx->hook, &ring_bulk->list);
                        found = 1;
                        break;
                }
        }

        if (found == 0) {
                ring_bulk = slab_stream_alloc(sizeof(*ring_bulk));
                LTG_ASSERT(ring_bulk);
                INIT_LIST_HEAD(&ring_bulk->list);
                ring_bulk->ring = ring;
                list_add_tail(&ctx->hook, &ring_bulk->list);
                list_add_tail(&ring_bulk->hook, queue);
        }
}

static void S_LTG __core_ring_commit__(struct ringbuf *ring, struct list_head *list)
{
        int ret, count;
        void *array[128];
        ring_ctx_t *ctx;
        struct list_head *pos, *n;

        count = 0;
        list_for_each_safe(pos, n, list) {
                ctx = (void *)pos;
                list_del(pos);

                array[count] = ctx;
                count++;

                if (count == 128) {
                        DBUG("bulk %d\n", count);

#if ENABLE_RING_MP
                        ret = libringbuf_mp_enqueue_bulk(ring, array, count);
#else
                        ret = libringbuf_sp_enqueue_bulk(ring, array, count);
#endif
                        LTG_ASSERT(ret == 0);
                        count = 0;
                }
        }
        
        if (count) {
                DBUG("bulk %d\n", count);
                        
#if ENABLE_RING_MP
                ret = libringbuf_mp_enqueue_bulk(ring, array, count);
#else
                ret = libringbuf_sp_enqueue_bulk(ring, array, count);
#endif
                LTG_ASSERT(ret == 0);
        }
}

static void S_LTG __core_ring_commit(void *_core, void *var, void *arg)
{
        struct list_head *pos, *n;
        struct list_head list;
        ring_bulk_t *ring_bulk;

        (void)var;
        (void)arg;
        (void) _core;

        INIT_LIST_HEAD(&list);
        list_splice_init(&__queue__, &list);

        list_for_each_safe(pos, n, &list) {
                ring_bulk = (void *)pos;
                list_del(pos);

                __core_ring_commit__(ring_bulk->ring,
                                     &ring_bulk->list);

                slab_stream_free(ring_bulk);
        }
}


#endif

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

#if ENABLE_RING_MP
        ring->ringbuf = libringbuf_create((1<<12), RING_F_SC_DEQ);
#else
        INIT_LIST_HEAD(&ring->list);

        for (int i = 0; i < CORE_MAX; i++) {
                ring->ringbuf[i] = NULL;
        }
#endif

        core->ring = ring;
        
#if QUEUE_BULK
        DBUG("core ring bulk\n");
        
        INIT_LIST_HEAD(&__queue__);
        
        ret = core_register_poller("__core_ring_commit", __core_ring_commit, NULL);
        if (ret)
                GOTO(err_ret, ret);
#endif
        
        return 0;
err_ret:
        return ret;
}

static void S_LTG __core_ring_poller_run(void **array, int count)
{
        ring_ctx_t *ring_ctx = NULL;
        
        for (int i = 0; i < count; i++) {
                ring_ctx = array[i];
                        
                if (ring_ctx->type == OP_REPLY) {
                        ring_ctx->reply_func(ring_ctx->reply_ctx);
                } else if (ring_ctx->type == OP_REQUEST) {
                        if (ring_ctx->run_type == RING_TASK) {
                                sche_task_new("ring", ring_ctx->task_run,
                                              (void *)ring_ctx, ring_ctx->group);
                        } else {
                                ring_ctx->task_run(ring_ctx);
                        }
                } else {
                        DWARN("%p\n", ring_ctx);
                        UNIMPLEMENTED(__DUMP__);
                }
        }
}

static void S_LTG __core_ring_poller__(struct ringbuf *ringbuf)
{
        void *array[128];
        int count;
        
        while (1) {
                count = libringbuf_sc_dequeue_burst(ringbuf, array,
                                                    RING_ARRAY_SIZE);
                if (count == 0)
                        break;

                DBUG("count %u\n", count);

                __core_ring_poller_run(array, count);
        }
}

#if !ENABLE_RING_MP
typedef struct {
        struct list_head hook;
        struct ringbuf *ringbuf;
} ringlist_t;
#endif

void S_LTG core_ring_poller(void *_core, void *var, void *arg)
{
        core_t *core = _core;
        core_ring_t *ring = core->ring;

        (void)var;
        (void)arg;

#if ENABLE_RING_MP
        __core_ring_poller__(ring->ringbuf);
#else
        ringlist_t *ringlist;
        struct list_head *pos;
        list_for_each(pos, &ring->list) {
                ringlist = (void *)pos;
                __core_ring_poller__(ringlist->ringbuf);
        }
#endif
}

int core_ring_count(core_t *core)
{
        int count = 0;
        core_ring_t *ring = core->ring;

#if ENABLE_RING_MP
        count = libringbuf_count(ring->ringbuf);
#else
        for (int i = 0; i < CORE_MAX; i++) {
                if (ring->ringbuf[i]) {
                        count += libringbuf_count(ring->ringbuf[i]);
                }
        }
#endif

        return count;
}

#if !ENABLE_RING_MP
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
#endif

void S_LTG __core_ring_connect(core_t *rcore, core_t *lcore,
                               struct ringbuf **request,
                               struct ringbuf **reply)
{
#if ENABLE_RING_MP
        *request = rcore->ring->ringbuf;
        *reply = lcore->ring->ringbuf;
#else
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
#endif
}

void S_LTG core_ring_reply(ring_ctx_t *ring_ctx)
{
        ring_ctx->type = OP_REPLY;

#if QUEUE_BULK
        DBUG("reply bulk\n");
        
        __core_ring_queue__(ring_ctx->reply, ring_ctx);
#else
        libringbuf_enqueue(ring_ctx->reply, (void *)ring_ctx);
#endif

        if (unlikely(ltgconf_global.polling_timeout)) {
                int reply_coreid = ring_ctx->reply_coreid;
                core_t *rcore = core_get(reply_coreid);
                sche_post(rcore->sche);
        }
}

static void S_LTG __core_ring_run_task(void *_ctx)
{
        ring_ctx_t *ring_ctx = _ctx;

        ring_ctx->request_func(ring_ctx->request_ctx);

        core_ring_reply(ring_ctx);
}

static void S_LTG __core_ring_run_queue(void *_ctx)
{
        ring_ctx_t *ring_ctx = _ctx;

        ring_ctx->request_func(ring_ctx->request_ctx);
}


static void S_LTG __core_ring_queue(int coreid, int type, ring_ctx_t *ctx,
                                     func_t request, void *requestctx,
                                     func_t reply, void *replyctx)
{
        core_t *rcore;
        core_t *core = core_self();

        rcore = core_get(coreid);
        LTG_ASSERT(rcore);

        __core_ring_connect(rcore, core, &ctx->request, &ctx->reply);
        LTG_ASSERT(ctx->request && ctx->reply);

        if (type == RING_TASK) {
                ctx->task_run = __core_ring_run_task;
        } else {
                ctx->task_run = __core_ring_run_queue;
        }

        ctx->type = OP_REQUEST;
        ctx->reply_coreid = core->hash;
        ctx->request_func = request;
        ctx->reply_func = reply;
        ctx->request_ctx = requestctx;
        ctx->reply_ctx = replyctx;

#if QUEUE_BULK
        __core_ring_queue__(ctx->request, ctx);
#endif

        return ;
}

void S_LTG core_ring_queue(int coreid, int type, ring_ctx_t *ctx,
                           func_t request, void *requestctx,
                           func_t reply, void *replyctx)
{
        LTG_ASSERT(type == RING_TASK || type == RING_QUEUE);
        
        __core_ring_queue(coreid, type, ctx,
                          request, requestctx,
                          reply, replyctx);
        LTG_ASSERT(ctx->request && ctx->reply);

        ctx->group = -1;
        ctx->run_type = type;
        
#if !QUEUE_BULK
        libringbuf_enqueue(ctx->request, (void *)ctx);
#endif

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

int S_LTG core_ring_wait(int coreid, int type, const char *name,
                          func_va_t exec, ...)
{
        int ret;
        request_ctx_t request_ctx;
        ring_ctx_t ring_ctx, *ctx;
        task_t task;

        LTG_ASSERT(type == RING_TASK || type == RING_QUEUE);
        
        DBUG("core ring request\n");
        
        va_start(request_ctx.ap, exec);
        request_ctx.exec = exec;

        ctx = &ring_ctx;
        ctx->run_type = type;
        ctx->group = -1;
        __core_ring_queue(coreid, type, ctx,
                          __core_ring_request, &request_ctx,
                          __core_ring_reply, NULL);

        ret = sche_task_get1(sche_self(), &task);
        LTG_ASSERT(ret == 0);

        ctx->reply_ctx = (void *)&task;

#if !QUEUE_BULK
        libringbuf_enqueue(ctx->request, (void *)ctx);
#endif

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
