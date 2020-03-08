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

typedef ltg_net_conn_t entry_t;

typedef struct {
        nid_t parent;
        sockid_t sockid;
        uint64_t timeout;
        time_t ltime;
        ltg_spinlock_t lock;
        int reference;
        time_t last_update;
} hb_entry_t;

static void __heartbeat(void *_ent);

typedef struct {
        sockid_t sockid;
        uint64_t reply;
} hb_ctx_t;

static void __netable_heartbeat(void *_ctx)
{
        int ret;
        hb_ctx_t *ctx;

        ctx = _ctx;

        ANALYSIS_BEGIN(0);

        ret = net_rpc_heartbeat(&ctx->sockid, ctx->reply);
        if (unlikely(ret)) {
                DWARN("heartbeat %s/%u fail ret:%d\n", _inet_ntoa(ctx->sockid.addr),
                      ctx->sockid.sd, ret);
        } else {
                sdevent_heartbeat_set(&ctx->sockid, NULL, &ctx->reply);
        }

        ANALYSIS_END(0, 1000 * 1000, NULL);

        ltg_free((void **)&ctx);
}

static int __heartbeat__(const sockid_t *sockid, uint64_t reply)
{
        int ret;
        hb_ctx_t *ctx;

        ret = ltg_malloc((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ctx->sockid = *sockid;
        ctx->reply = reply;

        ret = main_loop_request(__netable_heartbeat, ctx, "heartbeat");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static void __heartbeat(void *_ent)
{
        int ret, lost;
        uint64_t sent, reply;
        hb_entry_t *hb_ent;
        time_t ltime;

#if 0
        LTG_ASSERT(core_self() == NULL);
#endif
        
        hb_ent = _ent;

        ltime = netable_conn_time(&hb_ent->parent);
        if (ltime != hb_ent->ltime) {
                ret = EEXIST;
                DINFO("%s maigc %u -> %u, heartbeat exit\n", netable_rname(&hb_ent->parent),
                      hb_ent->ltime, ltime);
                GOTO(err_ret, ret);
        }

        ret = sdevent_heartbeat_get(&hb_ent->sockid, &sent, &reply);
        if (unlikely(ret))
                GOTO(err_ret, ret);

#if 0 //_inet_ntoa maybe timeout
        DBUG("heartbeat %s/%u seq %llu %llu\n",
             _inet_ntoa(hb_ent->sockid.addr), hb_ent->sockid.sd, (LLU)sent, (LLU)reply);
#endif

        lost = sent - reply;
        if (lost && lost <= ltgconf_global.hb_retry) {
                DINFO("heartbeat %s lost ack %u\n", netable_rname(&hb_ent->parent), lost);
        } else if (lost > ltgconf_global.hb_retry) {
                DWARN("heartbeat %s fail, lost ack %u\n", netable_rname(&hb_ent->parent), lost);
                netable_close(&hb_ent->parent, "timeout at hb", &hb_ent->ltime);
                ret = ETIME;
                GOTO(err_ret, ret);
        }

        sent++;

        ret = sdevent_heartbeat_set(&hb_ent->sockid, &sent, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __heartbeat__(&hb_ent->sockid, sent);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        /**
         * @brief 一个心跳周期会发送多条心跳包，如果心跳包丢失大于1，则重置
         *
         * @todo 高负载情况下，为了减少误判的可能性，此方案有待改进
         */
        ret = timer_insert("heartbeat", hb_ent, __heartbeat, hb_ent->timeout / ltgconf_global.hb_retry);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        return;
err_ret:
        ltg_free((void **)&hb_ent);
        return;
}

int heartbeat_add(const sockid_t *sockid, const nid_t *parent, suseconds_t timeout, time_t ltime)
{
        int ret;
        hb_entry_t *hb_ent;

#if 0
        LTG_ASSERT(core_self() == NULL);
#endif
        LTG_ASSERT(sockid->addr);

        ret = ltg_malloc((void **)&hb_ent, sizeof(*hb_ent));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        hb_ent->sockid = *sockid;
        hb_ent->parent = *parent;
        hb_ent->ltime = ltime;
        hb_ent->timeout = timeout;
        hb_ent->reference = 0;
        hb_ent->last_update = gettime();

        ret = ltg_spin_init(&hb_ent->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

#if 0 //_inet_ntoa maybe timeout
        DBUG("add heartbeat %s/%u\n",
             _inet_ntoa(hb_ent->sockid.addr), hb_ent->sockid.sd);
#endif

        ret = timer_insert("heartbeat", hb_ent, __heartbeat, timeout);
        if (unlikely(ret))
                GOTO(err_free, ret);

        return 0;
err_free:
        ltg_free((void **)&hb_ent);
err_ret:
        return ret;
}
