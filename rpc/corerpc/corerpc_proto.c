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
        uint64_t latency;
} rpc_ctx_t;

#define LTG_MSG_MAX_KEEP (LTG_MSG_MAX * 2)

static net_prog_t __corenet_prog__[LTG_MSG_MAX_KEEP];

static void __request_nosys(void *arg)
{
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;
        nid_t nid;

        request_trans(arg, &nid, &sockid, &msgid, &buf, NULL);

        DBUG("nosys\n");
        sche_task_setname("nosys");
        ltgbuf_free(&buf);
        corerpc_reply_error(&sockid, &msgid, ENOSYS);
        return;
}

static void __request_stale(void *arg)
{
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;
        nid_t nid;

        request_trans(arg, &nid, &sockid, &msgid, &buf, NULL);

        DERROR("got stale msg\n");

        sche_task_setname("stale");
        ltgbuf_free(&buf);
        corerpc_reply_error(&sockid, &msgid, ESTALE);
        return;
}

static int IO_FUNC __corerpc_request_handler(corerpc_ctx_t *ctx, const ltg_net_head_t *head,
                                             ltgbuf_t *buf)
{
        int ret;
        rpc_request_t *rpc_request;
        const msgid_t *msgid;
        net_prog_t *prog;
        sockid_t *sockid;
        net_request_handler handler;

        sockid = &ctx->sockid;
        LTG_ASSERT(sockid->addr);
        DBUG("new msg from %s/%u, id (%u, %x)\n",
              _inet_ntoa(sockid->addr), sockid->sd, head->msgid.idx,
              head->msgid.figerprint);

        msgid = &head->msgid;
        LTG_ASSERT(head->prog < LTG_MSG_MAX_KEEP);
        prog = &__corenet_prog__[head->prog];

        rpc_request = slab_stream_alloc(sizeof(*rpc_request));
        if (!rpc_request) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(sockid->addr);
        rpc_request->sockid = *sockid;
        rpc_request->msgid = *msgid;
        LTG_ASSERT(sockid->reply);
        rpc_request->ctx = ctx;
        ltgbuf_init(&rpc_request->buf, 0);
        ltgbuf_merge(&rpc_request->buf, buf);

        if (unlikely(head->master_magic != ltg_global.master_magic)) {
                DERROR("got stale msg, master_magic %x:%x\n",
                       head->master_magic, ltg_global.master_magic);
                handler = __request_stale;
        } else {
                handler = prog->handler ? prog->handler : __request_nosys;
        }

        sche_task_new("corenet", handler, rpc_request, 0);

        return 0;
err_ret:
        return ret;
}

extern rpc_table_t *corerpc_self();

static void IO_FUNC __corerpc_reply_handler(const ltg_net_head_t *head, ltgbuf_t *buf)
{
        int ret, retval;
        rpc_table_t *__rpc_table_private__ = corerpc_self();

        NET_HEAD_DUMP(head);

        retval = ltg_pack_err(buf);
        if (unlikely(retval))
                ltgbuf_free(buf);

        ret = rpc_table_post(__rpc_table_private__, &head->msgid, retval, buf, head->latency);
        if (unlikely(ret)) {
                ltgbuf_free(buf);
        }

#if RPC_TABLE_POST_FREE
        rpc_table_free(__rpc_table_private__, &head->msgid);
#endif
}

static int __corerpc_handler(corerpc_ctx_t *ctx, ltgbuf_t *buf)
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

        switch (head.type) {
        case LTG_MSG_REQ:
                __corerpc_request_handler(ctx, &head, buf);
                break;
        case LTG_MSG_REP:
                __corerpc_reply_handler(&head, buf);
                break;
        default:
                DERROR("bad msgtype\n");
        }

        ANALYSIS_END(0, 1000 * 100, NULL);

        return 0;
}

static int __corerpc_len(void *buf, uint32_t len)
{
        int msg_len, io_len;
        ltg_net_head_t *head;

        LTG_ASSERT(len >= sizeof(ltg_net_head_t));
        head = buf;

        LTG_ASSERT(head->magic == LTG_MSG_MAGIC);

        DBUG("len %u %u\n", head->len, head->blocks);

        if (head->blocks) {
                msg_len =  head->len - head->blocks;
                io_len = head->blocks;
                LTG_ASSERT(io_len > 0);
        } else {
                msg_len =  head->len;
                io_len = 0;
        }

        return msg_len + io_len;
}

