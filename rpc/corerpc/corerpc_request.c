#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define SEND_TASK 2
#define SEND_QUEUE 3

#define CORE_CHECK 0

#if 0
#undef ENABLE_NETCTL_QUEUE
#define ENABLE_NETCTL_QUEUE 0
#endif

typedef struct {
        corerpc_op_t *op;
        const char *name;
        task_t task;
        ring_ctx_t ring_ctx;
        int retval;
#if CORE_CHECK
        int coreid;
#endif
} corerpc_ring_ctx_t;

typedef struct {
        sem_t sem;
        task_t task;
        uint64_t latency;
        corerpc_op_t op;
        int coreid;
        corerpc_ring_ctx_t *ctx;
} rpc_ctx_t;

extern rpc_table_t *corerpc_self_byctx(void *);
extern rpc_table_t *corerpc_self();
extern int corerpc_inited;

static void S_LTG __corerpc_post_task(void *arg1, void *arg2, void *arg3, void *arg4)
{
        rpc_ctx_t *ctx = arg1;
        int retval = *(int *)arg2;
        ltgbuf_t *buf = arg3;
        uint64_t latency = *(uint64_t *)arg4;
        corerpc_op_t *op = &ctx->op;
        
        ctx->latency = latency;

        if (buf && buf->len) {
                LTG_ASSERT(op->rbuf);
                LTG_ASSERT(op->rbuf->len >= buf->len);
                ltgbuf_clone1(op->rbuf, buf, 0);
                ltgbuf_free(buf);
        }

        sche_task_post(&ctx->task, retval, NULL);
}

static void __corerpc_request_reset(const msgid_t *msgid, int retval)
{
        rpc_table_t *__rpc_table_private__ = corerpc_self();

        (void) retval;

        DBUG("reset (%d, %d)\n", msgid->idx, msgid->figerprint);
#if 1
        rpc_table_post(__rpc_table_private__, msgid, retval, NULL, 0);
#else
        rpc_table_free(__rpc_table_private__, msgid);
#endif
}

