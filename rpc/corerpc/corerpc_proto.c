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

#define IN_IOV_MAX 2
#define CROSS_PTR 1

typedef struct {
        ring_ctx_t ctx;
#if CROSS_PTR
        int in_cnt;
        int out_cnt;
        struct iovec in_iov[IN_IOV_MAX];
        struct iovec out_iov[IN_IOV_MAX];
#endif
        ltgbuf_t out;
        ltgbuf_t in;
        int outlen;
        int replen;
        int retval;
        msgid_t msgid;
        sockid_t sockid;
        request_handler_func handler;
} corerpc_ring_t;

#define LTG_MSG_MAX_KEEP (LTG_MSG_MAX * 2)

static void __request_nosys(void *arg)
{
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL, NULL);

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

        request_trans(arg, NULL, &sockid, &msgid, &buf, NULL, NULL);

        DERROR("got stale msg\n");

        sche_task_setname("stale");
        ltgbuf_free(&buf);
        corerpc_reply_error(&sockid, &msgid, ESTALE);
        return;
}

static rpc_prog_t __corerpc_prog__[LTG_MSG_MAX_KEEP];

static void S_LTG __corerpc_request_task(void *arg)
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
        
        corerpc_reply_buffer(&sockid, &msgid, &out);

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
        corerpc_reply_error(&sockid, &msgid, ret);
#if 0
        DBUG("error op %u from %s, id (%u, %x)\n", req.op,
             _inet_ntoa(sockid.addr), msgid.idx, msgid.figerprint);
#endif
        return;
}

#if CROSS_PTR
inline static void INLINE __corerpc_request_queue_task1(void *_ctx)
{
        corerpc_ring_t *ctx = _ctx;

        DBUG("corerpc request queue task\n");

        ltgbuf_t in, out;
        ltgbuf_initwith2(&in, ctx->in_iov, ctx->in_cnt, NULL, NULL);
        ltgbuf_initwith2(&out, ctx->out_iov, ctx->out_cnt, NULL, NULL);
        
        ctx->retval = ctx->handler(&in, &out, &ctx->outlen);

        ltgbuf_free(&in);
        ltgbuf_free(&out);
}
#endif

static void S_LTG __corerpc_request_queue_task(void *_ctx)
{
        corerpc_ring_t *ctx = _ctx;

        DBUG("corerpc request queue task\n");

        ctx->retval = ctx->handler(&ctx->in, &ctx->out, &ctx->outlen);
}

static void S_LTG __corerpc_request_queue_reply(void *_ctx)
{
        int ret;
        corerpc_ring_t *ctx = _ctx;

        DBUG("corerpc request queue task, retval %d\n", ctx->retval);

        ret = ctx->retval;
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        LTG_ASSERT(ctx->outlen <= ctx->replen);
        if (unlikely(ctx->outlen < (int)ctx->out.len)) {
                ltgbuf_droptail(&ctx->out, ctx->replen - ctx->outlen);
        }

        corerpc_reply_buffer(&ctx->sockid, &ctx->msgid, &ctx->out);

        ltgbuf_free(&ctx->in);
        ltgbuf_free(&ctx->out);

        slab_stream_free(ctx);
        
        return ;
err_ret:
        ltgbuf_free(&ctx->out);
        ltgbuf_free(&ctx->in);
        corerpc_reply_error(&ctx->sockid, &ctx->msgid, ret);
        slab_stream_free(ctx);
        return;
}

