#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

typedef struct {
        coreid_t coreid;
        sockid_t sockid;
        coreid_t localid;
        uint64_t seq;
} hb_ctx_t;

static int __corenet_hb_connected(void *_ctx)
{
        hb_ctx_t *ctx = _ctx;

        LTG_ASSERT(ctx->sockid.sd != -1);

#if 0
        long int r = _random();
        if (r % 3 == 1) {
                DWARN("for rdma: sockid %d seq %ju coreid %s/%d.%d\n",
                      ctx->sockid.sd,
                      ctx->seq,
                      netable_rname(&ctx->coreid.nid),
                      ctx->coreid.nid.id,
                      ctx->coreid.idx);
                return 0;
        }
#endif

        return corenet_maping_connected(&ctx->coreid.nid, &ctx->sockid);
}

static int __corenet_hb_send_va(va_list ap)
{
        int ret;
        const hb_ctx_t *ctx = va_arg(ap, hb_ctx_t *);

        va_end(ap);

        ret = net_rpc_hello2(&ctx->coreid, &ctx->sockid, ctx->seq);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __corenet_hb_send(void *_ctx, uint64_t seq)
{
        int ret;
        hb_ctx_t *ctx = _ctx;
        ctx->seq = seq;
        ret = core_request(ctx->localid.idx, -1, "hb send", __corenet_hb_send_va, ctx);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

static int __corenet_hb_close_va(va_list ap)
{
        const hb_ctx_t *ctx = va_arg(ap, hb_ctx_t *);

        va_end(ap);

        DINFO("close %s\n", netable_rname(&ctx->coreid.nid));
        
        corenet_maping_closeall(&ctx->coreid.nid, &ctx->sockid);

        return 0;
}
static int __corenet_hb_close(void *_ctx)
{
        hb_ctx_t *ctx = _ctx;

        core_request(ctx->localid.idx, -1, "hb close", __corenet_hb_close_va, ctx);

        return 0;
}

static int __corenet_hb_free(void *_ctx)
{
        hb_ctx_t *ctx = _ctx;

        ltg_free((void **)&ctx);
        
        return 0;
}

int corenet_hb_add(const coreid_t *coreid, const sockid_t *sockid)
{
        int ret, tmo;
        char name[MAX_NAME_LEN];
        hb_ctx_t *ctx;

#if 0
        DERROR("hb disabled\n");
        return 0;
#endif
        
        LTG_ASSERT(sockid->sd != -1);
        
        tmo = (1000 * 1000 * ltgconf_global.hb_timeout) / ltgconf_global.hb_retry;
        snprintf(name, MAX_NAME_LEN, "%s/%d", netable_rname(&coreid->nid),
                 coreid->idx);
        
        ret = ltg_malloc((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ctx->coreid = *coreid;
        ctx->sockid = *sockid;
        ret = core_getid(&ctx->localid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = heartbeat_add1(sockid, name, ctx,
                             __corenet_hb_connected,
                             __corenet_hb_send,
                             __corenet_hb_close,
                             __corenet_hb_free,
                             tmo, ltgconf_global.hb_retry);
        if (unlikely(ret))
                GOTO(err_free, ret);

        return 0;
err_free:
        ltg_free((void **)&ctx);
err_ret:
        return ret;
}
