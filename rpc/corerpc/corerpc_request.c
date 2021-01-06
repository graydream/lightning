#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

typedef struct {
        sem_t sem;
        task_t task;
        uint64_t latency;
} rpc_ctx_t;

static void __corerpc_close(void *arg1, void *arg2, void *arg3);
#if RPC_TABLE_POST_FREE
inline static void INLINE __corerpc_request_reg(void *ctx, const msgid_t *msgid,
                                                const ltgbuf_t *wbuf,
                                                const ltgbuf_t *rbuf);
#endif

extern rpc_table_t *corerpc_self_byctx(void *);
extern rpc_table_t *corerpc_self();
extern int corerpc_inited;

static void __corerpc_post_task(void *arg1, void *arg2, void *arg3, void *arg4)
{
        rpc_ctx_t *ctx = arg1;
        int retval = *(int *)arg2;
        ltgbuf_t *buf = arg3;
        uint64_t latency = *(uint64_t *)arg4;

        ctx->latency = latency;

        sche_task_post(&ctx->task, retval, buf);
}

static void __corerpc_request_reset(const msgid_t *msgid)
{
        (void) msgid;
        sche_task_reset();
#if RPC_TABLE_POST_FREE
        rpc_table_t *__rpc_table_private__ = corerpc_self();
        rpc_table_free(__rpc_table_private__, msgid);
#endif
}

