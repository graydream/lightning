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

static int __net_srv_heartbeat(const sockid_t *sockid, const msgid_t *msgid, ltgbuf_t *_buf)
{
        int buflen;
        msg_t *req;
        char buf[MAX_BUF_LEN];
        ltg_net_info_t *info;
        uint64_t *seq;

        ANALYSIS_BEGIN(0);
        __getmsg(_buf, &req, &buflen, buf);

        DBUG("heartbeat id (%u, %x)\n", msgid->idx, msgid->figerprint);

        _opaque_decode(req->buf, buflen,
                       &seq, NULL,
                       &info, NULL,
                       NULL);

        stdrpc_reply(sockid, msgid, NULL, 0);
        ANALYSIS_END(0, 1000 * 100, NULL);
        
        return 0;
}

int net_rpc_heartbeat(const sockid_t *sockid, uint64_t seq)
{
        int ret;
        char buf[MAX_BUF_LEN], info[MAX_BUF_LEN];
        uint32_t count, len;
        msg_t *req;
        net_handle_t nh;

        ANALYSIS_BEGIN(0);

        len = MAX_BUF_LEN;
        ret = rpc_getinfo(info, &len);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        req = (void *)buf;
        req->op = NET_RPC_HEARTBEAT;
        _opaque_encode(req->buf, &count, &seq, sizeof(seq), info, len, NULL);

#if 0
        DINFO("heartbeat to %s seq %ju\n", _inet_ntoa(sockid->addr), seq);
#endif
        sock2nh(&nh, sockid);
        ret = stdrpc_request_wait_sock("net_rpc_hb", &nh,
                                    req, sizeof(*req) + count,
                                    NULL, NULL,
                                    MSG_HEARTBEAT, 0, ltgconf.hb_timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

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

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL);

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

        ret = handler(&sockid, &msgid, &buf);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ltgbuf_free(&buf);

        return ;
err_ret:
        ltgbuf_free(&buf);
        stdrpc_reply_error(&sockid, &msgid, ret);
        return;
}

#if 1
static int __net_srv_corenetinfo(const sockid_t *sockid, const msgid_t *msgid,
                                 ltgbuf_t *_buf)
{
        int ret, buflen;
        msg_t *req;
        char *buf = slab_stream_alloc(PAGE_SIZE);
        const nid_t *nid;
        const coreid_t *coreid;
        char _addr[MAX_BUF_LEN];
        corenet_addr_t *addr;

        __getmsg(_buf, &req, &buflen, buf);

        _opaque_decode(req->buf, buflen,
                       &nid, NULL,
                       &coreid, NULL,
                       NULL);

        addr = (void *)_addr;
        ret = corenet_getaddr(coreid, addr);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        stdrpc_reply(sockid, msgid, addr, addr->len);

        slab_stream_free(buf);

        return 0;
err_ret:
        slab_stream_free(buf);
        return ret;
}

int net_rpc_coreinfo(const coreid_t *coreid, corenet_addr_t *addr)
{
        int ret;
        char *buf = slab_stream_alloc(PAGE_SIZE);
        uint32_t count;
        msg_t *req;
        const nid_t *nid = &coreid->nid;
        int buflen = MAX_BUF_LEN;

        ret = network_connect(nid, NULL, 1, 0);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ANALYSIS_BEGIN(0);

        req = (void *)buf;
        req->op = NET_RPC_COREADDR;
        _opaque_encode(&req->buf, &count,
                       net_getnid(), sizeof(nid_t),
                       coreid, sizeof(*coreid),
                       NULL);

        ret = stdrpc_request_wait("net_rpc_corenetinfo", nid,
                               req, sizeof(*req) + count, (void *)addr, &buflen,
                               MSG_HEARTBEAT, 0, ltgconf.rpc_timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ANALYSIS_QUEUE(0, IO_WARN, NULL);

        slab_stream_free(buf);

        return 0;
err_ret:
        slab_stream_free(buf);
        return ret;
}
#endif

#if 1
static int __net_srv_cores(const sockid_t *sockid, const msgid_t *msgid,
                           ltgbuf_t *_buf)
{
        int buflen;
        msg_t *req;
        char *buf = slab_stream_alloc(PAGE_SIZE);
        uint64_t mask;

        __getmsg(_buf, &req, &buflen, buf);

        mask = core_mask();

        stdrpc_reply(sockid, msgid, &mask, sizeof(mask));

        slab_stream_free(buf);

        return 0;
#if 0
err_ret:
        slab_stream_free(buf);
        return ret;
#endif
}

int net_rpc_coremask(const nid_t *nid, uint64_t *mask)
{
        int ret;
        char *buf = slab_stream_alloc(PAGE_SIZE);
        uint32_t count;
        msg_t *req;
        int buflen;

        ret = network_connect(nid, NULL, 1, 0);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ANALYSIS_BEGIN(0);

        req = (void *)buf;
        req->op = NET_RPC_CORES;
        _opaque_encode(&req->buf, &count,
                       net_getnid(), sizeof(nid_t),
                       NULL);

        buflen = sizeof(*mask);
        ret = stdrpc_request_wait("net_rpc_corenetinfo", nid,
                               req, sizeof(*req) + count, mask, &buflen,
                               MSG_HEARTBEAT, 0, ltgconf.rpc_timeout);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ANALYSIS_QUEUE(0, IO_WARN, NULL);

        slab_stream_free(buf);

        return 0;
err_ret:
        slab_stream_free(buf);
        return ret;
}
#endif

int net_rpc_init()
{
        __request_set_handler(NET_RPC_HEARTBEAT, __net_srv_heartbeat, "net_srv_heartbeat");
#if 1
        __request_set_handler(NET_RPC_CORES, __net_srv_cores, "net_srv_cores");
        __request_set_handler(NET_RPC_COREADDR, __net_srv_corenetinfo, "net_srv_coreinfo");
#endif

        __rpc_request_register(MSG_HEARTBEAT, __request_handler, NULL);

        return 0;
}