static void S_LTG __corerpc_request_queue(rpc_request_t *rpc_request)
{
        int ret;
        const char *name;
        coreid_t coreid;
        corerpc_ring_t *ctx = slab_stream_alloc(sizeof(*ctx));

        request_trans(rpc_request, &coreid, &ctx->sockid, &ctx->msgid, &ctx->in,
                      &ctx->replen, NULL);

        ctx->handler = NULL;
        rpc_request->handler(&ctx->in, &ctx->handler, &name);
        if (unlikely(ctx->handler == NULL)) {
                ret = ENOSYS;
                GOTO(err_ret, ret);
        }

        DBUG("name %s\n", name);
        SOCKID_DUMP(&ctx->sockid);
        MSGID_DUMP(&ctx->msgid);

        ltgbuf_init(&ctx->out, ctx->replen);

#if CROSS_PTR
        if (likely(ltgbuf_segcount(&ctx->in) <= IN_IOV_MAX)
            && likely(ltgbuf_segcount(&ctx->out) <= IN_IOV_MAX)) {
                //LTG_ASSERT(ltgbuf_segcount(&ctx->out) == 1);

                ctx->in_cnt = IN_IOV_MAX;
                ctx->out_cnt = IN_IOV_MAX;
                ltgbuf_trans(ctx->in_iov, &ctx->in_cnt, &ctx->in);
                ltgbuf_trans(ctx->out_iov, &ctx->out_cnt, &ctx->out);

                LTG_ASSERT(ctx->in_cnt);
                
                core_ring_queue(coreid.idx, RING_TASK, &ctx->ctx,
                                __corerpc_request_queue_task1, ctx,
                                __corerpc_request_queue_reply, ctx);
        } else {
                DWARN("iov count %d %d %d %d\n",
                      ltgbuf_segcount(&ctx->in),
                      ltgbuf_segcount(&ctx->out),
                      ctx->in.len, ctx->out.len);
                
                core_ring_queue(coreid.idx, RING_TASK, &ctx->ctx,
                                __corerpc_request_queue_task, ctx,
                                __corerpc_request_queue_reply, ctx);
        }
#else
        core_ring_queue(coreid.idx, RING_TASK, &ctx->ctx,
                        __corerpc_request_queue_task, ctx,
                        __corerpc_request_queue_reply, ctx);
#endif        

        return;
err_ret:
        ltgbuf_free(&ctx->in);
        corerpc_reply_error(&ctx->sockid, &ctx->msgid, ret);
        slab_stream_free(ctx);
        return;
}

static int S_LTG __corerpc_request_handler(corerpc_ctx_t *ctx,
                                             const ltg_net_head_t *head,
                                             ltgbuf_t *buf)
{
        int ret;
        rpc_request_t *rpc_request;
        const msgid_t *msgid;
        rpc_prog_t *prog;
        sockid_t *sockid;

        sockid = &ctx->sockid;
        LTG_ASSERT(sockid->addr);
        DBUG("new msg from %s/%u, id (%u, %x)\n",
              _inet_ntoa(sockid->addr), sockid->sd, head->msgid.idx,
              head->msgid.figerprint);

        msgid = &head->msgid;
        LTG_ASSERT(head->prog < LTG_MSG_MAX_KEEP);
        prog = &__corerpc_prog__[head->prog];

        rpc_request = slab_stream_alloc(sizeof(*rpc_request));
        if (!rpc_request) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(sockid->addr);
        rpc_request->sockid = *sockid;
        rpc_request->msgid = *msgid;
        rpc_request->dist.idx = head->coreid;
        rpc_request->dist.nid = *net_getnid();
        LTG_ASSERT(sockid->reply);
        rpc_request->replen = head->replen;
        rpc_request->ctx = ctx;
        ltgbuf_init(&rpc_request->buf, 0);
        ltgbuf_merge(&rpc_request->buf, buf);

        rpc_request->handler = prog->handler;

        if (unlikely(head->master_magic != ltg_global.master_magic)) {
                DWARN("got stale msg, master_magic %x:%x\n",
                       head->master_magic, ltg_global.master_magic);

                sche_task_new("corenet", __request_stale, rpc_request, 0);
        } else if (unlikely(prog->handler == NULL)) {
                DWARN("no func\n");

                sche_task_new("corenet", __request_nosys, rpc_request, 0);
        } else {
                if (likely(netctl())) {
                        __corerpc_request_queue(rpc_request);
                } else {
                        sche_task_new("corenet", __corerpc_request_task, rpc_request, 0);
                }
        }

        return 0;
err_ret:
        return ret;
}

extern rpc_table_t *corerpc_self();

static void S_LTG __corerpc_reply_handler(const ltg_net_head_t *head, ltgbuf_t *buf)
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
        ltg_net_head_t *head;

        LTG_ASSERT(len >= sizeof(ltg_net_head_t));
        head = buf;

        LTG_ASSERT(head->magic == LTG_MSG_MAGIC);

        DBUG("len %u %u\n", head->len, head->blocks);

        return head->len + head->blocks;
}

#if ENABLE_RDMA

static int S_LTG __corerpc_rdma_handler(corerpc_ctx_t *ctx, ltgbuf_t *msg_buf)
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

int S_LTG corerpc_rdma_recv_data(void *_ctx, void *_msg_buf)
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

int S_LTG corerpc_rdma_recv_msg(void *_ctx, void *iov, int *_count)
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

                corenet_rdma_send(sockid, &_buf, addr, rkey, net_head->blocks,
                                  build_rdma_read_req);
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
int corerpc_tcp_recv(void *_ctx, void *buf, int *_count)
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

void corerpc_register(int type, request_get_handler handler, void *context)
{
        rpc_prog_t *prog;

        LTG_ASSERT(type < LTG_MSG_MAX_KEEP);
        prog = &__corerpc_prog__[type];

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
