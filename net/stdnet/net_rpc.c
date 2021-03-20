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

typedef enum {
        NET_RPC_NULL = 0,
        NET_RPC_HELLO1,
        NET_RPC_HELLO2,
        NET_RPC_HEARTBEAT,
        NET_RPC_COREADDR,
        NET_RPC_CORES,
        NET_RPC_MAX,
} net_rpc_op_t;

typedef struct {
        uint32_t op;
        uint32_t buflen;
        char buf[0];
} msg_t;


static request_handler_func  __request_handler__[NET_RPC_MAX - NET_RPC_NULL];
static char  __request_name__[NET_RPC_MAX - NET_RPC_NULL][__RPC_HANDLER_NAME__ ];

static void __request_get_handler(int op, request_handler_func *func, const char **name)
{
        *func = __request_handler__[op - NET_RPC_NULL];
        *name = __request_name__[op - NET_RPC_NULL];
}

static void __request_set_handler(int op, request_handler_func func, const char *name)
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

static int __net_srv_hello1(ltgbuf_t *_buf, ltgbuf_t *out, int *outlen,
                            uint64_t *status)
{
        int buflen;
        msg_t *req;
        char buf[MAX_BUF_LEN];
        uint64_t *seq;

        (void) out;
        
        ANALYSIS_BEGIN(0);
        __getmsg(_buf, &req, &buflen, buf);

        _opaque_decode(req->buf, buflen,
                       &seq, NULL,
                       NULL);

        *outlen = 0;
        *status = 0;

        ANALYSIS_END(0, 1000 * 100, NULL);
        
        return 0;
}

static int __net_srv_hello2(ltgbuf_t *_buf, ltgbuf_t *out, int *outlen, uint64_t *status)
{
        int buflen;
        msg_t *req;
        char buf[MAX_BUF_LEN];
        uint64_t *seq;

        (void) out;
        
        ANALYSIS_BEGIN(0);
        __getmsg(_buf, &req, &buflen, buf);

        _opaque_decode(req->buf, buflen,
                       &seq, NULL,
                       NULL);

        *outlen = 0;
        *status = 0;

        ANALYSIS_END(0, 1000 * 100, NULL);
        
        return 0;
}

int net_rpc_hello1(const nid_t *nid, const sockid_t *sockid, uint64_t seq)
{
        int ret;
        char buf[MAX_BUF_LEN];
        uint32_t count;
        msg_t *req;

        ANALYSIS_BEGIN(0);

        req = (void *)buf;
        req->op = NET_RPC_HELLO1;
        _opaque_encode(req->buf, &count, &seq, sizeof(seq), NULL);

        ret = stdrpc_request_wait_sock("hello1", nid, sockid,
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
                                    req, sizeof(*req) + count, MSG_NET, -1,
                                    ltgconf_global.hb_timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("corenet hello success\n");
        
        ANALYSIS_END(0, 1000 * 500, NULL);

        return 0;
err_ret:
        return ret;
}

static void __request_get_handler__(const ltgbuf_t *buf, request_handler_func *func,
                                    const char **name)
{
        msg_t req;
        
        if (unlikely(buf->len < sizeof(req))) {
                *func = NULL;
                return;
        }

        ltgbuf_get(buf, &req, sizeof(req));

        __request_get_handler(req.op, func, name);
        if (unlikely(func == NULL)) {
                DWARN("error op %u\n", req.op);
                return;
        }
}

int net_rpc_init()
{
        __request_set_handler(NET_RPC_HELLO1, __net_srv_hello1, "net_srv_hello");
        __request_set_handler(NET_RPC_HELLO2, __net_srv_hello2, "net_srv_hello");

        corerpc_register(MSG_NET, __request_get_handler__, NULL);
        rpc_request_register(MSG_NET, __request_get_handler__, NULL);

        return 0;
}