STATIC int S_LTG __corerpc_getslot(void *_ctx, const char *name,
                                   rpc_ctx_t *ctx, func3_t func3, int type)
{
        int ret;
        rpc_table_t *__rpc_table_private__ = corerpc_self_byctx(_ctx);
        corerpc_op_t *op = &ctx->op;

        ANALYSIS_BEGIN(0);

        ret = rpc_table_getslot(__rpc_table_private__, &op->msgid, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (type == SEND_TASK) {
                ctx->task = sche_task_get();
        } else {
                ctx->task.taskid = -1;
                ctx->task.fingerprint = -1;
        }

        ret = rpc_table_setslot(__rpc_table_private__, &op->msgid,
                                func3, ctx, &op->sockid,
                                &op->netctl.nid, op->timeout);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ANALYSIS_QUEUE(0, IO_INFO, NULL);

        return 0;
err_ret:
        return ret;
}

STATIC int S_LTG __corerpc_wait__(const char *name, ltgbuf_t *rbuf,
                                  rpc_ctx_t *ctx)
{
        int ret;

        DBUG("%s yield wait\n", name);
        ret = sche_yield(name, rbuf, ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        DBUG("%s yield resume\n", name);

        return 0;
err_ret:
        return ret;
}

#if ENABLE_RDMA

static void __corerpc_msgid_prep(msgid_t *msgid, const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                                 int msg_size, const rdma_conn_t *handler)
{

        if (msg_size <= 0)
                return;
        memset(&msgid->data_prop, 0x00, sizeof(data_prop_t));
	if (wbuf != NULL) {
		msgid->data_prop.rkey = handler->mr->rkey;
		ltgbuf_trans_addr((void **)msgid->data_prop.remote_addr, wbuf);
		msgid->data_prop.size = msg_size;
	} else if (rbuf != NULL){
                LTG_ASSERT((int)rbuf->len == msg_size);

		msgid->data_prop.rkey = handler->mr->rkey;
		ltgbuf_trans_addr((void **)msgid->data_prop.remote_addr, rbuf);
		msgid->data_prop.size = msg_size;
	}

        MSGID_DUMP(msgid);
}

#endif

int S_LTG corerpc_rdma_request(void *ctx, void *_op)
{
        int ret;
        ltgbuf_t buf;
        corerpc_op_t *op = _op;
        rdma_conn_t *handler = (rdma_conn_t *)op->sockid.rdma_handler;

        (void) ctx;
        
        __corerpc_msgid_prep(&op->msgid, op->wbuf, op->rbuf, op->msg_size, handler);

        ret = rpc_request_prep(&buf, &op->msgid, op->request, op->reqlen,
                               op->replen, op->wbuf, op->msg_type, 0, op->group,
                               op->coreid.idx);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = corenet_rdma_send(&op->sockid, &buf, NULL, 0, 0, build_post_send_req);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

        return 0;
err_free:
        ltgbuf_free(&buf);
err_ret:
        return ret;
}

int corerpc_tcp_request(void *ctx, void *_op)
{
        int ret;
        ltgbuf_t buf;
        corerpc_op_t *op = _op;

        ret = rpc_request_prep(&buf, &op->msgid, op->request, op->reqlen,
                               op->replen, op->wbuf, op->msg_type, 1, op->group,
                               op->coreid.idx);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = corenet_tcp_send(ctx, &op->sockid, &buf);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }
        
        return 0;
err_free:
        ltgbuf_free(&buf);
err_ret:
        return ret;
}

static int S_LTG __corerpc_send_sock(void *core, const char *name,
                                     rpc_ctx_t *ctx, func3_t func3, int type)
{
        int ret;
        corerpc_op_t *op = &ctx->op;

        ANALYSIS_BEGIN(0);

        DBUG("%s\n", name);

        ret = __corerpc_getslot(core, name, ctx, func3, type);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = op->sockid.request(core, op);
        if (unlikely(ret)) { 
                if (sche_running()) {
                        sche_task_reset();
                }

                __corerpc_request_reset(&op->msgid, ret);
                corenet_maping_close(&op->netctl.nid, &op->sockid);
                //reset rpc table, do not return errno;

                goto out;
#if 0
                sche_task_reset();
		ret = _errno_net(ret);
		LTG_ASSERT(ret == ENONET || ret == ESHUTDOWN);
                GOTO(err_ret, ret);
#endif
	}

        DBUG("%s msgid (%u, %x) to %s\n", name, &op->msgid.idx,
             op->msgid.figerprint, _inet_ntoa(op->sockid.addr));

        SOCKID_DUMP(&op->sockid);
        MSGID_DUMP(&op->msgid);

out:
        ANALYSIS_QUEUE(0, IO_INFO, NULL);
        
        return 0;
err_ret:
        return ret;
}

static int S_LTG __corerpc_send(const char *name, rpc_ctx_t *ctx, func3_t func,
                                int type)
{
        int ret;
        core_t *core = core_self();
        corerpc_op_t *op = &ctx->op;

        ANALYSIS_BEGIN(0);

        if (!netable_connected(&op->netctl.nid)) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }
        
        ret = corenet_maping(core, &op->netctl, &op->sockid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("send to %s/%d, sd %u\n", netable_rname(&op->netctl.nid),
             op->netctl.idx, op->sockid.sd);

        ret = __corerpc_send_sock(core, name, ctx, func, type);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ANALYSIS_QUEUE(0, IO_INFO, NULL);

        return 0;
err_ret:
        return ret;
}

static int S_LTG __corerpc_postwait(const char *name, const corerpc_op_t *op)
{
        int ret;
        rpc_ctx_t ctx;

        ctx.op = *op;

        ret = __corerpc_send(name, &ctx, __corerpc_post_task, SEND_TASK);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __corerpc_wait__(name, op->rbuf, &ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }
        
        return 0;
err_ret:
        return ret;
}

int corerpc_postwait_sock(const char *name, const coreid_t *coreid,
                          const sockid_t *sockid, const void *request,
                          int reqlen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                          int msg_type, int msg_size, int group, int timeout)
{
        int ret;
        rpc_ctx_t ctx;
        corerpc_op_t *op = &ctx.op;

        op->coreid = *coreid;
        op->netctl = *coreid;
        op->request = request;
        op->reqlen = reqlen;
        op->wbuf = wbuf;
        op->rbuf = rbuf;
        op->group = group;
        op->msg_type = msg_type;
        op->msg_size = msg_size;
        op->timeout = timeout;
        op->sockid = *sockid;

        LTG_ASSERT(ltgconf_global.daemon);
        core_t *core = core_self();
        ret = __corerpc_send_sock(core, name, &ctx, __corerpc_post_task, SEND_TASK);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ret = __corerpc_wait__(name, op->rbuf, &ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

#if ENABLE_NETCTL_QUEUE

static void S_LTG __corerpc_post_queue(void *arg1, void *arg2, void *arg3,
                                       void *arg4)
{
        rpc_ctx_t *ctx = arg1;
        int retval = *(int *)arg2;
        ltgbuf_t *buf = arg3;
        corerpc_ring_ctx_t *ring = ctx->ctx;
        corerpc_op_t *op = &ctx->op;

        (void) arg4;

#if CORE_CHECK
        coreid_t coreid;
        int ret = core_getid(&coreid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        LTG_ASSERT(ctx->coreid == coreid.idx);
#endif
        
        ring->retval = retval;
        if (buf && buf->len) {
                LTG_ASSERT(op->rbuf);
                LTG_ASSERT(op->rbuf->len >= buf->len);
                ltgbuf_clone1(op->rbuf, buf, 0);
                ltgbuf_free(buf);
        }

        core_ring_reply(&ring->ring_ctx);
        slab_stream_free(ctx);
}

static void S_LTG __corerpc_queue_exec(void *_ring)
{
        int ret;
        corerpc_ring_ctx_t *ring = _ring;
        rpc_ctx_t *ctx = slab_stream_alloc(sizeof(*ctx));

        ctx->op = *ring->op;
        ctx->ctx = ring;

#if CORE_CHECK
        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ctx->coreid = coreid.idx;
#endif

        ret = __corerpc_send(ring->name, ctx, __corerpc_post_queue, SEND_QUEUE);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return;
err_ret:
        DWARN("send fail %p\n", ctx);
#if 1
        ring->retval = ret;
        core_ring_reply(&ring->ring_ctx);
        slab_stream_free(ctx);
#endif
        return;
}

static void S_LTG __corerpc_queue_reply(void *_ring)
{
        corerpc_ring_ctx_t *ring = _ring;

#if CORE_CHECK
        int ret;
        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        LTG_ASSERT(ring->coreid == coreid.idx);
#endif

        sche_task_post(&ring->task, ring->retval, NULL);
}

static int S_LTG __corerpc_ring_wait(int netctl, const char *name,
                                     corerpc_op_t *op)
{
        int ret;
        corerpc_ring_ctx_t ring;

        ring.op = op;
        ring.name = name;

#if CORE_CHECK
        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ring.coreid = coreid.idx;
#endif

        ring.task = sche_task_get();

#if 0
        if (ring.rbuf) {
                LTG_ASSERT((int)ring.rbuf->len == op->replen);
        }
#endif
        
        core_ring_queue(netctl, RING_QUEUE, &ring.ring_ctx,
                        __corerpc_queue_exec, &ring,
                        __corerpc_queue_reply, &ring);

        ret = sche_yield(name, op->rbuf, NULL);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

#else

STATIC int S_LTG __corerpc_postwait_task(va_list ap)
{
        const char *name = va_arg(ap, const char *);
        corerpc_op_t *op = va_arg(ap, corerpc_op_t *);

        va_end(ap);

        DBUG("%s redirect to netctl\n", name);
        
        return __corerpc_postwait(name, op);
}

#endif

int S_LTG corerpc_postwait(const char *name, const coreid_t *coreid,
                           const void *request, int reqlen, int replen,
                           const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                           int msg_type, int msg_size, int group, int timeout)
{
        coreid_t netctl;
        corerpc_op_t op;

        op.coreid = *coreid;
        op.request = request;
        op.reqlen = reqlen;
        op.replen = replen;
        op.wbuf = wbuf;
        op.rbuf = rbuf;
        op.group = group;
        op.msg_type = msg_type;
        op.msg_size = msg_size;
        op.timeout = timeout;
        
        if (likely(netctl_get(coreid, &netctl))) {
                op.netctl = netctl;

                DBUG("%s redirect to netctl\n", name);
#if ENABLE_NETCTL_QUEUE
                return __corerpc_ring_wait(netctl.idx, name, &op);
#else
                return core_ring_wait(netctl.idx, RING_TASK, name,
                                      __corerpc_postwait_task, name, &op);
#endif
        } else {
                op.netctl = *coreid;

                if (ltgconf_global.daemon && corerpc_inited) {
                        return __corerpc_postwait(name, &op);
                } else {
                        return stdrpc_request_wait3(name, coreid, request, reqlen, replen,
                                                    wbuf, rbuf, msg_type, -1, timeout);
                }
        }
}

int S_LTG corerpc_postwait1(const char *name, const coreid_t *coreid,
                            const void *request, int reqlen, void *reply,
                            int *_replen, int msg_type, int group, int timeout)
{
        int ret, replen;
        ltgbuf_t *rbuf, tmp;

        ANALYSIS_BEGIN(0);
        
        if (reply) {
                replen = *_replen;
                ltgbuf_init(&tmp, replen);
                rbuf = &tmp;
        } else {
                rbuf = NULL;
                replen = 0;
        }

        ret = corerpc_postwait(name, coreid, request, reqlen, replen, NULL,
                               rbuf, msg_type, replen, group, timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        if (rbuf) {
                LTG_ASSERT(*_replen >= (int)rbuf->len);
                ltgbuf_popmsg(rbuf, reply, rbuf->len);
                ltgbuf_free(rbuf);
        }

        ANALYSIS_END(0, IO_INFO, name);
        
        return 0;
err_ret:
        if (rbuf) {
                ltgbuf_free(rbuf);
        }
        ANALYSIS_END(0, IO_INFO, name);
        return ret;
}
