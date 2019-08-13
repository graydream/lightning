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
        int retval;
        uint64_t latency;
        ltgbuf_t buf;
} rpc_ctx_t;

static int __rpc_request_send(const sockid_t *sockid, int coreid,
                              const msgid_t *msgid, const void *request,
                              int reqlen, const ltgbuf_t *data,
                              int msg_type, int priority)
{
        int ret;
        ltgbuf_t buf;
        net_handle_t nh;

        ret = rpc_request_prep(&buf, msgid, request, reqlen, data,
                               msg_type, 1, priority);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ltg_net_head_t *net_req = ltgbuf_head(&buf);
        net_req->coreid = coreid;
        
        DBUG("send msg to %s, id (%u, %x), len %u\n",
              _inet_ntoa(sockid->addr), msgid->idx,
              msgid->figerprint, buf.len);

        sock2nh(&nh, sockid);
        ret = sdevent_queue(&nh, &buf);
        if (unlikely(ret)) {
                ret = _errno_net(ret);
                GOTO(err_free, ret);
        }

        ltgbuf_free(&buf);

        return 0;
err_free:
        ltgbuf_free(&buf);
err_ret:
        return ret;
}

static void __rpc_request_post_sem(void *arg1, void *arg2, void *arg3, void *arg4)
{
        rpc_ctx_t *ctx = arg1;
        int retval = *(int *)arg2;
        ltgbuf_t *buf = arg3;
        uint64_t latency = *(uint64_t *)arg4;

        ctx->latency = latency;
        ctx->retval = retval;
        if (buf) {
                ltgbuf_merge(&ctx->buf, buf);
        } else {
                ctx->buf.len = 0;
        }

        sem_post(&ctx->sem);
}

static void __rpc_request_post_task(void *arg1, void *arg2, void *arg3, void *arg4)
{
        rpc_ctx_t *ctx = arg1;
        int retval = *(int *)arg2;
        ltgbuf_t *buf = arg3;
        uint64_t latency = *(uint64_t *)arg4;

        ctx->latency = latency;
        ctx->retval = -1;

        if (buf) {
                ltgbuf_merge(&ctx->buf, buf);
        } else {
                ctx->buf.len = 0;
        }
        
        sche_task_post(&ctx->task, retval, NULL);
}

static void __rpc_request_reset(const msgid_t *msgid)
{
        if (sche_running()) {
                sche_task_reset();
        }
        
        rpc_table_free(__rpc_table__, msgid);
}

static int __rpc_request_getslot(msgid_t *msgid, rpc_ctx_t *ctx, const char *name,
                                 const sockid_t *sockid, const nid_t *nid, int timeout)
{
        int ret;

        ret = rpc_table_getslot(__rpc_table__, msgid, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ltgbuf_init(&ctx->buf, 0);

        LTG_ASSERT(sockid->type == SOCKID_NORMAL);

        if (sche_running()) {
                ctx->task = sche_task_get();

                ret = rpc_table_setslot(__rpc_table__, msgid, __rpc_request_post_task,
                                        ctx, sockid, nid, timeout);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
        } else {
                ret = sem_init(&ctx->sem, 0, 0);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                ret = rpc_table_setslot(__rpc_table__, msgid, __rpc_request_post_sem,
                                        ctx, sockid, nid, timeout);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
        }

        return 0;
err_ret:
        return ret;
}

STATIC int __stdrpc_request_wait__(const net_handle_t *nh, const char *name, ltgbuf_t *rbuf,
                                rpc_ctx_t *ctx, int timeout)
{
        int ret;

        (void) timeout;
        
        ANALYSIS_BEGIN(0);
        
        if (sche_running()) {
                DBUG("%s yield wait\n", name);
                ret = sche_yield(name, NULL, ctx);
                if (unlikely(ret))
                        goto err_ret;
                
                DBUG("%s yield resume\n", name);
        } else {
                DBUG("%s sem wait\n", name);
                ret = _sem_wait(&ctx->sem);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }

                ret = ctx->retval;
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                DBUG("%s sem resume\n", name);
        }

        if (rbuf) {
                ltgbuf_clone1(rbuf, &ctx->buf, 0);
                ltgbuf_free(&ctx->buf);
        } else {
                LTG_ASSERT(ctx->buf.len == 0);
        }
                
        
        if (nh->type == NET_HANDLE_PERSISTENT) {
                DBUG("%s latency %llu\n", netable_rname_nid(&nh->u.nid), (LLU)ctx->latency);
                netable_load_update(&nh->u.nid, ctx->latency);
        }

#ifdef RPC_ASSERT
        timeout = _max(timeout, ltgconf.rpc_timeout);
        ANALYSIS_ASSERT(0, 1000 * 1000 * (timeout * 3), name);
#else
        ANALYSIS_END(0, IO_INFO, NULL);
#endif

        return 0;
err_ret:
#ifdef RPC_ASSERT
        timeout = _max(timeout, ltgconf.rpc_timeout);
        ANALYSIS_ASSERT(0, 1000 * 1000 * (timeout * 3), name);
#else
        ANALYSIS_END(0, IO_INFO, NULL);
#endif
        return ret;
}

