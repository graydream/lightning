#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

typedef struct {
        uint32_t op;
        uint32_t buflen;
        char buf[0];
} msg_t;

typedef enum {
        NET_RPC_NULL = 0,
        NET_RPC_HELLO1,
        NET_RPC_HELLO2,
        NET_RPC_HEARTBEAT,
        NET_RPC_COREADDR,
        NET_RPC_CORES,
        NET_RPC_MAX,
} net_rpc_op_t;

static __request_handler_func__  __request_handler__[NET_RPC_MAX - NET_RPC_NULL];
static char  __request_name__[NET_RPC_MAX - NET_RPC_NULL][__RPC_HANDLER_NAME__ ];

static void __request_get_handler(int op, __request_handler_func__ *func, char *name)
{
        *func = __request_handler__[op - NET_RPC_NULL];
        strcpy(name, __request_name__[op - NET_RPC_NULL]);
}

static void __request_set_handler(int op, __request_handler_func__ func, const char *name)
{
        LTG_ASSERT(strlen(name) + 1 < __RPC_HANDLER_NAME__ );
        strcpy(__request_name__[op - NET_RPC_NULL], name);
        __request_handler__[op - NET_RPC_NULL] = func;
}

static void __getmsg(ltgbuf_t *buf, msg_t **_req, int *buflen, char *_buf)
{
        msg_t *req;

        LTG_ASSERT(buf->len <= PAGE_SIZE);

        req = (void *)_buf;
        *buflen = buf->len - sizeof(*req);
        ltgbuf_get(buf, req, buf->len);

        *_req = req;
}

static int __net_srv_hello1(const sockid_t *sockid, const msgid_t *msgid,
                            ltgbuf_t *_buf, ltgbuf_t *out, int *outlen)
{
        int buflen;
        msg_t *req;
        char buf[MAX_BUF_LEN];
        uint64_t *seq;

        (void) sockid;
        (void) msgid;
        (void) out;
        
        ANALYSIS_BEGIN(0);
        __getmsg(_buf, &req, &buflen, buf);

        DBUG("hello id (%u, %x)\n", msgid->idx, msgid->figerprint);

        _opaque_decode(req->buf, buflen,
                       &seq, NULL,
                       NULL);

        *outlen = 0;

        ANALYSIS_END(0, 1000 * 100, NULL);
        
        return 0;
}

static int __net_srv_hello2(const sockid_t *sockid, const msgid_t *msgid,
                            ltgbuf_t *_buf, ltgbuf_t *out, int *outlen)
{
        int buflen;
        msg_t *req;
        char buf[MAX_BUF_LEN];
        uint64_t *seq;

        (void) sockid;
        (void) msgid;
        (void) out;
        
        ANALYSIS_BEGIN(0);
        __getmsg(_buf, &req, &buflen, buf);

        DBUG("hello id (%u, %x)\n", msgid->idx, msgid->figerprint);

        _opaque_decode(req->buf, buflen,
                       &seq, NULL,
                       NULL);

        *outlen = 0;

        ANALYSIS_END(0, 1000 * 100, NULL);
        
        return 0;
}

int net_rpc_hello1(const sockid_t *sockid, uint64_t seq)
{
        int ret;
        char buf[MAX_BUF_LEN];
        uint32_t count;
        msg_t *req;
        net_handle_t nh;

        ANALYSIS_BEGIN(0);

        req = (void *)buf;
        req->op = NET_RPC_HELLO1;
        _opaque_encode(req->buf, &count, &seq, sizeof(seq), NULL);

        sock2nh(&nh, sockid);
        ret = stdrpc_request_wait_sock("hello1", &nh,
                                       req, sizeof(*req) + count,
                                       NULL, NULL,
                                       MSG_NET, 0, ltgconf_global.hb_timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("corenet hello success\n");

        ANALYSIS_END(0, 1000 * 500, NULL);

        return 0;
err_ret:
        return ret;
}

int net_rpc_hello2(const coreid_t *coreid, const sockid_t *sockid, uint64_t seq)
{
        int ret;
        char buf[MAX_BUF_LEN];
        uint32_t count;
        msg_t *req;

        ANALYSIS_BEGIN(0);

        req = (void *)buf;
        req->op = NET_RPC_HELLO2;
        _opaque_encode(req->buf, &count, &seq, sizeof(seq), NULL);

        ret = corerpc_postwait_sock("hello2", coreid, sockid,
                                    req, sizeof(*req) + count, NULL,
                                    NULL, MSG_NET, -1, -1,
                                    ltgconf_global.hb_timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("corenet hello success\n");
        
        ANALYSIS_END(0, 1000 * 500, NULL);

        return 0;
err_ret:
        return ret;
}


static void __request_handler(void *arg)
{
        int ret;
        msg_t req;
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;
        __request_handler_func__ handler;
        char name[MAX_NAME_LEN];

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL, NULL);

        if (buf.len < sizeof(req)) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        ltgbuf_get(&buf, &req, sizeof(req));

        DBUG("new job op %u\n", req.op);

        __request_get_handler(req.op, &handler, name);
        if (handler == NULL) {
                ret = ENOSYS;
                DWARN("error op %u\n", req.op);
                GOTO(err_ret, ret);
        }

        sche_task_setname(name);

        int outlen = 0;
        ret = handler(&sockid, &msgid, &buf, NULL, &outlen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ltgbuf_free(&buf);

        return ;
err_ret:
        ltgbuf_free(&buf);
        stdrpc_reply_error(&sockid, &msgid, ret);
        return;
}

int net_rpc_init()
{
        __request_set_handler(NET_RPC_HELLO1, __net_srv_hello1, "net_srv_hello");
        __request_set_handler(NET_RPC_HELLO2, __net_srv_hello2, "net_srv_hello");

        rpc_request_register(MSG_NET, __request_handler, NULL);
        corerpc_register(MSG_NET, __request_handler, NULL);

        return 0;
}
