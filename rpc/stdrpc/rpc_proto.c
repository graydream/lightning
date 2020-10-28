#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_utils.h"
#include "ltg_core.h"

#define LTG_MSG_MAX_KEEP (LTG_MSG_MAX * 2)

static net_prog_t  __stdnet_prog__[LTG_MSG_MAX_KEEP];

STATIC void __request_nosys(void *arg)
{
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL);

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

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL);

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

        if (head->blocks) {
                *msg_len =  head->len - head->blocks;
                *io_len = head->blocks;
                LTG_ASSERT(*io_len > 0);
        } else {
                *msg_len =  head->len;
                *io_len = 0;
        }

        DBUG("magic %x, msg_len %u io_len %u\n", head->magic, *msg_len, *io_len);
        
        LTG_ASSERT(*msg_len > 0);

        return 0;
err_ret:
        return ret;
}

STATIC int __core_request(va_list ap)
{
        net_request_handler handler = va_arg(ap, net_request_handler);
        rpc_request_t *rpc_request = va_arg(ap, rpc_request_t *);
        int priority = va_arg(ap, int);

        va_end(ap);
        
        sche_task_new("rpc", handler, rpc_request, priority);

        return 0;
}

STATIC int __rpc_request_handler(const nid_t *nid, const sockid_t *sockid,
                                 const ltg_net_head_t *head, ltgbuf_t *buf)
{
        int ret;
        rpc_request_t *rpc_request;
        const msgid_t *msgid;
        net_prog_t *prog;
        net_request_handler handler;

        (void) nid;
        
        LTG_ASSERT(sockid->addr);

        DBUG("new msg from %s/%u, id (%u, %x)\n",
              _inet_ntoa(sockid->addr), sockid->sd, head->msgid.idx,
              head->msgid.figerprint);

        msgid = &head->msgid;
        LTG_ASSERT(head->prog < LTG_MSG_MAX_KEEP);
        prog = &__stdnet_prog__[head->prog];

#ifdef HAVE_STATIC_ASSERT
        static_assert(sizeof(*rpc_request)  < sizeof(mem_cache128_t), "rpc_request_t");
#endif
        rpc_request = slab_stream_alloc(sizeof(rpc_request_t));
        if (!rpc_request) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        rpc_request->sockid = *sockid;
        rpc_request->msgid = *msgid;
        rpc_request->dist.nid.id = 0;
        rpc_request->dist.idx = 0;
        rpc_request->sockid.reply = stdrpc_reply_tcp;
        ltgbuf_init(&rpc_request->buf, 0);
        ltgbuf_merge(&rpc_request->buf, buf);
        
        UNIMPLEMENTED(__NULL__);
        handler = prog->handler ? prog->handler : __request_nosys;

        if (head->coreid == (uint32_t)-1) {
                sche_task_new("rpc", handler, rpc_request, 0);
        } else {
                ret = core_request(head->coreid, -1, "rpc_request",
                                   __core_request, handler, rpc_request, head->group);
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

        ret = rpc_table_post(__rpc_table__, &head->msgid, retval, buf, head->latency);
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

void rpc_request_register(int type, net_request_handler handler, void *context)
{
        net_prog_t *prog;

        DINFO("set %d %p\n", type, handler);
        
        LTG_ASSERT(type < LTG_MSG_MAX_KEEP);
        prog = &__stdnet_prog__[type];

        LTG_ASSERT(prog->handler == NULL);
        LTG_ASSERT(prog->context == NULL);

        prog->handler = handler;
        prog->context = context;
}