STATIC int __stdrpc_request_wait(const char *name, const net_handle_t *nh,
                              int coreid, const void *request,
                              int reqlen, const ltgbuf_t *wbuf,
                              ltgbuf_t *rbuf, int msg_type,
                              int priority, int timeout)
{
        int ret;
        msgid_t msgid;
        rpc_ctx_t ctx;
        sockid_t sockid;
        const nid_t *nid;

        if (nh->type == NET_HANDLE_PERSISTENT) {
                ret = netable_getsock(&nh->u.nid, &sockid);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                nid = &nh->u.nid;
        } else {
                sockid = nh->u.sd;
                nid = NULL;
        }

        ANALYSIS_BEGIN(0);
        ret = __rpc_request_getslot(&msgid, &ctx, name, &sockid, nid, timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __rpc_request_send(&sockid, coreid, &msgid, request, reqlen,
                                 wbuf, msg_type, priority);
        if (unlikely(ret)) {
                ret = _errno_net(ret);
                LTG_ASSERT(ret == ENONET || ret == ESHUTDOWN || ret == ESTALE || ret == ENOSYS);
                GOTO(err_free, ret);
        }

        DBUG("%s msgid (%u, %x) to %s\n", name, msgid.idx, msgid.figerprint,
             _inet_ntoa(sockid.addr));

        ANALYSIS_END(0, IO_INFO, NULL);
        
        ret = __stdrpc_request_wait__(nh, name, rbuf, &ctx, timeout);
        if (unlikely(ret)) {
                goto err_ret;
        }

        return 0;
err_free:
        __rpc_request_reset(&msgid);
err_ret:
        return ret;
}

int stdrpc_request_wait(const char *name, const nid_t *nid, const void *request, int reqlen,
                     void *reply, int *replen, int msg_type,
                     int priority, int timeout)
{
        int ret;
        ltgbuf_t *buf, tmp;
        net_handle_t nh;

        if (reply) {
                ltgbuf_init(&tmp, *replen);
                buf = &tmp;
        } else {
                buf = NULL;
        }

        id2nh(&nh, nid);

        ret = __stdrpc_request_wait(name, &nh, -1, request, reqlen, NULL, buf,
                                 msg_type, priority, timeout);
        if (unlikely(ret))
                goto err_ret;

        if (buf) {
                LTG_ASSERT(buf->len);
                if (replen) {
                        LTG_ASSERT((int)buf->len <= *replen);
                        *replen = buf->len;
                }

                ltgbuf_get(buf, reply, buf->len);
                ltgbuf_free(buf);
        }

        return 0;
err_ret:
        if (buf) {
                ltgbuf_free(buf);
        }
        return ret;
}

int stdrpc_request_wait3(const char *name, const coreid_t *coreid, const void *request,
                      int reqlen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf, int msg_type,
                      int priority, int timeout)
{
        int ret;
        net_handle_t nh;

        id2nh(&nh, &coreid->nid);
        ret = __stdrpc_request_wait(name, &nh, coreid->idx, request, reqlen, wbuf, rbuf,
                                 msg_type, priority, timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int stdrpc_request_wait_sock(const char *name, const net_handle_t *nh, const void *request,
                          int reqlen, void *reply, int *replen, int msg_type,
                          int priority, int timeout)
{
        int ret;
        ltgbuf_t *buf, tmp;

        if (reply) {
                ltgbuf_init(&tmp, *replen);
                buf = &tmp;
        } else {
                buf = NULL;
        }

        ret = __stdrpc_request_wait(name, nh, -1, request, reqlen, NULL, buf,
                                 msg_type, priority, timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (reply) {
                if (replen) {
                        LTG_ASSERT((int)buf->len <= *replen);
                        *replen = buf->len;
                }

                ltgbuf_get(buf, reply, buf->len);
                ltgbuf_free(buf);
        } else {
                LTG_ASSERT(reply == NULL);
                LTG_ASSERT(replen == NULL);
        }

        return 0;
err_ret:
        return ret;
}
