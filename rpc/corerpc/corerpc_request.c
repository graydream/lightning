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

typedef struct corerpc_op {
        coreid_t netctl;
        coreid_t coreid;
        const void *request;
        int msglen;

        int wbuflen;
        int rbuflen;

        void *wbuf;
        void *rbuf;

        int msg_type;
        int timeout;
        uint32_t group;
        sockid_t sockid;
        msgid_t msgid;
} corerpc_op_t;

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
                LTG_ASSERT(op->rbuflen >= (int)buf->len);
                ltgbuf_get(buf, op->rbuf, buf->len);
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

STATIC int S_LTG __corerpc_wait__(const char *name, rpc_ctx_t *ctx)
{
        int ret;

        DBUG("%s yield wait\n", name);
        ret = sche_yield(name, NULL, ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        DBUG("%s yield resume\n", name);

        return 0;
err_ret:
        return ret;
}

#if ENABLE_RDMA

#if 0
inline void INLINE __ltgbuf_trans_addr(void **addr, void *buf)
{
        int i = 0;

        addr[i++] = buf;

        LTG_ASSERT(i < MAX_SGE);
}
#endif


static void __corerpc_msgid_prep(msgid_t *msgid, const void *wbuf, int wlen,
                                 void *rbuf, int rlen, const rdma_conn_t *handler)
{

        if (rlen == 0 && wlen == 0)
                return;

        memset(&msgid->data_prop, 0x00, sizeof(data_prop_t));
	if (wbuf != NULL) {
		msgid->data_prop.rkey = handler->mr->rkey;
		//ltgbuf_trans_addr((void **)msgid->data_prop.remote_addr, (void *)wbuf);
                msgid->data_prop.remote_addr[0] = (uintptr_t)wbuf;
                
		msgid->data_prop.size = wlen;
	} else if (rbuf != NULL){
                //LTG_ASSERT((int)rbuf->len == msg_size);

		msgid->data_prop.rkey = handler->mr->rkey;
		//__ltgbuf_trans_addr((void **)msgid->data_prop.remote_addr, rbuf);
                msgid->data_prop.remote_addr[0] = (uintptr_t)rbuf;
		msgid->data_prop.size = rlen;
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
        
        __corerpc_msgid_prep(&op->msgid, op->wbuf, op->wbuflen, op->rbuf,
                             op->rbuflen, handler);

        ret = rpc_request_prep(&buf, &op->msgid, op->request, op->msglen,
                               op->rbuflen, op->rbuflen, op->msg_type, op->group,
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

static int __corenet_tcp_free(void *ptr)
{
        (void) ptr;

        return 0;
}

int corerpc_tcp_request(void *ctx, void *_op)
{
        int ret;
        ltgbuf_t buf;
        corerpc_op_t *op = _op;

        ret = rpc_request_prep(&buf, &op->msgid, op->request, op->msglen,
                               op->rbuflen, op->wbuflen, op->msg_type, op->group,
                               op->coreid.idx);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (op->wbuflen) {
                ltgbuf_t tmp;
                LTG_ASSERT(op->wbuf);
                ltgbuf_initwith(&tmp, op->wbuf, op->wbuflen, NULL, __corenet_tcp_free);
                ltgbuf_merge(&buf, &tmp);
        }

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
                __corerpc_request_reset(&op->msgid, ret);
                if (sche_running()) {
                        sche_task_reset();
#if 0
                        corenet_maping_close(&op->netctl.nid, &op->sockid);
#endif
                }

                if (type == SEND_QUEUE) {
                        goto out;
                } else {
                        ret = _errno_net(ret);
                        LTG_ASSERT(ret == ENONET || ret == ESHUTDOWN);
                        GOTO(err_ret, ret);
                }
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

        ret = __corerpc_wait__(name, &ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }
        
        return 0;
err_ret:
        return ret;
}

int corerpc_postwait_sock(const char *name, const coreid_t *coreid,
                          const sockid_t *sockid, const void *request,
                          int reqlen, int msg_type, int group, int timeout)
{
        int ret;
        rpc_ctx_t ctx;
        corerpc_op_t *op = &ctx.op;

        op->coreid = *coreid;
        op->netctl = *coreid;
        op->request = request;
        op->msglen = reqlen;
        op->group = group;
        op->msg_type = msg_type;
        op->timeout = timeout;
        op->sockid = *sockid;
        
        op->wbuf = NULL;
        op->rbuf = NULL;
        op->wbuflen = 0;
        op->rbuflen = 0;

        LTG_ASSERT(ltgconf_global.daemon);
        core_t *core = core_self();
        ret = __corerpc_send_sock(core, name, &ctx, __corerpc_post_task, SEND_TASK);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ret = __corerpc_wait__(name, &ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

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
                LTG_ASSERT(op->rbuflen >= (int)buf->len);
                ltgbuf_get(buf, op->rbuf, buf->len);
                ltgbuf_free(buf);
        }

        core_ring_reply(&ring->ring_ctx);
        slab_stream_free(ctx);
}

inline static void INLINE __corerpc_queue_exec(void *_ring)
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
        DWARN("send to %s fail %p\n", netable_rname(&ctx->op.coreid.nid), ctx);
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

static int S_LTG __corerpc_ring_queue_wait(int netctl, const char *name,
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

        ret = sche_yield(name, NULL, NULL);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

STATIC int S_LTG __corerpc_postwait_task(va_list ap)
{
        const char *name = va_arg(ap, const char *);
        corerpc_op_t *op = va_arg(ap, corerpc_op_t *);

        va_end(ap);

        DBUG("%s redirect to netctl\n", name);

        return __corerpc_postwait(name, op);
}

static int S_LTG __corerpc_ring_task_wait(int netctl, const char *name,
                                          corerpc_op_t *op)
{
        int ret;

        ret = core_ring_wait(netctl, RING_TASK, name,
                             __corerpc_postwait_task, name, op);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

#define MEM_MALLOC 0

inline static void INLINE __corerpc_trans_addr(const ltgbuf_t *buf,
                                               mem_handler_t *handler, void **addr)
{
        int ret;
        
        if (buf == NULL) {
                *addr = NULL;
                return;
        }

        if (likely(ltgbuf_segcount(buf) == 1)) {
                *addr = ltgbuf_head(buf);
        } else {
#if MEM_MALLOC
                (void) handler;
                void *tmp;
                ret = ltg_malloc((void **)&tmp, buf->len);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                *addr = tmp;
#else
                uint32_t len = buf->len;
                ret = mem_ring_new(&len, handler);
                if (unlikely(ret)) {
                        UNIMPLEMENTED(__DUMP__);
                }

                LTG_ASSERT(len == buf->len);
                *addr = handler->ptr;
#endif
        }
}

inline static void INLINE __corerpc_trans_free(const ltgbuf_t *buf,
                                               mem_handler_t *handler, void *addr)
{
        if (buf == NULL) {
                return;
        }

        if (likely(addr == ltgbuf_head(buf))) {
                return;
        }

#if MEM_MALLOC
        (void) handler;
        ltg_free((void **)&addr);
#else
        LTG_ASSERT(addr == handler->ptr);
        mem_ring_deref(handler);
#endif
}

inline int INLINE corerpc_postwait(const char *name, const coreid_t *coreid,
                                   const void *request, int reqlen, int replen,
                                   const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                                   int msg_type, int msg_size, int group, int timeout)
{
        int ret;
        coreid_t netctl;
        corerpc_op_t op;
        mem_handler_t whandler, rhandler;

        LTG_ASSERT(rbuf == NULL || wbuf == NULL);
        
        if (unlikely(!ltgconf_global.daemon || !corerpc_inited)) {
                return stdrpc_request_wait3(name, coreid, request, reqlen, replen,
                                            wbuf, rbuf, msg_type, -1, timeout);
        }

        (void) msg_size;

        op.coreid = *coreid;
        op.request = request;
        op.msglen = reqlen;
        op.msg_type = msg_type;
        op.timeout = timeout;
        op.group = group;

        op.wbuflen = wbuf ? wbuf->len : 0;
        op.rbuflen = rbuf ? rbuf->len : 0;

        LTG_ASSERT(replen == op.rbuflen);
        
        __corerpc_trans_addr(wbuf, &whandler, &op.wbuf);
        __corerpc_trans_addr(rbuf, &rhandler, &op.rbuf);

        if (unlikely(op.wbuf && ltgbuf_head(wbuf) != op.wbuf)) {
                ltgbuf_get(wbuf, op.wbuf, wbuf->len);
        }

        if (likely(netctl_get(coreid, &netctl))) {
                op.netctl = netctl;
                ret =  __corerpc_ring_queue_wait(netctl.idx, name, &op);
                if (unlikely(ret)) {
                        if (ret == ENOSYS) {
                                DINFO("retry connect\n");
                                ret =  __corerpc_ring_task_wait(netctl.idx,
                                                                name, &op);
                                if (unlikely(ret))
                                        GOTO(err_ret, ret);
                        } else {
                                GOTO(err_ret, ret);
                        }
                }
        } else {
                op.netctl = *coreid;
                ret = __corerpc_postwait(name, &op);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        if (unlikely(op.rbuf && ltgbuf_head(rbuf) != op.rbuf)) {
                ltgbuf_copy3(rbuf, op.rbuf, rbuf->len);
        }
        
        __corerpc_trans_free(wbuf, &whandler, op.wbuf);
        __corerpc_trans_free(rbuf, &rhandler, op.rbuf);

        return 0;
err_ret:
        DINFO("%p %p\n", op.wbuf, op.rbuf);
        __corerpc_trans_free(wbuf, &whandler, op.wbuf);
        __corerpc_trans_free(rbuf, &rhandler, op.rbuf);
        return ret;
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
