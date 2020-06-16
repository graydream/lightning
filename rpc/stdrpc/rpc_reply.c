#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_net.h"
#include "ltg_core.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"

void stdrpc_reply1(const sockid_t *sockid, const msgid_t *msgid, ltgbuf_t *_buf)
{
        int ret;
        ltgbuf_t buf;
        net_handle_t nh;

        DBUG("reply msgid (%d, %x) %s, len %u\n", msgid->idx, msgid->figerprint,
              _inet_ntoa(sockid->addr), _buf->len);

        stdrpc_reply_init_prep(msgid, &buf, _buf, 0, 1);

        sock2nh(&nh, sockid);
        ret = sdevent_queue(&nh, &buf);
        if (unlikely(ret)) {
                ret = _errno_net(ret);
                GOTO(err_free, ret);
        }

        ltgbuf_free(&buf);
        
        return;
err_free:
        ltgbuf_free(&buf);
        return;
}

void stdrpc_reply(const sockid_t *sockid, const msgid_t *msgid, const void *_buf, int len)
{
        ltgbuf_t buf;

        ltgbuf_init(&buf, 0);
        if (len)
                ltgbuf_copy(&buf, _buf, len);

        stdrpc_reply1(sockid, msgid, &buf);
}

void stdrpc_reply_tcp(void *ctx, void *arg)
{
        sockop_reply_t *reply = arg;
        const msgid_t *msgid = reply->msgid;

        (void) ctx;

        if (likely(reply->err == 0)) {
                stdrpc_reply1(reply->sockid, msgid, reply->buf);
        } else {
                stdrpc_reply_error(reply->sockid, msgid, reply->err);
        }
}
