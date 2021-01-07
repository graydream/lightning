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

typedef struct {
        sockid_t sockid;
        nid_t nid;
        time_t ltime;
} hb_ctx_t;

static int __netable_hb_connected(void *_ctx)
{
        hb_ctx_t *ctx = _ctx;

        return sdevent_connected(&ctx->sockid);
}

static int __netable_hb_send(void *_ctx, uint64_t seq)
{
        int ret;
        hb_ctx_t *ctx = _ctx;

        ret = net_rpc_hello1(&ctx->nid, &ctx->sockid, seq);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

static int __netable_hb_close(void *_ctx)
{
        hb_ctx_t *ctx = _ctx;

        net_handle_t nh;
        sock2nh(&nh, &ctx->sockid);
        sdevent_close(&nh);
        netable_close(&ctx->nid, "timeout at hb", &ctx->ltime);

        rpc_table_reset(__rpc_table__, &ctx->sockid, &ctx->nid);
        
        return 0;
}

static int __netable_hb_free(void *_ctx)
{
        hb_ctx_t *ctx = _ctx;

        ltg_free((void **)&ctx);
        
        return 0;
}

int heartbeat_add(const sockid_t *sockid, const nid_t *nid, suseconds_t tmo, time_t ltime)
{
        int ret;
        char name[MAX_NAME_LEN];
        hb_ctx_t *ctx;

        snprintf(name, MAX_NAME_LEN, "%s", netable_rname(nid));
        
        ret = ltg_malloc((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ctx->nid = *nid;
        ctx->sockid = *sockid;
        ctx->ltime = ltime;

        ret = heartbeat_add1(sockid, name, ctx,
                             __netable_hb_connected,
                             __netable_hb_send,
                             __netable_hb_close,
                             __netable_hb_free,
                             tmo / ltgconf_global.hb_retry,
                             ltgconf_global.hb_retry);
        if (unlikely(ret))
                GOTO(err_free, ret);

        return 0;
err_free:
        ltg_free((void **)&ctx);
err_ret:
        return ret;
}