#if ENABLE_RDMA

static int IO_FUNC __corerpc_rdma_handler(corerpc_ctx_t *ctx, ltgbuf_t *msg_buf)
{
        ltg_net_head_t head;
        ANALYSIS_BEGIN(0);

        ltgbuf_rdma_popmsg(msg_buf, (void *)&head, sizeof(ltg_net_head_t));
        LTG_ASSERT(head.magic == LTG_MSG_MAGIC);
        switch (head.type) {
                case LTG_MSG_REQ:
                        __corerpc_request_handler(ctx, &head, msg_buf);
                        break;
                case LTG_MSG_REP:
                        __corerpc_reply_handler(&head, msg_buf);
                        ltgbuf_free(msg_buf);
                        break;
                default:
                        DERROR("bad msgtype:%d\n", head.type);
                        LTG_ASSERT(0);
        }

        ANALYSIS_END(0, 1000 * 100, NULL);

        return 0;
}

int IO_FUNC corerpc_rdma_recv_data(void *_ctx, void *_msg_buf)
{
        corerpc_ctx_t *ctx = _ctx;
        ltgbuf_t *msg_buf  = _msg_buf;
        int ret;

        ret = __corerpc_rdma_handler(ctx, msg_buf);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int IO_FUNC corerpc_rdma_recv_msg(void *_ctx, void *iov, int *_count)
{
        int len = 0, left = 0;
        ltgbuf_t _buf;
        void *msg = iov - RDMA_MESSAGE_SIZE;
        corerpc_ctx_t *ctx = _ctx;
        ltg_net_head_t *net_head = NULL;
        sockid_t *sockid;

        left = *_count;
        sockid = &ctx->sockid;
        net_head = msg;

        LTG_ASSERT(left == (int)net_head->len);

        NET_HEAD_DUMP(net_head);

        ltgbuf_initwith(&_buf, msg, net_head->len, iov, corenet_rdma_post_recv);

        if (net_head->blocks) {
                void **addr = (void **)net_head->msgid.data_prop.remote_addr;
                uint32_t rkey = net_head->msgid.data_prop.rkey;

                corenet_rdma_send(sockid, &_buf, addr, rkey, net_head->blocks, build_rdma_read_req);
        } else {
                __corerpc_rdma_handler(ctx,  &_buf);
        }

        LTG_ASSERT(len <= RDMA_MESSAGE_SIZE);

        return 0;
}
#endif

/**
 * for TCP
 */
int corerpc_recv(void *_ctx, void *buf, int *_count)
{
        int len, count = 0;
        char tmp[MAX_BUF_LEN];
        ltgbuf_t _buf, *mbuf = buf;
        corerpc_ctx_t *ctx = _ctx;

        DBUG("recv %u\n", mbuf->len);

        if (mbuf->len < sizeof(ltg_net_head_t)) {
                DERROR("buflen %u, need %u\n", mbuf->len, sizeof(ltg_net_head_t));
                return 0;
        }

        while (mbuf->len >= sizeof(ltg_net_head_t)) {
                ltgbuf_get(mbuf, tmp, sizeof(ltg_net_head_t));
                len = __corerpc_len(tmp, sizeof(ltg_net_head_t));

                DBUG("msg len %u\n", len);

                if (len > (int)mbuf->len) {
                        DBUG("wait %u %u\n", len, mbuf->len);
                        break;
                }

                ltgbuf_init(&_buf, 0);
                ltgbuf_pop1(buf, &_buf, len, 1);

                __corerpc_handler(ctx, &_buf);
                count++;
        }

        *_count = count;

        return 0;
}

void corerpc_register(int type, net_request_handler handler, void *context)
{
        net_prog_t *prog;

        LTG_ASSERT(type < LTG_MSG_MAX_KEEP);
        prog = &__corenet_prog__[type];

        LTG_ASSERT(prog->handler == NULL);
        LTG_ASSERT(prog->context == NULL);
        
        prog->handler = handler;
        prog->context = context;
}


void corerpc_close(void *_ctx)
{
        corerpc_ctx_t *ctx = _ctx;

        if (ctx->running) {
                DWARN("running %u\n", ctx->running);
                EXIT(EAGAIN);
        }

        slab_static_free((void *)ctx);
}
