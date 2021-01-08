#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

void S_LTG corerpc_reply_rdma(void *ctx, void *arg)
{
        int ret;
        ltgbuf_t reply_buf;
        sockop_reply_t *reply = arg;
        const msgid_t *msgid = reply->msgid;

        (void) ctx;

        if (likely(reply->err == 0)) {
                stdrpc_reply_init_prep(msgid, &reply_buf, 0);

                if (reply->buf) {
                        ltgbuf_merge(&reply_buf, reply->buf);
                }
                
                ret = corenet_rdma_send(reply->sockid, &reply_buf,
                                        (void **)msgid->data_prop.remote_addr,
                                        msgid->data_prop.rkey, 0,
                                        build_rdma_write_req);
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

void corerpc_reply_tcp(void *ctx, void *arg)
{
        int ret;
        ltgbuf_t reply_buf;
        sockop_reply_t *reply = arg;
        const msgid_t *msgid = reply->msgid;

        (void) ctx;

        if (likely(reply->err == 0)) {
                stdrpc_reply_init_prep(msgid, &reply_buf,
                                       reply->buf ? reply->buf->len : 0);

                if (reply->buf ? reply->buf->len : 0) {
                        ltgbuf_merge(&reply_buf, reply->buf);
                }
                
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

void S_LTG corerpc_reply_buffer(const sockid_t *sockid, const msgid_t *msgid, ltgbuf_t *buf)
{

        sockop_reply_t reply;

        reply.err = 0;
        reply.latency = 0;
        reply.msgid = msgid;
        reply.buf = buf;
        reply.sockid = sockid;

        sockid->reply(NULL, &reply);
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
