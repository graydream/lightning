#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_utils.h"
#include "ltg_core.h"

#define LTG_MSG_MAX_KEEP (LTG_MSG_MAX * 2)

STATIC void __request_nosys(void *arg)
{
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL, NULL);

        DBUG("nosys\n");
        sche_task_setname("nosys");
        ltgbuf_free(&buf);
        stdrpc_reply_error(&sockid, &msgid, ENOSYS);
        return;
}

STATIC void __request_stale(void *arg)
{
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL, NULL);

        sche_task_setname("stale");
        ltgbuf_free(&buf);
        stdrpc_reply_error(&sockid, &msgid, ESTALE);
        return;
}

int rpc_pack_len(void *buf, uint32_t len, int *msg_len, int *io_len)
{
        int ret;
        ltg_net_head_t *head;

        if (len > sizeof(uint32_t)) {
                head = buf;
                LTG_ASSERT(head->magic == LTG_MSG_MAGIC);
        }

        if (len < sizeof(ltg_net_head_t)) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        head = buf;

        *msg_len = head->len;
        *io_len = head->blocks;

        DBUG("magic %x, msg_len %u io_len %u\n", head->magic, *msg_len, *io_len);
        
        LTG_ASSERT(*msg_len > 0);

        return 0;
err_ret:
        return ret;
}

static rpc_prog_t  __stdrpc_prog__[LTG_MSG_MAX_KEEP];

static void S_LTG __rpc_request_task(void *arg)
{
        int ret, replen;
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;
        request_handler_func handler;
        const char *name;
        coreid_t coreid;
        rpc_request_t *rpc_request = arg;

        request_trans(arg, &coreid, &sockid, &msgid, &buf, &replen, NULL);

#if 0
        DBUG("new op %u from %s, id (%u, %x)\n", req.op,
             _inet_ntoa(sockid.addr), msgid.idx, msgid.figerprint);
#endif
 
        rpc_request->handler(&buf, &handler, &name);
        if (unlikely(handler == NULL)) {
                ret = ENOSYS;
                GOTO(err_ret, ret);
        }

        sche_task_setname(name);

        DBUG("name %s\n", name);
        SOCKID_DUMP(&sockid);
        MSGID_DUMP(&msgid);

        ltgbuf_t out;
        int outlen;

        ltgbuf_init(&out, replen);

        ret = handler(&buf, &out, &outlen);
        if (unlikely(ret))
                GOTO(err_free, ret);

        LTG_ASSERT(outlen <= replen);
        if (unlikely(outlen < (int)out.len)) {
                ltgbuf_droptail(&out, replen - outlen);
        }
        
        stdrpc_reply1(&sockid, &msgid, &out);

        ltgbuf_free(&buf);
        ltgbuf_free(&out);

#if 0
        DBUG("reply op %u from %s, id (%u, %x)\n", req.op,
             _inet_ntoa(sockid.addr), msgid.idx, msgid.figerprint);
#endif

        return ;
err_free:
        ltgbuf_free(&out);
err_ret:
        ltgbuf_free(&buf);
        stdrpc_reply_error(&sockid, &msgid, ret);
#if 0
        DBUG("error op %u from %s, id (%u, %x)\n", req.op,
             _inet_ntoa(sockid.addr), msgid.idx, msgid.figerprint);
#endif
        return;
}

STATIC int __core_request(va_list ap)
{
        rpc_request_t *rpc_request = va_arg(ap, rpc_request_t *);

        va_end(ap);
        
        sche_task_new("rpc", __rpc_request_task, rpc_request, 0);

        return 0;
}

STATIC int __rpc_request_handler(const nid_t *nid, const sockid_t *sockid,
                                 const ltg_net_head_t *head, ltgbuf_t *buf)
{
        int ret;
        rpc_request_t *rpc_request;
        const msgid_t *msgid;
        rpc_prog_t *prog;

        (void) nid;
        
        LTG_ASSERT(sockid->addr);

        DBUG("new msg from %s/%u, id (%u, %x)\n",
              _inet_ntoa(sockid->addr), sockid->sd, head->msgid.idx,
              head->msgid.figerprint);

        msgid = &head->msgid;
        LTG_ASSERT(head->prog < LTG_MSG_MAX_KEEP);
        prog = &__stdrpc_prog__[head->prog];

#ifdef HAVE_STATIC_ASSERT
        static_assert(sizeof(*rpc_request)  < sizeof(mem_cache128_t),
                      "rpc_request_t");
#endif
        rpc_request = slab_stream_alloc(sizeof(rpc_request_t));
        if (!rpc_request) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        rpc_request->sockid = *sockid;
        rpc_request->msgid = *msgid;
        rpc_request->dist.nid = *net_getnid();
        rpc_request->dist.idx = head->coreid;
        rpc_request->replen = head->replen;
        rpc_request->sockid.reply = stdrpc_reply_tcp;
        ltgbuf_init(&rpc_request->buf, 0);
        ltgbuf_merge(&rpc_request->buf, buf);
 
        rpc_request->handler = prog->handler;

        if (unlikely(prog->handler == NULL)) {
                DWARN("no func\n");
                sche_task_new("rpc", __request_nosys, rpc_request, 0);
        } else if (head->coreid == (uint32_t)-1) {
                sche_task_new("rpc", __rpc_request_task, rpc_request, 0);
        } else {
                ret = core_request(head->coreid, -1, "rpc_request",
                                   __core_request, rpc_request);
                if (ret)
                        GOTO(err_ret, ret);
                
        }

        return 0;
err_ret:
        return ret;
}

STATIC void __rpc_reply_handler(const ltg_net_head_t *head, ltgbuf_t *buf)
{
        int ret, retval;

        retval = ltg_pack_err(buf);
        if (retval)
                ltgbuf_free(buf);

        DBUG("reply msg id (%u, %x), len %u\n", head->msgid.idx,
              head->msgid.figerprint, head->len);

        ret = rpc_table_post(__rpc_table__, &head->msgid, retval, buf, head->status);
        if (unlikely(ret)) {
                ltgbuf_free(buf);
        }
}

int rpc_pack_handler(const nid_t *nid, const sockid_t *sockid, ltgbuf_t *buf)
{
        int ret;
        ltg_net_head_t head;

        ANALYSIS_BEGIN(0);

        ret = ltgnet_pack_crcverify(buf);
        if (unlikely(ret)) {
                ltgbuf_free(buf);
                LTG_ASSERT(0);
        }

        DBUG("new msg %u\n", buf->len);

        ret = ltgbuf_popmsg(buf, &head, sizeof(ltg_net_head_t));
        if (unlikely(ret))
                LTG_ASSERT(0);

        //LTG_ASSERT(head.len == buf->len + sizeof(ltg_net_head_t));

        switch (head.type) {
        case LTG_MSG_REQ:
                __rpc_request_handler(nid, sockid, &head, buf);
                break;
        case LTG_MSG_REP:
                __rpc_reply_handler(&head, buf);
                break;
        default:
                DERROR("bad msgtype\n");
        }

        ANALYSIS_END(0, 1000 * 100, NULL);

        return 0;
}

void rpc_request_register(int type, request_get_handler handler, void *context)
{
        rpc_prog_t *prog;

        DINFO("set %d %p\n", type, handler);
        
        LTG_ASSERT(type < LTG_MSG_MAX_KEEP);
        prog = &__stdrpc_prog__[type];

        LTG_ASSERT(prog->handler == NULL);
        LTG_ASSERT(prog->context == NULL);

        prog->handler = handler;
        prog->context = context;
}
