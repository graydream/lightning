#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

void IO_FUNC corerpc_reply_rdma(void *ctx, void *arg)
{
        int ret;
        ltgbuf_t reply_buf;
        sockop_reply_t *reply = arg;
        const msgid_t *msgid = reply->msgid;

        (void) ctx;

        if (likely(reply->err == 0)) {
                stdrpc_reply_init_prep(msgid, &reply_buf, reply->buf, 0);

                ret = corenet_rdma_send(reply->sockid, &reply_buf,
                                        (void **)msgid->data_prop.remote_addr,
                                        msgid->data_prop.rkey, 0, build_rdma_write_req);
                if (unlikely(ret)) {
                        DERROR("corenet rdma post send reply fail ret:%d\n", ret);
                        ltgbuf_free(&reply_buf);
                }
        } else {
                ltgbuf_t buf;
                stdrpc_reply_error_prep(msgid, &buf, reply->err);
                ret = corenet_rdma_send(reply->sockid, &buf, NULL, 0, 0, build_post_send_req);
                if (unlikely(ret)) {
                        ltgbuf_free(&buf);
                }
        }
}

void IO_FUNC corerpc_reply_tcp(void *ctx, void *arg)
{
        int ret;
        ltgbuf_t reply_buf;
        sockop_reply_t *reply = arg;
        const msgid_t *msgid = reply->msgid;

        (void) ctx;

        if (likely(reply->err == 0)) {
                stdrpc_reply_init_prep(msgid, &reply_buf, reply->buf, 1);
                
                ret = corenet_tcp_send(NULL, reply->sockid, &reply_buf);
                if (unlikely(ret))
                        ltgbuf_free(&reply_buf);
        } else {
                ltgbuf_t buf;
                stdrpc_reply_error_prep(msgid, &buf, reply->err);
                ret = corenet_tcp_send(NULL, reply->sockid, &buf);
                if (unlikely(ret))
                        ltgbuf_free(&buf);
        }
}

void IO_FUNC corerpc_reply_buffer(const sockid_t *sockid, const msgid_t *msgid, ltgbuf_t *buf)
{

        sockop_reply_t reply;

        reply.err = 0;
        reply.msgid = msgid;
        reply.buf = buf;
        reply.sockid = sockid;

        sockid->reply(NULL, &reply);
}

void IO_FUNC corerpc_reply(const sockid_t *sockid, const msgid_t *msgid,
                                    const void *_buf, int len)
{
        ltgbuf_t buf;

        ltgbuf_init(&buf, 0);
        if (unlikely(len))
                ltgbuf_copy(&buf, _buf, len);

        corerpc_reply_buffer(sockid, msgid, &buf);
}

void corerpc_reply_error(const sockid_t *sockid, const msgid_t *msgid, int _error)
{
        sockop_reply_t reply;

        reply.err = _error;
        reply.msgid = msgid;
        reply.buf = NULL;
        reply.sockid = sockid;

        sockid->reply(NULL, &reply);
}