STATIC int __corerpc_getslot(void *_ctx, rpc_ctx_t *ctx, corerpc_op_t *op, const char *name)
{
        int ret;
        rpc_table_t *__rpc_table_private__ = corerpc_self_byctx(_ctx);

        ANALYSIS_BEGIN(0);

        ret = rpc_table_getslot(__rpc_table_private__, &op->msgid, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ctx->task = sche_task_get();

        ret = rpc_table_setslot(__rpc_table_private__, &op->msgid,
                                __corerpc_post_task, ctx, __corerpc_close,
                                &op->coreid.nid, &op->sockid, op->timeout);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ANALYSIS_QUEUE(0, IO_INFO, NULL);

        return 0;
err_ret:
        return ret;
}

STATIC int __corerpc_wait__(const char *name, ltgbuf_t *rbuf,
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

int corerpc_rdma_request(void *ctx, void *_op)
{
        int ret;
        ltgbuf_t buf;
        corerpc_op_t *op = _op;
        rdma_conn_t *handler = (rdma_conn_t *)op->sockid.rdma_handler;

        (void) ctx;
        
        __corerpc_msgid_prep(&op->msgid, op->wbuf, op->rbuf, op->msg_size, handler);

        ret = rpc_request_prep(&buf, &op->msgid, op->request, op->reqlen,
                               op->wbuf, op->msg_type, 0, op->group);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = corenet_rdma_send(&op->sockid, &buf, NULL, 0, 0, build_post_send_req);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

#if RPC_TABLE_POST_FREE
        __corerpc_request_reg(ctx, &op->msgid, op->wbuf, op->rbuf);
#endif
        
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
                               op->wbuf, op->msg_type, 1, op->group);
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

static int __corerpc_send_and_wait(void *core, const char *name, corerpc_op_t *op,
                                   uint64_t *latency)
{
        int ret;
        rpc_ctx_t rpc_ctx;

        ANALYSIS_BEGIN(0);

        DBUG("%s\n", name);

        ret = __corerpc_getslot(core, &rpc_ctx, op, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = op->sockid.request(core, op);
        if (unlikely(ret)) {
                sche_task_reset();
                corenet_maping_closeall(&op->coreid.nid, &op->sockid);
		ret = _errno_net(ret);
		LTG_ASSERT(ret == ENONET || ret == ESHUTDOWN);
		GOTO(err_free, ret);
	}

        DBUG("%s msgid (%u, %x) to %s\n", name, &op->msgid.idx,
             op->msgid.figerprint, _inet_ntoa(op->sockid.addr));

        SOCKID_DUMP(&op->sockid);
        MSGID_DUMP(&op->msgid);
        ret = __corerpc_wait__(name, op->rbuf, &rpc_ctx);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        *latency = rpc_ctx.latency;

        ANALYSIS_QUEUE(0, IO_INFO, NULL);

        return 0;

err_free:
        __corerpc_request_reset(&op->msgid);
err_ret:
        return ret;
}

int IO_FUNC __corerpc_postwait(const char *name, corerpc_op_t *op, uint64_t *latency)
{
        int ret;
        core_t *core = core_self();

        ANALYSIS_BEGIN(0);

        if (!netable_connected(&op->coreid.nid)) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }
        
        ret = corenet_maping(core, &op->coreid, &op->sockid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("send to %s/%d, sd %u\n", netable_rname(&op->coreid.nid),
             op->coreid.idx, op->sockid.sd);

        ret = __corerpc_send_and_wait(core, name, op, latency);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ANALYSIS_QUEUE(0, IO_INFO, NULL);

        return 0;
err_ret:
        return ret;
}

int IO_FUNC corerpc_postwait(const char *name, const coreid_t *coreid, const void *request,
                             int reqlen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                             int msg_type, int msg_size, int group, int timeout)
{
        int ret;
        corerpc_op_t op;
        uint64_t latency;

        op.coreid = *coreid;
        op.request = request;
        op.reqlen = reqlen;
        op.wbuf = wbuf;
        op.rbuf = rbuf;
        op.group = group;
        op.msg_type = msg_type;
        op.msg_size = msg_size;
        op.timeout = timeout;

        if (likely(ltgconf_global.daemon && corerpc_inited)) {
                ret = __corerpc_postwait(name, &op, &latency);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                ret = stdrpc_request_wait3(name, coreid, request, reqlen,
                                           wbuf, rbuf, msg_type, -1, timeout);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int IO_FUNC corerpc_postwait1(const char *name, const coreid_t *coreid,
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
                replen = -1;
        }

        ret = corerpc_postwait(name, coreid, request, reqlen,
                               NULL, rbuf, msg_type, replen, group, timeout);
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

int IO_FUNC corerpc_postwait2(const char *name, const coreid_t *coreid,
                              const void *request, int reqlen,
                              const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                              uint64_t *latency, int msg_type, int msg_size,
                              int group, int timeout)
{
        int ret;
        corerpc_op_t op;

        op.coreid = *coreid;
        op.request = request;
        op.reqlen = reqlen;
        op.wbuf = wbuf;
        op.rbuf = rbuf;
        op.group = group;
        op.msg_type = msg_type;
        op.msg_size = msg_size;
        op.timeout = timeout;

        if (likely(ltgconf_global.daemon && corerpc_inited)) {
                ret = __corerpc_postwait(name, &op, latency);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                ret = stdrpc_request_wait3(name, coreid, request, reqlen,
                                           wbuf, rbuf, msg_type, -1, timeout);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                *latency = 0;
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
        corerpc_op_t op;
        uint64_t latency;

        op.coreid = *coreid;
        op.request = request;
        op.reqlen = reqlen;
        op.wbuf = wbuf;
        op.rbuf = rbuf;
        op.group = group;
        op.msg_type = msg_type;
        op.msg_size = msg_size;
        op.timeout = timeout;
        op.sockid = *sockid;
        
        if (likely(ltgconf_global.daemon)) {
                core_t *core = core_self();
                ret = __corerpc_send_and_wait(core, name, &op, &latency);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        } else {
                UNIMPLEMENTED(__DUMP__);
        }

        return 0;
err_ret:
        return ret;
}

inline static void INLINE __corerpc_request_free(void *ctx)
{
        ltgbuf_t *buf = ctx;

        DBUG("free %p\n", ctx);
        
        ltgbuf_free(buf);
        slab_stream_free(buf);
}

#if RPC_TABLE_POST_FREE
inline static void INLINE __corerpc_request_reg(void *ctx, const msgid_t *msgid,
                                                const ltgbuf_t *wbuf,
                                                const ltgbuf_t *rbuf)
{
        int ret;
        const ltgbuf_t *iobuf = wbuf ? wbuf : rbuf;

        if (unlikely(iobuf == NULL)) {
                return;
        }

        LTG_ASSERT(wbuf == NULL || rbuf == NULL);
        ltgbuf_t *tmp = slab_stream_alloc(sizeof(*tmp));
        ltgbuf_init(tmp, 0);
        ltgbuf_reference(tmp, iobuf);

        rpc_table_t *rpc_table = corerpc_self_byctx(ctx);
        ret = rpc_table_setfree(rpc_table, msgid, __corerpc_request_free, tmp);
        if (unlikely(ret)) {
                ltgbuf_free(tmp);
                slab_stream_free(tmp);
        }
}
#endif

static void __corerpc_close(void *arg1, void *arg2, void *arg3)
{
        const nid_t *nid = arg1;
        const sockid_t *sockid = arg2;

        (void) arg3;
        (void) nid;
        (void) sockid;

        corenet_maping_close(nid, sockid);
}
