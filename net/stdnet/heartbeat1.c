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

extern int ltg_nofile_max;

typedef struct {
        uint64_t timeout;
        uint64_t reply;
        sockid_t sockid;
        int retry;
        void *ctx;
        int (*connected)(void *);
        int (*send)(void *, uint64_t);
        int (*close)(void *);
        char name[MAX_NAME_LEN];
} entry_t;

static uint64_t *__send__;
static uint64_t *__reply__;

static void __heartbeat_set(const sockid_t *id, const uint64_t *send, const uint64_t *reply)
{
        if (send) {
                __send__[id->sd] = *send;
        }

        if (reply) {
                __reply__[id->sd] = *reply;
        }
}

static void __heartbeat_get(const sockid_t *id, uint64_t *send, uint64_t *reply)
{
        if (send) {
                *send = __send__[id->sd];
        }

        if (reply) {
                *reply = __reply__[id->sd];
        }
}

#if 1
static void __heartbeat_send(void *_ctx)
{
        int ret;
        entry_t *ent;

        ent = _ctx;

        ANALYSIS_BEGIN(0);

        ret = ent->send(ent->ctx, ent->reply);
        if (unlikely(ret)) {
                DWARN("heartbeat %s/%s fail ret:%d, seq %ju\n", ent->name,
                      _inet_ntoa(ent->sockid.addr), ret, ent->reply);
        } else {
                __heartbeat_set(&ent->sockid, NULL, &ent->reply);
        }

        ANALYSIS_END(0, 1000 * 1000, NULL);

        ltg_free((void **)&ent);
}

#else
static void *__heartbeat_send(void *_ctx)
{
        int ret;
        entry_t *ent;

        ent = _ctx;

        ANALYSIS_BEGIN(0);

        ret = ent->send(ent->ctx, ent->reply);
        if (unlikely(ret)) {
                DWARN("heartbeat %s/%s fail ret:%d, seq %ju\n", ent->name,
                      _inet_ntoa(ent->sockid.addr), ret, ent->reply);
        } else {
                __heartbeat_set(&ent->sockid, NULL, &ent->reply);
        }

        ANALYSIS_END(0, 1000 * 1000, NULL);

        ltg_free((void **)&ent);

        pthread_exit(NULL);
}
#endif

static int __heartbeat__(const entry_t *_ent, uint64_t reply)
{
        int ret;
        entry_t *ent;

        ret = ltg_malloc((void **)&ent, sizeof(*ent));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memcpy(ent, _ent, sizeof(*_ent));
        ent->reply = reply;

#if 1
        ret = main_loop_request(__heartbeat_send, ent, "heartbeat1");
        if (unlikely(ret))
                GOTO(err_ret, ret);
#else
        pthread_t th;
        pthread_attr_t ta;

        (void) pthread_attr_init(&ta);
        (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
        
        ret = pthread_create(&th, &ta, __heartbeat_send, ent);
        if (unlikely(ret))
                GOTO(err_ret, ret);
#endif

        return 0;
err_ret:
        return ret;
}

int heartbeat_init()
{
        int ret, size;

        size = sizeof(*__send__) * ltg_nofile_max;
        ret = ltg_malloc((void **)&__send__, size);
        if (ret)
                GOTO(err_ret, ret);

        ret = ltg_malloc((void **)&__reply__, size);
        if (ret)
                GOTO(err_ret, ret);

        memset(__send__, 0x0, size);
        memset(__reply__, 0x0, size);

        return 0;
err_ret:
        return ret;
}

static void __heartbeat(void *_ent)
{
        int ret, lost;
        uint64_t sent, reply;
        entry_t *ent;
        
        ent = _ent;

        if (!ent->connected(ent->ctx)) {
                DWARN("%s already closed\n", ent->name);
                ret = ENONET;
                GOTO(err_ret, ret);
        }

        __heartbeat_get(&ent->sockid, &sent, &reply);

        DINFO("heartbeat to %s/%s seq %llu %llu\n", ent->name,
              _inet_ntoa(ent->sockid.addr), (LLU)sent, (LLU)reply);

        lost = sent - reply;
        if (lost && lost <= ent->retry) {
                DINFO("heartbeat %s lost ack %u\n", ent->name, lost);
        } else if (lost > ent->retry) {
                DWARN("heartbeat %s fail, lost ack %u\n", ent->name, lost);
                ret = ETIME;
                GOTO(err_ret, ret);
        }

        sent++;

        __heartbeat_set(&ent->sockid, &sent, NULL);

        ret = __heartbeat__(ent, sent);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        /**
         * @brief 一个心跳周期会发送多条心跳包，如果心跳包丢失大于1，则重置
         *
         * @todo 高负载情况下，为了减少误判的可能性，此方案有待改进
         */
        ret = timer_insert("heartbeat", ent, __heartbeat, ent->timeout);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        return;
err_ret:
        ent->close(ent->ctx);
        ltg_free((void **)&ent);
        return;
}

int heartbeat_add1(const sockid_t *sockid, const char *name, void *ctx,
                   int (*connected)(void *), int (*send)(void *, uint64_t), int (*close)(void *),
                   suseconds_t timeout, int retry)
{
        int ret;
        entry_t *ent;

        LTG_ASSERT(sockid->addr);

        ret = ltg_malloc((void **)&ent, sizeof(*ent));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ent->ctx = ctx;
        ent->send = send;
        ent->connected = connected;
        ent->close = close;
        ent->sockid = *sockid;
        ent->timeout = timeout;
        ent->retry = retry;
        strcpy(ent->name, name);

        DINFO("add heartbeat %s/%s\n", ent->name, _inet_ntoa(ent->sockid.addr));

        ret = timer_insert("heartbeat", ent, __heartbeat, timeout);
        if (unlikely(ret))
                GOTO(err_free, ret);

        return 0;
err_free:
        ltg_free((void **)&ent);
err_ret:
        return ret;
}
