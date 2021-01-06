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
                              int reqlen, int replen, const ltgbuf_t *data,
                              int msg_type, int priority)
{
        int ret;
        ltgbuf_t buf;
        net_handle_t nh;

        ret = rpc_request_prep(&buf, msgid, request, reqlen, replen,
                               data ? data->len : 0,
                               msg_type, priority, coreid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (data && data->len) {
                ltgbuf_t tmp;
                ltgbuf_init(&tmp, 0);
                ltgbuf_reference(&tmp, data);
                ltgbuf_merge(&buf, &tmp);
        }
        
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
        (void) msgid;
        
        if (sche_running()) {
                sche_task_reset();
        }

        rpc_table_free(__rpc_table__, msgid);
}

static void __rpc_table_close(void *arg1, void *arg2, void *arg3)
{
        const nid_t *nid = arg1;
        const sockid_t *sockid = arg2;

        (void) arg3;
        (void) nid;
        (void) sockid;

#if 0
        net_handle_t nh;
        sock2nh(&nh, sockid);
        sdevent_close(&nh);
#endif
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
                                        ctx, __rpc_table_close, nid, sockid, timeout);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
        } else {
                ret = sem_init(&ctx->sem, 0, 0);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                ret = rpc_table_setslot(__rpc_table__, msgid, __rpc_request_post_sem,
                                        ctx, __rpc_table_close, nid, sockid, timeout);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
        }

        return 0;
err_ret:
        return ret;
}

STATIC int __stdrpc_request_wait__(const char *name, ltgbuf_t *rbuf,
                                   rpc_ctx_t *ctx)
{
        int ret;

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
                
        return 0;
err_ret:
        return ret;
}

STATIC int __stdrpc_request_wait(const char *name,
                                 const nid_t *nid, const sockid_t *_sockid,
                                 int coreid, const void *request,
                                 int reqlen, int replen, const ltgbuf_t *wbuf,
                                 ltgbuf_t *rbuf, int msg_type,
                                 int priority, int timeout)
{
        int ret;
        msgid_t msgid;
        rpc_ctx_t ctx;
        sockid_t sockid;

        if (!netable_connected(nid)) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }

        if (_sockid) {
                sockid = *_sockid;
        } else {
                ret = netable_getsock(nid, &sockid);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        ANALYSIS_BEGIN(0);
        ret = __rpc_request_getslot(&msgid, &ctx, name, &sockid, nid, timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __rpc_request_send(&sockid, coreid, &msgid, request, reqlen, replen,
                                 wbuf, msg_type, priority);
        if (unlikely(ret)) {
                ret = _errno_net(ret);
                LTG_ASSERT(ret == ENONET || ret == ESHUTDOWN || ret == ESTALE || ret == ENOSYS);
                GOTO(err_free, ret);
        }

        DBUG("%s msgid (%u, %x) to %s\n", name, msgid.idx, msgid.figerprint,
             _inet_ntoa(sockid.addr));

        ANALYSIS_END(0, IO_INFO, NULL);
        
        ret = __stdrpc_request_wait__(name, rbuf, &ctx);
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
                     void *reply, int *_replen, int msg_type,
                     int priority, int timeout)
{
        int ret, replen;
        ltgbuf_t *buf, tmp;

        if (reply) {
                ltgbuf_init(&tmp, *_replen);
                buf = &tmp;
                replen = *_replen;
        } else {
                buf = NULL;
                replen = 0;
        }

        ret = __stdrpc_request_wait(name, nid, NULL, -1, request, reqlen, replen, NULL, buf,
                                    msg_type, priority, timeout);
        if (unlikely(ret))
                goto err_ret;

        if (buf) {
                LTG_ASSERT(buf->len);
                if (_replen) {
                        LTG_ASSERT((int)buf->len <= *_replen);
                        *_replen = buf->len;
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
                         int reqlen, int replen, const ltgbuf_t *wbuf,
                         ltgbuf_t *rbuf, int msg_type,
                         int priority, int timeout)
{
        int ret;

        ret = __stdrpc_request_wait(name, &coreid->nid, NULL, coreid->idx,
                                    request, reqlen, replen, wbuf, rbuf,
                                    msg_type, priority, timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int stdrpc_request_wait_sock(const char *name, const nid_t *nid,
                             const sockid_t *sockid, const void *request,
                             int reqlen, void *reply, int *replen, int msg_type,
                             int priority, int timeout)
{
        int ret, _replen;
        ltgbuf_t *buf, tmp;

        if (reply) {
                ltgbuf_init(&tmp, *replen);
                buf = &tmp;
                _replen = *replen;
        } else {
                buf = NULL;
                _replen = 0;
        }

        ret = __stdrpc_request_wait(name, nid, sockid, -1, request, reqlen, _replen,
                                    NULL, buf, msg_type, priority, timeout);
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

int stdrpc_request_wait2(const char *name, const coreid_t *coreid,
                         const void *request, int reqlen,
                         void *reply, int *replen, int msg_type, int timeout)
{
        int ret, _replen;
        ltgbuf_t *buf, tmp;

        if (reply) {
                ltgbuf_init(&tmp, *replen);
                buf = &tmp;
                _replen = *replen;
        } else {
                buf = NULL;
                _replen = 0;
        }

        ret = __stdrpc_request_wait(name, &coreid->nid, NULL, coreid->idx,
                                    request, reqlen, _replen, NULL, buf, msg_type, -1,
                                    timeout);
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
